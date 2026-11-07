// The renner.dev Start layer: a dimmed full-screen window with a centred card.
// Typing searches (results replace the overview); Esc clears, then closes;
// arrows move, Enter starts.
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kde.plasma.private.kicker as Kicker

Kicker.DashboardWindow {
    id: win

    property var appsModel
    property var pinnedModel
    property var runnerModel
    readonly property bool searching: search.text.length > 0
    readonly property var results: runnerModel && runnerModel.count > 0 ? runnerModel.modelForRow(0) : null

    backgroundColor: Qt.rgba(0.03, 0.035, 0.06, 0.62)
    keyEventProxy: search

    onKeyEscapePressed: {
        if (search.text.length > 0) {
            search.clear();
        } else {
            win.toggle();
        }
    }
    onVisibleChanged: {
        if (visible) {
            search.clear();
            search.forceActiveFocus();
            content.opacity = 0;
            content.scale = 0.97;
            intro.restart();
        }
    }

    function launch(model, row) {
        if (model && row >= 0) {
            model.trigger(row, "", null);
            win.toggle();
        }
    }

    mainItem: MouseArea {
        anchors.fill: parent
        // A click outside the card closes it.
        onClicked: win.toggle()

        Rectangle {
            id: content
            width: Math.min(parent.width - 96, 760)
            height: Math.min(parent.height - 120, 620)
            anchors.centerIn: parent
            radius: 22
            color: "#f011131d"
            border.width: 1
            border.color: "#1affffff"

            ParallelAnimation {
                id: intro
                NumberAnimation { target: content; property: "opacity"; to: 1; duration: 160; easing.type: Easing.OutCubic }
                NumberAnimation { target: content; property: "scale"; to: 1; duration: 200; easing.type: Easing.OutCubic }
            }

            MouseArea { anchors.fill: parent }   // clicks on the card stay

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 28
                spacing: 18

                // Search field
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 54
                    radius: 14
                    color: "#0c0e16"
                    border.width: 1
                    border.color: search.activeFocus ? "#ff7a1a" : "#21ffffff"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 18
                        anchors.rightMargin: 18
                        spacing: 12
                        Kirigami.Icon {
                            source: "search"
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 22
                            color: "#9ba5b9"
                        }
                        TextInput {
                            id: search
                            Layout.fillWidth: true
                            color: "#f7f8fb"
                            font.pixelSize: 19
                            font.family: "Inter"
                            selectionColor: "#66ff7a1a"
                            clip: true
                            onTextChanged: {
                                runnerModel.query = text;
                                resultList.currentIndex = 0;
                            }
                            Keys.onReturnPressed: {
                                if (win.searching) {
                                    win.launch(win.results, Math.max(0, resultList.currentIndex));
                                }
                            }
                            Keys.onDownPressed: resultList.incrementCurrentIndex()
                            Keys.onUpPressed: resultList.decrementCurrentIndex()
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
                    visible: win.searching
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: win.results
                    highlightMoveDuration: 0
                    spacing: 2
                    delegate: ItemRow {
                        width: ListView.view.width
                        selected: ListView.isCurrentItem
                        onActivated: win.launch(win.results, index)
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: win.searching && resultList.count === 0 && !runnerModel.querying
                        text: i18n("Nothing found")
                        color: "#8b95a8"
                        font.pixelSize: 15
                    }
                }

                // Overview: pinned, then all apps
                Flickable {
                    id: overview
                    visible: !win.searching
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentHeight: overviewColumn.implicitHeight
                    QQC2.ScrollBar.vertical: QQC2.ScrollBar {}

                    ColumnLayout {
                        id: overviewColumn
                        width: overview.width
                        spacing: 10

                        SectionTitle { text: i18n("Pinned") }
                        AppGrid {
                            Layout.fillWidth: true
                            availableWidth: overview.width - 16
                            model: win.pinnedModel
                            onLaunched: (row) => win.launch(win.pinnedModel, row)
                        }
                        SectionTitle { text: i18n("All apps"); Layout.topMargin: 14 }
                        Repeater {
                            model: win.appsModel
                            delegate: ItemRow {
                                Layout.fillWidth: true
                                compact: true
                                onActivated: win.launch(win.appsModel, index)
                            }
                        }
                    }
                }
            }
        }
    }

    component SectionTitle: Text {
        color: "#9ba5b9"
        font.pixelSize: 13
        font.weight: Font.DemiBold
        font.letterSpacing: 0.6
    }
}
