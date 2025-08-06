/*
 * TM & (C) 2025 Syndromatic Ltd. All rights reserved.
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <QGuiApplication>
#include <QQuickView>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QScreen>
#include <QRect>
#include "WaveItem.h"  // Include our custom wave item header.

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    // Register our custom WaveItem with the QML type system.
    // This makes it available as 'WaveItem' in QML files.
    qmlRegisterType<WaveItem>("Syndromatic.XMS", 1, 0, "WaveItem");

    // Set up the QML engine for rendering our UI.
    QQmlApplicationEngine engine;

    // Load the main QML file that defines the wave background.
    const QUrl url(QStringLiteral("qrc:/qml/main.qml"));
    engine.load(url);

    // Check if loading succeeded; bail out if not.
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    // Grab the root window object from the engine.
    QObject *rootObject = engine.rootObjects().first();
    QQuickWindow *window = qobject_cast<QQuickWindow *>(rootObject);
    if (!window) {
        return -1;
    }

    // Set window properties for a seamless, appliance-like experience.
    window->setTitle("Syndromatic XMS");
    window->setColor(Qt::black);  // Fallback black background.

    // Launch in borderless fullscreen mode, adapting to primary screen.
    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenGeometry = screen->geometry();
    window->setGeometry(screenGeometry);
    window->setFlags(Qt::FramelessWindowHint | Qt::Window);
    window->showFullScreen();

    return app.exec();
}