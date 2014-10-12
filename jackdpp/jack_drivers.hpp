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
#ifndef JACK_DRIVERS_HPP
#define JACK_DRIVERS_HPP

#include <string>
#include <vector>

#include "jack_engine.hpp"

namespace jack
{

std::vector<jack_driver_desc_t*> jack_drivers_load_pp( bool verbose );

jack_driver_desc_t * jack_drivers_find_descriptor_pp(
    const std::vector<jack_driver_desc_t*> & loaded_drivers,
    const std::string & name );

/* Should just be internal? */
jack_driver_desc_t * jack_drivers_find_so_descriptor_pp(
    const std::vector<jack_driver_desc_t*> & loaded_drivers,
    const std::string & so_name,
    bool verbose );

}

#endif
