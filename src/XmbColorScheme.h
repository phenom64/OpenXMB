#pragma once
#include <QColor>
#include <QDateTime>
#include <QVector4D>

struct XmbScheme {
  QVector4D base;     // RGBA 0..1
  QVector4D wave;     // RGBA 0..1
  float brightness;   // 0..1.2
};

class XmbColorScheme {
public:
  static XmbScheme current(const QDateTime& dt);

private:
  static QVector4D toVec(const QColor& c, float a = 1.0f);
  static float hourBrightness(int hour, int minute);
};