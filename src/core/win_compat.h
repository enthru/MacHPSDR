/*  win_compat.h

    Shims for the handful of functions mingw's CRT does not provide.

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
 * FORCE-INCLUDED on Windows only, via -include in the Makefile's WIN_OPTIONS —
 * it is never named by an #include anywhere.
 *
 * The reason it is force-included rather than included: the functions missing
 * from mingw's CRT are used by the VENDORED trees (ft8_lib), which are carried
 * verbatim and must stay that way.  Reaching them without an #include is the
 * whole point.
 *
 * Everything here must therefore be safe to inject into every translation unit
 * in the build, including upstream code: no macros over common names, nothing
 * that is not guarded against already existing.
 */

#ifndef WIN_COMPAT_H
#define WIN_COMPAT_H

#if defined(_WIN32)

#include <string.h>

/* POSIX/GNU; mingw has strcpy but not the variant returning the END of the
 * copy.  ft8_lib/ft8/message.c builds message text with it. */
#ifndef HAVE_STPCPY
static inline char *stpcpy(char *dst, const char *src) {
  size_t n = strlen(src);
  memcpy(dst, src, n + 1);
  return dst + n;
}
#define HAVE_STPCPY 1
#endif

#endif /* _WIN32 */

#endif /* WIN_COMPAT_H */
