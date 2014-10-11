/*
  Copyright (C) 2014- Daniel Hams
    
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

#include <config.h>

#include "jack_drivers.hpp"
#include "jack_cpp_utils.hpp"

#include "driver.h"

#include <errno.h>
#include <dirent.h>
#include <string.h>

#include <cstdio>
#include <iostream>
#include <sstream>

namespace jack
{

using std::string;
using std::vector;
using std::stringstream;

using jack::addon_dir;

vector<jack_driver_desc_t*> jack_drivers_load_pp( bool verbose )
{
    vector<jack_driver_desc_t*> loaded_drivers;

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
	return loaded_drivers;
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

	desc = jack_drivers_find_descriptor_pp( loaded_drivers, dir_entry->d_name );

	if (desc) {
	    loaded_drivers.push_back( desc );
	}
    }

    err = closedir( dir_stream );
    if (err) {
	jack_error("error closing driver directory %s: %s\n",
		    driver_dir, strerror (errno));
    }

    if (loaded_drivers.size() == 0) {
	jack_error("could not find any drivers in %s!\n", driver_dir);
    }

    return loaded_drivers;
}

jack_driver_desc_t * jack_drivers_find_descriptor_pp(
    const vector<jack_driver_desc_t*> & loaded_drivers,
    const string & name )
{
    for( jack_driver_desc_t * desc : loaded_drivers ) {
	if( desc->name == name ) {
	    return desc;
	}
    }

    return NULL;
}

jack_driver_desc_t * jack_drivers_find_so_descriptor_pp(
    const std::vector<jack_driver_desc_t*> & loaded_drivers,
    const std::string & so_name,
    bool verbose )
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

    so_get_descriptor = (JackDriverDescFunction)
	dlsym( dlhandle, "driver_get_descriptor" );

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
    for( jack_driver_desc_t * other_descriptor : loaded_drivers ) {
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

}
