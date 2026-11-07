// Pinned apps as a calm grid of tiles (icon above name).
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

GridLayout {
    id: grid
    property alias model: tiles.model
    // The width to fill (the grid's own width is 0 while it is laid out).
    property real availableWidth: 600
    signal launched(int row)

    columns: Math.max(1, Math.floor(availableWidth / 112))
    columnSpacing: 6
    rowSpacing: 6

    Repeater {
        id: tiles
        delegate: Rectangle {
            id: tile
            Layout.preferredWidth: 106
            Layout.preferredHeight: 96
            radius: 12
            color: hover.hovered ? "#12ffffff" : "transparent"
            HoverHandler { id: hover }
            TapHandler { onTapped: grid.launched(index) }
            ColumnLayout {
                anchors.centerIn: parent
                width: parent.width - 12
                spacing: 8
                Kirigami.Icon {
                    source: decoration
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                }
                Text {
                    Layout.fillWidth: true
                    text: display
                    color: "#f7f8fb"
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                }
            }
        }
    }
}
