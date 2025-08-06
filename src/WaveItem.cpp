// src/WaveItem.cpp
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.

#include "WaveItem.h"
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QFile>
#include <QQuickWindow>
#include <QVector2D>

//================================================================
// WaveItem
//================================================================

WaveItem::WaveItem(QQuickItem *parent) : QQuickItem(parent)
{
    // This flag tells Qt Quick that we are providing our own rendered content.
    setFlag(ItemHasContents, true);
}

// Property Setters - these trigger an update() call to redraw
void WaveItem::setTime(qreal time)
{
    if (m_time != time) {
        m_time = time;
        update();
        emit timeChanged();
    }
}

void WaveItem::setSpeed(qreal speed)
{
    if (m_speed != speed) {
        m_speed = speed;
        update();
        emit speedChanged();
    }
}

void WaveItem::setAmplitude(qreal amplitude)
{
    if (m_amplitude != amplitude) {
        m_amplitude = amplitude;
        update();
        emit amplitudeChanged();
    }
}

void WaveItem::setFrequency(qreal frequency)
{
    if (m_frequency != frequency) {
        m_frequency = frequency;
        update();
        emit frequencyChanged();
    }
}

void WaveItem::setBaseColor(const QColor &color)
{
    if (m_baseColor != color) {
        m_baseColor = color;
        update();
        emit baseColorChanged();
    }
}

void WaveItem::setWaveColor(const QColor &color)
{
    if (m_waveColor != color) {
        m_waveColor = color;
        update();
        emit waveColorChanged();
    }
}

void WaveItem::setThreshold(qreal threshold)
{
    if (m_threshold != threshold) {
        m_threshold = threshold;
        update();
        emit thresholdChanged();
    }
}

void WaveItem::setDustIntensity(qreal dustIntensity)
{
    if (m_dustIntensity != dustIntensity) {
        m_dustIntensity = dustIntensity;
        update();
        emit dustIntensityChanged();
    }
}

void WaveItem::setMinDist(qreal minDist)
{
    if (m_minDist != minDist) {
        m_minDist = minDist;
        update();
        emit minDistChanged();
    }
}

void WaveItem::setMaxDist(qreal maxDist)
{
    if (m_maxDist != maxDist) {
        m_maxDist = maxDist;
        update();
        emit maxDistChanged();
    }
}

void WaveItem::setMaxDraws(int maxDraws)
{
    if (m_maxDraws != maxDraws) {
        m_maxDraws = maxDraws;
        update();
        emit maxDrawsChanged();
    }
}

QSGNode *WaveItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    QSGGeometryNode *node = nullptr;
    QSGGeometry *geometry = nullptr;

    if (!oldNode) {
        // Create a new node if one doesn't exist.
        node = new QSGGeometryNode;
        geometry = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
        geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry, true);
        node->setMaterial(new WaveMaterial);
        node->setFlag(QSGNode::OwnsMaterial, true);
    } else {
        node = static_cast<QSGGeometryNode *>(oldNode);
        geometry = node->geometry();
    }

    // Update geometry vertices to match the item's size.
    const QRectF r = boundingRect();
    QSGGeometry::TexturedPoint2D *vertices = geometry->vertexDataAsTexturedPoint2D();
    vertices[0].set(r.left(), r.top(), 0, 0);
    vertices[1].set(r.left(), r.bottom(), 0, 1);
    vertices[2].set(r.right(), r.top(), 1, 0);
    vertices[3].set(r.right(), r.bottom(), 1, 1);
    node->markDirty(QSGNode::DirtyGeometry);

    // Update the material with the current property values.
    WaveMaterial *material = static_cast<WaveMaterial *>(node->material());
    material->m_time = m_time;
    material->m_speed = m_speed;
    material->m_amplitude = m_amplitude;
    material->m_frequency = m_frequency;
    material->m_baseColor = m_baseColor;
    material->m_waveColor = m_waveColor;
    material->m_threshold = m_threshold;
    material->m_dustIntensity = m_dustIntensity;
    material->m_minDist = m_minDist;
    material->m_maxDist = m_maxDist;
    material->m_maxDraws = m_maxDraws;
    material->m_resolution = size();
    node->markDirty(QSGNode::DirtyMaterial);

    return node;
}


//================================================================
// WaveMaterial
//================================================================

WaveMaterial::WaveMaterial()
{
    setFlag(Blending, true);
}

QSGMaterialType *WaveMaterial::type() const
{
    static QSGMaterialType type;
    return &type;
}

int WaveMaterial::compare(const QSGMaterial *other) const
{
    // For simplicity, we just check if it's the same instance.
    return (this == other) ? 0 : 1;
}

QSGMaterialShader *WaveMaterial::createShader(QSGRendererInterface::RenderMode) const
{
    return new WaveShader;
}


//================================================================
// WaveShader
//================================================================

WaveShader::WaveShader()
{
    // Load fragment shader from resource
    QFile fragFile(":/shaders/WaveShader.frag");
    fragFile.open(QIODevice::ReadOnly);
    QByteArray fragSource = fragFile.readAll();

    // Vertex shader source
    QByteArray vertSource =
        "#version 330 core\n"
        "in highp vec4 vertex;\n"
        "in highp vec2 texCoord;\n"
        "uniform highp mat4 matrix;\n"
        "out highp vec2 coord;\n"
        "void main() {\n"
        "    coord = texCoord;\n"
        "    gl_Position = matrix * vertex;\n"
        "}\n";

    // Set shader sources using Qt6 approach
    setShaderSourceCode(VertexStage, vertSource);
    setShaderSourceCode(FragmentStage, fragSource);
}



bool WaveShader::updateUniformData(RenderState &state, QSGMaterial *newMaterial, QSGMaterial *oldMaterial)
{
    Q_UNUSED(oldMaterial);
    WaveMaterial *mat = static_cast<WaveMaterial *>(newMaterial);

    // Update the matrix uniform if it has changed.
    if (state.isMatrixDirty()) {
        program()->setUniformValue("matrix", state.combinedMatrix());
    }

    // Update all our custom uniforms using string names (Qt6 approach).
    program()->setUniformValue("uniform_time", (float)mat->m_time);
    program()->setUniformValue("uniform_speed", (float)mat->m_speed);
    program()->setUniformValue("uniform_amplitude", (float)mat->m_amplitude);
    program()->setUniformValue("uniform_frequency", (float)mat->m_frequency);
    program()->setUniformValue("uniform_baseColor", mat->m_baseColor);
    program()->setUniformValue("uniform_waveColor", mat->m_waveColor);
    program()->setUniformValue("uniform_threshold", (float)mat->m_threshold);
    program()->setUniformValue("uniform_dustIntensity", (float)mat->m_dustIntensity);
    program()->setUniformValue("uniform_minDist", (float)mat->m_minDist);
    program()->setUniformValue("uniform_maxDist", (float)mat->m_maxDist);
    program()->setUniformValue("uniform_maxDraws", mat->m_maxDraws);
    program()->setUniformValue("uniform_resolution", QVector2D(mat->m_resolution.width(), mat->m_resolution.height()));

    return true;
}
