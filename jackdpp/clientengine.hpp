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

int	jack_engine_client_activate( jack_engine_t & engine, jack_uuid_t id);
int	jack_engine_client_deactivate( jack_engine_t & engine, jack_uuid_t id);
int	jack_engine_client_create( jack_engine_t & engine, int client_fd);
void	jack_engine_client_internal_delete( jack_engine_t & engine,
					    jack_client_internal_t *client);
int	jack_engine_mark_client_socket_error( jack_engine_t & engine, int fd);
jack_client_internal_t *
	jack_engine_create_driver_client_internal( jack_engine_t & engine, char *name);
void	jack_engine_intclient_handle_request( jack_engine_t & engine,
				       jack_request_t *req);
void	jack_engine_intclient_load_request( jack_engine_t & engine,
				     jack_request_t *req);
void	jack_engine_intclient_name_request( jack_engine_t & engine,
				     jack_request_t *req);
void	jack_engine_intclient_unload_request( jack_engine_t & engine,
				       jack_request_t *req);
int	jack_engine_check_clients( jack_engine_t & engine, int with_timeout_check);

void	jack_engine_remove_clients( jack_engine_t & engine, int* exit_freewheeling);

void	jack_engine_client_registration_notify( jack_engine_t & engine, const char* name, int yn );

void    jack_engine_remove_client_internal( jack_engine_t & engine, jack_client_internal_t *client );

//#ifdef __cplusplus
///* Close of extern "C" { */
//}
//#endif

#endif
