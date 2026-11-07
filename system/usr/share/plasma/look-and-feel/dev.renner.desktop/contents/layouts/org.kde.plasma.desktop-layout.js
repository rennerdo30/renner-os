// renner.dev OS default layout: the wallpaper and one bottom taskbar.
// Start | pinned and running apps | tray | clock | show desktop.
var desktopsArray = desktopsForActivity(currentActivity());
for (var j = 0; j < desktopsArray.length; j++) {
    var d = desktopsArray[j];
    d.wallpaperPlugin = "org.kde.image";
    d.currentConfigGroup = ["Wallpaper", "org.kde.image", "General"];
    d.writeConfig("Image", "file:///usr/share/wallpapers/renner/contents/images/3840x2160.png");
}

var panel = new Panel;
panel.location = "bottom";
panel.height = 48;   // Windows 11 taskbar height; icons get air (style margins)
panel.floating = false;
panel.lengthMode = "fill";

// renner.dev Start: search first, pinned and all apps below (its pinned list
// is in its own configuration, see dev.renner.start/contents/config/main.xml).
panel.addWidget("dev.renner.start");

var tasks = panel.addWidget("org.kde.plasma.icontasks");
tasks.currentConfigGroup = ["General"];
tasks.writeConfig("launchers", [
    "applications:org.kde.dolphin.desktop",
    "applications:com.google.Chrome.desktop",
    "applications:steam.desktop",
    "applications:renner-launcher.desktop"
]);
tasks.writeConfig("groupingStrategy", "1");
tasks.writeConfig("iconSpacing", "0");   // tight, like Windows (owner: "horizontal spacing way too big")

panel.addWidget("org.kde.plasma.marginsseparator");
var tray = panel.addWidget("org.kde.plasma.systemtray");
rennerSystray(tray);
var clock = panel.addWidget("org.kde.plasma.digitalclock");
rennerClock(clock);
panel.addWidget("dev.renner.showdesktop");

// --- generated from theme/*.js by theme/layout_sync.py ---
// renner.dev OS taskbar clock, Windows 11 style (owner, 2026-11-08: "the clock
// in the bottom right also horrible"): two small lines, time above date, in
// the regular weight, date in the region's short format.
// Used by the global theme's layout and by lab/plasma-eval.sh.
// Call: rennerClock(clockApplet)
function rennerClock(clock) {
    clock.currentConfigGroup = ["Appearance"];
    clock.writeConfig("autoFontAndSize", "false");
    clock.writeConfig("fontFamily", "Inter");
    clock.writeConfig("fontSize", "9");
    clock.writeConfig("fontWeight", "400");
    clock.writeConfig("boldText", "false");
    clock.writeConfig("showDate", "true");
    clock.writeConfig("dateDisplayFormat", "2");   // 2 = below the time
    clock.writeConfig("dateFormat", "shortDate");
    clock.writeConfig("showSeconds", "0");
}

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
