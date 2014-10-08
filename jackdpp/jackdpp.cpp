/* -*- mode: c; c-file-style: "bsd"; -*- */
/*
    Copyright (C) 2001-2005 Paul Davis
    
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

// Needed to get PRId32 formatters
#define __STDC_FORMAT_MACROS


#include <config.h>

#include <stdio.h>
#include <ctype.h>
#include <signal.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <dirent.h>
#include <dlfcn.h>

// CPlusPlus bits
#include <string>
#include <vector>
#include <iostream>
#include <sstream>

#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#endif
#include <boost/program_options.hpp>
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic pop
#endif

#include <jack/midiport.h>
#include <jack/intclient.h>
#include <jack/uuid.h>

#include "engine.h"
#include "internal.h"
#include "driver.h"
#include "shm.h"
#include "driver_parse.h"
#include "messagebuffer.h"
#include "clientengine.h"
#include "sanitycheck.h"

#include "jack_options_parser.hpp"

#ifdef USE_CAPABILITIES

#include <sys/stat.h>
/* capgetp and capsetp are linux only extensions, not posix */
#undef _POSIX_SOURCE
#include <sys/capability.h>
#include "start.h"

static struct stat pipe_stat;

#endif /* USE_CAPABILITIES */

using std::cout;
using std::cerr;
using std::ostream;
using std::endl;
using std::string;
using std::vector;
using std::stringstream;

using jack::jack_options;
using jack::jack_options_parser;

namespace po = boost::program_options;

static JSList *drivers = NULL;
static sigset_t signals;
static jack_engine_t *engine = NULL;
//static char *server_name = NULL;
//static int realtime = 1;
//static int realtime_priority = 10;
//static int do_mlock = 1;
//static int temporary = 0;
//static int verbose = 0;
//static int client_timeout = 0; /* msecs; if zero, use period size. */
//static unsigned int port_max = 256;
//static int do_unlock = 0;
//static jack_nframes_t frame_time_offset = 0;
//static int nozombies = 0;
//static int timeout_count_threshold = 0;

char * JackAddOnDir = ADDON_DIR;

static jack_driver_desc_t *
jack_find_driver_descriptor (const char * name);

static void 
do_nothing_handler (int sig)
{
	/* this is used by the child (active) process, but it never
	   gets called unless we are already shutting down after
	   another signal.
	*/
	char buf[64];
	snprintf (buf, sizeof(buf),
		  "received signal %d during shutdown (ignored)\n", sig);
	write (1, buf, strlen (buf));
}

static void
jack_load_internal_clients_pp (const vector<string> & load_list)
{
    for( const string & str : load_list ) {
        jack_request_t req;
        string client_name, path, args, rest;

        /* possible argument forms:

           client-name:client-type/args
           client-type/args
           client-name:client-type
           client-type

           client-name is the desired JACK client name.
           client-type is basically the name of the DLL/DSO without any suffix.
           args is a string whose contents will be passed to the client as
           it is instantiated
        */
        string::size_type str_length = str.size();

        string::size_type colon_pos = str.find(':');
        string::size_type slash_pos = str.find('/');

        if ((slash_pos == string::npos && colon_pos == string::npos) ||
            ((slash_pos != string::npos) && (colon_pos != string::npos) && (colon_pos > slash_pos))) {
            /* client-type */
            client_name = str;
            path = client_name;
        }
        else if (slash_pos != string::npos && colon_pos != string::npos) {
            /* client-name:client-type/args */
            client_name = str.substr(0,colon_pos);

            string::size_type len = slash_pos - (colon_pos + 1);
            if (len > 0) {
                path = str.substr(colon_pos+1,len);
            } else {
                path = client_name;
            }

            string::size_type rest_len = len - (slash_pos + 1);
            if (rest_len > 0 )
            {
                rest = str.substr(slash_pos+1,rest_len);
                args = rest;
            }
        } else if (slash_pos != string::npos && colon_pos == string::npos) {
            /* client-type/args */
            path = str.substr(0, slash_pos);
            string::size_type rest_len = str_length - (slash_pos+1);
            if (rest_len > 0) {
                rest = str.substr(slash_pos+1,rest_len);
                args = rest;
            }
        } else {
            /* client-name:client-type */
            client_name = str.substr(0,colon_pos);
            string::size_type rest_len = str.size() - (colon_pos+1);
            if( rest_len > 0 ) {
                path = str.substr((colon_pos+1),rest_len);
            }
        }

        // Check client name / path format
        if (client_name.size() == 0 || path.size() == 0 ) {
            const char * cstr = str.c_str();
            fprintf (stderr, "incorrect format for internal client specification (%s)\n", cstr);
            exit (1);
        }

        memset (&req, 0, sizeof (req));
        req.type = IntClientLoad;
        const char * client_name_cstr = client_name.c_str();
        strncpy (req.x.intclient.name, client_name_cstr, sizeof (req.x.intclient.name));
        const char * path_cstr = path.c_str();
        strncpy (req.x.intclient.path, path_cstr, sizeof (req.x.intclient.path));

        if (args.size() > 0) {
            const char * args_cstr = args.c_str();
            strncpy (req.x.intclient.init, args_cstr, sizeof (req.x.intclient.init));
        } else {
            req.x.intclient.init[0] = '\0';
        }

        pthread_mutex_lock (&engine->request_lock);
        jack_intclient_load_request (engine, &req);
        pthread_mutex_unlock (&engine->request_lock);
    }
}

static void
jack_load_internal_clients (JSList* load_list)
{ 
    JSList * node;

        for (node = load_list; node; node = jack_slist_next (node)) {

                char* str = (char*) node->data;
                jack_request_t req;
                char* colon = strchr (str, ':');
                char* slash = strchr (str, '/');
                char* client_name = NULL;
                char* path = NULL;
                char* args = NULL;
                char* rest = NULL;
                int free_path = 0;
                int free_name = 0;
                size_t len;

                /* possible argument forms:

                   client-name:client-type/args
                   client-type/args
                   client-name:client-type
                   client-type

                   client-name is the desired JACK client name.
                   client-type is basically the name of the DLL/DSO without any suffix.
                   args is a string whose contents will be passed to the client as
                   it is instantiated
                */

                if ((slash == NULL && colon == NULL) || (slash && colon && colon > slash)) {

                        /* client-type */

                        client_name = str;
                        path = client_name;

                } else if (slash && colon) {

                        /* client-name:client-type/args */

                        len = colon - str;
                        if (len) {
                                /* add 1 to leave space for a NULL */
                                client_name = (char*) malloc (len + 1);
                                free_name = 1;
                                memcpy (client_name, str, len);
                                client_name[len] = '\0';
                        }

                        len = slash - (colon+1);
                        if (len) {
                                /* add 1 to leave space for a NULL */
                                path = (char*) malloc (len + 1);
                                free_path = 1;
                                memcpy (path, colon + 1, len);
                                path[len] = '\0';
                        } else {
                                path = client_name;
                        }
                        
                        rest = slash + 1;
                        len = strlen (rest);
                                
                        if (len) {
                                /* add 1 to leave space for a NULL */
                                args = (char*) malloc (len + 1);
                                memcpy (args, rest, len);
                                args[len] = '\0';
                        }
                        
                } else if (slash && colon == NULL) {

                        /* client-type/args */

                        len = slash - str;

                        if (len) {
                                /* add 1 to leave space for a NULL */
                                path = (char *) malloc (len + 1);
                                free_path = 1;
                                memcpy (path, str, len);
                                path[len] = '\0';
                        }

                        rest = slash + 1;
                        len = strlen (rest);
                                
                        if (len) {
                                /* add 1 to leave space for a NULL */
                                args = (char*) malloc (len + 1);
                                memcpy (args, rest, len);
                                args[len] = '\0';
                        }
                } else {
                        
                        /* client-name:client-type */

                        len = colon - str;

                        if (len) {
                                /* add 1 to leave space for a NULL */
                                client_name = (char *) malloc (len + 1);
                                free_name = 1;
                                memcpy (client_name, str, len);
                                client_name[len] = '\0';
                                path = colon + 1;
                        }
                }

                if (client_name == NULL || path == NULL) {
                        fprintf (stderr, "incorrect format for internal client specification (%s)\n", str);
                        exit (1);
                }

                memset (&req, 0, sizeof (req));
                req.type = IntClientLoad;
                req.x.intclient.options = 0;
                strncpy (req.x.intclient.name, client_name, sizeof (req.x.intclient.name));
                strncpy (req.x.intclient.path, path, sizeof (req.x.intclient.path));

                if (args) {
                        strncpy (req.x.intclient.init, args, sizeof (req.x.intclient.init));
                } else {
                        req.x.intclient.init[0] = '\0';
                }

                pthread_mutex_lock (&engine->request_lock);
                jack_intclient_load_request (engine, &req);
                pthread_mutex_unlock (&engine->request_lock);
   
                if (free_name) {
                        free (client_name);
                }
                if (free_path) {
                        free (path);
                }
                if (args) {
                        free (args);
                }
        }
}

static int
jack_main( jack_options & parsed_options, jack_driver_desc_t * driver_desc, JSList * driver_params, JSList * slave_names, JSList * load_list )
{
	int sig;
	int i;
	sigset_t allsignals;
	struct sigaction action;
	int waiting;
	JSList * node;

	/* ensure that we are in our own process group so that
	   kill (SIG, -pgrp) does the right thing.
	*/

	setsid ();

	pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	/* what's this for?

	   POSIX says that signals are delivered like this:

	   * if a thread has blocked that signal, it is not
	       a candidate to receive the signal.
           * of all threads not blocking the signal, pick
	       one at random, and deliver the signal.

           this means that a simple-minded multi-threaded program can
           expect to get POSIX signals delivered randomly to any one
           of its threads,

	   here, we block all signals that we think we might receive
	   and want to catch. all "child" threads will inherit this
	   setting. if we create a thread that calls sigwait() on the
	   same set of signals, implicitly unblocking all those
	   signals. any of those signals that are delivered to the
	   process will be delivered to that thread, and that thread
	   alone. this makes cleanup for a signal-driven exit much
	   easier, since we know which thread is doing it and more
	   importantly, we are free to call async-unsafe functions,
	   because the code is executing in normal thread context
	   after a return from sigwait().
	*/

	sigemptyset (&signals);
	sigaddset(&signals, SIGHUP);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGQUIT);
	sigaddset(&signals, SIGPIPE);
	sigaddset(&signals, SIGTERM);
	sigaddset(&signals, SIGUSR1);
	sigaddset(&signals, SIGUSR2);

	/* all child threads will inherit this mask unless they
	 * explicitly reset it 
	 */

	pthread_sigmask (SIG_BLOCK, &signals, 0);

	if( !parsed_options.realtime && parsed_options.client_timeout == 0) {
		parsed_options.client_timeout = 500; /* 0.5 sec; usable when non realtime. */
    }

	/* get the engine/driver started */
	if ((engine = jack_engine_new(
             parsed_options.realtime,
             parsed_options.realtime_priority,
             parsed_options.memory_locked,
             parsed_options.unlock_memory,
             parsed_options.server_name.c_str(),
             parsed_options.temporary,
             parsed_options.verbose,
             parsed_options.client_timeout,
             parsed_options.port_max,
             getpid(),
             parsed_options.frame_time_offset, 
             parsed_options.no_zombies,
             parsed_options.timeout_threshold,
             drivers)) == 0) {
		jack_error ("cannot create engine");
		return -1;
	}

	jack_info ("loading driver ..");
	
	if (jack_engine_load_driver (engine, driver_desc, driver_params)) {
		jack_error ("cannot load driver module %s",
			 driver_desc->name);
		goto error;
	}

	for (node=slave_names; node; node=jack_slist_next(node)) {
        char *sl_name = (char*)node->data;
		jack_driver_desc_t *sl_desc = jack_find_driver_descriptor(sl_name);
		if (sl_desc) {
			jack_engine_load_slave_driver(engine, sl_desc, NULL);
		}
	}


	if (jack_drivers_start (engine) != 0) {
		jack_error ("cannot start driver");
		goto error;
	}

        jack_load_internal_clients (load_list);

	/* install a do-nothing handler because otherwise pthreads
	   behaviour is undefined when we enter sigwait.
	*/

	sigfillset (&allsignals);
	action.sa_handler = do_nothing_handler;
	action.sa_mask = allsignals;
	action.sa_flags = SA_RESTART|SA_RESETHAND;

	for (i = 1; i < NSIG; i++) {
		if (sigismember (&signals, i)) {
			sigaction (i, &action, 0);
		} 
	}
	
	if( parsed_options.verbose ) {
		jack_info ("%d waiting for signals", getpid());
	}

	waiting = TRUE;

	while (waiting) {
		sigwait (&signals, &sig);

		jack_info ("jack main caught signal %d", sig);
		
		switch (sig) {
		case SIGUSR1:
			jack_dump_configuration(engine, 1);
			break;
		case SIGUSR2:
			/* driver exit */
			waiting = FALSE;
			break;
		default:
			waiting = FALSE;
			break;
		}
	} 
	
	if (sig != SIGSEGV) {

		/* unblock signals so we can see them during shutdown.
		   this will help prod developers not to lose sight of
		   bugs that cause segfaults etc. during shutdown.
		*/
		sigprocmask (SIG_UNBLOCK, &signals, 0);
	}
	
	jack_engine_delete (engine);
	return 1;
	
error:
	jack_engine_delete (engine);
	return -1;
}

static jack_driver_desc_t *
jack_drivers_get_descriptor( jack_options & parsed_options, JSList * drivers, const char * sofile)
{
	jack_driver_desc_t * descriptor, * other_descriptor;
	JackDriverDescFunction so_get_descriptor;
	JSList * node;
	void * dlhandle;
	char * filename;
	const char * dlerr;
	int err;
	char* driver_dir;

	if ((driver_dir = getenv("JACK_DRIVER_DIR")) == 0) {
		driver_dir = JackAddOnDir;
	}
	filename = (char*)malloc (strlen (driver_dir) + 1 + strlen (sofile) + 1);
	sprintf (filename, "%s/%s", driver_dir, sofile);

	if( parsed_options.verbose ) {
		jack_info ("getting driver descriptor from %s", filename);
	}

	if ((dlhandle = dlopen (filename, RTLD_NOW|RTLD_GLOBAL)) == NULL) {
		jack_error ("could not open driver .so '%s': %s\n", filename, dlerror ());
		free (filename);
		return NULL;
	}

	so_get_descriptor = (JackDriverDescFunction)
		dlsym (dlhandle, "driver_get_descriptor");

	if ((dlerr = dlerror ()) != NULL) {
		jack_error("%s", dlerr);
		dlclose (dlhandle);
		free (filename);
		return NULL;
	}

	if ((descriptor = so_get_descriptor ()) == NULL) {
		jack_error ("driver from '%s' returned NULL descriptor\n", filename);
		dlclose (dlhandle);
		free (filename);
		return NULL;
	}

	if ((err = dlclose (dlhandle)) != 0) {
		jack_error ("error closing driver .so '%s': %s\n", filename, dlerror ());
	}

	/* check it doesn't exist already */
	for (node = drivers; node; node = jack_slist_next (node)) {
		other_descriptor = (jack_driver_desc_t *) node->data;

		if (strcmp (descriptor->name, other_descriptor->name) == 0) {
			jack_error ("the drivers in '%s' and '%s' both have the name '%s'; using the first\n",
				    other_descriptor->file, filename, other_descriptor->name);
			/* FIXME: delete the descriptor */
			free (filename);
			return NULL;
		}
	}

	snprintf (descriptor->file, sizeof(descriptor->file), "%s", filename);
	free (filename);

	return descriptor;
}

static JSList *
jack_drivers_load( jack_options & parsed_options )
{
	struct dirent * dir_entry;
	DIR * dir_stream;
	const char * ptr;
	int err;
	JSList * driver_list = NULL;
	jack_driver_desc_t * desc;
	char* driver_dir;

	if ((driver_dir = getenv("JACK_DRIVER_DIR")) == 0) {
		driver_dir = JackAddOnDir;
	}

	/* search through the driver_dir and add get descriptors
	   from the .so files in it */
	dir_stream = opendir (driver_dir);
	if (!dir_stream) {
		jack_error ("could not open driver directory %s: %s\n",
			    driver_dir, strerror (errno));
		return NULL;
	}
  
	while ( (dir_entry = readdir (dir_stream)) ) {
		/* check the filename is of the right format */
		if (strncmp ("jack_", dir_entry->d_name, 5) != 0) {
			continue;
		}

		ptr = strrchr (dir_entry->d_name, '.');
		if (!ptr) {
			continue;
		}
		ptr++;
		if (strncmp ("so", ptr, 2) != 0) {
			continue;
		}

		desc = jack_drivers_get_descriptor( parsed_options, drivers, dir_entry->d_name );
		if (desc) {
			driver_list = jack_slist_append (driver_list, desc);
		}
	}

	err = closedir (dir_stream);
	if (err) {
		jack_error ("error closing driver directory %s: %s\n",
			    driver_dir, strerror (errno));
	}

	if (!driver_list) {
		jack_error ("could not find any drivers in %s!\n", driver_dir);
		return NULL;
	}

	return driver_list;
}

static void copyright( ostream & os)
{
    os << "jackd " << VERSION << endl;
    os << "Copyright 2001-2009 Paul Davis, Stephane Letz, Jack O'Quinn, Torben Hohn and others." << endl;
    os << "jackd comes with ABSOLUTELY NO WARRANTY" << endl;
    os << "This is free software, and you are welcome to redistribute it" << endl;
    os << "under certain conditions; see the file COPYING for details" << endl << endl;
}

static jack_driver_desc_t *
jack_find_driver_descriptor (const char * name)
{
	jack_driver_desc_t * desc = 0;
	JSList * node;

	for (node = drivers; node; node = jack_slist_next (node)) {
		desc = (jack_driver_desc_t *) node->data;

		if (strcmp (desc->name, name) != 0) {
			desc = NULL;
		} else {
			break;
		}
	}

	return desc;
}

static void
jack_cleanup_files (const char *server_name)
{
	DIR *dir;
	struct dirent *dirent;
	char dir_name[PATH_MAX+1] = "";
        jack_server_dir (server_name, dir_name);

	/* On termination, we remove all files that jackd creates so
	 * subsequent attempts to start jackd will not believe that an
	 * instance is already running.  If the server crashes or is
	 * terminated with SIGKILL, this is not possible.  So, cleanup
	 * is also attempted when jackd starts.
	 *
	 * There are several tricky issues.  First, the previous JACK
	 * server may have run for a different user ID, so its files
	 * may be inaccessible.  This is handled by using a separate
	 * JACK_TMP_DIR subdirectory for each user.  Second, there may
	 * be other servers running with different names.  Each gets
	 * its own subdirectory within the per-user directory.  The
	 * current process has already registered as `server_name', so
	 * we know there is no other server actively using that name.
	 */

	/* nothing to do if the server directory does not exist */
	if ((dir = opendir (dir_name)) == NULL) {
		return;
	}

	/* unlink all the files in this directory, they are mine */
	while ((dirent = readdir (dir)) != NULL) {

		char fullpath[PATH_MAX+1];

		if ((strcmp (dirent->d_name, ".") == 0)
		    || (strcmp (dirent->d_name, "..") == 0)) {
			continue;
		}

		snprintf (fullpath, sizeof (fullpath), "%s/%s",
			  dir_name, dirent->d_name);

		if (unlink (fullpath)) {
			jack_error ("cannot unlink `%s' (%s)", fullpath,
				    strerror (errno));
		}
	} 

	closedir (dir);

	/* now, delete the per-server subdirectory, itself */
	if (rmdir (dir_name)) {
 		jack_error ("cannot remove `%s' (%s)", dir_name,
			    strerror (errno));
	}

	/* finally, delete the per-user subdirectory, if empty */
	if (rmdir (jack_user_dir ())) {
		if (errno != ENOTEMPTY) {
			jack_error ("cannot remove `%s' (%s)",
				    jack_user_dir (), strerror (errno));
		}
	}
}

static void
maybe_use_capabilities ()
{
#ifdef USE_CAPABILITIES
	int status;

	/* check to see if there is a pipe in the right descriptor */
	if ((status = fstat (PIPE_WRITE_FD, &pipe_stat)) == 0 &&
	    S_ISFIFO(pipe_stat.st_mode)) {

		/* tell jackstart we are up and running */
  	        char c = 1;

	        if (write (PIPE_WRITE_FD, &c, 1) != 1) {
		        jack_error ("cannot write to jackstart sync "
				 "pipe %d (%s)", PIPE_WRITE_FD,
				 strerror (errno));
	        }

		if (close(PIPE_WRITE_FD) != 0) {
			jack_error("jackd: error on startup pipe close: %s",
				   strerror (errno));
		} else {
			/* wait for jackstart process to set our capabilities */
			if (wait (&status) == -1) {
				jack_error ("jackd: wait for startup "
					    "process exit failed");
			}
			if (!WIFEXITED (status) || WEXITSTATUS (status)) {
				jack_error ("jackd: jackstart did not "
					    "exit cleanly");
				exit (1);
			}
		}
	}
#endif /* USE_CAPABILITIES */
}

static
void display_version( ostream & os )
{
    os << "jackd version " << VERSION << " tmpdir " DEFAULT_TMP_DIR <<
        " protocol " << PROTOCOL_VERSION << endl;
}

int	       
main (int argc, char *argv[])

{
    jack_driver_desc_t * desc;

	setvbuf (stdout, NULL, _IOLBF, 0);

    cout << "Jackd CPP Test Server *** NOT TO BE USED ***" << endl;

	maybe_use_capabilities ();

    jack_options_parser options_parser( argc, argv, true );

    jack_options & parsed_options = options_parser.get_parsed_options();

    if( parsed_options.show_temporary ) {
        cout << jack_tmpdir << endl;
        exit (0);
    }

    if( parsed_options.show_help ) {
        display_version( cout );
        options_parser.display_usage();
        if( parsed_options.error_message.length() > 0 ) {
            cerr << "Error: " << parsed_options.error_message << endl;
        }
        exit(1);
    }

	if( parsed_options.show_version ) {
        display_version( cout );
		return 0;
	}

	copyright( cout );

	if( parsed_options.sanity_checks && (0 < sanitycheck( parsed_options.realtime, FALSE))) {
        cerr << "Failed sanity checks" << endl;
        exit(1);
	}

    if( !parsed_options.success ) {
        options_parser.display_usage();
        if( parsed_options.error_message.length() > 0 ) {
            cerr << "Error: " << parsed_options.error_message << endl;
        }
        exit(1);
    }

	drivers = jack_drivers_load( parsed_options );

	if (!drivers) {
        cerr << "jackd: no drivers found; exiting" << endl;
		exit (1);
	}
	
	if( parsed_options.midi_buffer_size != 0 ) {
		jack_port_type_info_t* port_type = &jack_builtin_port_types[JACK_MIDI_PORT_TYPE];
		port_type->buffer_size = parsed_options.midi_buffer_size * jack_midi_internal_event_size ();
		port_type->buffer_scale_factor = -1;
		if( parsed_options.verbose ) {
            cerr << "Set MIDI buffer size to " << port_type->buffer_size << " bytes" << endl;
		}
	}

	desc = jack_find_driver_descriptor( parsed_options.driver.c_str() );
	if (!desc) {
        cerr << "jackd: unknown driver '" << parsed_options.driver << "'" << endl;
		exit (1);
	}

    int driver_nargs = options_parser.get_driver_argc();
    char ** driver_args = options_parser.get_driver_argv();

    JSList * driver_params = NULL;

	if (jack_parse_driver_params (desc, driver_nargs,
				      driver_args, &driver_params)) {
		exit (0);
	}

	if( parsed_options.server_name.length() == 0 ) {
		parsed_options.server_name = jack_default_server_name ();
    }

	int rc = jack_register_server( parsed_options.server_name.c_str(), parsed_options.replace_registry );
	switch (rc) {
	case EEXIST:
        cerr << "'" << parsed_options.server_name << "' server already active" << endl;
		exit (1);
	case ENOSPC:
        cerr << "too many servers already active" << endl;
		exit (2);
	case ENOMEM:
        cerr << "no access to shm registry" << endl;
		exit (3);
	default:
		if( parsed_options.verbose )
            cerr << "server '" << parsed_options.server_name << "' registered" << endl;
	}

	/* clean up shared memory and files from any previous
	 * instance of this server name */
	jack_cleanup_shm();
	jack_cleanup_files( parsed_options.server_name.c_str() );

	/* run the server engine until it terminates */
    JSList * slave_drivers_jsl = NULL;
    for( string & sd : parsed_options.slave_drivers ) {
        slave_drivers_jsl = jack_slist_append( slave_drivers_jsl, (void*)sd.c_str() );
    }
    JSList * load_list_jsl = NULL;
    for( string & ic : parsed_options.internal_clients ) {
        load_list_jsl = jack_slist_append( load_list_jsl, (void*)ic.c_str() );
    }
	jack_main( parsed_options, desc, driver_params, slave_drivers_jsl, load_list_jsl);

	/* clean up shared memory and files from this server instance */
	if( parsed_options.verbose )
        cerr << "cleaning up shared memory" << endl;
	jack_cleanup_shm ();
	if( parsed_options.verbose )
        cerr << "cleaning up files" << endl;
	jack_cleanup_files( parsed_options.server_name.c_str() );
	if( parsed_options.verbose )
        cerr << "unregistering server '" << parsed_options.server_name << "'" << endl;
	jack_unregister_server( parsed_options.server_name.c_str() );

	exit (0);
}
