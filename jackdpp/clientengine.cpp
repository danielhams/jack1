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

#include "libjackpp/local.hpp"

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

static void jack_engine_client_disconnect_ports( jack_engine_t & engine,
						 jack_client_internal_t *client )
{
    /* call tree **** MUST HOLD *** engine->client_lock */

    for( jack_port_internal_t * port : client->ports_vector ) {
	jack_engine_port_clear_connections( engine, port);
	jack_engine_port_registration_notify( engine, port->shared->id, FALSE);
	jack_engine_port_release( engine, port );
    }

    client->truefeeds_vector.clear();
    client->sortfeeds_vector.clear();
    client->ports_vector.clear();
}			

int jack_engine_client_do_deactivate( jack_engine_t &engine,
				      jack_client_internal_t *client,
				      int sort_graph)
{
    /* caller must hold engine->client_lock and must have checked for and/or
     *   cleared all connections held by client.
     */
    VERBOSE( &engine,"+++ deactivate %s", client->control->name);

    client->control->active = FALSE;

    jack_transport_client_exit( engine, client );

    if( !jack_client_is_internal(client) &&
	engine.external_client_cnt > 0) {
	engine.external_client_cnt--;
    }
	
    if (sort_graph) {
	jack_engine_sort_graph( engine );
    }
    return 0;
}

static int jack_engine_load_client( jack_engine_t & engine, jack_client_internal_t *client,
				    const char *so_name)
{
    const char *errstr;

    if (!so_name) {
	return -1;
    }

    stringstream ss( stringstream::out );
    if (so_name[0] == '/') {
	/* Absolute, use as-is, user beware ... */
	ss << so_name << ".so";
    } else {
	ss << jack::addon_dir << '/' << so_name << ".so";
    }

    string path_to_so( ss.str() );
    const char * path_to_so_cstr = path_to_so.c_str();

    client->handle = dlopen( path_to_so_cstr, RTLD_NOW|RTLD_GLOBAL);
	
    if (client->handle == 0) {
	if ((errstr = dlerror ()) != 0) {
	    jack_error ("%s", errstr);
	} else {
	    jack_error ("bizarre error loading %s", so_name);
	}
	return -1;
    }

    client->initialize =
	(int (*)(jack_client_t*,const char*))
	dlsym (client->handle, "jack_initialize");

    if ((errstr = dlerror ()) != 0) {
	jack_error ("%s has no initialize() function\n", so_name);
	dlclose (client->handle);
	client->handle = 0;
	return -1;
    }

    client->finish = (void (*)(void *)) dlsym (client->handle,
					       "jack_finish");

    if ((errstr = dlerror ()) != 0) {
	jack_error ("%s has no finish() function", so_name);
	dlclose (client->handle);
	client->handle = 0;
	return -1;
    }

    return 0;
}

static void jack_client_unload( jack_client_internal_t *client )
{
    if (client->handle) {
	if (client->finish) {
	    client->finish( client->private_client->process_arg );
	}
	dlclose( client->handle );
    }
}

static void jack_engine_zombify_client( jack_engine_t & engine, jack_client_internal_t *client)
{
    VERBOSE( &engine, "removing client \"%s\" from the processing chain",
	     client->control->name);

    /* caller must hold the client_lock */

    /* this stops jack_engine_deliver_event() from contacing this client */

    client->control->dead = TRUE;

    jack_engine_client_disconnect_ports( engine, client );
    jack_engine_client_do_deactivate( engine, client, FALSE );
}

void jack_engine_remove_client_internal( jack_engine_t & engine, jack_client_internal_t *client )
{
    jack_uuid_t finalizer = JACK_UUID_EMPTY_INITIALIZER;

    jack_uuid_clear (&finalizer);

    /* caller must write-hold the client lock */

    VERBOSE( &engine, "removing client \"%s\"", client->control->name);

    if (client->control->type == ClientInternal) {
	/* unload it while its still a regular client */

	jack_client_unload (client);
    }

    /* if its not already a zombie, make it so */

    if (!client->control->dead) {
	jack_engine_zombify_client( engine, client);
    }

    if (client->session_reply_pending) {
	engine.session_pending_replies -= 1;

	if( engine.session_pending_replies == 0 ) {
	    if( write( engine.session_reply_fd, &finalizer, sizeof (finalizer))
		< (ssize_t) sizeof (finalizer)) {
		jack_error ("cannot write SessionNotify result "
			    "to client via fd = %d (%s)",
			    engine.session_reply_fd, strerror (errno));
	    }
	    engine.session_reply_fd = -1;
	}
    }

    if (client->control->type == ClientExternal) {

	/* try to force the server thread to return from poll */
	
	close (client->event_fd);
	close (client->request_fd);
    }

    VERBOSE( &engine, "before: client vector contains %d", engine.clients.size() );

    auto cFinder = std::find_if( engine.clients.begin(), engine.clients.end(),
				 [&client] ( jack_client_internal_t * ic ) {
				     return jack_uuid_compare( ic->control->uuid, client->control->uuid ) == 0;
				 } );

    if( cFinder != engine.clients.end() ) {
	engine.clients.erase( cFinder );
	VERBOSE( &engine, "Found and removed from client vector via matching UUID" );
    }

    VERBOSE( &engine, "after: client vector contains %d", engine.clients.size() );

    jack_engine_client_internal_delete( engine, client );

    if( engine.temporary ) {
	int external_clients = 0;

	/* count external clients only when deciding whether to shutdown */

	for( jack_client_internal_t * client : engine.clients ) {
	    if( client->control->type == ClientExternal ) {
		++external_clients;
	    }
	}

	if (external_clients == 0) {
	    if( engine.wait_pid >= 0 ) {
		/* block new clients from being created
		   after we release the lock.
		*/
		engine.new_clients_allowed = 0;
		/* tell the waiter we're done
		   to initiate a normal shutdown.
		*/
		VERBOSE( &engine, "Kill wait pid to stop");
		kill( engine.wait_pid, SIGUSR2 );
		/* unlock the graph so that the server thread can finish */
		jack_unlock_graph( (&engine) );
		sleep (-1);
	    } else {
		exit (0);
	    }
	}
    }
}

int jack_engine_check_clients( jack_engine_t & engine, int with_timeout_check)
{
    /* CALLER MUST HOLD graph read lock */
    int errs = 0;

    for( jack_client_internal_t * client : engine.clients ) {

	if (client->error) {
	    VERBOSE( &engine, "client %s already marked with error = %d\n", client->control->name, client->error);
	    errs++;
	    continue;
	}

	if (with_timeout_check) {

	    /* we can only consider the timeout a client error if
	     * it actually woke up.  its possible that the kernel
	     * scheduler screwed us up and never woke up the
	     * client in time. sigh.
	     */
			
	    VERBOSE( &engine, "checking client %s: awake at %" PRIu64 " finished at %" PRIu64, 
		     client->control->name,
		     client->control->awake_at,
		     client->control->finished_at);
			
	    if (client->control->awake_at > 0) {
		if (client->control->finished_at == 0) {
		    jack_time_t now = jack_get_microseconds();

		    if ((now - client->control->awake_at) < engine.driver->period_usecs) {
			/* we give the client a bit of time, to finish the cycle
			 * we assume here, that we dont get signals delivered to this thread.
			 */
			struct timespec wait_time;
			wait_time.tv_sec = 0;
			wait_time.tv_nsec = (engine.driver->period_usecs - (now - client->control->awake_at)) * 1000;
			VERBOSE( &engine, "client %s seems to have timed out. we may have mercy of %d ns.",
				 client->control->name, (int) wait_time.tv_nsec );
			nanosleep (&wait_time, NULL);
		    }

		    if (client->control->finished_at == 0) {
			client->control->timed_out++;
			client->error++;
			errs++;
			VERBOSE( &engine, "client %s has timed out", client->control->name);
		    } else {
			/*
			 * the client recovered. if this is a single occurence, thats probably fine.
			 * however, we increase the continuous_stream flag.
			 */

			engine.timeout_count += 1;
		    }
		}
	    }
	}
    }
		
    if (errs) {
	jack_engine_signal_problems( engine );
    }

    return errs;
}

void jack_engine_remove_clients( jack_engine_t & engine, int* exit_freewheeling_when_done )
{
    int need_sort = FALSE;
    /* CALLER MUST HOLD GRAPH LOCK */

    VERBOSE( &engine, "++ Removing failed clients ...");

    // Take a copy of the clients vector so we don't get invalid iterators
    vector<jack_client_internal_t*> clients_copy = engine.clients;

    /* remove all dead clients */
    for( jack_client_internal_t * client : clients_copy ) {

	VERBOSE( &engine, "client %s error status %d", client->control->name, client->error);
		
	if (client->error) {
			
	    if (engine.freewheeling && jack_uuid_compare (client->control->uuid, engine.fwclient) == 0) {
		VERBOSE( &engine, "freewheeling client has errors");
		*exit_freewheeling_when_done = 1;
	    }
			
	    /* if we have a communication problem with the
	       client, remove it. otherwise, turn it into
	       a zombie. the client will/should realize
	       this and will close its sockets.  then
	       we'll end up back here again and will
	       finally remove the client.
	    */
	    if (client->error >= JACK_ERROR_WITH_SOCKETS) {
		VERBOSE( &engine, "removing failed "
			 "client %s state = %s errors"
			 " = %d", 
			 client->control->name,
			 jack_client_state_name (client),
			 client->error);
		jack_engine_remove_client_internal( engine, client );
	    } else {
		VERBOSE( &engine, "client failure: "
			 "client %s state = %s errors"
			 " = %d", 
			 client->control->name,
			 jack_client_state_name (client),
			 client->error);
		if (!engine.nozombies) {
		    jack_engine_zombify_client( engine, client );
		    client->error = 0;
		}
	    }

	    need_sort = TRUE;
	}
    }

    if (need_sort) {
	jack_engine_sort_graph( engine );
    }
	
    jack_engine_reset_rolling_usecs( engine );

    VERBOSE( &engine, "-- Removing failed clients ...");
}

void jack_engine_client_registration_notify( jack_engine_t & engine,
					     const char* name, int yn)
{
    jack_event_t event;

    event.type = (yn ? ClientRegistered : ClientUnregistered);
    snprintf (event.x.name, sizeof (event.x.name), "%s", name);
	
    for( jack_client_internal_t * client : engine.clients ) {

	if (!client->control->active) {
	    continue;
	}

	if (strcmp ((char*) client->control->name, (char*) name) == 0) {
	    /* do not notify client of its own registration */
	    continue;
	}

	if (client->control->client_register_cbset) {
	    if (jack_engine_deliver_event( engine, client, &event)) {
		jack_error ("cannot send client registration"
			    " notification to %s (%s)",
			    client->control->name,
			    strerror (errno));
	    }
	}
    }
}

static int jack_engine_client_id_by_name( jack_engine_t & engine, const char *name, jack_uuid_t id)
{
    int ret = -1;

    jack_uuid_clear (&id);

    jack_rdlock_graph( (&engine) );

    for( jack_client_internal_t * client : engine.clients ) {
	if( strcmp((const char *)client->control->name, name) == 0) {
	    jack_uuid_copy (&id, client->control->uuid);
	    ret = 0;
	    break;
	}
    }

    jack_unlock_graph( (&engine) );
    return ret;
}

static int jack_engine_client_name_reserved( jack_engine_t & engine, const char *name )
{
    auto cnFinder = std::find_if( engine.reserved_client_names.begin(),
				  engine.reserved_client_names.end(),
				  [&name] ( jack_reserved_name_t * res ) {
				      return strcmp( res->name, name ) == 0;
				  } );

    if( cnFinder != engine.reserved_client_names.end() ) {
	return 1;
    }
    else {
	return 0;
    }
}

/* generate a unique client name
 *
 * returns 0 if successful, updates name in place
 */
static inline int jack_engine_generate_unique_name( jack_engine_t & engine, char *name)
{
    int tens, ones;
    int length = strlen (name);

    if (length > JACK_CLIENT_NAME_SIZE - 4) {
	jack_error ("%s exists and is too long to make unique", name);
	return 1;		/* failure */
    }

    /*  generate a unique name by appending "-01".."-99" */
    name[length++] = '-';
    tens = length++;
    ones = length++;
    name[tens] = '0';
    name[ones] = '1';
    name[length] = '\0';
    while (jack_engine_client_by_name( engine, name ) || jack_engine_client_name_reserved( engine, name )) {
	if (name[ones] == '9') {
	    if (name[tens] == '9') {
		jack_error ("client %s has 99 extra"
			    " instances already", name);
		return 1; /* give up */
	    }
	    name[tens]++;
	    name[ones] = '0';
	} else {
	    name[ones]++;
	}
    }
    return 0;
}

static int jack_engine_client_name_invalid( jack_engine_t & engine, char *name,
					    jack_options_t options, jack_status_t *status)
{
    /* Since this is always called from the server thread, no
     * other new client will be created at the same time.  So,
     * testing a name for uniqueness is valid here.  When called
     * from jack_engine_load_driver() this is not strictly true,
     * but that seems to be adequately serialized due to engine
     * startup.  There are no other clients at that point, anyway.
     */

    if( jack_engine_client_by_name( engine, name ) || jack_engine_client_name_reserved( engine, name )) {

	*status = (jack_status_t)(*status | JackNameNotUnique);

	if( options & JackUseExactName ) {
	    jack_error ("cannot create new client; %s already"
			" exists", name);
	    *status = (jack_status_t)(*status | JackFailure);
	    return TRUE;
	}

	if( jack_engine_generate_unique_name( engine, name) ) {
	    *status = (jack_status_t)(*status | JackFailure);
	    return TRUE;
	}
    }

    return FALSE;
}

/* Set up the engine's client internal and control structures for both
 * internal and external clients. */
static jack_client_internal_t * jack_engine_setup_client_control(
    jack_engine_t & engine, int fd, ClientType type, const char *name, jack_uuid_t uuid)
{
    jack_client_internal_t *client;

    client = new jack_client_internal_t();

    client->request_fd = fd;
    client->event_fd = -1;
    client->ports_vector.clear();
    client->truefeeds_vector.clear();
    client->sortfeeds_vector.clear();
    client->execution_order = UINT_MAX;
    client->next_client = NULL;
    client->handle = NULL;
    client->finish = NULL;
    client->error = 0;
    client->private_client = NULL;

    if (type != ClientExternal) {
	client->control = (jack_client_control_t *)malloc (sizeof (jack_client_control_t));		
    } else {
	if (jack_shmalloc (sizeof (jack_client_control_t), 
			   &client->control_shm)) {
	    jack_error ("cannot create client control block for %s",
			name);
	    delete client;
	    return 0;
	}

	if (jack_attach_shm(&client->control_shm)) {
	    jack_error ("cannot attach to client control block "
			"for %s (%s)", name, strerror (errno));
	    jack_destroy_shm(&client->control_shm);
	    delete client;
	    return 0;
	}

	client->control = (jack_client_control_t *)jack_shm_addr (&client->control_shm);
    }

    client->control->type = type;
    client->control->active = 0;
    client->control->dead = FALSE;
    client->control->timed_out = 0;

    if (jack_uuid_empty (uuid)) {
	client->control->uuid = jack_client_uuid_generate();
    } else {
	jack_uuid_t * jud = (jack_uuid_t*)(&client->control->uuid);
	jack_uuid_copy( jud, uuid );
    }

    strcpy ((char *) client->control->name, name);
    client->subgraph_start_fd = -1;
    client->subgraph_wait_fd = -1;

    client->session_reply_pending = FALSE;

    client->control->process_cbset = FALSE;
    client->control->bufsize_cbset = FALSE;
    client->control->srate_cbset = FALSE;
    client->control->xrun_cbset = FALSE;
    client->control->port_register_cbset = FALSE;
    client->control->port_connect_cbset = FALSE;
    client->control->graph_order_cbset = FALSE;
    client->control->client_register_cbset = FALSE;
    client->control->thread_cb_cbset = FALSE;
    client->control->session_cbset = FALSE;
    client->control->property_cbset = FALSE;
    client->control->latency_cbset = FALSE;

#if 0
    if (type != ClientExternal) {
	client->process = NULL;
	client->process_arg = NULL;
	client->bufsize = NULL;
	client->bufsize_arg = NULL;
	client->srate = NULL;
	client->srate_arg = NULL;
	client->xrun = NULL;
	client->xrun_arg = NULL;
	client->port_register = NULL;
	client->port_register_arg = NULL;
	client->port_connect = NULL;
	client->port_connect_arg = NULL;
	client->graph_order = NULL;
	client->graph_order_arg = NULL;
	client->client_register = NULL;
	client->client_register_arg = NULL;
	client->thread_cb = NULL;
	client->thread_cb_arg = NULL;
    }
#endif
    jack_transport_client_new(client);
        
    return client;
}

static void jack_engine_ensure_uuid_unique( jack_engine_t & engine, jack_uuid_t uuid )
{
    if (jack_uuid_empty (uuid)) {
	return;
    }

    jack_lock_graph( (&engine) );

    for( jack_client_internal_t * client : engine.clients ) {
	if( jack_uuid_compare( client->control->uuid, uuid ) == 0 ) {
	    jack_uuid_clear( &uuid );
	}
    }
    jack_unlock_graph( (&engine) );
}

/*
 * Build the jack_client_t structure for an internal client.
 */
jack_client_t * jack_engine_internal_client_alloc( jack_engine_t & engine, jack_client_control_t *cc )
{
	jack_client_t* client;

	client = jack_client_alloc ();

	client->control = cc;
	client->engine = engine.control;

	client->n_port_types = client->engine->n_port_types;
	client->port_segment = &engine.port_segment[0];

	return client;
}

/* set up all types of clients */
static jack_client_internal_t * jack_engine_setup_client(
    jack_engine_t & engine, ClientType type, char *name, jack_uuid_t uuid, jack_options_t options,
    jack_status_t *status, int client_fd, const char *object_path, const char *object_data)
{
    /* called with the request_lock */
    jack_client_internal_t *client;
    char bufx[64];

    /* validate client name, generate a unique one if appropriate */
    if( jack_engine_client_name_invalid( engine, name, options, status))
	return NULL;

    jack_engine_ensure_uuid_unique( engine, uuid);

    /* create a client struct for this name */
    if ((client = jack_engine_setup_client_control( engine, client_fd,
						    type, name, uuid)) == NULL) {
	*status = (jack_status_t)(*status | (JackFailure|JackInitFailure));
	jack_error ("cannot create new client object");
	return NULL;
    }

    /* only for internal clients, driver is already loaded */
    if (type == ClientInternal) {
	if (jack_engine_load_client( engine, client, object_path) ) {
	    jack_error ("cannot dynamically load client from"
			" \"%s\"", object_path);
	    jack_engine_client_internal_delete( engine, client );
	    *status = (jack_status_t)(*status | (JackFailure|JackLoadFailure));
	    return NULL;
	}
    }

    jack_uuid_unparse(client->control->uuid, bufx);
                
    VERBOSE( &engine, "new client: %s, uuid = %s" 
	     " type %d @ %p fd = %d", 
	     client->control->name, bufx,
	     type, client->control, client_fd);

    if (jack_client_is_internal(client)) {

	// XXX: do i need to lock the graph here ?
	// i moved this one up in the init process, lets see what happens.

	/* Internal clients need to make regular JACK API
	 * calls, which need a jack_client_t structure.
	 * Create one here.
	 */
	client->private_client = jack_engine_internal_client_alloc( engine, client->control );

	/* Set up the pointers necessary for the request
	 * system to work.  The client is in the same address
	 * space */

	client->private_client->deliver_request = internal_client_request;
	client->private_client->deliver_arg = &engine;
    }

    /* add new client to the clients list */
    jack_lock_graph( (&engine) );

    engine.clients.push_back( client );

    jack_engine_reset_rolling_usecs( engine );
	
    if (jack_client_is_internal(client)) {

	jack_unlock_graph( (&engine) );

	/* Call its initialization function.  This function
	 * may make requests of its own, so we temporarily
	 * release and then reacquire the request_lock.  */
	if (client->control->type == ClientInternal) {

	    pthread_mutex_unlock(&engine.request_lock);
	    if (client->initialize(client->private_client,
				    object_data)) {

		/* failed: clean up client data */
		VERBOSE( &engine, "%s jack_initialize() failed!", client->control->name);
		jack_lock_graph( (&engine) );
		jack_engine_remove_client_internal( engine, client );
		jack_unlock_graph( (&engine) );
		*status = (jack_status_t)(*status | (JackFailure|JackInitFailure));
		client = NULL;
		//JOQ: not clear that all allocated
		//storage has been cleaned up properly.
	    }
	    pthread_mutex_lock(&engine.request_lock);
	}

    } else {			/* external client */
	jack_unlock_graph( (&engine) );
    }

    return client;
}

jack_client_internal_t * jack_engine_create_driver_client_internal( jack_engine_t & engine, char *name )
{
    jack_client_connect_request_t req;
    jack_status_t status;
    jack_client_internal_t *client;
    jack_uuid_t empty_uuid = JACK_UUID_EMPTY_INITIALIZER;

    VALGRIND_MEMSET(&empty_uuid, 0, sizeof(empty_uuid));

    snprintf (req.name, sizeof (req.name), "%s", name);

    jack_uuid_clear (&empty_uuid);

    pthread_mutex_lock( &engine.request_lock );
    client = jack_engine_setup_client( engine, ClientDriver, name, empty_uuid, JackUseExactName,
				       &status, -1, NULL, NULL);
    pthread_mutex_unlock( &engine.request_lock );

    return client;
}

static jack_status_t jack_engine_handle_unload_client( jack_engine_t & engine, jack_uuid_t id)
{
    /* called *without* the request_lock */
    jack_client_internal_t *client;
    jack_status_t status = (jack_status_t)(JackNoSuchClient|JackFailure);

    jack_lock_graph( (&engine) );

    if ((client = jack_engine_client_internal_by_id( engine, id))) {
	VERBOSE( &engine, "unloading client \"%s\"",
		 client->control->name);
	if (client->control->type != ClientInternal) {
	    status = (jack_status_t)(JackFailure|JackInvalidOption);
	} else {
	    jack_engine_remove_client_internal( engine, client );
	    status = (jack_status_t)0;
	}
    }

    jack_unlock_graph( (&engine) );

    return status;
}

static char * jack_engine_get_reserved_name (jack_engine_t & engine, jack_uuid_t uuid)
{
    auto rnFinder = std::find_if( engine.reserved_client_names.begin(),
				  engine.reserved_client_names.end(),
				  [&uuid] ( jack_reserved_name_t * res ) {
				      return jack_uuid_compare( res->uuid, uuid ) == 0;
				  } );

    if( rnFinder != engine.reserved_client_names.end() ) {
	jack_reserved_name_t * res = *rnFinder;
	char * retval = strdup( res->name );
	engine.reserved_client_names.erase( rnFinder );
	free( res );
	return retval;
    }
    else {
	return 0;
    }
}

int jack_engine_client_create( jack_engine_t & engine, int client_fd )
{
    /* called *without* the request_lock */
    jack_client_internal_t *client;
    jack_client_connect_request_t req;
    jack_client_connect_result_t res;
    ssize_t nbytes;

    res.status = (jack_status_t)0;

    VALGRIND_MEMSET(&res, 0, sizeof (res));

    nbytes = read (client_fd, &req, sizeof (req));
        
    if (nbytes == 0) {		/* EOF? */
	jack_error ("cannot read connection request from client (%s)", strerror(errno));
	return -1;
    }

    /* First verify protocol version (first field of request), if
     * present, then make sure request has the expected length. */
    if (((size_t)nbytes < sizeof (req.protocol_v))
	|| (req.protocol_v != jack_protocol_version)
	|| (nbytes != sizeof (req))) {

	/* JACK protocol incompatibility */
	res.status = (jack_status_t)(res.status | (JackFailure|JackVersionError));
	jack_error ("JACK protocol mismatch (%d vs %d)", req.protocol_v, jack_protocol_version);
	if (write (client_fd, &res, sizeof (res)) != sizeof (res)) {
	    jack_error ("cannot write client connection response");
	}
	return -1;
    }

    if (!req.load) {		/* internal client close? */

	int rc = -1;
	jack_uuid_t id = JACK_UUID_EMPTY_INITIALIZER;

	if (jack_engine_client_id_by_name( engine, req.name, id) == 0) {
	    rc = jack_engine_handle_unload_client( engine, id );
	}

	/* close does not send a reply */
	return rc;
    }
	
    pthread_mutex_lock( &engine.request_lock );
    if (!jack_uuid_empty (req.uuid)) {
	char *res_name = jack_engine_get_reserved_name( engine, req.uuid );
	if (res_name) {
	    snprintf (req.name, sizeof(req.name), "%s", res_name);
	    free (res_name);
	}
    }

    client = jack_engine_setup_client( engine, req.type, req.name, req.uuid,
				       req.options, &res.status, client_fd,
				       req.object_path, req.object_data);
    pthread_mutex_unlock(&engine.request_lock);
    if (client == NULL) {
	res.status = (jack_status_t)(res.status | JackFailure); /* just making sure */
	return -1;
    }
    res.client_shm_index = client->control_shm.index;
    res.engine_shm_index = engine.control_shm.index;
    res.realtime = engine.control->real_time;
    res.realtime_priority = engine.rtpriority - 1;
    strncpy (res.name, req.name, sizeof(res.name));

    if (jack_client_is_internal(client)) {
	/* the ->control pointers are for an internal client
	   so we know they are the right sized pointers
	   for this server. however, to keep the result
	   structure the same size for both 32 and 64 bit
	   clients/servers, the result structure stores
	   them as 64 bit integer, so we have to do a slightly
	   forced cast here.
	*/
	res.client_control = (uint64_t) ((intptr_t) client->control);
	res.engine_control = (uint64_t) ((intptr_t) engine.control);
    } else {
	strcpy (res.fifo_prefix, engine.fifo_prefix);
    }

    if (write (client_fd, &res, sizeof (res)) != sizeof (res)) {
	jack_error ("cannot write connection response to client");
	jack_lock_graph( (&engine) );
	client->control->dead = 1;
	jack_engine_remove_client_internal( engine, client );
	jack_unlock_graph( (&engine) );
	return -1;
    }

    if (jack_client_is_internal(client)) {
	close (client_fd);
    }

    jack_engine_client_registration_notify( engine, (const char*) client->control->name, 1 );

    return 0;
}

int jack_engine_client_activate( jack_engine_t & engine, jack_uuid_t id )
{
    jack_client_internal_t *client;
    int ret = -1;
    int i;
    jack_event_t event;

    VALGRIND_MEMSET(&event, 0, sizeof(event));

    jack_lock_graph( (&engine) );

    if ((client = jack_engine_client_internal_by_id( engine, id)))
    {
	client->control->active = TRUE;

	jack_transport_activate( engine, client );

	/* we call this to make sure the FIFO is
	 * built+ready by the time the client needs
	 * it. we don't care about the return value at
	 * this point.
	 */

	jack_engine_get_fifo_fd( engine, ++engine.external_client_cnt);
	jack_engine_sort_graph( engine );


	for (i = 0; i < engine.control->n_port_types; ++i) {
	    event.type = AttachPortSegment;
	    event.y.ptid = i;
	    jack_engine_deliver_event( engine, client, &event);
	}

	event.type = BufferSizeChange;
	event.x.n = engine.control->buffer_size;
	jack_engine_deliver_event( engine, client, &event);

	// send delayed notifications for ports.
	for( jack_port_internal_t * port : client->ports_vector ) {
	    jack_engine_port_registration_notify( engine, port->shared->id, TRUE);
	}

	ret = 0;
    }


    jack_unlock_graph( (&engine) );
    return ret;
}	

int jack_engine_client_deactivate( jack_engine_t & engine, jack_uuid_t id )
{
    int ret = -1;

    jack_lock_graph( (&engine) );

    for( jack_client_internal_t * client : engine.clients ) {
	if( jack_uuid_compare( client->control->uuid, id) == 0 ) {
	    for( jack_port_internal_t * port : client->ports_vector ) {
		jack_engine_port_clear_connections( engine, port );
	    }

	    ret = jack_engine_client_do_deactivate( engine, client, TRUE);
	    break;
	}
    }

    jack_unlock_graph( (&engine) );

    return ret;
}	


int jack_engine_mark_client_socket_error( jack_engine_t & engine, int fd )
{
    /* CALLER MUST HOLD GRAPH LOCK */

    auto cFinder = std::find_if( engine.clients.begin(),
				 engine.clients.end(),
				 [&fd] ( jack_client_internal_t * client ) {
				     if( jack_client_is_internal( client ) ) {
					 return false;
				     }
				     else {
					 return client->request_fd == fd;
				     }
				 } );

    if( cFinder != engine.clients.end() ) {
	jack_client_internal_t * client = *cFinder;
	VERBOSE( &engine, "marking client %s with SOCKET error state = "
		 "%s errors = %d", client->control->name,
		 jack_client_state_name (client),
		 client->error);
	client->error += JACK_ERROR_WITH_SOCKETS;
    }

    return 0;
}

void jack_engine_client_internal_delete( jack_engine_t & engine, jack_client_internal_t *client )
{
    jack_uuid_t uuid = JACK_UUID_EMPTY_INITIALIZER;
    jack_uuid_copy (&uuid, client->control->uuid);

    jack_engine_client_registration_notify( engine, (const char*) client->control->name, 0);

    jack_remove_properties(NULL, uuid);
    /* have to do the notification ourselves, since the client argument
       to jack_remove_properties() was NULL
    */
    jack_engine_property_change_notify( engine, PropertyDeleted, uuid, NULL);

    if (jack_client_is_internal(client)) {
	delete client->private_client;
	if( client->control ) {
	    free( (void*)client->control);
	}

    } else {

	/* release the client segment, mark it for
	   destruction, and free up the shm registry
	   information so that it can be reused.
	*/

	jack_release_shm (&client->control_shm);
	jack_destroy_shm (&client->control_shm);
    }

    delete client;
}

void jack_engine_intclient_handle_request( jack_engine_t & engine, jack_request_t *req )
{
    jack_client_internal_t *client;

    req->status = 0;
    if ((client = jack_engine_client_by_name( engine, req->x.intclient.name))) {
	jack_uuid_copy (&req->x.intclient.uuid, client->control->uuid);
    } else {
	req->status = (jack_status_t)(req->status | (JackNoSuchClient|JackFailure));
    }
}

void jack_engine_intclient_load_request( jack_engine_t & engine, jack_request_t *req )
{
    /* called with the request_lock */
    jack_client_internal_t *client;
    jack_status_t status = (jack_status_t)0;
    jack_uuid_t empty_uuid = JACK_UUID_EMPTY_INITIALIZER;

    VERBOSE( &engine, "load internal client %s from %s, init `%s', "
	     "options: 0x%x", req->x.intclient.name,
	     req->x.intclient.path, req->x.intclient.init,
	     req->x.intclient.options);

    jack_uuid_clear (&empty_uuid);

    jack_options_t client_options = (jack_options_t)(req->x.intclient.options | JackUseExactName);

    client = jack_engine_setup_client( engine, ClientInternal, req->x.intclient.name, empty_uuid,
				       client_options, &status, -1,
				       req->x.intclient.path, req->x.intclient.init);

    if (client == NULL) {
	status = (jack_status_t)(status | JackFailure);	/* just making sure */
	jack_uuid_clear (&req->x.intclient.uuid);
	VERBOSE( &engine, "load failed, status = 0x%x", status);
    } else {
	jack_uuid_copy (&req->x.intclient.uuid, client->control->uuid);
    }

    req->status = status;
}

void jack_engine_intclient_name_request( jack_engine_t & engine, jack_request_t *req)
{
    jack_client_internal_t *client;

    jack_rdlock_graph( (&engine) );
    if ((client = jack_engine_client_internal_by_id( engine,
						     req->x.intclient.uuid))) {
	strncpy ((char *) req->x.intclient.name,
		 (char *) client->control->name,
		 sizeof (req->x.intclient.name));
	req->status = 0;
    } else {
	req->status = (JackNoSuchClient|JackFailure);
    }
    jack_unlock_graph( (&engine) );
}

void jack_engine_intclient_unload_request( jack_engine_t & engine, jack_request_t *req )
{
    /* Called with the request_lock, but we need to call
     * jack_engine_handle_unload_client() *without* it. */

    if (!jack_uuid_empty (req->x.intclient.uuid)) {
	/* non-empty UUID */
	pthread_mutex_unlock(&engine.request_lock);
	req->status = jack_engine_handle_unload_client( engine, req->x.intclient.uuid);
	pthread_mutex_lock(&engine.request_lock);
    } else {
	VERBOSE( &engine, "invalid unload request");
	req->status = JackFailure;
    }
}
