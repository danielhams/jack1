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

#include <internal.hpp>

#include <sstream>
#include <vector>
#include <iostream>

#include <sys/types.h>
#include <dirent.h>
#include <string.h>

namespace jack
{

using std::string;
using std::stringstream;
using std::vector;

using std::cout;
using std::endl;

static string internal_server_default_name;

const string & server_default_name()
{
    if( internal_server_default_name.size() == 0 ) {
	char * envsn = getenv("JACK_DEFAULT_SERVER");
	if( envsn == NULL ) {
	    internal_server_default_name = "default";
	}
	else {
	    internal_server_default_name = envsn;
	}
    }

    return internal_server_default_name;
}

static string internal_server_user_dir;

const string & server_user_dir()
{
    /* format the path name on the first call */
    if( internal_server_user_dir.size() == 0 ) {
	stringstream udss( stringstream::out );
	if (getenv ("JACK_PROMISCUOUS_SERVER")) {
	    udss << server_tmp_dir() << "/jack";
	} else {
	    udss << server_tmp_dir() << "/jack-" << getuid();
	}
	internal_server_user_dir = udss.str();
    }

    return internal_server_user_dir;
}

template < class CONTAINER >
void string_tokenize( const std::string & str,
		      CONTAINER & tokens,
		      const std::string & delimiters = " ",
		      bool trimEmpty = false )
{
    string::size_type pos, lastPos = 0;

    using value_type = typename CONTAINER::value_type;
    using size_type  = typename CONTAINER::size_type;

    while(true)
    {
	pos = str.find_first_of(delimiters, lastPos);
	if(pos == string::npos) {
	    pos = str.length();

	    if(pos != lastPos || !trimEmpty) {
		tokens.push_back(value_type(str.data()+lastPos,
					    (size_type)pos-lastPos ));
	    }

	    break;
	}
	else {
	    if(pos != lastPos || !trimEmpty) {
		tokens.push_back(value_type(str.data()+lastPos,
					    (size_type)pos-lastPos ));
	    }
	}
	lastPos = pos + 1;
    }
};

static string internal_server_tmp_dir;

const string & server_tmp_dir()
{
    if( internal_server_tmp_dir.size() == 0 ) {
	internal_server_tmp_dir = DEFAULT_TMP_DIR;
    }

    return internal_server_tmp_dir;
}

static string internal_server_dir;

const string & server_dir( const string & server_name )
{
    if( internal_server_dir.size() == 0 ) {
	/* format the path name into the suppled server_dir char array,
	 * assuming that server_dir is at least as large as PATH_MAX+1 */
	stringstream snss( stringstream::out );

	if( server_name.size() == 0 ) {
	    snss << server_user_dir() << '/' << server_default_name();
	}
	else {
	    snss << server_user_dir() << '/' << server_name;
	}
	internal_server_dir = snss.str();
    }

    return internal_server_dir;
}

static string internal_client_user_dir;

const string & client_user_dir()
{
    /* format the path name on the first call */
    if( internal_client_user_dir.size() == 0 ) {
	stringstream udss( stringstream::out );
	if (getenv ("JACK_PROMISCUOUS_SERVER")) {
	    udss << client_tmp_dir() << "/jack";
	} else {
	    udss << client_tmp_dir() << "/jack-" << getuid();
	}
	internal_client_user_dir = udss.str();
    }

    return internal_client_user_dir;
}

static string internal_client_tmp_dir;

const string & client_tmp_dir()
{
    if( internal_client_tmp_dir.size() == 0 ) {
	internal_client_tmp_dir = server_tmp_dir();

	/* allow tmpdir to live anywhere, plus newline, plus null */
	stringstream tdss( stringstream::out );
	
	FILE* in;
	size_t len;
	string buf( PATH_MAX+2, ' ');
	const char *pathenv;

	/* some implementations of popen(3) close a security loophole by
	   resetting PATH for the exec'd command. since we *want* to
	   use the user's PATH setting to locate jackd, we have to
	   do it ourselves.
	*/

	if ((pathenv = getenv("PATH")) == 0) {
		return internal_client_tmp_dir;
	}

	// Don't play with the real environment variable
	string pathenv_str = pathenv;

	// split up by separator
	vector<string> tokens;
	string_tokenize( pathenv_str, tokens, ":", false );

	bool success = false;

	for( auto & token : tokens ) {
	    stringstream jackdss( stringstream::out );
	    jackdss << token << "/jackd";
	    string jackd = jackdss.str();
	    if( access( jackd.c_str(), X_OK ) == 0 ) {
		stringstream commandss( stringstream::out );
		commandss << jackd << " -l";
		string command = commandss.str();
		if( (in = popen( command.c_str(), "r")) != NULL ) {
		    success = true;
		    break;
		}
	    }
	}

	if( success ) {
	    if( fgets( (char*)buf.data(), buf.length(), in ) == NULL ) {
		pclose( in );
	    }
	    else {
		len = strlen( buf.c_str() );

		if (buf[len-1] == '\n') {
		    internal_client_tmp_dir = string( &buf[0], &buf[len-1] );
		}
		else {
		    /* didn't get a whole line */
		}
		pclose (in);
	    }
	}
    }

    return internal_client_tmp_dir;
}

void cleanup_files( const string & server_name )
{
    DIR *dir;
    struct dirent *dirent;

    string server_dir_str = server_dir( server_name );

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
    if( (dir = opendir( server_dir_str.c_str() ) ) == NULL) {
	return;
    }

    /* unlink all the files in this directory, they are mine */
    while( (dirent = readdir(dir)) != NULL ) {
	if( (strcmp (dirent->d_name, ".") == 0)
	    || (strcmp (dirent->d_name, "..") == 0) ) {
	    continue;
	}

	stringstream ss( stringstream::out );
	ss << server_dir_str << '/' << dirent->d_name;

	string fullpath( ss.str() );

	if( unlink(fullpath.c_str()) ) {
	    jack_error( "cannot unlink `%s' (%s)", fullpath.c_str(),
			strerror(errno) );
	}
    } 

    closedir( dir );

    /* now, delete the per-server subdirectory, itself */
    if( rmdir( server_dir_str.c_str() ) ) {
	jack_error( "cannot remove `%s' (%s)", server_dir_str.c_str(),
		    strerror (errno) );
    }

    /* finally, delete the per-user subdirectory, if empty */
    const string & server_user_dir_str = server_user_dir();
    if( rmdir( server_user_dir_str.c_str() ) ) {
	if( errno != ENOTEMPTY ) {
	    jack_error( "cannot remove `%s' (%s)",
			server_user_dir_str.c_str(), strerror( errno ) );
	}
    }
}

} // end jack namespace
