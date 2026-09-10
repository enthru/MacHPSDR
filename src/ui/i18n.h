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

void i18n_init(const char *config_dir);
I18nLanguage i18n_language(void);
const char *i18n_language_code(void);
const char *i18n_language_name(I18nLanguage language);
void i18n_set_language(I18nLanguage language);
const char *i18n_tr(const char *english);

#ifdef GTK_MAJOR_VERSION
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
#define gtk_label_new(text) gtk_label_new(i18n_tr(text))
#define gtk_label_set_text(label,text) gtk_label_set_text(label,i18n_tr(text))
#define gtk_button_new_with_label(text) gtk_button_new_with_label(i18n_tr(text))
#define gtk_button_set_label(button,text) gtk_button_set_label(button,i18n_tr(text))
#define gtk_check_button_new_with_label(text) gtk_check_button_new_with_label(i18n_tr(text))
#define gtk_frame_new(text) gtk_frame_new(i18n_tr(text))
#define gtk_window_set_title(window,text) gtk_window_set_title(window,i18n_tr(text))
#define gtk_widget_set_tooltip_text(widget,text) gtk_widget_set_tooltip_text(widget,i18n_tr(text))
#define gtk_search_entry_set_placeholder_text(entry,text) gtk_search_entry_set_placeholder_text(entry,i18n_tr(text))
#define gtk_entry_set_placeholder_text(entry,text) gtk_entry_set_placeholder_text(entry,i18n_tr(text))
#define gtk_menu_button_set_label(button,text) gtk_menu_button_set_label(button,i18n_tr(text))
#define gtk_string_list_append(list,text) gtk_string_list_append(list,i18n_tr(text))
#define gtk_file_dialog_set_title(dialog,text) gtk_file_dialog_set_title(dialog,i18n_tr(text))
#define gtk_drop_down_new_from_strings(strings) i18n_drop_down_new_from_strings(strings)
#endif

#endif
