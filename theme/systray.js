// renner.dev OS system tray, Windows-like (owner, 2026-11-08: "this menu
// looks horrible!"): status icons next to the clock, background apps in the
// "^" popup, no shortcuts that belong into Settings.
// Used by the global theme's layout (new users) and by lab/plasma-eval.sh
// (live, on a running session). Call: rennerSystray(trayApplet)
function rennerSystray(tray) {
    // Plasma 6 keeps the tray's settings in the tray applet itself (older
    // versions used a separate containment, SystrayContainmentId).
    var id = tray.readConfig("SystrayContainmentId");
    var sys = id ? desktopById(id) : null;
    if (!sys) {
        sys = tray;
    }
    sys.currentConfigGroup = ["General"];
    // Plasma's own tray entries we offer at all (everything else is off:
    // weather, display setup, printers, caps lock, input methods, ...).
    sys.writeConfig("extraItems", [
        "org.kde.plasma.notifications",
        "org.kde.plasma.networkmanagement",
        "org.kde.plasma.volume",
        "org.kde.plasma.battery",
        "org.kde.plasma.bluetooth",
        "org.kde.plasma.devicenotifier",
        "org.kde.plasma.mediacontroller",
        "org.kde.plasma.keyboardlayout",
        "org.kde.plasma.cameraindicator",
        "org.kde.plasma.clipboard"
    ]);
    sys.writeConfig("knownItems", [
        "org.kde.plasma.notifications", "org.kde.plasma.networkmanagement", "org.kde.plasma.volume",
        "org.kde.plasma.battery", "org.kde.plasma.brightness", "org.kde.plasma.bluetooth",
        "org.kde.plasma.devicenotifier", "org.kde.plasma.mediacontroller", "org.kde.plasma.keyboardlayout",
        "org.kde.plasma.cameraindicator", "org.kde.plasma.clipboard", "org.kde.plasma.weather",
        "org.kde.plasma.printmanager", "org.kde.kscreen", "org.kde.plasma.keyboardindicator",
        "org.kde.plasma.manage-inputmethod", "org.kde.plasma.vault", "org.kde.kdeconnect",
        "org.kde.plasma.dwellclicker"
    ]);
    // Always next to the clock (battery and brightness show themselves only
    // where there is a battery or a dimmable screen).
    sys.writeConfig("shownItems", [
        "org.kde.plasma.networkmanagement", "org.kde.plasma.volume", "org.kde.plasma.battery",
        "org.kde.plasma.bluetooth", "org.kde.plasma.notifications"
    ]);
    // In the "^" popup, not in the panel.
    sys.writeConfig("hiddenItems", ["org.kde.plasma.clipboard"]);
    sys.writeConfig("showAllItems", "false");
    sys.writeConfig("scaleIconsToFit", "false");
    sys.writeConfig("iconSpacing", "2");
    // A compact popup instead of the huge default (top-level applet keys).
    tray.currentConfigGroup = [];
    tray.writeConfig("popupWidth", "360");
    tray.writeConfig("popupHeight", "380");
}
