/*
  Original file (jackd.c):
  Copyright (C) 2001-2005 Paul Davis
  C++ Conversion And Modifications:
  Copyright (C) 2014 Daniel Hams
    
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

// Needed to get PRId32 formatters
#define __STDC_FORMAT_MACROS


#include <config.h>

// CPlusPlus bits
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <memory>

#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#endif
#include <boost/program_options.hpp>
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic pop
#endif

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

#include <jack/midiport.h>
#include <jack/intclient.h>
#include <jack/uuid.h>

#include "driver.hpp"
#include "shm.hpp"
#include "driver_parse.hpp"
#include "messagebuffer.hpp"
#include "engine.hpp"
#include "clientengine.hpp"
#include "sanitycheck.hpp"
#include "jack_constants.hpp"

#include "jack_options_parser.hpp"
#include "jack_signals.hpp"
#include "jack_drivers.hpp"
#include "jack_engine.hpp"

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
using std::unique_ptr;

using jack::addon_dir;
using jack::jack_options;
using jack::jack_options_parser;
using jack::jack_signals_create;
using jack::jack_signals_unblock;
using jack::jack_signals_install_do_nothing_action;
using jack::jack_signals_wait;
using jack::jack_drivers_load_pp;
using jack::jack_drivers_find_descriptor_pp;
using jack::jack_drivers_find_so_descriptor_pp;

static void jack_load_internal_clients_pp( jack_engine_t * engine, const vector<string> & internal_clients )
{
    for( const string & internal_client : internal_clients ) {
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
	string::size_type str_length = internal_client.size();

	string::size_type colon_pos = internal_client.find(':');
	string::size_type slash_pos = internal_client.find('/');

	if ((slash_pos == string::npos && colon_pos == string::npos) ||
	    ((slash_pos != string::npos) && (colon_pos != string::npos) && (colon_pos > slash_pos))) {
	    /* client-type */
	    client_name = internal_client;
	    path = client_name;
	}
	else if (slash_pos != string::npos && colon_pos != string::npos) {
	    /* client-name:client-type/args */
	    client_name = internal_client.substr(0,colon_pos);

	    string::size_type len = slash_pos - (colon_pos + 1);
	    if (len > 0) {
		path = internal_client.substr(colon_pos+1,len);
	    } else {
		path = client_name;
	    }

	    string::size_type rest_len = len - (slash_pos + 1);
	    if (rest_len > 0 )
	    {
		rest = internal_client.substr(slash_pos+1,rest_len);
		args = rest;
	    }
	} else if (slash_pos != string::npos && colon_pos == string::npos) {
	    /* client-type/args */
	    path = internal_client.substr(0, slash_pos);
	    string::size_type rest_len = str_length - (slash_pos+1);
	    if (rest_len > 0) {
		rest = internal_client.substr(slash_pos+1,rest_len);
		args = rest;
	    }
	} else {
	    /* client-name:client-type */
	    client_name = internal_client.substr(0,colon_pos);
	    string::size_type rest_len = str_length - (colon_pos+1);
	    if( rest_len > 0 ) {
		path = internal_client.substr((colon_pos+1),rest_len);
	    }
	}

	// Check client name / path format
	if (client_name.size() == 0 || path.size() == 0 ) {
	    cerr << "incorrect format for internal client specification (" << internal_client << ")" << endl;
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
	jack_intclient_load_request( engine, &req );
	pthread_mutex_unlock (&engine->request_lock);
    }
}

static int jack_main( const jack_options & parsed_options,
		      const vector<jack_driver_desc_t*> & loaded_drivers,
		      jack_driver_desc_t * driver_desc,
		      JSList * driver_params_jsl )
{
    unique_ptr<jack_engine_t> engine;
    int sig;

    sigset_t signals = jack_signals_create();

    if( (engine = jack_engine_create(
	     parsed_options,
	     getpid(),
	     loaded_drivers )) == 0 ) {
	jack_error ("cannot create engine");
	return -1;
    }

    jack_info ("loading driver ..");
	
    if( jack_engine_load_driver( *engine, driver_desc, driver_params_jsl )) {
	jack_error ("cannot load driver module %s",
		    driver_desc->name);
	goto error;
    }

    for( const string & slave_driver_name : parsed_options.slave_drivers ) {
	jack_driver_desc_t *sl_desc = jack_drivers_find_descriptor_pp( loaded_drivers, slave_driver_name );
	if (sl_desc) {
	    jack_engine_load_slave_driver( *engine, sl_desc, NULL );
	}
    }


    if (jack_engine_drivers_start( *engine ) != 0) {
	jack_error ("cannot start driver");
	goto error;
    }

    jack_load_internal_clients_pp( engine.get(), parsed_options.internal_clients );

    /* install a do-nothing handler because otherwise pthreads
       behaviour is undefined when we enter sigwait.
    */
    jack_signals_install_do_nothing_action( signals );

    if( parsed_options.verbose ) {
	jack_info ("%d waiting for signals", getpid());
    }

    sig = jack_signals_wait( signals, engine.get() );

    if (sig != SIGSEGV) {
	/* unblock signals so we can see them during shutdown.
	   this will help prod developers not to lose sight of
	   bugs that cause segfaults etc. during shutdown.
	*/
	jack_signals_unblock( signals );
    }

    jack_engine_cleanup( *engine );
    return 1;
	
  error:
    jack_engine_cleanup( *engine );
    return -1;
}

static void copyright( ostream & os)
{
    os << "jackd " << VERSION << endl;
    os << "Copyright 2001-2009 Paul Davis, Stephane Letz, Jack O'Quinn, Torben Hohn and others." << endl;
    os << "jackd comes with ABSOLUTELY NO WARRANTY" << endl;
    os << "This is free software, and you are welcome to redistribute it" << endl;
    os << "under certain conditions; see the file COPYING for details" << endl << endl;
}


static void jack_cleanup_files (const char *server_name)
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
     * There are several tricky issues. First, the previous JACK
     * server may have run for a different user ID, so its files
     * may be inaccessible.  This is handled by using a separate
     * JACK_TMP_DIR subdirectory for each user.	 Second, there may
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
	if ((strcmp (dirent->d_name, ".") == 0)
	    || (strcmp (dirent->d_name, "..") == 0)) {
	    continue;
	}

	stringstream ss( stringstream::out );
	ss << dir_name << '/' << dirent->d_name;

	string fullpath( ss.str() );

	if (unlink (fullpath.c_str())) {
	    jack_error ("cannot unlink `%s' (%s)", fullpath.c_str(),
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

static void maybe_use_capabilities ()
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

static void display_version( ostream & os )
{
    os << "jackd version " << VERSION << " tmpdir " DEFAULT_TMP_DIR <<
	" protocol " << PROTOCOL_VERSION << endl;
}

int main (int argc, char *argv[])
{
    jack_driver_desc_t * desc;

    setvbuf (stdout, NULL, _IOLBF, 0);

    cout << "Jackd CPP Test Server *** NOT TO BE USED ***" << endl;

    maybe_use_capabilities ();

#ifdef DEBUG_ENABLED
    jack_options_parser options_parser( argc, argv, true );
#else
    jack_options_parser options_parser( argc, argv );
#endif

    jack_options & parsed_options = options_parser.get_parsed_options();

    if( parsed_options.show_temporary ) {
	cout << jack_tmpdir << endl;
	exit (0);
    }

    if( parsed_options.show_version ) {
	display_version( cout );
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

    vector<jack_driver_desc_t*> loaded_drivers = jack_drivers_load_pp( parsed_options.verbose );

    if (loaded_drivers.size() == 0) {
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

    desc = jack_drivers_find_descriptor_pp( loaded_drivers, parsed_options.driver );
    if (!desc) {
	cerr << "jackd: unknown driver '" << parsed_options.driver << "'" << endl;
	exit (1);
    }

    int driver_nargs = options_parser.get_driver_argc();
    char ** driver_args = options_parser.get_driver_argv();

    JSList * driver_params_jsl = NULL;

    if (jack_parse_driver_params( desc, driver_nargs,
				  driver_args, &driver_params_jsl)) {
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
    jack_main( parsed_options,
	       loaded_drivers,
	       desc,
	       driver_params_jsl );

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
