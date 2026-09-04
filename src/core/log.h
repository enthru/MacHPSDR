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

/*
 * Minimal levelled logging for MacHPSDR.
 *
 * Three levels, in increasing verbosity:  ERROR < INFO < DEBUG.
 * A message is printed when its level is at or below the current threshold,
 * so the default INFO threshold shows ERROR + INFO and hides DEBUG.
 *
 * Set the threshold at start-up from the environment (MACHPSDR_LOG=debug|info|
 * error) or the command line (--log-level=debug, --debug, -v); see main.c.
 *
 * The level guard lives in the macro, so a log_debug() on a hot audio/protocol
 * path costs a single int comparison when DEBUG is not enabled — the arguments
 * are never evaluated.  Output is line-serialised so messages emitted from the
 * audio / protocol / decoder threads do not interleave.
 */

#ifndef _MACHPSDR_LOG_H
#define _MACHPSDR_LOG_H

typedef enum {
  LOG_LEVEL_ERROR = 0,
  LOG_LEVEL_INFO  = 1,
  LOG_LEVEL_DEBUG = 2
} log_level_t;

/* Current threshold; read directly by the macros below. Do not write it
 * except through log_set_level()/log_set_level_name(). */
extern log_level_t machpsdr_log_level;

/* Configure before starting worker threads. Categories filter DEBUG only. */
typedef enum {
  LOG_GENERAL, LOG_RX, LOG_TX, LOG_SYNC, LOG_PROTOCOL, LOG_DECODER, LOG_UI,
  LOG_CATEGORY_COUNT
} log_category_t;
extern unsigned int machpsdr_debug_categories;
/* Comma-separated names, or all/none. Invalid input leaves the mask intact. */
int log_set_debug_categories(const char *names);
void log_emit_debug(log_category_t category, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((format(printf, 2, 3)))
#endif
  ;

void        log_set_level(log_level_t level);
/* Parse "error"/"info"/"debug" (also "warn"->info, "verbose"->debug),
 * case-insensitive. Returns 0 on success, -1 on an unknown name. */
int         log_set_level_name(const char *name);
log_level_t log_get_level(void);
const char *log_level_name(log_level_t level);

void        log_emit(log_level_t level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
              __attribute__((format(printf, 2, 3)))
#endif
              ;

#define log_error(...) \
  do { if (machpsdr_log_level >= LOG_LEVEL_ERROR) log_emit(LOG_LEVEL_ERROR, __VA_ARGS__); } while (0)
#define log_info(...) \
  do { if (machpsdr_log_level >= LOG_LEVEL_INFO)  log_emit(LOG_LEVEL_INFO,  __VA_ARGS__); } while (0)
#define log_debug_area(category, ...) \
  do { if (machpsdr_log_level >= LOG_LEVEL_DEBUG && \
           (machpsdr_debug_categories & (1u << (category)))) \
    log_emit_debug(category, __VA_ARGS__); } while (0)
#define log_debug(...) log_debug_area(LOG_GENERAL, __VA_ARGS__)

#endif /* _MACHPSDR_LOG_H */
