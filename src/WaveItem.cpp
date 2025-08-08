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
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QDebug>

// =====================================================================================
// Uniforms & vertices (must match shaders)
// =====================================================================================
struct UniformBlock {
  float    time;
  float    speed;
  float    amplitude;
  float    frequency;
  QVector4D baseColor;
  QVector4D waveColor;
  float    threshold;
  float    dustIntensity;
  float    minDist;
  float    maxDist;
  int      maxDraws;
  QVector2D resolution; // pixels
  float    brightness;
  float    pad;
};

struct QuadVertex {
  QVector2D pos;
  QVector2D uv;
};

struct RibbonVertex {
  QVector3D pos; // x,z in [-1..1], y=0 initially
  QVector2D uv;  // [0..1]
};

// =====================================================================================
// QRC helpers + QSB loader
// =====================================================================================
static QStringList shaderSearchPaths(const QString &nameWithExt) {
  QStringList paths;
  // 1) Embedded resources
  paths << ("qrc:/shaders/" + nameWithExt);
  paths << (":/shaders/" + nameWithExt);
  // 2) Dev-time filesystem fallbacks
  const QString exeDir = QCoreApplication::applicationDirPath();
  paths << QDir(exeDir).filePath("../.qsb/shaders/" + nameWithExt); // build tree
  paths << QDir(exeDir).filePath("shaders/" + nameWithExt);         // side-by-side
  paths << QDir(exeDir).filePath(".qsb/shaders/" + nameWithExt);    // local .qsb
  return paths;
}

static QShader loadQsb(const QString &qsbFileName) {
  const QStringList candidates = shaderSearchPaths(qsbFileName);
  for (const QString &p : candidates) {
    QFile f(p);
    if (f.open(QIODevice::ReadOnly)) {
      const QByteArray data = f.readAll();
      QShader s = QShader::fromSerialized(data);
      if (!s.isValid()) {
        qWarning() << "Invalid QSB:" << p;
        continue;
      }
      qInfo() << "Loaded shader:" << p;
      return s;
    }
  }
  qWarning() << "Failed to open shader at any of:" << candidates;
  return QShader();
}

// =====================================================================================
// Ribbon grid builder (triangle strips with degenerate stitching) — 32-bit indices
// =====================================================================================
static void buildRibbonGrid(int cols, int rows,
                            QVector<RibbonVertex> &verts,
                            QVector<quint32> &idx)
{
  cols = qMax(1, cols);
  rows = qMax(1, rows);

  const int vtxCount = (cols + 1) * (rows + 1);
  verts.clear();
  verts.reserve(vtxCount);

  for (int r = 0; r <= rows; ++r) {
    const float v = float(r) / float(rows);      // 0..1 (vertical)
    const float z = v * 2.0f - 1.0f;             // -1..1 (depth)
    for (int c = 0; c <= cols; ++c) {
      const float u = float(c) / float(cols);    // 0..1 (horizontal)
      const float x = u * 2.0f - 1.0f;           // -1..1
      RibbonVertex rv;
      rv.pos = QVector3D(x, 0.0f, z);
      rv.uv  = QVector2D(u, v);
      verts.push_back(rv);
    }
  }

  // Indices for triangle strips per row
  const int stripForRow = (cols + 1) * 2 + 2; // +2 for degenerates between rows
  const int totalIdx = rows * stripForRow - 2; // last strip doesn't need trailing degenerates
  idx.clear();
  idx.reserve(totalIdx);

  auto vIndex = [cols](int r, int c) -> quint32 {
    return quint32(r * (cols + 1) + c);
  };

  for (int r = 0; r < rows; ++r) {
    if (r > 0) {
      idx.push_back(vIndex(r, 0)); // degenerate (bridge from previous row)
    }
    for (int c = 0; c <= cols; ++c) {
      idx.push_back(vIndex(r, c));
      idx.push_back(vIndex(r + 1, c));
    }
    if (r < rows - 1) {
      idx.push_back(vIndex(r + 1, cols)); // degenerate (bridge to next row)
    }
  }
}

// =====================================================================================
// WaveNode: QRhi on QSGRenderNode
// =====================================================================================
class WaveNode final : public QSGRenderNode {
public:
  explicit WaveNode(QQuickWindow *w) : m_window(w) {
    m_schemeTimer = new QTimer();
    m_schemeTimer->setInterval(30000);
    QObject::connect(m_schemeTimer, &QTimer::timeout, [this]{ m_schemeDirty = true; });
    m_schemeTimer->start();
  }
  ~WaveNode() override { release(); delete m_schemeTimer; }

  void setProperties(const UniformBlock &ub) { m_ub = ub; m_propsDirty = true; }
  void setItemSize(const QSizeF &s) { if (m_itemSize == s) return; m_itemSize = s; }
  void setAutoScheme(bool on) { if (m_autoScheme == on) return; m_autoScheme = on; m_schemeDirty = true; }
  void setUseRibbon(bool on) { m_useRibbon = on; }

  // QSGRenderNode API
  void render(const RenderState *) override {
    QRhi *rhi = m_window ? m_window->rhi() : nullptr;
    QRhiCommandBuffer *cb = commandBuffer();
    QRhiRenderTarget *rt = renderTarget();
    if (!rhi || !cb || !rt) return;

    if (!m_inited) init(rhi);

    // Recreate pipelines if renderpass or sample count changed
    if (m_rpDesc != rt->renderPassDescriptor() || m_rtSampleCount != rt->sampleCount()) {
      m_rpDesc = rt->renderPassDescriptor();
      m_rtSampleCount = rt->sampleCount();
      destroyPipelines();
      createPipelines();
    }

    const QSize ps = rt->pixelSize();
    cb->setViewport(QRhiViewport(0, 0, float(ps.width()), float(ps.height())));
    cb->setScissor(QRhiScissor(0, 0, ps.width(), ps.height()));

    // Keep resolution in pixels
    m_ub.resolution = QVector2D(float(ps.width()), float(ps.height()));

    if (m_schemeDirty && m_autoScheme) {
      const auto s = XmbColorScheme::current(QDateTime::currentDateTime());
      m_ub.baseColor = s.base;
      m_ub.waveColor = s.wave;
      m_ub.brightness = s.brightness;
      m_propsDirty = true;
      m_schemeDirty = false;
    }

    // Upload resources
    QRhiResourceUpdateBatch *rub = rhi->nextResourceUpdateBatch();

    if (!m_quadUploaded) {
      const QuadVertex quad[4] = {
        {{-1.0f, -1.0f}, {0.0f, 0.0f}},
        {{-1.0f,  1.0f}, {0.0f, 1.0f}},
        {{ 1.0f, -1.0f}, {1.0f, 0.0f}},
        {{ 1.0f,  1.0f}, {1.0f, 1.0f}},
      };
      rub->updateDynamicBuffer(m_vbufQuad, 0, sizeof(quad), quad);
      m_quadUploaded = true;
    }

    if (m_useRibbon && !m_ribbonUploaded) {
      // Build & upload grid once
      QVector<RibbonVertex> verts;
      QVector<quint32> idx;
      buildRibbonGrid(192, 64, verts, idx); // wide & smooth

      const quint32 vsize = verts.size() * quint32(sizeof(RibbonVertex));
      const quint32 isize = idx.size()   * quint32(sizeof(quint32));

      if (!m_vbufRibbon) {
        m_vbufRibbon = m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, vsize);
        if (!m_vbufRibbon->create()) qFatal("Ribbon vertex buffer create failed");
      }
      if (!m_ibufRibbon) {
        m_ibufRibbon = m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, isize);
        if (!m_ibufRibbon->create()) qFatal("Ribbon index buffer create failed");
      }

      rub->uploadStaticBuffer(m_vbufRibbon, 0, vsize, verts.constData());
      rub->uploadStaticBuffer(m_ibufRibbon, 0, isize, idx.constData());

      m_ribbonIndexCount = idx.size();
      m_ribbonUploaded = true;
    }

    if (m_propsDirty) {
      rub->updateDynamicBuffer(m_ubuf, 0, sizeof(UniformBlock), &m_ub);
      m_propsDirty = false;
    }

    cb->resourceUpdate(rub);

    // Pick pipeline
    bool drew = false;
    if (m_useRibbon && m_gpRibbon && m_gpRibbonReady &&
        m_vbufRibbon && m_ibufRibbon && m_ribbonIndexCount >= 3)
    {
      cb->setGraphicsPipeline(m_gpRibbon);
      // Use SRB that matches ribbon shader layout
      cb->setShaderResources(m_srbRibbon ? m_srbRibbon : m_srbWave);
      const QRhiCommandBuffer::VertexInput v(m_vbufRibbon, 0);
      cb->setVertexInput(0, 1, &v, m_ibufRibbon, 0, QRhiCommandBuffer::IndexUInt32);
      cb->drawIndexed(m_ribbonIndexCount, 1);
      drew = true;
    }

    if (!drew && m_gpQuad && m_gpQuadReady) {
      cb->setGraphicsPipeline(m_gpQuad);
      cb->setShaderResources(m_srbWave);
      const QRhiCommandBuffer::VertexInput v(m_vbufQuad, 0);
      cb->setVertexInput(0, 1, &v, nullptr);
      cb->draw(4);
      drew = true;
    }

    if (!drew) {
      qWarning() << "WaveNode: nothing drawn (pipelines or buffers missing).";
    }
  }

  StateFlags changedStates() const override { return StateFlags(0); }
  RenderingFlags flags() const override { return BoundedRectRendering; }
  QRectF rect() const override { return QRectF(QPointF(0, 0), m_itemSize); }

private:
  // inside WaveNode
  QRhiShaderResourceBindings *makeSrb(bool includeSampler)
  {
      auto *srb = m_rhi->newShaderResourceBindings();

      if (includeSampler && m_noiseTex && m_noiseSamp) {
          srb->setBindings({
              QRhiShaderResourceBinding::uniformBuffer(
                  0,
                  QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                  m_ubuf),
              QRhiShaderResourceBinding::sampledTexture(
                  1,
                  QRhiShaderResourceBinding::FragmentStage,
                  m_noiseTex, m_noiseSamp)
          });
      } else {
          srb->setBindings({
              QRhiShaderResourceBinding::uniformBuffer(
                  0,
                  QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                  m_ubuf)
          });
      }

      if (!srb->create())
          qFatal("SRB create failed");
      return srb;
  }


  void init(QRhi *rhi) {
    m_rhi = rhi;

    // UB
    m_ubuf = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(UniformBlock));
    if (!m_ubuf->create()) qFatal("Uniform buffer create failed");

    // Fullscreen quad VB
    m_vbufQuad = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, sizeof(QuadVertex) * 4);
    if (!m_vbufQuad->create()) qFatal("Quad vertex buffer create failed");

    // Load shaders (quad is mandatory; ribbon optional)
    if (!m_vsQuad.isValid()) m_vsQuad = loadQsb("WaveRhi.vert.qsb");
    if (!m_fsQuad.isValid()) m_fsQuad = loadQsb("WaveRhi.frag.qsb");

    // Optional ribbon pipeline shaders (only if present)
    m_vsRibbon = loadQsb("RibbonRhi.vert.qsb");
    m_fsRibbon = loadQsb("RibbonRhi.frag.qsb");

    // --- Noise sampler + texture at binding=1 (for WaveRhi.frag) ---
    if (!m_noiseSamp) {
      m_noiseSamp = m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest,
                                      QRhiSampler::None, QRhiSampler::Repeat, QRhiSampler::Repeat);
      if (!m_noiseSamp->create()) qFatal("noise sampler create failed");
    }
    if (!m_noiseTex) {
      // No special flags needed for uploads on Qt 6.9
      m_noiseTex = m_rhi->newTexture(QRhiTexture::RGBA8, QSize(256, 256), 1);
      if (!m_noiseTex->create()) qFatal("noise texture create failed");

      // Load dissolve.png from qrc, fallback to procedural if missing
      QImage img(QStringLiteral("qrc:/interfaceFX/GraphicsServer/dissolve.png"));
      if (img.isNull()) {
        // quick procedural fallback
        img = QImage(256, 256, QImage::Format_RGBA8888);
        srand(1);
        for (int y = 0; y < 256; ++y) {
          uchar *line = img.scanLine(y);
          for (int x = 0; x < 256; ++x) {
            uchar v = uchar(rand() & 0xff);
            line[x * 4 + 0] = v;
            line[x * 4 + 1] = v;
            line[x * 4 + 2] = v;
            line[x * 4 + 3] = 255;
          }
        }
      } else if (img.format() != QImage::Format_RGBA8888) {
        img = img.convertToFormat(QImage::Format_RGBA8888);
      }

      // Convenience upload: copies the QImage into the texture
      QRhiResourceUpdateBatch *rub = m_rhi->nextResourceUpdateBatch();
      rub->uploadTexture(m_noiseTex, img); // QRhi copies the data
      commandBuffer()->resourceUpdate(rub);
      m_noiseReady = true;
    }

    // SRBs
    m_srbWave   = makeSrb(true);    // UBO + sampler (wave)
    m_srbRibbon = makeSrb(false);   // UBO only (ribbon)
    if (!m_srbRibbon) {
      // In case ribbon shaders actually have the sampler too, fall back
      m_srbRibbon = makeSrb(true);
    }
    if (!m_srbWave) qFatal("Failed to create wave SRB");
    // m_srbRibbon can be null if ribbon shaders are absent; we check at draw time.

    m_rpDesc = renderTarget() ? renderTarget()->renderPassDescriptor() : nullptr;
    m_rtSampleCount = renderTarget() ? renderTarget()->sampleCount() : 1;

    createPipelines();
    m_inited = true;
  }

  void createPipelines() {
    if (!m_rpDesc) return; // wait until we have a valid renderpass

    // Quad pipeline (fullscreen)
    if (!m_gpQuad && m_vsQuad.isValid() && m_fsQuad.isValid()) {
      m_gpQuad = m_rhi->newGraphicsPipeline();

      QRhiVertexInputLayout il;
      il.setBindings({ QRhiVertexInputBinding(sizeof(QuadVertex)) });
      il.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, offsetof(QuadVertex, uv)),
      });

      m_gpQuad->setVertexInputLayout(il);
      m_gpQuad->setSampleCount(m_rtSampleCount);
      m_gpQuad->setTopology(QRhiGraphicsPipeline::TriangleStrip);
      m_gpQuad->setCullMode(QRhiGraphicsPipeline::None);

      QRhiGraphicsPipeline::TargetBlend tb; tb.enable = false; // opaque base
      m_gpQuad->setTargetBlends({ tb });

      m_gpQuad->setShaderStages({
        QRhiShaderStage(QRhiShaderStage::Vertex,   m_vsQuad),
        QRhiShaderStage(QRhiShaderStage::Fragment, m_fsQuad),
      });

      m_gpQuad->setShaderResourceBindings(m_srbWave);
      m_gpQuad->setRenderPassDescriptor(m_rpDesc);

      if (!m_gpQuad->create()) qFatal("Quad pipeline create failed");
      m_gpQuadReady = true;
    }

    // Ribbon pipeline (if shaders exist)
    if (!m_gpRibbon && m_vsRibbon.isValid() && m_fsRibbon.isValid()) {
      m_gpRibbon = m_rhi->newGraphicsPipeline();

      QRhiVertexInputLayout il2;
      il2.setBindings({ QRhiVertexInputBinding(sizeof(RibbonVertex)) });
      il2.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float3, 0),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, offsetof(RibbonVertex, uv)),
      });

      m_gpRibbon->setVertexInputLayout(il2);
      m_gpRibbon->setSampleCount(m_rtSampleCount);
      m_gpRibbon->setTopology(QRhiGraphicsPipeline::TriangleStrip);
      m_gpRibbon->setCullMode(QRhiGraphicsPipeline::None);

      // Enable alpha blending only for ribbon (so it can glow over base)
      QRhiGraphicsPipeline::TargetBlend tb2;
      tb2.enable = true;
      tb2.srcColor = QRhiGraphicsPipeline::SrcAlpha;
      tb2.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
      tb2.opColor  = QRhiGraphicsPipeline::Add;
      tb2.srcAlpha = QRhiGraphicsPipeline::One;
      tb2.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
      tb2.opAlpha  = QRhiGraphicsPipeline::Add;
      m_gpRibbon->setTargetBlends({ tb2 });

      m_gpRibbon->setShaderStages({
        QRhiShaderStage(QRhiShaderStage::Vertex,   m_vsRibbon),
        QRhiShaderStage(QRhiShaderStage::Fragment, m_fsRibbon),
      });

      // Bind SRB that matches ribbon shaders
      m_gpRibbon->setShaderResourceBindings(m_srbRibbon ? m_srbRibbon : m_srbWave);
      m_gpRibbon->setRenderPassDescriptor(m_rpDesc);

      if (!m_gpRibbon->create()) {
        qWarning() << "Ribbon pipeline create failed; falling back to quad.";
        delete m_gpRibbon; m_gpRibbon = nullptr; m_gpRibbonReady = false;
      } else {
        m_gpRibbonReady = true;
      }
    }
  }

  void destroyPipelines() {
    if (m_gpQuad)   { delete m_gpQuad;   m_gpQuad = nullptr;   m_gpQuadReady = false; }
    if (m_gpRibbon) { delete m_gpRibbon; m_gpRibbon = nullptr; m_gpRibbonReady = false; }
  }

  void release() {
    destroyPipelines();

    if (m_srbWave)   { delete m_srbWave;   m_srbWave = nullptr; }
    if (m_srbRibbon) { delete m_srbRibbon; m_srbRibbon = nullptr; }
    if (m_ubuf)       { delete m_ubuf;       m_ubuf = nullptr; }
    if (m_vbufQuad)   { delete m_vbufQuad;   m_vbufQuad = nullptr; }
    if (m_vbufRibbon) { delete m_vbufRibbon; m_vbufRibbon = nullptr; }
    if (m_ibufRibbon) { delete m_ibufRibbon; m_ibufRibbon = nullptr; }
    if (m_noiseTex)   { delete m_noiseTex;   m_noiseTex = nullptr; }
    if (m_noiseSamp)  { delete m_noiseSamp;  m_noiseSamp = nullptr; }

    m_quadUploaded = false;
    m_ribbonUploaded = false;

    m_inited = false;
  }

private:
  QQuickWindow *m_window = nullptr;
  QSizeF m_itemSize;

  // RHI
  QRhi *m_rhi = nullptr;
  QRhiRenderPassDescriptor *m_rpDesc = nullptr; // not owned
  int m_rtSampleCount = 1;

  // Pipelines
  QRhiGraphicsPipeline *m_gpQuad = nullptr;
  QRhiGraphicsPipeline *m_gpRibbon = nullptr;
  bool m_gpQuadReady = false;
  bool m_gpRibbonReady = false;

  // SRBs & buffers
  QRhiShaderResourceBindings *m_srbWave = nullptr;   // UBO + sampler
  QRhiShaderResourceBindings *m_srbRibbon = nullptr; // UBO only (or fallback with sampler)

  QRhiBuffer *m_ubuf = nullptr;

  QRhiBuffer *m_vbufQuad = nullptr;   // fullscreen quad (dynamic)
  bool m_quadUploaded = false;

  QRhiBuffer *m_vbufRibbon = nullptr; // grid (immutable)
  QRhiBuffer *m_ibufRibbon = nullptr; // indices (immutable)
  bool m_ribbonUploaded = false;
  int  m_ribbonIndexCount = 0;

  // Shaders
  QShader m_vsQuad, m_fsQuad;
  QShader m_vsRibbon, m_fsRibbon; // optional

  // Noise (for WaveRhi)
  QRhiTexture *m_noiseTex = nullptr;
  QRhiSampler *m_noiseSamp = nullptr;
  bool m_noiseReady = false;

  // Uniforms
  UniformBlock m_ub{};
  bool m_propsDirty = true;

  // Colour scheme auto-refresh
  bool m_autoScheme = true;
  bool m_schemeDirty = true;
  QTimer *m_schemeTimer = nullptr;

  // Mode
  bool m_useRibbon = false;

  bool m_inited = false;
};

// =====================================================================================
// WaveItem (QQuickItem)
// =====================================================================================
WaveItem::WaveItem(QQuickItem *parent) : QQuickItem(parent) {
  setFlag(ItemHasContents, true);
  connect(this, &QQuickItem::windowChanged, this, [this]{ if (window()) window()->setColor(Qt::black); });
}

WaveItem::~WaveItem() = default;

void WaveItem::scheduleUpdate() { update(); }

QSGNode *WaveItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
  auto *node = static_cast<WaveNode *>(old);
  if (!node) node = new WaveNode(window());

  node->setAutoScheme(m_useXmbScheme);
  node->setUseRibbon(m_useRibbon);

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
  ub.baseColor = QVector4D(m_baseColor.redF(), m_baseColor.greenF(), m_baseColor.blueF(), 1.0f);
  ub.waveColor = QVector4D(m_waveColor.redF(), m_waveColor.greenF(), m_waveColor.blueF(), 1.0f);
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
void WaveItem::setTime(qreal v)         { if (m_time == v) return; m_time = v; Q_EMIT timeChanged();         scheduleUpdate(); }
void WaveItem::setSpeed(qreal v)        { if (m_speed == v) return; m_speed = v; Q_EMIT speedChanged();        scheduleUpdate(); }
void WaveItem::setAmplitude(qreal v)    { if (m_amplitude == v) return; m_amplitude = v; Q_EMIT amplitudeChanged(); scheduleUpdate(); }
void WaveItem::setFrequency(qreal v)    { if (m_frequency == v) return; m_frequency = v; Q_EMIT frequencyChanged(); scheduleUpdate(); }
void WaveItem::setBaseColor(const QColor &c) { if (m_baseColor == c) return; m_baseColor = c; Q_EMIT baseColorChanged(); scheduleUpdate(); }
void WaveItem::setWaveColor(const QColor &c) { if (m_waveColor == c) return; m_waveColor = c; Q_EMIT waveColorChanged(); scheduleUpdate(); }
void WaveItem::setThreshold(qreal v)    { if (m_threshold == v) return; m_threshold = v; Q_EMIT thresholdChanged();    scheduleUpdate(); }
void WaveItem::setDustIntensity(qreal v){ if (m_dustIntensity == v) return; m_dustIntensity = v; Q_EMIT dustIntensityChanged(); scheduleUpdate(); }
void WaveItem::setMinDist(qreal v)      { if (m_minDist == v) return; m_minDist = v; Q_EMIT minDistChanged();      scheduleUpdate(); }
void WaveItem::setMaxDist(qreal v)      { if (m_maxDist == v) return; m_maxDist = v; Q_EMIT maxDistChanged();      scheduleUpdate(); }
void WaveItem::setMaxDraws(int v)       { if (m_maxDraws == v) return; m_maxDraws = v; Q_EMIT maxDrawsChanged();       scheduleUpdate(); }
void WaveItem::setBrightness(qreal v)   { if (m_brightness == v) return; m_brightness = v; Q_EMIT brightnessChanged();   scheduleUpdate(); }
void WaveItem::setUseXmbScheme(bool v)  { if (m_useXmbScheme == v) return; m_useXmbScheme = v; Q_EMIT useXmbSchemeChanged(); if (m_useXmbScheme) updateXmbScheme(); scheduleUpdate(); }
// Note: setUseRibbon(bool) is inline in WaveItem.h – do not duplicate here.
