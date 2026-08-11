/*  time_compat.h

    gmtime_r() on Windows.

    Copyright (C) 2026

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

*/

/*
 * Everything here is UTC timestamping — decoded-image filenames, the WAV
 * recorder, FT8's slot clock and QSO log.  They all reach for gmtime_r(), the
 * reentrant form, precisely because they run off decoder and audio threads
 * where the static buffer behind plain gmtime() would be shared.
 *
 * Windows has no gmtime_r.  Its equivalent is gmtime_s, whose arguments are in
 * the OTHER ORDER — (struct tm *out, const time_t *in) rather than POSIX's
 * (const time_t *in, struct tm *out).  A wrong guess is caught by the compiler
 * here (the types differ), but only because this shim exists in one place; the
 * reason to have it at all is that the reentrancy must not quietly be dropped
 * by falling back to gmtime().
 */

#ifndef TIME_COMPAT_H
#define TIME_COMPAT_H

#include <time.h>

#if defined(_WIN32)

static inline struct tm *gmtime_r(const time_t *timep, struct tm *result) {
  return gmtime_s(result, timep) == 0 ? result : NULL;
}

#endif /* _WIN32 */

#endif /* TIME_COMPAT_H */
