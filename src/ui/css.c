#include <gtk/gtk.h>
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "css.h"

// ---------------------------------------------------------------------------
// Skins for the main window.
//
// The whole look of the main window (VFO rows, toolbar buttons, bottom bar) and
// the Configure dialog is driven by a small palette of named colors declared as
// GTK @define-color at the top of the style sheet.  The rule body below never
// mentions a concrete color -- it only references those names -- so a "skin" is
// simply a different palette prepended to the same body.
//
// Two text roles are kept apart on purpose so that light skins work too:
//   OFF_WHITE  - normal body/label text.  Light on dark skins, dark on light.
//   ON_ACCENT  - text that sits on top of an accent-colored fill (checked
//                buttons, warnings).  Always kept light, whatever the skin.
// ---------------------------------------------------------------------------

typedef struct {
  const char *name;
  const char *palette;
  gboolean dark;      // ask GTK for the dark variant of stock widgets?
} THEME;

// ---- Palettes ----

static const char palette_charcoal[]=
"  @define-color BACKGROUND rgb(9%,9%,10%);\n"
"  @define-color SURFACE rgb(16%,16%,19%);\n"
"  @define-color SURFACE_HOVER rgb(24%,24%,28%);\n"
"  @define-color BORDER rgb(28%,28%,33%);\n"
"  @define-color OFF_WHITE rgb(89%,89%,91%);\n"
"  @define-color DARK_TEXT rgb(66%,66%,72%);\n"
"  @define-color ON_ACCENT rgb(89%,89%,91%);\n"
"  @define-color ACCENT_A rgb(50%,80%,84%);\n"
"  @define-color ACCENT_B rgb(93%,62%,50%);\n"
"  @define-color ACCENT_ON rgb(58%,46%,72%);\n"
"  @define-color WARNING rgb(85%,27%,27%);\n"
"  @define-color INFO_ON rgb(15%,58%,60%);\n"
"  @define-color SPECTRUM_BG rgb(9%,9%,10%);\n";

static const char palette_solarized_dark[]=
"  @define-color BACKGROUND #002b36;\n"
"  @define-color SURFACE #073642;\n"
"  @define-color SURFACE_HOVER #0d4a5a;\n"
"  @define-color BORDER #35545c;\n"
"  @define-color OFF_WHITE #eee8d5;\n"
"  @define-color DARK_TEXT #93a1a1;\n"
"  @define-color ON_ACCENT #fdf6e3;\n"
"  @define-color ACCENT_A #2aa198;\n"
"  @define-color ACCENT_B #cb4b16;\n"
"  @define-color ACCENT_ON #6c71c4;\n"
"  @define-color WARNING #dc322f;\n"
"  @define-color INFO_ON #268bd2;\n"
"  @define-color SPECTRUM_BG #002b36;\n";

static const char palette_solarized_light[]=
"  @define-color BACKGROUND #fdf6e3;\n"
"  @define-color SURFACE #eee8d5;\n"
"  @define-color SURFACE_HOVER #e0d9c3;\n"
"  @define-color BORDER #c9c2ad;\n"
"  @define-color OFF_WHITE #073642;\n"
"  @define-color DARK_TEXT #657b83;\n"
"  @define-color ON_ACCENT #fdf6e3;\n"
"  @define-color ACCENT_A #2aa198;\n"
"  @define-color ACCENT_B #cb4b16;\n"
"  @define-color ACCENT_ON #6c71c4;\n"
"  @define-color WARNING #dc322f;\n"
"  @define-color INFO_ON #268bd2;\n"
"  @define-color SPECTRUM_BG #002b36;\n";

static const char palette_nord[]=
"  @define-color BACKGROUND #2e3440;\n"
"  @define-color SURFACE #3b4252;\n"
"  @define-color SURFACE_HOVER #434c5e;\n"
"  @define-color BORDER #4c566a;\n"
"  @define-color OFF_WHITE #eceff4;\n"
"  @define-color DARK_TEXT #b8c0d0;\n"
"  @define-color ON_ACCENT #eceff4;\n"
"  @define-color ACCENT_A #88c0d0;\n"
"  @define-color ACCENT_B #d08770;\n"
"  @define-color ACCENT_ON #5e81ac;\n"
"  @define-color WARNING #bf616a;\n"
"  @define-color INFO_ON #8fbcbb;\n"
"  @define-color SPECTRUM_BG #242933;\n";

static const char palette_gruvbox_dark[]=
"  @define-color BACKGROUND #282828;\n"
"  @define-color SURFACE #3c3836;\n"
"  @define-color SURFACE_HOVER #504945;\n"
"  @define-color BORDER #665c54;\n"
"  @define-color OFF_WHITE #ebdbb2;\n"
"  @define-color DARK_TEXT #a89984;\n"
"  @define-color ON_ACCENT #fbf1c7;\n"
"  @define-color ACCENT_A #8ec07c;\n"
"  @define-color ACCENT_B #fe8019;\n"
"  @define-color ACCENT_ON #b16286;\n"
"  @define-color WARNING #cc241d;\n"
"  @define-color INFO_ON #458588;\n"
"  @define-color SPECTRUM_BG #1d2021;\n";

// Dracula (dracula-theme.com): cyan VFO-A, orange VFO-B, purple "on" state.
static const char palette_dracula[]=
"  @define-color BACKGROUND #282a36;\n"
"  @define-color SURFACE #343746;\n"
"  @define-color SURFACE_HOVER #44475a;\n"
"  @define-color BORDER #4d5066;\n"
"  @define-color OFF_WHITE #f8f8f2;\n"
"  @define-color DARK_TEXT #6272a4;\n"
"  @define-color ON_ACCENT #f8f8f2;\n"
"  @define-color ACCENT_A #8be9fd;\n"
"  @define-color ACCENT_B #ffb86c;\n"
"  @define-color ACCENT_ON #bd93f9;\n"
"  @define-color WARNING #ff5555;\n"
"  @define-color INFO_ON #6272a4;\n"
"  @define-color SPECTRUM_BG #21222c;\n";

// Tokyo Night (enkia): deep indigo base, cyan/orange accents, blue "on".
static const char palette_tokyo_night[]=
"  @define-color BACKGROUND #1a1b26;\n"
"  @define-color SURFACE #24283b;\n"
"  @define-color SURFACE_HOVER #292e42;\n"
"  @define-color BORDER #3b4261;\n"
"  @define-color OFF_WHITE #c0caf5;\n"
"  @define-color DARK_TEXT #565f89;\n"
"  @define-color ON_ACCENT #c0caf5;\n"
"  @define-color ACCENT_A #7dcfff;\n"
"  @define-color ACCENT_B #ff9e64;\n"
"  @define-color ACCENT_ON #7aa2f7;\n"
"  @define-color WARNING #f7768e;\n"
"  @define-color INFO_ON #bb9af7;\n"
"  @define-color SPECTRUM_BG #16161e;\n";

// Catppuccin Mocha (catppuccin.com): soft pastel accents, so ON_ACCENT is dark
// (accent fills are light) — a gentle, low-glare dark skin.
static const char palette_catppuccin_mocha[]=
"  @define-color BACKGROUND #1e1e2e;\n"
"  @define-color SURFACE #313244;\n"
"  @define-color SURFACE_HOVER #45475a;\n"
"  @define-color BORDER #585b70;\n"
"  @define-color OFF_WHITE #cdd6f4;\n"
"  @define-color DARK_TEXT #a6adc8;\n"
"  @define-color ON_ACCENT #1e1e2e;\n"
"  @define-color ACCENT_A #89dceb;\n"
"  @define-color ACCENT_B #fab387;\n"
"  @define-color ACCENT_ON #cba6f7;\n"
"  @define-color WARNING #f38ba8;\n"
"  @define-color INFO_ON #89b4fa;\n"
"  @define-color SPECTRUM_BG #11111b;\n";

// Rosé Pine (rosepinetheme.com): muted, "cosy" low-contrast dark; pastel accents
// so ON_ACCENT is dark like Catppuccin.
static const char palette_rose_pine[]=
"  @define-color BACKGROUND #191724;\n"
"  @define-color SURFACE #1f1d2e;\n"
"  @define-color SURFACE_HOVER #26233a;\n"
"  @define-color BORDER #403d52;\n"
"  @define-color OFF_WHITE #e0def4;\n"
"  @define-color DARK_TEXT #908caa;\n"
"  @define-color ON_ACCENT #191724;\n"
"  @define-color ACCENT_A #9ccfd8;\n"
"  @define-color ACCENT_B #f6c177;\n"
"  @define-color ACCENT_ON #c4a7e7;\n"
"  @define-color WARNING #eb6f92;\n"
"  @define-color INFO_ON #3e8fb0;\n"
"  @define-color SPECTRUM_BG #12101c;\n";

// One Dark (Atom): the classic slate-grey editor skin; medium accents keep a
// light ON_ACCENT.
static const char palette_one_dark[]=
"  @define-color BACKGROUND #282c34;\n"
"  @define-color SURFACE #31353f;\n"
"  @define-color SURFACE_HOVER #3b4048;\n"
"  @define-color BORDER #4b5263;\n"
"  @define-color OFF_WHITE #abb2bf;\n"
"  @define-color DARK_TEXT #5c6370;\n"
"  @define-color ON_ACCENT #e6e9ef;\n"
"  @define-color ACCENT_A #56b6c2;\n"
"  @define-color ACCENT_B #d19a66;\n"
"  @define-color ACCENT_ON #c678dd;\n"
"  @define-color WARNING #e06c75;\n"
"  @define-color INFO_ON #61afef;\n"
"  @define-color SPECTRUM_BG #21252b;\n";

// Gruvbox Light: a warm-paper light skin (companion to Gruvbox Dark). Accents are
// dark so text on them stays light; the spectrum stays dark for signal contrast.
static const char palette_gruvbox_light[]=
"  @define-color BACKGROUND #fbf1c7;\n"
"  @define-color SURFACE #ebdbb2;\n"
"  @define-color SURFACE_HOVER #d5c4a1;\n"
"  @define-color BORDER #bdae93;\n"
"  @define-color OFF_WHITE #3c3836;\n"
"  @define-color DARK_TEXT #665c54;\n"
"  @define-color ON_ACCENT #fbf1c7;\n"
"  @define-color ACCENT_A #427b58;\n"
"  @define-color ACCENT_B #af3a03;\n"
"  @define-color ACCENT_ON #8f3f71;\n"
"  @define-color WARNING #9d0006;\n"
"  @define-color INFO_ON #076678;\n"
"  @define-color SPECTRUM_BG #1d2021;\n";

static const THEME themes[]={
  { "Charcoal",         palette_charcoal,          TRUE  },
  { "Solarized Dark",   palette_solarized_dark,    TRUE  },
  { "Solarized Light",  palette_solarized_light,   FALSE },
  { "Nord",             palette_nord,              TRUE  },
  { "Gruvbox Dark",     palette_gruvbox_dark,      TRUE  },
  { "Dracula",          palette_dracula,           TRUE  },
  { "Tokyo Night",      palette_tokyo_night,       TRUE  },
  { "Catppuccin Mocha", palette_catppuccin_mocha,  TRUE  },
  { "Rosé Pine",        palette_rose_pine,         TRUE  },
  { "One Dark",         palette_one_dark,          TRUE  },
  { "Gruvbox Light",    palette_gruvbox_light,     FALSE },
};
static const int n_themes=(int)(sizeof(themes)/sizeof(themes[0]));

// ---- Fonts ----
//
// One family name each, NOT a fallback list: GTK CSS would accept a list, but
// pango_font_description_set_family() — which the panadapter and the meters use
// — takes the whole string as a single family name, so "Noto Sans, Segoe UI"
// resolves to nothing at all.  Observed as `couldn't load font "Noto Sans,
// Segoe UI, sans-serif"`.  Hence a setting with a per-platform default.
#if defined(_WIN32)
  #define UI_FONT_DEFAULT   "Segoe UI"
  #define MONO_FONT_DEFAULT "Consolas"
#elif defined(__APPLE__)
  #define UI_FONT_DEFAULT   "Noto Sans"
  #define MONO_FONT_DEFAULT "Noto Mono"
#else
  #define UI_FONT_DEFAULT   "Noto Sans"
  #define MONO_FONT_DEFAULT "Noto Mono"
#endif

static char ui_font[64]   = UI_FONT_DEFAULT;
static char mono_font[64] = MONO_FONT_DEFAULT;

const char *css_ui_font(void)   { return ui_font; }
const char *css_mono_font(void) { return mono_font; }

// Set both families and re-apply the stylesheet.  Empty or NULL restores the
// platform default rather than leaving an unusable name in place: a font picker
// that can leave the UI unreadable with no way back is worse than none.
void css_set_fonts(const char *sans,const char *mono) {
  g_strlcpy(ui_font,   (sans && *sans) ? sans : UI_FONT_DEFAULT,   sizeof(ui_font));
  g_strlcpy(mono_font, (mono && *mono) ? mono : MONO_FONT_DEFAULT, sizeof(mono_font));
  css_set_theme(css_get_theme());   // re-substitutes and reloads
}

const char *css_ui_font_default(void)   { return UI_FONT_DEFAULT; }
const char *css_mono_font_default(void) { return MONO_FONT_DEFAULT; }

// Replace every occurrence; g_strsplit/g_strjoinv rather than a hand-rolled
// walk, since the token count is tiny and correctness matters more than speed.
static char *str_replace_all(const char *in,const char *from,const char *to) {
  char **parts=g_strsplit(in,from,-1);
  char *out=g_strjoinv(to,parts);
  g_strfreev(parts);
  return out;
}

// ---- Rule body (palette-independent) ----

static const char css_body[]=
"  #receiver-window {\n"
"    background-image: none;\n"
"    background-color: @BACKGROUND;\n"
"    color: @OFF_WHITE;\n"
"    }\n"
"  #vfo-a-text {\n"
"    font-family: @UIFONT@;\n"
"    font-size: 12px;\n"
"    font-weight: bold;\n"
"    font-family: @UIFONT@;\n"
"    color: @ACCENT_A;\n"
"    }\n"
"  #frequency-a-text {\n"
"    font-family: @MONOFONT@;\n"
"    font-size: 32px;\n"
"    color: @ACCENT_A;\n"
"    }\n"
"  #vfo-b-text {\n"
"    font-family: @UIFONT@;\n"
"    font-size: 12px;\n"
"    font-weight: bold;\n"
"    color: @ACCENT_B;\n"
"    }\n"
"  #frequency-b-text {\n"
"    font-family: @MONOFONT@;\n"
"    font-size: 21px;\n"
"    color: @ACCENT_B;\n"
"    }\n"
"  #toolbar-button {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 4px;\n"
"    padding-right: 10px;\n"
"    padding-bottom: 4px;\n"
"    padding-left: 10px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 22px;\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @DARK_TEXT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #toolbar-button:hover {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 4px;\n"
"    padding-right: 10px;\n"
"    padding-bottom: 4px;\n"
"    padding-left: 10px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 22px;\n"
"    background-image: none;\n"
"    background-color: @SURFACE_HOVER;\n"
"    color: @DARK_TEXT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #toolbar-button:disabled {\n"
"    background-image: none;\n"
"    background-color: @BACKGROUND;\n"
"    color: alpha(@DARK_TEXT,0.55);\n"
"    opacity: 0.55;\n"
"    box-shadow: none;\n"
"    }\n"
// Bottom-bar decoder selector (Off/FT8/FT4/SSTV): a flat combo matching the
// toolbar buttons. GtkComboBox has inner button/arrow/cellview nodes that
// otherwise keep the stock theme's beveled look, so each is flattened here.
"  #decode-combo {\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    border: 1px solid @BORDER;\n"
"    border-radius: 5px;\n"
"    box-shadow: none;\n"
"    min-height: 22px;\n"
"    }\n"
"  #decode-combo:hover { background-color: @SURFACE_HOVER; }\n"
"  #decode-combo button {\n"
"    background-image: none;\n"
"    background-color: transparent;\n"
"    border: none;\n"
"    box-shadow: none;\n"
"    text-shadow: none;\n"
"    padding: 2px 6px;\n"
"    min-height: 20px;\n"
"    color: @DARK_TEXT;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    }\n"
"  #decode-combo button:hover { background-color: transparent; }\n"
"  #decode-combo cellview { color: @DARK_TEXT; background-color: transparent; }\n"
"  #decode-combo arrow {\n"
"    color: @DARK_TEXT;\n"
"    -gtk-icon-shadow: none;\n"
"    min-height: 12px;\n"
"    min-width: 12px;\n"
"    }\n"
"  #vfo-button {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 3px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 3px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 16px;\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @DARK_TEXT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #vfo-button:hover {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 3px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 3px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 16px;\n"
"    background-image: none;\n"
"    background-color: @SURFACE_HOVER;\n"
"    color: @DARK_TEXT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #vfo-toggle {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 3px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 3px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 16px;\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @DARK_TEXT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #vfo-toggle:hover {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 3px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 3px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 16px;\n"
"    background-image: none;\n"
"    background-color: @SURFACE_HOVER;\n"
"    color: @DARK_TEXT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #vfo-mode-filter-button {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 3px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 3px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 16px;\n"
"    background-image: none;\n"
"    background-color: @ACCENT_ON;\n"
"    color: @ON_ACCENT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #vfo-toggle:checked {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 3px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 3px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 16px;\n"
"    background-image: none;\n"
"    background-color: @ACCENT_ON;\n"
"    color: @ON_ACCENT;\n"
"    box-shadow: none;\n"
"    }\n"
"  .label {\n"
"    border-width: 0px;\n"
"    padding-top: 0px;\n"
"    padding-right: 0px;\n"
"    padding-bottom: 0px;\n"
"    padding-left: 0px;\n"
"    font-size: small;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    }\n"
"  #warning-label {\n"
"    font-family: @UIFONT@;\n"
"    font-size: 12px;\n"
"    font-weight: bold;\n"
"    color: @WARNING;\n"
"    }\n"
"  #vfo-value-group {\n"
"    border: 1px solid @BORDER;\n"
"    border-radius: 6px;\n"
"    background-color: @BACKGROUND;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    }\n"
"  #vfo-toggle-seg {\n"
"    border-radius: 5px 0px 0px 5px;\n"
"    border-style: none;\n"
"    padding-top: 2px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 2px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin: 0px;\n"
"    min-height: 16px;\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @DARK_TEXT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #vfo-toggle-seg:hover {\n"
"    background-color: @SURFACE_HOVER;\n"
"    }\n"
"  #vfo-toggle-seg:checked {\n"
"    background-color: @ACCENT_ON;\n"
"    color: @ON_ACCENT;\n"
"    }\n"
"  #rit-value {\n"
"    color: @OFF_WHITE;\n"
"    padding-left: 6px;\n"
"    padding-right: 8px;\n"
"    }\n"
"  #xit-value {\n"
"    color: @OFF_WHITE;\n"
"    padding-left: 6px;\n"
"    padding-right: 8px;\n"
"    }\n"
"  #afgain-text {\n"
"    font-family: @UIFONT@;\n"
"    font-size: 12px;\n"
"    font-weight: bold;\n"
"    background-image: none;\n"
"    background-color: @BACKGROUND;\n"
"    color: @OFF_WHITE;\n"
"    }\n"
"  #afgain-scale trough {\n"
"    background-color: @SURFACE;\n"
"    border-color: @BORDER;\n"
"    border-style: solid;\n"
"    border-width: 1px;\n"
"    border-radius: 4px;\n"
"    box-shadow: none;\n"
"    }\n"
"  #afgain-scale trough block.filled {\n"
"    border-color: @ACCENT_A;\n"
"    color: @ACCENT_A;\n"
"    border-radius: 4px;\n"
"    }\n"
"  #squelch-text {\n"
"    font-family: @UIFONT@;\n"
"    font-size: 12px;\n"
"    font-weight: bold;\n"
"    background-image: none;\n"
"    background-color: @BACKGROUND;\n"
"    color: @OFF_WHITE;\n"
"    }\n"
"  #squelch-scale trough {\n"
"    background-color: @SURFACE;\n"
"    border-color: @BORDER;\n"
"    border-style: solid;\n"
"    border-width: 1px;\n"
"    border-radius: 4px;\n"
"    box-shadow: none;\n"
"    }\n"
"  #squelch-scale trough block.filled {\n"
"    border-color: @ACCENT_A;\n"
"    color: @ACCENT_A;\n"
"    border-radius: 4px;\n"
"    }\n"
"  #agcgain-text {\n"
"    font-family: @UIFONT@;\n"
"    font-size: 12px;\n"
"    font-weight: bold;\n"
"    background-image: none;\n"
"    background-color: @BACKGROUND;\n"
"    color: @OFF_WHITE;\n"
"    }\n"
"  #agcgain-scale trough {\n"
"    background-color: @SURFACE;\n"
"    border-color: @BORDER;\n"
"    border-style: solid;\n"
"    border-width: 1px;\n"
"    border-radius: 4px;\n"
"    box-shadow: none;\n"
"    }\n"
"  #agcgain-scale trough block.filled  {\n"
"    background-color: @ACCENT_B;\n"
"    border-color: @OFF_WHITE;\n"
"    border-radius: 4px;\n"
"  }\n"
/* I/Q Player scrub bar: the default GTK trough is near-black, so the
   not-yet-played part of the track vanished against the black area below the
   waterfall. Give the whole trough a visible @SURFACE fill + border; the
   played portion is the @ACCENT_A highlight, the thumb an off-white knob. */
/* The scrub bar floats over the foot of the waterfall, so it carries its own
   dimmed backdrop — otherwise it would be unreadable over a bright trace. */
"  #iq-seek {\n"
"    background-color: alpha(@BACKGROUND,0.60);\n"
"    border-radius: 6px;\n"
"    padding: 2px 8px;\n"
"    margin: 0px 6px 4px 6px;\n"
"    }\n"
"  #iq-seek trough {\n"
"    background-color: @SURFACE;\n"
"    border-color: @BORDER;\n"
"    border-style: solid;\n"
"    border-width: 1px;\n"
"    border-radius: 4px;\n"
"    min-height: 6px;\n"
"    box-shadow: none;\n"
"    }\n"
"  #iq-seek highlight {\n"
"    background-color: @ACCENT_A;\n"
"    border-radius: 4px;\n"
"    }\n"
"  #iq-seek slider {\n"
"    background-color: @OFF_WHITE;\n"
"    border-radius: 50%;\n"
"    }\n"
"  #info-warning {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 3px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 3px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 16px;\n"
"    min-width: 34px;\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @DARK_TEXT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #info-warning:checked {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 3px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 3px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 16px;\n"
"    background-image: none;\n"
"    background-color: @WARNING;\n"
"    color: @ON_ACCENT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #info-button {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 3px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 3px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 16px;\n"
"    min-width: 34px;\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @DARK_TEXT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #info-button:hover {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 3px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 3px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 16px;\n"
"    background-image: none;\n"
"    background-color: @SURFACE_HOVER;\n"
"    color: @OFF_WHITE;\n"
"    box-shadow: none;\n"
"    }\n"
"  #info-button:checked {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 3px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 3px;\n"
"    padding-left: 6px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 16px;\n"
"    background-image: none;\n"
"    background-color: @INFO_ON;\n"
"    color: @ON_ACCENT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #transmit-warning {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 4px;\n"
"    padding-right: 10px;\n"
"    padding-bottom: 4px;\n"
"    padding-left: 10px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 22px;\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @DARK_TEXT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #transmit-warning:checked {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    border-width: 1px;\n"
"    padding-top: 4px;\n"
"    padding-right: 10px;\n"
"    padding-bottom: 4px;\n"
"    padding-left: 10px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    margin-top: 0px;\n"
"    margin-bottom: 0px;\n"
"    min-height: 22px;\n"
"    background-image: none;\n"
"    background-color: @WARNING;\n"
"    color: @ON_ACCENT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #vfo-close {\n"
"    border-radius: 4px;\n"
"    border-style: solid;\n"
"    border-width: 1px;\n"
"    border-color: @BORDER;\n"
"    padding-top: 1px;\n"
"    padding-right: 6px;\n"
"    padding-bottom: 1px;\n"
"    padding-left: 6px;\n"
"    margin-top: 4px;\n"
"    margin-right: 4px;\n"
"    margin-bottom: 4px;\n"
"    margin-left: 4px;\n"
"    min-height: 18px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    background-image: none;\n"
"    background-color: @SURFACE_HOVER;\n"
"    color: @OFF_WHITE;\n"
"    box-shadow: none;\n"
"    }\n"
"  #vfo-close:hover {\n"
"    background-image: none;\n"
"    background-color: @WARNING;\n"
"    color: @ON_ACCENT;\n"
"    border-color: @WARNING;\n"
"    box-shadow: none;\n"
"    }\n"
"  #toolbar-button:checked {\n"
"    border-radius: 5px;\n"
"    border-style: none;\n"
"    padding-top: 4px;\n"
"    padding-right: 10px;\n"
"    padding-bottom: 4px;\n"
"    padding-left: 10px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    min-height: 22px;\n"
"    background-image: none;\n"
"    background-color: @ACCENT_ON;\n"
"    color: @ON_ACCENT;\n"
"    box-shadow: none;\n"
"    }\n"
"  #bottom-bar {\n"
"    background-color: @BACKGROUND;\n"
"    padding-top: 12px;\n"
"    padding-bottom: 14px;\n"
"    padding-left: 16px;\n"
"    padding-right: 16px;\n"
"    }\n"
"  #section-label {\n"
"    font-family: @UIFONT@;\n"
"    font-size: 10px;\n"
"    font-weight: bold;\n"
"    color: @DARK_TEXT;\n"
"    border-left: 2px solid alpha(@ACCENT_A,0.7);\n"
"    padding-left: 6px;\n"
"    margin-left: 2px;\n"
"    }\n"
/* ---- a module's own status line: the same small dim text as a section label
   and deliberately WITHOUT its accent rule. A module title is always present,
   so the rule beside it reads as part of the title; a status line is empty most
   of the time, and borrowing #section-label left a stray teal tick floating
   under the button with nothing attached to it. ---- */
"  #module-status {\n"
"    font-family: @UIFONT@;\n"
"    font-size: 10px;\n"
"    color: @DARK_TEXT;\n"
"    }\n"
/* ---- TX MONITOR bezel: recess the little transmit panadapter into a framed
   display so it reads as an instrument, not a bare strip. The 3px padding ring
   is @SURFACE (same fill the Cairo surface draws), so the rounded @BORDER edge
   and the inset shadow form a thin bezel around the trace. ---- */
"  #tx-monitor-frame {\n"
"    background-color: @SURFACE;\n"
"    border: 1px solid @BORDER;\n"
"    border-radius: 5px;\n"
"    padding: 3px;\n"
"    box-shadow: inset 0 1px 3px alpha(black,0.45);\n"
"    }\n"
/* ---- RDS readout: 3-line typographic hierarchy ---- */
"  #rds-text-0 {\n"
"    font-family: monospace;\n"
"    font-size: 15px;\n"
"    font-weight: bold;\n"
"    color: @ACCENT_A;\n"
"    margin-left: 2px;\n"
"    }\n"
"  #rds-text-1 {\n"
"    font-family: monospace;\n"
"    font-size: 13px;\n"
"    color: @OFF_WHITE;\n"
"    margin-left: 2px;\n"
"    }\n"
"  #rds-text-2 {\n"
"    font-family: monospace;\n"
"    font-size: 13px;\n"
"    font-weight: bold;\n"
"    color: @ACCENT_B;\n"
"    margin-left: 2px;\n"
"    }\n"
"  #ft8-text {\n"
"    font-family: monospace;\n"
"    font-size: 13px;\n"
"    color: @OFF_WHITE;\n"
"    margin-left: 2px;\n"
"    }\n"
"  #bar-rail {\n"
"    background-color: transparent;\n"
"    background-image: linear-gradient(to bottom, alpha(@BORDER,0.0), @BORDER 18%, @BORDER 82%, alpha(@BORDER,0.0));\n"
"    min-width: 1px;\n"
"    margin-top: 2px;\n"
"    margin-bottom: 2px;\n"
"    margin-left: 14px;\n"
"    margin-right: 14px;\n"
"    border-style: none;\n"
"    }\n"
"  #bar-rail-accent {\n"
"    background-color: transparent;\n"
"    background-image: linear-gradient(to bottom, alpha(@ACCENT_A,0.0), alpha(@ACCENT_A,0.55) 15%, alpha(@ACCENT_A,0.55) 85%, alpha(@ACCENT_A,0.0));\n"
"    min-width: 2px;\n"
"    margin-top: 2px;\n"
"    margin-bottom: 2px;\n"
"    margin-left: 14px;\n"
"    margin-right: 14px;\n"
"    border-style: none;\n"
"    }\n"
/* ---- Link status strip (reconnect.c). Named, or it takes the platform
   theme: a pale bubble across the top of a dark console. WARNING because it is
   only ever on screen when the radio is not there. ---- */
"  #link-banner {\n"
"    background-color: alpha(@WARNING,0.18);\n"
"    border-bottom: 1px solid alpha(@WARNING,0.55);\n"
"    padding: 2px 4px;\n"
"    }\n"
"  #link-banner label {\n"
"    color: @WARNING;\n"
"    font-weight: bold;\n"
"    }\n"
"  #rx-bottom-sep {\n"
"    margin-top: 3px;\n"
"    margin-bottom: 3px;\n"
"    }\n"
"  #rx-bottom-sep separator {\n"
"    background-image: none;\n"
"    background-color: alpha(@ACCENT_A,0.30);\n"
"    min-height: 1px;\n"
"    }\n"
"  paned {\n"
"    background-color: @BACKGROUND;\n"
"    }\n"
/* ---- Paned handles: thin to look at, thick enough to actually grab. ----
   These were 2 px AND painted in @BACKGROUND — invisible, and since GTK4 takes
   the drag target straight from the CSS box, essentially un-hittable, so none
   of the splits could be dragged: neither RX against an open decoder panel
   (FT8/SSTV/WEFAX/CW/HFDL/APT alike) nor the panadapter against the waterfall.
   Keep the hairline look by painting the line with a gradient inside a 7 px
   box, so what the pointer has to hit is the whole box, not the line. */
"  paned > separator {\n"
"    background-color: transparent;\n"
"    }\n"
"  paned.vertical > separator {\n"
"    min-height: 7px;\n"
"    background-image: linear-gradient(to bottom,\n"
"        transparent 3px, alpha(@ACCENT_A,0.35) 3px,\n"
"        alpha(@ACCENT_A,0.35) 4px, transparent 4px);\n"
"    }\n"
"  paned.horizontal > separator {\n"
"    min-width: 7px;\n"
"    background-image: linear-gradient(to right,\n"
"        transparent 3px, alpha(@ACCENT_A,0.35) 3px,\n"
"        alpha(@ACCENT_A,0.35) 4px, transparent 4px);\n"
"    }\n"
/* Hover tells you it is draggable before you try — but only the line thickens,
   not the whole box.  Filling the 7 px grab area made the cue look like a slab;
   the target stays 7 px, the cue is 3 px. */
"  paned.vertical > separator:hover {\n"
"    background-image: linear-gradient(to bottom,\n"
"        transparent 2px, alpha(@ACCENT_A,0.55) 2px,\n"
"        alpha(@ACCENT_A,0.55) 5px, transparent 5px);\n"
"    }\n"
"  paned.horizontal > separator:hover {\n"
"    background-image: linear-gradient(to right,\n"
"        transparent 2px, alpha(@ACCENT_A,0.55) 2px,\n"
"        alpha(@ACCENT_A,0.55) 5px, transparent 5px);\n"
"    }\n"
/* ---- Spectrum stack: a hairline inset frame so the (dark) panadapter+waterfall ----
   ---- read as an intentional panel, especially on light skins.               ---- */
"  #rx-spectrum {\n"
"    border: 1px solid @BORDER;\n"
"    background-color: @SPECTRUM_BG;\n"
"    }\n"
/* The spectrum stack keeps its own quieter line (it sits inside a framed panel),
   but it needs the same grab area — this is the panadapter/waterfall split. */
"  #rx-spectrum > separator {\n"
"    background-color: transparent;\n"
"    min-width: 7px;\n"
"    min-height: 7px;\n"
"    background-image: linear-gradient(to bottom,\n"
"        transparent 3px, @BORDER 3px, @BORDER 4px, transparent 4px);\n"
"    }\n"
"  #rx-spectrum > separator:hover {\n"
"    background-image: linear-gradient(to bottom,\n"
"        transparent 2px, alpha(@ACCENT_A,0.55) 2px,\n"
"        alpha(@ACCENT_A,0.55) 5px, transparent 5px);\n"
"    }\n"
/* ---- Decoder panels (FT8/SSTV/WEFAX/CW/HFDL/ACARS/APT) ----
   Every widget in the main window above is named and painted from the palette,
   which is why none of them ever showed this: a panel is built from STOCK GTK
   widgets, and a stock widget paints itself from the PLATFORM theme.  So the
   FT8 decode list came out white on a light platform theme -- a white slab in
   the middle of a Solarized Light window -- and Adwaita's near-black on a dark
   one, in neither case the colour the skin had painted around it.  Scoped to
   the .decode-panel class each panel root carries, so the rest of the window
   keeps whatever the named rules above give it. */
"  .decode-panel {\n"
"    background-color: @BACKGROUND;\n"
"    color: @OFF_WHITE;\n"
"    }\n"
"  .decode-panel label {\n"
"    color: @OFF_WHITE;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 12px;\n"
"    }\n"
/* The reading surfaces -- the FT8 decode list, the CW/HFDL/ACARS text views --
   sit on @SURFACE inside a hairline @BORDER, the same recess #tx-monitor-frame
   gives the TX panadapter, rather than being a hole cut in the skin. */
"  .decode-panel scrolledwindow {\n"
"    border: 1px solid @BORDER;\n"
"    border-radius: 5px;\n"
"    }\n"
"  .decode-panel scrolledwindow,\n"
"  .decode-panel columnview,\n"
"  .decode-panel columnview > listview,\n"
"  .decode-panel listview,\n"
"  .decode-panel textview,\n"
"  .decode-panel textview > text {\n"
"    background-color: @SURFACE;\n"
"    color: @OFF_WHITE;\n"
"    }\n"
/* Rows stay transparent so the FT8 new-DXCC highlight (.ft8-gold/.ft8-blue,
   installed by ft8_panel.c) still shows through. */
"  .decode-panel columnview > listview > row,\n"
"  .decode-panel listview > row {\n"
"    background-color: transparent;\n"
"    }\n"
"  .decode-panel columnview > listview > row:hover,\n"
"  .decode-panel listview > row:hover {\n"
"    background-color: @SURFACE_HOVER;\n"
"    }\n"
"  .decode-panel columnview > header > button {\n"
"    background-image: none;\n"
"    background-color: @SURFACE_HOVER;\n"
"    border-style: none;\n"
"    border-radius: 0px;\n"
"    box-shadow: none;\n"
"    padding: 1px 6px;\n"
"    }\n"
"  .decode-panel columnview > header > button label {\n"
"    color: @DARK_TEXT;\n"
"    font-size: 11px;\n"
"    font-weight: bold;\n"
"    }\n"
"  .decode-panel button {\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @OFF_WHITE;\n"
"    border: 1px solid @BORDER;\n"
"    border-radius: 5px;\n"
"    box-shadow: none;\n"
"    padding: 3px 8px;\n"
"    }\n"
"  .decode-panel button:hover {\n"
"    background-color: @SURFACE_HOVER;\n"
"    }\n"
"  .decode-panel button:checked, .decode-panel button:active {\n"
"    background-color: @ACCENT_ON;\n"
"    color: @ON_ACCENT;\n"
"    border-color: @ACCENT_ON;\n"
"    }\n"
/* The FT8 panel arms and steers its transmitter with GTK's OWN .destructive-
   action / .suggested-action classes, and those live in the theme provider --
   lower priority than this sheet, so the plain `button` rule above would quietly
   take the red off "Enable Tx" while the transmitter is armed and the highlight
   off the active Tx1..Tx6 message. Restate both from the palette. */
"  .decode-panel button.destructive-action {\n"
"    background-color: @WARNING;\n"
"    color: @ON_ACCENT;\n"
"    border-color: @WARNING;\n"
"    }\n"
"  .decode-panel button.suggested-action {\n"
"    background-color: @ACCENT_A;\n"
"    color: @BACKGROUND;\n"
"    border-color: @ACCENT_A;\n"
"    }\n"
"  .decode-panel entry, .decode-panel spinbutton {\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @OFF_WHITE;\n"
"    border: 1px solid @BORDER;\n"
"    border-radius: 5px;\n"
"    box-shadow: none;\n"
"    }\n"
"  .decode-panel entry > text > placeholder {\n"
"    color: @DARK_TEXT;\n"
"    }\n"
"  .decode-panel spinbutton > button {\n"
"    background-color: transparent;\n"
"    border-style: none;\n"
"    border-radius: 0px;\n"
"    }\n"
"  .decode-panel dropdown > button {\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @OFF_WHITE;\n"
"    border: 1px solid @BORDER;\n"
"    border-radius: 5px;\n"
"    box-shadow: none;\n"
"    }\n"
"  .decode-panel check, .decode-panel radio {\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    border-color: @BORDER;\n"
"    }\n"
"  .decode-panel check:checked, .decode-panel radio:checked {\n"
"    background-image: none;\n"
"    background-color: @ACCENT_A;\n"
"    border-color: @ACCENT_A;\n"
"    color: @BACKGROUND;\n"
"    }\n"
/* A GtkPopover is a surface of its own but still a child node of the widget it
   hangs off, so the drop-down lists inside a panel (FT8/FT4, Even/Odd, the SSTV
   and WEFAX mode pickers) are reachable from here -- unlike a tooltip, which
   has to be styled globally further down. */
"  .decode-panel popover > contents {\n"
"    background-color: @SURFACE;\n"
"    color: @OFF_WHITE;\n"
"    border: 1px solid @BORDER;\n"
"    border-radius: 6px;\n"
"    }\n"
"  .decode-panel popover > arrow {\n"
"    background-color: @SURFACE;\n"
"    border: 1px solid @BORDER;\n"
"    }\n"
"  .decode-panel popover row:selected {\n"
"    background-color: @ACCENT_ON;\n"
"    color: @ON_ACCENT;\n"
"    }\n"
/* HFDL and ACARS put their message list and their station/aircraft tables in a
   notebook. */
"  .decode-panel notebook > header {\n"
"    background-color: @BACKGROUND;\n"
"    border-color: @BORDER;\n"
"    }\n"
"  .decode-panel notebook > header tab {\n"
"    color: @DARK_TEXT;\n"
"    }\n"
"  .decode-panel notebook > header tab:checked {\n"
"    color: @OFF_WHITE;\n"
"    box-shadow: inset 0 -3px @ACCENT_A;\n"
"    }\n"
"  .decode-panel notebook > stack {\n"
"    background-color: @BACKGROUND;\n"
"    }\n"
"  .decode-panel scrollbar {\n"
"    background-color: @SURFACE;\n"
"    border-style: none;\n"
"    }\n"
"  .decode-panel scrollbar slider {\n"
"    background-color: @BORDER;\n"
"    }\n"
"  .decode-panel scrollbar slider:hover {\n"
"    background-color: @DARK_TEXT;\n"
"    }\n"
"  .decode-panel separator {\n"
"    background-color: @BORDER;\n"
"    }\n"
/* Same reason as the #config-dialog block below: the colours above apply in
   every state, so without this an insensitive control looks fully active. */
"  .decode-panel button:disabled,\n"
"  .decode-panel entry:disabled,\n"
"  .decode-panel spinbutton:disabled,\n"
"  .decode-panel dropdown:disabled,\n"
"  .decode-panel checkbutton:disabled,\n"
"  .decode-panel label:disabled {\n"
"    opacity: 0.4;\n"
"    }\n"
/* ---- Configuration dialog: flat-dark, scoped under #config-dialog so the ----
   ---- main window (which paints its own named widgets) is never touched. ---- */
"  #config-dialog {\n"
"    background-color: @BACKGROUND;\n"
"    color: @OFF_WHITE;\n"
"    }\n"
"  #config-dialog decoration {\n"
"    background-color: @BACKGROUND;\n"
"    }\n"
"  #config-nav {\n"
"    background-color: @BACKGROUND;\n"
"    border-right: 1px solid @BORDER;\n"
"    min-width: 170px;\n"
"    }\n"
"  #config-search {\n"
"    margin: 8px;\n"
"    min-height: 26px;\n"
"    }\n"
"  #config-search-empty {\n"
"    color: @DARK_TEXT;\n"
"    margin: 8px 12px;\n"
"    }\n"
"  #config-dialog stacksidebar {\n"
"    background-color: @BACKGROUND;\n"
"    }\n"
"  #config-dialog stacksidebar list {\n"
"    background-color: @BACKGROUND;\n"
"    padding: 4px;\n"
"    }\n"
"  #config-dialog stacksidebar row {\n"
"    background-image: none;\n"
"    background-color: transparent;\n"
"    color: @DARK_TEXT;\n"
"    padding: 3px 14px;\n"
"    border-radius: 4px;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 12px;\n"
"    font-weight: bold;\n"
"    }\n"
/* Hairline divider between adjacent tabs (skipped after the last) — a little
   structure between the entries now that they sit closer together. */
"  #config-dialog stacksidebar row:not(:last-child) {\n"
"    border-bottom: 1px solid alpha(@BORDER,0.7);\n"
"    }\n"
"  #config-dialog stacksidebar row:hover {\n"
"    background-color: @SURFACE;\n"
"    color: @OFF_WHITE;\n"
"    }\n"
"  #config-dialog stacksidebar row:selected {\n"
"    background-color: @SURFACE;\n"
"    color: @OFF_WHITE;\n"
"    box-shadow: inset 3px 0 @ACCENT_A;\n"
"    }\n"
"  #config-dialog stacksidebar row label {\n"
"    color: inherit;\n"
"    }\n"
"  #config-stack {\n"
"    background-color: @BACKGROUND;\n"
"    padding: 12px 16px;\n"
"    }\n"
/* GTK4: GtkFrame draws its border on the "frame" node itself — the GTK3
   "border" subnode is gone, so the old `frame > border` rule matched nothing
   and `frame { border-style: none }` silently removed the group outlines. */
"  #config-dialog frame {\n"
"    border-style: solid;\n"
"    border-width: 1px;\n"
"    border-color: @BORDER;\n"
"    border-radius: 6px;\n"
"    }\n"
"  #config-dialog frame > label {\n"
"    color: @ACCENT_A;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 11px;\n"
"    font-weight: bold;\n"
"    margin-bottom: 4px;\n"
"    }\n"
"  #config-dialog label {\n"
"    color: @OFF_WHITE;\n"
"    font-family: @UIFONT@;\n"
"    font-size: 12px;\n"
"    }\n"
"  #config-dialog button {\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @OFF_WHITE;\n"
"    border-style: solid;\n"
"    border-width: 1px;\n"
"    border-color: @BORDER;\n"
"    border-radius: 5px;\n"
"    box-shadow: none;\n"
"    padding: 4px 10px;\n"
"    }\n"
"  #config-dialog button:hover {\n"
"    background-color: @SURFACE_HOVER;\n"
"    }\n"
"  #config-dialog button:checked, #config-dialog button:active {\n"
"    background-color: @ACCENT_ON;\n"
"    color: @ON_ACCENT;\n"
"    border-color: @ACCENT_ON;\n"
"    }\n"
"  #config-dialog entry, #config-dialog spinbutton {\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @OFF_WHITE;\n"
"    border-style: solid;\n"
"    border-width: 1px;\n"
"    border-color: @BORDER;\n"
"    border-radius: 5px;\n"
"    box-shadow: none;\n"
"    }\n"
"  #config-dialog spinbutton button {\n"
"    border-radius: 0px;\n"
"    border-style: none;\n"
"    }\n"
"  #config-dialog combobox button {\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    color: @OFF_WHITE;\n"
"    border-color: @BORDER;\n"
"    }\n"
"  #config-dialog checkbutton, #config-dialog radiobutton {\n"
"    color: @OFF_WHITE;\n"
"    }\n"
"  #config-dialog check, #config-dialog radio {\n"
"    background-image: none;\n"
"    background-color: @SURFACE;\n"
"    border-color: @BORDER;\n"
"    }\n"
"  #config-dialog check:checked, #config-dialog radio:checked {\n"
"    background-image: none;\n"
"    background-color: @ACCENT_A;\n"
"    border-color: @ACCENT_A;\n"
"    color: @BACKGROUND;\n"
"    }\n"
"  #config-dialog scale trough {\n"
"    background-color: @SURFACE;\n"
"    border-color: @BORDER;\n"
"    border-style: solid;\n"
"    border-width: 1px;\n"
"    border-radius: 4px;\n"
"    box-shadow: none;\n"
"    }\n"
"  #config-dialog scale highlight {\n"
"    background-color: @ACCENT_A;\n"
"    border-radius: 4px;\n"
"    }\n"
"  #config-dialog scale slider {\n"
"    background-color: @OFF_WHITE;\n"
"    border-radius: 50%;\n"
"    }\n"
/* The graphic-EQ band sliders carry the .eq-scale class so their knob can be
   shrunk (11 side-by-side vertical sliders) without affecting the ordinary
   horizontal scales elsewhere on the config pages. */
"  #config-dialog scale.eq-scale slider {\n"
"    min-width: 11px;\n"
"    min-height: 11px;\n"
"    margin: -4px;\n"
"    }\n"
"  #config-dialog separator {\n"
"    background-color: @BORDER;\n"
"    }\n"
/* Search hits get a restrained highlighter treatment.  The inset underline is
   still visible on buttons and frame titles whose own background is stronger. */
"  #config-dialog .search-match {\n"
"    background-color: alpha(@ACCENT_A,0.18);\n"
"    box-shadow: inset 0 -2px alpha(@ACCENT_A,0.9);\n"
"    border-radius: 3px;\n"
"    }\n"
/* The explicit colours above apply in every widget state, so they cancel GTK's
   default dimming of insensitive controls — a disabled scale/checkbox/dropdown
   would otherwise look fully active even though it ignores input. Restore a
   visible \"greyed out\" look for the disabled state (e.g. the Diversity page's
   gain/phase controls while diversity is off). */
"  #config-dialog scale:disabled,\n"
"  #config-dialog checkbutton:disabled,\n"
"  #config-dialog dropdown:disabled,\n"
"  #config-dialog combobox:disabled,\n"
"  #config-dialog label:disabled {\n"
"    opacity: 0.4;\n"
"    }\n"
/* Tooltips are a stock GTK widget drawn in a window of their own, so they take
   the platform theme rather than the skin unless they are styled here — a pale
   bubble over a dark console, or the reverse. The node is `tooltip.background`
   with a label inside it; both are named so neither the frame nor the text can
   fall back. */
"  tooltip.background {\n"
"    background-color: @SURFACE;\n"
"    border: 1px solid @BORDER;\n"
"    border-radius: 6px;\n"
"    }\n"
"  tooltip label {\n"
"    font-family: @UIFONT@;\n"
"    font-size: 12px;\n"
"    color: @OFF_WHITE;\n"
"    padding: 2px 4px;\n"
"    }\n"
;

// ---- Runtime ----

static GtkCssProvider *css_provider=NULL;
static int current_theme=0;

// ---- Numeric palette cache -------------------------------------------------
// Cairo-drawn widgets (S-meter, TX monitor, mic/drive bars, PureSignal panel)
// cannot read the CSS, so we parse the active palette's @define-color values
// into numbers once per skin change and hand them out via css_rgb(). This keeps
// a single source of truth: the same palette strings feed both CSS and Cairo.

typedef struct { char name[24]; double r,g,b; } SKIN_RGB;
static SKIN_RGB rgb_cache[16];
static int rgb_n=0;

static void parse_palette(const char *pal) {
  rgb_n=0;
  const char *p=pal;
  while((p=strstr(p,"@define-color"))!=NULL && rgb_n<(int)(sizeof(rgb_cache)/sizeof(rgb_cache[0]))) {
    p+=strlen("@define-color");
    while(*p==' ') p++;
    char name[24]; int i=0;
    while(*p && *p!=' ' && i<23) name[i++]=*p++;
    name[i]='\0';
    while(*p==' ') p++;
    double r=0,g=0,b=0;
    if(*p=='#') {
      unsigned v=0;
      if(sscanf(p+1,"%6x",&v)==1) {
        r=((v>>16)&0xff)/255.0; g=((v>>8)&0xff)/255.0; b=(v&0xff)/255.0;
      }
    } else if(strncmp(p,"rgb",3)==0) {
      const char *q=strchr(p,'(');
      if(q!=NULL) {
        q++;
        double vals[3]={0,0,0};
        for(int k=0;k<3 && *q!='\0' && *q!=')';k++) {
          char *end;
          double val=strtod(q,&end);
          q=end;
          while(*q==' ') q++;
          if(*q=='%') { val/=100.0; q++; } else { val/=255.0; }
          while(*q==',' || *q==' ') q++;
          vals[k]=val;
        }
        r=vals[0]; g=vals[1]; b=vals[2];
      }
    }
    g_strlcpy(rgb_cache[rgb_n].name,name,sizeof(rgb_cache[rgb_n].name));
    rgb_cache[rgb_n].r=r; rgb_cache[rgb_n].g=g; rgb_cache[rgb_n].b=b;
    rgb_n++;
  }
}

// Fill *r,*g,*b (0..1) with the active skin's color for a palette name
// (e.g. "BACKGROUND","ACCENT_A"). Leaves them untouched and returns FALSE if the
// name is unknown, so callers can pass in a fallback color first.
gboolean css_rgb(const char *name, double *r, double *g, double *b) {
  if(rgb_n==0) parse_palette(themes[current_theme].palette);  // before load_css()
  for(int i=0;i<rgb_n;i++) {
    if(strcmp(rgb_cache[i].name,name)==0) {
      *r=rgb_cache[i].r; *g=rgb_cache[i].g; *b=rgb_cache[i].b;
      return TRUE;
    }
  }
  return FALSE;
}

int css_theme_count(void) { return n_themes; }

const char *css_theme_name(int idx) {
  if(idx<0 || idx>=n_themes) return "";
  return themes[idx].name;
}

int css_get_theme(void) { return current_theme; }

// Rebuild the full style sheet (selected palette + shared body) and push it into
// the one application-wide provider. Safe to call at any time; the main window
// and any open dialog restyle live.
void css_set_theme(int idx) {
  if(idx<0 || idx>=n_themes) idx=0;
  current_theme=idx;
  parse_palette(themes[idx].palette);   // refresh numeric cache for Cairo widgets

  if(css_provider==NULL) return;   // load_css() not run yet

  // Ask GTK for the light/dark variant of stock widgets (combobox popups, menus,
  // scrollbars, tooltips) so they match the skin.
  //
  // Both spellings, because 4.20 replaced the first with the second and the two
  // do not coexist gracefully: measured on 4.22, once gtk-interface-color-scheme
  // has been written at all — to ANY value, UNSUPPORTED included — a later
  // gtk-application-prefer-dark-theme=TRUE has no effect whatever. So a desktop
  // that reports its own colour scheme silently takes the stock widgets away
  // from the skin, and the deprecated flag cannot take them back. Never leave
  // the new setting at DEFAULT/UNSUPPORTED for the same reason: that IS the
  // "follow the desktop" state.
  g_object_set(gtk_settings_get_default(),
               "gtk-application-prefer-dark-theme",themes[idx].dark,NULL);
#if GTK_CHECK_VERSION(4,20,0)
  g_object_set(gtk_settings_get_default(),
               "gtk-interface-color-scheme",
               themes[idx].dark ? GTK_INTERFACE_COLOR_SCHEME_DARK
                                : GTK_INTERFACE_COLOR_SCHEME_LIGHT,NULL);
#endif

  // The two font families are substituted rather than written into css_body:
  // they are an operator setting (see css_set_fonts) because no single family
  // exists everywhere — Noto ships with most Linux desktops and is installed on
  // this developer's Mac, but a stock Windows has neither Noto Sans nor Noto
  // Mono and falls back to whatever fontconfig can find last.
  char *body=g_strdup(css_body);
  { char *t;
    t=str_replace_all(body,"@UIFONT@",ui_font);     g_free(body); body=t;
    t=str_replace_all(body,"@MONOFONT@",mono_font); g_free(body); body=t; }
  char *full=g_strconcat(themes[idx].palette,body,NULL);
  g_free(body);
  // GTK4: load_from_data(...,-1,NULL) → load_from_string (null-terminated).
  gtk_css_provider_load_from_string(css_provider,full);
  g_free(full);
}

void load_css(void) {
  GdkDisplay *display;

  log_info("%s\n",__FUNCTION__);

  css_provider = gtk_css_provider_new ();
  display = gdk_display_get_default ();
  // GTK4: providers attach to the display, not a GdkScreen (removed).
  gtk_style_context_add_provider_for_display (display,
                                             GTK_STYLE_PROVIDER(css_provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  css_set_theme(current_theme);
}
