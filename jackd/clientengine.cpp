/*
 *  Client creation and destruction interfaces for JACK engine.
 *
 *  Copyright (C) 2001-2003 Paul Davis
 *  Copyright (C) 2004 Jack O'Quin
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
 *
 */

#define __STDC_FORMAT_MACROS

#include <config.h>

#include <vector>
#include <string>
#include <sstream>

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include "internal.hpp"
#include "engine.hpp"
#include "messagebuffer.hpp"
#include "version.hpp"
#include "driver.hpp"
#include <sysdeps/poll.h>
#include <sysdeps/ipc.h>

#include "clientengine.hpp"
#include "transengine.hpp"

#include <jack/uuid.h>
#include <jack/metadata.h>

#include "libjack/local.hpp"

#include "jack_constants.hpp"
#include "jack_engine.hpp"

using std::string;
using std::stringstream;
using std::vector;

const char * client_state_names[] = {
    "Not triggered",
    "Triggered",
    "Running",
    "Finished"
};
