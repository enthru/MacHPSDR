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
  fprintf(stderr, "[%s] ", log_level_name(level));
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
  g_mutex_unlock(&log_mutex);
}
