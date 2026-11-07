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
panel.height = 2 * Math.floor(gridUnit * 2.5 / 2);
panel.floating = false;
panel.lengthMode = "fill";

var start = panel.addWidget("org.kde.plasma.kickoff");
start.currentConfigGroup = ["General"];
start.writeConfig("icon", "renner-start");
// Pinned in the start menu (ported into KDE's activity store on first use).
start.writeConfig("favorites", "applications:com.google.Chrome.desktop,applications:org.kde.dolphin.desktop,applications:steam.desktop,applications:renner-launcher.desktop,applications:kwil.desktop,applications:com.obsproject.Studio.desktop,applications:systemsettings.desktop,applications:org.kde.discover.desktop");
start.writeConfig("favoritesPortedToKAstats", "false");
start.writeConfig("favoritesDisplay", "0");     // grid
start.writeConfig("applicationsDisplay", "0");  // grid

var tasks = panel.addWidget("org.kde.plasma.icontasks");
tasks.currentConfigGroup = ["General"];
tasks.writeConfig("launchers", [
    "applications:org.kde.dolphin.desktop",
    "applications:com.google.Chrome.desktop",
    "applications:steam.desktop",
    "applications:renner-launcher.desktop"
]);
tasks.writeConfig("groupingStrategy", "1");

panel.addWidget("org.kde.plasma.marginsseparator");
panel.addWidget("org.kde.plasma.systemtray");
var clock = panel.addWidget("org.kde.plasma.digitalclock");
clock.currentConfigGroup = ["Appearance"];
clock.writeConfig("showDate", "true");
clock.writeConfig("dateFormat", "shortDate");
panel.addWidget("org.kde.plasma.showdesktop");
