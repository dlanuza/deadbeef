/*
    DeaDBeeF - ultimate music player for GNU/Linux systems with X11
    Copyright (C) 2009  Alexey Yakovenko

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
#include <dirent.h>
#include <dlfcn.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif
#include "plugins.h"
#include "md5/md5.h"
#include "messagepump.h"
#include "messages.h"
#include "threading.h"
#include "progress.h"
#include "playlist.h"
#include "volume.h"
#include "streamer.h"
#include "playback.h"

// deadbeef api
DB_functions_t deadbeef_api = {
    // FIXME: set to 1.0 after api freeze
    .vmajor = 0,
    .vminor = 0,
    .ev_subscribe = plug_ev_subscribe,
    .ev_unsubscribe = plug_ev_unsubscribe,
    .md5 = plug_md5,
    .md5_to_str = plug_md5_to_str,
    .playback_next = plug_playback_next,
    .playback_prev = plug_playback_prev,
    .playback_pause = plug_playback_pause,
    .playback_stop = plug_playback_stop,
    .playback_play = plug_playback_play,
    .playback_random = plug_playback_random,
    .playback_get_pos = plug_playback_get_pos,
    .playback_set_pos = plug_playback_set_pos,
    .playback_get_samplerate = p_get_rate,
    .quit = plug_quit,
    // threading
    .thread_start = thread_start,
    .thread_join = thread_join,
    .mutex_create = mutex_create,
    .mutex_free = mutex_free,
    .mutex_lock = mutex_lock,
    .mutex_unlock = mutex_unlock,
    .cond_create = cond_create,
    .cond_free = cond_free,
    .cond_wait = cond_wait,
    .cond_signal = cond_signal,
    .cond_broadcast = cond_broadcast,
    // playlist access
    .pl_item_alloc = (DB_playItem_t* (*)(void))pl_item_alloc,
    .pl_item_free = (void (*)(DB_playItem_t *))pl_item_free,
    .pl_item_copy = (void (*)(DB_playItem_t *, DB_playItem_t *))pl_item_copy,
    .pl_insert_item = (DB_playItem_t *(*) (DB_playItem_t *after, DB_playItem_t *it))pl_insert_item,
    // metainfo
    .pl_add_meta = (void (*) (DB_playItem_t *, const char *, const char *))pl_add_meta,
    .pl_find_meta = (const char *(*) (DB_playItem_t *, const char *))pl_find_meta,
    // cuesheet support
    .pl_insert_cue_from_buffer = (DB_playItem_t *(*) (DB_playItem_t *after, const char *fname, const uint8_t *buffer, int buffersize, struct DB_decoder_s *decoder, const char *ftype))pl_insert_cue_from_buffer,
    .pl_insert_cue = (DB_playItem_t *(*)(DB_playItem_t *, const char *, struct DB_decoder_s *, const char *))pl_insert_cue,
    // volume control
    .volume_set_db = plug_volume_set_db,
    .volume_get_db = volume_get_db,
    .volume_set_amp = plug_volume_set_amp,
    .volume_get_amp = volume_get_amp,
};

void
volumebar_notify_changed (void);

void
plug_volume_set_db (float db) {
    volume_set_db (db);
    volumebar_notify_changed ();
}

void
plug_volume_set_amp (float amp) {
    volume_set_amp (amp);
    volumebar_notify_changed ();
}

#define MAX_DECODERS 50
DB_decoder_t *g_decoders[MAX_DECODERS+1];

void
plug_md5 (uint8_t sig[16], const char *in, int len) {
    md5_buffer (in, len, sig);
}

void
plug_md5_to_str (char *str, const uint8_t sig[16]) {
    md5_sig_to_string ((char *)sig, str, 33);
}

// event handlers
typedef struct {
    DB_plugin_t *plugin;
    DB_callback_t callback;
    uintptr_t data;
} evhandler_t;
#define MAX_HANDLERS 100
static evhandler_t handlers[DB_EV_MAX][MAX_HANDLERS];

// plugin control structures
typedef struct plugin_s {
    void *handle;
    DB_plugin_t *plugin;
    struct plugin_s *next;
} plugin_t;
plugin_t *plugins;

void
plug_ev_subscribe (DB_plugin_t *plugin, int ev, DB_callback_t callback, uintptr_t data) {
    assert (ev < DB_EV_MAX && ev >= 0);
    for (int i = 0; i < MAX_HANDLERS; i++) {
        if (!handlers[ev][i].plugin) {
            handlers[ev][i].plugin = plugin;
            handlers[ev][i].callback = callback;
            handlers[ev][i].data = data;
            return;
        }
    }
    fprintf (stderr, "failed to subscribe plugin %s to event %d (too many event handlers)\n", plugin->name, ev);
}

void
plug_ev_unsubscribe (DB_plugin_t *plugin, int ev, DB_callback_t callback, uintptr_t data) {
    assert (ev < DB_EV_MAX && ev >= 0);
    for (int i = 0; i < MAX_HANDLERS; i++) {
        if (handlers[ev][i].plugin == plugin) {
            handlers[ev][i].plugin = NULL;
            handlers[ev][i].callback = NULL;
            handlers[ev][i].data = 0;
            return;
        }
    }
}

void
plug_playback_next (void) {
    messagepump_push (M_NEXTSONG, 0, 0, 0);
}

void
plug_playback_prev (void) {
    messagepump_push (M_PREVSONG, 0, 0, 0);
}

void
plug_playback_pause (void) {
    messagepump_push (M_PAUSESONG, 0, 0, 0);
}

void 
plug_playback_stop (void) {
    messagepump_push (M_STOPSONG, 0, 0, 0);
}

void 
plug_playback_play (void) {
    messagepump_push (M_PLAYSONG, 0, 0, 0);
}

void 
plug_playback_random (void) {
    messagepump_push (M_PLAYRANDOM, 0, 0, 0);
}

float
plug_playback_get_pos (void) {
    if (playlist_current.duration <= 0) {
        return 0;
    }
    return streamer_get_playpos () * 100 / playlist_current.duration;
}

void
plug_playback_set_pos (float pos) {
    if (playlist_current.duration <= 0) {
        return;
    }
    float t = pos * playlist_current.duration / 100.f;
    streamer_set_seek (t);
}

void 
plug_quit (void) {
    progress_abort ();
    messagepump_push (M_TERMINATE, 0, 0, 0);
}

/////// non-api functions (plugin support)
void
plug_trigger_event (int ev) {
    DB_event_t *event;
    switch (ev) {
    case DB_EV_SONGSTARTED:
    case DB_EV_SONGFINISHED:
        {
        DB_event_song_t *pev = malloc (sizeof (DB_event_song_t));
        pev->song = DB_PLAYITEM (&playlist_current);
        event = DB_EVENT (pev);
        }
        break;
    default:
        event = malloc (sizeof (DB_event_t));
    }
    event->event = ev;
    event->time = (double)clock () / CLOCKS_PER_SEC;
    for (int i = 0; i < MAX_HANDLERS; i++) {
        if (handlers[ev][i].plugin && !handlers[ev][i].plugin->inactive) {
            handlers[ev][i].callback (event, handlers[ev][i].data);
        }
    }
    free (event);
}

int
plug_init_plugin (DB_plugin_t* (*loadfunc)(DB_functions_t *), void *handle) {
    DB_plugin_t *plugin_api = loadfunc (&deadbeef_api);
    if (!plugin_api) {
        return -1;
    }
    plugin_t *plug = malloc (sizeof (plugin_t));
    memset (plug, 0, sizeof (plugin_t));
    plug->plugin = plugin_api;
    plug->handle = handle;
    plug->next = plugins;
    if (plug->plugin->start) {
        if (plug->plugin->start () < 0) {
            plug->plugin->inactive = 1;
        }
    }
    plugins = plug;
    return 0;
}

void
plug_load_all (void) {
    char dirname[1024];
    snprintf (dirname, 1024, "%s/lib/deadbeef", PREFIX);
    struct dirent **namelist = NULL;
    int n = scandir (dirname, &namelist, NULL, alphasort);
    if (n < 0)
    {
        if (namelist) {
            free (namelist);
        }
        return;	// not a dir or no read access
    }
    else
    {
        int i;
        for (i = 0; i < n; i++)
        {
            // no hidden files
            if (namelist[i]->d_name[0] != '.')
            {
                int l = strlen (namelist[i]->d_name);
                if (l < 3) {
                    continue;
                }
                if (strcasecmp (&namelist[i]->d_name[l-3], ".so")) {
                    continue;
                }
                char fullname[1024];
                strcpy (fullname, dirname);
                strncat (fullname, "/", 1024);
                strncat (fullname, namelist[i]->d_name, 1024);
                printf ("loading plugin %s\n", namelist[i]->d_name);
                void *handle = dlopen (fullname, RTLD_NOW);
                if (!handle) {
                    fprintf (stderr, "dlopen error: %s\n", dlerror ());
                    continue;
                }
                namelist[i]->d_name[l-3] = 0;
                printf ("module name is %s\n", namelist[i]->d_name);
                strcat (namelist[i]->d_name, "_load");
                DB_plugin_t *(*plug_load)(DB_functions_t *api) = dlsym (handle, namelist[i]->d_name);
                if (!plug_load) {
                    fprintf (stderr, "dlsym error: %s\n", dlerror ());
                    dlclose (handle);
                    continue;
                }
                if (plug_init_plugin (plug_load, handle) < 0) {
                    namelist[i]->d_name[l-3] = 0;
                    fprintf (stderr, "plugin %s is incompatible with current version of deadbeef, please upgrade the plugin\n", namelist[i]->d_name);
                    dlclose (handle);
                    continue;
                }
            }
            free (namelist[i]);
        }
        free (namelist);
    }
// load all compiled-in modules
#define PLUG(n) extern DB_plugin_t * n##_load (DB_functions_t *api);
#include "moduleconf.h"
#undef PLUG
#define PLUG(n) plug_init_plugin (n##_load, NULL);
#include "moduleconf.h"
#undef PLUG

    // find all decoders, and put in g_decoders list
    int numdecoders = 0;
    for (plugin_t *plug = plugins; plug; plug = plug->next) {
        if (plug->plugin->type == DB_PLUGIN_DECODER) {
            printf ("found decoder plugin %s\n", plug->plugin->name);
            if (numdecoders >= MAX_DECODERS) {
                break;
            }
            g_decoders[numdecoders] = (DB_decoder_t *)plug->plugin;
            numdecoders++;
        }
    }
    g_decoders[numdecoders] = NULL;
}

void
plug_unload_all (void) {
    while (plugins) {
        plugin_t *next = plugins->next;
        if (plugins->plugin->stop) {
            plugins->plugin->stop ();
        }
        if (plugins->handle) {
            dlclose (plugins->handle);
        }
        plugins = next;
    }
}

struct DB_decoder_s **
plug_get_decoder_list (void) {
    return g_decoders;
}

