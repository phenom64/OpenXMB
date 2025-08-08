// qml/main.qml
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.

import QtQuick
import QtQuick.Window 2.15
import QtMultimedia
import Syndromatic.XMS 1.0

Window {
  id: root
  // Do not set 'visible' here; C++ will show full screen.
  color: "black"

  // Keys must be on an Item
  Item {
    id: keyLayer
    anchors.fill: parent
    focus: true
    Keys.onPressed: if (event.key === Qt.Key_Escape || event.key === Qt.Key_Q) Qt.quit()
  }

  WaveItem {
    id: wave
    anchors.fill: parent
    useXmbScheme: false
    baseColor: "#203a9a"
    waveColor: "#3a9aff"
    speed: 0.5
    amplitude: 0.12 //0.05
    frequency: 10.0
    threshold: 0.99
    dustIntensity: 1.0
    minDist: 0.10
    maxDist: 120.0
    maxDraws: 80
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

  MediaPlayer {
    id: startup
    source: "qrc:/interfaceFX/AudioServer/NSE.startup.ogg"
    audioOutput: AudioOutput { id: out; volume: 1.0 }
    Component.onCompleted: play()
  }

  Item {
    id: splash
    anchors.fill: parent
    opacity: 0.0
    visible: true

    Text {
      id: title
      text: "Syndromatic Design Engineering"
      color: "white"
      anchors.horizontalCenter: parent.horizontalCenter
      anchors.verticalCenter: parent.verticalCenter
      wrapMode: Text.WordWrap
      horizontalAlignment: Text.AlignHCenter
      font.family: "Play"
      font.bold: true
      font.pixelSize: Math.round(Math.min(root.width, root.height) * 0.04)
      opacity: splash.opacity
      layer.enabled: true
      layer.smooth: true
    }

    SequentialAnimation on opacity {
      running: true
      NumberAnimation { from: 0; to: 1; duration: 600; easing.type: Easing.InOutQuad }
      PauseAnimation { duration: 1600 }
      NumberAnimation { from: 1; to: 0; duration: 900; easing.type: Easing.InOutQuad }
      onFinished: splash.visible = false
    }
  }
}