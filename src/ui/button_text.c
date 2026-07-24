/* Copyright (C)
* 2018 - John Melton, G0ORX/N6LYT
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*
*/
#include <gtk/gtk.h>

// GTK4: per-widget style providers (gtk_style_context_add_provider) are gone.
// Instead we install ONE display-wide provider that defines a CSS class per
// distinct colour ever requested, and swap that class onto the widget. This
// keeps the per-widget semantics without leaking a provider on every call
// (previously every mox toggle added a fresh provider to the widget context).
void set_button_text_color(GtkWidget *widget, char *color) {
  static GtkCssProvider *provider = NULL;
  static GHashTable *colors = NULL;   // colour string -> class name
  static GString *css = NULL;
  static guint next_id = 0;

  if (provider == NULL) {
    provider = gtk_css_provider_new();
    colors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    css = g_string_new("");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }

  // Ensure a class exists for this colour, defining it once on first use.
  char *cls = g_hash_table_lookup(colors, color);
  if (cls == NULL) {
    cls = g_strdup_printf("btntext-%u", next_id++);
    g_hash_table_insert(colors, g_strdup(color), cls);
    // GTK4 always uses the CSS3 element selectors.
    g_string_append_printf(css, ".%s, .%s button, .%s label { color: %s; }\n",
                           cls, cls, cls, color);
    gtk_css_provider_load_from_string(provider, css->str);
  }

  // Drop any previously-applied btntext-* class, then add the new one.
  char **existing = gtk_widget_get_css_classes(widget);
  for (int i = 0; existing && existing[i]; i++) {
    if (g_str_has_prefix(existing[i], "btntext-"))
      gtk_widget_remove_css_class(widget, existing[i]);
  }
  g_strfreev(existing);
  gtk_widget_add_css_class(widget, cls);
}

