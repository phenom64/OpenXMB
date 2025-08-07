// src/WaveItem.h
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.

#ifndef WAVEITEM_H
#define WAVEITEM_H


#include <QQuickItem>
#include <QSGRendererInterface>
#include <QSGTexture>
#include <QSGGeometryNode>
#include <QFile>
#include <QByteArray>
#include <QDebug>
#include <QVector2D>
#include <QVector3D>
#include <QMatrix4x4>
#include <QColor>

// Forward declarations for QRhi types (private Qt headers)
class QRhi;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiShaderResourceBindings;
class QRhiBuffer;
class QRhiRenderPassDescriptor;
class QRhiRenderTarget;

// Custom QQuickItem for rendering a GPU-accelerated PS3-style wave shader using Vulkan via QRhi.
class WaveItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(qreal time READ time WRITE setTime NOTIFY timeChanged)
    Q_PROPERTY(qreal speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(qreal amplitude READ amplitude WRITE setAmplitude NOTIFY amplitudeChanged)
    Q_PROPERTY(qreal frequency READ frequency WRITE setFrequency NOTIFY frequencyChanged)
    Q_PROPERTY(QColor baseColor READ baseColor WRITE setBaseColor NOTIFY baseColorChanged)
    Q_PROPERTY(QColor waveColor READ waveColor WRITE setWaveColor NOTIFY waveColorChanged)
    Q_PROPERTY(qreal threshold READ threshold WRITE setThreshold NOTIFY thresholdChanged)
    Q_PROPERTY(qreal dustIntensity READ dustIntensity WRITE setDustIntensity NOTIFY dustIntensityChanged)
    Q_PROPERTY(qreal minDist READ minDist WRITE setMinDist NOTIFY minDistChanged)
    Q_PROPERTY(qreal maxDist READ maxDist WRITE setMaxDist NOTIFY maxDistChanged)
    Q_PROPERTY(int maxDraws READ maxDraws WRITE setMaxDraws NOTIFY maxDrawsChanged)

public:
    explicit WaveItem(QQuickItem *parent = nullptr);
    ~WaveItem() override;

    qreal time() const { return m_time; }
    void setTime(qreal time);

    qreal speed() const { return m_speed; }
    void setSpeed(qreal speed);

    qreal amplitude() const { return m_amplitude; }
    void setAmplitude(qreal amplitude);

    qreal frequency() const { return m_frequency; }
    void setFrequency(qreal frequency);

    QColor baseColor() const { return m_baseColor; }
    void setBaseColor(const QColor &color);

    QColor waveColor() const { return m_waveColor; }
    void setWaveColor(const QColor &color);

    qreal threshold() const { return m_threshold; }
    void setThreshold(qreal threshold);

    qreal dustIntensity() const { return m_dustIntensity; }
    void setDustIntensity(qreal dustIntensity);

    qreal minDist() const { return m_minDist; }
    void setMinDist(qreal minDist);

    qreal maxDist() const { return m_maxDist; }
    void setMaxDist(qreal maxDist);

    int maxDraws() const { return m_maxDraws; }
    void setMaxDraws(int maxDraws);

signals:
    void timeChanged();
    void speedChanged();
    void amplitudeChanged();
    void frequencyChanged();
    void baseColorChanged();
    void waveColorChanged();
    void thresholdChanged();
    void dustIntensityChanged();
    void minDistChanged();
    void maxDistChanged();
    void maxDrawsChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

private:
    void initializeRhi();
    void updateUniformBuffer();
    const char* vertexShader() const;
    const char* fragmentShader() const;
    QByteArray loadShaderSource(const QString &path) const;

    QRhi *m_rhi = nullptr;
    QRhiCommandBuffer *m_cb = nullptr;
    QRhiGraphicsPipeline *m_pipeline = nullptr;
    QRhiShaderResourceBindings *m_bindings = nullptr;
    QRhiBuffer *m_uniformBuffer = nullptr;
    QRhiBuffer *m_vertexBuffer = nullptr;
    QRhiRenderPassDescriptor *m_rpDesc = nullptr;
    QRhiRenderTarget *m_rt = nullptr;

    qreal m_time = 0.0;
    qreal m_speed = 0.5;
    qreal m_amplitude = 0.05;
    qreal m_frequency = 10.0;
    QColor m_baseColor = QColor("#000000");
    QColor m_waveColor = QColor("#1A1A1A");
    qreal m_threshold = 0.99;
    qreal m_dustIntensity = 1.0;
    qreal m_minDist = 0.13;
    qreal m_maxDist = 40.0;
    int m_maxDraws = 40;

    bool m_initialized = false;
};

#endif // WAVEITEM_H