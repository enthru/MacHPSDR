/* resource_path.h -- where a resource that SHIPS WITH THE BINARY lives.

This file is part of MacHPSDR.  GNU GPL (v2 or later); see LICENSE.

The rule its call sites share: a file that travels with the executable is found
RELATIVE TO THE EXECUTABLE, never relative to the working directory -- the
working directory belongs to whoever launched us, and for a double-clicked
bundle it is not the install directory at all.  Three layouts ship that way and
this fork uses all three:

    <exe>/../Resources/<name>        the macOS .app bundle (Contents/Resources)
    <exe>/../share/machpsdr/<name>   a Unix prefix install -- and an AppImage,
                                     whose root is $APPDIR and not /, so the
                                     absolute /usr/share fallbacks below it
                                     point at the HOST and miss
    <exe>/assets/<name>              the binary and assets/ unpacked side by side

The cwd probes each caller already has stay exactly where they are: they are
what makes ./machpsdr work straight out of the repo, and they run first or last
according to what that caller already did.

HEADER-ONLY, and static inline, on purpose.  apt_coast.c and ft8_dxcc.c are two
of the objects the offline harnesses link, and those six rules each build their
own command line (see the Makefile) -- a new .o here would have to be added to
every one of them, and would be left off the seventh.  A header costs nothing
and cannot be forgotten.

Windows is deliberately NOT handled: win-package.sh puts assets/ beside the
.exe and main.c already chdir's there when it is not otherwise reachable, so
that platform has a working mechanism and adding a second one would mean two
things to keep in step.  The helper answers FALSE there and every caller falls
through to what it did before.
*/

#ifndef _RESOURCE_PATH_H
#define _RESOURCE_PATH_H

#include <stdio.h>
#include <string.h>
#include <glib.h>

#if defined(__APPLE__)
  #include <mach-o/dyld.h>
  #include <unistd.h>
#elif !defined(_WIN32)
  #include <unistd.h>
#endif

/* The directory holding this process's executable, or NULL if it cannot be
   determined.  Resolved once; the answer cannot change while we run. */
static inline const char *machpsdr_exe_dir(void) {
  static char dir[1024];
  static int resolved = 0;
  if (resolved) return dir[0] ? dir : NULL;
  resolved = 1;
  dir[0] = '\0';

#if defined(__APPLE__)
  uint32_t sz = sizeof(dir);
  if (_NSGetExecutablePath(dir, &sz) != 0) { dir[0] = '\0'; return NULL; }
#elif defined(_WIN32)
  return NULL;                       /* see the header comment */
#else
  /* /proc/self/exe is Linux's answer and is what an AppImage resolves to
     inside its own mount ($APPDIR/usr/bin/machpsdr).  readlink does not
     terminate, and reports truncation only by filling the buffer. */
  ssize_t n = readlink("/proc/self/exe", dir, sizeof(dir) - 1);
  if (n <= 0 || (size_t)n >= sizeof(dir) - 1) { dir[0] = '\0'; return NULL; }
  dir[n] = '\0';
#endif

  char *slash = strrchr(dir, '/');
  if (slash == NULL) { dir[0] = '\0'; return NULL; }
  *slash = '\0';
  return dir[0] ? dir : NULL;
}

/* Fills `out` with the first of the three layouts above that actually exists
   and returns TRUE; leaves `out` empty and returns FALSE if none does (or if
   the executable's own path is unavailable, which is the whole Windows case). */
static inline gboolean machpsdr_resource_path(const char *name, char *out, size_t outlen) {
  if (out == NULL || outlen == 0) return FALSE;
  out[0] = '\0';
  const char *dir = machpsdr_exe_dir();
  if (dir == NULL || name == NULL) return FALSE;

  /* Ordered, and the order is the one each platform is shipped in: a machine
     carrying two of these layouts at once is a developer's, and there the
     bundle is what was asked for. */
  static const char *const layouts[] = {
    "%s/../Resources/%s",
    "%s/../share/machpsdr/%s",
    "%s/assets/%s",
  };
  for (size_t i = 0; i < G_N_ELEMENTS(layouts); i++) {
    snprintf(out, outlen, layouts[i], dir, name);
    if (access(out, F_OK) == 0) return TRUE;
  }
  out[0] = '\0';
  return FALSE;
}

#endif
