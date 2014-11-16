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

#ifndef JACK_SIGNALS_HPP
#define JACK_SIGNALS_HPP

#include <config.h>
#include <signal.h>

#include "jack_engine.hpp"

namespace jack
{

sigset_t jack_signals_create();
sigset_t jack_signals_block();
void jack_signals_unblock( sigset_t & signals );
void jack_signals_install_do_nothing_action( sigset_t & signals );
int jack_signals_wait( sigset_t & signals, jack_engine_t * engine );
int jack_signals_wait_pp( sigset_t & signals, jack::engine * engine );

}

#endif
