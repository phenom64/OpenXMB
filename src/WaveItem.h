// src/WaveItem.h
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.

#pragma once
#include <QColor>
#include <QQuickItem>
#include <QVector2D>
#include <QVector4D>

class WaveItem : public QQuickItem {
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
  Q_PROPERTY(qreal brightness READ brightness WRITE setBrightness NOTIFY brightnessChanged)
  Q_PROPERTY(bool useXmbScheme READ useXmbScheme WRITE setUseXmbScheme NOTIFY useXmbSchemeChanged)

public:
  explicit WaveItem(QQuickItem* parent = nullptr);
  ~WaveItem() override;

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
  qreal brightness() const { return m_brightness; }
  bool useXmbScheme() const { return m_useXmbScheme; }

  void setTime(qreal v);
  void setSpeed(qreal v);
  void setAmplitude(qreal v);
  void setFrequency(qreal v);
  void setBaseColor(const QColor& c);
  void setWaveColor(const QColor& c);
  void setThreshold(qreal v);
  void setDustIntensity(qreal v);
  void setMinDist(qreal v);
  void setMaxDist(qreal v);
  void setMaxDraws(int v);
  void setBrightness(qreal v);
  void setUseXmbScheme(bool v);

Q_SIGNALS:
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
  void brightnessChanged();
  void useXmbSchemeChanged();

protected:
  QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
  void geometryChange(const QRectF& newG, const QRectF& oldG) override;

private:
  void scheduleUpdate();
  void updateXmbScheme();

  qreal m_time = 0.0;
  qreal m_speed = 0.5;
  qreal m_amplitude = 0.05;
  qreal m_frequency = 10.0;
  QColor m_baseColor = QColor("#000000");
  QColor m_waveColor = QColor("#1A1A1A");
  qreal m_threshold = 0.99;
  qreal m_dustIntensity = 1.0;
  qreal m_minDist = 0.13;
  qreal m_maxDist = 120.0;
  int m_maxDraws = 40;
  qreal m_brightness = 1.0;
  bool m_useXmbScheme = true;
};