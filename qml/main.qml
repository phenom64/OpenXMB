// qml/main.qml
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.

import QtQuick 2.15
import QtQuick.Window 2.15
import Syndromatic.XMS 1.0 // Import our custom C++ components

Window {
    id: root
    visible: true
    color: "black"

    // Use our powerful, custom C++ WaveItem for rendering.
    WaveItem {
        id: waveBackground
        anchors.fill: parent

        // Set initial properties for the wave.
        speed: 0.5
        amplitude: 0.05
        frequency: 10.0
        baseColor: "#000000"
        waveColor: "#1A1A1A"

        // Animate the 'time' property for continuous motion.
        NumberAnimation on time {
            from: 0
            to: 3600 // Animate over an hour's worth of seconds
            duration: 3600 * 1000 // at 1 second per second
            loops: Animation.Infinite
            running: true
        }
    }

    // Placeholder for future UI elements (e.g., icons, menus).
    Item {
        anchors.fill: parent
        // Add typography, icons, etc., here later.
    }
}
