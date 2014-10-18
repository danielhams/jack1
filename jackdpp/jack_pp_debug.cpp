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
