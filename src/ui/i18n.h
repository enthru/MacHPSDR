/* Small, dependency-free UI translation layer.
 *
 * MacHPSDR deliberately keeps radio/DSP vocabulary (VFO, AGC, Panadapter,
 * PureSignal, protocol names, units, and mode names) in English.  Only ordinary
 * interface prose belongs in the catalogue in i18n.c.
 */
#ifndef MACHPSDR_I18N_H
#define MACHPSDR_I18N_H

typedef enum {
  I18N_EN = 0,
  I18N_RU,
  I18N_UK,
  I18N_BE,
  I18N_LANGUAGE_COUNT
} I18nLanguage;

/* Which property of a registered widget carries the translated string, so the
 * live-retranslation pass (i18n_set_language) knows which setter to re-apply. */
typedef enum {
  I18N_W_LABEL = 0,          /* gtk_label_set_text */
  I18N_W_MARKUP,             /* gtk_label_set_markup */
  I18N_W_BUTTON,             /* gtk_button_set_label */
  I18N_W_CHECK,              /* gtk_check_button_set_label */
  I18N_W_FRAME,              /* gtk_frame_set_label */
  I18N_W_WINDOW_TITLE,       /* gtk_window_set_title */
  I18N_W_TOOLTIP,            /* gtk_widget_set_tooltip_text */
  I18N_W_MENU_LABEL,         /* gtk_menu_button_set_label */
  I18N_W_SEARCH_PLACEHOLDER, /* gtk_search_entry_set_placeholder_text */
  I18N_W_ENTRY_PLACEHOLDER,  /* gtk_entry_set_placeholder_text */
  I18N_W_STACK_TITLE         /* gtk_stack_page_set_title (a GtkStackPage) */
} I18nWidgetKind;

void i18n_init(const char *config_dir);
I18nLanguage i18n_language(void);
const char *i18n_language_code(void);
const char *i18n_language_name(I18nLanguage language);
void i18n_set_language(I18nLanguage language);
const char *i18n_tr(const char *english);

#ifdef GTK_MAJOR_VERSION
/* Remember a widget carries a translatable string so a later language change can
 * re-apply it in place, instead of forcing a restart.  Registration is silently
 * dropped for a string that is not in the catalogue (dynamic text, empty
 * strings): those manage their own contents and never need retranslating.  The
 * widget is tracked by a weak reference, so it de-registers itself on destroy.
 * All calls are on the GTK main thread. */
void i18n_register_widget(GtkWidget *widget, I18nWidgetKind kind,
                          const char *english);

/* The same, for a translatable string carried by a plain GObject rather than a
 * widget — currently a GtkStackPage title (the Configure sidebar tabs). */
void i18n_register_object(GObject *object, I18nWidgetKind kind,
                          const char *english);

/* The wrappers below translate at creation/set time (as the macros always did)
 * AND register the widget for live retranslation.  Placed before the macro
 * definitions so their own gtk_* calls reach the real functions; the
 * parenthesised name is belt-and-braces against the function-like macros. */
static inline GtkWidget *i18n_label_new(const char *text) {
  GtkWidget *w=(gtk_label_new)(i18n_tr(text));
  i18n_register_widget(w,I18N_W_LABEL,text);
  return w;
}
static inline void i18n_label_set_text(GtkLabel *label, const char *text) {
  (gtk_label_set_text)(label,i18n_tr(text));
  i18n_register_widget(GTK_WIDGET(label),I18N_W_LABEL,text);
}
static inline void i18n_label_set_markup(GtkLabel *label, const char *text) {
  (gtk_label_set_markup)(label,i18n_tr(text));
  i18n_register_widget(GTK_WIDGET(label),I18N_W_MARKUP,text);
}
static inline GtkWidget *i18n_button_new_with_label(const char *text) {
  GtkWidget *w=(gtk_button_new_with_label)(i18n_tr(text));
  i18n_register_widget(w,I18N_W_BUTTON,text);
  return w;
}
static inline void i18n_button_set_label(GtkButton *button, const char *text) {
  (gtk_button_set_label)(button,i18n_tr(text));
  i18n_register_widget(GTK_WIDGET(button),I18N_W_BUTTON,text);
}
static inline GtkWidget *i18n_check_button_new_with_label(const char *text) {
  GtkWidget *w=(gtk_check_button_new_with_label)(i18n_tr(text));
  i18n_register_widget(w,I18N_W_CHECK,text);
  return w;
}
static inline GtkWidget *i18n_frame_new(const char *text) {
  GtkWidget *w=(gtk_frame_new)(text!=NULL?i18n_tr(text):NULL);
  i18n_register_widget(w,I18N_W_FRAME,text);
  return w;
}
static inline void i18n_window_set_title(GtkWindow *window, const char *text) {
  (gtk_window_set_title)(window,i18n_tr(text));
  i18n_register_widget(GTK_WIDGET(window),I18N_W_WINDOW_TITLE,text);
}
static inline void i18n_widget_set_tooltip_text(GtkWidget *widget,
                                                const char *text) {
  (gtk_widget_set_tooltip_text)(widget,i18n_tr(text));
  i18n_register_widget(widget,I18N_W_TOOLTIP,text);
}
static inline void i18n_menu_button_set_label(GtkMenuButton *button,
                                              const char *text) {
  (gtk_menu_button_set_label)(button,i18n_tr(text));
  i18n_register_widget(GTK_WIDGET(button),I18N_W_MENU_LABEL,text);
}
static inline void i18n_search_entry_set_placeholder_text(GtkSearchEntry *entry,
                                                          const char *text) {
  (gtk_search_entry_set_placeholder_text)(entry,i18n_tr(text));
  i18n_register_widget(GTK_WIDGET(entry),I18N_W_SEARCH_PLACEHOLDER,text);
}
static inline void i18n_entry_set_placeholder_text(GtkEntry *entry,
                                                   const char *text) {
  (gtk_entry_set_placeholder_text)(entry,i18n_tr(text));
  i18n_register_widget(GTK_WIDGET(entry),I18N_W_ENTRY_PLACEHOLDER,text);
}
static inline GtkWidget *i18n_drop_down_new_from_strings(
    const char * const *strings) {
  GtkStringList *list=gtk_string_list_new(NULL);
  if(strings!=NULL) {
    for(int i=0;strings[i]!=NULL;i++)
      gtk_string_list_append(list,i18n_tr(strings[i]));
  }
  return gtk_drop_down_new(G_LIST_MODEL(list),NULL);
}
#endif

/* All UI translation units include radio.h after GTK, so these aliases affect
 * application calls without interfering with GTK's declarations.  Dynamic
 * strings simply miss the catalogue and are returned unchanged. */
#ifndef I18N_IMPLEMENTATION
#define gtk_label_new(text) i18n_label_new(text)
#define gtk_label_set_text(label,text) i18n_label_set_text(label,text)
#define gtk_label_set_markup(label,text) i18n_label_set_markup(label,text)
#define gtk_button_new_with_label(text) i18n_button_new_with_label(text)
#define gtk_button_set_label(button,text) i18n_button_set_label(button,text)
#define gtk_check_button_new_with_label(text) i18n_check_button_new_with_label(text)
#define gtk_frame_new(text) i18n_frame_new(text)
#define gtk_window_set_title(window,text) i18n_window_set_title(window,text)
#define gtk_widget_set_tooltip_text(widget,text) i18n_widget_set_tooltip_text(widget,text)
#define gtk_search_entry_set_placeholder_text(entry,text) i18n_search_entry_set_placeholder_text(entry,text)
#define gtk_entry_set_placeholder_text(entry,text) i18n_entry_set_placeholder_text(entry,text)
#define gtk_menu_button_set_label(button,text) i18n_menu_button_set_label(button,text)
#define gtk_string_list_append(list,text) gtk_string_list_append(list,i18n_tr(text))
#define gtk_file_dialog_set_title(dialog,text) gtk_file_dialog_set_title(dialog,i18n_tr(text))
#define gtk_drop_down_new_from_strings(strings) i18n_drop_down_new_from_strings(strings)
#endif

#endif
