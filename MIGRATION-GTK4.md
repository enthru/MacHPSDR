# GTK3 → GTK4 migration (branch `gtk4-migration`)

Working notes / progress tracker for porting MacHPSDR from GTK3 to GTK4.
This branch is **GTK4-only** (no dual-build): the API changes (pack_start
signature, removed menus/EventBox, drawing model) are too pervasive to `#ifdef`.
`master` stays on GTK3.

---
## ▶ RESUME HERE (state as of the Phase-3 .app-bundle commit)

**Where we are:** Phase 1 + runtime check-button/signal fixups + **Phase 3 (.app
bundle)** all done — the tree builds, links, launches clean on GTK 4.22 (0 GTK
criticals under `G_DEBUG=fatal-warnings`), and `make app` produces a working
self-contained `MacHPSDR.app`. Branch `gtk4-migration`. Requires `brew install
gtk4` (4.22 installed). Build/run to confirm:

```bash
make                                   # clean arm64 machpsdr against gtk4
./machpsdr --faker ft4.wav --usb-only  # launches end-to-end (no hardware)
make app && open MacHPSDR.app          # self-contained GTK4 bundle
```

**DONE this pass:**
1. ✅ **Runtime check-button fix.** Converted every `gtk_toggle_button_*_active`
   on an actual `GtkCheckButton` → `gtk_check_button_*_active`/`GTK_CHECK_BUTTON`.
   Method: files with **no** `gtk_toggle_button_new` (all check buttons) were
   blanket-converted; the mixed files were done per-variable. Real toggle buttons
   kept the old API: **vfo.c** (16 `gtk_toggle_button_new` — VFO ctl bar),
   **radio_info.c** (7 — status lamps), **radio.c** (mox/vox/tune + preamp/att10/
   att20 bottom-bar toggles), **ft8_panel.c** `enable_btn` ("Enable Tx"). Check
   buttons fixed in: eer/diversity/midi/ft8_dialog/oc/pa/puresignal/recorder/
   receiver_dialog/radio_dialog/transmitter_dialog/wideband_dialog/xvtr_dialog +
   panels wefax_panel (also fixed its `GtkToggleButton *b` cb sigs) and ft8_panel
   (`auto_chk`, `cqchk`).
2. ✅ **Grouped radio→check "pressed"→"toggled".** receiver_dialog grouped
   selectors (deviation/filter/adc/sample_rate) now connect `"toggled"` with an
   `if(!gtk_check_button_get_active(...)) return;` guard so only the newly-active
   button acts. radio_dialog's grouped ptt_ring/tip cbs already guarded.
3. ✅ **GtkButton "pressed"→"clicked".** GTK4 removed `"pressed"`/`"released"` from
   GtkButton (moved to GtkGestureClick). Momentary buttons converted to
   `"clicked"`: midi_dialog add/update/delete; vfo.c a2b/b2a/aswapb/zoom_b/step_b.
   (vfo.c:93 `"pressed"` is on a GtkGestureClick — left as-is.)

**DONE — Phase 3 (.app bundle):** the Makefile `app` target now builds a working
self-contained GTK4 `MacHPSDR.app` (62 MB, 47 bundled frameworks, `libgtk-4.1.dylib`,
no gtk-3 leak). Changes: `lib/gtk-3.0` module copy → `lib/gtk-4.0` (GTK4 statically
links its backends, so it's normally a no-op — kept for future dlopen'd modules);
dropped the obsolete `GTK_IM_MODULE_FILE`/immodules.cache export (GTK4 has none);
`etc/gtk-3.0/settings.ini` → `etc/gtk-4.0`. gdk-pixbuf loaders + `org.gtk.gtk4.*`
schemas already handled. Verified: bundle launches self-contained via faker, no
dyld/schema/gtk errors.

**Also fixed two runtime bugs surfaced by the real (fatal-warnings) run** — not
check-button related:
- **vfo.c split_b**: the SPLIT/SAT/RSAT right-click menu callback was wired to the
  removed `"button_press_event"` (criticaled + menu never appeared). Now a
  GtkGestureClick on the **secondary** button (left-click stays with `"toggled"`).
- **radio.c create_radio**: `set_sensitive(add_receiver_b,…)` ran *before*
  `create_visual` created that button → `GTK_IS_WIDGET` assert on NULL. Moved the
  sensitivity update to after `create_visual`, with a NULL guard.
- **radio_info.c midi_b**: status-lamp toggle button opening Configure→MIDI was on
  `"button_press_event"` → now `"clicked"` (fires on user click, not the frequent
  programmatic `set_active`).
Faker run is now **0 GTK criticals**.

**TODO, in priority order:**
1. Sanity-drive the GUI by hand (VFO menus/popovers, sliders, right-click config,
   bookmark menu, file choosers, dialog checkboxes) — faker launch is clean but the
   fixes aren't all exercised interactively. `open MacHPSDR.app` or run the binary.
2. **Phase 2**: ComboBox→GtkDropDown (~318), TreeView/ListStore→GtkColumnView (~184)
   — deprecated-but-working today.
3. Optional: GtkSnapshot/GdkTexture GPU path for the waterfalls.

**Also revisit (deliberate Phase-1 simplifications):** vfo band right-click menu
flattened to headers (no submenus); ft8 waterfall not re-split live on resize;
window position not restored (gtk_window_move gone).

**Key infra added this migration (reuse these):** `gtk4_dialog_run()` shim in
ext.c (removed gtk_dialog_run); `child_remove_from_parent()` in radio.c (no
generic gtk_container_remove); vfo.c popover-menu macro shim + `vfo_attach_ctl()`
(EventBox replacement). Reusable GTK3→GTK4 replacement table is below.

---

## ✅ PHASE 1 COMPLETE — builds, links, and launches on GTK 4.22

`make` produces a clean `machpsdr` arm64 binary against `pkg-config gtk4`, and it
runs end-to-end under `--faker` (discovery → radio → **create_vfo with all the
migrated menus/controllers** → receiver → CoreAudio audio, no crash). All 0
compile errors across the tree (version.c/cwdaemon.c only errored in the isolated
syntax check — GIT_* defines come from the Makefile; cwdaemon is Linux-only).

### Remaining after Phase 1
- **Runtime fixups (compile-clean, wrong at runtime)** — do a dedicated pass:
  - `GtkCheckButton` is no longer a `GtkToggleButton`: every
    `gtk_toggle_button_get/set_active(GTK_TOGGLE_BUTTON(cb))` on a *check* button
    must become `gtk_check_button_get/set_active(GTK_CHECK_BUTTON(cb))`
    (pervasive across dialogs + panels).
  - `receiver_dialog` / `radio_dialog` grouped check-buttons connect `"pressed"`
    (a removed GtkButton signal) — should be `"toggled"` with an active-guard.
  - `vfo.c` band right-click menu was **flattened** (no submenus) — bands are
    non-clickable headers with entries below; revisit if a two-level
    `GtkPopoverMenu` is wanted.
  - ft8 waterfall no longer re-splits its 1/3 width live on window resize
    (`size-allocate` gone); sized once on open.
  - client-side window position (`gtk_window_move`) is gone — radio.x/y persist
    as -1 and are not restored (compositor owns placement).
- **Phase 2**: ComboBox → GtkDropDown (~318), TreeView/ListStore → GtkColumnView
  (~184) — currently deprecated-but-working under `-Wno-deprecated-declarations`.
- **Phase 3**: `.app` bundle (`app` target: gtk-3.0 → gtk-4.0 paths, drop im
  modules); optional GtkSnapshot/GdkTexture GPU path for the waterfalls.
- Verify on real hardware / full GUI interaction (only faker-launch tested).

## Strategy (agreed)

1. **Phase 1 — compile & RUN on GTK4.** Mechanical replacements + real reworks
   (event controllers, draw funcs + backing surfaces, `vfo.c` menus,
   `gtk_dialog_run`, EventBox, main.c init/loop). ComboBox/TreeView are left as
   **deprecated-but-working** here (suppressed by `-Wno-deprecated-declarations`,
   already in CFLAGS) so we reach a working binary as a checkpoint.
2. **Phase 2 — migrate ComboBox → `GtkDropDown`, TreeView → `GtkColumnView`**,
   one dialog at a time, on top of the already-running GTK4 app.
3. **Phase 3 — `.app` bundle** (gtk-4.0 paths) + optional GPU path
   (`GtkSnapshot`/`GdkTexture`) for the waterfalls.

## Reusable replacement patterns (GTK3 → GTK4)

| GTK3 | GTK4 |
|---|---|
| `gtk_container_add(GTK_CONTAINER(box), c)` | `gtk_box_append(GTK_BOX(box), c)` |
| `gtk_container_add(GTK_CONTAINER(win), c)` | `gtk_window_set_child(GTK_WINDOW(win), c)` |
| `gtk_container_add(scrolled, c)` | `gtk_scrolled_window_set_child(...)` |
| `gtk_container_remove(GTK_CONTAINER(grid), c)` | `gtk_grid_remove(GTK_GRID(grid), c)` |
| `gtk_container_remove(GTK_CONTAINER(box), c)` | `gtk_box_remove(GTK_BOX(box), c)` |
| `gtk_box_pack_start(box,c,exp,fill,pad)` | `gtk_box_append(box,c)` + `gtk_widget_set_hexpand/vexpand(c,exp)` + margins for pad |
| `gtk_widget_show_all(w)` | (children visible by default) toplevel: `gtk_widget_set_visible(w,TRUE)` / `gtk_window_present` |
| `gtk_widget_show(w)` | usually drop; toplevel `gtk_window_present` |
| `gtk_widget_destroy(w)` | `gtk_window_destroy(GTK_WINDOW(w))` (win) / remove from parent |
| `gdk_window_set_cursor(gtk_widget_get_window(w), gdk_cursor_new(GDK_ARROW))` | `gtk_widget_set_cursor_from_name(w, "default")` (WATCH→"wait"/"progress", CROSSHAIR→"crosshair", DOUBLE_ARROW→"ew-resize") |
| `"delete-event"` | `"close-request"` (window) |
| `"key-press-event"`/`"key-release-event"` | `GtkEventControllerKey` → `"key-pressed"`/`"key-released"` |
| `"button-press-event"` etc. | `GtkGestureClick` → `"pressed"`/`"released"`; motion → `GtkEventControllerMotion`; scroll → `GtkEventControllerScroll` |
| `gtk_widget_add_events`/`set_events` | drop (controllers replace event masks) |
| `GdkEventButton`/`Key`/`Motion`/`Scroll` in cbs | controller signal args (x,y,button,keyval,state) |
| `GtkEventBox` | plain `GtkBox`/widget + a gesture/controller |
| `gtk_events_pending()`/`gtk_main_iteration()` | `g_main_context_pending(NULL)`/`g_main_context_iteration(NULL,FALSE)` |
| `GdkScreen`/`gdk_screen_get_default` | `GdkDisplay`/`gdk_display_get_default` |
| `gtk_window_set_icon_from_file` | removed → `gtk_window_set_icon_name` (themed) or drop |
| `gtk_window_move` | removed (no client positioning) → drop |
| `gtk_window_resize` | `gtk_window_set_default_size` |
| `"draw"` signal + cairo | `gtk_drawing_area_set_draw_func(area, fn, data, NULL)` |
| `gdk_window_create_similar_surface` backing surface | `cairo_image_surface_create` (kept in the widget struct) |
| `gtk_menu_*` / `GtkMenuItem` | `GMenu` + `GtkPopoverMenu` + `GAction` (see vfo.c) |
| `gtk_dialog_run` | async: show + connect `"response"` |

## File-by-file status

Legend: ⬜ todo · 🟡 in progress · ✅ compiles on gtk4 · ✔️ verified at runtime

### Foundation
- ✅ Makefile — `gtk+-3.0` → `gtk4`
- ✅ main.c — init/loop, cursors, EventBox→GtkPicture, key controller, window
  icon/move/resize, GdkScreen→GdkDisplay, dialog close-request. **Compiles clean
  on gtk4 (0 errors).** Introduced the new controller-based handler signatures in
  the headers (implemented later): `receiver_pressed/released/motion/scroll_cb`,
  `receiver_key_pressed/released` (receiver.h); `wideband_pressed/released/motion/
  scroll_cb` (wideband.h); `radio_pressed_cb` (radio.h).

> NOTE: because the headers now declare the new (gtk4) handler signatures while
> the matching .c files still define the old ones, the tree does **not link**
> until those .c files are ported. That is expected mid-Phase-1; first green
> build == end of Phase 1.

### Display widgets (draw func + backing surface + event controllers)
- ✅ rx_panadapter.c · tx_panadapter.c · waterfall.c
- ✅ wideband_panadapter.c · wideband_waterfall.c  (⬜ wideband.c still)
- ✅ ft8_waterfall.c
- ✅ meter.c (also GtkMenu→GtkPopover) · tx_info_meter.c · mic_level.c · mic_gain.c · drive_level.c
- ✅ puresignal_dialog.c
  - Patterns nailed down here: `gdk_window_create_similar_surface`→`cairo_image_surface_create(CAIRO_FORMAT_RGB24,...)`; `"configure-event"`→DrawingArea `"resize"`; `"draw"` signal→`gtk_drawing_area_set_draw_func` (cb sig `(area,cr,w,h,data)`, returns void); button/motion/scroll→`GtkGestureClick`/`GtkEventControllerMotion`/`GtkEventControllerScroll`; enter/leave cursor→one `gtk_widget_set_cursor_from_name(w,"ew-resize")`; small right-click menu→`GtkPopover` of flat buttons.

### ⚠️ Known Phase-1 runtime fixups (compile clean, wrong at runtime)
- **GtkCheckButton is no longer a GtkToggleButton in GTK4.** `gtk_toggle_button_get/set_active(GTK_TOGGLE_BUTTON(cb))` on a *check* button now criticals + no-ops. Must become `gtk_check_button_get/set_active(GTK_CHECK_BUTTON(cb))`. Pervasive across every *_dialog.c — do a dedicated grep pass. (Real toggle buttons keep the old API.)
- Audit `gtk_widget_get_allocated_width/height` left in hot paths (deprecated-but-works; fine for now).

### Handler-body files
- ✅ receiver.c · wideband.c · transmitter.c (already clean; its gesture cb lives in tx_panadapter.c)
- ✅ css.c (GdkScreen→display, load_from_data→load_from_string) · button_text.c

### Menus (GMenu/popover)
- ✅ meter.c (done in display group)
- ⬜ vfo.c (**256 errors — the big one**, ~49 gtk_menu_new / 80 menu_item / popup_at_pointer) · bookmark_dialog.c (27)

### Remaining Phase-1 files (accurate compile-error counts)
vfo.c 256 · radio.c 46 · receiver_dialog.c 32 · bookmark_dialog.c 27 · xvtr_dialog.c 19 · midi_dialog.c 17 · recorder.c 17 · ft8_panel.c 15 · ft8_dialog.c 14 · labels_dialog.c 8 · sstv_panel.c 8 · css.c✅ · configure_dialog.c 6 · wefax_panel.c 5 · pa_dialog.c 4 · tx_info.c 3 · radio_dialog.c 7 · diversity/eer/transmitter/wideband_dialog/reconnect 2ea · oc_dialog/radio_info/error_handler 1ea
(version.c / cwdaemon.c error only in my standalone syntax check — GIT_* defines come from the Makefile; cwdaemon is Linux-only. Not real.)

### Dominant remaining patterns (mechanical unless noted)
- **Menus** (vfo.c, bookmark): `gtk_menu_*`→`GtkPopover`/GMenu (the real work)
- `gtk_entry_get_text`/`set_text` (~86) → `gtk_editable_get_text`/`set_text` (sed-able)
- `GTK_CONTAINER`/`gtk_container_add`/`remove` (~150) → parent-specific setters
- `gtk_widget_show_all`/`show` (~78) → drop
- `GdkEventButton/Scroll/Motion` handlers (~67) → controllers
- cursor `gdk_window_set_cursor` (~30) → `gtk_widget_set_cursor_from_name`
- `gtk_widget_destroy` (~30) → `gtk_window_destroy`
- `gtk_box_pack_start` (~28) → `gtk_box_append`
- `gtk_radio_button_*` (~25) → `GtkCheckButton` + `gtk_check_button_set_group`
- `gtk_dialog_run` (10) → async `response`
- `gtk_file_chooser_get_filename` (15) → `gtk_file_chooser_get_file` (GFile) + async
- `gtk_event_box_new` (9) → drop, controller on child

### Phase 2 (deprecated → new)
- ⬜ ComboBox → GtkDropDown (~318 sites)
- ⬜ TreeView/ListStore → GtkColumnView (~184 sites)

### Phase 3
- ⬜ `app` target in Makefile (gtk-4.0 bundle paths)
- ⬜ optional GtkSnapshot GPU path for waterfalls
