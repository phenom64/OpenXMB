// src/WaveItem.cpp
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.

#include "WaveItem.h"
#include <QSGRendererInterface>
#include <QQuickWindow>
#include <QRhiVulkanInitParams>
#include <QRhiShaderStage>
#include <QRhiVertexInputLayout>
#include <QRhiShaderResourceBinding>
#include <QRhiCommandBuffer>
#include <QRhiViewport>
#include <QRhiColorAttachment>
#include <QRhiRenderBuffer>
#include <QRhiRenderTarget>
#include <QRhiRenderPassDescriptor>
#include <QRhiGraphicsPipeline>
#include <QRhiBuffer>
#include <QDebug>

// Structure for uniform buffer (matches shader uniforms).
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
    float padding[2];  // Padding to align to 16-byte boundaries for Vulkan.
};

WaveItem::WaveItem(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_initialized = false;
}

WaveItem::~WaveItem()
{
    delete m_pipeline;
    delete m_bindings;
    delete m_uniformBuffer;
    delete m_vertexBuffer;
    delete m_rpDesc;
    delete m_rt;
    delete m_rhi;
}

QSGNode *WaveItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    Q_UNUSED(oldNode);

    if (!m_initialized) {
        initializeRhi();
    }

    if (!m_rhi) {
        qDebug() << "RHI not initialized, skipping render";
        return nullptr;
    }

    QSGRendererInterface *rif = window()->rendererInterface();
    m_cb = static_cast<QRhiCommandBuffer *>(rif->getResource(window(), QSGRendererInterface::CommandBufferResource));

    if (!m_cb) {
        qDebug() << "Command buffer not available";
        return nullptr;
    }

    // Begin recording commands for this frame.
    m_cb->beginPass(m_rt);

    // Bind the graphics pipeline for drawing.
    m_cb->setGraphicsPipeline(m_pipeline);

    // Bind the shader resources (uniforms).
    m_cb->setShaderResources(m_bindings);

    // Set the viewport to match the item's dimensions.
    m_cb->setViewport(QRhiViewport(0, 0, width(), height()));

    // Bind vertex input and draw the full-screen quad (triangle strip with 4 vertices).
    QRhiCommandBuffer::VertexInput vinput(m_vertexBuffer, 0);
    m_cb->setVertexInput(0, 1, &vinput);
    m_cb->draw(4);

    // End the pass and submit.
    m_cb->endPass();

    return nullptr;  // RHI handles rendering directly; no need for a traditional scene graph node.
}

void WaveItem::initializeRhi()
{
    QSGRendererInterface *rif = window()->rendererInterface();
    m_rhi = static_cast<QRhi *>(rif->getResource(window(), QSGRendererInterface::RhiResource));
    if (!m_rhi) {
        qFatal("Failed to get QRhi from window");
    }

    qDebug() << "RHI backend:" << m_rhi->backendName();

    // Create render target description for the swapchain color buffer.
    QRhiColorAttachment color0;
    QRhiTextureRenderTargetDescription rtDesc;
    rtDesc.setColorAttachments({color0});
    m_rt = m_rhi->newTextureRenderTarget(rtDesc);
    m_rpDesc = m_rt->newCompatibleRenderPassDescriptor();
    m_rt->setRenderPassDescriptor(m_rpDesc);
    if (!m_rt->create()) {
        qFatal("Failed to create render target");
    }

    // Create uniform buffer with dynamic updates for all properties.
    m_uniformBuffer = m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(UniformBlock));
    if (!m_uniformBuffer->create()) {
        qFatal("Failed to create uniform buffer");
    }

    // Create vertex buffer for a full-screen quad (triangle strip).
    m_vertexBuffer = m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, 16 * sizeof(float));  // 4 vertices, 4 floats each (pos + texcoord)
    if (!m_vertexBuffer->create()) {
        qFatal("Failed to create vertex buffer");
    }

    // Vertex data for full-screen quad.
    float vdata[] = {
        0.0f, 0.0f, 0.0f, 0.0f,  // Bottom-left
        0.0f, 1.0f, 0.0f, 1.0f,  // Top-left
        1.0f, 0.0f, 1.0f, 0.0f,  // Bottom-right
        1.0f, 1.0f, 1.0f, 1.0f   // Top-right
    };
    m_vertexBuffer->uploadStaticData((const char*)vdata, sizeof(vdata));

    // Load shader sources (GLSL; RHI compiles to SPIR-V for Vulkan).
    QRhiShaderStage vs(QRhiShaderStage::Vertex, QRhiShaderStage::GlslShader, QByteArray(vertexShader()));
    QRhiShaderStage fs(QRhiShaderStage::Fragment, QRhiShaderStage::GlslShader, QByteArray(fragmentShader()));

    // Create graphics pipeline with detailed configuration.
    m_pipeline = m_rhi->newGraphicsPipeline();
    m_pipeline->setShaderStages({vs, fs});
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({QRhiVertexInputBinding(4 * sizeof(float))});
    inputLayout.setAttributes({
        QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
        QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float))
    });
    m_pipeline->setVertexInputLayout(inputLayout);
    m_pipeline->setSampleCount(1);  // No multisampling for simplicity.
    m_pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    m_pipeline->setRenderPassDescriptor(m_rpDesc);
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.opColor = QRhiGraphicsPipeline::Add;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.opAlpha = QRhiGraphicsPipeline::Add;
    m_pipeline->setTargetBlends({blend});
    if (!m_pipeline->create()) {
        qFatal("Failed to create graphics pipeline");
    }

    // Create shader resource bindings for the uniform buffer.
    m_bindings = m_rhi->newShaderResourceBindings();
    m_bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_uniformBuffer)
    });
    if (!m_bindings->create()) {
        qFatal("Failed to create shader resource bindings");
    }

    updateUniformBuffer();  // Initial update of uniform data.
    m_initialized = true;
    qDebug() << "RHI initialized with Vulkan backend successfully";
}

void WaveItem::updateUniformBuffer()
{
    UniformBlock ub = {};
    ub.time = (float)m_time;
    ub.speed = (float)m_speed;
    ub.amplitude = (float)m_amplitude;
    ub.frequency = (float)m_frequency;
    ub.baseColor = QVector4D(m_baseColor.redF(), m_baseColor.greenF(), m_baseColor.blueF(), m_baseColor.alphaF());
    ub.waveColor = QVector4D(m_waveColor.redF(), m_waveColor.greenF(), m_waveColor.blueF(), m_waveColor.alphaF());
    ub.threshold = (float)m_threshold;
    ub.dustIntensity = (float)m_dustIntensity;
    ub.minDist = (float)m_minDist;
    ub.maxDist = (float)m_maxDist;
    ub.maxDraws = m_maxDraws;
    ub.resolution = QVector2D(width(), height());

    m_rhi->updateBuffer(m_uniformBuffer, (const char*)&ub, sizeof(UniformBlock));
}

// Property setters with change detection, buffer update, and render trigger.
void WaveItem::setTime(qreal time)
{
    if (m_time == time) return;
    m_time = time;
    updateUniformBuffer();
    update();
    emit timeChanged();
}

void WaveItem::setSpeed(qreal speed)
{
    if (m_speed == speed) return;
    m_speed = speed;
    updateUniformBuffer();
    update();
    emit speedChanged();
}

void WaveItem::setAmplitude(qreal amplitude)
{
    if (m_amplitude == amplitude) return;
    m_amplitude = amplitude;
    updateUniformBuffer();
    update();
    emit amplitudeChanged();
}

void WaveItem::setFrequency(qreal frequency)
{
    if (m_frequency == frequency) return;
    m_frequency = frequency;
    updateUniformBuffer();
    update();
    emit frequencyChanged();
}

void WaveItem::setBaseColor(const QColor &color)
{
    if (m_baseColor == color) return;
    m_baseColor = color;
    updateUniformBuffer();
    update();
    emit baseColorChanged();
}

void WaveItem::setWaveColor(const QColor &color)
{
    if (m_waveColor == color) return;
    m_waveColor = color;
    updateUniformBuffer();
    update();
    emit waveColorChanged();
}

void WaveItem::setThreshold(qreal threshold)
{
    if (m_threshold == threshold) return;
    m_threshold = threshold;
    updateUniformBuffer();
    update();
    emit thresholdChanged();
}

void WaveItem::setDustIntensity(qreal dustIntensity)
{
    if (m_dustIntensity == dustIntensity) return;
    m_dustIntensity = dustIntensity;
    updateUniformBuffer();
    update();
    emit dustIntensityChanged();
}

void WaveItem::setMinDist(qreal minDist)
{
    if (m_minDist == minDist) return;
    m_minDist = minDist;
    updateUniformBuffer();
    update();
    emit minDistChanged();
}

void WaveItem::setMaxDist(qreal maxDist)
{
    if (m_maxDist == maxDist) return;
    m_maxDist = maxDist;
    updateUniformBuffer();
    update();
    emit maxDistChanged();
}

void WaveItem::setMaxDraws(int maxDraws)
{
    if (m_maxDraws == maxDraws) return;
    m_maxDraws = maxDraws;
    updateUniformBuffer();
    update();
    emit maxDrawsChanged();
}

// Load GLSL source (used for fragment shader).
QByteArray WaveItem::loadShaderSource(const QString &path) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open shader:" << path;
        return {};
    }
    QByteArray data = f.readAll();
    qDebug() << "Loaded shader source from" << path << "with size" << data.size() << "bytes";
    return data;
}

// Vertex shader GLSL (Vulkan-compatible, detailed with comments).
const char *WaveItem::vertexShader() const
{
    return 
        "#version 450\n"
        "// Input attributes for vertex position and texture coordinates.\n"
        "layout(location = 0) in vec4 vertex;\n"
        "layout(location = 1) in vec2 texCoord;\n"
        "// Output texture coordinate for fragment shader.\n"
        "layout(location = 0) out vec2 coord;\n"
        "// Uniform matrix for transformation.\n"
        "layout(location = 0) uniform mat4 matrix;\n"
        "void main() {\n"
        "    // Pass texture coordinate to fragment.\n"
        "    coord = texCoord;\n"
        "    // Transform vertex position.\n"
        "    gl_Position = matrix * vertex;\n"
        "}\n";
}

// Fragment shader GLSL (adapted from your provided code, with Vulkan uniforms and detailed comments).
const char* WaveItem::fragmentShader() const
{
    static QByteArray src;  // Make it static to persist
    if (src.isEmpty()) {
        src = loadShaderSource(":/shaders/WaveShader.frag");
        if (src.isEmpty()) {
            qWarning() << "Using fallback fragment shader";
            src = "#version 450\n"
                  "layout(location = 0) in vec2 coord;\n"
                  "layout(location = 0) out vec4 fragColor;\n"
                  "layout(std140, binding = 0) uniform UniformBlock {\n"
                  "    float time;\n"
                  "    float speed;\n"
                  "    float amplitude;\n"
                  "    float frequency;\n"
                  "    vec4 baseColor;\n"
                  "    vec4 waveColor;\n"
                  "    float threshold;\n"
                  "    float dustIntensity;\n"
                  "    float minDist;\n"
                  "    float maxDist;\n"
                  "    int maxDraws;\n"
                  "    vec2 resolution;\n"
                  "} ub;\n"
                  "void main() {\n"
                  "    vec2 uv = coord;\n"
                  "    float wave = sin(uv.x * ub.frequency + ub.time * ub.speed) * ub.amplitude;\n"
                  "    vec3 color = mix(ub.baseColor.rgb, ub.waveColor.rgb, wave + 0.5);\n"
                  "    fragColor = vec4(color, 1.0);\n"
                  "}\n";
        }
    }
    return src.constData();
}