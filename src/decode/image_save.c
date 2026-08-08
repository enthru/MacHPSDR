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

#include <glib.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <time.h>

#include "image_save.h"
#include "log.h"

typedef struct {
  guint8 *pix;
  int     w, h, nchan;
  char    path[700];
  char    prefix[16];
} save_job;

static gpointer save_worker(gpointer data) {
  save_job *j = data;
  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, j->w, j->h);
  if (pb != NULL) {
    guint8 *dst = gdk_pixbuf_get_pixels(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);
    for (int y = 0; y < j->h; y++) {
      const guint8 *src = j->pix + (size_t)y * j->w * j->nchan;
      guint8 *row = dst + (size_t)y * stride;
      if (j->nchan == 3) {
        memcpy(row, src, (size_t)j->w * 3);
      } else {
        for (int x = 0; x < j->w; x++)
          row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = src[x];
      }
    }
    GError *err = NULL;
    if (gdk_pixbuf_save(pb, j->path, "png", &err, NULL))
      log_info("%s: auto-saved %s (%d lines)\n", j->prefix, j->path, j->h);
    else {
      log_error("%s: auto-save failed: %s\n", j->prefix, err ? err->message : "?");
      if (err) g_error_free(err);
    }
    g_object_unref(pb);
  }
  g_free(j->pix);
  g_free(j);
  return NULL;
}

void image_save_folder(char *out, gsize len, const char *dir, const char *prefix) {
  if (dir != NULL && dir[0] != '\0') g_strlcpy(out, dir, len);
  else g_snprintf(out, len, "%s/.local/share/machpsdr/%s", g_get_home_dir(), prefix);
}

void image_save_async(guint8 *pix, int w, int h, int nchan,
                      const char *dir, const char *prefix) {
  if (pix == NULL) return;
  if (w <= 0 || h <= 0 || (nchan != 1 && nchan != 3)) { g_free(pix); return; }

  char folder[640];
  image_save_folder(folder, sizeof(folder), dir, prefix);
  if (g_mkdir_with_parents(folder, 0755) != 0) {
    log_error("%s: auto-save cannot create %s\n", prefix, folder);
    g_free(pix);
    return;
  }

  save_job *j = g_new0(save_job, 1);
  j->pix = pix;
  j->w = w; j->h = h; j->nchan = nchan;
  g_strlcpy(j->prefix, prefix, sizeof(j->prefix));
  time_t now = time(NULL);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
  g_snprintf(j->path, sizeof(j->path), "%s/%s_%s.png", folder, prefix, ts);

  GThread *t = g_thread_new("img-save", save_worker, j);
  if (t != NULL) g_thread_unref(t);
}
