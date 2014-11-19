/*
 *  Client creation and destruction interfaces for JACK engine.
 *
 *  Copyright (C) 2001-2003 Paul Davis
 *  Copyright (C) 2004 Jack O'Quin
 *  
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License as
 *  published by the Free Software Foundation; either version 2 of the
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
#ifndef __jackd_client_engine_h__
#define __jackd_client_engine_h__

#include "internal.hpp"

//#ifdef __cplusplus
//extern "C" {
//#endif

static inline int jack_client_is_internal (jack_client_internal_t *client)
{
    return (client->control->type == ClientInternal) ||
	(client->control->type == ClientDriver);
}

extern const char * client_state_names[];

static inline const char * jack_client_state_name (jack_client_internal_t *client)
{
    return client_state_names[client->control->state];
}

#define JACK_ERROR_WITH_SOCKETS 10000000

//#ifdef __cplusplus
///* Close of extern "C" { */
//}
//#endif

#endif
