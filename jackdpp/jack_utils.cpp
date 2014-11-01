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

#include "jack_utils.hpp"

#include "internal.hpp"

#include <sstream>

#include <sys/types.h>
#include <dirent.h>

namespace jack
{

using std::string;
using std::stringstream;

static string internal_default_server_name;

const string & default_server_name()
{
    if( internal_default_server_name.size() == 0 ) {
	char * envsn = getenv("JACK_DEFAULT_SERVER");
	if( envsn == NULL ) {
	    internal_default_server_name = "default";
	}
	else {
	    internal_default_server_name = envsn;
	}
    }

    return internal_default_server_name;
}

static string internal_user_dir;

const string & user_dir()
{
    /* format the path name on the first call */
    if( internal_user_dir.size() == 0 ) {
	stringstream udss( stringstream::out );
	if (getenv ("JACK_PROMISCUOUS_SERVER")) {
	    udss << jack_tmpdir << "/jack";
	} else {
	    udss << jack_tmpdir << "/jack-" << getuid();
	}
	internal_user_dir = udss.str();
    }

    return internal_user_dir;
}

string server_dir( const string & server_name, const string & server_dir )
{
    /* format the path name into the suppled server_dir char array,
     * assuming that server_dir is at least as large as PATH_MAX+1 */
    stringstream snss( stringstream::out );

    if( server_name.size() == 0 ) {
	snss << user_dir() << '/' << default_server_name();
    }
    else {
	snss << user_dir() << '/' << server_name;
    }

    return snss.str();
}

void cleanup_files( const string & server_name )
{
    DIR *dir;
    struct dirent *dirent;
    char dir_name[PATH_MAX+1] = "";
    jack_server_dir( server_name.c_str(), dir_name );

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

}
