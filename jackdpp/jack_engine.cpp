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

#define __STDC_FORMAT_MACROS

#include <config.h>

#include "jack_engine.hpp"
#include "jack_signals.hpp"

#include <string>
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

using std::endl;
using std::string;
using std::vector;
using std::stringstream;
using std::unique_ptr;
using std::make_unique;

typedef jack_driver_t * (*jack_driver_info_init_callback_t)(jack_client_t*, const JSList *);
typedef void (*jack_driver_info_finish_callback_t)(jack_driver_t*);

typedef struct _jack_driver_info {
    jack_driver_info_init_callback_t initialize;
    jack_driver_info_finish_callback_t finish;
    char           (*client_name);
    dlhandle       handle;
} jack_driver_info_t;

jack_timer_type_t clock_source = JACK_TIMER_SYSTEM_CLOCK;

static int jack_engine_port_assign_buffer( jack_engine_t &,
				    jack_port_internal_t *);
static jack_port_internal_t * jack_engine_get_port_by_name( jack_engine_t &,
							    const char *name);
static int jack_engine_rechain_graph( jack_engine_t & engine );
static void jack_engine_clear_fifos( jack_engine_t & engine );
static int jack_engine_port_do_connect( jack_engine_t & engine,
					const char *source_port,
					const char *destination_port );
static int jack_engine_port_do_disconnect( jack_engine_t & engine,
					    const char *source_port,
					    const char *destination_port);
static int jack_engine_port_do_disconnect_all( jack_engine_t & engine,
						jack_port_id_t);
static int jack_engine_port_do_register( jack_engine_t & engine, jack_request_t *, int );
static int jack_engine_do_get_port_connections( jack_engine_t & engine,
						jack_request_t *req, int reply_fd );
static int jack_engine_send_connection_notification( jack_engine_t &,
						     jack_uuid_t,
						     jack_port_id_t,
						     jack_port_id_t, int);
static void jack_engine_deliver_event_to_all( jack_engine_t & engine,
					      jack_event_t *event);
static void jack_engine_notify_all_port_interested_clients( jack_engine_t & engine,
							    jack_uuid_t exclude_src_id,
							    jack_uuid_t exclude_dst_id,
							    jack_port_id_t a,
							    jack_port_id_t b,
							    int connect);
static void jack_engine_post_process( jack_engine_t & );
static int jack_engine_run_cycle( jack_engine_t * engine, jack_nframes_t nframes, float delayed_usecs);
static int jack_engine_run_one_cycle( jack_engine_t & engine, jack_nframes_t nframes, float delayed_usecs);
static void jack_engine_delay( jack_engine_t * engine,
			       float delayed_usecs);
static void jack_engine_driver_exit( jack_engine_t * engine );
static int jack_engine_start_freewheeling( jack_engine_t & engine, jack_uuid_t );
static int jack_client_feeds_transitive (jack_client_internal_t *source,
					  jack_client_internal_t *dest);
static void jack_engine_check_acyclic( jack_engine_t & engine );
static void jack_engine_compute_all_port_total_latencies( jack_engine_t & engine );
static void jack_engine_compute_port_total_latency( jack_engine_t & engine, jack_port_shared_t* );
static int jack_engine_check_client_status( jack_engine_t & engine );
static int jack_engine_do_session_notify( jack_engine_t & engine, jack_request_t *req, int reply_fd );
static void jack_engine_client_fill_request_port_name_by_uuid( jack_engine_t & engine, jack_request_t *req);
static void jack_engine_do_get_uuid_by_client_name( jack_engine_t & engine, jack_request_t *req );
static void jack_engine_do_reserve_name( jack_engine_t & engine, jack_request_t *req );
static void jack_engine_do_session_reply( jack_engine_t & engine, jack_request_t *req );
static void jack_engine_compute_new_latency( jack_engine_t & engine);
static int jack_engine_do_has_session_cb( jack_engine_t & engine, jack_request_t *req );

static inline int jack_rolling_interval( jack_time_t period_usecs )
{
    return floor ((JACK_ENGINE_ROLLING_INTERVAL * 1000.0f) / period_usecs);
}

void jack_engine_reset_rolling_usecs( jack_engine_t & engine )
{
    memset (engine.rolling_client_usecs, 0,
	    sizeof (engine.rolling_client_usecs));
    engine.rolling_client_usecs_index = 0;
    engine.rolling_client_usecs_cnt = 0;

    if (engine.driver) {
	engine.rolling_interval =
	    jack_rolling_interval (engine.driver->period_usecs);
    } else {
	engine.rolling_interval = JACK_ENGINE_ROLLING_INTERVAL;
    }

    engine.spare_usecs = 0;
}

static void jack_engine_buffer_list_remove_buffer(
    std::vector<jack_port_buffer_info_t*> & buffer_list_free, jack_port_buffer_info_t * bi )
{
    auto bi_finder = std::find( buffer_list_free.begin(),
				buffer_list_free.end(),
				bi );

    if( bi_finder != buffer_list_free.end() ) {
	buffer_list_free.erase( bi_finder );
    }
    else {
	jack_error("Failed to find buffer info to remove from buffer free list");
    }
}


void jack_engine_place_port_buffers( jack_engine_t & engine, 
				     jack_port_type_id_t ptid,
				     jack_shmsize_t one_buffer,
				     jack_shmsize_t size,
				     unsigned long nports,
				     jack_nframes_t nframes)
{
    jack_shmsize_t offset;		/* shared memory offset */
    jack_port_buffer_info_t *bi;
    jack_port_buffer_list_t* pti = &engine.port_buffers[ptid];
    jack_port_functions_t *pfuncs = jack_get_port_functions(ptid);

    pthread_mutex_lock (&pti->lock);
    offset = 0;
	
    if (pti->info) {

	/* Buffer info array already allocated for this port
	 * type.  This must be a resize operation, so
	 * recompute the buffer offsets, but leave the free
	 * list alone.
	 */
	size_t i;

	bi = pti->info;
	while (offset < size) {
	    bi->offset = offset;
	    offset += one_buffer;
	    ++bi;
	}

	/* update any existing output port offsets */
	for (i = 0; i < engine.port_max; i++) {
	    jack_port_shared_t *port = &engine.control->ports[i];
	    if (port->in_use &&
		(port->flags & JackPortIsOutput) &&
		port->ptype_id == ptid) {
		bi = engine.internal_ports[i].buffer_info;
		if (bi) {
		    port->offset = bi->offset;
		}
	    }
	}

    } else {
	jack_port_type_info_t* port_type = &engine.control->port_types[ptid];

	/* Allocate an array of buffer info structures for all
	 * the buffers in the segment.  Chain them to the free
	 * list in memory address order, offset zero must come
	 * first.
	 */
	bi = pti->info = (jack_port_buffer_info_t *)malloc (nports * sizeof (jack_port_buffer_info_t));

	while (offset < size) {
	    bi->offset = offset;
	    pti->freelist_vector.push_back( bi );
	    offset += one_buffer;
	    ++bi;
	}

	/* Allocate the first buffer of the port segment
	 * for an empy buffer area.
	 * NOTE: audio buffer is zeroed in its buffer_init function.
	 */
	bi = pti->freelist_vector[0];
	jack_engine_buffer_list_remove_buffer( pti->freelist_vector, bi );
	port_type->zero_buffer_offset = bi->offset;
	if (ptid == JACK_AUDIO_PORT_TYPE) {
	    engine.silent_buffer = bi;
	}
    }
    /* initialize buffers */
    {
	size_t i;
	jack_shm_info_t *shm_info = &engine.port_segment[ptid];
	char* shm_segment = (char *) jack_shm_addr(shm_info);

	bi = pti->info;
	for (i=0; i<nports; ++i, ++bi)
	    pfuncs->buffer_init(shm_segment + bi->offset, one_buffer, nframes);
    }

    pthread_mutex_unlock (&pti->lock);
}

#ifdef __linux

/* Linux kernels somewhere between 2.6.18 and 2.6.24 had a bug
   in poll(2) that led poll to return early. To fix it, we need
   to know that that jack_get_microseconds() is monotonic.
*/

#ifdef HAVE_CLOCK_GETTIME
static const int system_clock_monotonic = 1;
#else
static const int system_clock_monotonic = 0;
#endif

static int
linux_poll_bug_encountered (jack_engine_t* engine, jack_time_t then, jack_time_t *required)
{
    if (engine->control->clock_source != JACK_TIMER_SYSTEM_CLOCK || system_clock_monotonic) {
	jack_time_t now = jack_get_microseconds ();

	if ((now - then) < *required) {
	    /*
	      So, adjust poll timeout to account for time already spent waiting.
	    */

	    VERBOSE (engine, "FALSE WAKEUP (%lldusecs vs. %lld usec)", (now - then), *required);
	    *required -= (now - then);

	    /* allow 0.25msec slop */
	    return 1;
	}
    }
    return 0;
}
#endif

int jack_engine_deliver_event( jack_engine_t & engine, jack_client_internal_t *client,
    const jack_event_t *event, ...)
{
    va_list ap;
    char status=0;
    char* key = 0;
    size_t keylen = 0;

    va_start (ap, event);

    /* caller must hold the graph lock */

    DEBUG ("delivering event (type %s)", jack_event_type_name (event->type));

    /* we are not RT-constrained here, so use kill(2) to beef up
       our check on a client's continued well-being
    */

    if (client->control->dead || client->error >= JACK_ERROR_WITH_SOCKETS 
	|| (client->control->type == ClientExternal && kill (client->control->pid, 0))) {
	DEBUG ("client %s is dead - no event sent",
	       client->control->name);
	return 0;
    }

    DEBUG ("client %s is still alive", client->control->name);

    /* Check property change events for matching key_size and keys */

    if (event->type == PropertyChange) {
	key = va_arg (ap, char*);
	if (key && key[0] != '\0') {
	    keylen = strlen (key) + 1;
	    if (event->y.key_size != keylen) {
		jack_error ("property change key %s sent with wrong length (%d vs %d)", key, event->y.key_size, keylen);
		return -1;
	    }
	}
    }

    va_end (ap);

    if (jack_client_is_internal (client)) {
	switch (event->type) {
	    case PortConnected:
	    case PortDisconnected:
		jack_client_handle_port_connection( client->private_client,
						    (jack_event_t*)event);
		break;
	    case BufferSizeChange:
		jack_client_fix_port_buffers (client->private_client);

		if (client->control->bufsize_cbset) {
		    if (event->x.n < 16) {
			abort ();
		    }
		    client->private_client->bufsize
			(event->x.n, client->private_client->bufsize_arg);
		}
		break;

	    case SampleRateChange:
		if (client->control->srate_cbset) {
		    client->private_client->srate
			(event->x.n,
			 client->private_client->srate_arg);
		}
		break;

	    case GraphReordered:
		if (client->control->graph_order_cbset) {
		    client->private_client->graph_order
			(client->private_client->graph_order_arg);
		}
		break;

	    case XRun:
		if (client->control->xrun_cbset) {
		    client->private_client->xrun
			(client->private_client->xrun_arg);
		}
		break;

	    case PropertyChange:
		if (client->control->property_cbset) {
		    client->private_client->property_cb
			(event->x.uuid, key, event->z.property_change, client->private_client->property_cb_arg);
		}
		break;

	    case LatencyCallback:
		jack_client_handle_latency_callback( client->private_client,
						     (jack_event_t*)event,
						     (client->control->type == ClientDriver));
		break;

	    default:
		/* internal clients don't need to know */
		break;
	}
    } else {
	if (client->control->active) {

	    /* there's a thread waiting for events, so
	     * it's worth telling the client */

	    DEBUG ("engine writing on event fd");

	    if (write (client->event_fd, event, sizeof (*event))
		!= sizeof (*event)) {
		jack_error ("cannot send event to client [%s]"
			    " (%s)", client->control->name,
			    strerror (errno));
		client->error += JACK_ERROR_WITH_SOCKETS;
		jack_engine_signal_problems( engine );
	    }

	    /* for property changes, deliver the extra data representing
	       the variable length "key" that has changed in some way.
	    */
                        
	    if (event->type == PropertyChange) {
		if (write (client->event_fd, key, keylen) != (ssize_t)keylen) {
		    jack_error ("cannot send property change key to client [%s] (%s)",
				client->control->name,
				strerror (errno));
		    client->error += JACK_ERROR_WITH_SOCKETS;
		    jack_engine_signal_problems( engine );
		}
	    }

	    if (client->error) {
		status = -1;
	    } else {
		// then we check whether there really is an error.... :)
 
		struct pollfd pfd[1];
		pfd[0].fd = client->event_fd;
		pfd[0].events = POLLERR|POLLIN|POLLHUP|POLLNVAL;
		jack_time_t poll_timeout = JACKD_CLIENT_EVENT_TIMEOUT;
		int poll_ret;
		jack_time_t then = jack_get_microseconds ();
		jack_time_t now;
				
		/* if we're not running realtime and there is a client timeout set
		   that exceeds the default client event timeout (which is not
		   bound by RT limits, then use the larger timeout.
		*/

		if (!engine.control->real_time && ((size_t)engine.client_timeout_msecs > poll_timeout)) {
		    poll_timeout = engine.client_timeout_msecs;
		}

#ifdef __linux
	      again:
#endif
		VERBOSE( &engine,"client event poll on %d for %s starts at %lld", 
			client->event_fd, client->control->name, then);
		if ((poll_ret = poll (pfd, 1, poll_timeout)) < 0) {
		    DEBUG ("client event poll not ok! (-1) poll returned an error");
		    jack_error ("poll on subgraph processing failed (%s)", strerror (errno));
		    status = -1;
		} else {
 
		    DEBUG ("\n\n\n\n\n back from client event poll, revents = 0x%x\n\n\n", pfd[0].revents);
		    now = jack_get_microseconds();
		    VERBOSE( &engine,"back from client event poll after %lld usecs", now - then);

		    if (pfd[0].revents & ~POLLIN) {

			/* some kind of OOB socket event */

			DEBUG ("client event poll not ok! (-2), revents = %d\n", pfd[0].revents);
			jack_error ("subgraph starting at %s lost client", client->control->name);
			status = -2; 

		    } else if (pfd[0].revents & POLLIN) {

			/* client responded normally */

			DEBUG ("client event poll ok!");
			status = 0;

		    } else if (poll_ret == 0) {

			/* no events, no errors, we woke up because poll()
			   decided that time was up ...
			*/
						
#ifdef __linux		
			if (linux_poll_bug_encountered( &engine, then, &poll_timeout)) {
			    goto again;
			}
						
			if (poll_timeout < 200) {
			    VERBOSE( &engine, "FALSE WAKEUP skipped, remaining = %lld usec", poll_timeout);
			    status = 0;
			} else {
#endif
			    DEBUG ("client event poll not ok! (1 = poll timed out, revents = 0x%04x, poll_ret = %d)", pfd[0].revents, poll_ret);
			    VERBOSE( &engine,"client %s did not respond to event type %d in time"
				     "(fd=%d, revents = 0x%04x, timeout was %lld)", 
				     client->control->name, event->type,
				     client->event_fd,
				     pfd[0].revents,
				     poll_timeout);
			    status = -2;
#ifdef __linux
			}
#endif
		    }
		}
	    }
                        
	    if (status == 0) {
		if (read (client->event_fd, &status, sizeof (status)) != sizeof (status)) {
		    jack_error ("cannot read event response from "
				"client [%s] (%s)",
				client->control->name,
				strerror (errno));
		    status = -1;
		} 

	    } else {
		switch (status) {
		    case -1:
			jack_error ("internal poll failure reading response from client %s to a %s event",
				    client->control->name,
				    jack_event_type_name (event->type));
			break;
		    case -2:
			jack_error ("timeout waiting for client %s to handle a %s event",
				    client->control->name,
				    jack_event_type_name (event->type));
			break;
		    default:
			jack_error ("bad status (%d) from client %s while handling a %s event",
				    (int) status, 
				    client->control->name,
				    jack_event_type_name (event->type));
		}
	    }

	    if (status<0) {
		client->error += JACK_ERROR_WITH_SOCKETS;
		jack_engine_signal_problems( engine );
	    }
	}
    }
    DEBUG ("event delivered");

    return status;
}

static void jack_engine_deliver_event_to_all( jack_engine_t & engine, jack_event_t *event )
{
    jack_rdlock_graph( (&engine) );
    for( jack_client_internal_t * client : engine.clients ) {
	jack_engine_deliver_event( engine,
				   client,
				   event );
    }
    jack_unlock_graph( (&engine) );
}

static int jack_engine_resize_port_segment( jack_engine_t & engine,
					    jack_port_type_id_t ptid,
					    unsigned long nports)
{
    jack_event_t event;
    jack_shmsize_t one_buffer;	/* size of one buffer */
    jack_shmsize_t size;		/* segment size */
    jack_port_type_info_t* port_type = &engine.control->port_types[ptid];
    jack_shm_info_t* shm_info = &engine.port_segment[ptid];

    one_buffer = jack_port_type_buffer_size( port_type, engine.control->buffer_size );
    VERBOSE( &engine, "resizing port buffer segment for type %d, one buffer = %u bytes", ptid, one_buffer);

    size = nports * one_buffer;

    if (shm_info->attached_at == 0) {

	if (jack_shmalloc (size, shm_info)) {
	    jack_error ("cannot create new port segment of %d"
			" bytes (%s)", 
			size,
			strerror (errno));
	    return -1;
	}
		
	if (jack_attach_shm (shm_info)) {
	    jack_error ("cannot attach to new port segment "
			"(%s)", strerror (errno));
	    return -1;
	}

	engine.control->port_types[ptid].shm_registry_index =
	    shm_info->index;

    } else {

	/* resize existing buffer segment */
	if (jack_resize_shm (shm_info, size)) {
	    jack_error ("cannot resize port segment to %d bytes,"
			" (%s)", size,
			strerror (errno));
	    return -1;
	}
    }

    jack_engine_place_port_buffers( engine, ptid, one_buffer, size, nports, engine.control->buffer_size );

#ifdef USE_MLOCK
    if (engine.control->real_time) {

	/* Although we've called mlockall(CURRENT|FUTURE), the
	 * Linux VM manager still allows newly allocated pages
	 * to fault on first reference.  This mlock() ensures
	 * that any new pages are present before restarting
	 * the process cycle.  Since memory locks do not
	 * stack, they can still be unlocked with a single
	 * munlockall().
	 */

	int rc = mlock (jack_shm_addr (shm_info), size);
	if (rc < 0) {
	    jack_error("JACK: unable to mlock() port buffers: "
		       "%s", strerror(errno));
	}
    }
#endif /* USE_MLOCK */

    /* Tell everybody about this segment. */
    event.type = AttachPortSegment;
    event.y.ptid = ptid;
    jack_engine_deliver_event_to_all( engine, &event );

    /* XXX need to clean up in the evnt of failures */

    return 0;
}

/* The driver invokes this callback both initially and whenever its
 * buffer size changes. 
 */
static int jack_engine_driver_buffer_size( jack_engine_t * engine, jack_nframes_t nframes )
{
    int i;
    jack_event_t event;

    VERBOSE( engine, "new buffer size %" PRIu32, nframes);

    engine->control->buffer_size = nframes;
    if( engine->driver )
	engine->rolling_interval = jack_rolling_interval( engine->driver->period_usecs );

    for (i = 0; i < engine->control->n_port_types; ++i) {
	if (jack_engine_resize_port_segment( *engine, i, engine->control->port_max)) {
	    return -1;
	}
    }

    event.type = BufferSizeChange;
    event.x.n = engine->control->buffer_size;
    jack_engine_deliver_event_to_all( *engine, &event );

    return 0;
}

static void jack_engine_delay( jack_engine_t * engine, float delayed_usecs )
{
    jack_event_t event;
	
    engine->control->frame_timer.reset_pending = 1;

    engine->control->xrun_delayed_usecs = delayed_usecs;

    if (delayed_usecs > engine->control->max_delayed_usecs)
	engine->control->max_delayed_usecs = delayed_usecs;

    event.type = XRun;

    jack_engine_deliver_event_to_all( *engine, &event );
}

static int jack_engine_drivers_read( jack_engine_t & engine, jack_nframes_t nframes )
{
    /* first read the slave drivers */
    for( jack_driver_t * sdriver : engine.slave_drivers ) {
	sdriver->read( sdriver, nframes );
    }

    /* now the master driver is read */
    return engine.driver->read( engine.driver, nframes );
}

static vector<jack_client_internal_t*>::iterator
jack_engine_process_internal( jack_engine_t & engine,
			      vector<jack_client_internal_t*>::iterator current_iterator,
			      vector<jack_client_internal_t*>::iterator end_marker,
			      jack_nframes_t nframes )
{
    jack_client_internal_t *client = *current_iterator;
    jack_client_control_t *ctl = client->control;

    /* internal client */

    DEBUG ("invoking an internal client's (%s) callbacks", ctl->name);
    ctl->state = Running;
    engine.current_client = client;

    /* XXX how to time out an internal client? */

    if (ctl->sync_cb_cbset) {
	jack_call_sync_client (client->private_client);
    }

    if (ctl->process_cbset)
    {
	if (client->private_client->process (nframes, client->private_client->process_arg)) {
	    jack_error ("internal client %s failed", ctl->name);
	    engine.process_errors++;
	}
    }

    if (ctl->timebase_cb_cbset) {
	jack_call_timebase_master (client->private_client);
    }

    ctl->state = Finished;

    if (engine.process_errors) {
	return end_marker;		/* will stop the loop */
    }
    else {
	return ++current_iterator;
    }
}

static int jack_engine_check_client_status( jack_engine_t & engine )
{
    int err = 0;

    /* we are already late, or something else went wrong,
       so it can't hurt to check the existence of all
       clients.
    */
	
    for( jack_client_internal_t * client : engine.clients ) {
	if (client->control->type == ClientExternal) {
	    if (kill (client->control->pid, 0)) {
		VERBOSE( &engine,
			 "client %s has died/exited",
			 client->control->name);
		client->error++;
		err++;
	    }
	    if(client->control->last_status != 0) {
		VERBOSE( &engine,
			 "client %s has nonzero process callback status (%d)\n",
			 client->control->name, client->control->last_status);
		client->error++;
		err++;
	    }
	}
		
	DEBUG ("client %s errors = %d", client->control->name,
	       client->error);
    }

    return err;
}

static vector<jack_client_internal_t*>::iterator
jack_engine_process_external( jack_engine_t & engine,
			      vector<jack_client_internal_t*>::iterator current_iterator,
			      vector<jack_client_internal_t*>::iterator end_marker )
{
    int status = 0;
    char c = 0;
    struct pollfd pfd[1];
    int poll_timeout;
    jack_time_t poll_timeout_usecs;
    jack_client_internal_t *client = *current_iterator;
    jack_client_control_t *ctl = client->control;
    jack_time_t now, then;
    int pollret;

    /* external subgraph */

    /* a race exists if we do this after the write(2) */
    ctl->state = Triggered;

    ctl->signalled_at = jack_get_microseconds();

    engine.current_client = client;

    DEBUG ("calling process() on an external subgraph, fd==%d",
	   client->subgraph_start_fd);

    if (write (client->subgraph_start_fd, &c, sizeof (c)) != sizeof (c)) {
	jack_error ("cannot initiate graph processing (%s)",
		    strerror (errno));
	engine.process_errors++;
	jack_engine_signal_problems( engine );
	return end_marker; /* will stop the loop */
    }

    then = jack_get_microseconds ();

    if (engine.freewheeling) {
	poll_timeout_usecs = 250000; /* 0.25 seconds */
    } else {
	poll_timeout_usecs = (engine.client_timeout_msecs > 0 ?
			      engine.client_timeout_msecs * 1000 :
			      engine.driver->period_usecs);
    }

  again:
    poll_timeout = 1 + poll_timeout_usecs / 1000;
    pfd[0].fd = client->subgraph_wait_fd;
    pfd[0].events = POLLERR|POLLIN|POLLHUP|POLLNVAL;

    DEBUG ("waiting on fd==%d for process() subgraph to finish (timeout = %d, period_usecs = %d)",
	   client->subgraph_wait_fd, poll_timeout, engine.driver->period_usecs);

    if ((pollret = poll (pfd, 1, poll_timeout)) < 0) {
	jack_error ("poll on subgraph processing failed (%s)",
		    strerror (errno));
	status = -1;
    }

    DEBUG ("\n\n\n\n\n back from subgraph poll, revents = 0x%x\n\n\n", pfd[0].revents);

    if (pfd[0].revents & ~POLLIN) {
	jack_error ("subgraph starting at %s lost client",
		    client->control->name);
	status = -2;
    }

    if (pfd[0].revents & POLLIN) {

	status = 0;

    } else if (status == 0) {

	/* no events, no errors, we woke up because poll()
	   decided that time was up ...
	*/

	if (engine.freewheeling) {
	    if (jack_engine_check_client_status( engine ) ) {
		return end_marker;
	    } else {
		/* all clients are fine - we're just not done yet. since
		   we're freewheeling, that is fine.
		*/
		goto again;
	    }
	}

#ifdef __linux
	if (linux_poll_bug_encountered( &engine, then, &poll_timeout_usecs)) {
	    goto again;
	}

	if (poll_timeout_usecs < 200) {
	    VERBOSE( &engine, "FALSE WAKEUP skipped, remaining = %lld usec", poll_timeout_usecs);
	} else {
#endif
	    jack_error ("subgraph starting at %s timed out "
			"(subgraph_wait_fd=%d, status = %d, state = %s, pollret = %d revents = 0x%x)",
			client->control->name,
			client->subgraph_wait_fd, status,
			jack_client_state_name (client),
			pollret, pfd[0].revents);
	    status = 1;
#ifdef __linux
	}
#endif
    }

    now = jack_get_microseconds ();

    if (status != 0) {
	VERBOSE( &engine, "at %" PRIu64
		 " waiting on %d for %" PRIu64
		 " usecs, status = %d sig = %" PRIu64
		 " awa = %" PRIu64 " fin = %" PRIu64
		 " dur=%" PRIu64,
		 now,
		 client->subgraph_wait_fd,
		 now - then,
		 status,
		 ctl->signalled_at,
		 ctl->awake_at,
		 ctl->finished_at,
		 ctl->finished_at? (ctl->finished_at -
				    ctl->signalled_at): 0);

	if( jack_engine_check_clients( engine, 1) ) {

	    engine.process_errors++;
	    return end_marker;		/* will stop the loop */
	}
    } else {
	engine.timeout_count = 0;
    }


    DEBUG ("reading byte from subgraph_wait_fd==%d",
	   client->subgraph_wait_fd);

    if (read (client->subgraph_wait_fd, &c, sizeof(c))
	!= sizeof (c)) {
	if (errno == EAGAIN) {
	    jack_error ("pp: cannot clean up byte from graph wait "
			"fd (%s) - no data present");
	} else {
	    jack_error ("pp: cannot clean up byte from graph wait "
			"fd (%s)", strerror (errno));
	    client->error++;
	}
	return end_marker;	/* will stop the loop */
    }

    /* Move to next internal client (or end of client list) */
    while( current_iterator != end_marker ) {
	if( jack_client_is_internal( *current_iterator ) ) {
	    break;
	}
	++current_iterator;
    }

    return current_iterator;
}

static int jack_engine_process( jack_engine_t & engine, jack_nframes_t nframes )
{
    /* precondition: caller has graph_lock */

    engine.process_errors = 0;

    for( jack_client_internal_t * client : engine.clients ) {
	jack_client_control_t * ctl = client->control;
	ctl->state = NotTriggered;
	ctl->timed_out = 0;
	ctl->awake_at = 0;
	ctl->finished_at = 0;
    }

    vector<jack_client_internal_t*>::iterator current_iterator = engine.clients.begin();
    vector<jack_client_internal_t*>::iterator end_marker = engine.clients.end();
    for( ; engine.process_errors == 0 && current_iterator != end_marker ; ) {
	jack_client_internal_t * client = *current_iterator;
	jack_client_control_t * ctl = client->control;

	DEBUG( "considering client %s for processing", ctl->name );

	if( !ctl->active ||
	    (!ctl->process_cbset && !ctl->thread_cb_cbset) ||
	    ctl->dead ) {
	    ++current_iterator;
	} else if( jack_client_is_internal( client ) ) {
	    current_iterator = jack_engine_process_internal( engine,
								current_iterator,
								end_marker,
								nframes );
	} else {
	    current_iterator = jack_engine_process_external( engine,
							     current_iterator,
							     end_marker );
	}
    }

    return engine.process_errors > 0;
}

static int jack_engine_drivers_write( jack_engine_t & engine, jack_nframes_t nframes )
{
    /* first start the slave drivers */
    for( jack_driver_t * sdriver : engine.slave_drivers ) {
	sdriver->write( sdriver, nframes );
    }

    /* now the master driver is written */
    return engine.driver->write( engine.driver, nframes );
}

static void jack_engine_calc_cpu_load( jack_engine_t & engine )
{
    jack_time_t cycle_end = jack_get_microseconds ();
	
    /* store the execution time for later averaging */

    engine.rolling_client_usecs[engine.rolling_client_usecs_index++] = 
	cycle_end - engine.control->current_time.usecs;

    //jack_info ("cycle_end - engine.control->current_time.usecs %ld",
    //	(long) (cycle_end - engine.control->current_time.usecs));

    if (engine.rolling_client_usecs_index >= JACK_ENGINE_ROLLING_COUNT) {
	engine.rolling_client_usecs_index = 0;
    }

    /* every so often, recompute the current maximum use over the
       last JACK_ENGINE_ROLLING_COUNT client iterations.
    */

    if (++engine.rolling_client_usecs_cnt
	% engine.rolling_interval == 0) {
	float max_usecs = 0.0f;
	int i;

	for (i = 0; i < JACK_ENGINE_ROLLING_COUNT; i++) {
	    if (engine.rolling_client_usecs[i] > max_usecs) {
		max_usecs = engine.rolling_client_usecs[i];
	    }
	}

	if (max_usecs > engine.max_usecs) {
	    engine.max_usecs = max_usecs;
	}

	if (max_usecs < engine.driver->period_usecs) {
	    engine.spare_usecs =
		engine.driver->period_usecs - max_usecs;
	} else {
	    engine.spare_usecs = 0;
	}

	engine.control->cpu_load =
	    (1.0f - (engine.spare_usecs /
		     engine.driver->period_usecs)) * 50.0f
	    + (engine.control->cpu_load * 0.5f);

	VERBOSE( &engine, "load = %.4f max usecs: %.3f, "
		 "spare = %.3f", engine.control->cpu_load,
		 max_usecs, engine.spare_usecs);
    }

}

static void jack_engine_post_process( jack_engine_t & engine )
{
    /* precondition: caller holds the graph lock. */

    jack_transport_cycle_end( engine );
    jack_engine_calc_cpu_load( engine );
    jack_engine_check_clients( engine, 0 );
}

static int jack_engine_run_one_cycle( jack_engine_t & engine, jack_nframes_t nframes, float delayed_usecs)
{
    jack_driver_t* driver = engine.driver;
    int ret = -1;
    static int consecutive_excessive_delays = 0;

#define WORK_SCALE 1.0f

    if (!engine.freewheeling && 
	engine.control->real_time &&
	engine.spare_usecs &&
	((WORK_SCALE * engine.spare_usecs) <= delayed_usecs)) {

	MESSAGE("delay of %.3f usecs exceeds estimated spare"
		" time of %.3f; restart ...\n",
		delayed_usecs, WORK_SCALE * engine.spare_usecs);
		
	if (++consecutive_excessive_delays > 10) {
	    jack_error ("too many consecutive interrupt delays "
			"... engine pausing");
	    return -1;	/* will exit the thread loop */
	}

	jack_engine_delay( &engine, delayed_usecs );
		
	return 0;

    } else {
	consecutive_excessive_delays = 0;
    }

    DEBUG ("trying to acquire read lock (FW = %d)", engine.freewheeling);
    if (jack_try_rdlock_graph( (&engine) )) {
	VERBOSE( &engine, "lock-driven null cycle");
	if (!engine.freewheeling) {
	    driver->null_cycle(driver, nframes);
	} else {
	    /* don't return too fast */
	    usleep (1000);
	}
	return 0;
    }

    if (jack_trylock_problems( (&engine) )) {
	VERBOSE( &engine, "problem-lock-driven null cycle");
	jack_unlock_graph( (&engine) );
	if (!engine.freewheeling) {
	    driver->null_cycle (driver, nframes);
	} else {
	    /* don't return too fast */
	    usleep (1000);
	}
	return 0;
    }

    if (engine.problems || (engine.timeout_count_threshold && ((size_t)engine.timeout_count > (1 + engine.timeout_count_threshold*1000/engine.driver->period_usecs) ))) {
	VERBOSE( &engine, "problem-driven null cycle problems=%d", engine.problems);
	jack_unlock_problems( (&engine) );
	jack_unlock_graph( (&engine) );
	if (!engine.freewheeling) {
	    driver->null_cycle (driver, nframes);
	} else {
	    /* don't return too fast */
	    usleep (1000);
	}
	return 0;
    }

    jack_unlock_problems( (&engine) );
		
    if (!engine.freewheeling) {
	DEBUG("waiting for driver read\n");
	if (jack_engine_drivers_read( engine, nframes )) {
	    goto unlock;
	}
    }
	
    DEBUG("run process\n");

    if (jack_engine_process( engine, nframes ) != 0) {
	DEBUG ("engine process cycle failed");
	jack_engine_check_client_status( engine );
    }
		
    if (!engine.freewheeling) {
	if (jack_engine_drivers_write( engine, nframes )) {
	    goto unlock;
	}
    }

    jack_engine_post_process( engine );

    if (delayed_usecs > engine.control->max_delayed_usecs)
	engine.control->max_delayed_usecs = delayed_usecs;
	
    ret = 0;

  unlock:
    jack_unlock_graph( (&engine) );
    DEBUG("cycle finished, status = %d", ret);

    return ret;
}

static int jack_engine_run_cycle( jack_engine_t * engine, jack_nframes_t nframes, float delayed_usecs)
{
    jack_time_t now = engine->driver->last_wait_ust;
    jack_time_t dus = 0;
    jack_time_t p_usecs = engine->driver->period_usecs ;
    jack_nframes_t b_size = engine->control->buffer_size;
    jack_nframes_t left;
    jack_frame_timer_t* timer = &engine->control->frame_timer;

    if( engine->verbose ) {
	if (nframes != b_size) { 
	    VERBOSE( engine, 
		     "late driver wakeup: nframes to process = %"
		     PRIu32 ".", nframes);
	}
    }

    /* Run as many cycles as it takes to consume nframes */

    for (left = nframes; left >= b_size; left -= b_size)
    {
	/* Change: the DLL code is now inside this loop which ensures
	   that more than one period is run if more than a buffer size
	   of frames is available. This is a very unlikely event, but
	   it is possible and now handled correctly. 
	   FA 25/06/2014 
	*/
	/* Change: 'first_wakeup' now means only the very first wakeup
	   after the engine was created. In that case frame time is not
	   modified, it stays at the initialised value.
	   OTOH 'reset_pending' is  used after freewheeling or an xrun.
	   In those cases frame time is adjusted by using the difference
	   in usecs time between the end of the previous period and the
	   start of the current one. This way, a client monitoring the
	   frame time for the start of each period will have a correct
	   idea of the number of frames that were skipped. 
	   FA 25/06/2014 
	*/
	/* Change: in contrast to previous versions, the DLL is *not*
	   run if any of the two conditions above is true, it is just 
	   initialised correctly for the current period. Previously it
	   was initialised and then run, which meant that the requred
	   initialisation was rather counter-intiutive.
	   FA 25/06/2014 
	*/
	/* Added initialisation of timer->period_usecs, required
	   due to the modified implementation of the DLL itself. 
	   OTOH, this should maybe not be repeated after e.g.
	   freewheeling or an xrun, as the current value would be
	   more accurate than the nominal one. But it doesn't really
	   harm either.
	   FA 13/02/2012
	*/
	/* Added initialisation of timer->filter_omega. This makes 
	   the DLL bandwidth independent of the actual period time.
	   The bandwidth is now 1/8 Hz in all cases. The value of
	   timer->filter_omega is 2 * pi * BW * Tperiod.
	   FA 13/02/2012
	*/

	// maybe need a memory barrier here
	timer->guard1++;

	if (timer->reset_pending)
	{
	    // Adjust frame time after a discontinuity.
	    // The frames of the previous period.
	    timer->frames += b_size;
	    // The frames that were skipped.
	    dus = now - timer->next_wakeup; 
	    timer->frames += (dus * b_size) / p_usecs;
	}
	if (engine->first_wakeup || timer->reset_pending)
	{
	    // First wakeup or after discontinuity.
	    // Initialiase the DLL.
	    timer->current_wakeup = now;
	    timer->next_wakeup = now + p_usecs;
	    timer->period_usecs = (float) p_usecs;
	    timer->filter_omega = timer->period_usecs * 7.854e-7f;
	    timer->initialized = 1;

	    // Reset both conditions.
	    engine->first_wakeup = 0;
	    timer->reset_pending = 0;
	}
	else 
	{
	    // Normal cycle. This code was originally in
	    // jack_inc_frame_time() but only used here.
	    // Moving it here means that now all code 
	    // related to timekeeping is close together
	    // and easy to understand.
	    float delta = (float)((int64_t) now - (int64_t) timer->next_wakeup);
	    delta *= timer->filter_omega;
	    timer->current_wakeup = timer->next_wakeup;
	    timer->frames += b_size;
	    timer->period_usecs += timer->filter_omega * delta;	
	    timer->next_wakeup += (int64_t) floorf (timer->period_usecs + 1.41f * delta + 0.5f);
	}

	// maybe need a memory barrier here
	timer->guard2++;

	if( jack_engine_run_one_cycle( *engine, b_size, delayed_usecs ) ) {
	    jack_error ("cycle execution failure, exiting");
	    return EIO;
	}
    }

    return 0;
}

static void jack_engine_driver_exit( jack_engine_t * engine)
{
    jack_driver_t* driver = engine->driver;

    VERBOSE( engine, "stopping driver");
    driver->stop( driver );
    VERBOSE( engine, "detaching driver");
    driver->detach( driver, engine );

    /* tell anyone waiting that the driver exited. */
    kill( engine->wait_pid, SIGUSR2 );
	
    engine->driver = NULL;
}

static int make_directory (const char *path)
{
    struct stat statbuf;

    if (stat (path, &statbuf)) {

	if (errno == ENOENT) {
	    int mode;

	    if (getenv ("JACK_PROMISCUOUS_SERVER")) {
		mode = 0777;
	    } else {
		mode = 0700;
	    }

	    if (mkdir (path, mode) < 0){
		jack_error ("cannot create %s directory (%s)\n",
			    path, strerror (errno));
		return -1;
	    }
	} else {
	    jack_error ("cannot stat() %s\n", path);
	    return -1;
	}

    } else {

	if (!S_ISDIR (statbuf.st_mode)) {
	    jack_error ("%s already exists, but is not"
			" a directory!\n", path);
	    return -1;
	}
    }

    return 0;
}

static int make_socket_subdirectories (const char *server_name)
{
    struct stat statbuf;
    char server_dir[PATH_MAX+1] = "";

    /* check tmpdir directory */
    if (stat (jack_tmpdir, &statbuf)) {
	jack_error ("cannot stat() %s (%s)\n",
		    jack_tmpdir, strerror (errno));
	return -1;
    } else {
	if (!S_ISDIR(statbuf.st_mode)) {
	    jack_error ("%s exists, but is not a directory!\n",
			jack_tmpdir);
	    return -1;
	}
    }

    /* create user subdirectory */
    if (make_directory (jack_user_dir ()) < 0) {
	return -1;
    }

    /* create server_name subdirectory */
    if (make_directory (jack_server_dir (server_name, server_dir)) < 0) {
	return -1;
    }

    return 0;
}

static int make_sockets (const char *server_name, int fd[2])
{
    struct sockaddr_un addr;
    int i;
    char server_dir[PATH_MAX+1] = "";

    if (make_socket_subdirectories (server_name) < 0) {
	return -1;
    }

    /* First, the master server socket */

    if ((fd[0] = socket (AF_UNIX, SOCK_STREAM, 0)) < 0) {
	jack_error ("cannot create server socket (%s)",
		    strerror (errno));
	return -1;
    }

    addr.sun_family = AF_UNIX;
    for (i = 0; i < 999; i++) {
	stringstream ss( stringstream::out );
	ss << jack_server_dir( server_name, server_dir ) << "/jack_" << i;

	string path_to_check( ss.str() );

	if (access( path_to_check.c_str(), F_OK) != 0) {
	    strncpy(addr.sun_path, path_to_check.c_str(), sizeof( addr.sun_path ) - 1 );
	    break;
	}
    }

    if (i == 999) {
	jack_error ("all possible server socket names in use!!!");
	close (fd[0]);
	return -1;
    }

    if (bind (fd[0], (struct sockaddr *) &addr, sizeof (addr)) < 0) {
	jack_error ("cannot bind server to socket (%s)",
		    strerror (errno));
	close (fd[0]);
	return -1;
    }

    if (listen (fd[0], 1) < 0) {
	jack_error ("cannot enable listen on server socket (%s)",
		    strerror (errno));
	close (fd[0]);
	return -1;
    }

    /* Now the client/server event ack server socket */

    if ((fd[1] = socket (AF_UNIX, SOCK_STREAM, 0)) < 0) {
	jack_error ("cannot create event ACK socket (%s)",
		    strerror (errno));
	close (fd[0]);
	return -1;
    }

    addr.sun_family = AF_UNIX;
    for (i = 0; i < 999; i++) {
	stringstream ss( stringstream::out );
	ss << jack_server_dir( server_name, server_dir ) << "/jack_ack_" << i;

	string path_to_check( ss.str() );

	if (access( path_to_check.c_str(), F_OK) != 0) {
	    strncpy( addr.sun_path, path_to_check.c_str(), sizeof( addr.sun_path ) - 1 );
	    break;
	}
    }

    if (i == 999) {
	jack_error ("all possible server ACK socket names in use!!!");
	close (fd[0]);
	close (fd[1]);
	return -1;
    }

    if (bind (fd[1], (struct sockaddr *) &addr, sizeof (addr)) < 0) {
	jack_error ("cannot bind server to socket (%s)",
		    strerror (errno));
	close (fd[0]);
	close (fd[1]);
	return -1;
    }

    if (listen (fd[1], 1) < 0) {
	jack_error ("cannot enable listen on server socket (%s)",
		    strerror (errno));
	close (fd[0]);
	close (fd[1]);
	return -1;
    }

    return 0;
}

static jack_port_internal_t * jack_engine_get_port_by_name(
    jack_engine_t & engine, const char *name )
{
    jack_port_id_t id;

    /* Note the potential race on "in_use". Other design
       elements prevent this from being a problem.
    */

    for (id = 0; id < engine.port_max; id++) {
	if (engine.control->ports[id].in_use &&
	    jack_port_name_equals( &engine.control->ports[id], name)) {
	    return &engine.internal_ports[id];
	}
    }

    return NULL;
}

static jack_port_id_t jack_engine_get_free_port( jack_engine_t & engine )

{
    jack_port_id_t i;

    pthread_mutex_lock(&engine.port_lock);

    for (i = 0; i < engine.port_max; i++) {
	if (engine.control->ports[i].in_use == 0) {
	    engine.control->ports[i].in_use = 1;
	    break;
	}
    }
	
    pthread_mutex_unlock(&engine.port_lock);
	
    if (i == engine.port_max) {
	return (jack_port_id_t) -1;
    }

    return i;
}

static inline jack_port_type_info_t * jack_engine_port_type_info( jack_engine_t & engine, jack_port_internal_t *port )
{
    /* Returns a pointer to the port type information in the
       engine's shared control structure. 
    */
    return &engine.control->port_types[port->shared->ptype_id];
}

static inline jack_port_buffer_list_t * jack_engine_port_buffer_list( jack_engine_t & engine, jack_port_internal_t *port )
{
    /* Points to the engine's private port buffer list struct. */
    return &engine.port_buffers[port->shared->ptype_id];
}

void jack_engine_property_change_notify( jack_engine_t & engine,
					 jack_property_change_t change,
					 jack_uuid_t uuid,
					 const char* key )
{
    jack_event_t event;

    event.type = PropertyChange;
    event.z.property_change = change;
    jack_uuid_copy (&event.x.uuid, uuid);

    if (key) {
	event.y.key_size = strlen (key) + 1;
    } else {
	event.y.key_size = 0;
    }

    for( jack_client_internal_t * client : engine.clients ) {
	if (!client->control->active) {
	    continue;
	}

	if (client->control->property_cbset) {
	    if (jack_engine_deliver_event( engine, client, &event, key)) {
		jack_error ("cannot send property change notification to %s (%s)",
			    client->control->name,
			    strerror (errno));
	    }
	}
    }
}

int jack_engine_port_assign_buffer( jack_engine_t & engine, jack_port_internal_t *port )
{
    jack_port_buffer_list_t *blist = jack_engine_port_buffer_list( engine, port );
    jack_port_buffer_info_t *bi;

    if (port->shared->flags & JackPortIsInput) {
	port->shared->offset = 0;
	return 0;
    }
	
    pthread_mutex_lock (&blist->lock);

    if( blist->freelist_vector.size() == 0 ) {
	jack_port_type_info_t *port_type = jack_engine_port_type_info( engine, port);
	jack_error ("all %s port buffers in use!",
		    port_type->type_name);
	pthread_mutex_unlock (&blist->lock);
	return -1;
    }

    bi = blist->freelist_vector[0];
    jack_engine_buffer_list_remove_buffer( blist->freelist_vector, bi );

    port->shared->offset = bi->offset;
    port->buffer_info = bi;

    pthread_mutex_unlock (&blist->lock);
    return 0;
}

void jack_engine_port_registration_notify(
    jack_engine_t & engine,
    jack_port_id_t port_id, int yn)
{
    jack_event_t event;

    event.type = (yn ? PortRegistered : PortUnregistered);
    event.x.port_id = port_id;
	
    for( jack_client_internal_t * client : engine.clients ) {
	if (!client->control->active) {
	    continue;
	}

	if (client->control->port_register_cbset) {
	    if (jack_engine_deliver_event( engine, client, &event)) {
		jack_error ("cannot send port registration"
			    " notification to %s (%s)",
			    client->control->name,
			    strerror (errno));
	    }
	}
    }
}

void jack_engine_port_release( jack_engine_t & engine, jack_port_internal_t *port )
{
    char buf[JACK_UUID_STRING_SIZE];
    jack_uuid_unparse (port->shared->uuid, buf);
    if (jack_remove_properties (NULL, port->shared->uuid) > 0) {
	/* have to do the notification ourselves, since the client argument
	   to jack_remove_properties() was NULL
	*/
	jack_engine_property_change_notify( engine, PropertyDeleted, port->shared->uuid, NULL);
    }

    pthread_mutex_lock (&engine.port_lock);
    port->shared->in_use = 0;
    port->shared->alias1[0] = '\0';
    port->shared->alias2[0] = '\0';

    if (port->buffer_info) {
	jack_port_buffer_list_t *blist = jack_engine_port_buffer_list( engine, port);
	pthread_mutex_lock (&blist->lock);
	blist->freelist_vector.push_back( port->buffer_info );
	port->buffer_info = NULL;
	pthread_mutex_unlock (&blist->lock);
    }
    pthread_mutex_unlock( &engine.port_lock );
}

int jack_engine_port_do_register( jack_engine_t & engine, jack_request_t *req, int internal )
{
    jack_port_id_t port_id;
    jack_port_shared_t *shared;
    jack_port_internal_t *port;
    jack_client_internal_t *client;
    ssize_t i;
    char *backend_client_name;
    size_t len;

    for (i = 0; i < engine.control->n_port_types; ++i) {
	if (strcmp (req->x.port_info.type,
		    engine.control->port_types[i].type_name) == 0) {
	    break;
	}
    }

    if (i == engine.control->n_port_types) {
	jack_error ("cannot register a port of type \"%s\"",
		    req->x.port_info.type);
	return -1;
    }

    jack_lock_graph( (&engine) );
    if ((client = jack_engine_client_internal_by_id( engine,
						     req->x.port_info.client_id))
	== NULL) {
	jack_error ("unknown client id in port registration request");
	jack_unlock_graph( (&engine) );
	return -1;
    }

    if ((port = jack_engine_get_port_by_name( engine, req->x.port_info.name) ) != NULL) {
	jack_error ("duplicate port name (%s) in port registration request", req->x.port_info.name);
	jack_unlock_graph( (&engine) );
	return -1;
    }

    if ((port_id = jack_engine_get_free_port(engine)) == (jack_port_id_t) -1) {
	jack_error ("no ports available!");
	jack_unlock_graph( (&engine) );
	return -1;
    }

    shared = &engine.control->ports[port_id];

    if (!internal || !engine.driver) {
	goto fallback;
    }

    /* if the port belongs to the backend client, do some magic with names 
     */

    backend_client_name = (char *)engine.driver->internal_client->control->name;
    len = strlen (backend_client_name);

    if (strncmp (req->x.port_info.name, backend_client_name, len) != 0) {
	goto fallback;
    }

    /* use backend's original as an alias, use predefined names */

    if (strcmp(req->x.port_info.type, JACK_DEFAULT_AUDIO_TYPE) == 0) {
	if ((req->x.port_info.flags & (JackPortIsPhysical|JackPortIsInput)) == (JackPortIsPhysical|JackPortIsInput)) {
	    snprintf (shared->name, sizeof (shared->name), JACK_BACKEND_ALIAS ":playback_%d", ++engine.audio_out_cnt);
	    strcpy (shared->alias1, req->x.port_info.name);
	    goto next;
	} 
	else if ((req->x.port_info.flags & (JackPortIsPhysical|JackPortIsOutput)) == (JackPortIsPhysical|JackPortIsOutput)) {
	    snprintf (shared->name, sizeof (shared->name), JACK_BACKEND_ALIAS ":capture_%d", ++engine.audio_in_cnt);
	    strcpy (shared->alias1, req->x.port_info.name);
	    goto next;
	}
    }

#if 0 // do not do this for MIDI

    else if (strcmp(req->x.port_info.type, JACK_DEFAULT_MIDI_TYPE) == 0) {
	if ((req->x.port_info.flags & (JackPortIsPhysical|JackPortIsInput)) == (JackPortIsPhysical|JackPortIsInput)) {
	    snprintf (shared->name, sizeof (shared->name), JACK_BACKEND_ALIAS ":midi_playback_%d", ++engine.midi_out_cnt);
	    strcpy (shared->alias1, req->x.port_info.name);
	    goto next;
	} 
	else if ((req->x.port_info.flags & (JackPortIsPhysical|JackPortIsOutput)) == (JackPortIsPhysical|JackPortIsOutput)) {
	    snprintf (shared->name, sizeof (shared->name), JACK_BACKEND_ALIAS ":midi_capture_%d", ++engine.midi_in_cnt);
	    strcpy (shared->alias1, req->x.port_info.name);
	    goto next;
	}
    }
#endif

  fallback:
    strcpy (shared->name, req->x.port_info.name);

  next:
    shared->ptype_id = engine.control->port_types[i].ptype_id;
    jack_uuid_copy (&shared->client_id, req->x.port_info.client_id);
    shared->uuid = jack_port_uuid_generate (port_id);
    shared->flags = req->x.port_info.flags;
    shared->latency = 0;
    shared->capture_latency.min = shared->capture_latency.max = 0;
    shared->playback_latency.min = shared->playback_latency.max = 0;
    shared->monitor_requests = 0;

    port = &engine.internal_ports[port_id];

    port->shared = shared;
    port->connections_vector.clear();
    port->buffer_info = NULL;
	
    if (jack_engine_port_assign_buffer( engine, port) ) {
	jack_error ("cannot assign buffer for port");
	jack_engine_port_release( engine, &engine.internal_ports[port_id]);
	jack_unlock_graph( (&engine) );
	return -1;
    }

    client->ports_vector.push_back( port );
    if( client->control->active )
	jack_engine_port_registration_notify( engine, port_id, TRUE);
    jack_unlock_graph( (&engine) );

    VERBOSE( &engine, "registered port %s, offset = %u",
	     shared->name, (unsigned int)shared->offset);

    req->x.port_info.port_id = port_id;

    return 0;
}

static void jack_engine_clear_fifos( jack_engine_t & engine )
{
    /* caller must hold client_lock */

    unsigned int i;
    char buf[16];

    /* this just drains the existing FIFO's of any data left in
       them by aborted clients, etc. there is only ever going to
       be 0, 1 or 2 bytes in them, but we'll allow for up to 16.
    */
    for (i = 0; i < engine.fifo_size; i++) {
	if (engine.fifo[i] >= 0) {
	    int nread = read (engine.fifo[i], buf, sizeof (buf));

	    if (nread < 0 && errno != EAGAIN) {
		jack_error ("clear fifo[%d] error: %s",
			    i, strerror (errno));
	    } 
	}
    }
}

int jack_engine_get_fifo_fd( jack_engine_t & engine, unsigned int which_fifo )
{
    /* caller must hold client_lock */
    char path[PATH_MAX+1];
    struct stat statbuf;

    snprintf (path, sizeof (path), "%s-%d", engine.fifo_prefix,
	      which_fifo);

    DEBUG ("%s", path);

    if (stat (path, &statbuf)) {
	if (errno == ENOENT) {

	    if (mkfifo(path, 0666) < 0){
		jack_error ("cannot create inter-client FIFO"
			    " [%s] (%s)\n", path,
			    strerror (errno));
		return -1;
	    }

	} else {
	    jack_error ("cannot check on FIFO %d\n", which_fifo);
	    return -1;
	}
    } else {
	if (!S_ISFIFO(statbuf.st_mode)) {
	    jack_error ("FIFO %d (%s) already exists, but is not"
			" a FIFO!\n", which_fifo, path);
	    return -1;
	}
    }

    if (which_fifo >= engine.fifo_size) {
	unsigned int i;

	engine.fifo = (int *)
	    realloc (engine.fifo,
		     sizeof (int) * (engine.fifo_size + 16));
	for (i = engine.fifo_size; i < engine.fifo_size + 16; i++) {
	    engine.fifo[i] = -1;
	}
	engine.fifo_size += 16;
    }

    if (engine.fifo[which_fifo] < 0) {
	if ((engine.fifo[which_fifo] =
	     open (path, O_RDWR|O_CREAT|O_NONBLOCK, 0666)) < 0) {
	    jack_error ("cannot open fifo [%s] (%s)", path,
			strerror (errno));
	    return -1;
	}
	DEBUG ("opened engine.fifo[%d] == %d (%s)",
	       which_fifo, engine.fifo[which_fifo], path);
    }

    return engine.fifo[which_fifo];
}

int jack_engine_rechain_graph( jack_engine_t & engine )
{
    unsigned long n;
    int err = 0;
    jack_client_internal_t *subgraph_client, *next_client;
    jack_event_t event;
    int upstream_is_jackd;

    VALGRIND_MEMSET(&event, 0, sizeof (event));

    jack_engine_clear_fifos( engine );

    subgraph_client = 0;

    VERBOSE( &engine, "++ jack_engine_rechain_graph():");

    event.type = GraphReordered;

    vector<jack_client_internal_t*>::iterator current_iterator = engine.clients.begin();
    vector<jack_client_internal_t*>::iterator end_marker = engine.clients.end();

    for( n = 0 ; current_iterator != end_marker ; ++current_iterator ) {
	jack_client_internal_t * client = *current_iterator;
	vector<jack_client_internal_t*>::iterator next_iterator = current_iterator + 1;
                
	jack_client_control_t * ctl = client->control;

	if (!ctl->process_cbset && !ctl->thread_cb_cbset) {
	    continue;
	}

	VERBOSE( &engine, "+++ client is now %s active ? %d", ctl->name, ctl->active);

	if( ctl->active ) {

	    /* find the next active client. its ok for
	     * this to be NULL */
	    while( next_iterator != end_marker ) {
		if( ctl->active &&
		    (ctl->process_cbset || ctl->thread_cb_cbset)) {
		    break;
		}
		++next_iterator;
	    };

	    if( next_iterator == end_marker ) {
		next_client = NULL;
	    } else {
		next_client = *next_iterator;
	    }

	    client->execution_order = n;
	    client->next_client = next_client;
			
	    if( jack_client_is_internal( client ) ) {
				
		/* break the chain for the current
		 * subgraph. the server will wait for
		 * chain on the nth FIFO, and will
		 * then execute this internal
		 * client. */
				
		if (subgraph_client) {
		    subgraph_client->subgraph_wait_fd =
			jack_engine_get_fifo_fd( engine, n );
		    VERBOSE( &engine, "client %s: wait_fd="
			     "%d, execution_order="
			     "%lu.", 
			     subgraph_client->control->name,
			     subgraph_client->subgraph_wait_fd,
			     n);
		    n++;
		}

		VERBOSE( &engine, "client %s: internal "
			 "client, execution_order="
			 "%lu.", 
			 ctl->name, n);

		/* this does the right thing for
		 * internal clients too 
		 */

		jack_engine_deliver_event( engine, client, &event );

		subgraph_client = 0;

	    } else {
				
		if (subgraph_client == NULL) {

		    /* start a new subgraph. the
		     * engine will start the chain
		     * by writing to the nth
		     * FIFO. 
		     */

		    subgraph_client = client;
		    subgraph_client->subgraph_start_fd =
			jack_engine_get_fifo_fd( engine, n );
		    VERBOSE( &engine, "client %s: "
			     "start_fd=%d, execution"
			     "_order=%lu.",
			     subgraph_client->control->name,
			     subgraph_client->subgraph_start_fd,
			     n);

		    /* this external client after
		       this will have jackd as its
		       upstream connection.
		    */

		    upstream_is_jackd = 1;

		} 
		else {
		    VERBOSE( &engine, "client %s: in"
			     " subgraph after %s, "
			     "execution_order="
			     "%lu.",
			     ctl->name,
			     subgraph_client->control->name,
			     n);
		    subgraph_client->subgraph_wait_fd = -1;

		    /* this external client after
		       this will have another
		       client as its upstream
		       connection.
		    */

		    upstream_is_jackd = 0;
		}

		/* make sure fifo for 'n + 1' exists
		 * before issuing client reorder
		 */
		(void)jack_engine_get_fifo_fd( engine,
					       client->execution_order + 1);
		event.x.n = client->execution_order;
		event.y.n = upstream_is_jackd;
		jack_engine_deliver_event( engine, client, &event);
		n++;
	    }
	}
    }

    if (subgraph_client) {
	subgraph_client->subgraph_wait_fd = jack_engine_get_fifo_fd( engine, n );
	VERBOSE( &engine, "client %s: wait_fd=%d, execution_order=%lu (last client).",
		 subgraph_client->control->name,
		 subgraph_client->subgraph_wait_fd, n);
    }

    VERBOSE( &engine, "-- jack_engine_rechain_graph()");

    return err;
}

struct jack_engine_clients_compare
{
    bool operator()( jack_client_internal_t * a, jack_client_internal_t * b ) {
	/* drivers are forced to the front, ie considered as sources
	   rather than sinks for purposes of the sort */

	if( jack_client_feeds_transitive( a, b ) ||
	    (a->control->type == ClientDriver &&
	     b->control->type != ClientDriver)) {
	    return true;
	}
	else if( jack_client_feeds_transitive( b, a ) ||
		 (b->control->type == ClientDriver &&
		  a->control->type != ClientDriver)) {
	    return false;
	}
	else {
	    return a < b;
	}
    }
};

/* How the sort works:
 *
 * Each client has a "sortfeeds" list of clients indicating which clients
 * it should be considered as feeding for the purposes of sorting the
 * graph. This list differs from the clients it /actually/ feeds in the
 * following ways:
 *
 * 1. Connections from a client to itself are disregarded
 *
 * 2. Connections to a driver client are disregarded
 *
 * 3. If a connection from A to B is a feedback connection (ie there was
 *    already a path from B to A when the connection was made) then instead
 *    of B appearing on A's sortfeeds list, A will appear on B's sortfeeds
 *    list.
 *
 * If client A is on client B's sortfeeds list, client A must come after
 * client B in the execution order. The above 3 rules ensure that the
 * sortfeeds relation is always acyclic so that all ordering constraints
 * can actually be met. 
 *
 * Each client also has a "truefeeds" list which is the same as sortfeeds
 * except that feedback connections appear normally instead of reversed.
 * This is used to detect whether the graph has become acyclic.
 *
 */ 

void jack_engine_sort_graph( jack_engine_t & engine )
{
    /* called, obviously, must hold engine->client_lock */

    VERBOSE( &engine, "++ jack_engine_sort_graph");

    std::sort( engine.clients.begin(), engine.clients.end(), jack_engine_clients_compare() );

    jack_engine_compute_all_port_total_latencies( engine );
    jack_engine_compute_new_latency( engine );
    jack_engine_rechain_graph( engine );
    engine.timeout_count = 0;

    VERBOSE( &engine, "-- jack_engine_sort_graph");
}

static void jack_engine_port_connection_remove( jack_engine_t & engine,
    std::vector<jack_connection_internal_t*> & connections,
    jack_connection_internal_t * to_remove )
{
    auto cFinder = std::find(connections.begin(), connections.end(), to_remove );

    if( cFinder == connections.end() ) {
	VERBOSE( &engine, "-- failed port connection removal");
    }
    else {
	connections.erase( cFinder );
    }
}

static void jack_engine_client_truefeed_remove(
    vector<jack_client_internal_t*> & truefeeds,
    jack_client_internal_t * client_to_remove )
{
    auto tf_finder = std::find( truefeeds.begin(),
				truefeeds.end(),
				client_to_remove );
    if( tf_finder != truefeeds.end() ) {
	truefeeds.erase( tf_finder );
    }
    else {
	jack_error("Failed to find client to remove from truefeeds");
    }
}

static void jack_engine_client_sortfeed_remove(
    vector<jack_client_internal_t*> & sortfeeds,
    jack_client_internal_t * client_to_remove )
{
    auto sf_finder = std::find( sortfeeds.begin(),
				sortfeeds.end(),
				client_to_remove );
    if( sf_finder != sortfeeds.end() ) {
	sortfeeds.erase( sf_finder );
    }
    else {
	jack_error("Failed to find client to remove from sortfeeds");
    }
}

static int jack_engine_port_disconnect_internal(
    jack_engine_t & engine, 
    jack_port_internal_t *srcport, 
    jack_port_internal_t *dstport )

{
    int ret = -1;
    jack_port_id_t src_id, dst_id;
    int check_acyclic = engine.feedbackcount;

    /* call tree **** MUST HOLD **** engine->client_lock. */
    for( jack_connection_internal_t * connection : srcport->connections_vector ) {
	if( connection->source == srcport &&
	    connection->destination == dstport ) {
	    VERBOSE( &engine, "connections_vector DIS-connect %s and %s",
		     srcport->shared->name,
		     dstport->shared->name);

	    jack_engine_port_connection_remove( engine, srcport->connections_vector, connection );
	    jack_engine_port_connection_remove( engine, dstport->connections_vector, connection );

	    src_id = srcport->shared->id;
	    dst_id = dstport->shared->id;

	    // this is a bit harsh, but it basically says
	    // that if we actually do a disconnect, and
	    // its the last one, then make sure that any
	    // input monitoring is turned off on the
	    // srcport. this isn't ideal for all
	    // situations, but it works better for most of
	    // them.

	    if( srcport->connections_vector.size() == 0 ) {
		srcport->shared->monitor_requests = 0;
	    }

	    jack_engine_send_connection_notification(
		engine, srcport->shared->client_id, src_id, dst_id, FALSE);
	    jack_engine_send_connection_notification (
		engine, dstport->shared->client_id, dst_id, src_id, FALSE);

	    // send a port connection notification just once to everyone who cares
	    // excluding clients involved in the connection

	    jack_engine_notify_all_port_interested_clients(
		engine,
		srcport->shared->client_id, dstport->shared->client_id,
		src_id, dst_id, 0);

	    if( connection->dir ) {

		jack_client_internal_t *src;
		jack_client_internal_t *dst;

		src = jack_engine_client_internal_by_id( engine, srcport->shared->client_id );

		dst = jack_engine_client_internal_by_id( engine, dstport->shared->client_id );

		jack_info( "Attempting to remove from %d to %d",
			   srcport->shared->client_id,
			   dstport->shared->client_id );

		jack_engine_client_truefeed_remove( src->truefeeds_vector, dst );

		dst->fedcount--;

		if (connection->dir == 1) {
		    // normal connection: remove dest from
		    // source's sortfeeds list
		    jack_engine_client_sortfeed_remove( src->sortfeeds_vector, dst );
		} else {
		    // feedback connection: remove source
		    // from dest's sortfeeds list
		    jack_engine_client_sortfeed_remove( dst->sortfeeds_vector, src );
		    engine.feedbackcount--;
		    VERBOSE( &engine,
			     "feedback count down to %d",
			     engine.feedbackcount);

		}
	    } // else self-connection: do nothing

	    free( connection );
	    ret = 0;
	    break;
	}
    }

    if( check_acyclic ) {
	jack_engine_check_acyclic( engine );
    }
	
    jack_engine_sort_graph( engine );

    return ret;
}

void jack_engine_port_clear_connections( jack_engine_t & engine,
					 jack_port_internal_t *port )
{
    // Take a copy so our iterators aren't invalidated
    vector<jack_connection_internal_t*> port_connections = port->connections_vector;
    for( jack_connection_internal_t * connection : port_connections ) {
	jack_engine_port_disconnect_internal (
	    engine,
	    connection->source,
	    connection->destination);
    }

    port->connections_vector.clear();
}

static void jack_client_remove_client_port( std::vector<jack_port_internal_t*> & ports_vector,
					    jack_port_internal_t * port )
{
    auto p_finder = std::find( ports_vector.begin(), ports_vector.end(), port );

    if( p_finder == ports_vector.end() ) {
	jack_error( "Failed to find client port to erase" );
    }
    else {
	ports_vector.erase( p_finder );
    }
}

int jack_engine_port_do_unregister( jack_engine_t & engine, jack_request_t *req )
{
    jack_client_internal_t *client;
    jack_port_shared_t *shared;
    jack_port_internal_t *port;
    jack_uuid_t uuid;

    if (req->x.port_info.port_id < 0 ||
	req->x.port_info.port_id > engine.port_max) {
	jack_error( "invalid port ID %" PRIu32
		    " in unregister request",
		    req->x.port_info.port_id);
	return -1;
    }

    shared = &engine.control->ports[req->x.port_info.port_id];

    if (jack_uuid_compare (shared->client_id, req->x.port_info.client_id) != 0) {
	char buf[JACK_UUID_STRING_SIZE];
	jack_uuid_unparse (req->x.port_info.client_id, buf);
	jack_error ("Client %s is not allowed to remove port %s",
		    buf, shared->name);
	return -1;
    }

    jack_uuid_copy (&uuid, shared->uuid);

    jack_lock_graph( (&engine) );
    if ((client = jack_engine_client_internal_by_id( engine, shared->client_id))
	== NULL) {
	jack_error ("unknown client id in port registration request");
	jack_unlock_graph( (&engine) );
	return -1;
    }

    port = &engine.internal_ports[req->x.port_info.port_id];

    jack_engine_port_clear_connections( engine, port );
    jack_engine_port_release( engine, &engine.internal_ports[req->x.port_info.port_id]);
	
    jack_client_remove_client_port( client->ports_vector, port );
    jack_engine_port_registration_notify( engine, req->x.port_info.port_id,
					  FALSE);
    jack_unlock_graph( (&engine) );

    return 0;
}

/* transitive closure of the relation expressed by the sortfeeds lists. */
static int jack_client_feeds_transitive (jack_client_internal_t *source,
					 jack_client_internal_t *dest )
{
    jack_client_internal_t *med;

    auto sf_iter = source->sortfeeds_vector.begin();
    auto sf_end = source->sortfeeds_vector.end();

    auto d_finder = std::find( sf_iter,
			       sf_end,
			       dest );

    if( d_finder != sf_end ) {
	return 1;
    }

    for( ; sf_iter != sf_end ; ++sf_iter ) {
	med = *sf_iter;

	if (jack_client_feeds_transitive (med, dest)) {
	    return 1;
	}
    }

    return 0;
}

static int jack_engine_send_connection_notification( jack_engine_t & engine,
						     jack_uuid_t client_id, 
						     jack_port_id_t self_id,
						     jack_port_id_t other_id,
						     int connected )

{
    jack_client_internal_t *client;
    jack_event_t event;
 
    VALGRIND_MEMSET(&event, 0, sizeof(event));

    if ((client = jack_engine_client_internal_by_id( engine, client_id)) == NULL) {
	jack_error ("no such client %" PRIu32
		    " during connection notification", client_id);
	return -1;
    }

    if (client->control->active) {
	event.type = (connected ? PortConnected : PortDisconnected);
	event.x.self_id = self_id;
	event.y.other_id = other_id;
		
	if (jack_engine_deliver_event( engine, client, &event)) {
	    jack_error ("cannot send port connection notification"
			" to client %s (%s)", 
			client->control->name, strerror (errno));
	    return -1;
	}
    }

    return 0;
}

static void jack_engine_notify_all_port_interested_clients(
    jack_engine_t & engine,
    jack_uuid_t src,
    jack_uuid_t dst,
    jack_port_id_t a,
    jack_port_id_t b,
    int connected )
{
    jack_event_t event;
  
    event.type = (connected ? PortConnected : PortDisconnected);
    event.x.self_id = a;
    event.y.other_id = b;
 	
    /* GRAPH MUST BE LOCKED : see callers of jack_engine_send_connection_notification() 
     */

    jack_client_internal_t* src_client = jack_engine_client_internal_by_id( engine, src);
    jack_client_internal_t* dst_client = jack_engine_client_internal_by_id( engine, dst);

    for( jack_client_internal_t * client : engine.clients ) {
	if (src_client != client && dst_client != client && client->control->port_connect_cbset != FALSE) {
			
	    /* one of the ports belong to this client or it has a port connect callback */
	    jack_engine_deliver_event( engine, client, &event);
	} 
    }
}

/**
 * Dumps current engine configuration.
 */
void jack_engine_dump_configuration( jack_engine_t & engine, int take_lock )
{
    jack_info ("engine.c: <-- dump begins -->");

    if (take_lock) {
	jack_rdlock_graph( (&engine) );
    }

    int n = 0;
    int m = 0;
    int o = 0;

    for( jack_client_internal_t * client : engine.clients ) {
	jack_client_control_t *ctl = client->control;

	jack_info ("client #%d: %s (type: %d, process? %s, thread ? %s"
		   " start=%d wait=%d",
		   ++n,
		   ctl->name,
		   ctl->type,
		   ctl->process_cbset ? "yes" : "no",
		   ctl->thread_cb_cbset ? "yes" : "no",
		   client->subgraph_start_fd,
		   client->subgraph_wait_fd);

	for( jack_port_internal_t * port : client->ports_vector ) {
	    jack_info("\t port #%d: %s", ++m, port->shared->name);

	    for( jack_connection_internal_t * connection : port->connections_vector ) {
		jack_info("\t\t connection #%d: %s %s",
			  ++o,
			  (port->shared->flags
			   & JackPortIsInput)? "<-": "->",
			  (port->shared->flags & JackPortIsInput)?
			  connection->source->shared->name:
			  connection->destination->shared->name);
	    }
	}
    }

    if (take_lock) {
	jack_unlock_graph( (&engine) );
    }

    jack_info("jack_engine.cpp: <-- dump ends -->");
}

static int jack_engine_port_do_connect( jack_engine_t & engine,
					const char *source_port,
					const char *destination_port)
{
    jack_connection_internal_t *connection;
    jack_port_internal_t *srcport, *dstport;
    jack_port_id_t src_id, dst_id;
    jack_client_internal_t *srcclient, *dstclient;

    if ((srcport = jack_engine_get_port_by_name( engine, source_port)) == NULL) {
	jack_error ("unknown source port in attempted connection [%s]",
		    source_port);
	return -1;
    }

    if ((dstport = jack_engine_get_port_by_name( engine, destination_port))
	== NULL) {
	jack_error ("unknown destination port in attempted connection"
		    " [%s]", destination_port);
	return -1;
    }

    if ((dstport->shared->flags & JackPortIsInput) == 0) {
	jack_error ("destination port in attempted connection of"
		    " %s and %s is not an input port", 
		    source_port, destination_port);
	return -1;
    }

    if ((srcport->shared->flags & JackPortIsOutput) == 0) {
	jack_error ("source port in attempted connection of %s and"
		    " %s is not an output port",
		    source_port, destination_port);
	return -1;
    }

    if (srcport->shared->ptype_id != dstport->shared->ptype_id) {
	jack_error ("ports used in attemped connection are not of "
		    "the same data type");
	return -1;
    }

    if ((srcclient = jack_engine_client_internal_by_id( engine,
							srcport->shared->client_id))
	== 0) {
	jack_error ("unknown client set as owner of port - "
		    "cannot connect");
	return -1;
    }
	
    if (!srcclient->control->active) {
	jack_error ("cannot connect ports owned by inactive clients;"
		    " \"%s\" is not active", srcclient->control->name);
	return -1;
    }

    if ((dstclient = jack_engine_client_internal_by_id( engine,
							dstport->shared->client_id))
	== 0) {
	jack_error ("unknown client set as owner of port - cannot "
		    "connect");
	return -1;
    }
	
    if (!dstclient->control->active) {
	jack_error ("cannot connect ports owned by inactive clients;"
		    " \"%s\" is not active", dstclient->control->name);
	return -1;
    }

    for( jack_connection_internal_t * con : srcport->connections_vector ) {
	if( con->destination == dstport ) {
	    return EEXIST;
	}
    }

    connection = (jack_connection_internal_t *)malloc( sizeof (jack_connection_internal_t) );

    connection->source = srcport;
    connection->destination = dstport;
    connection->srcclient = srcclient;
    connection->dstclient = dstclient;

    src_id = srcport->shared->id;
    dst_id = dstport->shared->id;

    jack_lock_graph( (&engine) );

    if( dstport->connections_vector.size() > 0 && !dstport->shared->has_mixdown) {
	jack_port_type_info_t *port_type = jack_engine_port_type_info( engine, dstport);
	jack_error ("cannot make multiple connections to a port of"
		    " type [%s]", port_type->type_name);
	free (connection);
	jack_unlock_graph( (&engine) );
	return -1;
    } else {

	if (dstclient->control->type == ClientDriver)
	{
	    /* Ignore output connections to drivers for purposes
	       of sorting. Drivers are executed first in the sort
	       order anyway, and we don't want to treat graphs
	       such as driver -> client -> driver as containing
	       feedback */
			
	    VERBOSE( &engine,
		     "connect %s and %s (output)",
		     srcport->shared->name,
		     dstport->shared->name);

	    connection->dir = 1;

	}
	else if (srcclient != dstclient) {
		
	    srcclient->truefeeds_vector.push_back( dstclient );

	    dstclient->fedcount++;				

	    if (jack_client_feeds_transitive (dstclient,
					      srcclient ) ||
		(dstclient->control->type == ClientDriver &&
		 srcclient->control->type != ClientDriver)) {
		    
		/* dest is running before source so
		   this is a feedback connection */
				
		VERBOSE( &engine,
			 "connect %s and %s (feedback)",
			 srcport->shared->name,
			 dstport->shared->name);
				 
		dstclient->sortfeeds_vector.push_back( srcclient );

		connection->dir = -1;
		engine.feedbackcount++;
		VERBOSE( &engine,
			 "feedback count up to %d",
			 engine.feedbackcount);

	    } else {
		
		/* this is not a feedback connection */

		VERBOSE( &engine, "connect %s and %s (forward)",
			 srcport->shared->name,
			 dstport->shared->name);

		srcclient->sortfeeds_vector.push_back( dstclient );

		connection->dir = 1;
	    }
	}
	else
	{
	    /* this is a connection to self */

	    VERBOSE( &engine,
		     "connect %s and %s (self)",
		     srcport->shared->name,
		     dstport->shared->name);
			
	    connection->dir = 0;
	}

	dstport->connections_vector.push_back( connection );
	srcport->connections_vector.push_back( connection );
		
	DEBUG ("actually sorted the graph...");

	jack_engine_send_connection_notification( engine,
						  srcport->shared->client_id,
						  src_id, dst_id, TRUE);
		

	jack_engine_send_connection_notification( engine,
						  dstport->shared->client_id,
						  dst_id, src_id, TRUE);
						   
	/* send a port connection notification just once to everyone who cares excluding clients involved in the connection */

	jack_engine_notify_all_port_interested_clients(
	    engine, srcport->shared->client_id, dstport->shared->client_id,
	    src_id, dst_id, 1);

	jack_engine_sort_graph( engine );
    }

    jack_unlock_graph( (&engine) );

    return 0;
}

static int jack_engine_port_do_disconnect_all( jack_engine_t & engine,
					       jack_port_id_t port_id)
{
    if (port_id >= engine.control->port_max) {
	jack_error ("illegal port ID in attempted disconnection [%"
		    PRIu32 "]", port_id);
	return -1;
    }

    VERBOSE( &engine, "clear connections for %s",
	     engine.internal_ports[port_id].shared->name);

    jack_lock_graph( (&engine) );
    jack_engine_port_clear_connections( engine, &engine.internal_ports[port_id] );
    jack_engine_sort_graph( engine );
    jack_unlock_graph( (&engine) );

    return 0;
}

/**
 * Checks whether the graph has become acyclic and if so modifies client
 * sortfeeds lists to turn leftover feedback connections into normal ones.
 * This lowers latency, but at the expense of some data corruption.
 */
static void jack_engine_check_acyclic( jack_engine_t & engine )
{

    jack_client_internal_t *src, *dst;
    jack_port_internal_t *port;
    jack_connection_internal_t *conn;
    int stuck;
    int unsortedclients = 0;

    VERBOSE( &engine, "checking for graph become acyclic");

    for( auto cvIter = engine.clients.begin(), end = engine.clients.end() ;
	 cvIter != end ; ++cvIter ) {
	src = *cvIter;
	src->tfedcount = src->fedcount;
	unsortedclients++;
    }
	
    stuck = FALSE;

    /* find out whether a normal sort would have been possible */
    while (unsortedclients && !stuck) {
	
	stuck = TRUE;

	for( auto cvIter = engine.clients.begin(), end = engine.clients.end() ;
	     cvIter != end ; ++cvIter ) {
	    src = *cvIter;
			
	    if (!src->tfedcount) {
			
		stuck = FALSE;
		unsortedclients--;
		src->tfedcount = -1;
				
		for( auto tf_iter = src->truefeeds_vector.begin(),
			 tf_end = src->truefeeds_vector.end() ;
		     tf_iter != tf_end ;
		     ++tf_iter ) {
		    dst = *tf_iter;
		    dst->tfedcount--;
		}
	    }
	}
    }
	
    if (stuck) {

	VERBOSE( &engine, "graph is still cyclic" );
    } else {

	VERBOSE( &engine, "graph has become acyclic");

	/* turn feedback connections around in sortfeeds */
	for( auto cvIter = engine.clients.begin(), end = engine.clients.end() ;
	     cvIter != end ; ++cvIter ) {
	    src = *cvIter;

	    for( auto sp_iter = src->ports_vector.begin(), sp_end = src->ports_vector.end() ;
		 sp_iter != sp_end ; ++sp_iter ) {
		port = *sp_iter;
			
		vector<jack_connection_internal_t*>::iterator con_iter = port->connections_vector.begin();
		vector<jack_connection_internal_t*>::iterator con_end = port->connections_vector.end();
		for( ; con_iter != con_end ; ++con_iter ) {
		    conn = *con_iter;
		    if( conn->dir == -1 ) /* && conn->srcclient == src) */{
				
			VERBOSE( &engine,
				 "reversing connection from "
				 "%s to %s",
				 conn->srcclient->control->name,
				 conn->dstclient->control->name);
			conn->dir = 1;
			jack_engine_client_sortfeed_remove( conn->dstclient->sortfeeds_vector,
							    conn->srcclient );
					     
			jack_engine_client_sortfeed_remove( conn->srcclient->sortfeeds_vector,
							    conn->dstclient );
		    }
		}
	    }
	}
	engine.feedbackcount = 0;
    }
}

static int jack_engine_port_do_disconnect( jack_engine_t & engine,
					   const char *source_port,
					   const char *destination_port)
{
    jack_port_internal_t *srcport, *dstport;
    int ret = -1;

    if ((srcport = jack_engine_get_port_by_name( engine, source_port ) ) == NULL) {
	jack_error ("unknown source port in attempted disconnection"
		    " [%s]", source_port);
	return -1;
    }

    if ((dstport = jack_engine_get_port_by_name( engine, destination_port ))
	== NULL) {
	jack_error ("unknown destination port in attempted"
		    " disconnection [%s]", destination_port);
	return -1;
    }

    jack_lock_graph( (&engine) );

    ret = jack_engine_port_disconnect_internal( engine, srcport, dstport );

    jack_unlock_graph( (&engine) );

    return ret;
}

int jack_engine_do_get_port_connections( jack_engine_t & engine, jack_request_t *req, int reply_fd )
{
    unsigned int i;
    int ret = -1;
    int internal = FALSE;

    jack_rdlock_graph( (&engine) );

    jack_port_internal_t *port = &engine.internal_ports[req->x.port_info.port_id];

    DEBUG ("Getting connections for port '%s'.", port->shared->name);

    req->x.port_connections.nports = port->connections_vector.size();
    req->status = 0;

    /* figure out if this is an internal or external client */
    auto iFinder = std::find_if( engine.clients.begin(),
				 engine.clients.end(),
				 [&reply_fd] ( jack_client_internal_t * client ) {
				     return client->request_fd == reply_fd;
				 } );

    if( iFinder != engine.clients.end() ) {
	internal = jack_client_is_internal( *iFinder );
    }

    if (!internal) {
	if (write (reply_fd, req, sizeof (*req))
	    < (ssize_t) sizeof (req)) {
	    jack_error ("cannot write GetPortConnections result "
			"to client via fd = %d (%s)", 
			reply_fd, strerror (errno));
	    goto out;
	}
    } else {
	req->x.port_connections.ports =
	    (const char**)malloc (sizeof (char *) * req->x.port_connections.nports);
    }

    if (req->type == GetPortConnections) {
		
	vector<jack_connection_internal_t*>::iterator con_iter = port->connections_vector.begin();
	vector<jack_connection_internal_t*>::iterator con_end = port->connections_vector.end();
	for( i = 0 ; con_iter != con_end ; ++con_iter, ++i ) {
	    jack_connection_internal_t * tst_con = *con_iter;

	    jack_port_id_t port_id;

	    if ( tst_con->source == port ) {
		port_id = tst_con->destination->shared->id;
	    } else {
		port_id = tst_con->source->shared->id;
	    }
			
	    if (internal) {

		/* internal client asking for
		 * names. store in malloc'ed space,
		 * client frees
		 */
		char **ports = (char **) req->x.port_connections.ports;

		ports[i] = engine.control->ports[port_id].name;

	    } else {

		/* external client asking for
		 * names. we write the port id's to
		 * the reply fd.
		 */
		if (write (reply_fd, &port_id, sizeof (port_id))
		    < (ssize_t) sizeof (port_id)) {
		    jack_error ("cannot write port id to client");
		    goto out;
		}
	    }
	}
    }

    ret = 0;

  out:
    req->status = ret;
    jack_unlock_graph( (&engine) );
    return ret;
}

static int jack_engine_drivers_stop( jack_engine_t & engine )
{
    /* first stop the master driver */
    int retval = engine.driver->stop( engine.driver );

    /* now the slave drivers are stopped */
    for( jack_driver_t * sdriver : engine.slave_drivers ) {
	sdriver->stop( sdriver );
    }

    return retval;
}

static void* jack_engine_freewheel( void *arg )
{
    jack_engine_t* engine = (jack_engine_t *) arg;
    jack_client_internal_t* client;

    VERBOSE( engine, "freewheel thread starting ...");

    /* we should not be running SCHED_FIFO, so we don't 
       have to do anything about scheduling.
    */

    client = jack_engine_client_internal_by_id( *engine, engine->fwclient);

    while (!engine->stop_freewheeling) {

	jack_engine_run_one_cycle( *engine, engine->control->buffer_size, 0.0f);

	if (client && client->error) {
	    /* run one cycle() will already have told the server thread
	       about issues, and the server thread will clean up.
	       however, its time for us to depart this world ...
	    */
	    break;
	}
    }

    VERBOSE( engine, "freewheel came to an end, naturally");
    return 0;
}

static int jack_engine_start_freewheeling( jack_engine_t & engine, jack_uuid_t client_id )
{
    jack_event_t event;
    jack_client_internal_t *client;

    if( engine.freewheeling ) {
	return 0;
    }

    if( engine.driver == NULL ) {
	jack_error ("cannot start freewheeling without a driver!");
	return -1;
    }

    /* stop driver before telling anyone about it so 
       there are no more process() calls being handled.
    */

    if( jack_engine_drivers_stop( engine ) ) {
	jack_error ("could not stop driver for freewheeling");
	return -1;
    }

    client = jack_engine_client_internal_by_id( engine, client_id);

    if (client->control->process_cbset || client->control->thread_cb_cbset) {
	jack_uuid_copy( &engine.fwclient, client_id );
    }

    engine.freewheeling = 1;
    engine.stop_freewheeling = 0;

    event.type = StartFreewheel;
    jack_engine_deliver_event_to_all( engine, &event );
	
    if (jack_client_create_thread (NULL, &engine.freewheel_thread, 0, FALSE,
				   jack_engine_freewheel, &engine)) {
	jack_error ("could not start create freewheel thread");
	return -1;
    }

    return 0;
}

/* handle client SetBufferSize request */
int jack_engine_set_buffer_size_request( jack_engine_t & engine, jack_nframes_t nframes )
{
    /* precondition: caller holds the request_lock */
    int rc;
    jack_driver_t* driver = engine.driver;

    if (driver == NULL)
	return ENXIO;		/* no such device */

    if (!jack_power_of_two(nframes)) {
	jack_error("buffer size %" PRIu32 " not a power of 2",
		   nframes);
	return EINVAL;
    }

    rc = driver->bufsize(driver, nframes);
    if (rc != 0)
	jack_error("driver does not support %" PRIu32
		   "-frame buffers", nframes);

    return rc;
}

static void jack_engine_compute_new_latency( jack_engine_t & engine )
{
    jack_event_t event;

    VALGRIND_MEMSET (&event, 0, sizeof (event));

    event.type = LatencyCallback;
    event.x.n  = 0;

    /* iterate over all clients in graph order, and emit
     * capture latency callback.
     * also builds up list in reverse graph order.
     */
    for( jack_client_internal_t * client : engine.clients ) {
	jack_engine_deliver_event( engine, client, &event);
    }

    if (engine.driver) {
	jack_engine_deliver_event( engine, engine.driver->internal_client, &event);
    }

    /* now issue playback latency callbacks in reverse graphorder
     */
    event.x.n  = 1;
    for( auto rcIter = engine.clients.rbegin(), end = engine.clients.rend() ;
	 rcIter != end ; ++rcIter ) {
	jack_client_internal_t * client = *rcIter;
	jack_engine_deliver_event( engine, client, &event);
    }

    if (engine.driver) {
	jack_engine_deliver_event( engine, engine.driver->internal_client, &event);
    }
}

static jack_nframes_t jack_engine_get_port_total_latency(
    jack_engine_t & engine, jack_port_internal_t *port, int hop_count, int toward_port )
{
    jack_nframes_t latency;
    jack_nframes_t max_latency = 0;

#ifdef DEBUG_TOTAL_LATENCY_COMPUTATION
    char prefix[32];
    int i;

    for (i = 0; i < hop_count; ++i) {
	prefix[i] = '\t';
    }

    prefix[i] = '\0';
#endif

    /* call tree must hold engine->client_lock. */

    latency = port->shared->latency;

    /* we don't prevent cyclic graphs, so we have to do something
       to bottom out in the event that they are created.
    */

    if (hop_count > 8) {
	return latency;
    }

#ifdef DEBUG_TOTAL_LATENCY_COMPUTATION
    jack_info ("%sFor port %s (%s)", prefix, port->shared->name, (toward_port ? "toward" : "away"));
#endif
	
    for( jack_connection_internal_t * connection : port->connections_vector ) {

	jack_nframes_t this_latency;

	if ((toward_port && (connection->source->shared == port->shared)) ||
	    (!toward_port && (connection->destination->shared == port->shared))) {

#ifdef DEBUG_TOTAL_LATENCY_COMPUTATION
	    jack_info ("%s\tskip connection %s->%s",
		       prefix,
		       connection->source->shared->name,
		       connection->destination->shared->name);
#endif

	    continue;
	}

#ifdef DEBUG_TOTAL_LATENCY_COMPUTATION
	jack_info ("%s\tconnection %s->%s ... ", 
		   prefix,
		   connection->source->shared->name,
		   connection->destination->shared->name);
#endif
	/* if we're a destination in the connection, recurse
	   on the source to get its total latency
	*/

	if (connection->destination == port) {

	    if (connection->source->shared->flags & JackPortIsTerminal) {
		this_latency = connection->source->shared->latency;
	    } else {
		this_latency = jack_engine_get_port_total_latency(
		    engine, connection->source, hop_count + 1, 
		    toward_port);
	    }

	} else {

	    /* "port" is the source, so get the latency of
	     * the destination */
	    if (connection->destination->shared->flags & JackPortIsTerminal) {
		this_latency = connection->destination->shared->latency;
	    } else {
		this_latency = jack_engine_get_port_total_latency(
		    engine, connection->destination, hop_count + 1, 
		    toward_port);
	    }
	}

	if (this_latency > max_latency) {
	    max_latency = this_latency;
	}
    }

#ifdef DEBUG_TOTAL_LATENCY_COMPUTATION
    jack_info ("%s\treturn %lu + %lu = %lu", prefix, latency, max_latency, latency + max_latency);
#endif

    return latency + max_latency;
}

static void jack_engine_compute_port_total_latency( jack_engine_t & engine, jack_port_shared_t* port )
{
    if (port->in_use) {
	port->total_latency = jack_engine_get_port_total_latency(
	    engine, &engine.internal_ports[port->id], 0, !(port->flags & JackPortIsOutput));
    }
}

static void jack_engine_compute_all_port_total_latencies( jack_engine_t & engine )
{
    jack_port_shared_t *shared = engine.control->ports;
    unsigned int i;
    int toward_port;

    for (i = 0; i < engine.control->port_max; i++) {
	if (shared[i].in_use) {

	    if (shared[i].flags & JackPortIsOutput) {
		toward_port = FALSE;
	    } else {
		toward_port = TRUE;
	    }

	    shared[i].total_latency =
		jack_engine_get_port_total_latency (
		    engine, &engine.internal_ports[i],
		    0, toward_port);
	}
    }
}

static void jack_engine_client_fill_request_port_name_by_uuid( jack_engine_t & engine, jack_request_t *req )
{
    auto cFinder = std::find_if( engine.clients.begin(),
				 engine.clients.end(),
				 [&req] ( jack_client_internal_t * client ) {
				     return jack_uuid_compare( client->control->uuid, req->x.client_id ) == 0;
				 } );

    if( cFinder != engine.clients.end() ) {
	jack_client_internal_t * client = *cFinder;
	snprintf( req->x.port_info.name, sizeof(req->x.port_info.name), "%s", client->control->name );
	req->status = 0;
    }
    else {
	req->status = -1;
    }
}

static void jack_engine_do_get_uuid_by_client_name( jack_engine_t & engine, jack_request_t *req )
{
    if (strcmp (req->x.name, "system") == 0) {
	/* request concerns the driver */
	if (engine.driver) {
	    jack_uuid_copy (&req->x.client_id, engine.driver->internal_client->control->uuid);
	    req->status = 0;
	}
	else {
	    req->status = -1;
	}
	return;
    }

    auto cFinder = std::find_if( engine.clients.begin(),
				 engine.clients.end(),
				 [&req] ( jack_client_internal_t * client ) {
				     return strcmp( (const char*)client->control->name, req->x.name ) == 0;
				 } );

    if( cFinder != engine.clients.end() ) {
	jack_client_internal_t * client = *cFinder;
	jack_uuid_copy( &req->x.client_id, client->control->uuid );
	req->status = 0;
    }
    else {
	req->status = -1;
    }
}

static void jack_engine_do_reserve_name( jack_engine_t & engine, jack_request_t *req )
{
    jack_reserved_name_t *reservation;

    auto nFinder = std::find_if( engine.clients.begin(),
				 engine.clients.end(),
				 [&req] ( jack_client_internal_t * client ) {
				     return !strcmp( (char*)client->control->name, req->x.reservename.name );
				 } );

    if( nFinder != engine.clients.end() ) {
	req->status = -1;
	return;
    }

    reservation = (jack_reserved_name_t*)malloc (sizeof (jack_reserved_name_t));
    if (reservation == NULL) {
	req->status = -1;
	return;
    }

    snprintf (reservation->name, sizeof (reservation->name), "%s", req->x.reservename.name);
    jack_uuid_copy (&reservation->uuid, req->x.reservename.uuid);
    engine.reserved_client_names.push_back( reservation );

    req->status = 0;
}

static int jack_engine_send_session_reply( jack_engine_t & engine, jack_client_internal_t *client )
{
    if (write (engine.session_reply_fd, (const void *) &client->control->uuid, sizeof (client->control->uuid))
	< (ssize_t) sizeof (client->control->uuid)) {
	jack_error ("cannot write SessionNotify result " 
		    "to client via fd = %d (%s)", 
		    engine.session_reply_fd, strerror (errno));
	return -1;
    }
    if (write (engine.session_reply_fd, (const void *) client->control->name, sizeof (client->control->name))
	< (ssize_t) sizeof (client->control->name)) {
	jack_error ("cannot write SessionNotify result "
		    "to client via fd = %d (%s)", 
		    engine.session_reply_fd, strerror (errno));
	return -1;
    }
    if (write (engine.session_reply_fd, (const void *) client->control->session_command, 
	       sizeof (client->control->session_command))
	< (ssize_t) sizeof (client->control->session_command)) {
	jack_error ("cannot write SessionNotify result "
		    "to client via fd = %d (%s)", 
		    engine.session_reply_fd, strerror (errno));
	return -1;
    }
    if (write (engine.session_reply_fd, (const void *) ( & client->control->session_flags ), 
	       sizeof (client->control->session_flags))
	< (ssize_t) sizeof (client->control->session_flags)) {
	jack_error ("cannot write SessionNotify result "
		    "to client via fd = %d (%s)", 
		    engine.session_reply_fd, strerror (errno));
	return -1;
    }

    return 0;
}

static int jack_engine_do_session_notify( jack_engine_t & engine, jack_request_t *req, int reply_fd )
{
    jack_event_t event;
  
    int reply;
    jack_uuid_t finalizer;
    struct stat sbuf;

    jack_uuid_clear (&finalizer);

    if (engine.session_reply_fd != -1) {
	// we should have a notion of busy or somthing.
	// just sending empty reply now.
	goto send_final;
    }

    engine.session_reply_fd = reply_fd;
    engine.session_pending_replies = 0;

    event.type = SaveSession;
    event.y.n = req->x.session.type;
 	
    /* GRAPH MUST BE LOCKED : see callers of jack_engine_send_connection_notification() 
     */

    if (stat (req->x.session.path, &sbuf) != 0 || !S_ISDIR (sbuf.st_mode)) {
	jack_error ("session parent directory (%s) does not exist", req->x.session.path);
	goto send_final;
    }

    for( jack_client_internal_t * client : engine.clients ) {
	if (client->control->session_cbset) {

	    // in case we only want to send to a special client.
	    // uuid assign is still complete. not sure if thats necessary.
	    if( (req->x.session.target[0] != 0) && strcmp(req->x.session.target, (char *)client->control->name) )
		continue;

	    /* the caller of jack_session_notify() is required to have created the session dir
	     */
                        
	    if (req->x.session.path[strlen(req->x.session.path)-1] == '/') {
		snprintf (event.x.name, sizeof (event.x.name), "%s%s/", req->x.session.path, client->control->name );
	    } else {
		snprintf (event.x.name, sizeof (event.x.name), "%s/%s/", req->x.session.path, client->control->name );
	    }
	    if (mkdir (event.x.name, 0777) != 0) {
		jack_error ("cannot create session directory (%s) for client %s: %s",
			    event.x.name, client->control->name, strerror (errno));
		break;
	    }
	    reply = jack_engine_deliver_event( engine, client, &event );

	    if (reply == 1) {
		// delayed reply
		engine.session_pending_replies += 1;
		client->session_reply_pending = TRUE;
	    } else if (reply == 2) {
		// immediate reply
		if (jack_engine_send_session_reply( engine, client ) )
		    goto error_out;
	    }
	} 
    }

    if (engine.session_pending_replies != 0)
	return 0;

  send_final:
    if (write (reply_fd, &finalizer, sizeof (finalizer))
	< (ssize_t) sizeof (finalizer)) {
	jack_error ("cannot write SessionNotify result "
		    "to client via fd = %d (%s)", 
		    reply_fd, strerror (errno));
	goto error_out;
    }

    engine.session_reply_fd = -1;
    return 0;
  error_out:
    return -3;
}

static int jack_engine_do_has_session_cb( jack_engine_t & engine, jack_request_t *req)
{
    jack_client_internal_t *client;
    int retval = -1;

    client = jack_engine_client_by_name( engine, req->x.name );
    if (client == NULL)
	goto out;

    retval = client->control->session_cbset ? 1 : 0;
  out:
    return retval;
}

static void jack_engine_do_session_reply( jack_engine_t & engine, jack_request_t *req )
{
    jack_uuid_t client_id;
    jack_client_internal_t *client;
    jack_uuid_t finalizer = JACK_UUID_EMPTY_INITIALIZER;

    jack_uuid_copy (&client_id, req->x.client_id);
    client = jack_engine_client_internal_by_id( engine, client_id);
    jack_uuid_clear (&finalizer);

    req->status = 0;

    client->session_reply_pending = 0;

    if (engine.session_reply_fd == -1) {
	jack_error ("spurious Session Reply");
	return;
    }

    engine.session_pending_replies -= 1;

    if( jack_engine_send_session_reply( engine, client ) ) {
	// maybe need to fix all client pendings.
	// but we will just get a set of spurious replies now.
	engine.session_reply_fd = -1;
	return;
    }

    if( engine.session_pending_replies == 0 ) {
	if (write (engine.session_reply_fd, &finalizer, sizeof (finalizer))
	    < (ssize_t) sizeof (finalizer)) {
	    jack_error ("cannot write SessionNotify result "
			"to client via fd = %d (%s)", 
			engine.session_reply_fd, strerror (errno));
	    req->status = -1;
	}
	engine.session_reply_fd = -1;
    }
}

int jack_engine_stop_freewheeling( jack_engine_t & engine, int engine_exiting )
{
    jack_event_t event;
    void *ftstatus;

    if (!engine.freewheeling) {
	return 0;
    }

    if (engine.driver == NULL) {
	jack_error ("cannot start freewheeling without a driver!");
	return -1;
    }

    if (!engine.freewheeling) {
	VERBOSE( &engine, "stop freewheel when not freewheeling" );
	return 0;
    }

    /* tell the freewheel thread to stop, and wait for it
       to exit.
    */

    engine.stop_freewheeling = 1;

    VERBOSE( &engine, "freewheeling stopped, waiting for thread");
    pthread_join(engine.freewheel_thread, &ftstatus);
    VERBOSE( &engine, "freewheel thread has returned");

    jack_uuid_clear( &engine.fwclient );
    engine.freewheeling = 0;
    engine.control->frame_timer.reset_pending = 1;

    if (!engine_exiting) {
	/* tell everyone we've stopped */
		
	event.type = StopFreewheel;
	jack_engine_deliver_event_to_all( engine, &event );
		
	/* restart the driver */
		
	if (jack_engine_drivers_start( engine )) {
	    jack_error ("could not restart driver after freewheeling");
	    return -1;
	}
    }

    return 0;
}

/* perform internal or external client request
 *
 * reply_fd is NULL for internal requests
 */
static void jack_engine_do_request( jack_engine_t & engine, jack_request_t *req, int *reply_fd)
{
    /* The request_lock serializes internal requests (from any
     * thread in the server) with external requests (always from "the"
     * server thread). 
     */
    pthread_mutex_lock (&engine.request_lock);

    DEBUG ("got a request of type %d (%s)", req->type, jack_event_type_name ((JackEventType)req->type));

    switch (req->type) {
	case RegisterPort:
	    req->status = jack_engine_port_do_register( engine, req, reply_fd ? FALSE : TRUE);
	    break;

	case UnRegisterPort:
	    req->status = jack_engine_port_do_unregister( engine, req);
	    break;

	case ConnectPorts:
	    req->status = jack_engine_port_do_connect( engine, req->x.connect.source_port,
		 req->x.connect.destination_port);
	    break;

	case DisconnectPort:
	    req->status = jack_engine_port_do_disconnect_all( engine, req->x.port_info.port_id);
	    break;

	case DisconnectPorts:
	    req->status = jack_engine_port_do_disconnect( engine, req->x.connect.source_port,
		 req->x.connect.destination_port);
	    break;

	case ActivateClient:
	    req->status = jack_engine_client_activate( engine, req->x.client_id);
	    break;

	case DeactivateClient:
	    req->status = jack_engine_client_deactivate( engine, req->x.client_id);
	    break;

	case SetTimeBaseClient:
	    req->status = jack_timebase_set( engine,
					     req->x.timebase.client_id,
					     req->x.timebase.conditional);
	    break;

	case ResetTimeBaseClient:
	    req->status = jack_timebase_reset( engine, req->x.client_id);
	    break;

	case SetSyncClient:
	    req->status = jack_transport_client_set_sync( engine, req->x.client_id);
	    break;

	case ResetSyncClient:
	    req->status = jack_transport_client_reset_sync( engine, req->x.client_id);
	    break;

	case SetSyncTimeout:
	    req->status = jack_transport_set_sync_timeout( engine, req->x.timeout);
	    break;

	case GetPortConnections:
	case GetPortNConnections:
	    //JOQ bug: reply_fd may be NULL if internal request
	    if ((req->status = jack_engine_do_get_port_connections( engine, req, *reply_fd))
		== 0) {
		/* we have already replied, don't do it again */
		*reply_fd = -1;
	    }
	    break;

	case FreeWheel:
	    req->status = jack_engine_start_freewheeling( engine, req->x.client_id);
	    break;

	case StopFreeWheel:
	    req->status = jack_engine_stop_freewheeling( engine, 0);
	    break;

	case SetBufferSize:
	    req->status = jack_engine_set_buffer_size_request(
		engine, req->x.nframes);
	    jack_lock_graph( (&engine) );
	    jack_engine_compute_new_latency( engine );
	    jack_unlock_graph( (&engine) );
	    break;

	case IntClientHandle:
	    jack_engine_intclient_handle_request( engine, req);
	    break;

	case IntClientLoad:
	    jack_engine_intclient_load_request( engine, req);
	    break;

	case IntClientName:
	    jack_engine_intclient_name_request( engine, req);
	    break;

	case IntClientUnload:
	    jack_engine_intclient_unload_request( engine, req);
	    break;

	case RecomputeTotalLatencies:
	    jack_lock_graph( (&engine) );
	    jack_engine_compute_all_port_total_latencies( engine );
	    jack_engine_compute_new_latency( engine );
	    jack_unlock_graph( (&engine) );
	    req->status = 0;
	    break;

	case RecomputeTotalLatency:
	    jack_lock_graph( (&engine) );
	    jack_engine_compute_port_total_latency( engine, &engine.control->ports[req->x.port_info.port_id]);
	    jack_unlock_graph( (&engine) );
	    req->status = 0;
	    break;

	case GetClientByUUID:
	    jack_rdlock_graph( (&engine) );
	    jack_engine_client_fill_request_port_name_by_uuid( engine, req );
	    jack_unlock_graph( (&engine) );
	    break;
	case GetUUIDByClientName:
	    jack_rdlock_graph( (&engine));
	    jack_engine_do_get_uuid_by_client_name( engine, req );
	    jack_unlock_graph( (&engine) );
	    break;
	case ReserveName:
	    jack_rdlock_graph( (&engine) );
	    jack_engine_do_reserve_name( engine, req );
	    jack_unlock_graph( (&engine) );
	    break;
	case SessionReply:
	    jack_rdlock_graph( (&engine) );
	    jack_engine_do_session_reply( engine, req );
	    jack_unlock_graph( (&engine) );
	    break;
	case SessionNotify:
	    jack_rdlock_graph( (&engine) );
	    if ((req->status =
		 jack_engine_do_session_notify( engine, req, *reply_fd))
		>= 0) {
		/* we have already replied, don't do it again */
		*reply_fd = -1;
	    }
	    jack_unlock_graph( (&engine) );
	    break;
	case SessionHasCallback:
	    jack_rdlock_graph( (&engine) );
	    req->status = jack_engine_do_has_session_cb( engine, req );
	    jack_unlock_graph( (&engine) );
	    break;
        case PropertyChangeNotify:
	    jack_engine_property_change_notify( engine, req->x.property.change, req->x.property.uuid, req->x.property.key);
	    break;

	default:
	    /* some requests are handled entirely on the client
	     * side, by adjusting the shared memory area(s) */
	    break;
    }
        
    pthread_mutex_unlock (&engine.request_lock);

    DEBUG ("status of request: %d", req->status);
}

int internal_client_request( void * ptr, jack_request_t * request )
{
    jack_engine_do_request( *((jack_engine_t*)ptr), request, NULL);
    return request->status;
}

static int handle_external_client_request( jack_engine_t * engine, int fd )
{
    /* CALLER holds read lock on graph */

    jack_request_t req;
    jack_client_internal_t *client = 0;
    int reply_fd;
    ssize_t r;

    auto cFinder = std::find_if( engine->clients.begin(),
				 engine->clients.end(),
				 [&fd] ( jack_client_internal_t * client ) {
				     return client->request_fd == fd;
				 } );

    if( cFinder == engine->clients.end() ) {
	jack_error( "Client input on unknown fd %d!", fd );
	return -1;
    }

    client = *cFinder;

    if ((r = read (client->request_fd, &req, sizeof (req)))
	< (ssize_t) sizeof (req)) {
	if (r == 0) {
	    return 1;
	} else {
	    jack_error ("cannot read request from client (%d/%d/%s)",
			r, sizeof(req), strerror (errno));
	    // XXX: shouldnt we mark this client as error now ?

	    return -1;
	}
    }

    if (req.type == PropertyChangeNotify) {
	if (req.x.property.keylen) {
	    req.x.property.key = (char*) malloc (req.x.property.keylen);
	    if ((r = read (client->request_fd, (char*) req.x.property.key, req.x.property.keylen)) != (ssize_t)req.x.property.keylen) {
		jack_error ("cannot read property key from client (%d/%d/%s)",
			    r, sizeof(req), strerror (errno));
		return -1;
	    }
	} else {
	    req.x.property.key = 0;
	}
    }

    reply_fd = client->request_fd;
	
    jack_unlock_graph( engine );
    jack_engine_do_request( *engine, &req, &reply_fd );
    jack_lock_graph( engine );

    if (req.type == PropertyChangeNotify && req.x.property.key) {
	free ((char *) req.x.property.key);
    }

    if (reply_fd >= 0) {
	DEBUG ("replying to client");
	if (write (reply_fd, &req, sizeof (req))
	    < (ssize_t) sizeof (req)) {
	    jack_error ("cannot write request result to client");
	    return -1;
	}
    } else {
	DEBUG ("*not* replying to client");
    }

    return 0;
}

static int handle_client_ack_connection (jack_engine_t *engine, int client_fd)
{
    jack_client_internal_t *client;
    jack_client_connect_ack_request_t req;
    jack_client_connect_ack_result_t res;

    if (read (client_fd, &req, sizeof (req)) != sizeof (req)) {
	jack_error ("cannot read ACK connection request from client");
	return -1;
    }

    if ((client = jack_engine_client_internal_by_id( *engine, req.client_id))
	== NULL) {
	jack_error ("unknown client ID in ACK connection request");
	return -1;
    }

    client->event_fd = client_fd;
    VERBOSE (engine, "new client %s using %d for events", client->control->name,
	     client->event_fd);

    res.status = 0;

    if (write (client->event_fd, &res, sizeof (res)) != sizeof (res)) {
	jack_error ("cannot write ACK connection response to client");
	return -1;
    }

    return 0;
}

static void * jack_server_thread( void *arg )
{
    jack_engine_t *engine = (jack_engine_t *)arg;
    struct sockaddr_un client_addr;
    socklen_t client_addrlen;
    int problemsProblemsPROBLEMS = 0;
    int client_socket;
    int done = 0;
    size_t i;
    const int fixed_fd_cnt = 3;
    int stop_freewheeling;

    while (!done) {
	size_t clients;

	jack_rdlock_graph (engine);

	clients = engine->clients.size();

	if( engine->pfd_size < fixed_fd_cnt + clients ) {
	    if (engine->pfd) {
		free (engine->pfd);
	    }
                        
	    engine->pfd = (struct pollfd *)malloc(sizeof(struct pollfd) * (fixed_fd_cnt + clients));
				
	    if (engine->pfd == NULL) {
		/*
		 * this can happen if limits.conf was changed
		 * but the user hasn't logged out and back in yet
		 */
		if (errno == EAGAIN)
		    jack_error("malloc failed (%s) - make" 
			       "sure you log out and back"
			       "in after changing limits"
			       ".conf!", strerror(errno));
		else
		    jack_error("malloc failed (%s)",
			       strerror(errno));

		break;
	    }
	}

	engine->pfd[0].fd = engine->fds[0];
	engine->pfd[0].events = POLLIN|POLLERR;
	engine->pfd[1].fd = engine->fds[1];
	engine->pfd[1].events = POLLIN|POLLERR;
	engine->pfd[2].fd = engine->cleanup_fifo[0];
	engine->pfd[2].events = POLLIN|POLLERR;
	engine->pfd_max = fixed_fd_cnt;
		
	for( jack_client_internal_t * client : engine->clients ) {

	    if (client->request_fd < 0 || client->error >= JACK_ERROR_WITH_SOCKETS) {
		continue;
	    }
	    if( client->control->dead ) {
		engine->pfd[engine->pfd_max].fd = client->request_fd;
		engine->pfd[engine->pfd_max].events = POLLHUP|POLLNVAL;
		engine->pfd_max++;
		continue;
	    }
	    engine->pfd[engine->pfd_max].fd = client->request_fd;
	    engine->pfd[engine->pfd_max].events = POLLIN|POLLPRI|POLLERR|POLLHUP|POLLNVAL;
	    engine->pfd_max++;
	}

	jack_unlock_graph (engine);
		
	VERBOSE (engine, "start poll on %d fd's", engine->pfd_max);
		
	/* go to sleep for a long, long time, or until a request
	   arrives, or until a communication channel is broken
	*/

	if (poll (engine->pfd, engine->pfd_max, -1) < 0) {
	    if (errno == EINTR) {
		continue;
	    }
	    jack_error ("poll failed (%s)", strerror (errno));
	    break;
	}
		
	VERBOSE(engine, "server thread back from poll");
		
	/* Stephane Letz: letz@grame.fr : has to be added
	 * otherwise pthread_cancel() does not work on MacOSX */
	pthread_testcancel();


	/* empty cleanup FIFO if necessary */

	if (engine->pfd[2].revents & ~POLLIN) {
	    /* time to die */
	    break;
	}

	if (engine->pfd[2].revents & POLLIN) {
	    char c;
	    while (read (engine->cleanup_fifo[0], &c, 1) == 1);
	}

	/* check each client socket before handling other request*/
		
	jack_rdlock_graph (engine);

	for (i = fixed_fd_cnt; i < engine->pfd_max; i++) {

	    if (engine->pfd[i].fd < 0) {
		continue;
	    }

	    if (engine->pfd[i].revents & ~POLLIN) {
                                
		jack_engine_mark_client_socket_error( *engine, engine->pfd[i].fd );
		jack_engine_signal_problems( *engine );
		VERBOSE (engine, "non-POLLIN events on fd %d", engine->pfd[i].fd);
	    } else if (engine->pfd[i].revents & POLLIN) {

		if (handle_external_client_request (engine, engine->pfd[i].fd)) {
		    jack_error ("could not handle external"
				" client request");
		    jack_engine_signal_problems( *engine );
		}
	    }
	}

	problemsProblemsPROBLEMS = engine->problems;

	jack_unlock_graph (engine);

	/* need to take write lock since we may/will rip out some clients,
	   and reset engine->problems
	*/

	stop_freewheeling = 0;

	while (problemsProblemsPROBLEMS) {
			
	    VERBOSE (engine, "trying to lock graph to remove %d problems", problemsProblemsPROBLEMS);
	    jack_lock_graph (engine);
	    VERBOSE (engine, "we have problem clients (problems = %d", problemsProblemsPROBLEMS);
	    jack_engine_remove_clients( *engine, &stop_freewheeling);
	    if (stop_freewheeling) {
		VERBOSE (engine, "need to stop freewheeling once problems are cleared");
	    }
	    jack_unlock_graph (engine);

	    jack_lock_problems (engine);
	    engine->problems -= problemsProblemsPROBLEMS;
	    problemsProblemsPROBLEMS = engine->problems;
	    jack_unlock_problems (engine);

	    VERBOSE (engine, "after removing clients, problems = %d", problemsProblemsPROBLEMS);
	}
		
	if (engine->freewheeling && stop_freewheeling) {
	    jack_engine_stop_freewheeling( *engine, 0 );
	}
			
	/* check the master server socket */

	if (engine->pfd[0].revents & POLLERR) {
	    jack_error ("error on server socket");
	    break;
	}
	
	if (engine->control->engine_ok && engine->pfd[0].revents & POLLIN) {
	    DEBUG ("pfd[0].revents & POLLIN");

	    memset (&client_addr, 0, sizeof (client_addr));
	    client_addrlen = sizeof (client_addr);

	    if ((client_socket =
		 accept (engine->fds[0],
			 (struct sockaddr *) &client_addr,
			 &client_addrlen)) < 0) {
		jack_error ("cannot accept new connection (%s)",
			    strerror (errno));
	    } else if (!engine->new_clients_allowed || jack_engine_client_create( *engine, client_socket) < 0) {
		jack_error ("cannot complete client "
			    "connection process");
		close (client_socket);
	    }
	}
		
	/* check the ACK server socket */

	if (engine->pfd[1].revents & POLLERR) {
	    jack_error ("error on server ACK socket");
	    break;
	}

	if (engine->control->engine_ok && engine->pfd[1].revents & POLLIN) {
	    DEBUG ("pfd[1].revents & POLLIN");

	    memset (&client_addr, 0, sizeof (client_addr));
	    client_addrlen = sizeof (client_addr);

	    if ((client_socket =
		 accept (engine->fds[1],
			 (struct sockaddr *) &client_addr,
			 &client_addrlen)) < 0) {
		jack_error ("cannot accept new ACK connection"
			    " (%s)", strerror (errno));
	    } else if (handle_client_ack_connection
		       (engine, client_socket)) {
		jack_error ("cannot complete client ACK "
			    "connection process");
		close (client_socket);
	    }
	}
    }

    return 0;
}

/* driver callback */
int jack_engine_set_sample_rate( jack_engine_t * engine, jack_nframes_t nframes )
{
    jack_control_t *ectl = engine->control;

    ectl->current_time.frame_rate = nframes;
    ectl->pending_time.frame_rate = nframes;

    return 0;
}

unique_ptr<jack_engine_t> jack_engine_create(
    const jack::jack_options & parsed_options,
    pid_t wait_pid,
    const std::vector<jack_driver_desc_t*> & loaded_drivers )
{
    std::unique_ptr<jack_engine_t> engine = make_unique<jack_engine_t>();
    unsigned int i;
    char server_dir[PATH_MAX+1] = "";

    /* before we start allocating resources, make sure that if realtime was requested that we can 
       actually do it.
    */

    if( parsed_options.realtime ) {
	if (jack_acquire_real_time_scheduling (pthread_self(), 10) != 0) {
	    /* can't run realtime - time to bomb */
	    return NULL;
	}

	jack_drop_real_time_scheduling (pthread_self());

#ifdef USE_MLOCK

	if( parsed_options.memory_locked && (mlockall (MCL_CURRENT | MCL_FUTURE) != 0)) {
	    jack_error ("cannot lock down memory for jackd (%s)",
			strerror (errno));
#ifdef ENSURE_MLOCK
	    return NULL;
#endif /* ENSURE_MLOCK */
	}
#endif /* USE_MLOCK */
    }

    /* start a thread to display messages from realtime threads */
    jack_messagebuffer_init();

    jack_init_time ();

    engine->drivers = loaded_drivers;

    engine->driver = NULL;
    engine->driver_desc = NULL;
    engine->driver_params = NULL;

    engine->set_sample_rate = jack_engine_set_sample_rate;
    engine->set_buffer_size = jack_engine_driver_buffer_size;
    engine->run_cycle = jack_engine_run_cycle;
    engine->delay = jack_engine_delay;
    engine->driver_exit = jack_engine_driver_exit;
    engine->transport_cycle_start = jack_transport_cycle_start;
    engine->client_timeout_msecs = parsed_options.client_timeout;
    engine->timeout_count = 0;
    engine->problems = 0;

    engine->port_max = parsed_options.port_max;
    engine->server_thread = 0;
    engine->rtpriority = parsed_options.realtime_priority;
    engine->silent_buffer = 0;
    engine->verbose = parsed_options.verbose;
    engine->server_name = parsed_options.server_name.c_str();
    engine->temporary = parsed_options.temporary;
    engine->freewheeling = 0;
    engine->stop_freewheeling = 0;
    jack_uuid_clear( &engine->fwclient );
    engine->feedbackcount = 0;
    engine->wait_pid = wait_pid;
    engine->nozombies = parsed_options.no_zombies;
    engine->timeout_count_threshold = parsed_options.timeout_threshold;
    engine->removing_clients = 0;
    engine->new_clients_allowed = 1;

    engine->session_reply_fd = -1;
    engine->session_pending_replies = 0;

    engine->audio_out_cnt = 0;
    engine->audio_in_cnt = 0;
    engine->midi_out_cnt = 0;
    engine->midi_in_cnt = 0;

    jack_engine_reset_rolling_usecs( *engine );
    engine->max_usecs = 0.0f;

    pthread_rwlock_init (&engine->client_lock, 0);
    pthread_mutex_init (&engine->port_lock, 0);
    pthread_mutex_init (&engine->request_lock, 0);
    pthread_mutex_init (&engine->problem_lock, 0);

    engine->clients.clear();
    engine->reserved_client_names.clear();

    engine->pfd_size = 0;
    engine->pfd_max = 0;
    engine->pfd = 0;

    engine->fifo_size = 16;
    engine->fifo = (int *) malloc (sizeof (int) * engine->fifo_size);
    for (i = 0; i < engine->fifo_size; i++) {
	engine->fifo[i] = -1;
    }

    if (pipe (engine->cleanup_fifo)) {
	jack_error ("cannot create cleanup FIFOs (%s)", strerror (errno));
	return NULL;
    }

    if (fcntl (engine->cleanup_fifo[0], F_SETFL, O_NONBLOCK)) {
	jack_error ("cannot set O_NONBLOCK on cleanup read FIFO (%s)", strerror (errno));
	return NULL;
    }

    if (fcntl (engine->cleanup_fifo[1], F_SETFL, O_NONBLOCK)) {
	jack_error ("cannot set O_NONBLOCK on cleanup write FIFO (%s)", strerror (errno));
	return NULL;
    }

    engine->external_client_cnt = 0;

    srandom (time ((time_t *) 0));

    if (jack_shmalloc (sizeof (jack_control_t)
		       + ((sizeof (jack_port_shared_t) * engine->port_max)),
		       &engine->control_shm)) {
	jack_error ("cannot create engine control shared memory "
		    "segment (%s)", strerror (errno));
	return NULL;
    }

    if (jack_attach_shm (&engine->control_shm)) {
	jack_error ("cannot attach to engine control shared memory"
		    " (%s)", strerror (errno));
	jack_destroy_shm (&engine->control_shm);
	return NULL;
    }

    engine->control = (jack_control_t *)
	jack_shm_addr (&engine->control_shm);

    /* Setup port type information from builtins. buffer space is
     * allocated when the driver calls jack_driver_buffer_size().
     */
    for( i = 0 ; i < jack_builtin_port_types.size() ; ++i ) {
	jack_port_type_info_t & bipt = jack_builtin_port_types[i];

	memcpy (&engine->control->port_types[i],
		&bipt,
		sizeof (jack_port_type_info_t));

	VERBOSE (engine, "registered builtin port type %s",
		 engine->control->port_types[i].type_name);

	/* the port type id is index into port_types array */
	engine->control->port_types[i].ptype_id = i;

	/* be sure to initialize mutex correctly */
	pthread_mutex_init (&engine->port_buffers[i].lock, NULL);

	/* set buffer list info correctly */
	engine->port_buffers[i].freelist_vector.clear();
	engine->port_buffers[i].info = NULL;

	/* mark each port segment as not allocated */
	engine->port_segment[i].index = -1;
	engine->port_segment[i].attached_at = 0;
    }

    engine->control->n_port_types = i;

    /* Mark all ports as available */

    for (i = 0; i < engine->port_max; i++) {
	engine->control->ports[i].in_use = 0;
	engine->control->ports[i].id = i;
	engine->control->ports[i].alias1[0] = '\0';
	engine->control->ports[i].alias2[0] = '\0';
    }

    /* allocate internal port structures so that we can keep track
     * of port connections.
     */
    engine->internal_ports.resize( engine->port_max );

    for (i = 0; i < engine->port_max; i++) {
	engine->internal_ports[i].connections_vector.clear();
    }

    if (make_sockets (engine->server_name, engine->fds) < 0) {
	jack_error ("cannot create server sockets");
	return NULL;
    }

    engine->control->port_max = engine->port_max;
    engine->control->real_time = parsed_options.realtime;

    /* leave some headroom for other client threads to run
       with priority higher than the regular client threads
       but less than the server. see thread.h for
       jack_client_real_time_priority() and jack_client_max_real_time_priority()
       which are affected by this.
    */

    engine->control->client_priority = (parsed_options.realtime
					? engine->rtpriority - 5
					: 0);
    engine->control->max_client_priority = (parsed_options.realtime
					    ? engine->rtpriority - 1
					    : 0);
    engine->control->do_mlock = parsed_options.memory_locked;
    engine->control->do_munlock = parsed_options.unlock_gui_memory;
    engine->control->cpu_load = 0;
    engine->control->xrun_delayed_usecs = 0;
    engine->control->max_delayed_usecs = 0;

    jack_set_clock_source( clock_source );
    engine->control->clock_source = clock_source;
    engine->get_microseconds = jack_get_microseconds_pointer();

    VERBOSE( engine.get(), "clock source = %s", jack_clock_source_name (clock_source) );

    engine->control->frame_timer.frames = parsed_options.frame_time_offset;
    engine->control->frame_timer.reset_pending = 0;
    engine->control->frame_timer.current_wakeup = 0;
    engine->control->frame_timer.next_wakeup = 0;
    engine->control->frame_timer.initialized = 0;
    engine->control->frame_timer.filter_omega = 0; /* Initialised later */
    engine->control->frame_timer.period_usecs = 0; /* Initialised later */

    engine->first_wakeup = 1;

    engine->control->buffer_size = 0;
    jack_transport_init( *engine );
    jack_engine_set_sample_rate( engine.get(), 0 );
    engine->control->internal = 0;

    engine->control->has_capabilities = 0;

    engine->control->engine_ok = 1;

    snprintf( engine->fifo_prefix, sizeof (engine->fifo_prefix),
	      "%s/jack-ack-fifo-%d",
	      jack_server_dir (engine->server_name, server_dir), getpid ());

    (void) jack_engine_get_fifo_fd( *engine, 0);

    jack_client_create_thread( NULL, &engine->server_thread, 0, FALSE,
			       &jack_server_thread, engine.get());

    return engine;
}

void jack_engine_cleanup( jack_engine_t & engine )
{
    VERBOSE( &engine, "starting server engine shutdown" );

    jack_engine_stop_freewheeling( engine, 1 );

    engine.control->engine_ok = 0;	/* tell clients we're going away */

    /* this will wake the server thread and cause it to exit */

    close( engine.cleanup_fifo[0]);
    close( engine.cleanup_fifo[1]);

    /* shutdown master socket to prevent new clients arriving */
    shutdown( engine.fds[0], SHUT_RDWR);
    // close (engine->fds[0]);

    /* now really tell them we're going away */

    for ( size_t i = 0; i < engine.pfd_max; ++i) {
	shutdown( engine.pfd[i].fd, SHUT_RDWR );
    }

    if( engine.driver ) {
	jack_driver_t* driver = engine.driver;

	VERBOSE( &engine, "stopping driver");
	driver->stop( driver );
	// VERBOSE (engine, "detaching driver");
	// driver->detach (driver, engine);
	VERBOSE( &engine, "unloading driver");
	jack_driver_unload( driver );
	engine.driver = NULL;
    }

    if( engine.slave_drivers.size() > 0 ) {
	VERBOSE( &engine, "unloading slave drivers");
	for( jack_driver_t * slave_driver : engine.slave_drivers ) {
	    jack_engine_unload_slave_driver( engine, slave_driver );
	    free( slave_driver );
	}
    }

    VERBOSE( &engine, "freeing shared port segments");
    for ( ssize_t i = 0; i < engine.control->n_port_types; ++i) {
	jack_release_shm( &engine.port_segment[i] );
	jack_destroy_shm( &engine.port_segment[i] );
    }

    /* stop the other engine threads */
    VERBOSE( &engine, "stopping server thread");

    pthread_cancel( engine.server_thread );
    pthread_join( engine.server_thread, NULL );

    VERBOSE( &engine, "last xrun delay: %.3f usecs",
	     engine.control->xrun_delayed_usecs);
    VERBOSE( &engine, "max delay reported by backend: %.3f usecs",
	     engine.control->max_delayed_usecs);

    /* free engine control shm segment */
    engine.control = NULL;
    VERBOSE( &engine, "freeing engine shared memory");
    jack_release_shm( &engine.control_shm);
    jack_destroy_shm( &engine.control_shm);

    VERBOSE( &engine, "max usecs: %.3f, engine deleted", engine.max_usecs);

    jack_messagebuffer_exit();

    if( engine.fifo ) {
	free( engine.fifo );
	engine.fifo = NULL;
    }
}

static jack_driver_info_t * jack_engine_load_driver_so( jack_engine_t & engine, jack_driver_desc_t * driver_desc )
{
    const char *errstr;
    jack_driver_info_t *info;

    info = (jack_driver_info_t *) calloc (1, sizeof (*info));

    info->handle = dlopen( driver_desc->file, RTLD_NOW|RTLD_GLOBAL );
	
    if (info->handle == NULL) {
	if ((errstr = dlerror ()) != 0) {
	    jack_error ("can't load \"%s\": %s", driver_desc->file,
			errstr);
	} else {
	    jack_error ("bizarre error loading driver shared "
			"object %s", driver_desc->file);
	}
	goto fail;
    }

    info->initialize = (jack_driver_info_init_callback_t)dlsym(info->handle, "driver_initialize");

    if ((errstr = dlerror()) != 0) {
	jack_error ("no initialize function in shared object %s\n",
		    driver_desc->file);
	goto fail;
    }

    info->finish = (jack_driver_info_finish_callback_t)dlsym(info->handle, "driver_finish");

    if ((errstr = dlerror()) != 0) {
	jack_error ("no finish function in in shared driver object %s",
		    driver_desc->file);
	goto fail;
    }

    info->client_name = (char *)dlsym(info->handle, "driver_client_name");

    if ((errstr = dlerror()) != 0) {
	jack_error ("no client name in in shared driver object %s",
		    driver_desc->file);
	goto fail;
    }

    return info;

  fail:
    if (info->handle) {
	dlclose (info->handle);
    }
    free (info);
    return NULL;
	
}

void jack_driver_unload (jack_driver_t *driver)
{
    void* handle = driver->handle;
    driver->finish (driver);
    dlclose (handle);
}

int jack_engine_load_driver( jack_engine_t & engine,
			     jack_driver_desc_t * driver_desc,
			     JSList * driver_params_jsl )
{
    jack_client_internal_t *client;
    jack_driver_t *driver;
    jack_driver_info_t *info;

    if ((info = jack_engine_load_driver_so( engine, driver_desc )) == NULL) {
	return -1;
    }

    if ((client = jack_engine_create_driver_client_internal( engine, info->client_name )
	    ) == NULL) {
	return -1;
    }

    if ((driver = info->initialize( client->private_client,
				    driver_params_jsl)) == NULL) {
	free (info);
	return -1;
    }

    driver->handle = info->handle;
    driver->finish = info->finish;
    driver->internal_client = client;
    free (info);

    if (jack_engine_use_driver( engine, driver ) < 0) {
	jack_engine_remove_client_internal( engine, client );
	return -1;
    }

    engine.driver_desc   = driver_desc;
    engine.driver_params = driver_params_jsl;

    return 0;
}

int jack_engine_add_slave_driver( jack_engine_t & engine, jack_driver_t *driver )
{
    if (driver) {
	if (driver->attach (driver, &engine)) {
	    jack_info ("could not attach slave %s\n", driver->internal_client->control->name);
	    return -1;
	}

	engine.slave_drivers.push_back( driver );
    }

    return 0;
}

int jack_engine_load_slave_driver (jack_engine_t & engine,
				   jack_driver_desc_t * driver_desc,
				   JSList * driver_params)
{
    jack_client_internal_t *client;
    jack_driver_t *driver;
    jack_driver_info_t *info;

    if ((info = jack_engine_load_driver_so( engine, driver_desc )) == NULL) {
	jack_info ("Loading slave failed\n");
	return -1;
    }

    if ((client = jack_engine_create_driver_client_internal( engine, info->client_name ) ) == NULL) {
	jack_info ("Creating slave failed\n");
	return -1;
    }

    if ((driver = info->initialize( client->private_client,
				    driver_params )) == NULL) {
	free (info);
	jack_info ("Initializing slave failed\n");
	return -1;
    }

    driver->handle = info->handle;
    driver->finish = info->finish;
    driver->internal_client = client;
    free (info);

    if( jack_engine_add_slave_driver( engine, driver ) < 0 ) {
	jack_info ("Adding slave failed\n");
	jack_engine_client_internal_delete( engine, client);
	return -1;
    }

    //engine->driver_desc   = driver_desc;
    //engine->driver_params = driver_params;

    return 0;
}

static void jack_engine_slave_driver_remove( jack_engine_t & engine, jack_driver_t * sdriver )
{
    sdriver->detach( sdriver, &engine );
    auto sdFinder = std::find(engine.slave_drivers.begin(), engine.slave_drivers.end(), sdriver );
    if( sdFinder != engine.slave_drivers.end() ) {
	engine.slave_drivers.erase( sdFinder );
    }

    jack_driver_unload(sdriver);
}

int jack_engine_unload_slave_driver( jack_engine_t & engine,
				     jack_driver_t * driver_desc )
{
    jack_engine_slave_driver_remove( engine, driver_desc );
    jack_engine_client_internal_delete( engine, driver_desc->internal_client );
    return 0;
}

int jack_engine_drivers_start( jack_engine_t & engine )
{
    vector<jack_driver_t*> failed_drivers;
    /* first start the slave drivers */
    for( jack_driver_t * sdriver : engine.slave_drivers ) {
	if( sdriver->start( sdriver ) ) {
	    failed_drivers.push_back( sdriver );
	}
    }
    for( jack_driver_t * sdriver : failed_drivers ) {
	jack_error( "slave driver %s failed to start, removing it", sdriver->internal_client->control->name );
	jack_engine_slave_driver_remove( engine, sdriver);
    }
    /* now the master driver is started */
    return engine.driver->start( engine.driver );
}

int jack_engine_use_driver( jack_engine_t & engine, jack_driver_t *driver )
{
    if (engine.driver) {
	engine.driver->detach( engine.driver, &engine );
	engine.driver = 0;
    }

    if (driver) {
	engine.driver = driver;

	if( driver->attach( driver, &engine)) {
	    engine.driver = 0;
	    return -1;
	}

	engine.rolling_interval =
	    jack_rolling_interval( driver->period_usecs );
    }

    return 0;
}

static void jack_wake_server_thread (jack_engine_t* engine)
{
    char c = 0;
    /* we don't actually care if this fails */
    VERBOSE (engine, "waking server thread");
    write (engine->cleanup_fifo[1], &c, 1);
}

void jack_engine_signal_problems( jack_engine_t & engine )
{
    jack_lock_problems( (&engine) );
    engine.problems++;
    jack_unlock_problems( (&engine) );
    jack_wake_server_thread( &engine );
}

jack_client_internal_t * jack_engine_client_by_name( jack_engine_t & engine, const char *name )
{
    jack_client_internal_t *client = NULL;

    jack_rdlock_graph( (&engine) );

    auto cFinder = std::find_if( engine.clients.begin(),
				 engine.clients.end(),
				 [&name] ( jack_client_internal_t * client ) {
				     return strcmp( (const char *)client->control->name,
						    name ) == 0;
				 } );

    if( cFinder != engine.clients.end() ) {
	client = *cFinder;
    }

    jack_unlock_graph( (&engine) );
    return client;
}

jack_client_internal_t * jack_engine_client_internal_by_id( jack_engine_t & engine, jack_uuid_t id )
{
    jack_client_internal_t * client = NULL;

    /* call tree ***MUST HOLD*** the graph lock */

    auto cFinder = std::find_if( engine.clients.begin(),
				 engine.clients.end(),
				 [&id] ( jack_client_internal_t * client ) {
				     return jack_uuid_compare( client->control->uuid, id) == 0;
				 } );

    if( cFinder != engine.clients.end() ) {
	client = *cFinder;
    }

    return client;
}
