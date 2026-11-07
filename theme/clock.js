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
