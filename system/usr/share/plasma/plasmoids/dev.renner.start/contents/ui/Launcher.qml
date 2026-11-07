// The renner.dev Start panel: a compact popup above the start button (owner,
// 2026-11-08: a centred full-screen layer "on a 32inch multi monitor setup ...
// is pretty shit"). Search first; pinned apps and all apps A-Z below.
// Typing searches; Esc clears, then Plasma closes the popup; arrows move,
// Enter starts.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

Item {
    id: panel

    property var appsModel
    property var pinnedModel
    property var runnerModel
    signal done()

    readonly property bool searching: search.text.length > 0
    readonly property var results: runnerModel && runnerModel.count > 0 ? runnerModel.modelForRow(0) : null

    Layout.preferredWidth: 640
    Layout.preferredHeight: 700
    Layout.minimumWidth: 480
    Layout.minimumHeight: 420

    function reset() {
        search.clear();
        overview.contentY = 0;
        search.forceActiveFocus();
    }

    function launch(model, row) {
        if (model && row >= 0) {
            model.trigger(row, "", null);
            panel.done();
        }
    }

    Keys.onEscapePressed: (event) => {
        if (search.text.length > 0) {
            search.clear();
            event.accepted = true;
        } else {
            event.accepted = false;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 16

        // Search field
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            radius: 12
            color: "#0c0e16"
            border.width: 1
            border.color: search.activeFocus ? "#ff7a1a" : "#21ffffff"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 12
                Kirigami.Icon {
                    source: "search"
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    color: "#9ba5b9"
                }
                TextInput {
                    id: search
                    Layout.fillWidth: true
                    color: "#f7f8fb"
                    font.pixelSize: 16
                    font.family: "Inter"
                    selectionColor: "#66ff7a1a"
                    clip: true
                    focus: true
                    onTextChanged: {
                        panel.runnerModel.query = text;
                        resultList.currentIndex = 0;
                    }
                    Keys.onReturnPressed: {
                        if (panel.searching) {
                            panel.launch(panel.results, Math.max(0, resultList.currentIndex));
                        }
                    }
                    Keys.onDownPressed: resultList.incrementCurrentIndex()
                    Keys.onUpPressed: resultList.decrementCurrentIndex()
                    Keys.onEscapePressed: (event) => {
                        if (text.length > 0) {
                            clear();
                            event.accepted = true;
                        } else {
                            event.accepted = false;
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: search.text.length === 0
                        text: i18n("Search apps, settings and files")
                        color: "#8b95a8"
                        font: search.font
                    }
                }
            }
        }

        // Search results
        ListView {
            id: resultList
            visible: panel.searching
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: panel.results
            highlightMoveDuration: 0
            spacing: 2
            delegate: ItemRow {
                width: ListView.view.width
                selected: ListView.isCurrentItem
                onActivated: panel.launch(panel.results, index)
            }
            Text {
                anchors.centerIn: parent
                visible: panel.searching && resultList.count === 0 && !panel.runnerModel.querying
                text: i18n("Nothing found")
                color: "#8b95a8"
                font.pixelSize: 14
            }
        }

        // Overview: pinned, then all apps
        Flickable {
            id: overview
            visible: !panel.searching
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: overviewColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

            ColumnLayout {
                id: overviewColumn
                width: overview.width
                spacing: 8

                SectionTitle { text: i18n("Pinned") }
                AppGrid {
                    Layout.fillWidth: true
                    availableWidth: overview.width - 16
                    model: panel.pinnedModel
                    onLaunched: (row) => panel.launch(panel.pinnedModel, row)
                }
                SectionTitle { text: i18n("All apps"); Layout.topMargin: 12 }
                Repeater {
                    model: panel.appsModel
                    delegate: ItemRow {
                        Layout.fillWidth: true
                        compact: true
                        onActivated: panel.launch(panel.appsModel, index)
                    }
                }
            }
        }
    }

    component SectionTitle: Text {
        color: "#9ba5b9"
        font.pixelSize: 12
        font.weight: Font.DemiBold
        font.letterSpacing: 0.6
    }
}
