/* Copyright (C)
* 2026 - MacHPSDR fork
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

#include <string.h>
#include <ctype.h>
#include <gtk/gtk.h>

#include "discovered.h"
#include "adc.h"
#include "dac.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "radio.h"
#include "main.h"

#include "ft8_decoder.h"
#include "ft8_qso.h"
#include "ft8_panel.h"

// Tree store columns.  CALLDE/EXTRA are hidden, kept so a clicked row can be
// turned back into a QSO answer without a separate backing array.
enum { COL_UTC, COL_DB, COL_DT, COL_FREQ, COL_MSG, COL_TOME, COL_CQ, COL_B4,
       COL_CALLDE, COL_EXTRA, N_COLS };

#define MAX_ROWS 1000   // rolling band-activity cap

// Single live panel instance (only one FT8 panel ever exists at a time).
static GtkListStore     *store = NULL;
static GtkTreeModel     *filter = NULL;   // CQ-only view over `store`
static gboolean          cq_only = FALSE;
static GtkWidget    *view = NULL;
static GtkWidget    *status_label = NULL;
static GtkWidget    *dx_label = NULL;
static GtkWidget    *enable_btn = NULL;   // GtkToggleButton "Enable Tx"
static GtkWidget    *auto_chk = NULL;     // GtkCheckButton "Auto Seq"
static GtkWidget    *offset_spin = NULL;  // TX offset (Hz), synced to shift-click
static GtkWidget    *free_entry = NULL;   // free-text message entry
static GtkWidget    *txbtn[6] = { NULL };  // Tx1..Tx6 message buttons
static guint         refresh_id = 0;

static char          disp_utc[8] = "";     // last slot appended to the list

// ---- helpers ---------------------------------------------------------------
static void upper_copy(char *dst, size_t dstsz, const char *src) {
  size_t i = 0;
  for (; src[i] && i < dstsz - 1; i++) dst[i] = (char)toupper((unsigned char)src[i]);
  dst[i] = '\0';
}

// ---- station config callbacks ----------------------------------------------
static void call_changed(GtkEditable *e, gpointer data) {
  upper_copy(radio->station_call, sizeof(radio->station_call), gtk_entry_get_text(GTK_ENTRY(e)));
}
static void grid_changed(GtkEditable *e, gpointer data) {
  upper_copy(radio->station_grid, sizeof(radio->station_grid), gtk_entry_get_text(GTK_ENTRY(e)));
}
static void offset_changed(GtkSpinButton *sb, gpointer data) {
  radio->ft8_tx_offset = gtk_spin_button_get_value_as_int(sb);
}
static void slot_changed(GtkComboBox *cb, gpointer data) {
  radio->ft8_tx_even = (gtk_combo_box_get_active(cb) == 0);
}

// ---- TX buttons ------------------------------------------------------------
static void halt_clicked(GtkButton *b, gpointer data)  { ft8_qso_halt(); }

static void tx_clicked(GtkButton *b, gpointer data) {
  ft8_qso_select_tx(GPOINTER_TO_INT(data));   // 1..6
}
static void enable_toggled(GtkToggleButton *t, gpointer data) {
  ft8_qso_set_tx_enabled(gtk_toggle_button_get_active(t));
}
static void auto_toggled(GtkToggleButton *t, gpointer data) {
  ft8_qso_set_auto(gtk_toggle_button_get_active(t));
}
static void free_send(GtkWidget *w, gpointer data) {
  if (free_entry) ft8_qso_send_free(gtk_entry_get_text(GTK_ENTRY(free_entry)));
}
static gboolean filter_visible(GtkTreeModel *m, GtkTreeIter *it, gpointer data) {
  if (!cq_only) return TRUE;
  gboolean cq = FALSE;
  gtk_tree_model_get(m, it, COL_CQ, &cq, -1);
  return cq;
}
static void cqonly_toggled(GtkToggleButton *t, gpointer data) {
  cq_only = gtk_toggle_button_get_active(t);
  if (filter) gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(filter));
}
static void erase_clicked(GtkButton *b, gpointer data) {
  if (store) gtk_list_store_clear(store);
  disp_utc[0] = '\0';
}

// Double-click a decode row: work that station.
static void row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data) {
  GtkTreeModel *model = gtk_tree_view_get_model(tv);   // may be the CQ filter
  GtkTreeIter iter;
  if (!gtk_tree_model_get_iter(model, &iter, path)) return;
  gchar *callde = NULL, *extra = NULL, *utc = NULL;
  gtk_tree_model_get(model, &iter,
                     COL_CALLDE, &callde, COL_EXTRA, &extra, COL_UTC, &utc, -1);
  if (callde && callde[0]) {
    FT8_DECODE d;
    memset(&d, 0, sizeof(d));
    snprintf(d.call_de, sizeof(d.call_de), "%s", callde);
    snprintf(d.extra, sizeof(d.extra), "%s", extra ? extra : "");
    snprintf(d.utc, sizeof(d.utc), "%s", utc ? utc : "");
    ft8_qso_answer(&d);
  }
  g_free(callde);
  g_free(extra);
  g_free(utc);
}

// ---- periodic refresh ------------------------------------------------------
static gboolean refresh(gpointer data) {
  FT8_DECODE d[64];
  char utc[8] = "";
  int n = ft8_decoder_get_decodes(d, 64, utc);

  // Append each new slot's decodes to a rolling band-activity list.
  if (n > 0 && utc[0] && strcmp(utc, disp_utc) != 0) {
    snprintf(disp_utc, sizeof(disp_utc), "%s", utc);

    const char *mycall = radio->station_call;
    GtkTreeIter it;
    for (int i = 0; i < n; i++) {
      gboolean tome = mycall[0] && d[i].call_to[0] &&
                      g_ascii_strcasecmp(d[i].call_to, mycall) == 0;
      gboolean iscq = strncmp(d[i].call_to, "CQ", 2) == 0;
      gboolean b4 = d[i].call_de[0] && ft8_qso_worked(d[i].call_de);
      char dt[8];
      snprintf(dt, sizeof(dt), "%+.1f", d[i].dt);
      gtk_list_store_append(store, &it);
      gtk_list_store_set(store, &it,
                         COL_UTC,    d[i].utc,
                         COL_DB,     (gint)d[i].snr,
                         COL_DT,     dt,
                         COL_FREQ,   (gint)d[i].freq,
                         COL_MSG,    d[i].text,
                         COL_TOME,   tome,
                         COL_CQ,     iscq,
                         COL_B4,     b4,
                         COL_CALLDE, d[i].call_de,
                         COL_EXTRA,  d[i].extra,
                         -1);
    }
    // Cap the history: drop the oldest rows from the top.
    int rows = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store), NULL);
    while (rows > MAX_ROWS) {
      GtkTreeIter first;
      if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &first)) break;
      gtk_list_store_remove(store, &first);
      rows--;
    }
    // Keep the newest decode in view (count in the model the view shows).
    int vis = gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(view)), NULL);
    if (vis > 0) {
      GtkTreePath *path = gtk_tree_path_new_from_indices(vis - 1, -1);
      gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(view), path, NULL, FALSE, 0, 0);
      gtk_tree_path_free(path);
    }
  }

  // Tx1..Tx6 message buttons: labels, availability, and active highlight.
  char msgs[6][32];
  int active = ft8_qso_messages(msgs);
  for (int i = 0; i < 6; i++) {
    if (!txbtn[i]) continue;
    char lbl[40];
    snprintf(lbl, sizeof(lbl), "Tx%d  %s", i + 1, msgs[i][0] ? msgs[i] : "—");
    gtk_button_set_label(GTK_BUTTON(txbtn[i]), lbl);
    gtk_widget_set_sensitive(txbtn[i], msgs[i][0] != '\0');
    GtkStyleContext *sc = gtk_widget_get_style_context(txbtn[i]);
    if (i + 1 == active) gtk_style_context_add_class(sc, "suggested-action");
    else                 gtk_style_context_remove_class(sc, "suggested-action");
  }

  // Sync the toggles from the engine without re-entering their handlers.
  if (enable_btn) {
    g_signal_handlers_block_by_func(enable_btn, (gpointer)enable_toggled, NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enable_btn), ft8_qso_tx_enabled());
    g_signal_handlers_unblock_by_func(enable_btn, (gpointer)enable_toggled, NULL);
  }
  if (auto_chk) {
    g_signal_handlers_block_by_func(auto_chk, (gpointer)auto_toggled, NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_chk), ft8_qso_auto());
    g_signal_handlers_unblock_by_func(auto_chk, (gpointer)auto_toggled, NULL);
  }
  if (offset_spin) {   // reflect shift-click changes made on the panadapter
    if (gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(offset_spin)) != radio->ft8_tx_offset) {
      g_signal_handlers_block_by_func(offset_spin, (gpointer)offset_changed, NULL);
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(offset_spin), radio->ft8_tx_offset);
      g_signal_handlers_unblock_by_func(offset_spin, (gpointer)offset_changed, NULL);
    }
  }
  if (dx_label) {
    const char *dx = ft8_qso_dx_call();
    char buf[32];
    snprintf(buf, sizeof(buf), "DX: %s", (dx && dx[0]) ? dx : "—");
    gtk_label_set_text(GTK_LABEL(dx_label), buf);
  }
  if (status_label) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Status: %s", ft8_qso_status());
    gtk_label_set_text(GTK_LABEL(status_label), buf);
  }
  return G_SOURCE_CONTINUE;
}

static void on_destroy(GtkWidget *w, gpointer data) {
  if (refresh_id) { g_source_remove(refresh_id); refresh_id = 0; }
  store = NULL; filter = NULL; view = NULL; status_label = NULL; dx_label = NULL;
  enable_btn = NULL; auto_chk = NULL; offset_spin = NULL; free_entry = NULL;
  for (int i = 0; i < 6; i++) txbtn[i] = NULL;
  disp_utc[0] = '\0';
}

// ---- construction ----------------------------------------------------------
GtkWidget *ft8_panel_create(void) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_name(box, "ft8-panel");

  // --- station config row ---
  GtkWidget *cfg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_pack_start(GTK_BOX(cfg), gtk_label_new("Call"), FALSE, FALSE, 0);
  GtkWidget *call = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(call), 8);
  gtk_entry_set_text(GTK_ENTRY(call), radio->station_call);
  g_signal_connect(call, "changed", G_CALLBACK(call_changed), NULL);
  gtk_box_pack_start(GTK_BOX(cfg), call, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(cfg), gtk_label_new("Grid"), FALSE, FALSE, 0);
  GtkWidget *grid = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(grid), 6);
  gtk_entry_set_text(GTK_ENTRY(grid), radio->station_grid);
  g_signal_connect(grid, "changed", G_CALLBACK(grid_changed), NULL);
  gtk_box_pack_start(GTK_BOX(cfg), grid, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(cfg), gtk_label_new("Tx Hz"), FALSE, FALSE, 0);
  offset_spin = gtk_spin_button_new_with_range(200, 2800, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(offset_spin), radio->ft8_tx_offset);
  g_signal_connect(offset_spin, "value-changed", G_CALLBACK(offset_changed), NULL);
  gtk_box_pack_start(GTK_BOX(cfg), offset_spin, FALSE, FALSE, 0);

  GtkWidget *slot = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(slot), "Even");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(slot), "Odd");
  gtk_combo_box_set_active(GTK_COMBO_BOX(slot), radio->ft8_tx_even ? 0 : 1);
  g_signal_connect(slot, "changed", G_CALLBACK(slot_changed), NULL);
  gtk_box_pack_start(GTK_BOX(cfg), slot, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(box), cfg, FALSE, FALSE, 0);

  // --- decode list ---
  store = gtk_list_store_new(N_COLS,
                             G_TYPE_STRING,  /* UTC  */ G_TYPE_INT,     /* dB   */
                             G_TYPE_STRING,  /* DT   */ G_TYPE_INT,     /* Hz   */
                             G_TYPE_STRING,  /* Msg  */ G_TYPE_BOOLEAN, /* tome */
                             G_TYPE_BOOLEAN, /* cq   */ G_TYPE_BOOLEAN, /* b4   */
                             G_TYPE_STRING,  /* de   */ G_TYPE_STRING   /* extra*/);
  filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL);
  gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter),
                                         filter_visible, NULL, NULL);
  view = gtk_tree_view_new_with_model(filter);
  g_object_unref(store);
  g_object_unref(filter);
  g_signal_connect(view, "row-activated", G_CALLBACK(row_activated), NULL);

  struct { const char *t; int c; } cols[] = {
    {"UTC", COL_UTC}, {"dB", COL_DB}, {"DT", COL_DT}, {"Hz", COL_FREQ}, {"Message", COL_MSG}
  };
  for (unsigned k = 0; k < G_N_ELEMENTS(cols); k++) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c =
      gtk_tree_view_column_new_with_attributes(cols[k].t, r, "text", cols[k].c, NULL);
    // Bold the rows addressed to our station; strike out worked-before calls.
    gtk_tree_view_column_add_attribute(c, r, "weight-set", COL_TOME);
    g_object_set(r, "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_tree_view_column_add_attribute(c, r, "strikethrough-set", COL_B4);
    g_object_set(r, "strikethrough", TRUE, NULL);
    // Colour CQ messages green so they stand out in the band-activity list.
    if (cols[k].c == COL_MSG) {
      gtk_tree_view_column_add_attribute(c, r, "foreground-set", COL_CQ);
      g_object_set(r, "foreground", "#33aa33", NULL);
    }
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
  }

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(scroll), view);

  // --- Tx1..Tx6 message buttons (double-click a decode fills the DX call) ---
  GtkWidget *txbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
  for (int i = 0; i < 6; i++) {
    txbtn[i] = gtk_button_new_with_label("—");
    gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(txbtn[i])), GTK_ALIGN_START);
    g_signal_connect(txbtn[i], "clicked", G_CALLBACK(tx_clicked), GINT_TO_POINTER(i + 1));
    gtk_box_pack_start(GTK_BOX(txbox), txbtn[i], FALSE, FALSE, 0);
  }

  // --- main row: Tx message column on the left, band-activity list on the right ---
  GtkWidget *mainrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start(GTK_BOX(mainrow), txbox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(mainrow), scroll, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), mainrow, TRUE, TRUE, 0);

  // --- TX controls + status ---
  GtkWidget *ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  enable_btn = gtk_toggle_button_new_with_label("Enable Tx");
  gtk_style_context_add_class(gtk_widget_get_style_context(enable_btn), "destructive-action");
  g_signal_connect(enable_btn, "toggled", G_CALLBACK(enable_toggled), NULL);
  gtk_box_pack_start(GTK_BOX(ctl), enable_btn, FALSE, FALSE, 0);
  auto_chk = gtk_check_button_new_with_label("Auto Seq");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_chk), ft8_qso_auto());
  g_signal_connect(auto_chk, "toggled", G_CALLBACK(auto_toggled), NULL);
  gtk_box_pack_start(GTK_BOX(ctl), auto_chk, FALSE, FALSE, 0);
  GtkWidget *cqchk = gtk_check_button_new_with_label("CQ only");
  g_signal_connect(cqchk, "toggled", G_CALLBACK(cqonly_toggled), NULL);
  gtk_box_pack_start(GTK_BOX(ctl), cqchk, FALSE, FALSE, 0);
  GtkWidget *erase = gtk_button_new_with_label("Erase");
  g_signal_connect(erase, "clicked", G_CALLBACK(erase_clicked), NULL);
  gtk_box_pack_start(GTK_BOX(ctl), erase, FALSE, FALSE, 0);
  GtkWidget *halt = gtk_button_new_with_label("Halt Tx");
  g_signal_connect(halt, "clicked", G_CALLBACK(halt_clicked), NULL);
  gtk_box_pack_start(GTK_BOX(ctl), halt, FALSE, FALSE, 0);
  dx_label = gtk_label_new("DX: —");
  gtk_box_pack_start(GTK_BOX(ctl), dx_label, FALSE, FALSE, 6);
  gtk_box_pack_start(GTK_BOX(box), ctl, FALSE, FALSE, 0);

  // --- free-text message row ---
  GtkWidget *frow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  free_entry = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(free_entry), 13);
  gtk_entry_set_placeholder_text(GTK_ENTRY(free_entry), "free text (≤13)");
  g_signal_connect(free_entry, "activate", G_CALLBACK(free_send), NULL);
  gtk_box_pack_start(GTK_BOX(frow), free_entry, TRUE, TRUE, 0);
  GtkWidget *freebtn = gtk_button_new_with_label("Send Free");
  g_signal_connect(freebtn, "clicked", G_CALLBACK(free_send), NULL);
  gtk_box_pack_start(GTK_BOX(frow), freebtn, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), frow, FALSE, FALSE, 0);

  status_label = gtk_label_new("Status: Idle");
  gtk_widget_set_halign(status_label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);

  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), NULL);
  refresh_id = g_timeout_add(500, refresh, NULL);

  gtk_widget_show_all(box);
  return box;
}
