#include "XmbColorScheme.h"
#include <algorithm>

static const QColor kPivots[24] = {
    QColor("#C9B95A"), // Jan 15
    QColor("#7F2020"), // Jan 24
    QColor("#2D7E29"), // Feb 15
    QColor("#3A9930"), // Feb 24
    QColor("#7FCC3E"), // Mar 15
    QColor("#F59CB2"), // Mar 24
    QColor("#E06AA7"), // Apr 15
    QColor("#9AA39F"), // Apr 24
    QColor("#6AA06A"), // May 15
    QColor("#9B6ACF"), // May 24
    QColor("#6A2EBF"), // Jun 15
    QColor("#59C7CF"), // Jun 24
    QColor("#4AA2FF"), // Jul 15
    QColor("#0C2B7E"), // Jul 24
    QColor("#0A1E59"), // Aug 15
    QColor("#E58AD2"), // Aug 24
    QColor("#7B1F1F"), // Sep 15
    QColor("#D6C36A"), // Sep 24
    QColor("#8A5A2E"), // Oct 15
    QColor("#B46A1F"), // Oct 24
    QColor("#AF2A2A"), // Nov 15
    QColor("#FF2A2A"), // Nov 24
    QColor("#C2A3E6"), // Dec 15
    QColor("#9AA3AB")  // Dec 24
};

static int prevPivotIndex(const QDate& d) {
  // 0 -> Jan 15, 1 -> Jan 24, ..., 23 -> Dec 24
  const int m = d.month();
  const int day = d.day();
  const int base = (m - 1) * 2;
  if (day >= 24) return std::min(23, base + 1);
  if (day >= 15) return base;
  // previous month's 24th
  return (base + 23) % 24;
}

static int nextPivotIndex(int prev) {
  return (prev + 1) % 24;
}

static QDate pivotDateForIndex(int year, int idx) {
  const int month = idx / 2 + 1;
  const int day = (idx % 2 == 0) ? 15 : 24;
  return QDate(year, month, day);
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

static QColor lerp(const QColor& a, const QColor& b, float t) {
  return QColor::fromRgbF(
      std::clamp(lerp(a.redF(), b.redF(), t), 0.0f, 1.0f),
      std::clamp(lerp(a.greenF(), b.greenF(), t), 0.0f, 1.0f),
      std::clamp(lerp(a.blueF(), b.blueF(), t), 0.0f, 1.0f), 1.0f);
}

QVector4D XmbColorScheme::toVec(const QColor& c, float a) {
  return QVector4D(c.redF(), c.greenF(), c.blueF(), a);
}

float XmbColorScheme::hourBrightness(int hour, int minute) {
  static const float H[24] = {
      0.10f, 0.10f, 0.10f, 0.10f, 0.18f, 0.28f, 0.45f, 0.65f, 0.80f, 0.90f,
      1.00f, 1.00f, 1.00f, 0.98f, 0.95f, 0.85f, 0.75f, 0.60f, 0.42f, 0.30f,
      0.20f, 0.12f, 0.10f, 0.10f};
  const int h0 = std::clamp(hour, 0, 23);
  const int h1 = (h0 + 1) % 24;
  const float t = std::clamp(minute / 60.0f, 0.0f, 1.0f);
  return lerp(H[h0], H[h1], t);
}

XmbScheme XmbColorScheme::current(const QDateTime& dt) {
  const QDate d = dt.date();
  const int year = d.year();

  const int p0 = prevPivotIndex(d);
  const int p1 = nextPivotIndex(p0);
  const QDate date0 = pivotDateForIndex(year, p0);
  const QDate date1 =
      (p1 == 0) ? pivotDateForIndex(year + 1, p1) : pivotDateForIndex(year, p1);

  const qint64 t0 = QDateTime(date0, QTime(12, 0)).toMSecsSinceEpoch();
  const qint64 t1 = QDateTime(date1, QTime(12, 0)).toMSecsSinceEpoch();
  const qint64 tx = dt.toMSecsSinceEpoch();
  float t = 0.0f;
  if (t1 != t0) t = std::clamp((tx - t0) / float(t1 - t0), 0.0f, 1.0f);

  const QColor c0 = kPivots[p0];
  const QColor c1 = kPivots[p1];
  const QColor base = lerp(c0, c1, t);
  const QColor wave = QColor::fromRgbF(
      std::clamp(base.redF() * 0.7f, 0.0f, 1.0f),
      std::clamp(base.greenF() * 0.7f, 0.0f, 1.0f),
      std::clamp(base.blueF() * 0.7f, 0.0f, 1.0f), 1.0f);

  const float br =
      std::clamp(hourBrightness(dt.time().hour(), dt.time().minute()), 0.0f,
                 1.2f);

  XmbScheme s;
  s.base = toVec(base);
  s.wave = toVec(wave);
  s.brightness = br;
  return s;
}