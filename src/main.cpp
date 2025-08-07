/*
 * TM & (C) 2025 Syndromatic Ltd. All rights reserved.
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at  option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITH ABSOLUTELY NO WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <QGuiApplication>
#include <QFontDatabase>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QVulkanInstance>
#include <QSGRendererInterface>

#include "WaveItem.h"

using namespace Qt::StringLiterals;

static bool initVulkanInstance(QVulkanInstance &inst) {
  QByteArrayList instExt;
  instExt << "VK_KHR_portability_enumeration";
  inst.setExtensions(instExt);
  return inst.create();
}

int main(int argc, char *argv[]) {
  QGuiApplication app(argc, argv);
  app.setApplicationName("OpenXMB");
  app.setOrganizationName("Syndromatic Ltd");
  app.setApplicationVersion("0.1.0");

  QFontDatabase::addApplicationFont("qrc:/interfaceFX/TypefaceServer/Play-Regular.ttf");
  QFontDatabase::addApplicationFont("qrc:/interfaceFX/TypefaceServer/Play-Bold.ttf");
  QGuiApplication::setFont(QFont("Play"));

  // Prefer Vulkan; fall back to Metal if not available
  QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
  QVulkanInstance vkInst;
  const bool vulkanOk = initVulkanInstance(vkInst);
  if (!vulkanOk) {
    qWarning("Vulkan instance failed (MoltenVK not available?). Falling back to Metal.");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
  }

  qmlRegisterType<WaveItem>("Syndromatic.XMS", 1, 0, "WaveItem");

  QQmlApplicationEngine engine;
  const QUrl url(u"qrc:/qml/main.qml"_s);
  QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, &app,
                   [url](QObject *obj, const QUrl &objUrl) {
                     if (!obj && url == objUrl) QCoreApplication::exit(-1);
                   }, Qt::QueuedConnection);
  engine.load(url);

  for (QObject *obj : engine.rootObjects()) {
    if (auto *win = qobject_cast<QQuickWindow *>(obj)) {
      if (vulkanOk)
        win->setVulkanInstance(&vkInst);
      win->showFullScreen();
    }
  }

  return app.exec();
}