/*
 *  Copyright (C) 2001-2003 Paul Davis
 *  Copyright (C) 2004 Jack O'Quin
 *  Copyright (C) 2014- Daniel Hams
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <config.h>

#include <unistd.h>
#include <getopt.h>

#include "jack_constants.hpp"

#include "jack_drivers.hpp"
#include "jack_signals.hpp"

#include "internal.hpp"
#include "driver.hpp"
#include "driver_interface.hpp"

#include <errno.h>
#include <dirent.h>
#include <string.h>

#include <sstream>

namespace jack
{

using std::string;
using std::vector;
using std::stringstream;

using jack::addon_dir;

static jack_driver_desc_t * find_desc_by_so_name( const vector<jack_driver_desc_t*> & already_loaded_descs,
						  const string & so_name,
						  const bool verbose )
{
    jack_driver_desc_t * descriptor;
    JackDriverDescFunction so_get_descriptor;
    void * dlhandle;
    const char * dlerr;
    int err;
    const char* driver_dir;

    if ((driver_dir = getenv("JACK_DRIVER_DIR")) == 0) {
	driver_dir = addon_dir;
    }
    stringstream ss( stringstream::out );
    ss << driver_dir << '/' << so_name;
    string filename = ss.str();

    if( verbose ) {
	jack_info( "getting driver descriptor from %s", filename.c_str() );
    }

    if ((dlhandle = dlopen( filename.c_str(), RTLD_NOW|RTLD_GLOBAL )) == NULL) {
	jack_error( "could not open driver .so '%s': %s\n", filename.c_str(), dlerror() );
	return NULL;
    }

    so_get_descriptor = (JackDriverDescFunction)dlsym( dlhandle, "driver_get_descriptor" );

    if ( (dlerr = dlerror ()) != NULL) {
	jack_error("%s", dlerr);
	dlclose (dlhandle);
	return NULL;
    }

    if ( (descriptor = so_get_descriptor()) == NULL) {
	jack_error("driver from '%s' returned NULL descriptor\n", filename.c_str() );
	dlclose (dlhandle);
	return NULL;
    }

    if ( (err = dlclose(dlhandle)) != 0) {
	jack_error("error closing driver .so '%s': %s\n", filename.c_str(), dlerror());
    }

    /* check it doesn't exist already */
    for( jack_driver_desc_t * other_descriptor : already_loaded_descs ) {
	if( descriptor->name == other_descriptor->name ) {
	    jack_error ("the drivers in '%s' and '%s' both have the name '%s'; using the first\n",
			other_descriptor->file, filename.c_str(), other_descriptor->name);
	    /* FIXME: delete the descriptor */
	    return NULL;
	}
    }

    snprintf (descriptor->file, sizeof(descriptor->file), "%s", filename.c_str());

    return descriptor;
}

drivers::drivers( bool verbose ) :
    verbose_( verbose )
{
    struct dirent * dir_entry;
    DIR * dir_stream;
    const char * ptr;
    int err;
    jack_driver_desc_t * desc;
    const char* driver_dir;

    if ((driver_dir = getenv("JACK_DRIVER_DIR")) == 0) {
	driver_dir = addon_dir;
    }

    /* search through the driver_dir and add get descriptors
       from the .so files in it */
    dir_stream = opendir (driver_dir);
    if (!dir_stream) {
	jack_error ("could not open driver directory %s: %s\n",
		    driver_dir, strerror (errno));
	return;
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
	if( strncmp("so", ptr, 2) != 0) {
	    continue;
	}

	desc = find_desc_by_so_name( loaded_descs, dir_entry->d_name, verbose );

	if (desc) {
	    loaded_descs.push_back( desc );
	}
    }

    err = closedir( dir_stream );
    if (err) {
	jack_error("error closing driver directory %s: %s\n",
		    driver_dir, strerror (errno));
    }

    if (loaded_descs.size() == 0) {
	jack_error("could not find any drivers in %s!\n", driver_dir);
    }
}

jack_driver_desc_t * drivers::find_desc_by_name( const string & name ) const
{
    auto d_finder = std::find_if( loaded_descs.begin(), loaded_descs.end(),
				  [&name] ( const jack_driver_desc_t * desc ) {
				      return desc->name == name;
				  } );
    if( d_finder != loaded_descs.end() ) {
	return *d_finder;
    }
    return NULL;
}

static void driver_params_options_print( jack_driver_desc_t * desc, FILE * file )
{
    unsigned long i;
    char arg_default[JACK_DRIVER_PARAM_STRING_MAX + 1];

    for (i = 0; i < desc->nparams; i++) {
	switch (desc->params[i].type) {
	    case JackDriverParamInt:
		sprintf (arg_default, "%" PRId32, desc->params[i].value.i);
		break;
	    case JackDriverParamUInt:
		sprintf (arg_default, "%" PRIu32, desc->params[i].value.ui);
		break;
	    case JackDriverParamChar:
		sprintf (arg_default, "%c", desc->params[i].value.c);
		break;
	    case JackDriverParamString:
		if (desc->params[i].value.str &&
		    strcmp (desc->params[i].value.str, "") != 0)
		    sprintf (arg_default, "%s", desc->params[i].value.str);
		else
		    sprintf (arg_default, "none");
		break;
	    case JackDriverParamBool:
		sprintf (arg_default, "%s", desc->params[i].value.i ? "true" : "false");
		break;
	}

	fprintf (file, "\t-%c, --%s \t%s (default: %s)\n",
		 desc->params[i].character,
		 desc->params[i].name,
		 desc->params[i].short_desc,
		 arg_default);
    }
}

static void driver_params_usage_print( jack_driver_desc_t * desc, unsigned long param, FILE *file )
{
    fprintf( file, "Usage information for the '%s' parameter for driver '%s':\n",
	     desc->params[param].name, desc->name );

    fprintf( file, "%s\n", desc->params[param].long_desc );
}

bool drivers::driver_params_parse( jack_driver_desc_t * desc,
				   int argc,
				   char ** argv,
				   JSList ** param_ptr )
{
    struct option * long_options;
    char * options, * options_ptr;
    unsigned long i;
    int opt;
    uint32_t param_index;
    JSList * params = NULL;
    jack_driver_param_t * driver_param;

    if (argc <= 1) {
	*param_ptr = NULL;
	return false;
    }

    /* check for help */
    if (strcmp (argv[1], "-h") == 0 || strcmp (argv[1], "--help") == 0) {
	if (argc > 2) {
	    for (i = 0; i < desc->nparams; i++) {
		if (strcmp (desc->params[i].name, argv[2]) == 0) {
		    driver_params_usage_print( desc, i, stdout );
		    return false;
		}
	    }

	    fprintf (stderr, "jackd: unknown option '%s' "
		     "for driver '%s'\n", argv[2],
		     desc->name);
	}

	printf ("Parameters for driver '%s' (all parameters are optional):\n", desc->name);
	driver_params_options_print( desc, stdout );
	return false;
    }


    /* set up the stuff for getopt */
    options = (char*)calloc (desc->nparams*3 + 1, sizeof (char));
    long_options = (struct option*)calloc (desc->nparams + 1, sizeof( struct option ));

    options_ptr = options;
    for (i = 0; i < desc->nparams; i++) {
	sprintf (options_ptr, "%c::", desc->params[i].character);
	options_ptr += 3;

	long_options[i].name    = desc->params[i].name;
	long_options[i].flag    = NULL;
	long_options[i].val     = desc->params[i].character;
	long_options[i].has_arg = optional_argument;
    }

    /* create the params */
    optind = 0;
    opterr = 0;
    while ((opt = getopt_long(argc, argv, options, long_options, NULL)) != -1) {

	if (opt == ':' || opt == '?') {
	    if (opt == ':') {
		fprintf (stderr, "Missing option to argument '%c'\n", optopt);
	    } else {
		fprintf (stderr, "Unknownage with option '%c'\n", optopt);
	    }

	    fprintf (stderr, "Options for driver '%s':\n", desc->name);
	    driver_params_options_print( desc, stderr );
	    return false;
	}

	for (param_index = 0; param_index < desc->nparams; param_index++) {
	    if (opt == desc->params[param_index].character) {
		break;
	    }
	}

	driver_param = (jack_driver_param_t*)calloc (1, sizeof (jack_driver_param_t));

	driver_param->character = desc->params[param_index].character;

	if (!optarg && optind < argc &&
	    strlen(argv[optind]) &&
	    argv[optind][0] != '-') {
	    optarg = argv[optind];
	}

	if (optarg) {
	    switch (desc->params[param_index].type) {
		case JackDriverParamInt:
		    driver_param->value.i = atoi (optarg);
		    break;
		case JackDriverParamUInt:
		    driver_param->value.ui = strtoul (optarg, NULL, 10);
		    break;
		case JackDriverParamChar:
		    driver_param->value.c = optarg[0];
		    break;
		case JackDriverParamString:
		    strncpy (driver_param->value.str, optarg, JACK_DRIVER_PARAM_STRING_MAX);
		    break;
		case JackDriverParamBool:

		    if (strcasecmp ("false",  optarg) == 0 ||
			strcasecmp ("off",    optarg) == 0 ||
			strcasecmp ("no",     optarg) == 0 ||
			strcasecmp ("0",      optarg) == 0 ||
			strcasecmp ("(null)", optarg) == 0   ) {

			driver_param->value.i = FALSE;

		    } else {

			driver_param->value.i = TRUE;

		    }
		    break;
	    }
	} else {
	    if (desc->params[param_index].type == JackDriverParamBool) {
		driver_param->value.i = TRUE;
	    } else {
		driver_param->value = desc->params[param_index].value;
	    }
	}

	params = jack_slist_append (params, driver_param);
    }

    free (options);
    free (long_options);

    if (param_ptr)
	*param_ptr = params;

    return true;
}

}
