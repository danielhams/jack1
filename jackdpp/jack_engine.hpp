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

#include "engine.hpp"

#include <stdarg.h>

/*
#ifdef __cplusplus
extern "C" {
#endif
*/

std::unique_ptr<jack_engine_t> jack_engine_create(
    const jack::jack_options & parsed_options,
    pid_t waitpid,
    const std::vector<jack_driver_desc_t*> & loaded_drivers );

void jack_engine_cleanup( jack_engine_t * );

int jack_engine_load_driver( jack_engine_t *engine,
			     jack_driver_desc_t * driver_desc,
			     JSList * driver_params_jsl );

int jack_engine_load_slave_driver( jack_engine_t *engine,
				   jack_driver_desc_t * driver_desc,
				   JSList * driver_params_jsl );

int jack_drivers_start (jack_engine_t *engine);
int jack_use_driver (jack_engine_t *engine, struct _jack_driver *driver);

int jack_deliver_event (jack_engine_t *, jack_client_internal_t *, const jack_event_t *, ...);

/*
#ifdef __cplusplus
}
#endif
*/

#endif
