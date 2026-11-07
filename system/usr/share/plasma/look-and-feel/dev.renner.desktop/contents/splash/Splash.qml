// renner.dev OS login splash (KSplash), shown while Plasma starts after login.
// The dark brand background, the r. mark and a thin progress line in the
// brand edge gradient. KSplash raises `stage` from 1 to 6 as Plasma comes up.
import QtQuick

Rectangle {
    id: root
    color: "#08090f"
    property int stage

    onStageChanged: {
        if (stage === 2) {
            intro.running = true;
        }
    }

    // Warm and cool glows, like the wallpaper and the installer.
    Rectangle {
        width: root.width * 0.9
        height: width
        x: -width * 0.35
        y: -height * 0.45
        radius: width / 2
        opacity: 0.18
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#ff7a1a" }
            GradientStop { position: 1.0; color: "transparent" }
        }
    }

    Item {
        id: content
        anchors.centerIn: parent
        width: 320
        height: 200
        opacity: 0

        Image {
            id: mark
            source: "images/mark.svg"
            sourceSize.width: 96
            sourceSize.height: 96
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
        }

        Text {
            id: word
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: mark.bottom
            anchors.topMargin: 18
            textFormat: Text.StyledText
            text: "renner<font color=\"#ff7a1a\">.dev</font>"
            color: "#f7f8fb"
            font.family: "Inter"
            font.pixelSize: 28
            font.weight: Font.Bold
        }

        // Progress: a slim track and the brand gradient growing with `stage`.
        Rectangle {
            id: track
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: word.bottom
            anchors.topMargin: 28
            width: 180
            height: 3
            radius: 2
            color: "#ffffff14"

            Rectangle {
                height: parent.height
                radius: parent.radius
                width: parent.width * Math.min(1, Math.max(0.08, (root.stage - 1) / 5))
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "#ff7a1a" }
                    GradientStop { position: 1.0; color: "#2de2f0" }
                }
                Behavior on width { NumberAnimation { duration: 400; easing.type: Easing.OutCubic } }
            }
        }
    }

    OpacityAnimator {
        id: intro
        target: content
        from: 0
        to: 1
        duration: 500
        easing.type: Easing.OutCubic
    }
}
