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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <glib.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "ft8_dxcc.h"

// ---- parsed entity table ---------------------------------------------------
#define MAX_ENTITIES 512

typedef struct { char prefix[16]; char name[64]; } dxcc_entity_t;

static dxcc_entity_t entities[MAX_ENTITIES];
static int           n_entities = 0;

static GHashTable   *exact_ht  = NULL;  // "=CALL" exact matches -> ent+1
static GHashTable   *prefix_ht = NULL;  // prefix -> ent+1
static int           max_prefix_len = 0;
static char          loaded_path[1024] = "";  // where cty.dat was read from

// ---- cty.dat token cleanup -------------------------------------------------
// Trim leading/trailing whitespace and cut the base callsign/prefix off at the
// first cty.dat annotation char: "(cq)[itu]<lat/lon>{cont}~tz~".  Keeps '/'
// (part of exact callsigns like "=AH6ES/0").
static void clean_token(const char *src, char *dst, size_t dstsz) {
  while (*src && isspace((unsigned char)*src)) src++;
  size_t j = 0;
  for (; *src && j < dstsz - 1; src++) {
    char c = *src;
    if (c == '(' || c == '[' || c == '<' || c == '{' || c == '~' ||
        c == ';' || c == ',' || isspace((unsigned char)c))
      break;
    dst[j++] = (char)toupper((unsigned char)c);
  }
  dst[j] = '\0';
}

static void add_prefix(const char *pfx, int ent) {
  if (!pfx[0]) return;
  int len = (int)strlen(pfx);
  if (len > max_prefix_len) max_prefix_len = len;
  // First definition wins: cty.dat lists entities in a stable order and resolves
  // real ambiguity through the exact-call lists, so we never overwrite.
  if (!g_hash_table_contains(prefix_ht, pfx))
    g_hash_table_insert(prefix_ht, g_strdup(pfx), GINT_TO_POINTER(ent + 1));
}

static void add_exact(const char *call, int ent) {
  if (!call[0]) return;
  g_hash_table_insert(exact_ht, g_strdup(call), GINT_TO_POINTER(ent + 1));
}

// Parse an 8-field ":"-separated header line into a new entity; -1 on failure.
static int parse_header(const char *line) {
  const char *p = line;
  char fields[8][64];
  for (int f = 0; f < 8; f++) {
    const char *colon = strchr(p, ':');
    if (!colon) return -1;
    int n = (int)(colon - p);
    if (n > 63) n = 63;
    // trim leading/trailing whitespace of the field
    int s = 0; while (s < n && isspace((unsigned char)p[s])) s++;
    int e = n;  while (e > s && isspace((unsigned char)p[e-1])) e--;
    memcpy(fields[f], p + s, e - s);
    fields[f][e - s] = '\0';
    p = colon + 1;
  }
  if (n_entities >= MAX_ENTITIES) return -1;
  dxcc_entity_t *ent = &entities[n_entities];
  g_strlcpy(ent->name, fields[0], sizeof(ent->name));
  const char *pfx = fields[7];
  if (pfx[0] == '*') pfx++;                 // '*' marks a non-DXCC (WAE) addition
  g_strlcpy(ent->prefix, pfx, sizeof(ent->prefix));
  // The primary prefix is itself a matchable prefix for this entity.
  char clean[16]; clean_token(ent->prefix, clean, sizeof(clean));
  add_prefix(clean, n_entities);
  return n_entities++;
}

// Parse a comma-separated alias fragment (may be part of a multi-line record).
static void parse_aliases(const char *line, int ent) {
  if (ent < 0) return;
  const char *p = line;
  while (*p) {
    // one token up to ',' or ';'
    char raw[64]; int j = 0;
    while (*p && *p != ',' && *p != ';' && j < 63) raw[j++] = *p++;
    raw[j] = '\0';
    if (*p == ',' || *p == ';') p++;
    // skip whitespace-only fragments
    const char *q = raw; while (*q && isspace((unsigned char)*q)) q++;
    if (!*q) continue;
    gboolean exact = (*q == '=');
    if (exact) q++;
    char base[32];
    clean_token(q, base, sizeof(base));
    if (!base[0]) continue;
    if (exact) add_exact(base, ent);
    else       add_prefix(base, ent);
  }
}

// ---- file location ---------------------------------------------------------
// Try `path`; on success record it in loaded_path and return the open handle.
static FILE *try_open(const char *path) {
  FILE *f = fopen(path, "r");
  if (f) g_strlcpy(loaded_path, path, sizeof(loaded_path));
  return f;
}

static FILE *open_cty(void) {
  loaded_path[0] = '\0';
  const char *env = g_getenv("MACHPSDR_CTY");
  if (env && env[0]) { FILE *f = try_open(env); if (f) return f; }

  char path[1024];

#ifdef __APPLE__
  // Next to the executable, inside the .app bundle (Contents/Resources).
  char exe[1024]; uint32_t sz = sizeof(exe);
  if (_NSGetExecutablePath(exe, &sz) == 0) {
    char *slash = strrchr(exe, '/');
    if (slash) {
      *slash = '\0';
      snprintf(path, sizeof(path), "%s/../Resources/cty.dat", exe);
      FILE *f = try_open(path); if (f) return f;
    }
  }
#endif

  // Repo/cwd (running straight from the source tree).
  { FILE *f = try_open("cty.dat"); if (f) return f; }

  // Per-user data dir, then a system install location.
  snprintf(path, sizeof(path), "%s/.local/share/machpsdr/cty.dat", g_get_home_dir());
  { FILE *f = try_open(path); if (f) return f; }
  { FILE *f = try_open("/usr/local/share/machpsdr/cty.dat"); if (f) return f; }
  { FILE *f = try_open("/usr/share/machpsdr/cty.dat"); if (f) return f; }
  return NULL;
}

// ===========================================================================
// Public API
// ===========================================================================
// Parse cty.dat into freshly-created (empty) tables.
static void load_cty(void) {
  FILE *f = open_cty();
  if (!f) {
    fprintf(stderr, "ft8-dxcc: cty.dat not found — new-DXCC highlight disabled\n");
    return;
  }

  char line[4096];
  int cur = -1;
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '\0' || line[0] == '\n' || line[0] == '#') continue;
    if (!isspace((unsigned char)line[0])) {
      // A header line begins a new record (8 colon-separated fields).
      cur = parse_header(line);
    } else {
      parse_aliases(line, cur);            // indented alias fragment
    }
  }
  fclose(f);
  fprintf(stderr, "ft8-dxcc: loaded %d DXCC entities from %s\n", n_entities, loaded_path);
}

void ft8_dxcc_init(void) {
  if (exact_ht) return;                     // already initialised
  exact_ht  = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  prefix_ht = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  load_cty();
}

int ft8_dxcc_reload(void) {
  if (!exact_ht) { ft8_dxcc_init(); return n_entities; }
  g_hash_table_remove_all(exact_ht);
  g_hash_table_remove_all(prefix_ht);
  n_entities = 0;
  max_prefix_len = 0;
  loaded_path[0] = '\0';
  load_cty();
  return n_entities;
}

gboolean ft8_dxcc_loaded(void) { return n_entities > 0; }
int ft8_dxcc_count(void) { return n_entities; }
const char *ft8_dxcc_path(void) { return loaded_path[0] ? loaded_path : NULL; }

int ft8_dxcc_entity(const char *call) {
  if (!call || !call[0] || n_entities == 0) return -1;

  char up[32];
  int j = 0;
  for (int i = 0; call[i] && j < 31; i++) up[j++] = (char)toupper((unsigned char)call[i]);
  up[j] = '\0';

  // Exact-callsign override (handles slashed special calls in cty.dat).
  gpointer v = g_hash_table_lookup(exact_ht, up);
  if (v) return GPOINTER_TO_INT(v) - 1;

  // Otherwise the longest matching prefix, evaluated over each '/'-segment so a
  // portable call ("SV9/DL1XX", "DL1XX/P") resolves to its DX entity.
  int best_ent = -1, best_len = 0;
  char *saveptr = NULL;
  char work[32];
  g_strlcpy(work, up, sizeof(work));
  for (char *seg = strtok_r(work, "/", &saveptr); seg; seg = strtok_r(NULL, "/", &saveptr)) {
    int seglen = (int)strlen(seg);
    int L = seglen < max_prefix_len ? seglen : max_prefix_len;
    for (; L >= 1; L--) {
      char probe[32];
      memcpy(probe, seg, L);
      probe[L] = '\0';
      gpointer pv = g_hash_table_lookup(prefix_ht, probe);
      if (pv) {
        if (L > best_len) { best_len = L; best_ent = GPOINTER_TO_INT(pv) - 1; }
        break;                              // longest prefix for this segment
      }
    }
  }
  return best_ent;
}

const char *ft8_dxcc_name(int ent) {
  if (ent < 0 || ent >= n_entities) return NULL;
  return entities[ent].name;
}
