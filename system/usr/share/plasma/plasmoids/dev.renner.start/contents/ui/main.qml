// renner.dev Start (owner, 2026-11-08: "i am generally not a fan of start
// menus and like the apple spotlight search the most ... you need to know what
// you have installed and dont get a overview"). The start button (and the
// Windows key) opens a centred layer: search first, pinned apps and all apps
// below. Models come from Plasma's own kicker plugin (as Kickoff uses them).
import QtQuick
import QtQuick.Layouts
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.kirigami as Kirigami
import org.kde.plasma.private.kicker as Kicker

PlasmoidItem {
    id: root

    // A normal Plasma popup above the start button: opens on the screen of
    // the panel that was clicked, and the Windows key opens it natively.
    preferredRepresentation: compactRepresentation
    Plasmoid.icon: Plasmoid.configuration.icon || "renner-start"
    toolTipMainText: i18n("Start")
    toolTipSubText: i18n("Search apps, settings and files")

    // All apps, A to Z: the root model's flat "All Applications" list (the
    // plain AppsModel keeps its categories).
    Kicker.RootModel {
        id: rootApps
        appletInterface: root
        flat: true
        sorted: true
        showSeparators: false
        showAllApps: true
        showAllAppsCategorized: false
        showRecentApps: false
        showRecentDocs: false
        showRecentFolders: false
        showPowerSession: false
        showFavoritesPlaceholder: false
        appNameFormat: 0
        Component.onCompleted: refresh()
    }
    // Row 0 is "All Applications"; the rows after it are the categories.
    readonly property var allApps: rootApps.count > 0 ? rootApps.modelForRow(0) : null

    // Pinned apps (the list in the configuration).
    Kicker.SimpleFavoritesModel {
        id: pinnedApps
        favorites: Plasmoid.configuration.favorites
    }

    // Search: apps, settings, files, calculator ... (KRunner), one list.
    Kicker.RunnerModel {
        id: searchResults
        appletInterface: root
        mergeResults: true
        favoritesModel: pinnedApps
    }

    fullRepresentation: Launcher {
        // Distinct ids: `appsModel: appsModel` would bind the property to itself.
        appsModel: root.allApps
        pinnedModel: pinnedApps
        runnerModel: searchResults
        onDone: root.expanded = false
    }

    onExpandedChanged: (expanded) => {
        if (expanded && root.fullRepresentationItem) {
            root.fullRepresentationItem.reset();
        }
    }

    compactRepresentation: MouseArea {
        id: button
        // A square as tall as the panel (without a size the panel lays it out at 0 px).
        Layout.minimumWidth: height
        Layout.preferredWidth: height
        Layout.maximumWidth: height
        hoverEnabled: true
        onClicked: root.expanded = !root.expanded
        Kirigami.Icon {
            anchors.fill: parent
            anchors.margins: 2
            source: Plasmoid.icon || "renner-start"
            active: button.containsMouse
        }
    }
}
