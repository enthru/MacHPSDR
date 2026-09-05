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

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>          /* _commit, _fileno */
#else
#include <unistd.h>      /* fsync, fileno */
#endif
#include "log.h"
#include "property.h"

// Active property store: name -> value, both heap strings owned by the table.
// This used to be a singly-linked list scanned linearly by getProperty/
// setProperty, which made building the store (radio_save_state / restore, one
// set/get per field) O(n^2). A hash map makes each get/set O(1).
static GHashTable* properties=NULL;

// Stash of properties temporarily moved aside so they survive an
// initProperties() wipe during a full re-serialization (radio_save_state).
static GHashTable* retained=NULL;

static double version=0.0;

static void ensure_store(void) {
  if(properties==NULL) {
    properties=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,g_free);
  }
}

void initProperties(void) {
  ensure_store();
  g_hash_table_remove_all(properties);
}

// A SECOND file needs a second store, not this one.  The store is a single
// global that initProperties() and loadProperties() both wipe, so reading or
// writing bookmarks or a MIDI file through it destroys the radio's live
// settings: every getProperty() after that finds nothing (a receiver added
// later comes up at factory defaults), and radio_save_state()'s retain pass
// then writes a props file with the closed receivers' settings gone for good.
// Push before touching another file, pop after; the pushed store is untouched
// in between.  Not reentrant beyond one level, which is all any caller needs
// (both are GTK-thread only).
static GHashTable* parked=NULL;
static double parked_version=0.0;

void pushPropertyStore(void) {
  ensure_store();
  if(parked!=NULL) {
    log_error("pushPropertyStore: already pushed -- the second file would clobber the first\n");
    return;
  }
  parked=properties;
  parked_version=version;   // loadProperties leaves it set, and it gates the load
  properties=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,g_free);
}

void popPropertyStore(void) {
  if(parked==NULL) {
    log_error("popPropertyStore: nothing pushed\n");
    return;
  }
  if(properties!=NULL) g_hash_table_destroy(properties);
  properties=parked;
  version=parked_version;
  parked=NULL;
}

// Move every property whose name begins with prefix out of the active store
// into the retained stash. The stash is not touched by initProperties(), so
// this is used to keep the settings of an inactive (user-closed) receiver
// across the full save/re-serialize in radio_save_state.
void retainProperties(char* prefix) {
  ensure_store();
  if(retained==NULL) {
    retained=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,g_free);
  }
  size_t plen=strlen(prefix);
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter,properties);
  while(g_hash_table_iter_next(&iter,&key,&value)) {
    if(strncmp((char*)key,prefix,plen)==0) {
      // Transfer ownership of key/value from the active store to the stash
      // without copying or freeing: steal removes without invoking the
      // destroy funcs, and insert takes over the pointers.
      g_hash_table_iter_steal(&iter);
      g_hash_table_insert(retained,key,value);
    }
  }
}

// Merge all retained properties back into the active store and clear the stash.
void releaseRetainedProperties(void) {
  ensure_store();
  if(retained==NULL) return;
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter,retained);
  while(g_hash_table_iter_next(&iter,&key,&value)) {
    g_hash_table_iter_steal(&iter);
    g_hash_table_insert(properties,key,value); // overwrites any live value
  }
}

/* A props file must not depend on the operator's locale.  Every float in it
   used to go through sprintf("%f") and atof(), so under a comma-decimal locale
   the file reads `ppm_correction_value=1,500000` -- which the same machine
   parses back correctly and every other one, and every LC_NUMERIC=C run (CI, a
   plain shell, a harness) reads as 1.  Colours went black, contrast and PA
   calibration reset, the ppm correction was silently dropped.
   Written in the C locale; read tolerant of BOTH, so an existing comma file is
   still understood and is rewritten with dots on the next save. */
void setPropertyDouble(char* name,double value) {
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(buf,sizeof(buf),"%f",value);
  setProperty(name,buf);
}

double propToDouble(const char* value) {
  char buf[64];
  int i;
  if(value==NULL) return 0.0;
  for(i=0;i<(int)sizeof(buf)-1 && value[i]!='\0';i++) {
    buf[i]=(value[i]==',')?'.':value[i];
  }
  buf[i]='\0';
  return g_ascii_strtod(buf,NULL);
}

/* --------------------------------------------------------------------------*/
/**
* @brief Load Properties
*
* @param filename
*/
// Read one file into the (already emptied) store. FALSE means the file could
// not be opened or held nothing -- which is what the backup below answers.
// A file that PARSES but fails the version gate returns TRUE: an older config
// from an older build is an understood state the gate exists to discard, not
// damage to recover from, and falling back there would quietly resurrect
// settings the gate was asked to drop.
static gboolean load_one(const char* filename) {
    char string[256];
    char* name;
    char* value;
    gboolean any=FALSE;
    FILE* f=fopen(filename,"r");

    if(f==NULL) return FALSE;

    while(fgets(string,sizeof(string),f)) {
        if(string[0]!='#') {
            name=strtok(string,"=");
            value=strtok(NULL,"\n");
            if (name != NULL && value != NULL) {
                setProperty(name,value);
                any=TRUE;
                if(strcmp(name,"property_version")==0) {
                    version=propToDouble(value);
                }
            }
        }
    }
    fclose(f);
    return any;
}

void loadProperties(char* filename) {
    ensure_store();
    g_hash_table_remove_all(properties);
    // Judge THIS file, not the last one: `version` is a static, so without the
    // reset the gate below only ever ran on the first load of the process and a
    // later file with no property_version line (or an older one) was accepted
    // on the strength of its predecessor.
    version=0.0;

    log_info("loadProperties: %s\n",filename);

    if(!load_one(filename)) {
        // Missing or empty. On a first run that is simply how it starts, so it
        // is not an error -- but it is also the one window saveProperties()
        // leaves open (between moving the old file aside and moving the new one
        // in), and the whole point of keeping the previous generation is that
        // something can pick it up again. A backup is only consulted here; it
        // is never consulted over a main file that has content.
        char* bak=g_strdup_printf("%s.bak",filename);
        if(load_one(bak)) {
            log_error("loadProperties: %s was missing or empty -- recovered from %s\n",
                      filename,bak);
        }
        g_free(bak);
    }

    if(version!=PROPERTY_VERSION) {
      g_hash_table_remove_all(properties);
      log_info("loadProperties: version=%f expected version=%f ignoring\n",version,PROPERTY_VERSION);
    }
}

/* --------------------------------------------------------------------------*/
/**
* @brief Save Properties
*
* @param filename
*/
// Get the bytes onto the disk, not merely out of stdio's buffer. Without this
// the rename below can be committed ahead of the data it is supposed to be
// publishing, and a power cut between the two leaves a settings file that is
// present, named correctly and empty.
static gboolean flush_to_disk(FILE* f) {
    if(fflush(f)!=0) return FALSE;
#ifdef _WIN32
    return _commit(_fileno(f))==0;
#else
    if(fsync(fileno(f))!=0) {
        // A filesystem that cannot sync at all (some network mounts, some
        // container overlays) answers EINVAL/ENOTSUP. The bytes are in the
        // file; they are merely not PROMISED to survive a power cut, which is
        // not a reason to refuse to save.
        if(errno!=EINVAL && errno!=ENOTSUP) return FALSE;
    }
    return TRUE;
#endif
}

/* Writing the settings in place is how settings get LOST rather than merely
   not updated. fopen(...,"w+") truncates the operator's entire configuration
   before the first byte of the replacement is written, and neither the writes
   nor the close were checked -- so a full disk, a disconnected network home, a
   crash or a kill between the truncate and the last fwrite left a short file,
   and loadProperties()'s version gate then discarded even that. Both ends were
   silent: nothing told the operator, and the next start simply came up at
   factory defaults.

   So the whole file is built BESIDE the target, flushed to the disk, and only
   then moved into place. rename(2) is atomic, so at every instant the settings
   file is either the old complete one or the new complete one and never a
   half-written one; if anything fails, the existing file has not been touched
   and the operator has lost an update rather than their configuration. The
   previous generation is kept as "<filename>.bak", both for them to recover by
   hand and for loadProperties(), which reads it when the main file is missing.

   The rename order (unlink .bak, main -> .bak, tmp -> main) is also what makes
   this work on Windows, where rename() will not replace an existing file.
   Returns FALSE if the settings did not reach the disk. */
gboolean saveProperties(char* filename) {
    char version_line[G_ASCII_DTOSTR_BUF_SIZE];
    gboolean ok=TRUE;

    g_ascii_formatd(version_line,sizeof(version_line),"%0.2f",PROPERTY_VERSION);
    setProperty("property_version",version_line);

    ensure_store();

    char* tmp=g_strdup_printf("%s.tmp",filename);
    char* bak=g_strdup_printf("%s.bak",filename);

    FILE* f=fopen(tmp,"w");
    if(!f) {
        log_error("saveProperties: cannot create %s: %s -- settings NOT saved, %s left unchanged\n",
                  tmp,g_strerror(errno),filename);
        g_free(tmp); g_free(bak);
        return FALSE;
    }

    // Sort the keys so the output file is deterministic (stable across saves,
    // human-diffable) rather than following the hash bucket order.
    GList* keys=g_hash_table_get_keys(properties);
    keys=g_list_sort(keys,(GCompareFunc)strcmp);
    for(GList* k=keys; k!=NULL; k=k->next) {
        const char* name=(const char*)k->data;
        const char* value=(const char*)g_hash_table_lookup(properties,name);
        // g_strdup_printf, not a 512-byte snprintf: a truncated line loses its
        // newline and therefore MERGES two settings into one malformed key,
        // which is worse than the long value it was guarding against.
        char* line=g_strdup_printf("%s=%s\n",name,value);
        size_t len=strlen(line);
        if(fwrite(line,1,len,f)!=len) {
            log_error("saveProperties: write to %s failed: %s\n",tmp,g_strerror(errno));
            ok=FALSE;
        }
        g_free(line);
        if(!ok) break;
    }
    g_list_free(keys);

    if(ok && !flush_to_disk(f)) {
        log_error("saveProperties: cannot flush %s to disk: %s\n",tmp,g_strerror(errno));
        ok=FALSE;
    }
    // fclose can fail on its own (a deferred write hitting a full disk), and it
    // invalidates the stream either way -- so it is checked, and only checked
    // here.
    if(fclose(f)!=0 && ok) {
        log_error("saveProperties: cannot close %s: %s\n",tmp,g_strerror(errno));
        ok=FALSE;
    }

    if(!ok) {
        g_unlink(tmp);
        log_error("saveProperties: settings NOT saved, %s left unchanged\n",filename);
        g_free(tmp); g_free(bak);
        return FALSE;
    }

    gboolean had_old=g_file_test(filename,G_FILE_TEST_EXISTS);
    if(had_old) {
        g_unlink(bak);                       // Windows rename will not replace
        if(g_rename(filename,bak)!=0) {
            // Not fatal: the update is still worth publishing, the operator
            // just does not get a previous generation to fall back on.
            log_error("saveProperties: cannot keep a backup at %s: %s\n",bak,g_strerror(errno));
            had_old=FALSE;
        }
    }
    if(g_rename(tmp,filename)!=0) {
        log_error("saveProperties: cannot move %s into place: %s\n",tmp,g_strerror(errno));
        g_unlink(tmp);
        // Put the old settings back rather than leave the operator with
        // nothing under the name the application reads.
        if(had_old && g_rename(bak,filename)!=0)
            log_error("saveProperties: and could not restore %s from %s -- the settings are in %s\n",
                      filename,bak,bak);
        g_free(tmp); g_free(bak);
        return FALSE;
    }

    g_free(tmp); g_free(bak);
    return TRUE;
}

/* --------------------------------------------------------------------------*/
/**
* @brief Get Properties
*
* @param name
*
* @return
*/
char* getProperty(char* name) {
    if(properties==NULL) return NULL;
    return (char*)g_hash_table_lookup(properties,name);
}

/* --------------------------------------------------------------------------*/
/**
* @brief Set Properties
*
* @param name
* @param value
*/
void setProperty(char* name,char* value) {
    ensure_store();
    // insert takes ownership of the duplicated strings; for an existing key it
    // frees the duplicate key and replaces the value (old value freed).
    g_hash_table_insert(properties,g_strdup(name),g_strdup(value));
}
