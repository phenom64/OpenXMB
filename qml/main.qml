// qml/main.qml
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.

import QtQuick 2.15
import QtQuick.Window 2.15
import QtMultimedia 6.5
import Syndromatic.XMS 1.0

Window {
  id: root
  visible: true
  visibility: Window.FullScreen
  color: "black"

  WaveItem {
    id: wave
    anchors.fill: parent
    useXmbScheme: true
    speed: 0.5
    amplitude: 0.05
    frequency: 10.0
    threshold: 0.99
    dustIntensity: 1.0
    minDist: 0.13
    maxDist: 40.0
    maxDraws: 40
    brightness: 1.0

    Timer {
      interval: 16
      running: true
      repeat: true
      onTriggered: {
        wave.time += 0.01
        if (wave.time > 1000) wave.time = 0
        wave.update()
      }
    }
  }

  Audio {
    id: startup
    source: "qrc:/interfaceFX/AudioServer/NSE.startup.ogg"
    loops: 1
    onStatusChanged: if (status === Audio.Ready) play()
  }

  Item {
    id: splash
    anchors.fill: parent
    opacity: 0.0
    visible: true

    Text {
      id: title
      text: "Syndromatic Engineering Bharat Britannia"
      color: "white"
      anchors.horizontalCenter: parent.horizontalCenter
      anchors.verticalCenter: parent.verticalCenter
      wrapMode: Text.WordWrap
      horizontalAlignment: Text.AlignHCenter
      font.family: "Play"
      font.bold: true
      font.pixelSize: Math.round(Math.min(root.width, root.height) * 0.06)
      opacity: splash.opacity
      layer.enabled: true
      layer.smooth: true
    }

    SequentialAnimation on opacity {
      id: splashAnim
      running: true
      NumberAnimation { from: 0; to: 1; duration: 900; easing.type: Easing.InOutQuad }
      PauseAnimation { duration: 1400 }
      NumberAnimation { from: 1; to: 0; duration: 900; easing.type: Easing.InOutQuad }
      onFinished: splash.visible = false
    }
  }

  Keys.onPressed: if (event.key === Qt.Key_Escape || event.key === Qt.Key_Q) Qt.quit()
}