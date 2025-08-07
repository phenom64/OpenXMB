// src/WaveItem.cpp
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
// GPLv3.

#include "WaveItem.h"
#include "XmbColorScheme.h"

#include <QFile>
#include <QQuickWindow>
#include <QSGRenderNode>
#include <QSGRendererInterface>
#include <QtGui>
#include <rhi/qrhi.h>
#include <QTimer>

// Must match shaders/WaveRhi.frag (std140).
struct UniformBlock {
  float time;
  float speed;
  float amplitude;
  float frequency;
  QVector4D baseColor;
  QVector4D waveColor;
  float threshold;
  float dustIntensity;
  float minDist;
  float maxDist;
  int maxDraws;
  QVector2D resolution;
  float brightness;
  float pad;
};

struct Vertex {
  QVector2D pos;
  QVector2D uv;
};

static QShader loadQsb(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    qFatal("Cannot open shader: %s", qPrintable(path));
  }
  const QByteArray data = f.readAll();
  QShader s = QShader::fromSerialized(data);
  if (!s.isValid()) qFatal("Invalid QSB: %s", qPrintable(path));
  return s;
}

class WaveNode final : public QSGRenderNode {
public:
  explicit WaveNode(QQuickWindow *w) : m_window(w) {
    m_schemeTimer = new QTimer();
    m_schemeTimer->setInterval(30000);
    QObject::connect(m_schemeTimer, &QTimer::timeout, [this] {
      m_schemeDirty = true;
    });
    m_schemeTimer->start();
  }

  ~WaveNode() override {
    release();
    delete m_schemeTimer;
  }

  void setProperties(const UniformBlock &ub) {
    m_ub = ub;
    m_propsDirty = true;
  }

  void setItemSize(const QSizeF &s) {
    if (m_itemSize == s) return;
    m_itemSize = s;
    m_sizeDirty = true;
  }

  void setAutoScheme(bool on) {
    if (m_autoScheme == on) return;
    m_autoScheme = on;
    m_schemeDirty = true;
  }

  void render(const RenderState *state) override {
    Q_UNUSED(state);

    // Since Qt 6.6+, prefer the direct helpers:
    // - window()->rhi()
    // - this->commandBuffer()
    // - this->renderTarget()
    QRhi *rhi = m_window->rhi();
    QRhiCommandBuffer *cb = commandBuffer();
    QRhiRenderTarget *rt = renderTarget();
    if (!rhi || !cb || !rt) return;

    QRhiRenderPassDescriptor *rpDesc = rt->renderPassDescriptor();
    if (!m_inited) init(rhi, rpDesc);
    if (m_rpDesc != rpDesc) {
      m_rpDesc = rpDesc;
      m_pipeline->setRenderPassDescriptor(m_rpDesc);
      if (!m_pipeline->create()) qFatal("Pipeline recreate failed");
    }

    if (m_sizeDirty) {
      m_ub.resolution =
          QVector2D(float(m_itemSize.width()), float(m_itemSize.height()));
      m_sizeDirty = false;
      m_propsDirty = true;
    }

    if (m_schemeDirty && m_autoScheme) {
      const auto s = XmbColorScheme::current(QDateTime::currentDateTime());
      m_ub.baseColor = s.base;
      m_ub.waveColor = s.wave;
      m_ub.brightness = s.brightness;
      m_propsDirty = true;
      m_schemeDirty = false;
    }

    // Resource updates
    QRhiResourceUpdateBatch *rub = rhi->nextResourceUpdateBatch();
    if (!m_vbufUploaded) {
      const Vertex quad[4] = {
          {{-1.0f, -1.0f}, {0.0f, 0.0f}},
          {{-1.0f,  1.0f}, {0.0f, 1.0f}},
          {{ 1.0f, -1.0f}, {1.0f, 0.0f}},
          {{ 1.0f,  1.0f}, {1.0f, 1.0f}},
      };
      rub->updateDynamicBuffer(m_vbuf, 0, sizeof(quad), quad);
      m_vbufUploaded = true;
    }
    if (m_propsDirty) {
      rub->updateDynamicBuffer(m_ubuf, 0, sizeof(UniformBlock), &m_ub);
      m_propsDirty = false;
    }
    cb->resourceUpdate(rub);

    cb->setGraphicsPipeline(m_pipeline);
    cb->setShaderResources(m_srb);
    const QRhiCommandBuffer::VertexInput v(m_vbuf, 0);
    cb->setVertexInput(0, 1, &v, nullptr);
    cb->draw(4);
  }

  // Qt 6.9: State flags are DepthState, StencilState, ScissorState, ColorState, BlendState, CullState, ViewportState
  StateFlags changedStates() const override {
    return BlendState | CullState | DepthState | StencilState | ScissorState |
           ViewportState | ColorState;
  }

  RenderingFlags flags() const override { return BoundedRectRendering; }
  QRectF rect() const override { return QRectF(QPointF(0, 0), m_itemSize); }

private:
  void init(QRhi *rhi, QRhiRenderPassDescriptor *rp) {
    m_rhi = rhi;
    m_rpDesc = rp;

    if (!m_vbuf) {
      // Dynamic so we can write via updateDynamicBuffer
      m_vbuf = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                                sizeof(Vertex) * 4);
      if (!m_vbuf->create()) qFatal("Vertex buffer create failed");
    }
    if (!m_ubuf) {
      m_ubuf = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                sizeof(UniformBlock));
      if (!m_ubuf->create()) qFatal("Uniform buffer create failed");
    }

    if (!m_vs.isValid()) m_vs = loadQsb(":/shaders/WaveRhi.vert.qsb");
    if (!m_fs.isValid()) m_fs = loadQsb(":/shaders/WaveRhi.frag.qsb");

    if (!m_pipeline) {
      m_pipeline = m_rhi->newGraphicsPipeline();

      QRhiVertexInputLayout il;
      il.setBindings({QRhiVertexInputBinding(sizeof(Vertex))});
      il.setAttributes({
          QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
          QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2,
                                   offsetof(Vertex, uv)),
      });

      m_pipeline->setVertexInputLayout(il);
      m_pipeline->setSampleCount(1);
      m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
      m_pipeline->setShaderStages({
          QRhiShaderStage(QRhiShaderStage::Vertex, m_vs),
          QRhiShaderStage(QRhiShaderStage::Fragment, m_fs),
      });

      QRhiGraphicsPipeline::TargetBlend tb;
      tb.enable = false;
      m_pipeline->setTargetBlends({tb});

      m_pipeline->setRenderPassDescriptor(m_rpDesc);
      if (!m_pipeline->create()) qFatal("Pipeline create failed");
    }

    if (!m_srb) {
      m_srb = m_rhi->newShaderResourceBindings();
      m_srb->setBindings({
          QRhiShaderResourceBinding::uniformBuffer(
              0,
              QRhiShaderResourceBinding::VertexStage |
                  QRhiShaderResourceBinding::FragmentStage,
              m_ubuf),
      });
      if (!m_srb->create()) qFatal("SRB create failed");
    }

    m_inited = true;
  }

  void release() {
    if (!m_rhi) return;
    delete m_pipeline;
    delete m_srb;
    delete m_ubuf;
    delete m_vbuf;
    m_pipeline = nullptr;
    m_srb = nullptr;
    m_ubuf = nullptr;
    m_vbuf = nullptr;
    m_inited = false;
    m_vbufUploaded = false;
  }

private:
  QQuickWindow *m_window = nullptr;
  QSizeF m_itemSize;
  QRhi *m_rhi = nullptr;
  QRhiRenderPassDescriptor *m_rpDesc = nullptr;

  QRhiGraphicsPipeline *m_pipeline = nullptr;
  QRhiShaderResourceBindings *m_srb = nullptr;
  QRhiBuffer *m_vbuf = nullptr;
  QRhiBuffer *m_ubuf = nullptr;

  QShader m_vs;
  QShader m_fs;

  UniformBlock m_ub{};
  bool m_inited = false;
  bool m_propsDirty = true;
  bool m_sizeDirty = true;

  bool m_vbufUploaded = false;

  bool m_autoScheme = true;
  bool m_schemeDirty = true;
  QTimer *m_schemeTimer = nullptr;
};

WaveItem::WaveItem(QQuickItem *parent) : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  connect(this, &QQuickItem::windowChanged, this, [this] {
    if (window()) window()->setColor(Qt::black);
  });
}

WaveItem::~WaveItem() {}

void WaveItem::scheduleUpdate() { update(); }

QSGNode *WaveItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
  auto *node = static_cast<WaveNode *>(old);
  if (!node) node = new WaveNode(window());

  node->setAutoScheme(m_useXmbScheme);

  if (m_useXmbScheme) {
    const auto s = XmbColorScheme::current(QDateTime::currentDateTime());
    m_baseColor = QColor::fromRgbF(s.base.x(), s.base.y(), s.base.z(), 1.0);
    m_waveColor = QColor::fromRgbF(s.wave.x(), s.wave.y(), s.wave.z(), 1.0);
    m_brightness = s.brightness;
  }

  UniformBlock ub{};
  ub.time = float(m_time);
  ub.speed = float(m_speed);
  ub.amplitude = float(m_amplitude);
  ub.frequency = float(m_frequency);
  ub.baseColor = QVector4D(m_baseColor.redF(), m_baseColor.greenF(),
                           m_baseColor.blueF(), 1.0f);
  ub.waveColor = QVector4D(m_waveColor.redF(), m_waveColor.greenF(),
                           m_waveColor.blueF(), 1.0f);
  ub.threshold = float(m_threshold);
  ub.dustIntensity = float(m_dustIntensity);
  ub.minDist = float(m_minDist);
  ub.maxDist = float(m_maxDist);
  ub.maxDraws = m_maxDraws;
  ub.resolution = QVector2D(float(width()), float(height()));
  ub.brightness = float(m_brightness);
  ub.pad = 0.0f;

  node->setItemSize(QSizeF(width(), height()));
  node->setProperties(ub);
  return node;
}

void WaveItem::geometryChange(const QRectF &n, const QRectF &o) {
  QQuickItem::geometryChange(n, o);
  scheduleUpdate();
}

void WaveItem::updateXmbScheme() {
  const auto s = XmbColorScheme::current(QDateTime::currentDateTime());
  m_baseColor = QColor::fromRgbF(s.base.x(), s.base.y(), s.base.z(), 1.0);
  m_waveColor = QColor::fromRgbF(s.wave.x(), s.wave.y(), s.wave.z(), 1.0);
  m_brightness = s.brightness;
  Q_EMIT baseColorChanged();
  Q_EMIT waveColorChanged();
  Q_EMIT brightnessChanged();
  scheduleUpdate();
}

// setters (Q_EMIT because QT_NO_KEYWORDS)
void WaveItem::setTime(qreal v) { if (m_time == v) return; m_time = v; Q_EMIT timeChanged(); scheduleUpdate(); }
void WaveItem::setSpeed(qreal v) { if (m_speed == v) return; m_speed = v; Q_EMIT speedChanged(); scheduleUpdate(); }
void WaveItem::setAmplitude(qreal v) { if (m_amplitude == v) return; m_amplitude = v; Q_EMIT amplitudeChanged(); scheduleUpdate(); }
void WaveItem::setFrequency(qreal v) { if (m_frequency == v) return; m_frequency = v; Q_EMIT frequencyChanged(); scheduleUpdate(); }
void WaveItem::setBaseColor(const QColor &c) { if (m_baseColor == c) return; m_baseColor = c; Q_EMIT baseColorChanged(); scheduleUpdate(); }
void WaveItem::setWaveColor(const QColor &c) { if (m_waveColor == c) return; m_waveColor = c; Q_EMIT waveColorChanged(); scheduleUpdate(); }
void WaveItem::setThreshold(qreal v) { if (m_threshold == v) return; m_threshold = v; Q_EMIT thresholdChanged(); scheduleUpdate(); }
void WaveItem::setDustIntensity(qreal v) { if (m_dustIntensity == v) return; m_dustIntensity = v; Q_EMIT dustIntensityChanged(); scheduleUpdate(); }
void WaveItem::setMinDist(qreal v) { if (m_minDist == v) return; m_minDist = v; Q_EMIT minDistChanged(); scheduleUpdate(); }
void WaveItem::setMaxDist(qreal v) { if (m_maxDist == v) return; m_maxDist = v; Q_EMIT maxDistChanged(); scheduleUpdate(); }
void WaveItem::setMaxDraws(int v) { if (m_maxDraws == v) return; m_maxDraws = v; Q_EMIT maxDrawsChanged(); scheduleUpdate(); }
void WaveItem::setBrightness(qreal v) { if (m_brightness == v) return; m_brightness = v; Q_EMIT brightnessChanged(); scheduleUpdate(); }
void WaveItem::setUseXmbScheme(bool v) { if (m_useXmbScheme == v) return; m_useXmbScheme = v; Q_EMIT useXmbSchemeChanged(); if (m_useXmbScheme) updateXmbScheme(); scheduleUpdate(); }