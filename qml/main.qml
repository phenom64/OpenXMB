// qml/main.qml
//
// TM & (C) 2025 Syndromatic Ltd. All rights reserved.
// Designed by Kavish Krishnakumar in Manchester.
//
// This file is part of the OpenXMB project, licensed under GPLv3.

import QtQuick 2.15
import QtQuick.Window 2.15
import Syndromatic.XMS 1.0

Window {
    id: root
    visible: true
    color: "black"  // Base color if rendering fails.

    // Fullscreen wave background using custom WaveItem with Vulkan.
    WaveItem {
        anchors.fill: parent

        // Properties for animation and customization (mapped to uniforms in the shader).
        time: 0.0  // Drives the animation.
        speed: 0.5  // How fast the wave moves.
        amplitude: 0.05  // Wave height.
        frequency: 10.0  // Wave density.
        baseColor: "#000000"  // Dark base.
        waveColor: "#1A1A1A"  // Subtle grey wave for XMB vibe.
        threshold: 0.99
        dustIntensity: 1.0
        minDist: 0.13
        maxDist: 40.0
        maxDraws: 40

        // Animate the time property for continuous motion.
        Timer {
            interval: 16  // ~60 FPS.
            running: true
            repeat: true
            onTriggered: {
                parent.time += 0.01;  // Increment for smooth animation.
                if (parent.time > 1000) parent.time = 0;  // Loop to prevent overflow.
            }
        }
    }

    // Placeholder for future UI elements (e.g., icons, menus).
    Item {
        anchors.fill: parent
        // Add typography, icons, etc., here later.
    }
}