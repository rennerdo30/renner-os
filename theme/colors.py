#!/usr/bin/env python3
"""renner.dev OS color scheme, from the renner.dev brand tokens.

Writes the KDE color scheme (usr/share/color-schemes/RennerDark.colors) and the
same color groups into the system-wide kdeglobals defaults
(usr/share/renner-os/xdg/kdeglobals, on XDG_CONFIG_DIRS after /etc/xdg):
KDE applications read their colors from kdeglobals, so a new user gets the
scheme before anything is "applied". Run after changing a token:

    python theme/colors.py
"""
from pathlib import Path

SYSTEM = Path(__file__).resolve().parent.parent / "system"

# Brand tokens (renner.dev website, launcher and installer).
BG = "8,9,15"            # page background #08090f
SURFACE = "15,17,26"     # windows #0f111a
RAISED = "17,19,29"      # headers, panels #11131d
HOVER = "24,27,40"       # buttons #181b28
INPUT = "12,14,22"       # text fields, lists #0c0e16
TEXT = "247,248,251"     # #f7f8fb
MUTED = "155,165,185"    # #9ba5b9
FAINT = "139,149,168"    # #8b95a8
ACCENT = "255,122,26"    # orange #ff7a1a
ACCENT_SOFT = "255,162,76"  # #ffa24c
INK = "28,18,7"          # text on orange #1c1207
CYAN = "45,226,240"      # #2de2f0
FOCUS = "125,211,252"    # #7dd3fc
SUCCESS = "61,220,151"   # #3ddc97
DANGER = "255,92,122"    # #ff5c7a


# System Settings pages hidden by default: theme internals (the renner.dev look
# is the product), window manager details, search indexing, services and other
# power-user pages. Everything a Windows user looks for stays.
HIDDEN_SETTINGS = [
    "kcm_lookandfeel", "kcm_style", "kcm_desktoptheme", "kcm_icons", "kcm_cursortheme",
    "kcm_splashscreen", "kcm_kwindecoration", "kcm_kwin_effects", "kcm_kwin_scripts",
    "kcm_kwinrules", "kcm_kwinxwayland", "kcm_kwintabbox", "kcm_kwinoptions",
    "kcm_kwinscreenedges", "kcm_kwintouchscreen", "kcm_activities", "kcm_plasmasearch",
    "kcm_kded", "kcm_baloofile", "kcm_recentFiles", "kcm_smserver", "kcm_soundtheme",
    "kcm_feedback", "kcm_spellchecking", "kcm_sddm", "kcm_proxy", "kcm_netpref",
    "kcm_kgamma", "kcm_device_automounter", "kcm_solid_actions", "kcm_desktoppaths",
    "kcm_webshortcuts", "kcm_kamera", "kcm_workspace", "kcm_keys", "kcm_updates",
    "kcm_filetypes", "kcm_fonts", "kcm_kwinsupportinfo", "kcm_virtualkeyboard",
    # The "Quick settings" start page offers stock KDE themes and pages hidden
    # above; Settings opens on its first real page instead.
    "kcm_landingpage",
]


def group(background, alt, foreground=TEXT, decoration_hover=ACCENT_SOFT):
    return {
        "BackgroundNormal": background,
        "BackgroundAlternate": alt,
        "ForegroundNormal": foreground,
        "ForegroundInactive": MUTED,
        "ForegroundActive": ACCENT_SOFT,
        "ForegroundLink": CYAN,
        "ForegroundVisited": FOCUS,
        "ForegroundNegative": DANGER,
        "ForegroundNeutral": ACCENT_SOFT,
        "ForegroundPositive": SUCCESS,
        "DecorationFocus": ACCENT,
        "DecorationHover": decoration_hover,
    }


GROUPS = {
    "Colors:Window": group(SURFACE, RAISED),
    "Colors:View": group(INPUT, SURFACE),
    "Colors:Button": group(HOVER, RAISED),
    "Colors:Header": group(RAISED, SURFACE),
    "Colors:Header][Inactive": group(SURFACE, SURFACE),
    "Colors:Tooltip": group(RAISED, SURFACE),
    "Colors:Complementary": group(BG, SURFACE),
    # Selected rows are pale accent pills (our style fork), so selected text
    # and symbolic icons stay light; the style picks dark ink itself on full
    # accent fills such as the default button (2026-11-14: dark icons on the
    # pill with INK here).
    "Colors:Selection": {**group(ACCENT, ACCENT_SOFT, foreground=TEXT),
                         "ForegroundInactive": TEXT, "ForegroundActive": TEXT},
}
WM = {
    "activeBackground": RAISED, "activeForeground": TEXT, "activeBlend": RAISED,
    "inactiveBackground": SURFACE, "inactiveForeground": FAINT, "inactiveBlend": SURFACE,
}
GENERAL = {"ColorScheme": "RennerDark", "Name": "renner.dev Dark", "shadeSortColumn": "true"}
EFFECTS = {
    "ColorEffects:Disabled": {"Color": SURFACE, "ColorAmount": "0", "ColorEffect": "0",
                              "ContrastAmount": "0.65", "ContrastEffect": "1",
                              "IntensityAmount": "0.1", "IntensityEffect": "2"},
    "ColorEffects:Inactive": {"ChangeSelectionColor": "true", "Color": SURFACE,
                              "ColorAmount": "0.025", "ColorEffect": "2", "ContrastAmount": "0.1",
                              "ContrastEffect": "2", "Enable": "false",
                              "IntensityAmount": "0", "IntensityEffect": "0"},
}


def render(sections: dict) -> str:
    out = []
    for name, values in sections.items():
        out.append(f"[{name}]")
        out += [f"{k}={v}" for k, v in values.items()]
        out.append("")
    return "\n".join(out)


def main() -> None:
    scheme = {"General": GENERAL, **EFFECTS, **GROUPS, "WM": WM,
              "KDE": {"contrast": "4"}}
    target = SYSTEM / "usr/share/color-schemes/RennerDark.colors"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(render(scheme), newline="\n")

    # System-wide defaults: colors plus look, fonts and icons.
    kdeglobals = {
        "General": {
            "ColorScheme": "RennerDark",
            "font": "Inter,10,-1,5,400,0,0,0,0,0,0,0,0,0,0,1",
            "menuFont": "Inter,10,-1,5,400,0,0,0,0,0,0,0,0,0,0,1",
            "toolBarFont": "Inter,9,-1,5,400,0,0,0,0,0,0,0,0,0,0,1",
            "smallestReadableFont": "Inter,8,-1,5,400,0,0,0,0,0,0,0,0,0,0,1",
            "fixed": "JetBrains Mono,10,-1,5,400,0,0,0,0,0,0,0,0,0,0,1",
            # No AccentColor: it switches on Plasma's accent mode, which writes
            # its own darkened selection colors (182,89,24, white text) over
            # RennerDark's #ff7a1a with dark ink (2026-11-14).
            "BrowserApplication": "com.google.Chrome.desktop",
        },
        "WM": {**WM, "activeFont": "Inter,10,-1,5,600,0,0,0,0,0,0,0,0,0,0,1"},
        "Icons": {"Theme": "Papirus-Dark"},
        "KDE": {"LookAndFeelPackage": "dev.renner.desktop", "widgetStyle": "Breeze",
                "contrast": "4", "AnimationDurationFactor": "0.7"},
        **EFFECTS, **GROUPS,
        # A Windows-sized System Settings: KDE's expert pages are hidden (Kiosk).
        "KDE Control Module Restrictions": {m: "false" for m in HIDDEN_SETTINGS},
    }
    target = SYSTEM / "usr/share/renner-os/xdg/kdeglobals"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(render(kdeglobals), newline="\n")
    print("wrote RennerDark.colors and usr/share/renner-os/xdg/kdeglobals")


if __name__ == "__main__":
    main()
