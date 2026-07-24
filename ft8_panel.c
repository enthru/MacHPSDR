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

// GTK4: GtkTreeView/GtkListStore/GtkTreeModelFilter are deprecated. The band-
// activity list is a GtkColumnView over a GListStore of Ft8Item GObjects, with a
// GtkFilterListModel for the "CQ only" view. Per-row styling (bold to-me,
// strikethrough worked-before, green CQ message, gold/blue new-DXCC background)
// is applied in the cell bind via Pango attributes + CSS classes.
enum { VCOL_UTC, VCOL_DB, VCOL_DT, VCOL_FREQ, VCOL_MSG };   // visible columns

#define MAX_ROWS 1000   // rolling band-activity cap

#define FT8_TYPE_ITEM (ft8_item_get_type())
G_DECLARE_FINAL_TYPE(Ft8Item, ft8_item, FT8, ITEM, GObject)
struct _Ft8Item {
  GObject parent_instance;
  char *utc, *dt, *msg, *country, *callde, *extra, *bg;  // bg = NULL / "#rrggbb"
  int db, freq;
  gboolean tome, cq, b4;
};
G_DEFINE_TYPE(Ft8Item, ft8_item, G_TYPE_OBJECT)
static void ft8_item_finalize(GObject *o) {
  Ft8Item *it = FT8_ITEM(o);
  g_free(it->utc); g_free(it->dt); g_free(it->msg); g_free(it->country);
  g_free(it->callde); g_free(it->extra); g_free(it->bg);
  G_OBJECT_CLASS(ft8_item_parent_class)->finalize(o);
}
static void ft8_item_class_init(Ft8ItemClass *k){ G_OBJECT_CLASS(k)->finalize = ft8_item_finalize; }
static void ft8_item_init(Ft8Item *it){ }

// Single live panel instance (only one FT8 panel ever exists at a time).
static GListStore        *store = NULL;
static GtkFilterListModel *filter_model = NULL;
static GtkFilter         *cq_filter = NULL;
static GtkSelectionModel *list_sel = NULL;   // no-selection wrapper for the view
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
static void proto_changed(GtkDropDown *cb, GParamSpec *ps, gpointer data) {
  int p = (int)gtk_drop_down_get_selected(cb);
  if (p < 0) p = 0;
  radio->ft8_proto = p;
  radio->decode_mode = p ? DECODE_FT4 : DECODE_FT8;
}
static void slot_changed(GtkDropDown *cb, GParamSpec *ps, gpointer data) {
  radio->ft8_tx_even = (gtk_drop_down_get_selected(cb) == 0);
}
// Directed-CQ modifier: blank = plain CQ, else a region ("DX"/"EU"/…) or 3
// digits.  Keep only alnum, uppercased (the FT8 codec accepts 1-4 letters or
// exactly 3 digits after "CQ "); an unencodable value is surfaced at Tx time.
// GTK4 has no non-deprecated editable combo, so this is a plain GtkEntry.
static void cq_dir_changed(GtkEditable *cb, gpointer data) {
  const char *t = gtk_editable_get_text(cb);
  char clean[8]; int j = 0;
  if (t) for (const char *p = t; *p && j < 7; p++)
    if (g_ascii_isalnum(*p)) clean[j++] = g_ascii_toupper(*p);
  clean[j] = '\0';
  g_strlcpy(radio->ft8_cq_dir, clean, sizeof(radio->ft8_cq_dir));
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
// CQ-only filter over the store.
static gboolean cq_match(gpointer item, gpointer data) {
  if (!cq_only) return TRUE;
  return ((Ft8Item *)item)->cq;
}
static void cqonly_toggled(GtkCheckButton *t, gpointer data) {
  cq_only = gtk_check_button_get_active(t);
  if (cq_filter) gtk_filter_changed(cq_filter, GTK_FILTER_CHANGE_DIFFERENT);
}
static void erase_clicked(GtkButton *b, gpointer data) {
  if (store) g_list_store_remove_all(store);
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

// One display-wide provider defining the two new-DXCC cell backgrounds.
static void ft8_install_css(void) {
  static gboolean done = FALSE;
  if (done) return;
  done = TRUE;
  GtkCssProvider *p = gtk_css_provider_new();
  gtk_css_provider_load_from_string(p,
      ".ft8-gold{background-color:#b8860b;} .ft8-blue{background-color:#2f6fb0;}");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
      GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(p);
}

// Minimum width (in characters) reserved for each numeric column, sized to the
// worst case so a wider-than-current value never gets squeezed/clipped:
// UTC "123845" (6), dB "-24" (3), DT "+9,9" (4), Hz "3000" (4) — plus a char of
// padding. Message returns 0: it is the only expanding column (see ft8_col).
static int ft8_col_min_chars(int vc) {
  switch (vc) {
    case VCOL_UTC:  return 7;
    case VCOL_DB:   return 4;
    case VCOL_DT:   return 5;
    case VCOL_FREQ: return 5;
    default:        return 0;   // VCOL_MSG
  }
}

// Cell factory: a left-aligned label. Numeric columns reserve a fixed minimum
// width so they never squeeze; only the Message cell expands to take the slack
// (previously every cell had hexpand, so all columns shared width evenly and
// the narrow numeric columns clipped when the panel was tight or a value grew).
// The bind applies the text and per-row styling (bold/strikethrough/green +
// gold/blue background).
static void ft8_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u) {
  int vc = GPOINTER_TO_INT(u);
  GtkWidget *lbl = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
  if (vc == VCOL_MSG) {
    gtk_widget_set_hexpand(lbl, TRUE);
  } else {
    gtk_label_set_width_chars(GTK_LABEL(lbl), ft8_col_min_chars(vc));
  }
  gtk_list_item_set_child(li, lbl);
}
static void ft8_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u) {
  int vc = GPOINTER_TO_INT(u);
  Ft8Item *it = gtk_list_item_get_item(li);
  GtkWidget *lbl = gtk_list_item_get_child(li);
  char buf[64];
  const char *text = "";
  switch (vc) {
    case VCOL_UTC:  text = it->utc ? it->utc : "";                    break;
    case VCOL_DB:   snprintf(buf, sizeof buf, "%d", it->db);  text = buf; break;
    case VCOL_DT:   text = it->dt ? it->dt : "";                      break;
    case VCOL_FREQ: snprintf(buf, sizeof buf, "%d", it->freq); text = buf; break;
    case VCOL_MSG:  text = it->msg ? it->msg : "";                    break;
  }
  gtk_label_set_text(GTK_LABEL(lbl), text);

  // Pango attributes: bold (to-me), strikethrough (worked-before), green CQ msg.
  PangoAttrList *al = pango_attr_list_new();
  if (it->tome) pango_attr_list_insert(al, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
  if (it->b4)   pango_attr_list_insert(al, pango_attr_strikethrough_new(TRUE));
  if (vc == VCOL_MSG && it->cq)
    pango_attr_list_insert(al, pango_attr_foreground_new(0x3333, 0xaaaa, 0x3333));
  gtk_label_set_attributes(GTK_LABEL(lbl), al);
  pango_attr_list_unref(al);

  // New-DXCC background: gold = new entity ever, blue = new on this band.
  gtk_widget_remove_css_class(lbl, "ft8-gold");
  gtk_widget_remove_css_class(lbl, "ft8-blue");
  if (it->bg)
    gtk_widget_add_css_class(lbl, strcmp(it->bg, "#b8860b") == 0 ? "ft8-gold" : "ft8-blue");

  // Country tooltip (from cty.dat), shown on any cell of the row.
  gtk_widget_set_tooltip_text(lbl, (it->country && it->country[0]) ? it->country : NULL);
}
static GtkColumnViewColumn *ft8_col(const char *title, int vc) {
  GtkListItemFactory *f = gtk_signal_list_item_factory_new();
  g_signal_connect(f, "setup", G_CALLBACK(ft8_setup), GINT_TO_POINTER(vc));
  g_signal_connect(f, "bind",  G_CALLBACK(ft8_bind), GINT_TO_POINTER(vc));
  GtkColumnViewColumn *col = gtk_column_view_column_new(title, f);
  // Only the Message column expands to absorb slack; the numeric columns keep
  // their fixed minimum width (set per-cell in ft8_setup) so they never squeeze.
  if (vc == VCOL_MSG) gtk_column_view_column_set_expand(col, TRUE);
  return col;
}

// Double-click a decode row: work that station. pos indexes the view's model.
static void row_activated(GtkColumnView *cv, guint pos, gpointer data) {
  Ft8Item *it = g_list_model_get_item(G_LIST_MODEL(list_sel), pos);   // owned
  if (it != NULL && it->callde && it->callde[0]) {
    FT8_DECODE d;
    memset(&d, 0, sizeof(d));
    snprintf(d.call_de, sizeof(d.call_de), "%s", it->callde);
    snprintf(d.extra, sizeof(d.extra), "%s", it->extra ? it->extra : "");
    snprintf(d.utc, sizeof(d.utc), "%s", it->utc ? it->utc : "");
    ft8_qso_answer(&d);
  }
  if (it) g_object_unref(it);
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
      Ft8Item *it = g_object_new(FT8_TYPE_ITEM, NULL);
      it->utc     = g_strdup(d[i].utc);
      it->db      = (int)d[i].snr;
      it->dt      = g_strdup(dt);
      it->freq    = (int)d[i].freq;
      it->msg     = g_strdup(d[i].text);
      it->tome    = tome;
      it->cq      = iscq;
      it->b4      = b4;
      it->bg      = bg ? g_strdup(bg) : NULL;
      it->country = g_strdup(country ? country : "");
      it->callde  = g_strdup(d[i].call_de);
      it->extra   = g_strdup(d[i].extra);
      g_list_store_append(store, it);
      g_object_unref(it);
    }
    // Cap the history: drop the oldest rows from the top.
    while (g_list_model_get_n_items(G_LIST_MODEL(store)) > MAX_ROWS)
      g_list_store_remove(store, 0);
    // Keep the newest decode in view (count in the model the view shows).
    guint vis = g_list_model_get_n_items(G_LIST_MODEL(list_sel));
    if (vis > 0)
      gtk_column_view_scroll_to(GTK_COLUMN_VIEW(view), vis - 1, NULL,
                                GTK_LIST_SCROLL_NONE, NULL);
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
  store = NULL; filter_model = NULL; cq_filter = NULL; list_sel = NULL;
  view = NULL; status_label = NULL; dx_label = NULL;
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
  const char *proto_opts[] = {"FT8","FT4",NULL};
  GtkWidget *mode = gtk_drop_down_new_from_strings(proto_opts);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(mode), radio->ft8_proto ? 1 : 0);
  gtk_widget_set_tooltip_text(mode, "Digital protocol: FT8 (15 s) or FT4 (7.5 s)");
  g_signal_connect(mode, "notify::selected", G_CALLBACK(proto_changed), NULL);
  gtk_box_append(GTK_BOX(cfg),mode);

  gtk_box_append(GTK_BOX(cfg),gtk_label_new("Tx Hz"));
  offset_spin = gtk_spin_button_new_with_range(200, 2800, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(offset_spin), radio->ft8_tx_offset);
  g_signal_connect(offset_spin, "value-changed", G_CALLBACK(offset_changed), NULL);
  gtk_box_append(GTK_BOX(cfg),offset_spin);

  const char *slot_opts[] = {"Even","Odd",NULL};
  GtkWidget *slot = gtk_drop_down_new_from_strings(slot_opts);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(slot), radio->ft8_tx_even ? 0 : 1);
  g_signal_connect(slot, "notify::selected", G_CALLBACK(slot_changed), NULL);
  gtk_box_append(GTK_BOX(cfg),slot);

  // Directed CQ: blank = plain CQ, or pick/type a region (DX/EU/NA/…) or 3 digits.
  gtk_box_append(GTK_BOX(cfg),gtk_label_new("CQ"));
  GtkWidget *cqdir = gtk_entry_new();
  gtk_editable_set_width_chars(GTK_EDITABLE(cqdir), 4);
  gtk_entry_set_max_length(GTK_ENTRY(cqdir), sizeof(radio->ft8_cq_dir) - 1);
  gtk_entry_set_placeholder_text(GTK_ENTRY(cqdir), "CQ");
  gtk_editable_set_text(GTK_EDITABLE(cqdir), radio->ft8_cq_dir);
  gtk_widget_set_tooltip_text(cqdir,
      "Directed CQ: blank = CQ; a region (DX/EU/NA/SA/AS/AF/OC) or 3 digits");
  g_signal_connect(cqdir, "changed", G_CALLBACK(cq_dir_changed), NULL);
  gtk_box_append(GTK_BOX(cfg),cqdir);

  gtk_box_append(GTK_BOX(box),cfg);

  // (The FT8 band waterfall is placed to the RIGHT of the main RX spectrum, not
  // in this panel — see receiver_ft8_waterfall_sync().)

  // --- decode list ---
  ft8_install_css();
  store = g_list_store_new(FT8_TYPE_ITEM);
  cq_filter = GTK_FILTER(gtk_custom_filter_new(cq_match, NULL, NULL));
  filter_model = gtk_filter_list_model_new(G_LIST_MODEL(store), cq_filter);  // takes store+filter
  list_sel = GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(filter_model)));  // takes filter_model
  view = gtk_column_view_new(list_sel);   // takes list_sel
  g_signal_connect(view, "activate", G_CALLBACK(row_activated), NULL);

  gtk_column_view_append_column(GTK_COLUMN_VIEW(view), ft8_col("UTC",     VCOL_UTC));
  gtk_column_view_append_column(GTK_COLUMN_VIEW(view), ft8_col("dB",      VCOL_DB));
  gtk_column_view_append_column(GTK_COLUMN_VIEW(view), ft8_col("DT",      VCOL_DT));
  gtk_column_view_append_column(GTK_COLUMN_VIEW(view), ft8_col("Hz",      VCOL_FREQ));
  gtk_column_view_append_column(GTK_COLUMN_VIEW(view), ft8_col("Message", VCOL_MSG));

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
