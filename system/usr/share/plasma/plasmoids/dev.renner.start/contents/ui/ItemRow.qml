// One row: icon, name and (for search results) a short description.
// Used as a delegate; `display`, `decoration`, `description` come from the model.
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Rectangle {
    id: row
    property bool selected: false
    property bool compact: false
    signal activated()

    implicitHeight: compact ? 40 : 52
    radius: 10
    color: selected ? "#29ff7a1a" : (hover.hovered ? "#0fffffff" : "transparent")
    border.width: selected ? 1 : 0
    border.color: "#8cff7a1a"

    HoverHandler { id: hover }
    TapHandler { onTapped: row.activated() }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        spacing: 12
        Kirigami.Icon {
            source: typeof decoration !== "undefined" ? decoration : ""
            Layout.preferredWidth: row.compact ? 24 : 30
            Layout.preferredHeight: row.compact ? 24 : 30
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            Text {
                Layout.fillWidth: true
                text: typeof display !== "undefined" ? display : ""
                color: "#f7f8fb"
                font.pixelSize: row.compact ? 14 : 15
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                visible: !row.compact && text.length > 0
                text: typeof description !== "undefined" && description ? description : ""
                color: "#8b95a8"
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }
    }
}
