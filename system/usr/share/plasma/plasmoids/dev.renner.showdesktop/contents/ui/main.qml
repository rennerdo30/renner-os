// renner.dev OS: the end of the taskbar shows the desktop, like Windows' sliver
// (owner, 2026-11-08). A slim area with a fine line that brightens on hover;
// a click toggles KWin's "Show Desktop" (the same action as Meta+D).
import QtQuick
import QtQuick.Layouts
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.plasma5support as P5Support
import org.kde.kirigami as Kirigami

PlasmoidItem {
    id: root
    preferredRepresentation: fullRepresentation
    Plasmoid.backgroundHints: PlasmaCore.Types.NoBackground
    toolTipMainText: i18n("Show desktop")

    fullRepresentation: MouseArea {
        id: area
        // A sliver, like Windows (owner, 2026-11-08: "still too wide").
        Layout.minimumWidth: 5
        Layout.preferredWidth: 5
        Layout.maximumWidth: 5
        Layout.fillHeight: true
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton

        Rectangle {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 1
            height: parent.height * 0.45
            color: Kirigami.Theme.textColor
            opacity: area.containsMouse ? 0.55 : 0.18
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        onClicked: runner.connectSource(
            "dbus-send --session --dest=org.kde.kglobalaccel /component/kwin "
            + "org.kde.kglobalaccel.Component.invokeShortcut string:'Show Desktop'")
    }

    P5Support.DataSource {
        id: runner
        engine: "executable"
        onNewData: (source) => disconnectSource(source)
    }
}
