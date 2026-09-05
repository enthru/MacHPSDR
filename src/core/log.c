/* Copyright (C)
* 2026 - MacHPSDR contributors
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <stdio.h>
#include <stdarg.h>
#include <strings.h>
#include <glib.h>

#include "log.h"

/* Default threshold: ERROR + INFO shown, DEBUG hidden. */
log_level_t machpsdr_log_level = LOG_LEVEL_INFO;

/* Zero-initialised static GMutex is usable directly (no g_mutex_init needed). */
static GMutex log_mutex;
static gint64 log_started;

/* Called under log_mutex, including by the first worker to emit a message. */
static double log_elapsed(void) {
  gint64 now = g_get_monotonic_time();
  if (log_started == 0) log_started = now;
  return (now - log_started) / 1000000.0;
}

const char *log_level_name(log_level_t level) {
  switch (level) {
    case LOG_LEVEL_ERROR: return "ERROR";
    case LOG_LEVEL_INFO:  return "INFO";
    case LOG_LEVEL_DEBUG: return "DEBUG";
    default:              return "?";
  }
}

void log_set_level(log_level_t level) {
  machpsdr_log_level = level;
}

log_level_t log_get_level(void) {
  return machpsdr_log_level;
}

int log_set_level_name(const char *name) {
  if (name == NULL) return -1;
  if (!strcasecmp(name, "error") || !strcasecmp(name, "err")) {
    machpsdr_log_level = LOG_LEVEL_ERROR;
    return 0;
  }
  if (!strcasecmp(name, "info") || !strcasecmp(name, "warn") ||
      !strcasecmp(name, "warning") || !strcasecmp(name, "notice")) {
    machpsdr_log_level = LOG_LEVEL_INFO;
    return 0;
  }
  if (!strcasecmp(name, "debug") || !strcasecmp(name, "verbose") ||
      !strcasecmp(name, "trace")) {
    machpsdr_log_level = LOG_LEVEL_DEBUG;
    return 0;
  }
  return -1;
}

void log_emit(log_level_t level, const char *fmt, ...) {
  va_list ap;
  g_mutex_lock(&log_mutex);
  fprintf(stderr, "[%s] [+%.3fs] ", log_level_name(level), log_elapsed());
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
  g_mutex_unlock(&log_mutex);
}

static const char *debug_names[] = {
  "general", "rx", "tx", "sync", "protocol", "decoder", "ui"
};
unsigned int machpsdr_debug_categories = (1u << LOG_CATEGORY_COUNT) - 1;

int log_set_debug_categories(const char *names) {
  if (!names || !*names) return -1;
  gchar **parts = g_strsplit(names, ",", -1);
  unsigned int mask = 0;
  for (int i = 0; parts[i]; ++i) {
    const char *name = g_strstrip(parts[i]);
    if (!strcasecmp(name, "all")) {
      mask |= (1u << LOG_CATEGORY_COUNT) - 1;
    } else if (!strcasecmp(name, "none") && !parts[1]) {
      mask = 0;
    } else {
      int category;
      for (category = 0; category < LOG_CATEGORY_COUNT; ++category)
        if (!strcasecmp(name, debug_names[category])) break;
      if (category == LOG_CATEGORY_COUNT) {
        g_strfreev(parts);
        return -1;
      }
      mask |= 1u << category;
    }
  }
  g_strfreev(parts);
  machpsdr_debug_categories = mask;
  return 0;
}

void log_emit_debug(log_category_t category, const char *fmt, ...) {
  va_list ap;
  g_mutex_lock(&log_mutex);
  fprintf(stderr, "[DEBUG][%s] [+%.3fs] ", debug_names[category], log_elapsed());
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
  g_mutex_unlock(&log_mutex);
}
