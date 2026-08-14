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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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

/* --------------------------------------------------------------------------*/
/**
* @brief Load Properties
*
* @param filename
*/
void loadProperties(char* filename) {
    char string[256];
    char* name;
    char* value;
    FILE* f=fopen(filename,"r");

    ensure_store();
    g_hash_table_remove_all(properties);

    log_info("loadProperties: %s\n",filename);

    if(f) {
        while(fgets(string,sizeof(string),f)) {
            if(string[0]!='#') {
                name=strtok(string,"=");
                value=strtok(NULL,"\n");
                if (name != NULL && value != NULL) {
                    setProperty(name,value);
                    if(strcmp(name,"property_version")==0) {
                        version=atof(value);
                    }
                }
            }
        }
        fclose(f);
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
void saveProperties(char* filename) {
    FILE* f=fopen(filename,"w+");
    char line[512];
    if(!f) {
        log_error("can't open %s\n",filename);
        return;
    }

    sprintf(line,"%0.2f",PROPERTY_VERSION);
    setProperty("property_version",line);

    ensure_store();
    // Sort the keys so the output file is deterministic (stable across saves,
    // human-diffable) rather than following the hash bucket order.
    GList* keys=g_hash_table_get_keys(properties);
    keys=g_list_sort(keys,(GCompareFunc)strcmp);
    for(GList* k=keys; k!=NULL; k=k->next) {
        const char* name=(const char*)k->data;
        const char* value=(const char*)g_hash_table_lookup(properties,name);
        snprintf(line,sizeof(line),"%s=%s\n",name,value);
        fwrite(line,1,strlen(line),f);
    }
    g_list_free(keys);
    fclose(f);
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
