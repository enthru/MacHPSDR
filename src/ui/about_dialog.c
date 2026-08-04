/* Copyright (C)
* 2017 - John Melton, G0ORX/N6LYT
* 2024-2026 - Gleb Sushko (enthru)
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
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <wdsp.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef SOAPYSDR
#include <SoapySDR/Device.h>
#endif

#include "discovered.h"
#include "bpsk.h"
#include "receiver.h"
#include "transmitter.h"
#include "wideband.h"
#include "adc.h"
#include "dac.h"
#include "radio.h"
#include "version.h"
#include "settings_ui.h"
#include "about_dialog.h"

/*
 * One full-width text row inside a group grid. `markup` allows Pango markup.
 * `wrap` is FALSE for the short header lines: a wrapping label's height depends
 * on its width, and inside the header box that made GtkBox measure a natural
 * height below its minimum (a GTK warning), while the lines never wrap anyway.
 */
static void about_row_ex(GtkWidget *grid, int *row, const char *text, gboolean markup, gboolean wrap) {
  GtkWidget *label=gtk_label_new(NULL);
  if(markup) {
    gtk_label_set_markup(GTK_LABEL(label),text);
  } else {
    gtk_label_set_text(GTK_LABEL(label),text);
  }
  gtk_label_set_xalign(GTK_LABEL(label),0.0);
  gtk_label_set_justify(GTK_LABEL(label),GTK_JUSTIFY_LEFT);
  if(wrap) {
    gtk_label_set_wrap(GTK_LABEL(label),TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label),84);
  }
  gtk_label_set_selectable(GTK_LABEL(label),TRUE);
  gtk_grid_attach(GTK_GRID(grid),label,0,(*row)++,1,1);
}

static void about_row(GtkWidget *grid, int *row, const char *text, gboolean markup) {
  about_row_ex(grid,row,text,markup,TRUE);
}

/* Titled group with a single column of text rows, appended to the page. */
static GtkWidget *about_group(GtkWidget *page, int *page_row, const char *title) {
  GtkWidget *frame=gtk_frame_new(title);
  GtkWidget *grid=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(grid),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(grid),FALSE);
  sui_style_group(grid);
  gtk_frame_set_child(GTK_FRAME(frame),grid);
  gtk_grid_attach(GTK_GRID(page),frame,0,(*page_row)++,1,1);
  return grid;
}

/* The app icon, looked up the same way main.c looks it up for the window. */
static GtkWidget *about_logo(void) {
  char path[1024];

  path[0]='\0';
#ifdef __APPLE__
  char exe_path[1024];
  uint32_t size=sizeof(exe_path);
  if(_NSGetExecutablePath(exe_path,&size)==0) {
    char *last_slash=strrchr(exe_path,'/');
    if(last_slash) {
      *last_slash='\0';
      snprintf(path,sizeof(path),"%s/../Resources/machpsdr.png",exe_path);
      if(access(path,F_OK)!=0) path[0]='\0';
    }
  }
#endif
  if(path[0]=='\0') {
    if(access("assets/machpsdr.png",F_OK)==0) {
      strcpy(path,"assets/machpsdr.png");
    } else if(access("machpsdr.png",F_OK)==0) {
      strcpy(path,"machpsdr.png");
    } else if(access("/usr/share/machpsdr/machpsdr.png",F_OK)==0) {
      strcpy(path,"/usr/share/machpsdr/machpsdr.png");
    } else {
      return NULL;
    }
  }

  GtkWidget *image=gtk_image_new_from_file(path);
  gtk_image_set_pixel_size(GTK_IMAGE(image),72);
  gtk_widget_set_valign(image,GTK_ALIGN_START);
  return image;
}

GtkWidget *create_about_dialog(RADIO *r) {
  char text[2048];
  char addr[64];
  char interface_addr[64];
  const char *protocol="Protocol: unknown";
  int row;

  GtkWidget *page=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(page),FALSE);
  gtk_grid_set_column_homogeneous(GTK_GRID(page),FALSE);
  sui_style_page(page);

  int page_row=0;

  /* Header: icon + name/tagline, no frame (it is the page title). */
  GtkWidget *header=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);
  GtkWidget *logo=about_logo();
  if(logo!=NULL) gtk_box_append(GTK_BOX(header),logo);

  GtkWidget *titles=gtk_grid_new();
  gtk_grid_set_row_homogeneous(GTK_GRID(titles),FALSE);
  row=0;
  about_row_ex(titles,&row,"<span size=\"xx-large\" weight=\"bold\">MacHPSDR</span>",TRUE,FALSE);
  about_row_ex(titles,&row,
               "<span size=\"small\">A GTK4 control application for HPSDR, Hermes-Lite 2 "
               "and SoapySDR receivers — macOS and Linux.</span>",TRUE,FALSE);
  snprintf(text,sizeof(text),"<span size=\"small\">Version %s · built %s</span>",version,build_date);
  about_row_ex(titles,&row,text,TRUE,FALSE);
  gtk_box_append(GTK_BOX(header),titles);
  gtk_grid_attach(GTK_GRID(page),header,0,page_row++,1,1);

  /* What this program is, and where it came from. */
  GtkWidget *g=about_group(page,&page_row,"About");
  row=0;
  about_row(g,&row,
            "MacHPSDR is an independently maintained fork of LinHPSDR by "
            "Gleb Sushko (enthru), developed primarily on macOS.",FALSE);
  about_row(g,&row,
            "About half of the code in this program is new work written for the fork: the "
            "single-window UI and colour skins, GPU-rendered spectrum and meters, broadcast "
            "FM with RDS, the FT8/FT4, SSTV, WEFAX, CW and HFDL decoders, CW transmit and "
            "keyer, DX-cluster overlay, TCI server, NR3/NR4 noise reduction, manual notch "
            "filters, TX audio processing, the I/Q recorder and player, SoapySDR transmit "
            "and PPM calibration.",FALSE);
  about_row(g,&row,
            "The foundation — the HPSDR protocol stack, the WDSP receiver chain and the "
            "original application — is LinHPSDR by John Melton, G0ORX/N6LYT. Every source "
            "file that came from it keeps his copyright notice.",FALSE);
  about_row(g,&row,"https://github.com/enthru/MacHPSDR",FALSE);

  /* Credits. */
  g=about_group(page,&page_row,"Credits");
  row=0;
  about_row(g,&row,"John Melton, G0ORX/N6LYT — LinHPSDR, the application this fork is built on",FALSE);
  about_row(g,&row,"Gleb Sushko, enthru — MacHPSDR fork: macOS support, UI and new features",FALSE);
  about_row(g,&row,"Warren Pratt, NR0V — WDSP, the DSP library the whole receiver runs on",FALSE);
  about_row(g,&row,"Steve Wilson, KA6S — RIGCTL (CAT over TCP) and testing (LinHPSDR)",FALSE);
  about_row(g,&row,"Ken Hopper, N9VV — testing and documentation (LinHPSDR)",FALSE);

  /* Bundled third-party code. */
  g=about_group(page,&page_row,"Bundled components");
  row=0;
  snprintf(text,sizeof(text),"WDSP v%d.%02d (Warren Pratt, NR0V) — GPLv2, vendored and patched",
           GetWDSPVersion()/100,GetWDSPVersion()%100);
  about_row(g,&row,text,FALSE);
#ifdef FT8
  about_row(g,&row,"ft8_lib (Karlis Goba, YL3JG) — MIT · cty.dat DXCC data (Jim Reisert, AD1C)",FALSE);
#endif
  about_row(g,&row,"RNNoise (Xiph.Org) — BSD-3 · libspecbleach (Luciano Dato) — LGPL-2.1+  [NR3/NR4]",FALSE);
#ifdef HFDL
  about_row(g,&row,
            "HFDL decoder ported from dumphfdl and libacars (Tomasz Lemiech) — GPLv3 / MIT, "
            "with liquid-dsp (Joseph Gaeddert) — MIT and libfec (Phil Karn, KA9Q) — LGPL",FALSE);
#endif

  /* License. */
  g=about_group(page,&page_row,"License");
  row=0;
  about_row(g,&row,
            "Free software under the GNU General Public License, version 2 or later. "
            "There is NO WARRANTY, to the extent permitted by law. See the LICENSE and "
            "NOTICE files for the full text and the per-component terms.",FALSE);
#ifdef HFDL
  about_row(g,&row,
            "This build includes the HFDL decoder, which is ported from GPLv3 sources, so "
            "the binary as a whole is effectively GPLv3.",FALSE);
#endif

  /* Radio hardware this session is talking to. */
  g=about_group(page,&page_row,"Device");
  row=0;

  switch(r->discovered->protocol) {
    case PROTOCOL_1:
      protocol="Protocol: 1";
      break;
    case PROTOCOL_2:
      protocol="Protocol: 2";
      break;
    case PROTOCOL_FAKE:
      protocol="Protocol: I/Q Player";
      break;
#ifdef SOAPYSDR
    case PROTOCOL_SOAPYSDR:
      protocol="Protocol: SOAPYSDR";
      break;
#endif
    default:
      break;
  }
  snprintf(text,sizeof(text),"%s %s %s",r->discovered->name,protocol,r->discovered->software_version);
  about_row(g,&row,text,FALSE);

  switch(r->discovered->protocol) {
    case PROTOCOL_1:
    case PROTOCOL_2:
#ifdef USBOZY
      if(d->device==DEVICE_OZY) {
        about_row(g,&row,"Device OZY: USB /dev/ozy",FALSE);
      } else {
#endif
        strcpy(addr,inet_ntoa(r->discovered->info.network.address.sin_addr));
        strcpy(interface_addr,inet_ntoa(r->discovered->info.network.interface_address.sin_addr));
        snprintf(text,sizeof(text),"MAC address: %02X:%02X:%02X:%02X:%02X:%02X",
                r->discovered->info.network.mac_address[0],
                r->discovered->info.network.mac_address[1],
                r->discovered->info.network.mac_address[2],
                r->discovered->info.network.mac_address[3],
                r->discovered->info.network.mac_address[4],
                r->discovered->info.network.mac_address[5]);
        about_row(g,&row,text,FALSE);
        snprintf(text,sizeof(text),"IP address: %s on %s (%s)",addr,
                 r->discovered->info.network.interface_name,interface_addr);
        about_row(g,&row,text,FALSE);
#ifdef USBOZY
      }
#endif
      break;
    default:
      break;
  }

  return page;
}
