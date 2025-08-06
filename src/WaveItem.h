// src/WaveItem.h
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.

#ifndef WAVEITEM_H
#define WAVEITEM_H

#include <QQuickItem>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <QSGGeometryNode>
#include <QOpenGLShaderProgram>

// Forward declarations
class WaveMaterial;
class WaveShader;

// Custom QQuickItem for rendering a GPU-accelerated PS3-style wave shader.
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

    // Property getters
    qreal time() const { return m_time; }
    qreal speed() const { return m_speed; }
    qreal amplitude() const { return m_amplitude; }
    qreal frequency() const { return m_frequency; }
    QColor baseColor() const { return m_baseColor; }
    QColor waveColor() const { return m_waveColor; }
    qreal threshold() const { return m_threshold; }
    qreal dustIntensity() const { return m_dustIntensity; }
    qreal minDist() const { return m_minDist; }
    qreal maxDist() const { return m_maxDist; }
    int maxDraws() const { return m_maxDraws; }

    // Property setters
    void setTime(qreal time);
    void setSpeed(qreal speed);
    void setAmplitude(qreal amplitude);
    void setFrequency(qreal frequency);
    void setBaseColor(const QColor &color);
    void setWaveColor(const QColor &color);
    void setThreshold(qreal threshold);
    void setDustIntensity(qreal dustIntensity);
    void setMinDist(qreal minDist);
    void setMaxDist(qreal maxDist);
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
    // This is the main entry point for rendering our custom item.
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private:
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
};


// The material defines the state (uniforms) for our shader.
class WaveMaterial : public QSGMaterial
{
public:
    WaveMaterial();
    QSGMaterialType *type() const override;
    QSGMaterialShader *createShader(QSGRendererInterface::RenderMode renderMode) const override;
    int compare(const QSGMaterial *other) const override;

    // We store all uniform data directly in the material.
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
    QSizeF m_resolution;
};


// The shader class handles compiling and linking the GLSL code.
class WaveShader : public QSGMaterialShader
{
public:
    WaveShader();
    bool updateUniformData(RenderState &state, QSGMaterial *newMaterial, QSGMaterial *oldMaterial) override;


};

#endif // WAVEITEM_H
