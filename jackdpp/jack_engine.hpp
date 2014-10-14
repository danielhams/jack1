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
#ifndef JACK_ENGINE_HPP
#define JACK_ENGINE_HPP

#include <string>
#include <vector>
#include <memory>

#include "jack_options_parser.hpp"

#include "internal.hpp"
#include "engine.hpp"

#include <stdarg.h>

std::unique_ptr<jack_engine_t> jack_engine_create(
    const jack::jack_options & parsed_options,
    pid_t waitpid,
    const std::vector<jack_driver_desc_t*> & loaded_drivers );

void jack_engine_cleanup( jack_engine_t & );

int jack_engine_load_driver( jack_engine_t & engine,
			     jack_driver_desc_t * driver_desc,
			     JSList * driver_params_jsl );

int jack_engine_load_slave_driver( jack_engine_t & engine,
				   jack_driver_desc_t * driver_desc,
				   JSList * driver_params_jsl );

int jack_engine_drivers_start( jack_engine_t & engine );

int jack_engine_use_driver( jack_engine_t & engine, struct _jack_driver *driver );

int jack_engine_deliver_event( jack_engine_t &, jack_client_internal_t *, const jack_event_t *, ...);

void jack_engine_sort_graph( jack_engine_t & engine );

// private engine functions (are used by clients)
void jack_engine_reset_rolling_usecs( jack_engine_t & engine );

int internal_client_request( void * ptr, jack_request_t * request );

int jack_engine_get_fifo_fd( jack_engine_t & engine, unsigned int which_fifo );

/* Internal port handling interfaces for JACK engine. */

void jack_engine_port_clear_connections( jack_engine_t & engine, jack_port_internal_t *port );
void jack_engine_port_registration_notify( jack_engine_t &, jack_port_id_t, int );
void jack_engine_port_release( jack_engine_t & engine, jack_port_internal_t * );
int jack_engine_stop_freewheeling( jack_engine_t & engine, int engine_exiting );
jack_client_internal_t * jack_engine_client_by_name( jack_engine_t & engine, const char *name );

void jack_engine_signal_problems( jack_engine_t & engine );
int jack_engine_add_slave_driver( jack_engine_t & engine, struct _jack_driver *driver );

#endif
