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

#ifndef JACKD_UTILS_HPP
#define JACKD_UTILS_HPP

#include <config.h>

#include <string>

namespace jack
{

const std::string & server_default_name();
const std::string & server_user_dir();
const std::string & server_tmp_dir();
const std::string & server_dir( const std::string & server_name );

const std::string & client_user_dir();
const std::string & client_tmp_dir();

void cleanup_files( const std::string & server_name );

}

#endif // JACKD_UTILS_HPP
