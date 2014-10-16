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
#ifndef JACK_CONSTANTS_HPP
#define JACK_CONSTANTS_HPP

#include <config.h>

namespace jack
{

constexpr const char * addon_dir = ADDONPP_DIR;
constexpr const char * default_tmp_dir = DEFAULT_TMP_DIR;
constexpr const char * jack_location = JACK_LOCATION;

}

#endif
