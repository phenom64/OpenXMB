#include <QGuiApplication>
#include <QFontDatabase>
#include <QLoggingCategory>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QQmlContext>

#include "WaveItem.h"

int main(int argc, char *argv[]) {
  qputenv("QSG_RHI_BACKEND", "vulkan");
  qputenv("QSG_INFO", "1");

  QGuiApplication app(argc, argv);
  app.setApplicationName("OpenXMB");
  app.setOrganizationName("Syndromatic Ltd");
  app.setApplicationVersion("0.1.0");

  QFontDatabase::addApplicationFont(
      "qrc:/interfaceFX/TypefaceServer/Play-Regular.ttf");
  QFontDatabase::addApplicationFont(
      "qrc:/interfaceFX/TypefaceServer/Play-Bold.ttf");
  QGuiApplication::setFont(QFont("Play"));

  qmlRegisterType<WaveItem>("Syndromatic.XMS", 1, 0, "WaveItem");

  QQmlApplicationEngine engine;
  const QUrl url(u"qrc:/qml/main.qml"_qs);
  QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
                   [url](QObject *obj, const QUrl &objUrl) {
                     if (!obj && url == objUrl) QCoreApplication::exit(-1);
                   },
                   Qt::QueuedConnection);
  engine.load(url);

  return app.exec();
}