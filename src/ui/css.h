extern void load_css(void);

// Skins: swap the main-window/dialog palette at runtime.
extern void css_set_theme(int idx);   // apply skin by index (clamped)
extern int css_get_theme(void);       // index of the active skin
extern int css_theme_count(void);     // number of available skins
extern const char *css_theme_name(int idx);  // display name of skin idx

// Fonts. ONE family name each, not a fallback list: pango_font_description_
// set_family() takes the whole string as a single family, so a list resolves to
// nothing. Defaults are per-platform (Noto on macOS/Linux, Segoe UI/Consolas on
// Windows, which has no Noto). Every drawn label goes through these.
extern const char *css_ui_font(void);
extern const char *css_mono_font(void);
extern const char *css_ui_font_default(void);
extern const char *css_mono_font_default(void);
extern void css_set_fonts(const char *sans, const char *mono);

// Active skin's color for a palette name (e.g. "BACKGROUND","ACCENT_A"), as
// 0..1 RGB, for Cairo-drawn widgets. Returns FALSE (leaving *r/*g/*b as passed)
// if the name is unknown, so a fallback can be pre-loaded.
extern int css_rgb(const char *name, double *r, double *g, double *b);
