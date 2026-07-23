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
#include <math.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <semaphore.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <wdsp.h>

#include "log.h"
#include "css.h"
#include "discovery.h"
#include "fake_protocol.h"
#include "discovered.h"
#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "main.h"
#include "audio.h"
#include "protocol1.h"
#include "protocol2.h"
#ifdef SOAPYSDR
#include "soapy_protocol.h"
#endif
#include "property.h"
#include "rigctl.h"
#include "version.h"
#ifdef FT8
#include "ft8_decoder.h"
#include "ft8_qso.h"
#include "ft8_dxcc.h"
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

GtkWidget *main_window;
static GtkWidget *grid;

static sem_t *wisdom_sem;
static GThread *wisdom_thread_id;

static GListStore *store;
static GtkWidget *view;
static gulong selection_signal_id;
static GtkWidget *none_found;
static GtkWidget *start;
static GtkWidget *retry;
static GtkWidget *image;   // splash image (GTK4: no wrapping event box needed)

static DISCOVERED *d=NULL;

RADIO *radio;
gboolean opengl=FALSE;

enum {
  NAME_COLUMN,
  VERSION_COLUMN,
  PROTOCOL_COLUMN,
  IP_COLUMN,
  MAC_COLUMN,
  INTERFACE_COLUMN,
  STATUS_COLUMN,
  N_COLUMNS
};

gboolean main_delete (GtkWidget *widget) {
  if(radio!=NULL) {
    radio_save_state(radio);
    switch(radio->discovered->protocol) {
      case PROTOCOL_1:
        protocol1_stop();
        break;
      case PROTOCOL_2:
        protocol2_stop();
        break;
#ifdef SOAPYSDR
      case PROTOCOL_SOAPYSDR:
        soapy_protocol_stop();
        break;
#endif
    }
    audio_close_input(radio);
    //audio_close_output(radio);
  }
  _exit(0);
}

// Action invoked by the "Quit" menu item / Cmd-Q (macOS) or Ctrl-Q accelerator.
static void quit_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
  main_delete(NULL);
}

static gpointer wisdom_thread(gpointer arg) {
log_info("Creating wisdom file: %s\n", (char *)arg);
  WDSPwisdom ((char *)arg);
  sem_post(wisdom_sem);
  return NULL;
}

// GTK4: GtkTreeView/GtkListStore are deprecated. The device list is a
// GtkColumnView over a GListStore of DeviceItem GObjects; each item carries the
// discovered[] index, so selection maps straight back to the device (no more
// string-matching to recover the row).
#define DEVICE_TYPE_ITEM (device_item_get_type())
G_DECLARE_FINAL_TYPE(DeviceItem, device_item, DEVICE, ITEM, GObject)
struct _DeviceItem {
  GObject parent_instance;
  char *col[N_COLUMNS];   // display strings, indexed by *_COLUMN
  int index;              // index into discovered[]
};
G_DEFINE_TYPE(DeviceItem, device_item, G_TYPE_OBJECT)
static void device_item_finalize(GObject *o) {
  DeviceItem *it = DEVICE_ITEM(o);
  for(int c=0;c<N_COLUMNS;c++) g_free(it->col[c]);
  G_OBJECT_CLASS(device_item_parent_class)->finalize(o);
}
static void device_item_class_init(DeviceItemClass *k){ G_OBJECT_CLASS(k)->finalize = device_item_finalize; }
static void device_item_init(DeviceItem *it){ }

// One shared setup (a left-aligned label) + a bind that reads the column stored
// on the factory as object data.
static void dev_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u) {
  GtkWidget *lbl = gtk_label_new(NULL);
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_list_item_set_child(li, lbl);
}
static void dev_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer u) {
  int c = GPOINTER_TO_INT(u);
  DeviceItem *it = gtk_list_item_get_item(li);
  const char *s = (it && it->col[c]) ? it->col[c] : "";
  gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), s);
}
static GtkColumnViewColumn *dev_col(const char *title, int colid) {
  GtkListItemFactory *f = gtk_signal_list_item_factory_new();
  g_signal_connect(f,"setup",G_CALLBACK(dev_setup),NULL);
  g_signal_connect(f,"bind",G_CALLBACK(dev_bind),GINT_TO_POINTER(colid));
  return gtk_column_view_column_new(title, f);
}

static GtkSingleSelection *dev_selection;   // owns the model; kept for teardown

static void dev_selection_changed(GObject *sel, GParamSpec *ps, gpointer data) {
  DeviceItem *it = gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(sel));
  if(it==NULL) { d=NULL; return; }
  d = &discovered[it->index];
  switch(d->status) {
    case STATE_AVAILABLE: gtk_widget_set_sensitive(start, TRUE);  break;
    case STATE_SENDING:   gtk_widget_set_sensitive(start, FALSE); break;
  }
}

gboolean start_cb(GtkWidget *widget,gpointer data);  /* defined below; called for --faker */

static int discover(void *data) {
  char v[32];
  char mac[32];
  char protocol[32];
  char ip[32];
  char iface[32];
  gint i;

  discovery();
  log_info("main: discovery found %d devices\n",devices);

  if(devices>0) {
    store=g_list_store_new(DEVICE_TYPE_ITEM);
    dev_selection=gtk_single_selection_new(G_LIST_MODEL(store));   // takes the store ref
    gtk_single_selection_set_autoselect(dev_selection, FALSE);
    gtk_single_selection_set_can_unselect(dev_selection, TRUE);
    view=gtk_column_view_new(GTK_SELECTION_MODEL(dev_selection));  // takes the selection ref
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), dev_col("Device",   NAME_COLUMN));
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), dev_col("Protocol", PROTOCOL_COLUMN));
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), dev_col("Version",  VERSION_COLUMN));
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), dev_col("IP",       IP_COLUMN));
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), dev_col("MAC",      MAC_COLUMN));
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), dev_col("IFACE",    INTERFACE_COLUMN));
    gtk_column_view_append_column(GTK_COLUMN_VIEW(view), dev_col("Status",   STATUS_COLUMN));


    for(i=0;i<devices;i++) {
      d=&discovered[i];

log_info("discovered: %d device=%d\n",i,discovered[i].device);

      switch(d->device) {
#ifdef SOAPYSDR
        case DEVICE_SOAPYSDR:
          if(strcmp(d->name,"rtlsdr")==0) {
            sprintf(mac,"%d",d->info.soapy.rtlsdr_count);
          } else {
            strcpy(mac,"");
          }
          strcpy(ip,d->info.soapy.address);
          strcpy(iface,"");
          break;
#endif
        default:
          sprintf(mac,"%02X:%02X:%02X:%02X:%02X:%02X",
            d->info.network.mac_address[0],
            d->info.network.mac_address[1],
            d->info.network.mac_address[2],
            d->info.network.mac_address[3],
            d->info.network.mac_address[4],
            d->info.network.mac_address[5]);
          strcpy(ip,inet_ntoa(d->info.network.address.sin_addr));
          strcpy(iface,d->info.network.interface_name);
          break;
      }

      if(d->protocol==PROTOCOL_1) {
        strcpy(protocol,"1");
      } else if(d->protocol==PROTOCOL_2) {
        strcpy(protocol,"2");
#ifdef SOAPYSDR
      } else if(d->protocol==PROTOCOL_SOAPYSDR) {
        strcpy(protocol,"SoapySDR");
#endif
      } else {
        strcpy(protocol,"UNKNOWN");
      }


log_info("adding %s\n",d->name);
      DeviceItem *it = g_object_new(DEVICE_TYPE_ITEM, NULL);
      it->index = i;
      it->col[NAME_COLUMN]      = g_strdup(d->name);
      it->col[PROTOCOL_COLUMN]  = g_strdup(protocol);
      it->col[VERSION_COLUMN]   = g_strdup(d->software_version);
      it->col[IP_COLUMN]        = g_strdup(ip);
      it->col[MAC_COLUMN]       = g_strdup(mac);
      it->col[INTERFACE_COLUMN] = g_strdup(iface);
      it->col[STATUS_COLUMN]    = g_strdup(d->status==2?"Idle":"In Use");
      g_list_store_append(store, it);
      g_object_unref(it);   // the store holds the ref
    }

    gtk_grid_attach(GTK_GRID(grid), view, 1, 0, 4, 1);
    selection_signal_id=g_signal_connect(dev_selection,"notify::selected",
                                         G_CALLBACK(dev_selection_changed),NULL);
    gtk_single_selection_set_selected(dev_selection, 0);   // preselect the first row

  } else {
    gtk_widget_set_sensitive(start, FALSE);
    none_found=gtk_label_new("No HPSDR devices found");
    gtk_grid_attach(GTK_GRID(grid), none_found, 1, 0, 4, 1);
  }

  // --faker: skip the device-selection dialog entirely. Realize the window,
  // build the fake radio straight into the grid, then reveal the fully-built
  // radio window — the selection UI is never shown.  The faker is appended
  // last in discovered[] (see fake_discovery), so when real devices are also
  // present it is NOT row 0; explicitly select the PROTOCOL_FAKE row rather
  // than opening whatever happened to be discovered first.  Non-faker runs
  // show the selection window as before.
  if(enable_fake && devices>0) {
    for(i=0;i<devices;i++) {
      if(discovered[i].protocol==PROTOCOL_FAKE) {
        gtk_single_selection_set_selected(dev_selection, i);
        break;
      }
    }
    gtk_widget_realize(main_window);
    start_cb(NULL,NULL);
  }

  gtk_widget_set_visible(main_window, TRUE);

  gtk_widget_set_cursor_from_name(main_window, "default");

  return 0;
}

static gboolean wisdom_delete(GtkWidget *widget) {
  _exit(0);
}

static int check_wisdom(void *data) {
  char wisdom_directory[1024];
  char wisdom_file[1048];
  GtkWidget *dialog;
  char label[128];

  sprintf(wisdom_directory,"%s/.local/share/machpsdr/",g_get_home_dir());
  snprintf(wisdom_file, sizeof(wisdom_file), "%swdspWisdom", wisdom_directory);
  if(access(wisdom_file,F_OK)<0) {
#ifdef __APPLE__
      wisdom_sem=sem_open("wisdomsem",O_CREAT,0700,0);
#else
      wisdom_sem=malloc(sizeof(sem_t));
      sem_init(wisdom_sem, 0, 0);
#endif
      wisdom_thread_id = g_thread_new( "Wisdom", wisdom_thread, (gpointer)wisdom_directory);
      if( ! wisdom_thread_id ) {
        log_info("g_thread_new failed for wisdom_thread\n");
        exit( -1 );
      }

      dialog=gtk_window_new();
      g_signal_connect (dialog, "close-request", G_CALLBACK (wisdom_delete), NULL);
      gtk_window_set_title(GTK_WINDOW(dialog),"MacHPSDR: Creating FFTW3 wisdom file");
      GtkWidget *grid=gtk_grid_new();
      gtk_grid_set_row_spacing(GTK_GRID(grid),10);
      GtkWidget *info=gtk_label_new("               Optimizing FFT sizes through 262145:               ");
      gtk_grid_attach(GTK_GRID(grid),info,0,0,1,1);
      GtkWidget *text=gtk_label_new("                         ");
      gtk_grid_attach(GTK_GRID(grid),text,0,1,1,1);
      GtkWidget *patient=gtk_label_new("(Please be patient. This will take several minutes.)");
      gtk_grid_attach(GTK_GRID(grid),patient,0,2,1,1);
      gtk_window_set_child(GTK_WINDOW(dialog),grid);
      gtk_widget_set_visible(dialog, TRUE);
      while(sem_trywait(wisdom_sem)<0) {
        sprintf(label,"          %s          ",wisdom_get_status());
        gtk_label_set_label(GTK_LABEL(text),label);
        while (g_main_context_pending(NULL))
          g_main_context_iteration(NULL, FALSE);
        usleep(100000); // 100ms
      }
      gtk_window_destroy(GTK_WINDOW(dialog));
  }
  g_idle_add(discover,NULL);
  return 0;
}

gboolean retry_cb(GtkWidget *widget,gpointer data) {
  gtk_widget_set_cursor_from_name(main_window, "wait");
  if(view!=NULL) {
    if(dev_selection!=NULL) g_signal_handler_disconnect(dev_selection,selection_signal_id);
    gtk_grid_remove(GTK_GRID(grid),view);
    view=NULL;
    dev_selection=NULL;
  }
  if(none_found!=NULL) {
    gtk_grid_remove(GTK_GRID(grid),none_found);
    none_found=NULL;
  }
  g_idle_add(discover,NULL);
  return TRUE;
}

gboolean start_cb(GtkWidget *widget,gpointer data) {
  char v[32];
  char mac[32];
  char ip[32];
  char iface[128];
  char protocol[32];
  gchar title[128];
  char *value;
  gint x=-1;
  gint y=-1;

  if(d!=NULL && d->status==STATE_AVAILABLE) {
    switch(d->device) {
#ifdef SOAPYSDR
      case DEVICE_SOAPYSDR:
        if(strcmp(d->name,"rtlsdr")==0) {
          g_snprintf(mac,sizeof(mac),"%d",d->info.soapy.rtlsdr_count);
        } else {
          strcpy(mac,"");
        }
        strcpy(ip,d->info.soapy.address);
        strcpy(protocol,"Soapy");
        strcpy(iface,"");
        break;
#endif
      default:
        g_snprintf(mac,sizeof(mac),"(%02X:%02X:%02X:%02X:%02X:%02X)",
          d->info.network.mac_address[0],
          d->info.network.mac_address[1],
          d->info.network.mac_address[2],
          d->info.network.mac_address[3],
          d->info.network.mac_address[4],
          d->info.network.mac_address[5]);
        strcpy(ip,inet_ntoa(d->info.network.address.sin_addr));
        if(d->protocol==0) {
          strcpy(protocol,"P1");
        } else {
          strcpy(protocol,"P2");
        }
        snprintf(iface, sizeof(iface), "on %s", d->info.network.interface_name);
        break;
    }
    g_snprintf((gchar *)&title,sizeof(title),"MacHPSDR (%s, %s): %s %s %s %s %s %s",
      version,
      build_date,
      d->name,
      protocol,
      d->software_version,
      ip,
      mac,
      iface);

    log_info("starting %s\n",title);
    gtk_widget_set_cursor_from_name(main_window, "wait");
    gtk_widget_set_name(main_window,"receiver-window");
    gtk_window_set_title(GTK_WINDOW (main_window),title);
    while(g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);

    radio=create_radio(d);
    gtk_grid_remove(GTK_GRID(grid),view);
    gtk_grid_remove(GTK_GRID(grid),start);
    gtk_grid_remove(GTK_GRID(grid),retry);
    gtk_grid_remove(GTK_GRID(grid),image);
    gtk_grid_attach(GTK_GRID(grid), radio->visual, 0, 0, 5, 1);
    // Breathing room between the window titlebar and the first VFO button row.
    gtk_widget_set_margin_top(radio->rx_container, 8);
    gtk_grid_attach(GTK_GRID(grid), radio->rx_container, 0, 1, 5, 1);

    // Double-line divider between the RX stack and the bottom control panel
    // (two thin horizontal lines with a small gap; distinct from the vertical
    // gradient separator inside the bottom bar).
    GtkWidget *rx_bottom_sep=gtk_box_new(GTK_ORIENTATION_VERTICAL,3);
    gtk_widget_set_name(rx_bottom_sep,"rx-bottom-sep");
    gtk_box_append(GTK_BOX(rx_bottom_sep),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(rx_bottom_sep),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_grid_attach(GTK_GRID(grid), rx_bottom_sep, 0, 2, 5, 1);

    gtk_grid_attach(GTK_GRID(grid), radio->bottom_bar, 0, 3, 5, 1);

#ifdef FT8
    // If the radio restored a receiver already in DIGU, show the FT8 panel now
    // (the global `radio` and its rx_container exist only at this point).
    radio_ft8_panel_sync(radio);
#endif

    //launch_rigctl(radio);

    // GTK4 removed client-side window positioning (gtk_window_move): the
    // compositor owns placement, so radio.x/radio.y can no longer be restored.
    // (void) them to keep the persisted values harmless.
    value=getProperty("radio.x");
    if(value!=NULL) x=atoi(value);
    value=getProperty("radio.y");
    if(value!=NULL) y=atoi(value);
    (void)x; (void)y;

    int win_w=-1, win_h=-1;
    value=getProperty("radio.width");
    if(value!=NULL) win_w=atoi(value);
    value=getProperty("radio.height");
    if(value!=NULL) win_h=atoi(value);
    if(win_w>0 && win_h>0) {
      gtk_window_set_default_size(GTK_WINDOW(main_window),win_w,win_h);
    }

    gtk_widget_set_cursor_from_name(main_window, "default");

  }
  return TRUE;
}

static void activate_hpsdr(GtkApplication *app, gpointer data) {
  struct utsname unameData;
  char title[64];
  char png_path[256];

  log_info("Build: %s %s\n",build_date,version);
  log_info("GTK version %d.%d.%d\n", gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());
  uname(&unameData);
  log_info("sysname: %s\n",unameData.sysname);
  log_info("nodename: %s\n",unameData.nodename);
  log_info("release: %s\n",unameData.release);
  log_info("version: %s\n",unameData.version);
  log_info("machine: %s\n",unameData.machine);

  load_css();

  GdkDisplay *display=gdk_display_get_default();
  if(display==NULL) {
    log_info("HPSDR: no default display!\n");
    _exit(0);
  }

#ifdef OPENGL
  GtkWidget *opengl_widget=gtk_gl_area_new();
  opengl=opengl_widget!=NULL;
  if(opengl_widget!=NULL) {
    g_object_ref_sink(opengl_widget);
    g_object_unref(opengl_widget);
  }
#endif
  log_info("opengl: %d\n",opengl);

#ifdef __APPLE__
  // Load the window icon from the .app bundle (Contents/Resources), or from
  // the current directory when running ./machpsdr straight from the repo.
  // Never read it from a system-wide location on macOS.
  char exe_path[1024];
  uint32_t size = sizeof(exe_path);
  png_path[0] = '\0';
  if (_NSGetExecutablePath(exe_path, &size) == 0) {
    char *last_slash = strrchr(exe_path, '/');
    if (last_slash) {
      *last_slash = '\0';
      snprintf(png_path, sizeof(png_path), "%s/../Resources/machpsdr.png", exe_path);
    }
  }
  if (png_path[0] == '\0' || access(png_path, F_OK) != 0) {
    strcpy(png_path, "machpsdr.png");   // local fallback (running from the repo)
  }
  log_info("PNG path (macOS): %s\n", png_path);
#else
  // Prefer an icon next to the binary / in the working directory; fall back to
  // an installed copy under /usr/share only if there is no local one.
  if (access("machpsdr.png", F_OK) == 0) {
    strcpy(png_path, "machpsdr.png");
  } else {
    strcpy(png_path, "/usr/share/machpsdr/machpsdr.png");
  }
#endif

  main_window = gtk_application_window_new (app);
  snprintf(title,sizeof(title),"MacHPSDR (%s, %s)",version,build_date);
  gtk_window_set_title (GTK_WINDOW (main_window), title);
  gtk_window_set_resizable(GTK_WINDOW(main_window), TRUE);
  // GTK4 removed gtk_window_set_icon_from_file (icons come from the icon theme).
  // The .app bundle carries the dock icon; the themed name below is a best-effort
  // fallback for a window icon and harmlessly no-ops if the theme lacks it.
  (void)png_path;
  gtk_window_set_icon_name(GTK_WINDOW(main_window), "machpsdr");

  g_signal_connect (main_window, "close-request", G_CALLBACK (main_delete), NULL);

  // GTK4: key events come from a controller attached to the window, not from
  // "key-press-event"/"key-release-event" signals (removed).
  GtkEventController *keys=gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed",  G_CALLBACK(receiver_key_pressed),  NULL);
  g_signal_connect(keys, "key-released", G_CALLBACK(receiver_key_released), NULL);
  gtk_widget_add_controller(main_window, keys);

  grid = gtk_grid_new();
  //gtk_widget_set_size_request(grid, 800, 480);
  //gtk_grid_set_row_homogeneous(GTK_GRID(grid),TRUE);
  //gtk_grid_set_column_homogeneous(GTK_GRID(grid),FALSE);

  // Splash image (GTK4: GtkPicture renders a file at natural size; GtkImage
  // would clamp it to icon size. No GtkEventBox — it caught no events here.)
  image=gtk_picture_new_for_filename(png_path);
  gtk_grid_attach(GTK_GRID(grid), image, 0, 0, 1, 1);

  gtk_window_set_child (GTK_WINDOW (main_window), grid);

  retry=gtk_button_new_with_label("Retry Discovery");
  g_signal_connect(retry,"clicked",G_CALLBACK(retry_cb),NULL);
  gtk_grid_attach(GTK_GRID(grid), retry, 1, 1, 1, 1);

  start=gtk_button_new_with_label("Start Radio");
  g_signal_connect(start,"clicked",G_CALLBACK(start_cb),NULL);
  gtk_grid_attach(GTK_GRID(grid), start, 4, 1, 1, 1);

  //gtk_widget_set_visible(main_window, TRUE);

  g_idle_add(check_wisdom,NULL);

}

int main(int argc, char **argv) {
  GtkApplication *hpsdr;
  char text[1024];
  int rc;
  const char *homedir;

  // Log verbosity: environment first (MACHPSDR_LOG=debug|info|error), then the
  // command line below can override it. Default stays INFO (see log.c).
  {
    const char *env=getenv("MACHPSDR_LOG");
    if(env!=NULL && log_set_level_name(env)!=0) {
      log_error("unknown MACHPSDR_LOG value '%s' (use error|info|debug)\n",env);
    }
  }

  // --faker: offer the synthetic fake test device (see fake_protocol.c).
  // Strip it from argv so GtkApplication does not reject the unknown option.
  {
    int i, j;
    for(i=1,j=1;i<argc;i++) {
      if(strcmp(argv[i],"--log-level")==0 && i+1<argc) {
        // --log-level <error|info|debug>
        if(log_set_level_name(argv[++i])!=0)
          log_error("unknown --log-level '%s' (use error|info|debug)\n",argv[i]);
      } else if(strncmp(argv[i],"--log-level=",12)==0) {
        // --log-level=<error|info|debug>
        if(log_set_level_name(argv[i]+12)!=0)
          log_error("unknown --log-level '%s' (use error|info|debug)\n",argv[i]+12);
      } else if(strcmp(argv[i],"--debug")==0) {
        log_set_level(LOG_LEVEL_DEBUG);
      } else if(strcmp(argv[i],"--verbose")==0 || strcmp(argv[i],"-v")==0) {
        log_set_level(LOG_LEVEL_DEBUG);
      } else if(strcmp(argv[i],"--quiet")==0 || strcmp(argv[i],"-q")==0) {
        log_set_level(LOG_LEVEL_ERROR);
      } else if(strcmp(argv[i],"--faker")==0) {
        enable_fake=1;
        // Optional following argument: the I/Q WAV to loop (e.g. --faker ft8.wav).
        // Only consume it if it is not another option.
        if(i+1<argc && argv[i+1][0]!='-') {
          fake_iq_file=argv[++i];
        }
      } else if(strcmp(argv[i],"--revert-iq")==0) {
        // Swap the faker's I/Q (mirror the spectrum) for inverted-sideband
        // recordings whose image is not suppressed / that decode as a mirror.
        fake_revert_iq=1;
      } else if(strcmp(argv[i],"--usb-only")==0) {
        // Skip the blocking Protocol 1/2 network discovery; only enumerate
        // USB/SoapySDR devices for a near-instant startup.
        skip_network_discovery=TRUE;
      } else {
        argv[j++]=argv[i];
      }
    }
    argc=j;
    argv[argc]=NULL;
  }

  log_debug("log level = %s\n", log_level_name(log_get_level()));

#ifdef FT8
  // Load the DXCC country file first so the QSO log's worked-before scan can tag
  // worked entities as it reads the log.
  ft8_dxcc_init();
  // Spin up the FT8 decode worker thread (idle until a DIGU receiver feeds it).
  ft8_decoder_init();
  // Start the auto-QSO poll timer (idle until a QSO/CQ is started from the panel).
  ft8_qso_init();
#endif

  if((homedir=getenv("HOME"))==NULL) {
    homedir=getpwuid(getuid())->pw_dir;
  }
  sprintf(text,"%s/.local",homedir);
  rc=mkdir(text,0777);
  sprintf(text,"%s/.local/share",homedir);
  rc=mkdir(text,0777);

  // One-time migration: this fork was renamed from linhpsdr to machpsdr.
  // If the new config dir does not exist yet but the old one does, copy the
  // saved configuration across (radio props, bookmarks, MIDI mappings,
  // FFTW wisdom) so the rename does not lose the user's settings.
  {
    char newdir[1024], olddir[1024], cmd[2200];
    snprintf(newdir,sizeof(newdir),"%s/.local/share/machpsdr",homedir);
    snprintf(olddir,sizeof(olddir),"%s/.local/share/linhpsdr",homedir);
    if(access(newdir,F_OK)!=0 && access(olddir,F_OK)==0) {
      snprintf(cmd,sizeof(cmd),"cp -R \"%s\" \"%s\"",olddir,newdir);
      if(system(cmd)==0) {
        log_info("Migrated configuration from %s to %s\n",olddir,newdir);
      }
    }
  }

  sprintf(text,"%s/.local/share/machpsdr",homedir);
  rc=mkdir(text,0777);

  sprintf(text,"org.g0orx.hpsdr.pid%d",getpid());
  hpsdr=gtk_application_new(text, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(hpsdr, "activate", G_CALLBACK(activate_hpsdr), NULL);

  // Register app.quit so Cmd-Q (macOS) / Ctrl-Q performs a clean shutdown.
  GSimpleAction *quit=g_simple_action_new("quit", NULL);
  g_signal_connect(quit, "activate", G_CALLBACK(quit_action), NULL);
  g_action_map_add_action(G_ACTION_MAP(hpsdr), G_ACTION(quit));
  g_object_unref(quit);
  const char *quit_accels[]={"<Primary>q", NULL};
  gtk_application_set_accels_for_action(hpsdr, "app.quit", quit_accels);

  rc=g_application_run(G_APPLICATION(hpsdr), argc, argv);
  g_object_unref(hpsdr);
  return rc;
}
