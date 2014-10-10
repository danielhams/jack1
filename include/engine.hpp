/* -*- mode: c++; c-file-style: "bsd"; -*- */
/*
    (Original file engine.h)
    Copyright (C) 2001-2003 Paul Davis
	(Modifications for C++ engine.hpp)
	Copyright (C) 2014 Daniel Hams
	    
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
#ifndef __jack_engine_hpp__
#define __jack_engine_hpp__

#include <jack/jack.h>
#include <internal.h>
#include <driver_interface.h>

#include <vector>
#include <memory>
#include <string>

// jack_engine_t * jack_engine_new_pp( int real_time, int real_time_priority,
std::unique_ptr<jack_engine_t> jack_engine_create( int real_time, int real_time_priority,
												   int do_mlock, int do_unlock,
												   const char *server_name, int temporary,
												   int verbose, int client_timeout,
												   unsigned int port_max,
												   pid_t waitpid, jack_nframes_t frame_time_offset, int nozombies, 
												   int timeout_count_threshold,
												   const std::vector<jack_driver_desc_t*> & loaded_drivers);

void jack_engine_cleanup( jack_engine_t * );

int jack_engine_load_driver_pp( jack_engine_t *engine,
								jack_driver_desc_t * driver_desc,
								JSList * driver_params_jsl );

#endif
