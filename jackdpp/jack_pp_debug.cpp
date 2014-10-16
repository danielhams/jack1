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

#include "jack_pp_debug.hpp"

#include <string>
#include <iostream>
#include <sstream>
#include <vector>

#include "internal.hpp"
#include "driver.hpp"
#include "jack/uuid.h"
#include "shm.hpp"
#include "engine.hpp"
#include "transengine.hpp"
#include "clientengine.hpp"
#include "jack/types.h"
#include "messagebuffer.hpp"
#include "engine.hpp"

#include "libjackpp/local.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <poll.h>
#include <stdarg.h>

using std::cout;
using std::endl;
using std::string;
using std::vector;
using std::stringstream;
using std::unique_ptr;
using std::make_unique;

void CHECK_CONNECTIONS_VECTOR_MATCHES(
    const char * source,
    std::vector<jack_connection_internal_t*> & connections_vector,
    JSList * connections_jsl )
{
    auto cv_iterator = connections_vector.begin();
    uint32_t cv_count { 0 };

    JSList * cj_iterator = connections_jsl;
    uint32_t cj_count { 0 };

    bool done { false };
    bool was_error { false };
    uint32_t cur_el_num { 0 };

    while( !done ) {
	cur_el_num++;

	if( cj_iterator != NULL ) {
	    cj_count++;
	}
	if( cv_iterator != connections_vector.end() ) {
	    cv_count++;
	}

	if( cj_iterator != NULL && cv_iterator != connections_vector.end() ) {
	    jack_connection_internal_t * cj_data = (jack_connection_internal_t*)cj_iterator->data;
	    jack_connection_internal_t * cv_data = *cv_iterator;

//	    char * cj_name = (char*)cj_data->control->name;
//	    char * cv_name = (char*)cv_data->control->name;
//	    MESSAGE("(%s) (%d) comparing clients (%s-%p)(%s-%p)", source,
//		      cur_el_num,
//		      cj_name, cj_data,
//		      cv_name, cv_data );
	    if( cj_data != cv_data ) {
		was_error = true;
	    }
	}

	if( was_error ) {
	    MESSAGE("(%s) (%d) failed connection element check", source, cur_el_num );
	}

	if( cj_iterator != NULL ) {
	    cj_iterator = cj_iterator->next;
	}
	if( cv_iterator != connections_vector.end() ) {
	    cv_iterator++;
	}

	if( cj_iterator == NULL && cv_iterator == connections_vector.end() ) {
	    done = true;
	}
    }

    if( !was_error && cv_count == cj_count ) {
	MESSAGE("(%s) Success! connections list and vector matches cjCount(%d) cvCount(%d)", source,
		  cj_count, cv_count);
    }
    else {
	MESSAGE("(%s) Failed during connections list match cjCount(%d) cvCount(%d)", source,
		   cj_count, cv_count );
    }
}
