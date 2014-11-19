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

#ifndef JACK_DRIVERS_HPP
#define JACK_DRIVERS_HPP

#include "internal.hpp"
#include "engine.hpp"

#include <string>
#include <vector>

namespace jack
{

class drivers
{
public:
    drivers( const bool verbose );

    inline const std::vector<jack_driver_desc_t*> & get_loaded_descs() const { return loaded_descs; };

    jack_driver_desc_t * find_desc_by_name( const std::string & name ) const;

    bool driver_params_parse( jack_driver_desc_t * desc,
			      int driver_nargs,
			      char ** driver_args,
			      JSList ** params_jsl );

private:
    std::vector<jack_driver_desc_t*> loaded_descs;
    bool verbose_;
};

}

#endif
