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
#include "ft8_waterfall.h"

// Tree store columns.  CALLDE/EXTRA are hidden, kept so a clicked row can be
// turned back into a QSO answer without a separate backing array.
enum { COL_UTC, COL_DB, COL_DT, COL_FREQ, COL_MSG, COL_TOME, COL_CQ, COL_B4,
       COL_BG, COL_BGSET, COL_COUNTRY, COL_CALLDE, COL_EXTRA, N_COLS };

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

// ---- station config callbacks ----------------------------------------------
// Callsign and grid live in the FT8 configuration page (Configure -> FT8), not
// in this panel; only the operational TX offset/slot are adjusted live here.
static void offset_changed(GtkSpinButton *sb, gpointer data) {
  radio->ft8_tx_offset = gtk_spin_button_get_value_as_int(sb);
}
// Digital protocol selector: 0 = FT8 (15 s slot), 1 = FT4 (7.5 s slot).  The RX
// thread propagates this to the decoder each buffer; the encoder/QSO/reporters
// read radio->ft8_proto directly.  Also mirror the choice into radio->decode_mode
// so the bottom-bar decoder selector (the source of truth for which decoder runs)
// stays consistent — the panel is only shown while an FT8/FT4 decoder is active.
static void proto_changed(GtkComboBox *cb, gpointer data) {
  int p = gtk_combo_box_get_active(cb);
  if (p < 0) p = 0;
  radio->ft8_proto = p;
  radio->decode_mode = p ? DECODE_FT4 : DECODE_FT8;
}
static void slot_changed(GtkComboBox *cb, gpointer data) {
  radio->ft8_tx_even = (gtk_combo_box_get_active(cb) == 0);
}
// Directed-CQ modifier: blank = plain CQ, else a region ("DX"/"EU"/…) or 3
// digits.  Keep only alnum, uppercased (the FT8 codec accepts 1-4 letters or
// exactly 3 digits after "CQ "); an unencodable value is surfaced at Tx time.
static void cq_dir_changed(GtkComboBoxText *cb, gpointer data) {
  gchar *t = gtk_combo_box_text_get_active_text(cb);
  char clean[8]; int j = 0;
  if (t) for (const char *p = t; *p && j < 7; p++)
    if (g_ascii_isalnum(*p)) clean[j++] = g_ascii_toupper(*p);
  clean[j] = '\0';
  g_strlcpy(radio->ft8_cq_dir, clean, sizeof(radio->ft8_cq_dir));
  g_free(t);
}

// ---- TX buttons ------------------------------------------------------------
static void halt_clicked(GtkButton *b, gpointer data)  { ft8_qso_halt(); }

static void tx_clicked(GtkButton *b, gpointer data) {
  ft8_qso_select_tx(GPOINTER_TO_INT(data));   // 1..6
}
static void enable_toggled(GtkToggleButton *t, gpointer data) {
  ft8_qso_set_tx_enabled(gtk_toggle_button_get_active(t));
}
static void auto_toggled(GtkCheckButton *t, gpointer data) {
  ft8_qso_set_auto(gtk_check_button_get_active(t));
}
static void free_send(GtkWidget *w, gpointer data) {
  if (free_entry) ft8_qso_send_free(gtk_editable_get_text(GTK_EDITABLE(free_entry)));
}
static gboolean filter_visible(GtkTreeModel *m, GtkTreeIter *it, gpointer data) {
  if (!cq_only) return TRUE;
  gboolean cq = FALSE;
  gtk_tree_model_get(m, it, COL_CQ, &cq, -1);
  return cq;
}
static void cqonly_toggled(GtkCheckButton *t, gpointer data) {
  cq_only = gtk_check_button_get_active(t);
  if (filter) gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(filter));
}
static void erase_clicked(GtkButton *b, gpointer data) {
  if (store) gtk_list_store_clear(store);
  // The decoder keeps the last non-empty slot's decodes until a NEW slot decodes
  // (so the compact bottom-bar readout doesn't blank every ~2 s).  If we reset
  // disp_utc to empty here, refresh() would see that retained batch as "new"
  // (its utc != "") and immediately re-append the stations we just cleared.
  // Instead, stamp disp_utc with the current batch's label so only a genuinely
  // new slot re-populates the list; the top block stays clean until then.
  FT8_DECODE tmp[64];
  char utc[8] = "";
  ft8_decoder_get_decodes(tmp, 64, utc);
  snprintf(disp_utc, sizeof(disp_utc), "%s", utc);
}

// Hovering a decode row shows the sender's DXCC country (from cty.dat).
static gboolean on_query_tooltip(GtkWidget *w, gint x, gint y, gboolean kbd,
                                 GtkTooltip *tip, gpointer data) {
  GtkTreeView *tv = GTK_TREE_VIEW(w);
  GtkTreeModel *model; GtkTreePath *path; GtkTreeIter it;
  // GTK4: x,y are passed by value (no longer in/out bin-window coords).
  if (!gtk_tree_view_get_tooltip_context(tv, x, y, kbd, &model, &path, &it))
    return FALSE;
  gchar *country = NULL;
  gtk_tree_model_get(model, &it, COL_COUNTRY, &country, -1);
  gboolean show = country && country[0];
  if (show) {
    gtk_tooltip_set_text(tip, country);
    gtk_tree_view_set_tooltip_row(tv, tip, path);
  }
  gtk_tree_path_free(path);
  g_free(country);
  return show;
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
    long long dial = (radio && radio->active_receiver) ? radio->active_receiver->frequency_a : 0;
    GtkTreeIter it;
    for (int i = 0; i < n; i++) {
      gboolean tome = mycall[0] && d[i].call_to[0] &&
                      g_ascii_strcasecmp(d[i].call_to, mycall) == 0;
      gboolean iscq = strncmp(d[i].call_to, "CQ", 2) == 0;
      gboolean b4 = d[i].call_de[0] && ft8_qso_worked(d[i].call_de);
      // Two-level "new one" highlight: a brand-new DXCC (any band) is gold; a
      // country worked elsewhere but new on THIS band is blue.
      gboolean new_ever = d[i].call_de[0] && ft8_qso_new_dxcc(d[i].call_de);
      gboolean new_band = d[i].call_de[0] && !new_ever &&
                          ft8_qso_new_dxcc_band(d[i].call_de, dial);
      const char *bg = new_ever ? "#b8860b" : (new_band ? "#2f6fb0" : NULL);
      const char *country = d[i].call_de[0] ? ft8_qso_country(d[i].call_de) : NULL;
      char dt[8];
      snprintf(dt, sizeof(dt), "%+.1f", d[i].dt);
      gtk_list_store_append(store, &it);
      gtk_list_store_set(store, &it,
                         COL_UTC,     d[i].utc,
                         COL_DB,      (gint)d[i].snr,
                         COL_DT,      dt,
                         COL_FREQ,    (gint)d[i].freq,
                         COL_MSG,     d[i].text,
                         COL_TOME,    tome,
                         COL_CQ,      iscq,
                         COL_B4,      b4,
                         COL_BG,      bg ? bg : "",
                         COL_BGSET,   bg != NULL,
                         COL_COUNTRY, country ? country : "",
                         COL_CALLDE,  d[i].call_de,
                         COL_EXTRA,   d[i].extra,
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
    if (i + 1 == active) gtk_widget_add_css_class(txbtn[i], "suggested-action");
    else                 gtk_widget_remove_css_class(txbtn[i], "suggested-action");
  }

  // Sync the toggles from the engine without re-entering their handlers.
  if (enable_btn) {
    // Tx needs both identity fields (Configure -> FT8); grey the toggle out and
    // explain why until they're set, matching WSJT-X's "no call, no Tx" rule.
    gboolean ready = radio->station_call[0] && radio->station_grid[0];
    gtk_widget_set_sensitive(enable_btn, ready);
    gtk_widget_set_tooltip_text(enable_btn,
        ready ? NULL : "Set your callsign and grid in Configure \342\206\222 FT8");
    gboolean en = ft8_qso_tx_enabled();
    g_signal_handlers_block_by_func(enable_btn, (gpointer)enable_toggled, NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enable_btn), en);
    g_signal_handlers_unblock_by_func(enable_btn, (gpointer)enable_toggled, NULL);
    // Red only while armed; a plain grey button when Tx is off, so the state
    // is obvious at a glance.
    if (en) gtk_widget_add_css_class(enable_btn, "destructive-action");
    else    gtk_widget_remove_css_class(enable_btn, "destructive-action");
  }
  if (auto_chk) {
    g_signal_handlers_block_by_func(auto_chk, (gpointer)auto_toggled, NULL);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(auto_chk), ft8_qso_auto());
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
  // Breathing room so the rows don't sit flush against the panel edges.
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 4);
  gtk_widget_set_margin_bottom(box, 4);

  // --- operational config row (callsign/grid live in Configure -> FT8) ---
  GtkWidget *cfg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  // Protocol selector (FT8 / FT4) — leftmost, beside the TX frequency controls.
  GtkWidget *mode = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), "FT8");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(mode), "FT4");
  gtk_combo_box_set_active(GTK_COMBO_BOX(mode), radio->ft8_proto ? 1 : 0);
  gtk_widget_set_tooltip_text(mode, "Digital protocol: FT8 (15 s) or FT4 (7.5 s)");
  g_signal_connect(mode, "changed", G_CALLBACK(proto_changed), NULL);
  gtk_box_append(GTK_BOX(cfg),mode);

  gtk_box_append(GTK_BOX(cfg),gtk_label_new("Tx Hz"));
  offset_spin = gtk_spin_button_new_with_range(200, 2800, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(offset_spin), radio->ft8_tx_offset);
  g_signal_connect(offset_spin, "value-changed", G_CALLBACK(offset_changed), NULL);
  gtk_box_append(GTK_BOX(cfg),offset_spin);

  GtkWidget *slot = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(slot), "Even");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(slot), "Odd");
  gtk_combo_box_set_active(GTK_COMBO_BOX(slot), radio->ft8_tx_even ? 0 : 1);
  g_signal_connect(slot, "changed", G_CALLBACK(slot_changed), NULL);
  gtk_box_append(GTK_BOX(cfg),slot);

  // Directed CQ: blank = plain CQ, or pick/type a region (DX/EU/NA/…) or 3 digits.
  gtk_box_append(GTK_BOX(cfg),gtk_label_new("CQ"));
  GtkWidget *cqdir = gtk_combo_box_text_new_with_entry();
  const char *dirs[] = { "", "DX", "EU", "NA", "SA", "AS", "AF", "OC" };
  for (unsigned i = 0; i < G_N_ELEMENTS(dirs); i++)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cqdir), dirs[i]);
  GtkWidget *cqentry = gtk_combo_box_get_child(GTK_COMBO_BOX(cqdir));
  gtk_editable_set_width_chars(GTK_EDITABLE(cqentry), 4);
  gtk_entry_set_max_length(GTK_ENTRY(cqentry), sizeof(radio->ft8_cq_dir) - 1);
  gtk_editable_set_text(GTK_EDITABLE(cqentry), radio->ft8_cq_dir);
  gtk_widget_set_tooltip_text(cqdir,
      "Directed CQ: blank = CQ; a region (DX/EU/NA/SA/AS/AF/OC) or 3 digits");
  g_signal_connect(cqdir, "changed", G_CALLBACK(cq_dir_changed), NULL);
  gtk_box_append(GTK_BOX(cfg),cqdir);

  gtk_box_append(GTK_BOX(box),cfg);

  // (The FT8 band waterfall is placed to the RIGHT of the main RX spectrum, not
  // in this panel — see receiver_ft8_waterfall_sync().)

  // --- decode list ---
  store = gtk_list_store_new(N_COLS,
                             G_TYPE_STRING,  /* UTC   */ G_TYPE_INT,     /* dB    */
                             G_TYPE_STRING,  /* DT    */ G_TYPE_INT,     /* Hz    */
                             G_TYPE_STRING,  /* Msg   */ G_TYPE_BOOLEAN, /* tome  */
                             G_TYPE_BOOLEAN, /* cq    */ G_TYPE_BOOLEAN, /* b4    */
                             G_TYPE_STRING,  /* bg    */ G_TYPE_BOOLEAN, /* bgset */
                             G_TYPE_STRING,  /* cntry */
                             G_TYPE_STRING,  /* de    */ G_TYPE_STRING   /* extra */);
  filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL);
  gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter),
                                         filter_visible, NULL, NULL);
  view = gtk_tree_view_new_with_model(filter);
  g_object_unref(store);
  g_object_unref(filter);
  g_signal_connect(view, "row-activated", G_CALLBACK(row_activated), NULL);
  gtk_widget_set_has_tooltip(view, TRUE);
  g_signal_connect(view, "query-tooltip", G_CALLBACK(on_query_tooltip), NULL);

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
    // Highlight new DXCC (gold = new ever, blue = new on this band); the colour
    // and on/off flag come from the model so both levels share one renderer.
    gtk_tree_view_column_add_attribute(c, r, "cell-background", COL_BG);
    gtk_tree_view_column_add_attribute(c, r, "cell-background-set", COL_BGSET);
    // Colour CQ messages green so they stand out in the band-activity list.
    if (cols[k].c == COL_MSG) {
      gtk_tree_view_column_add_attribute(c, r, "foreground-set", COL_CQ);
      g_object_set(r, "foreground", "#33aa33", NULL);
    }
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), c);
  }

  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  // Keep the list's minimum small so the GTK4 paned can shrink the panel and
  // leave the RF spectrum above it a usable height (it still expands via
  // vexpand when the pane is tall).
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 80);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);

  // --- Tx1..Tx6 message buttons (double-click a decode fills the DX call) ---
  GtkWidget *txbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
  for (int i = 0; i < 6; i++) {
    txbtn[i] = gtk_button_new_with_label("—");
    gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(txbtn[i])), GTK_ALIGN_START);
    g_signal_connect(txbtn[i], "clicked", G_CALLBACK(tx_clicked), GINT_TO_POINTER(i + 1));
    gtk_box_append(GTK_BOX(txbox),txbtn[i]);
  }

  // --- main row: Tx message column on the left, band-activity list on the right ---
  GtkWidget *mainrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(mainrow),txbox);
  gtk_box_append(GTK_BOX(mainrow),scroll); gtk_widget_set_hexpand(scroll,TRUE); gtk_widget_set_vexpand(scroll,TRUE);
  gtk_box_append(GTK_BOX(box),mainrow); gtk_widget_set_hexpand(mainrow,TRUE); gtk_widget_set_vexpand(mainrow,TRUE);

  // --- TX controls + status ---
  GtkWidget *ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  enable_btn = gtk_toggle_button_new_with_label("Enable Tx");
  // The red "destructive-action" class is applied only while Tx is armed (see
  // refresh()); off, it stays a neutral grey toggle.
  g_signal_connect(enable_btn, "toggled", G_CALLBACK(enable_toggled), NULL);
  gtk_box_append(GTK_BOX(ctl),enable_btn);
  auto_chk = gtk_check_button_new_with_label("Auto Seq");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(auto_chk), ft8_qso_auto());
  g_signal_connect(auto_chk, "toggled", G_CALLBACK(auto_toggled), NULL);
  gtk_box_append(GTK_BOX(ctl),auto_chk);
  GtkWidget *cqchk = gtk_check_button_new_with_label("CQ only");
  g_signal_connect(cqchk, "toggled", G_CALLBACK(cqonly_toggled), NULL);
  gtk_box_append(GTK_BOX(ctl),cqchk);
  GtkWidget *erase = gtk_button_new_with_label("Erase");
  g_signal_connect(erase, "clicked", G_CALLBACK(erase_clicked), NULL);
  gtk_box_append(GTK_BOX(ctl),erase);
  GtkWidget *halt = gtk_button_new_with_label("Halt Tx");
  g_signal_connect(halt, "clicked", G_CALLBACK(halt_clicked), NULL);
  gtk_box_append(GTK_BOX(ctl),halt);
  dx_label = gtk_label_new("DX: —");
  gtk_box_append(GTK_BOX(ctl),dx_label);
  gtk_box_append(GTK_BOX(box),ctl);

  // --- free-text message row ---
  GtkWidget *frow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  free_entry = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(free_entry), 13);
  gtk_entry_set_placeholder_text(GTK_ENTRY(free_entry), "free text (≤13)");
  g_signal_connect(free_entry, "activate", G_CALLBACK(free_send), NULL);
  gtk_box_append(GTK_BOX(frow),free_entry); gtk_widget_set_hexpand(free_entry,TRUE); gtk_widget_set_vexpand(free_entry,TRUE);
  GtkWidget *freebtn = gtk_button_new_with_label("Send Free");
  g_signal_connect(freebtn, "clicked", G_CALLBACK(free_send), NULL);
  gtk_box_append(GTK_BOX(frow),freebtn);
  gtk_box_append(GTK_BOX(box),frow);

  status_label = gtk_label_new("Status: Idle");
  gtk_widget_set_halign(status_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box),status_label);

  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), NULL);
  refresh_id = g_timeout_add(500, refresh, NULL);

  gtk_widget_set_visible(box, TRUE);
  return box;
}
