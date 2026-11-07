//! Language, keyboard and time zone choices for the first page.

/// A system language: what the user picks, the locale it installs, and the
/// keyboard and time zone it suggests.
pub struct Language {
    pub label: &'static str,
    pub locale: &'static str,
    pub keyboard: &'static str,
    pub timezone: &'static str,
}

pub const LANGUAGES: &[Language] = &[
    Language { label: "English (United States)", locale: "en_US.UTF-8", keyboard: "us", timezone: "America/New_York" },
    Language { label: "English (United Kingdom)", locale: "en_GB.UTF-8", keyboard: "gb", timezone: "Europe/London" },
    Language { label: "Deutsch (Deutschland)", locale: "de_DE.UTF-8", keyboard: "de", timezone: "Europe/Berlin" },
    Language { label: "Deutsch (Österreich)", locale: "de_AT.UTF-8", keyboard: "de", timezone: "Europe/Vienna" },
    Language { label: "Deutsch (Schweiz)", locale: "de_CH.UTF-8", keyboard: "ch", timezone: "Europe/Zurich" },
    Language { label: "Français", locale: "fr_FR.UTF-8", keyboard: "fr", timezone: "Europe/Paris" },
    Language { label: "Español", locale: "es_ES.UTF-8", keyboard: "es", timezone: "Europe/Madrid" },
    Language { label: "Italiano", locale: "it_IT.UTF-8", keyboard: "it", timezone: "Europe/Rome" },
    Language { label: "Nederlands", locale: "nl_NL.UTF-8", keyboard: "us", timezone: "Europe/Amsterdam" },
    Language { label: "Português (Brasil)", locale: "pt_BR.UTF-8", keyboard: "br", timezone: "America/Sao_Paulo" },
    Language { label: "Português (Portugal)", locale: "pt_PT.UTF-8", keyboard: "pt", timezone: "Europe/Lisbon" },
    Language { label: "Polski", locale: "pl_PL.UTF-8", keyboard: "pl", timezone: "Europe/Warsaw" },
    Language { label: "Čeština", locale: "cs_CZ.UTF-8", keyboard: "cz", timezone: "Europe/Prague" },
    Language { label: "Svenska", locale: "sv_SE.UTF-8", keyboard: "se", timezone: "Europe/Stockholm" },
    Language { label: "Norsk bokmål", locale: "nb_NO.UTF-8", keyboard: "no", timezone: "Europe/Oslo" },
    Language { label: "Dansk", locale: "da_DK.UTF-8", keyboard: "dk", timezone: "Europe/Copenhagen" },
    Language { label: "Suomi", locale: "fi_FI.UTF-8", keyboard: "fi", timezone: "Europe/Helsinki" },
    Language { label: "Türkçe", locale: "tr_TR.UTF-8", keyboard: "tr", timezone: "Europe/Istanbul" },
    Language { label: "Русский", locale: "ru_RU.UTF-8", keyboard: "ru", timezone: "Europe/Moscow" },
    Language { label: "日本語", locale: "ja_JP.UTF-8", keyboard: "jp", timezone: "Asia/Tokyo" },
    Language { label: "한국어", locale: "ko_KR.UTF-8", keyboard: "kr", timezone: "Asia/Seoul" },
    Language { label: "中文 (简体)", locale: "zh_CN.UTF-8", keyboard: "us", timezone: "Asia/Shanghai" },
];

/// A keyboard layout: the desktop's XKB layout and the text console's keymap.
pub struct Keyboard {
    pub label: &'static str,
    pub xkb: &'static str,
    pub console: &'static str,
}

pub const KEYBOARDS: &[Keyboard] = &[
    Keyboard { label: "English (US)", xkb: "us", console: "us" },
    Keyboard { label: "English (UK)", xkb: "gb", console: "uk" },
    Keyboard { label: "German", xkb: "de", console: "de-latin1" },
    Keyboard { label: "German (Switzerland)", xkb: "ch", console: "de_CH-latin1" },
    Keyboard { label: "French", xkb: "fr", console: "fr-latin1" },
    Keyboard { label: "Spanish", xkb: "es", console: "es" },
    Keyboard { label: "Italian", xkb: "it", console: "it" },
    Keyboard { label: "Portuguese", xkb: "pt", console: "pt-latin1" },
    Keyboard { label: "Portuguese (Brazil)", xkb: "br", console: "br-abnt2" },
    Keyboard { label: "Polish", xkb: "pl", console: "pl2" },
    Keyboard { label: "Czech", xkb: "cz", console: "cz-qwertz" },
    Keyboard { label: "Swedish", xkb: "se", console: "sv-latin1" },
    Keyboard { label: "Norwegian", xkb: "no", console: "no-latin1" },
    Keyboard { label: "Danish", xkb: "dk", console: "dk-latin1" },
    Keyboard { label: "Finnish", xkb: "fi", console: "fi" },
    Keyboard { label: "Turkish", xkb: "tr", console: "trq" },
    Keyboard { label: "Russian", xkb: "ru", console: "ru" },
    Keyboard { label: "Japanese", xkb: "jp", console: "jp106" },
    Keyboard { label: "Korean", xkb: "kr", console: "us" },
];

pub fn keyboard_index(xkb: &str) -> usize {
    KEYBOARDS.iter().position(|k| k.xkb == xkb).unwrap_or(0)
}

/// Time zones grouped by region ("Europe" -> ["Berlin", ...]), from the
/// system's zone list; a short built-in list if that is missing.
pub fn timezones() -> Vec<(String, Vec<String>)> {
    let text = std::fs::read_to_string("/usr/share/zoneinfo/tzdata.zi").unwrap_or_default();
    let mut zones: Vec<String> = text
        .lines()
        .filter_map(|l| l.strip_prefix("Z ").or_else(|| l.strip_prefix("L ").and_then(|r| r.split_whitespace().nth(1))))
        .map(|l| l.split_whitespace().next().unwrap_or("").to_string())
        .filter(|z| {
            let region = z.split('/').next().unwrap_or("");
            z.contains('/')
                && ["Africa", "America", "Antarctica", "Asia", "Atlantic", "Australia", "Europe", "Indian", "Pacific"]
                    .contains(&region)
        })
        .collect();
    if zones.is_empty() {
        zones = LANGUAGES.iter().map(|l| l.timezone.to_string()).collect();
    }
    zones.sort();
    zones.dedup();
    let mut groups: Vec<(String, Vec<String>)> = Vec::new();
    for zone in zones {
        let (region, city) = zone.split_once('/').expect("filtered above");
        match groups.last_mut() {
            Some((r, cities)) if r == region => cities.push(city.to_string()),
            _ => groups.push((region.to_string(), vec![city.to_string()])),
        }
    }
    groups
}

/// (region index, city index) of a zone such as "Europe/Berlin".
pub fn find_zone(groups: &[(String, Vec<String>)], zone: &str) -> (usize, usize) {
    let (region, city) = zone.split_once('/').unwrap_or(("", ""));
    let r = groups.iter().position(|(name, _)| name == region).unwrap_or(0);
    let c = groups.get(r).and_then(|(_, cities)| cities.iter().position(|x| x == city)).unwrap_or(0);
    (r, c)
}
