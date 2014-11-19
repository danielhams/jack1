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
#include "jack_utils.hpp"
#include "jack_constants.hpp"

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
#include "version.hpp"

#include "libjack/local.hpp"

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

using jack::server_tmp_dir;
using jack::server_user_dir;
using jack::server_dir;

struct pollfd tester;

jack_timer_type_t clock_source = JACK_TIMER_SYSTEM_CLOCK;

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

static int make_socket_subdirectories( const string & server_name )
{
    struct stat statbuf;

    string server_tmp_dir_str = server_tmp_dir();

    /* check tmpdir directory */
    if( stat( server_tmp_dir_str.c_str(), &statbuf )) {
	jack_error ("cannot stat() %s (%s)\n",
		    server_tmp_dir_str.c_str(), strerror (errno));
	return -1;
    } else {
	if (!S_ISDIR(statbuf.st_mode)) {
	    jack_error ("%s exists, but is not a directory!\n",
			server_tmp_dir_str.c_str());
	    return -1;
	}
    }

    /* create user subdirectory */
    const string & user_dir_str = server_user_dir();
    if( make_directory( user_dir_str.c_str() ) < 0 ) {
	return -1;
    }

    /* create server_name subdirectory */
    const string & server_dir_str = server_dir( server_name );
    if( make_directory( server_dir_str.c_str() ) < 0) {
	return -1;
    }

    return 0;
}

static int make_sockets( const string & server_name, int fd[2] )
{
    struct sockaddr_un addr;
    int i;

    if( make_socket_subdirectories( server_name ) < 0) {
	return -1;
    }

    /* First, the master server socket */

    if( (fd[0] = socket( AF_UNIX, SOCK_STREAM, 0 ) ) < 0) {
	jack_error( "cannot create server socket (%s)",
		    strerror (errno) );
	return -1;
    }

    addr.sun_family = AF_UNIX;
    for( i = 0; i < 999; i++ ) {
	stringstream ss( stringstream::out );
	ss << server_dir( server_name ) << "/jack_" << i;

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
	ss << server_dir( server_name ) << "/jack_ack_" << i;

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

static inline int jack_rolling_interval( jack_time_t period_usecs )
{
    return floor ((JACK_ENGINE_ROLLING_INTERVAL * 1000.0f) / period_usecs);
}

// New C++ jack_engine
namespace jack
{

int internal_client_request_pp( void * ptr, jack_request_t * request )
{
    engine * engine_ptr = static_cast<engine*>( ptr );
    engine_ptr->do_request( request, nullptr );
    return request->status;
}

void engine::reset_rolling_usecs()
{
    memset( rolling_client_usecs, 0,
	    sizeof( rolling_client_usecs ) );
    rolling_client_usecs_index = 0;
    rolling_client_usecs_cnt = 0;

    if( driver) {
	rolling_interval = jack_rolling_interval( driver->period_usecs );
    } else {
	rolling_interval = JACK_ENGINE_ROLLING_INTERVAL;
    }

    spare_usecs = 0;
}

int engine::set_sample_rate( jack_nframes_t nframes )
{
    control->current_time.frame_rate = nframes;
    control->pending_time.frame_rate = nframes;

    return 0;
}

int engine::get_fifo_fd( unsigned int which_fifo )
{
    /* caller must hold client_lock */
    char path[PATH_MAX+1];
    struct stat statbuf;

    snprintf (path, sizeof (path), "%s-%d", fifo_prefix,
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

    if( which_fifo >= fifo_size ) {
	unsigned int i;

	fifo = (int *)realloc( fifo,
			       sizeof (int) * (fifo_size + 16));
	for( i = fifo_size; i < fifo_size + 16; i++) {
	    fifo[i] = -1;
	}
	fifo_size += 16;
    }

    if( fifo[which_fifo] < 0) {
	if( (fifo[which_fifo] = open (path, O_RDWR|O_CREAT|O_NONBLOCK, 0666)) < 0) {
	    jack_error ("cannot open fifo [%s] (%s)", path,
			strerror (errno));
	    return -1;
	}
	DEBUG ("opened engine.fifo[%d] == %d (%s)",
	       which_fifo, fifo[which_fifo], path);
    }

    return fifo[which_fifo];
}

engine::engine(
    int timeout_threshold,
    int frame_time_offset,
    bool memory_locked,
    const std::string & server_name,
    int port_max,
    bool realtime,
    int realtime_priority,
    bool temporary,
    int client_timeout,
    bool unlock_gui_memory,
    bool verbose,
    bool no_zombies,
    pid_t waitpid,
    const std::vector<jack_driver_desc_t*> & loaded_drivers ) :
    drivers( loaded_drivers ),
    driver( nullptr ),
    driver_desc( nullptr ),
    driver_params( nullptr ),
    set_buffer_size_c( engine::driver_buffer_size ),
    set_sample_rate_c( engine::set_sample_rate ),
    run_cycle_c( engine::run_cycle ),
    delay_c( engine::delay ),
    transport_cycle_start_c( jack_transport_cycle_start ),
    driver_exit_c( engine::driver_exit ),
    get_microseconds_c( nullptr ), // Initialised after a call to jack_set_clock_source
    client_timeout_msecs( client_timeout ),
    port_max( port_max ),
    server_thread( 0 ),
    pfd_size( 0 ),
    pfd_max( 0 ),
    pfd( 0 ),
    fifo_size( 16 ),
    session_reply_fd( -1 ),
    session_pending_replies( 0 ),
    external_client_cnt( 0 ),
    realtime( realtime ),
    rtpriority( realtime_priority ),
    freewheeling( false ),
    stop_freewheeling( false ),
    verbose( verbose ),
    memory_locked( memory_locked ),
    do_munlock( unlock_gui_memory ),
    server_name( server_name ),
    temporary( temporary ),
    reordered( 0 ),
    feedbackcount( 0 ),
    removing_clients( false ),
    wait_pid( waitpid ),
    nozombies( no_zombies ),
    timeout_count_threshold( timeout_threshold ),
    frame_time_offset( frame_time_offset ),
    problems( 0 ),
    timeout_count( 0 ),
    new_clients_allowed( true ),
    silent_buffer( 0 ),
    max_usecs( 0.0f ),
    first_wakeup( true ),
    audio_out_cnt( 0 ),
    audio_in_cnt( 0 ),
    midi_out_cnt( 0 ),
    midi_in_cnt( 0 )
{
}

void engine::transport_init()
{
    timebase_client = nullptr;
    control->transport_state = JackTransportStopped;
    control->transport_cmd = TransportCommandStop;
    control->previous_cmd = TransportCommandStop;
    memset( &control->current_time, 0, sizeof(control->current_time) );
    memset( &control->pending_time, 0, sizeof(control->pending_time) );
    memset( &control->request_time, 0, sizeof(control->request_time) );
    control->prev_request = 0;
//    control->seq_number = 1;		/* can't start at 0 */
    control->new_pos = 0;
    control->pending_pos = 0;
    control->pending_frame = 0;
    control->sync_clients = 0;
    control->sync_remain = 0;
    control->sync_timeout = 2000000;	/* 2 second default */
    control->sync_time_left = 0;

    seq_number = 1;
}

static void * server_thread_adaptor( void * arg )
{
    engine * eng_ptr = static_cast<engine*>( arg );
    eng_ptr->server_thread_func();

    return nullptr;
}

int engine::init()
{
    if( realtime ) {
	if( jack_acquire_real_time_scheduling( pthread_self(), 10) != 0 ) {
	    return -1;
	}

	jack_drop_real_time_scheduling( pthread_self() );

#ifdef USE_MLOCK
	if( memory_locked && ( mlockall( MCL_CURRENT | MCL_FUTURE ) != 0 ) ) {
	    jack_error( "cannot lock down memory for jackd (%s)",
			strerror( errno ) );
#ifdef ENSURE_MLOCK
	    return -1;
#endif /* ENSURE_MLOCK */
	}
#endif /* USE_MLOCK */
    }

    jack_messagebuffer_init();

    VERBOSE( this, "jack::engine::init()" );

    jack_init_time();

    jack_uuid_clear( &fwclient );

    reset_rolling_usecs();
    max_usecs = 0.0f;

    pthread_rwlock_init( &client_lock, 0 );
    pthread_mutex_init( &port_lock, 0 );
    pthread_mutex_init( &request_lock, 0 );
    pthread_mutex_init( &problem_lock, 0 );

    fifo = (int *) malloc (sizeof (int) * fifo_size);
    for( uint32_t i = 0; i < fifo_size; i++ ) {
	fifo[i] = -1;
    }

    if( pipe( cleanup_fifo ) ) {
	jack_error ("cannot create cleanup FIFOs (%s)", strerror (errno));
	return -1;
    }

    if( fcntl( cleanup_fifo[0], F_SETFL, O_NONBLOCK ) ) {
	jack_error ("cannot set O_NONBLOCK on cleanup read FIFO (%s)", strerror (errno));
	return -1;
    }

    if( fcntl( cleanup_fifo[1], F_SETFL, O_NONBLOCK ) ) {
	jack_error ("cannot set O_NONBLOCK on cleanup write FIFO (%s)", strerror (errno));
	return -1;
    }

    srandom( time( (time_t *) 0 )  );

    if( jack_shmalloc( sizeof( jack_control_t )
		       + ( (sizeof( jack_port_shared_t ) * port_max) ),
		       &control_shm) ) {
	jack_error ("cannot create engine control shared memory "
		    "segment (%s)", strerror(errno));
	return -1;
    }

    if( jack_attach_shm( &control_shm ) ) {
	jack_error ("cannot attach to engine control shared memory"
		    " (%s)", strerror (errno));
	jack_destroy_shm( &control_shm );
	return -1;
    }

    control = (jack_control_t *)jack_shm_addr(&control_shm);

    /* Setup port type information from builtins. buffer space is
     * allocated when the driver calls jack_driver_buffer_size().
     */
    for( uint32_t i = 0 ; i < jack_builtin_port_types.size() ; ++i ) {
	jack_port_type_info_t & bipt = jack_builtin_port_types[i];

	memcpy( &control->port_types[i],
		&bipt,
		sizeof( jack_port_type_info_t ));

	VERBOSE( this, "registered builtin port type %s",
		 control->port_types[i].type_name);

	/* the port type id is index into port_types array */
	control->port_types[i].ptype_id = i;

	/* be sure to initialize mutex correctly */
	pthread_mutex_init( &port_buffers[i].lock, NULL);

	/* set buffer list info correctly */
	port_buffers[i].freelist_vector.clear();
	port_buffers[i].info = NULL;

	/* mark each port segment as not allocated */
	port_segment[i].index = -1;
	port_segment[i].attached_at = 0;
    }

    control->n_port_types = jack_builtin_port_types.size();

    /* Mark all ports as available */

    for( uint32_t i = 0; i < port_max; i++ ) {
	control->ports[i].in_use = 0;
	control->ports[i].id = i;
	control->ports[i].alias1[0] = '\0';
	control->ports[i].alias2[0] = '\0';
    }

    /* allocate internal port structures so that we can keep track
     * of port connections.
     */
    internal_ports.resize( port_max );

    for( uint32_t i = 0; i < port_max; i++ ) {
	internal_ports[i].connections_vector.clear();
    }

    if( make_sockets( server_name, fds ) < 0) {
	jack_error( "cannot create server sockets" );
	return -1;
    }

    control->port_max = port_max;
    control->real_time = realtime;

    /* leave some headroom for other client threads to run
       with priority higher than the regular client threads
       but less than the server. see thread.h for
       jack_client_real_time_priority() and jack_client_max_real_time_priority()
       which are affected by this.
    */

    control->client_priority = ( realtime ?
				 rtpriority - 5 : 0 );

    control->max_client_priority = ( realtime ?
				     rtpriority - 1 : 0 );

    control->do_mlock = memory_locked;
    control->do_munlock = do_munlock;
    control->cpu_load = 0;
    control->xrun_delayed_usecs = 0;
    control->max_delayed_usecs = 0;

    jack_set_clock_source( clock_source );
    control->clock_source = clock_source;
    get_microseconds_c = jack_get_microseconds_pointer();

    VERBOSE( this, "clock source = %s", jack_clock_source_name( clock_source ) );

    control->frame_timer.frames = frame_time_offset;
    control->frame_timer.reset_pending = 0;
    control->frame_timer.current_wakeup = 0;
    control->frame_timer.next_wakeup = 0;
    control->frame_timer.initialized = 0;
    control->frame_timer.filter_omega = 0; /* Initialised later */
    control->frame_timer.period_usecs = 0; /* Initialised later */

    first_wakeup = 1;

    control->buffer_size = 0;
    transport_init();
    set_sample_rate( 0 );
    control->internal = 0;

    control->has_capabilities = 0;

    control->engine_ok = 1;

    const string & server_dir_str = server_dir( server_name );

    snprintf( fifo_prefix, sizeof( fifo_prefix ),
	      "%s/jack-ack-fifo-%d",
	      server_dir_str.c_str(), getpid());

    (void)get_fifo_fd( 0 );

    jack_client_create_thread( NULL,
			       &server_thread, 0, FALSE,
			       &server_thread_adaptor, this );

    return 0;
}

void engine::signal_problems()
{
    jack_lock_problems( this );
    problems++;
    jack_unlock_problems( this );
    wake_server_thread_();
}

/* Linux kernels somewhere between 2.6.18 and 2.6.24 had a bug
   in poll(2) that led poll to return early. To fix it, we need
   to know that that jack_get_microseconds() is monotonic.
*/

#ifdef HAVE_CLOCK_GETTIME
static const int system_clock_monotonic = 1;
#else
static const int system_clock_monotonic = 0;
#endif

int engine::linux_poll_bug_encountered( jack_time_t then, jack_time_t * required )
{
    if( control->clock_source != JACK_TIMER_SYSTEM_CLOCK || system_clock_monotonic ) {
	jack_time_t now = jack_get_microseconds();

	if( (now - then) < *required ) {
	    /*
	      So, adjust poll timeout to account for time already spent waiting.
	    */

	    VERBOSE( this, "FALSE WAKEUP (%lldusecs vs. %lld usec)", (now - then), *required );
	    *required -= (now - then);

	    /* allow 0.25msec slop */
	    return 1;
	}
    }
    return 0;
}

void engine::cleanup()
{
    VERBOSE( this, "starting server engine shutdown" );

    do_stop_freewheeling( 1 );

    control->engine_ok = 0;	/* tell clients we're going away */

    /* this will wake the server thread and cause it to exit */

    close( cleanup_fifo[0]);
    close( cleanup_fifo[1]);

    /* shutdown master socket to prevent new clients arriving */
    shutdown( fds[0], SHUT_RDWR);
    // close (fds[0]);

    /* now really tell them we're going away */

    for( size_t i = 0; i < pfd_max; ++i) {
	shutdown( pfd[i].fd, SHUT_RDWR );
    }

    if( driver ) {
	jack_driver_t* driverToStop = driver;

	VERBOSE( this, "stopping driver");
	driverToStop->stop( driverToStop );
	// VERBOSE (this, "detaching driver");
	// driver->detach (driver, this);
	driver_unload( driverToStop );
	driver = nullptr;
    }

    if( slave_drivers.size() > 0 ) {
	VERBOSE( this, "unloading slave drivers");
	for( jack_driver_t * slave_driver : slave_drivers ) {
	    unload_slave_driver( slave_driver );
	    free( slave_driver );
	}
    }

    VERBOSE( this, "freeing shared port segments");
    for ( ssize_t i = 0; i < control->n_port_types; ++i) {
	jack_release_shm( &port_segment[i] );
	jack_destroy_shm( &port_segment[i] );
    }

    /* stop the other engine threads */
    VERBOSE( this, "stopping server thread");

    pthread_cancel( server_thread );
    pthread_join( server_thread, NULL );

    VERBOSE( this, "last xrun delay: %.3f usecs",
	     control->xrun_delayed_usecs);
    VERBOSE( this, "max delay reported by backend: %.3f usecs",
	     control->max_delayed_usecs);

    /* free engine control shm segment */
    control = nullptr;
    VERBOSE( this, "freeing engine shared memory");
    jack_release_shm( &control_shm);
    jack_destroy_shm( &control_shm);

    VERBOSE( this, "max usecs: %.3f, engine deleted", max_usecs);

    jack_messagebuffer_exit();

    if( fifo ) {
	free( fifo );
	fifo = nullptr;
    }
}

void engine::deliver_event_to_all( jack_event_t * event )
{
    jack_rdlock_graph( this );
    for( jack_client_internal_t * client : clients ) {
	deliver_event( client, event );
    }
    jack_unlock_graph( this );
}

int engine::deliver_event( jack_client_internal_t * client,
			   const jack_event_t * event,
			   ... )
{
    va_list ap;
    char status=0;
    char* key = 0;
    size_t keylen = 0;

    va_start( ap, event );

    /* caller must hold the graph lock */

    DEBUG ("delivering event (type %s)", jack_event_type_name (event->type));

    /* we are not RT-constrained here, so use kill(2) to beef up
       our check on a client's continued well-being
    */

    if( client->control->dead || client->error >= JACK_ERROR_WITH_SOCKETS
	|| (client->control->type == ClientExternal && kill (client->control->pid, 0))) {
	DEBUG ("client %s is dead - no event sent",
	       client->control->name);
	return 0;
    }

    DEBUG ("client %s is still alive", client->control->name);

    /* Check property change events for matching key_size and keys */

    if( event->type == PropertyChange ) {
	key = va_arg (ap, char*);
	if( key && key[0] != '\0' ) {
	    keylen = strlen (key) + 1;
	    if( event->y.key_size != keylen ) {
		jack_error ("property change key %s sent with wrong length (%d vs %d)", key, event->y.key_size, keylen);
		return -1;
	    }
	}
    }

    va_end( ap );

    if( jack_client_is_internal( client )) {
	switch( event->type ) {
	    case PortConnected:
	    case PortDisconnected:
		jack_client_handle_port_connection( client->private_client,
						    (jack_event_t*)event);
		break;
	    case BufferSizeChange:
		jack_client_fix_port_buffers( client->private_client );

		if( client->control->bufsize_cbset ) {
		    if( event->x.n < 16 ) {
			abort ();
		    }
		    client->private_client->bufsize( event->x.n,
						     client->private_client->bufsize_arg );
		}
		break;
	    case SampleRateChange:
		if( client->control->srate_cbset ) {
		    client->private_client->srate( event->x.n,
						   client->private_client->srate_arg );
		}
		break;
	    case GraphReordered:
		if( client->control->graph_order_cbset ) {
		    client->private_client->graph_order( client->private_client->graph_order_arg );
		}
		break;
	    case XRun:
		if( client->control->xrun_cbset ) {
		    client->private_client->xrun( client->private_client->xrun_arg );
		}
		break;
	    case PropertyChange:
		if( client->control->property_cbset ) {
		    client->private_client->property_cb( event->x.uuid,
							 key,
							 event->z.property_change,
							 client->private_client->property_cb_arg );
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
    }
    else {
	if( client->control->active ) {

	    /* there's a thread waiting for events, so
	     * it's worth telling the client */

	    DEBUG ("engine writing on event fd");

	    if( write( client->event_fd, event, sizeof (*event) ) != sizeof (*event)) {
		jack_error ("cannot send event to client [%s]"
			    " (%s)", client->control->name,
			    strerror (errno));
		client->error += JACK_ERROR_WITH_SOCKETS;
		signal_problems();
	    }

	    /* for property changes, deliver the extra data representing
	       the variable length "key" that has changed in some way.
	    */

	    if( event->type == PropertyChange ) {
		if( write( client->event_fd, key, keylen) != (ssize_t)keylen ) {
		    jack_error ("cannot send property change key to client [%s] (%s)",
				client->control->name,
				strerror (errno));
		    client->error += JACK_ERROR_WITH_SOCKETS;
		    signal_problems();
		}
	    }

	    if( client->error ) {
		status = -1;
	    } else {
		// then we check whether there really is an error.... :)

		struct pollfd pfd[1];
		pfd[0].fd = client->event_fd;
		pfd[0].events = POLLERR|POLLIN|POLLHUP|POLLNVAL;
		jack_time_t poll_timeout = JACKD_CLIENT_EVENT_TIMEOUT;
		int poll_ret;
		jack_time_t then = jack_get_microseconds();
		jack_time_t now;

		/* if we're not running realtime and there is a client timeout set
		   that exceeds the default client event timeout (which is not
		   bound by RT limits, then use the larger timeout.
		*/

		if (!control->real_time && ((size_t)client_timeout_msecs > poll_timeout)) {
		    poll_timeout = client_timeout_msecs;
		}

#ifdef __linux
	      again:
#endif
		VERBOSE( this,"client event poll on %d for %s starts at %lld",
			 client->event_fd, client->control->name, then );
		if( (poll_ret = poll( pfd, 1, poll_timeout )) < 0 ) {
		    DEBUG( "client event poll not ok! (-1) poll returned an error" );
		    jack_error( "poll on subgraph processing failed (%s)", strerror (errno) );
		    status = -1;
		} else {
		    DEBUG( "\n\n\n\n\n back from client event poll, revents = 0x%x\n\n\n", pfd[0].revents );
		    now = jack_get_microseconds();
		    VERBOSE( this, "back from client event poll after %lld usecs", now - then );

		    if( pfd[0].revents & ~POLLIN ) {

			/* some kind of OOB socket event */

			DEBUG( "client event poll not ok! (-2), revents = %d\n", pfd[0].revents );
			jack_error( "subgraph starting at %s lost client", client->control->name );
			status = -2;

		    } else if( pfd[0].revents & POLLIN ) {

			/* client responded normally */

			DEBUG("client event poll ok!");
			status = 0;

		    } else if( poll_ret == 0 ) {

			/* no events, no errors, we woke up because poll()
			   decided that time was up ...
			*/

#ifdef __linux
			if( linux_poll_bug_encountered( then, &poll_timeout) ) {
			    goto again;
			}

			if( poll_timeout < 200 ) {
			    VERBOSE( this, "FALSE WAKEUP skipped, remaining = %lld usec", poll_timeout );
			    status = 0;
			} else {
#endif
			    DEBUG( "client event poll not ok! (1 = poll timed out, revents = 0x%04x, poll_ret = %d)", pfd[0].revents, poll_ret );
			    VERBOSE( this, "client %s did not respond to event type %d in time"
				     "(fd=%d, revents = 0x%04x, timeout was %lld)",
				     client->control->name, event->type,
				     client->event_fd,
				     pfd[0].revents,
				     poll_timeout );
			    status = -2;
#ifdef __linux
			}
#endif
		    }
		}
	    }

	    if( status == 0 ) {
		if( read( client->event_fd, &status, sizeof (status)) != sizeof (status) ) {
		    jack_error( "cannot read event response from "
				"client [%s] (%s)",
				client->control->name,
				strerror (errno) );
		    status = -1;
		}
	    } else {
		switch( status ) {
		    case -1:
			jack_error( "internal poll failure reading response from client %s to a %s event",
				    client->control->name,
				    jack_event_type_name (event->type));
			break;
		    case -2:
			jack_error( "timeout waiting for client %s to handle a %s event",
				    client->control->name,
				    jack_event_type_name (event->type));
			break;
		    default:
			jack_error( "bad status (%d) from client %s while handling a %s event",
				    (int) status,
				    client->control->name,
				    jack_event_type_name (event->type));
		}
	    }

	    if( status<0 ) {
		client->error += JACK_ERROR_WITH_SOCKETS;
		signal_problems();
	    }
	}
    }
    DEBUG( "event delivered" );

    return status;
}

int engine::drivers_start()
{
    vector<jack_driver_t*> failed_drivers;
    /* first start the slave drivers */
    for( jack_driver_t * sdriver : slave_drivers ) {
	if( sdriver->start( sdriver ) ) {
	    failed_drivers.push_back( sdriver );
	}
    }
    for( jack_driver_t * sdriver : failed_drivers ) {
	jack_error( "slave driver %s failed to start, removing it", sdriver->internal_client->control->name );
	slave_driver_remove( sdriver );
    }
    /* now the master driver is started */
    return driver->start( driver );
}

int engine::load_driver( jack_driver_desc_t * idriver_desc,
			 JSList * idriver_params_jsl )
{
    jack_client_internal_t *client;
    jack_driver_t *driver;
    jack_driver_info_t *info;

    if( (info = load_driver_so_( idriver_desc )) == nullptr) {
	return -1;
    }

    VERBOSE( this, "loaded driver so");

    if( (client = create_driver_client_internal( info->client_name )) == nullptr) {
	return -1;
    }

    VERBOSE( this, "created driver client");

    if( (driver = info->initialize( client->private_client, idriver_params_jsl)) == nullptr) {
	free( info );
	return -1;
    }

    VERBOSE( this, "Initialised driver");

    driver->handle = info->handle;
    driver->finish = info->finish;
    driver->internal_client = client;
    free( info );

    if( use_driver( driver ) < 0 ) {
	remove_client_internal( client );
	return -1;
    }

    VERBOSE( this, "Switched to driver");

    driver_desc   = idriver_desc;
    driver_params = idriver_params_jsl;

    return 0;
}

int engine::use_driver( jack_driver_t * idriver )
{
    if( driver ) {
	driver->detach( driver, this );
	driver = nullptr;
    }

    if( idriver ) {
	driver = idriver;

	if( idriver->attach( idriver, this )) {
	    driver = nullptr;
	    return -1;
	}

	rolling_interval = jack_rolling_interval( driver->period_usecs );
    }

    return 0;
}

jack_driver_info_t * engine::load_driver_so_( jack_driver_desc_t * driver_desc )
{
    const char *errstr;
    jack_driver_info_t *info;

    info = (jack_driver_info_t *)calloc( 1, sizeof(*info) );

    info->handle = dlopen( driver_desc->file, RTLD_NOW|RTLD_GLOBAL );

    if( info->handle == nullptr ) {
	if( (errstr = dlerror()) != 0 ) {
	    jack_error( "can't load \"%s\": %s", driver_desc->file,
			errstr);
	} else {
	    jack_error( "bizarre error loading driver shared "
			"object %s", driver_desc->file);
	}
	goto fail;
    }

    info->initialize = (jack_driver_info_init_callback_t)dlsym( info->handle, "driver_initialize" );

    if( (errstr = dlerror()) != 0 ) {
	jack_error( "no initialize function in shared object %s\n",
		    driver_desc->file );
	goto fail;
    }

    info->finish = (jack_driver_info_finish_callback_t)dlsym( info->handle, "driver_finish" );

    if( (errstr = dlerror()) != 0 ) {
	jack_error( "no finish function in in shared driver object %s",
		    driver_desc->file );
	goto fail;
    }

    info->client_name = (char *)dlsym( info->handle, "driver_client_name" );

    if( (errstr = dlerror()) != 0 ) {
	jack_error( "no client name in in shared driver object %s",
		    driver_desc->file );
	goto fail;
    }

    return info;

  fail:
    if( info->handle ) {
	dlclose( info->handle );
    }
    free( info );
    return nullptr;
}

int engine::load_slave_driver( jack_driver_desc_t * idriver_desc,
			       JSList * idriver_params_jsl )
{
    jack_client_internal_t *client;
    jack_driver_t *driver;
    jack_driver_info_t *info;

    if( (info = load_driver_so_( idriver_desc )) == nullptr ) {
	jack_info( "Loading slave failed\n" );
	return -1;
    }

    if( (client = create_driver_client_internal( info->client_name ) ) == nullptr ) {
	jack_info( "Creating slave failed\n" );
	return -1;
    }

    if( (driver = info->initialize( client->private_client,
				    idriver_params_jsl )) == nullptr ) {
	free( info );
	jack_info( "Initializing slave failed\n" );
	return -1;
    }

    driver->handle = info->handle;
    driver->finish = info->finish;
    driver->internal_client = client;
    free( info );

    if( add_slave_driver( driver ) < 0 ) {
	jack_info( "Adding slave failed\n" );
	client_internal_delete( client );
	return -1;
    }

    return 0;
}

int engine::add_slave_driver( jack_driver_t * sdriver )
{
    if( sdriver ) {
	if( sdriver->attach( sdriver, this )) {
	    jack_info( "could not attach slave %s\n", sdriver->internal_client->control->name );
	    return -1;
	}

	slave_drivers.push_back( sdriver );
    }

    return 0;
}

int engine::unload_slave_driver( jack_driver_t * sdriver )
{
    slave_driver_remove( sdriver );
    client_internal_delete( sdriver->internal_client );
    return 0;
}

void engine::slave_driver_remove( jack_driver_t * sdriver )
{
    sdriver->detach( sdriver, this );
    auto sdFinder = std::find( slave_drivers.begin(), slave_drivers.end(), sdriver );
    if( sdFinder != slave_drivers.end() ) {
	slave_drivers.erase( sdFinder );
    }

    driver_unload( sdriver );
}

void engine::property_change_notify( jack_property_change_t change,
				     jack_uuid_t uuid,
				     const char* key )
{
    jack_event_t event;

    event.type = PropertyChange;
    event.z.property_change = change;
    jack_uuid_copy( &event.x.uuid, uuid );

    if( key ) {
	event.y.key_size = strlen( key ) + 1;
    } else {
	event.y.key_size = 0;
    }

    for( jack_client_internal_t * client : clients ) {
	if( !client->control->active ) {
	    continue;
	}

	if( client->control->property_cbset ) {
	    if( deliver_event( client, &event, key ) ) {
		jack_error( "cannot send property change notification to %s (%s)",
			    client->control->name,
			    strerror(errno) );
	    }
	}
    }
}

void engine::client_registration_notify( const char * name, int yn )
{
    jack_event_t event;

    event.type = (yn ? ClientRegistered : ClientUnregistered);
    snprintf( event.x.name, sizeof (event.x.name), "%s", name );

    for( jack_client_internal_t * client : clients ) {

	if (!client->control->active) {
	    continue;
	}

	if( strcmp((char*) client->control->name, (char*) name) == 0 ) {
	    /* do not notify client of its own registration */
	    continue;
	}

	if( client->control->client_register_cbset ) {
	    if( deliver_event( client, &event )) {
		jack_error( "cannot send client registration"
			    " notification to %s (%s)",
			    client->control->name,
			    strerror(errno) );
	    }
	}
    }
}

void engine::client_internal_delete( jack_client_internal_t * client )
{
    jack_uuid_t uuid = JACK_UUID_EMPTY_INITIALIZER;
    jack_uuid_copy( &uuid, client->control->uuid );

    client_registration_notify( (const char*) client->control->name, 0 );

    jack_remove_properties( nullptr, uuid );
    /* have to do the notification ourselves, since the client argument
       to jack_remove_properties() was NULL
    */
    property_change_notify( PropertyDeleted, uuid, nullptr );

    if( jack_client_is_internal(client) ) {
	delete client->private_client;
	if( client->control ) {
	    free( (void*)client->control);
	}

    } else {

	/* release the client segment, mark it for
	   destruction, and free up the shm registry
	   information so that it can be reused.
	*/

	jack_release_shm( &client->control_shm );
	jack_destroy_shm( &client->control_shm );
    }

    delete client;
}

jack_client_internal_t * engine::create_driver_client_internal( char *name )
{
    jack_client_connect_request_t req;
    jack_status_t status;
    jack_client_internal_t *client;
    jack_uuid_t empty_uuid = JACK_UUID_EMPTY_INITIALIZER;

    VALGRIND_MEMSET(&empty_uuid, 0, sizeof(empty_uuid));

    snprintf( req.name, sizeof (req.name), "%s", name );

    jack_uuid_clear( &empty_uuid );

    pthread_mutex_lock( &request_lock );
    client = setup_client( ClientDriver, name, empty_uuid, JackUseExactName,
			   &status, -1, nullptr, nullptr );
    pthread_mutex_unlock( &request_lock );

    return client;
}

void engine::remove_client_internal( jack_client_internal_t *client )
{
    jack_uuid_t finalizer = JACK_UUID_EMPTY_INITIALIZER;

    jack_uuid_clear( &finalizer );

    /* caller must write-hold the client lock */

    VERBOSE( this, "removing client \"%s\"", client->control->name);

    if( client->control->type == ClientInternal ) {
	/* unload it while its still a regular client */

	client_internal_unload_( client );
    }

    /* if its not already a zombie, make it so */

    if( !client->control->dead ) {
	zombify_client_( client );
    }

    if( client->session_reply_pending ) {
	session_pending_replies -= 1;

	if( session_pending_replies == 0 ) {
	    if( write( session_reply_fd, &finalizer, sizeof (finalizer))
		< (ssize_t) sizeof (finalizer)) {
		jack_error( "cannot write SessionNotify result "
			    "to client via fd = %d (%s)",
			    session_reply_fd, strerror (errno));
	    }
	    session_reply_fd = -1;
	}
    }

    if( client->control->type == ClientExternal ) {
	/* try to force the server thread to return from poll */
	close( client->event_fd );
	close( client->request_fd );
    }

    VERBOSE( this, "before: client vector contains %d", clients.size() );

    auto cFinder = std::find_if( clients.begin(), clients.end(),
				 [&client] ( jack_client_internal_t * ic ) {
				     return jack_uuid_compare( ic->control->uuid, client->control->uuid ) == 0;
				 } );

    if( cFinder != clients.end() ) {
	clients.erase( cFinder );
	VERBOSE( this, "Found and removed from client vector via matching UUID" );
    }

    VERBOSE( this, "after: client vector contains %d", clients.size() );

    client_internal_delete( client );

    if( temporary ) {
	int external_clients = 0;

	/* count external clients only when deciding whether to shutdown */

	for( jack_client_internal_t * client : clients ) {
	    if( client->control->type == ClientExternal ) {
		++external_clients;
	    }
	}

	if( external_clients == 0 ) {
	    if( wait_pid >= 0 ) {
		/* block new clients from being created
		   after we release the lock.
		*/
		new_clients_allowed = 0;
		/* tell the waiter we're done
		   to initiate a normal shutdown.
		*/
		VERBOSE( this, "Kill wait pid to stop");
		kill( wait_pid, SIGUSR2 );
		/* unlock the graph so that the server thread can finish */
		jack_unlock_graph( this );
		sleep( -1 );
	    } else {
		exit( 0 );
	    }
	}
    }
}

int engine::do_stop_freewheeling( int engine_exiting )
{
    jack_event_t event;
    void *ftstatus;

    if( !freewheeling ) {
	return 0;
    }

    if( driver == nullptr ) {
	jack_error ("cannot stop freewheeling without a driver!");
	return -1;
    }

    if( !freewheeling ) {
	VERBOSE( this, "stop freewheel when not freewheeling" );
	return 0;
    }

    /* tell the freewheel thread to stop, and wait for it
       to exit.
    */

    stop_freewheeling = 1;

    VERBOSE( this, "freewheeling stopped, waiting for thread");
    pthread_join( freewheel_thread, &ftstatus );
    VERBOSE( this, "freewheel thread has returned");

    jack_uuid_clear( &fwclient );
    freewheeling = 0;
    control->frame_timer.reset_pending = 1;

    if( !engine_exiting ) {
	/* tell everyone we've stopped */
	event.type = StopFreewheel;
	deliver_event_to_all( &event );

	/* restart the driver */
	if( drivers_start() ) {
	    jack_error ("could not restart driver after freewheeling");
	    return -1;
	}
    }

    return 0;
}

engine::~engine()
{
}

void engine::client_internal_unload_( jack_client_internal_t * client )
{
    if( client->handle ) {
	if( client->finish ) {
	    client->finish( client->private_client->process_arg );
	}
	dlclose( client->handle );
    }
}

void engine::zombify_client_( jack_client_internal_t * client )
{
    VERBOSE( this, "removing client \"%s\" from the processing chain",
	     client->control->name );

    /* caller must hold the client_lock */

    /* this stops jack_engine_deliver_event() from contacing this client */

    client->control->dead = TRUE;

    client_disconnect_ports_( client );
    client_do_deactivate_( client, FALSE );
}

void engine::client_disconnect_ports_( jack_client_internal_t *client )
{
    /* call tree **** MUST HOLD *** engine->client_lock */

    for( jack_port_internal_t * port : client->ports_vector ) {
	port_clear_connections( port );
	port_registration_notify( port->shared->id, FALSE );
	port_release( port );
    }

    client->truefeeds_vector.clear();
    client->sortfeeds_vector.clear();
    client->ports_vector.clear();
}

int engine::client_do_deactivate_( jack_client_internal_t *client,
				   int sort_graph )
{
    /* caller must hold engine->client_lock and must have checked for and/or
     *   cleared all connections held by client.
     */
    VERBOSE( this,"+++ deactivate %s", client->control->name);

    client->control->active = FALSE;

    jack_transport_client_exit( this, client );

    if( !jack_client_is_internal(client) &&
	external_client_cnt > 0) {
	external_client_cnt--;
    }

    if( sort_graph ) {
	sort_graph_();
    }
    return 0;
}

void engine::port_clear_connections( jack_port_internal_t *port )
{
    // Take a copy so our iterators aren't invalidated
    vector<jack_connection_internal_t*> port_connections = port->connections_vector;
    for( jack_connection_internal_t * connection : port_connections ) {
	port_disconnect_internal_( connection->source, connection->destination );
    }

    port->connections_vector.clear();
}

void engine::port_registration_notify( jack_port_id_t port_id, int yn )
{
    jack_event_t event;

    event.type = (yn ? PortRegistered : PortUnregistered);
    event.x.port_id = port_id;

    for( jack_client_internal_t * client : clients ) {
	if (!client->control->active) {
	    continue;
	}

	if (client->control->port_register_cbset) {
	    if( deliver_event( client, &event) ) {
		jack_error( "cannot send port registration"
			    " notification to %s (%s)",
			    client->control->name,
			    strerror(errno) );
	    }
	}
    }
}

void engine::port_release( jack_port_internal_t *port )
{
    char buf[JACK_UUID_STRING_SIZE];
    jack_uuid_unparse( port->shared->uuid, buf );
    if( jack_remove_properties( NULL, port->shared->uuid ) > 0) {
	/* have to do the notification ourselves, since the client argument
	   to jack_remove_properties() was NULL
	*/
	property_change_notify( PropertyDeleted, port->shared->uuid, NULL );
    }

    pthread_mutex_lock( &port_lock );
    port->shared->in_use = 0;
    port->shared->alias1[0] = '\0';
    port->shared->alias2[0] = '\0';

    if( port->buffer_info ) {
	jack_port_buffer_list_t *blist = port_buffer_list_( port );
	pthread_mutex_lock( &blist->lock );
	blist->freelist_vector.push_back( port->buffer_info );
	port->buffer_info = NULL;
	pthread_mutex_unlock( &blist->lock );
    }
    pthread_mutex_unlock( &port_lock );
}

int engine::port_disconnect_internal_( jack_port_internal_t *srcport,
				       jack_port_internal_t *dstport )
{
    int ret = -1;
    jack_port_id_t src_id, dst_id;
    int check_acyclic = feedbackcount;

    /* call tree **** MUST HOLD **** engine->client_lock. */
    for( jack_connection_internal_t * connection : srcport->connections_vector ) {
	if( connection->source == srcport &&
	    connection->destination == dstport ) {
	    VERBOSE( this, "connections_vector DIS-connect %s and %s",
		     srcport->shared->name,
		     dstport->shared->name);

	    port_connection_remove_( srcport->connections_vector, connection );
	    port_connection_remove_( dstport->connections_vector, connection );

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

	    send_connection_notification_( srcport->shared->client_id, src_id, dst_id, FALSE );
	    send_connection_notification_( dstport->shared->client_id, dst_id, src_id, FALSE );

	    // send a port connection notification just once to everyone who cares
	    // excluding clients involved in the connection

	    notify_all_port_interested_clients_( srcport->shared->client_id,
						 dstport->shared->client_id,
						 src_id,
						 dst_id,
						 0 );

	    if( connection->dir ) {

		jack_client_internal_t *src;
		jack_client_internal_t *dst;

		src = client_internal_by_id( srcport->shared->client_id );

		dst = client_internal_by_id( dstport->shared->client_id );

		jack_info( "Attempting to remove from %d to %d",
			   srcport->shared->client_id,
			   dstport->shared->client_id );

		client_truefeed_remove_( src->truefeeds_vector, dst );

		dst->fedcount--;

		if( connection->dir == 1 ) {
		    // normal connection: remove dest from
		    // source's sortfeeds list
		    client_sortfeed_remove_( src->sortfeeds_vector, dst );
		} else {
		    // feedback connection: remove source
		    // from dest's sortfeeds list
		    client_sortfeed_remove_( dst->sortfeeds_vector, src );
		    feedbackcount--;
		    VERBOSE( this,
			     "feedback count down to %d",
			     feedbackcount);

		}
	    } // else self-connection: do nothing

	    free( connection );
	    ret = 0;
	    break;
	}
    }

    if( check_acyclic ) {
	check_acyclic_();
    }

    sort_graph_();

    return ret;
}

/* transitive closure of the relation expressed by the sortfeeds lists. */
static int jack_client_feeds_transitive( jack_client_internal_t *source,
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

	if( jack_client_feeds_transitive(med, dest) ) {
	    return 1;
	}
    }

    return 0;
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
void engine::sort_graph_()
{
    /* called, obviously, must hold engine->client_lock */

    VERBOSE( this, "++ jack_engine_sort_graph");

    std::sort( clients.begin(), clients.end(), jack_engine_clients_compare() );

    compute_all_port_total_latencies_();
    compute_new_latency_();
    rechain_graph();
    timeout_count = 0;

    VERBOSE( this, "-- jack_engine_sort_graph");
}

jack_port_buffer_list_t * engine::port_buffer_list_( jack_port_internal_t *port )
{
    /* Points to the engine's private port buffer list struct. */
    return &port_buffers[port->shared->ptype_id];
}

void engine::port_connection_remove_( std::vector<jack_connection_internal_t*> & connections,
				      jack_connection_internal_t * to_remove )
{
    auto cFinder = std::find(connections.begin(), connections.end(), to_remove );

    if( cFinder == connections.end() ) {
	VERBOSE( this, "-- failed port connection removal");
    }
    else {
	connections.erase( cFinder );
    }
}

int engine::send_connection_notification_(
    jack_uuid_t client_id,
    jack_port_id_t self_id,
    jack_port_id_t other_id,
    int connected )
{
    jack_client_internal_t *client;
    jack_event_t event;

    VALGRIND_MEMSET(&event, 0, sizeof(event));

    if( (client = client_internal_by_id( client_id) ) == nullptr ) {
	jack_error( "no such client %" PRIu32
		    " during connection notification", client_id);
	return -1;
    }

    if( client->control->active ) {
	event.type = (connected ? PortConnected : PortDisconnected);
	event.x.self_id = self_id;
	event.y.other_id = other_id;

	if( deliver_event( client, &event) ) {
	    jack_error( "cannot send port connection notification"
			" to client %s (%s)",
			client->control->name, strerror (errno));
	    return -1;
	}
    }

    return 0;
}

void engine::notify_all_port_interested_clients_(
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

    jack_client_internal_t* src_client = client_internal_by_id( src );
    jack_client_internal_t* dst_client = client_internal_by_id( dst );

    for( jack_client_internal_t * client : clients ) {
	if( src_client != client && dst_client != client && client->control->port_connect_cbset != FALSE ) {

	    /* one of the ports belong to this client or it has a port connect callback */
	    deliver_event( client, &event );
	}
    }
}

jack_client_internal_t * engine::client_internal_by_id( jack_uuid_t id )
{
    jack_client_internal_t * client = NULL;

    /* call tree ***MUST HOLD*** the graph lock */

    auto cFinder = std::find_if( clients.begin(),
				 clients.end(),
				 [&id] ( jack_client_internal_t * client ) {
				     return jack_uuid_compare( client->control->uuid, id) == 0;
				 } );

    if( cFinder != clients.end() ) {
	client = *cFinder;
    }

    return client;
}

void engine::client_truefeed_remove_( vector<jack_client_internal_t*> & truefeeds,
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

void engine::client_sortfeed_remove_( vector<jack_client_internal_t*> & sortfeeds,
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

/**
 * Checks whether the graph has become acyclic and if so modifies client
 * sortfeeds lists to turn leftover feedback connections into normal ones.
 * This lowers latency, but at the expense of some data corruption.
 */
void engine::check_acyclic_()
{
    jack_client_internal_t *src, *dst;
    jack_port_internal_t *port;
    jack_connection_internal_t *conn;
    int stuck;
    int unsortedclients = 0;

    VERBOSE( this, "checking for graph become acyclic");

    for( auto cvIter = clients.begin(), end = clients.end() ;
	 cvIter != end ; ++cvIter ) {
	src = *cvIter;
	src->tfedcount = src->fedcount;
	unsortedclients++;
    }

    stuck = FALSE;

    /* find out whether a normal sort would have been possible */
    while( unsortedclients && !stuck ) {
	stuck = TRUE;
	for( auto cvIter = clients.begin(), end = clients.end() ;
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
	VERBOSE( this, "graph is still cyclic" );
    } else {
	VERBOSE( this, "graph has become acyclic");

	/* turn feedback connections around in sortfeeds */
	for( auto cvIter = clients.begin(), end = clients.end() ;
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

			VERBOSE( this,
				 "reversing connection from "
				 "%s to %s",
				 conn->srcclient->control->name,
				 conn->dstclient->control->name);
			conn->dir = 1;
			client_sortfeed_remove_( conn->dstclient->sortfeeds_vector,
						 conn->srcclient );

			client_sortfeed_remove_( conn->srcclient->sortfeeds_vector,
						 conn->dstclient );
		    }
		}
	    }
	}
	feedbackcount = 0;
    }
}

void engine::compute_all_port_total_latencies_()
{
    jack_port_shared_t *shared = control->ports;
    unsigned int i;
    int toward_port;

    for( i = 0; i < control->port_max; i++ ) {
	if( shared[i].in_use ) {

	    if( shared[i].flags & JackPortIsOutput ) {
		toward_port = FALSE;
	    } else {
		toward_port = TRUE;
	    }

	    shared[i].total_latency = get_port_total_latency_(
		&internal_ports[i],
		0,
		toward_port);
	}
    }
}

void engine::compute_new_latency_()
{
    jack_event_t event;

    VALGRIND_MEMSET( &event, 0, sizeof(event) );

    event.type = LatencyCallback;
    event.x.n  = 0;

    /* iterate over all clients in graph order, and emit
     * capture latency callback.
     * also builds up list in reverse graph order.
     */
    for( jack_client_internal_t * client : clients ) {
	deliver_event( client, &event );
    }

    if( driver ) {
	deliver_event( driver->internal_client, &event );
    }

    /* now issue playback latency callbacks in reverse graphorder
     */
    event.x.n  = 1;
    for( auto rcIter = clients.rbegin(), end = clients.rend() ;
	 rcIter != end ; ++rcIter ) {
	jack_client_internal_t * client = *rcIter;
	deliver_event( client, &event );
    }

    if( driver ) {
	deliver_event( driver->internal_client, &event );
    }
}

int engine::rechain_graph()
{
    unsigned long n;
    int err = 0;
    jack_client_internal_t *subgraph_client, *next_client;
    jack_event_t event;
    int upstream_is_jackd;

    VALGRIND_MEMSET( &event, 0, sizeof(event) );

    clear_fifos_();

    subgraph_client = 0;

    VERBOSE( this, "++ jack_engine_rechain_graph():");

    event.type = GraphReordered;

    vector<jack_client_internal_t*>::iterator current_iterator = clients.begin();
    vector<jack_client_internal_t*>::iterator end_marker = clients.end();

    for( n = 0 ; current_iterator != end_marker ; ++current_iterator ) {
	jack_client_internal_t * client = *current_iterator;
	vector<jack_client_internal_t*>::iterator next_iterator = current_iterator + 1;

	jack_client_control_t * ctl = client->control;

	if (!ctl->process_cbset && !ctl->thread_cb_cbset) {
	    continue;
	}

	VERBOSE( this, "+++ client is now %s active ? %d", ctl->name, ctl->active);

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

		if( subgraph_client ) {
		    subgraph_client->subgraph_wait_fd = get_fifo_fd( n );
		    VERBOSE( this, "client %s: wait_fd="
			     "%d, execution_order="
			     "%lu.",
			     subgraph_client->control->name,
			     subgraph_client->subgraph_wait_fd,
			     n);
		    n++;
		}

		VERBOSE( this, "client %s: internal "
			 "client, execution_order="
			 "%lu.",
			 ctl->name, n);

		/* this does the right thing for
		 * internal clients too
		 */

		deliver_event( client, &event );

		subgraph_client = 0;

	    } else {

		if( subgraph_client == NULL ) {

		    /* start a new subgraph. the
		     * engine will start the chain
		     * by writing to the nth
		     * FIFO.
		     */

		    subgraph_client = client;
		    subgraph_client->subgraph_start_fd = get_fifo_fd( n );
		    VERBOSE( this, "client %s: "
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
		    VERBOSE( this, "client %s: in"
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
		(void)get_fifo_fd( client->execution_order + 1 );
		event.x.n = client->execution_order;
		event.y.n = upstream_is_jackd;
		deliver_event( client, &event);
		n++;
	    }
	}
    }

    if( subgraph_client ) {
	subgraph_client->subgraph_wait_fd = get_fifo_fd( n );
	VERBOSE( this, "client %s: wait_fd=%d, execution_order=%lu (last client).",
		 subgraph_client->control->name,
		 subgraph_client->subgraph_wait_fd, n);
    }

    VERBOSE( this, "-- jack_engine_rechain_graph()");

    return err;
}

jack_nframes_t engine::get_port_total_latency_( jack_port_internal_t *port, int hop_count, int toward_port )
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
    jack_info( "%sFor port %s (%s)", prefix, port->shared->name, (toward_port ? "toward" : "away") );
#endif

    for( jack_connection_internal_t * connection : port->connections_vector ) {
	jack_nframes_t this_latency;

	if( (toward_port && (connection->source->shared == port->shared)) ||
	    (!toward_port && (connection->destination->shared == port->shared))) {

#ifdef DEBUG_TOTAL_LATENCY_COMPUTATION
	    jack_info( "%s\tskip connection %s->%s",
		       prefix,
		       connection->source->shared->name,
		       connection->destination->shared->name );
#endif

	    continue;
	}

#ifdef DEBUG_TOTAL_LATENCY_COMPUTATION
	jack_info( "%s\tconnection %s->%s ... ",
		   prefix,
		   connection->source->shared->name,
		   connection->destination->shared->name );
#endif
	/* if we're a destination in the connection, recurse
	   on the source to get its total latency
	*/

	if( connection->destination == port ) {

	    if( connection->source->shared->flags & JackPortIsTerminal ) {
		this_latency = connection->source->shared->latency;
	    } else {
		this_latency = get_port_total_latency_( connection->source,
						       hop_count + 1,
						       toward_port );
	    }

	} else {

	    /* "port" is the source, so get the latency of
	     * the destination */
	    if( connection->destination->shared->flags & JackPortIsTerminal ) {
		this_latency = connection->destination->shared->latency;
	    } else {
		this_latency = get_port_total_latency_( connection->destination,
						       hop_count + 1,
						       toward_port );
	    }
	}

	if( this_latency > max_latency ) {
	    max_latency = this_latency;
	}
    }

#ifdef DEBUG_TOTAL_LATENCY_COMPUTATION
    jack_info( "%s\treturn %lu + %lu = %lu", prefix, latency, max_latency, latency + max_latency );
#endif

    return latency + max_latency;
}

void engine::clear_fifos_()
{
    /* caller must hold client_lock */

    unsigned int i;
    char buf[16];

    /* this just drains the existing FIFO's of any data left in
       them by aborted clients, etc. there is only ever going to
       be 0, 1 or 2 bytes in them, but we'll allow for up to 16.
    */
    for( i = 0; i < fifo_size; i++ ) {
	if( fifo[i] >= 0 ) {
	    int nread = read( fifo[i], buf, sizeof(buf) );

	    if( nread < 0 && errno != EAGAIN ) {
		jack_error( "clear fifo[%d] error: %s",
			    i, strerror(errno) );
	    }
	}
    }
}

/* set up all types of clients */
jack_client_internal_t * engine::setup_client(
    ClientType type, char *name, jack_uuid_t uuid, jack_options_t options,
    jack_status_t *status, int client_fd, const char *object_path, const char *object_data )
{
    /* called with the request_lock */
    jack_client_internal_t *client;
    char bufx[64];

    /* validate client name, generate a unique one if appropriate */
    if( client_name_invalid_( name, options, status ) )
	return NULL;

    ensure_uuid_unique_( uuid );

    /* create a client struct for this name */
    if( (client = setup_client_control_( client_fd, type, name, uuid)) == NULL) {
	*status = (jack_status_t)(*status | (JackFailure|JackInitFailure));
	jack_error( "cannot create new client object" );
	return NULL;
    }

    /* only for internal clients, driver is already loaded */
    if( type == ClientInternal ) {
	if( load_client_( client, object_path ) ) {
	    jack_error( "cannot dynamically load client from"
			" \"%s\"", object_path );
	    client_internal_delete( client );
	    *status = (jack_status_t)(*status | (JackFailure|JackLoadFailure));
	    return NULL;
	}
    }

    jack_uuid_unparse( client->control->uuid, bufx );

    VERBOSE( this, "new client: %s, uuid = %s"
	     " type %d @ %p fd = %d",
	     client->control->name, bufx,
	     type, client->control, client_fd);

    if( jack_client_is_internal(client) ) {

	// XXX: do i need to lock the graph here ?
	// i moved this one up in the init process, lets see what happens.

	/* Internal clients need to make regular JACK API
	 * calls, which need a jack_client_t structure.
	 * Create one here.
	 */
	client->private_client = internal_client_alloc( client->control );

	/* Set up the pointers necessary for the request
	 * system to work.  The client is in the same address
	 * space */

	client->private_client->deliver_request = internal_client_request_pp;
	client->private_client->deliver_arg = this;
    }

    /* add new client to the clients list */
    jack_lock_graph( this );

    clients.push_back( client );

    reset_rolling_usecs();

    if( jack_client_is_internal( client ) ) {

	jack_unlock_graph( this );

	/* Call its initialization function.  This function
	 * may make requests of its own, so we temporarily
	 * release and then reacquire the request_lock.  */
	if( client->control->type == ClientInternal ) {

	    pthread_mutex_unlock( &request_lock );
	    if( client->initialize( client->private_client,
				    object_data ) ) {

		/* failed: clean up client data */
		VERBOSE( this, "%s jack_initialize() failed!", client->control->name );
		jack_lock_graph( this );
		remove_client_internal( client );
		jack_unlock_graph( this );
		*status = (jack_status_t)(*status | (JackFailure|JackInitFailure));
		client = NULL;
		//JOQ: not clear that all allocated
		//storage has been cleaned up properly.
	    }
	    pthread_mutex_lock( &request_lock );
	}

    } else {			/* external client */
	jack_unlock_graph( this );
    }

    return client;
}

int engine::client_name_invalid_( char *name, jack_options_t options, jack_status_t *status)
{
    /* Since this is always called from the server thread, no
     * other new client will be created at the same time.  So,
     * testing a name for uniqueness is valid here.  When called
     * from jack_engine_load_driver() this is not strictly true,
     * but that seems to be adequately serialized due to engine
     * startup.  There are no other clients at that point, anyway.
     */

    if( client_by_name( name ) || client_name_reserved( name )) {

	*status = (jack_status_t)(*status | JackNameNotUnique);

	if( options & JackUseExactName ) {
	    jack_error( "cannot create new client; %s already"
			" exists", name );
	    *status = (jack_status_t)(*status | JackFailure);
	    return TRUE;
	}

	if( generate_unique_name( name ) ) {
	    *status = (jack_status_t)(*status | JackFailure);
	    return TRUE;
	}
    }

    return FALSE;
}

void engine::ensure_uuid_unique_( jack_uuid_t uuid )
{
    if( jack_uuid_empty( uuid ) ) {
	return;
    }

    jack_lock_graph( this );

    for( jack_client_internal_t * client : clients ) {
	if( jack_uuid_compare( client->control->uuid, uuid ) == 0 ) {
	    jack_uuid_clear( &uuid );
	}
    }
    jack_unlock_graph( this );
}

/* Set up the engine's client internal and control structures for both
 * internal and external clients. */
jack_client_internal_t * engine::setup_client_control_(
    int fd, ClientType type, const char *name, jack_uuid_t uuid )
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

    if( type != ClientExternal ) {
	client->control = (jack_client_control_t *)malloc(sizeof(jack_client_control_t));
    } else {
	if( jack_shmalloc( sizeof(jack_client_control_t), &client->control_shm) ) {
	    jack_error( "cannot create client control block for %s",
			name );
	    delete client;
	    return 0;
	}

	if( jack_attach_shm( &client->control_shm ) ) {
	    jack_error( "cannot attach to client control block "
			"for %s (%s)", name, strerror(errno) );
	    jack_destroy_shm( &client->control_shm );
	    delete client;
	    return 0;
	}

	client->control = (jack_client_control_t *)jack_shm_addr( &client->control_shm );
    }

    client->control->type = type;
    client->control->active = 0;
    client->control->dead = FALSE;
    client->control->timed_out = 0;

    if( jack_uuid_empty( uuid ) ) {
	client->control->uuid = jack_client_uuid_generate();
    } else {
	jack_uuid_t * jud = (jack_uuid_t*)(&client->control->uuid);
	jack_uuid_copy( jud, uuid );
    }

    strcpy( (char *)client->control->name, name );
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
    if( type != ClientExternal ) {
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
    jack_transport_client_new( client );

    return client;
}

int engine::load_client_( jack_client_internal_t *client,
			  const char *so_name )
{
    const char *errstr;

    if( !so_name ) {
	return -1;
    }

    stringstream ss( stringstream::out );
    if( so_name[0] == '/' ) {
	/* Absolute, use as-is, user beware ... */
	ss << so_name << ".so";
    } else {
	ss << jack::addon_dir << '/' << so_name << ".so";
    }

    string path_to_so( ss.str() );
    const char * path_to_so_cstr = path_to_so.c_str();

    client->handle = dlopen( path_to_so_cstr, RTLD_NOW|RTLD_GLOBAL );

    if( client->handle == 0 ) {
	if( (errstr = dlerror()) != 0 ) {
	    jack_error( "%s", errstr );
	} else {
	    jack_error( "bizarre error loading %s", so_name );
	}
	return -1;
    }

    client->initialize =
	(int (*)(jack_client_t*,const char*))dlsym( client->handle, "jack_initialize" );

    if( (errstr = dlerror()) != 0 ) {
	jack_error( "%s has no initialize() function\n", so_name );
	dlclose( client->handle );
	client->handle = 0;
	return -1;
    }

    client->finish =
	(void (*)(void *)) dlsym( client->handle, "jack_finish" );

    if( (errstr = dlerror ()) != 0 ) {
	jack_error( "%s has no finish() function", so_name );
	dlclose( client->handle );
	client->handle = 0;
	return -1;
    }

    return 0;
}

/*
 * Build the jack_client_t structure for an internal client.
 */
jack_client_t * engine::internal_client_alloc( jack_client_control_t *cc )
{
    jack_client_t* client;

    client = jack_client_alloc();

    client->control = cc;
    client->engine = control;

    client->n_port_types = client->engine->n_port_types;
    client->port_segment = &port_segment[0];

    return client;
}

jack_client_internal_t * engine::client_by_name( const char *name )
{
    jack_client_internal_t *client = NULL;

    jack_rdlock_graph( this );

    auto cFinder = std::find_if( clients.begin(),
				 clients.end(),
				 [&name] ( jack_client_internal_t * client ) {
				     return strcmp( (const char *)client->control->name,
						    name ) == 0;
				 } );

    if( cFinder != clients.end() ) {
	client = *cFinder;
    }

    jack_unlock_graph( this );
    return client;
}

int engine::client_name_reserved( const char *name )
{
    auto cnFinder = std::find_if( reserved_client_names.begin(),
				  reserved_client_names.end(),
				  [&name] ( jack_reserved_name_t * res ) {
				      return strcmp( res->name, name ) == 0;
				  } );

    if( cnFinder != reserved_client_names.end() ) {
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
int engine::generate_unique_name( char *name )
{
    int tens, ones;
    int length = strlen( name );

    if( length > JACK_CLIENT_NAME_SIZE - 4 ) {
	jack_error( "%s exists and is too long to make unique", name );
	return 1;		/* failure */
    }

    /*  generate a unique name by appending "-01".."-99" */
    name[length++] = '-';
    tens = length++;
    ones = length++;
    name[tens] = '0';
    name[ones] = '1';
    name[length] = '\0';
    while( client_by_name( name ) || client_name_reserved( name ) ) {
	if( name[ones] == '9' ) {
	    if( name[tens] == '9' ) {
		jack_error( "client %s has 99 extra"
			    " instances already", name );
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

void engine::intclient_load_request( jack_request_t *req )
{
    /* called with the request_lock */
    jack_client_internal_t *client;
    jack_status_t status = (jack_status_t)0;
    jack_uuid_t empty_uuid = JACK_UUID_EMPTY_INITIALIZER;

    VERBOSE( this, "load internal client %s from %s, init `%s', "
	     "options: 0x%x", req->x.intclient.name,
	     req->x.intclient.path, req->x.intclient.init,
	     req->x.intclient.options );

    jack_uuid_clear( &empty_uuid );

    jack_options_t client_options = (jack_options_t)(req->x.intclient.options | JackUseExactName);

    client = setup_client( ClientInternal, req->x.intclient.name, empty_uuid,
			   client_options, &status, -1,
			   req->x.intclient.path, req->x.intclient.init );

    if( client == nullptr ) {
	status = (jack_status_t)(status | JackFailure);	/* just making sure */
	jack_uuid_clear( &req->x.intclient.uuid );
	VERBOSE( this, "load failed, status = 0x%x", status);
    } else {
	jack_uuid_copy( &req->x.intclient.uuid, client->control->uuid );
    }

    req->status = status;
}

/**
 * Dumps current engine configuration.
 */
void engine::dump_configuration( int take_lock )
{
    jack_info( "jack_engine.cpp: <-- dump begins -->");

    if( take_lock ) {
	jack_rdlock_graph( this );
    }

    int n = 0;
    int m = 0;
    int o = 0;

    for( jack_client_internal_t * client : clients ) {
	jack_client_control_t *ctl = client->control;

	jack_info( "client #%d: %s (type: %d, process? %s, thread ? %s"
		   " start=%d wait=%d",
		   ++n,
		   ctl->name,
		   ctl->type,
		   ctl->process_cbset ? "yes" : "no",
		   ctl->thread_cb_cbset ? "yes" : "no",
		   client->subgraph_start_fd,
		   client->subgraph_wait_fd );

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

    if( take_lock ) {
	jack_unlock_graph( this );
    }

    jack_info( "jack_engine.cpp: <-- dump ends -->" );
}

void * engine::server_thread_func()
{
    struct sockaddr_un client_addr;
    socklen_t client_addrlen;
    int problemsProblemsPROBLEMS = 0;
    int client_socket;
    int done = 0;
    size_t i;
    const int fixed_fd_cnt = 3;
    int stop_freewheeling;

    while( !done ) {
	size_t num_clients;

	jack_rdlock_graph( this );

	num_clients = clients.size();

	if( pfd_size < fixed_fd_cnt + num_clients ) {
	    if( pfd ) {
		free( pfd );
	    }
                        
	    pfd = (struct pollfd *)malloc(sizeof(struct pollfd) * (fixed_fd_cnt + num_clients));
				
	    if( pfd == nullptr ) {
		/*
		 * this can happen if limits.conf was changed
		 * but the user hasn't logged out and back in yet
		 */
		if( errno == EAGAIN ) {
		    jack_error( "malloc failed (%s) - make" 
				"sure you log out and back"
				"in after changing limits"
				".conf!", strerror(errno));
		}
		else {
		    jack_error( "malloc failed (%s)",
				strerror(errno));
		}
		break;
	    }
	}

	pfd[0].fd = fds[0];
	pfd[0].events = POLLIN|POLLERR;
	pfd[1].fd = fds[1];
	pfd[1].events = POLLIN|POLLERR;
	pfd[2].fd = cleanup_fifo[0];
	pfd[2].events = POLLIN|POLLERR;
	pfd_max = fixed_fd_cnt;
		
	for( jack_client_internal_t * client : clients ) {

	    if( client->request_fd < 0 || client->error >= JACK_ERROR_WITH_SOCKETS ) {
		continue;
	    }
	    if( client->control->dead ) {
		pfd[pfd_max].fd = client->request_fd;
		pfd[pfd_max].events = POLLHUP|POLLNVAL;
		pfd_max++;
		continue;
	    }
	    pfd[pfd_max].fd = client->request_fd;
	    pfd[pfd_max].events = POLLIN|POLLPRI|POLLERR|POLLHUP|POLLNVAL;
	    pfd_max++;
	}

	jack_unlock_graph( this );
		
	VERBOSE( this, "start poll on %d fd's", pfd_max );
		
	/* go to sleep for a long, long time, or until a request
	   arrives, or until a communication channel is broken
	*/

	if( poll( pfd, pfd_max, -1 ) < 0 ) {
	    if( errno == EINTR ) {
		continue;
	    }
	    jack_error( "poll failed (%s)", strerror(errno) );
	    break;
	}

	VERBOSE( this, "server thread back from poll" );
		
	/* Stephane Letz: letz@grame.fr : has to be added
	 * otherwise pthread_cancel() does not work on MacOSX */
	pthread_testcancel();


	/* empty cleanup FIFO if necessary */

	if( pfd[2].revents & ~POLLIN ) {
	    /* time to die */
	    break;
	}

	if( pfd[2].revents & POLLIN ) {
	    char c;
	    while( read(cleanup_fifo[0], &c, 1) == 1 );
	}

	/* check each client socket before handling other request*/
	jack_rdlock_graph( this );

	for (i = fixed_fd_cnt; i < pfd_max; i++) {

	    if( pfd[i].fd < 0 ) {
		continue;
	    }

	    if( pfd[i].revents & ~POLLIN ) {
		mark_client_socket_error( pfd[i].fd );
		signal_problems();
		VERBOSE( this, "non-POLLIN events on fd %d", pfd[i].fd);
	    }
	    else if( pfd[i].revents & POLLIN ) {
		if( handle_external_client_request_( pfd[i].fd ) ) {
		    jack_error( "could not handle external"
				" client request");
		    signal_problems();
		}
	    }
	}

	problemsProblemsPROBLEMS = problems;

	jack_unlock_graph( this );

	/* need to take write lock since we may/will rip out some clients,
	   and reset engine->problems
	*/

	stop_freewheeling = 0;

	while( problemsProblemsPROBLEMS ) {
			
	    VERBOSE( this, "trying to lock graph to remove %d problems", problemsProblemsPROBLEMS );
	    jack_lock_graph( this );
	    VERBOSE( this, "we have problem clients (problems = %d", problemsProblemsPROBLEMS );
	    remove_clients( &stop_freewheeling );
	    if( stop_freewheeling ) {
		VERBOSE( this, "need to stop freewheeling once problems are cleared" );
	    }
	    jack_unlock_graph( this );

	    jack_lock_problems( this );
	    problems -= problemsProblemsPROBLEMS;
	    problemsProblemsPROBLEMS = problems;
	    jack_unlock_problems( this );

	    VERBOSE( this, "after removing clients, problems = %d", problemsProblemsPROBLEMS );
	}
		
	if( freewheeling && stop_freewheeling) {
	    do_stop_freewheeling( 0 );
	}
			
	/* check the master server socket */

	if( pfd[0].revents & POLLERR ) {
	    jack_error( "error on server socket" );
	    break;
	}
	
	if( control->engine_ok && pfd[0].revents & POLLIN ) {
	    DEBUG( "pfd[0].revents & POLLIN" );

	    memset( &client_addr, 0, sizeof(client_addr) );
	    client_addrlen = sizeof(client_addr);

	    if( (client_socket = accept(fds[0], (struct sockaddr *)&client_addr,
					&client_addrlen)) < 0 ) {
		jack_error( "cannot accept new connection (%s)",
			    strerror(errno) );
	    }
	    else if( !new_clients_allowed || client_create( client_socket ) < 0 ) {
		jack_error( "cannot complete client "
			    "connection process" );
		close( client_socket );
	    }
	}

	/* check the ACK server socket */

	if( pfd[1].revents & POLLERR ) {
	    jack_error( "error on server ACK socket" );
	    break;
	}

	if( control->engine_ok && pfd[1].revents & POLLIN ) {
	    DEBUG( "pfd[1].revents & POLLIN" );

	    memset( &client_addr, 0, sizeof(client_addr) );
	    client_addrlen = sizeof(client_addr);

	    if( (client_socket = accept(fds[1], (struct sockaddr *)&client_addr,
					&client_addrlen)) < 0) {
		jack_error( "cannot accept new ACK connection"
			    " (%s)", strerror(errno) );
	    }
	    else if( handle_client_ack_connection_( client_socket ) ) {
		jack_error( "cannot complete client ACK "
			    "connection process" );
		close( client_socket );
	    }
	}
    }

    return 0;
}

int engine::mark_client_socket_error( int fd )
{
    /* CALLER MUST HOLD GRAPH LOCK */

    auto cFinder = std::find_if( clients.begin(),
				 clients.end(),
				 [&fd] ( jack_client_internal_t * client ) {
				     if( jack_client_is_internal( client ) ) {
					 return false;
				     }
				     else {
					 return client->request_fd == fd;
				     }
				 } );

    if( cFinder != clients.end() ) {
	jack_client_internal_t * client = *cFinder;
	VERBOSE( this, "marking client %s with SOCKET error state = "
		 "%s errors = %d", client->control->name,
		 jack_client_state_name(client),
		 client->error);
	client->error += JACK_ERROR_WITH_SOCKETS;
    }

    return 0;
}

int engine::handle_external_client_request_( int fd )
{
    /* CALLER holds read lock on graph */

    jack_request_t req;
    jack_client_internal_t *client = 0;
    int reply_fd;
    ssize_t r;

    auto cFinder = std::find_if( clients.begin(),
				 clients.end(),
				 [&fd] ( jack_client_internal_t * client ) {
				     return client->request_fd == fd;
				 } );

    if( cFinder == clients.end() ) {
	jack_error( "Client input on unknown fd %d!", fd );
	return -1;
    }

    client = *cFinder;

    if( (r = read( client->request_fd, &req, sizeof(req) ) ) < (ssize_t) sizeof (req)) {
	if( r == 0 ) {
	    return 1;
	}
	else {
	    jack_error( "cannot read request from client (%d/%d/%s)",
			r, sizeof(req), strerror(errno) );
	    // XXX: shouldnt we mark this client as error now ?

	    return -1;
	}
    }

    if( req.type == PropertyChangeNotify ) {
	if( req.x.property.keylen ) {
	    req.x.property.key = (char*)malloc(req.x.property.keylen);
	    if( (r = read( client->request_fd, (char*)req.x.property.key, req.x.property.keylen)) !=
		(ssize_t)req.x.property.keylen) {
		jack_error( "cannot read property key from client (%d/%d/%s)",
			    r, sizeof(req), strerror(errno) );
		return -1;
	    }
	}
	else {
	    req.x.property.key = 0;
	}
    }

    reply_fd = client->request_fd;
	
    jack_unlock_graph( this );
    do_request( &req, &reply_fd );
    jack_lock_graph( this );

    if( req.type == PropertyChangeNotify && req.x.property.key ) {
	free( (char *)req.x.property.key );
    }

    if( reply_fd >= 0 ) {
	DEBUG( "replying to client" );
	if( write( reply_fd, &req, sizeof(req)) < (ssize_t)sizeof(req)) {
	    jack_error( "cannot write request result to client" );
	    return -1;
	}
    }
    else {
	DEBUG( "*not* replying to client" );
    }

    return 0;
}

void engine::remove_clients( int * exit_freewheeling_when_done )
{
    int need_sort = FALSE;
    /* CALLER MUST HOLD GRAPH LOCK */

    VERBOSE( this, "++ Removing failed clients ...");

    // Take a copy of the clients vector so we don't get invalid iterators
    vector<jack_client_internal_t*> clients_copy = clients;

    /* remove all dead clients */
    for( jack_client_internal_t * client : clients_copy ) {

	VERBOSE( this, "client %s error status %d", client->control->name, client->error);
		
	if( client->error ) {
			
	    if( freewheeling && jack_uuid_compare(client->control->uuid, fwclient) == 0 ) {
		VERBOSE( this, "freewheeling client has errors");
		*exit_freewheeling_when_done = 1;
	    }
			
	    /* if we have a communication problem with the
	       client, remove it. otherwise, turn it into
	       a zombie. the client will/should realize
	       this and will close its sockets.  then
	       we'll end up back here again and will
	       finally remove the client.
	    */
	    if( client->error >= JACK_ERROR_WITH_SOCKETS ) {
		VERBOSE( this, "removing failed "
			 "client %s state = %s errors"
			 " = %d", 
			 client->control->name,
			 jack_client_state_name(client),
			 client->error );
		remove_client_internal( client );
	    }
	    else {
		VERBOSE( this, "client failure: "
			 "client %s state = %s errors"
			 " = %d", 
			 client->control->name,
			 jack_client_state_name (client),
			 client->error );
		if( !nozombies ) {
		    zombify_client_( client );
		    client->error = 0;
		}
	    }

	    need_sort = TRUE;
	}
    }

    if( need_sort ) {
	sort_graph_();
    }
	
    reset_rolling_usecs();

    VERBOSE( this, "-- Removing failed clients ...");
}

int engine::client_create( int client_fd )
{
    /* called *without* the request_lock */
    jack_client_internal_t *client;
    jack_client_connect_request_t req;
    jack_client_connect_result_t res;
    ssize_t nbytes;

    res.status = (jack_status_t)0;

    VALGRIND_MEMSET( &res, 0, sizeof(res) );

    nbytes = read( client_fd, &req, sizeof(req) );
        
    if( nbytes == 0 ) {		/* EOF? */
	jack_error( "cannot read connection request from client (%s)", strerror(errno) );
	return -1;
    }

    /* First verify protocol version (first field of request), if
     * present, then make sure request has the expected length. */
    if( ((size_t)nbytes < sizeof (req.protocol_v))
	|| (req.protocol_v != jack_protocol_version)
	|| (nbytes != sizeof(req)) ) {

	/* JACK protocol incompatibility */
	res.status = (jack_status_t)(res.status | (JackFailure|JackVersionError));
	jack_error( "JACK protocol mismatch (%d vs %d)", req.protocol_v, jack_protocol_version );
	if( write( client_fd, &res, sizeof (res)) != sizeof(res) ) {
	    jack_error( "cannot write client connection response" );
	}
	return -1;
    }

    if( !req.load ) {		/* internal client close? */

	int rc = -1;
	jack_uuid_t id = JACK_UUID_EMPTY_INITIALIZER;

	if( client_id_by_name_( req.name, id ) == 0) {
	    rc = handle_unload_client_( id );
	}

	/* close does not send a reply */
	return rc;
    }
	
    pthread_mutex_lock( &request_lock );
    if( !jack_uuid_empty(req.uuid) ) {
	char *res_name = get_reserved_name_( req.uuid );
	if( res_name ) {
	    snprintf( req.name, sizeof(req.name), "%s", res_name );
	    free( res_name );
	}
    }

    client = setup_client( req.type, req.name, req.uuid,
			   req.options, &res.status, client_fd,
			   req.object_path, req.object_data );
    pthread_mutex_unlock( &request_lock );
    if( client == nullptr ) {
	res.status = (jack_status_t)(res.status | JackFailure); /* just making sure */
	return -1;
    }
    res.client_shm_index = client->control_shm.index;
    res.engine_shm_index = control_shm.index;
    res.realtime = control->real_time;
    res.realtime_priority = rtpriority - 1;
    strncpy( res.name, req.name, sizeof(res.name) );

    if( jack_client_is_internal(client) ) {
	/* the ->control pointers are for an internal client
	   so we know they are the right sized pointers
	   for this server. however, to keep the result
	   structure the same size for both 32 and 64 bit
	   clients/servers, the result structure stores
	   them as 64 bit integer, so we have to do a slightly
	   forced cast here.
	*/
	res.client_control = (uint64_t) ((intptr_t) client->control);
	res.engine_control = (uint64_t) ((intptr_t)control);
    }
    else {
	strcpy( res.fifo_prefix, fifo_prefix );
    }

    if( write( client_fd, &res, sizeof(res)) != sizeof(res) ) {
	jack_error( "cannot write connection response to client" );
	jack_lock_graph( this );
	client->control->dead = 1;
	remove_client_internal( client );
	jack_unlock_graph( this );
	return -1;
    }

    if( jack_client_is_internal(client) ) {
	close( client_fd );
    }

    client_registration_notify( (const char*)client->control->name, 1 );

    return 0;
}

int engine::handle_client_ack_connection_( int client_fd )
{
    jack_client_internal_t *client;
    jack_client_connect_ack_request_t req;
    jack_client_connect_ack_result_t res;

    if( read( client_fd, &req, sizeof (req)) != sizeof(req) ) {
	jack_error( "cannot read ACK connection request from client" );
	return -1;
    }

    if( (client = client_internal_by_id( req.client_id ) ) == nullptr ) {
	jack_error( "unknown client ID in ACK connection request" );
	return -1;
    }

    client->event_fd = client_fd;
    VERBOSE( this, "new client %s using %d for events", client->control->name,
	     client->event_fd );

    res.status = 0;

    if( write( client->event_fd, &res, sizeof(res)) != sizeof(res) ) {
	jack_error( "cannot write ACK connection response to client" );
	return -1;
    }

    return 0;
}

/* perform internal or external client request
 *
 * reply_fd is NULL for internal requests
 */
void engine::do_request( jack_request_t *req, int *reply_fd )
{
    /* The request_lock serializes internal requests (from any
     * thread in the server) with external requests (always from "the"
     * server thread). 
     */
    pthread_mutex_lock( &request_lock );

    DEBUG( "got a request of type %d (%s)", req->type, jack_event_type_name((JackEventType)req->type) );

    switch( req->type ) {
	case RegisterPort:
	    req->status = port_do_register( req, reply_fd ? FALSE : TRUE );
	    break;
	case UnRegisterPort:
	    req->status = port_do_unregister( req );
	    break;
	case ConnectPorts:
	    req->status = port_do_connect_( req->x.connect.source_port,
					    req->x.connect.destination_port );
	    break;
	case DisconnectPort:
	    req->status = port_do_disconnect_all_( req->x.port_info.port_id );
	    break;
	case DisconnectPorts:
	    req->status = port_do_disconnect_( req->x.connect.source_port,
					      req->x.connect.destination_port );
	    break;
	case ActivateClient:
	    req->status = client_activate( req->x.client_id );
	    break;
	case DeactivateClient:
	    req->status = client_deactivate( req->x.client_id );
	    break;
	case SetTimeBaseClient:
	    req->status = jack_timebase_set( this,
					     req->x.timebase.client_id,
					     req->x.timebase.conditional );
	    break;
	case ResetTimeBaseClient:
	    req->status = jack_timebase_reset( this, req->x.client_id );
	    break;
	case SetSyncClient:
	    req->status = jack_transport_client_set_sync( this, req->x.client_id );
	    break;
	case ResetSyncClient:
	    req->status = jack_transport_client_reset_sync( this, req->x.client_id );
	    break;
	case SetSyncTimeout:
	    req->status = jack_transport_set_sync_timeout( this, req->x.timeout);
	    break;
	case GetPortConnections:
	case GetPortNConnections:
	    //JOQ bug: reply_fd may be NULL if internal request
	    if ((req->status = do_get_port_connections( req, *reply_fd)) == 0) {
		/* we have already replied, don't do it again */
		*reply_fd = -1;
	    }
	    break;
	case FreeWheel:
	    req->status = do_start_freewheeling( req->x.client_id );
	    break;
	case StopFreeWheel:
	    req->status = do_stop_freewheeling( 0 );
	    break;
	case SetBufferSize:
	    req->status = set_buffer_size_request( req->x.nframes );
	    jack_lock_graph( this );
	    compute_new_latency_();
	    jack_unlock_graph( this );
	    break;
	case IntClientHandle:
	    intclient_handle_request( req );
	    break;
	case IntClientLoad:
	    intclient_load_request( req );
	    break;
	case IntClientName:
	    intclient_name_request( req );
	    break;
	case IntClientUnload:
	    intclient_unload_request( req );
	    break;
	case RecomputeTotalLatencies:
	    jack_lock_graph( this );
	    compute_all_port_total_latencies_();
	    compute_new_latency_();
	    jack_unlock_graph( this );
	    req->status = 0;
	    break;
	case RecomputeTotalLatency:
	    jack_lock_graph( this );
	    compute_port_total_latency_( &control->ports[req->x.port_info.port_id] );
	    jack_unlock_graph( this );
	    req->status = 0;
	    break;
	case GetClientByUUID:
	    jack_rdlock_graph( this );
	    client_fill_request_port_name_by_uuid_( req );
	    jack_unlock_graph( this );
	    break;
	case GetUUIDByClientName:
	    jack_rdlock_graph( this );
	    do_get_uuid_by_client_name_( req );
	    jack_unlock_graph( this );
	    break;
	case ReserveName:
	    jack_rdlock_graph( this );
	    do_reserve_name_( req );
	    jack_unlock_graph( this );
	    break;
	case SessionReply:
	    jack_rdlock_graph( this );
	    do_session_reply_( req );
	    jack_unlock_graph( this );
	    break;
	case SessionNotify:
	    jack_rdlock_graph( this );
	    if ((req->status = do_session_notify_( req, *reply_fd) ) >= 0) {
		/* we have already replied, don't do it again */
		*reply_fd = -1;
	    }
	    jack_unlock_graph( this );
	    break;
	case SessionHasCallback:
	    jack_rdlock_graph( this );
	    req->status = do_has_session_cb_( req );
	    jack_unlock_graph( this );
	    break;
        case PropertyChangeNotify:
	    property_change_notify( req->x.property.change, req->x.property.uuid, req->x.property.key );
	    break;
	default:
	    /* some requests are handled entirely on the client
	     * side, by adjusting the shared memory area(s) */
	    break;
    }
        
    pthread_mutex_unlock( &request_lock );

    DEBUG( "status of request: %d", req->status );
}

int engine::client_id_by_name_( const char *name, jack_uuid_t id )
{
    int ret = -1;

    jack_uuid_clear( &id );

    jack_rdlock_graph( this );

    for( jack_client_internal_t * client : clients ) {
	if( strcmp( (const char *)client->control->name, name) == 0 ) {
	    jack_uuid_copy( &id, client->control->uuid );
	    ret = 0;
	    break;
	}
    }

    jack_unlock_graph( this );
    return ret;
}

jack_status_t engine::handle_unload_client_( jack_uuid_t id )
{
    /* called *without* the request_lock */
    jack_client_internal_t *client;
    jack_status_t status = (jack_status_t)(JackNoSuchClient|JackFailure);

    jack_lock_graph( this );

    if( (client = client_internal_by_id( id )) ) {
	VERBOSE( this, "unloading client \"%s\"", client->control->name);
	if( client->control->type != ClientInternal ) {
	    status = (jack_status_t)(JackFailure|JackInvalidOption);
	}
	else {
	    remove_client_internal( client );
	    status = (jack_status_t)0;
	}
    }

    jack_unlock_graph( this );

    return status;
}

char * engine::get_reserved_name_( jack_uuid_t uuid )
{
    auto rnFinder = std::find_if( reserved_client_names.begin(),
				  reserved_client_names.end(),
				  [&uuid] ( jack_reserved_name_t * res ) {
				      return jack_uuid_compare( res->uuid, uuid ) == 0;
				  } );

    if( rnFinder != reserved_client_names.end() ) {
	jack_reserved_name_t * res = *rnFinder;
	char * retval = strdup( res->name );
	reserved_client_names.erase( rnFinder );
	free( res );
	return retval;
    }
    else {
	return 0;
    }
}

int engine::port_do_register( jack_request_t *req, int internal )
{
    jack_port_id_t port_id;
    jack_port_shared_t *shared;
    jack_port_internal_t *port;
    jack_client_internal_t *client;
    ssize_t i;
    char *backend_client_name;
    size_t len;

    for( i = 0; i < control->n_port_types; ++i ) {
	if( strcmp(req->x.port_info.type, control->port_types[i].type_name) == 0) {
	    break;
	}
    }

    if( i == control->n_port_types ) {
	jack_error( "cannot register a port of type \"%s\"",
		    req->x.port_info.type );
	return -1;
    }

    jack_lock_graph( this );
    if( (client = client_internal_by_id( req->x.port_info.client_id ) ) == nullptr ) {
	jack_error( "unknown client id in port registration request" );
	jack_unlock_graph( this );
	return -1;
    }

    if( (port = get_port_by_name_( req->x.port_info.name ) ) != nullptr ) {
	jack_error( "duplicate port name (%s) in port registration request", req->x.port_info.name );
	jack_unlock_graph( this );
	return -1;
    }

    if( (port_id = get_free_port_()) == (jack_port_id_t)-1 ) {
	jack_error( "no ports available!" );
	jack_unlock_graph( this );
	return -1;
    }

    shared = &control->ports[port_id];

    if( !internal || !driver ) {
	goto fallback;
    }

    /* if the port belongs to the backend client, do some magic with names 
     */

    backend_client_name = (char *)driver->internal_client->control->name;
    len = strlen(backend_client_name);

    if( strncmp( req->x.port_info.name, backend_client_name, len ) != 0 ) {
	goto fallback;
    }

    /* use backend's original as an alias, use predefined names */

    if( strcmp( req->x.port_info.type, JACK_DEFAULT_AUDIO_TYPE ) == 0 ) {
	if( (req->x.port_info.flags & (JackPortIsPhysical|JackPortIsInput)) == (JackPortIsPhysical|JackPortIsInput) ) {
	    snprintf( shared->name, sizeof(shared->name),
		      JACK_BACKEND_ALIAS ":playback_%d",
		      ++audio_out_cnt);
	    strcpy( shared->alias1, req->x.port_info.name );
	    goto next;
	} 
	else if( (req->x.port_info.flags & (JackPortIsPhysical|JackPortIsOutput)) == (JackPortIsPhysical|JackPortIsOutput) ) {
	    snprintf( shared->name, sizeof(shared->name),
		      JACK_BACKEND_ALIAS ":capture_%d",
		      ++audio_in_cnt);
	    strcpy( shared->alias1, req->x.port_info.name );
	    goto next;
	}
    }

#if 0 // do not do this for MIDI

    else if( strcmp( req->x.port_info.type, JACK_DEFAULT_MIDI_TYPE ) == 0 ) {
	if( (req->x.port_info.flags & (JackPortIsPhysical|JackPortIsInput)) == (JackPortIsPhysical|JackPortIsInput) ) {
	    snprintf( shared->name, sizeof(shared->name),
		      JACK_BACKEND_ALIAS ":midi_playback_%d",
		      ++midi_out_cnt );
	    strcpy( shared->alias1, req->x.port_info.name );
	    goto next;
	} 
	else if( (req->x.port_info.flags & (JackPortIsPhysical|JackPortIsOutput)) == (JackPortIsPhysical|JackPortIsOutput) ) {
	    snprintf( shared->name, sizeof(shared->name),
		      JACK_BACKEND_ALIAS ":midi_capture_%d",
		      ++midi_in_cnt);
	    strcpy( shared->alias1, req->x.port_info.name );
	    goto next;
	}
    }
#endif

  fallback:
    strcpy( shared->name, req->x.port_info.name );

  next:
    shared->ptype_id = control->port_types[i].ptype_id;
    jack_uuid_copy( &shared->client_id, req->x.port_info.client_id );
    shared->uuid = jack_port_uuid_generate( port_id );
    shared->flags = req->x.port_info.flags;
    shared->latency = 0;
    shared->capture_latency.min = shared->capture_latency.max = 0;
    shared->playback_latency.min = shared->playback_latency.max = 0;
    shared->monitor_requests = 0;

    port = &internal_ports[port_id];

    port->shared = shared;
    port->connections_vector.clear();
    port->buffer_info = NULL;
	
    if( port_assign_buffer( port ) ) {
	jack_error( "cannot assign buffer for port" );
	port_release( &internal_ports[port_id] );
	jack_unlock_graph( this );
	return -1;
    }

    client->ports_vector.push_back( port );
    if( client->control->active ) {
	port_registration_notify( port_id, TRUE);
    }
    jack_unlock_graph( this );

    VERBOSE( this, "registered port %s, offset = %u",
	     shared->name, (unsigned int)shared->offset );

    req->x.port_info.port_id = port_id;

    return 0;
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

int engine::port_do_unregister( jack_request_t *req )
{
    jack_client_internal_t *client;
    jack_port_shared_t *shared;
    jack_port_internal_t *port;
    jack_uuid_t uuid;

    if( req->x.port_info.port_id < 0 ||
	req->x.port_info.port_id > port_max) {
	jack_error( "invalid port ID %" PRIu32
		    " in unregister request",
		    req->x.port_info.port_id);
	return -1;
    }

    shared = &control->ports[req->x.port_info.port_id];

    if( jack_uuid_compare( shared->client_id, req->x.port_info.client_id ) != 0 ) {
	char buf[JACK_UUID_STRING_SIZE];
	jack_uuid_unparse( req->x.port_info.client_id, buf );
	jack_error( "Client %s is not allowed to remove port %s",
		    buf, shared->name );
	return -1;
    }

    jack_uuid_copy( &uuid, shared->uuid );

    jack_lock_graph( this );
    if( (client = client_internal_by_id( shared->client_id ) ) == nullptr ) {
	jack_error( "unknown client id in port registration request" );
	jack_unlock_graph( this );
	return -1;
    }

    port = &internal_ports[req->x.port_info.port_id];

    port_clear_connections( port );
    port_release( &internal_ports[req->x.port_info.port_id] );
	
    jack_client_remove_client_port( client->ports_vector, port );
    port_registration_notify( req->x.port_info.port_id, FALSE );
    jack_unlock_graph( this );

    return 0;
}

int engine::port_do_connect_( const char *source_port,
			      const char *destination_port )
{
    jack_connection_internal_t *connection;
    jack_port_internal_t *srcport, *dstport;
    jack_port_id_t src_id, dst_id;
    jack_client_internal_t *srcclient, *dstclient;

    if( (srcport = get_port_by_name_( source_port ) ) == nullptr ) {
	jack_error( "unknown source port in attempted connection [%s]",
		    source_port );
	return -1;
    }

    if( (dstport = get_port_by_name_( destination_port ) ) == nullptr ) {
	jack_error( "unknown destination port in attempted connection"
		    " [%s]", destination_port );
	return -1;
    }

    if( (dstport->shared->flags & JackPortIsInput) == 0 ) {
	jack_error( "destination port in attempted connection of"
		    " %s and %s is not an input port", 
		    source_port, destination_port );
	return -1;
    }

    if( (srcport->shared->flags & JackPortIsOutput) == 0 ) {
	jack_error( "source port in attempted connection of %s and"
		    " %s is not an output port",
		    source_port, destination_port );
	return -1;
    }

    if( srcport->shared->ptype_id != dstport->shared->ptype_id ) {
	jack_error( "ports used in attemped connection are not of "
		    "the same data type" );
	return -1;
    }

    if( (srcclient = client_internal_by_id( srcport->shared->client_id ) ) == 0 ) {
	jack_error( "unknown client set as owner of port - "
		    "cannot connect" );
	return -1;
    }
	
    if( !srcclient->control->active ) {
	jack_error( "cannot connect ports owned by inactive clients;"
		    " \"%s\" is not active", srcclient->control->name );
	return -1;
    }

    if( (dstclient = client_internal_by_id( dstport->shared->client_id ) ) == 0 ) {
	jack_error( "unknown client set as owner of port - cannot "
		    "connect" );
	return -1;
    }
	
    if( !dstclient->control->active ) {
	jack_error( "cannot connect ports owned by inactive clients;"
		    " \"%s\" is not active", dstclient->control->name );
	return -1;
    }

    for( jack_connection_internal_t * con : srcport->connections_vector ) {
	if( con->destination == dstport ) {
	    return EEXIST;
	}
    }

    connection = (jack_connection_internal_t *)malloc( sizeof(jack_connection_internal_t) );

    connection->source = srcport;
    connection->destination = dstport;
    connection->srcclient = srcclient;
    connection->dstclient = dstclient;

    src_id = srcport->shared->id;
    dst_id = dstport->shared->id;

    jack_lock_graph( this );

    if( dstport->connections_vector.size() > 0 && !dstport->shared->has_mixdown) {
	jack_port_type_info_t *port_type = port_type_info_( dstport );
	jack_error( "cannot make multiple connections to a port of"
		    " type [%s]", port_type->type_name );
	free( connection );
	jack_unlock_graph( this );
	return -1;
    }
    else {
	if( dstclient->control->type == ClientDriver ) {
	    /* Ignore output connections to drivers for purposes
	       of sorting. Drivers are executed first in the sort
	       order anyway, and we don't want to treat graphs
	       such as driver -> client -> driver as containing
	       feedback */

	    VERBOSE( this,
		     "connect %s and %s (output)",
		     srcport->shared->name,
		     dstport->shared->name );

	    connection->dir = 1;

	}
	else if( srcclient != dstclient ) {
	    srcclient->truefeeds_vector.push_back( dstclient );

	    dstclient->fedcount++;

	    if( jack_client_feeds_transitive( dstclient,
					      srcclient ) ||
		(dstclient->control->type == ClientDriver &&
		 srcclient->control->type != ClientDriver) ) {

		/* dest is running before source so
		   this is a feedback connection */

		VERBOSE( this,
			 "connect %s and %s (feedback)",
			 srcport->shared->name,
			 dstport->shared->name);

		dstclient->sortfeeds_vector.push_back( srcclient );

		connection->dir = -1;
		feedbackcount++;
		VERBOSE( this,
			 "feedback count up to %d",
			 feedbackcount );

	    }
	    else {
		/* this is not a feedback connection */

		VERBOSE( this, "connect %s and %s (forward)",
			 srcport->shared->name,
			 dstport->shared->name );

		srcclient->sortfeeds_vector.push_back( dstclient );

		connection->dir = 1;
	    }
	}
	else {
	    /* this is a connection to self */

	    VERBOSE( this,
		     "connect %s and %s (self)",
		     srcport->shared->name,
		     dstport->shared->name );

	    connection->dir = 0;
	}

	dstport->connections_vector.push_back( connection );
	srcport->connections_vector.push_back( connection );

	DEBUG( "actually sorted the graph..." );

	send_connection_notification_( srcport->shared->client_id,
				      src_id, dst_id, TRUE);

	send_connection_notification_( dstport->shared->client_id,
				      dst_id, src_id, TRUE);

	/* send a port connection notification just once to everyone who cares excluding clients involved in the connection */

	notify_all_port_interested_clients_( srcport->shared->client_id,
					     dstport->shared->client_id,
					     src_id,
					     dst_id,
					     1 );

	sort_graph_();
    }

    jack_unlock_graph( this );

    return 0;
}

int engine::port_do_disconnect_all_( jack_port_id_t port_id )
{
    if( port_id >= control->port_max ) {
	jack_error( "illegal port ID in attempted disconnection [%"
		    PRIu32 "]", port_id );
	return -1;
    }

    VERBOSE( this, "clear connections for %s",
	     internal_ports[port_id].shared->name );

    jack_lock_graph( this );
    port_clear_connections( &internal_ports[port_id] );
    sort_graph_();
    jack_unlock_graph( this );

    return 0;
}

int engine::port_do_disconnect_( const char *source_port,
				 const char *destination_port )
{
    jack_port_internal_t *srcport, *dstport;
    int ret = -1;

    if( (srcport = get_port_by_name_( source_port ) ) == nullptr ) {
	jack_error( "unknown source port in attempted disconnection"
		    " [%s]", source_port );
	return -1;
    }

    if( (dstport = get_port_by_name_( destination_port ) ) == nullptr ) {
	jack_error( "unknown destination port in attempted"
		    " disconnection [%s]", destination_port );
	return -1;
    }

    jack_lock_graph( this );

    ret = port_disconnect_internal_( srcport, dstport );

    jack_unlock_graph( this );

    return ret;
}

int engine::client_activate( jack_uuid_t id )
{
    jack_client_internal_t *client;
    int ret = -1;
    int i;
    jack_event_t event;

    VALGRIND_MEMSET( &event, 0, sizeof(event) );

    jack_lock_graph( this );

    if( (client = client_internal_by_id( id ) ) )
    {
	client->control->active = TRUE;

	jack_transport_activate( this, client );

	/* we call this to make sure the FIFO is
	 * built+ready by the time the client needs
	 * it. we don't care about the return value at
	 * this point.
	 */

	get_fifo_fd( ++external_client_cnt);
	sort_graph_();


	for( i = 0; i < control->n_port_types; ++i ) {
	    event.type = AttachPortSegment;
	    event.y.ptid = i;
	    deliver_event( client, &event );
	}

	event.type = BufferSizeChange;
	event.x.n = control->buffer_size;
	deliver_event( client, &event );

	// send delayed notifications for ports.
	for( jack_port_internal_t * port : client->ports_vector ) {
	    port_registration_notify( port->shared->id, TRUE );
	}

	ret = 0;
    }

    jack_unlock_graph( this );
    return ret;
}

int engine::client_deactivate( jack_uuid_t id )
{
    int ret = -1;

    jack_lock_graph( this );

    for( jack_client_internal_t * client : clients ) {
	if( jack_uuid_compare( client->control->uuid, id) == 0 ) {
	    for( jack_port_internal_t * port : client->ports_vector ) {
		port_clear_connections( port );
	    }

	    ret = client_do_deactivate_( client, TRUE );
	    break;
	}
    }

    jack_unlock_graph( this );

    return ret;
}	

int engine::do_get_port_connections( jack_request_t *req, int reply_fd )
{
    unsigned int i;
    int ret = -1;
    int internal = FALSE;

    jack_rdlock_graph( this );

    jack_port_internal_t *port = &internal_ports[req->x.port_info.port_id];

    DEBUG( "Getting connections for port '%s'.", port->shared->name );

    req->x.port_connections.nports = port->connections_vector.size();
    req->status = 0;

    /* figure out if this is an internal or external client */
    auto iFinder = std::find_if( clients.begin(),
				 clients.end(),
				 [&reply_fd] ( jack_client_internal_t * client ) {
				     return client->request_fd == reply_fd;
				 } );

    if( iFinder != clients.end() ) {
	internal = jack_client_is_internal( *iFinder );
    }

    if( !internal ) {
	if( write( reply_fd, req, sizeof(*req) ) < (ssize_t)sizeof(req) ) {
	    jack_error( "cannot write GetPortConnections result "
			"to client via fd = %d (%s)", 
			reply_fd, strerror(errno) );
	    goto out;
	}
    }
    else {
	req->x.port_connections.ports = (const char**)malloc(sizeof(char *)*req->x.port_connections.nports);
    }

    if( req->type == GetPortConnections ) {
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

	    if( internal ) {

		/* internal client asking for
		 * names. store in malloc'ed space,
		 * client frees
		 */
		char **ports = (char **) req->x.port_connections.ports;

		ports[i] = control->ports[port_id].name;

	    }
	    else {
		/* external client asking for
		 * names. we write the port id's to
		 * the reply fd.
		 */
		if( write( reply_fd, &port_id, sizeof(port_id) ) < (ssize_t)sizeof(port_id) ) {
		    jack_error( "cannot write port id to client" );
		    goto out;
		}
	    }
	}
    }

    ret = 0;

  out:
    req->status = ret;
    jack_unlock_graph( this );
    return ret;
}

static void* jack_engine_freewheel( void *arg )
{
    jack::engine * engine_ptr = (jack::engine *) arg;
    jack_client_internal_t* client;

    VERBOSE( engine_ptr, "freewheel thread starting ...");

    /* we should not be running SCHED_FIFO, so we don't 
       have to do anything about scheduling.
    */

    client = engine_ptr->client_internal_by_id( engine_ptr->fwclient );

    while( !engine_ptr->stop_freewheeling ) {

	engine_ptr->run_one_cycle( engine_ptr->control->buffer_size, 0.0f );

	if( client && client->error ) {
	    /* run one cycle() will already have told the server thread
	       about issues, and the server thread will clean up.
	       however, its time for us to depart this world ...
	    */
	    break;
	}
    }

    VERBOSE( engine_ptr, "freewheel came to an end, naturally");
    return 0;
}

int engine::do_start_freewheeling( jack_uuid_t client_id )
{
    jack_event_t event;
    jack_client_internal_t *client;

    if( freewheeling ) {
	return 0;
    }

    if( driver == nullptr ) {
	jack_error( "cannot start freewheeling without a driver!" );
	return -1;
    }

    /* stop driver before telling anyone about it so 
       there are no more process() calls being handled.
    */

    if( drivers_stop() ) {
	jack_error( "could not stop driver for freewheeling" );
	return -1;
    }

    client = client_internal_by_id( client_id );

    if( client->control->process_cbset || client->control->thread_cb_cbset ) {
	jack_uuid_copy( &fwclient, client_id );
    }

    freewheeling = 1;
    stop_freewheeling = 0;

    event.type = StartFreewheel;
    deliver_event_to_all( &event );
	
    if( jack_client_create_thread( nullptr, &freewheel_thread, 0, FALSE,
				   jack_engine_freewheel, this ) ) {
	jack_error( "could not start create freewheel thread" );
	return -1;
    }

    return 0;
}

/* handle client SetBufferSize request */
int engine::set_buffer_size_request( jack_nframes_t nframes )
{
    /* precondition: caller holds the request_lock */
    int rc;

    if( driver == NULL ) {
	return ENXIO;		/* no such device */
    }

    if( !jack_power_of_two(nframes) ) {
	jack_error( "buffer size %" PRIu32 " not a power of 2",
		    nframes );
	return EINVAL;
    }

    rc = driver->bufsize( driver, nframes );
    if( rc != 0 ) {
	jack_error( "driver does not support %" PRIu32
		    "-frame buffers", nframes );
    }
    return rc;
}

void engine::intclient_handle_request( jack_request_t *req )
{
    jack_client_internal_t *client;

    req->status = 0;
    if( (client = client_by_name( req->x.intclient.name) ) ) {
	jack_uuid_copy( &req->x.intclient.uuid, client->control->uuid );
    } else {
	req->status = (jack_status_t)(req->status | (JackNoSuchClient|JackFailure));
    }
}

void engine::intclient_name_request( jack_request_t *req )
{
    jack_client_internal_t *client;

    jack_rdlock_graph( this );
    if( (client = client_internal_by_id( req->x.intclient.uuid ) ) ) {
	strncpy( (char *)req->x.intclient.name, (char *)client->control->name,
		 sizeof(req->x.intclient.name) );
	req->status = 0;
    }
    else {
	req->status = (JackNoSuchClient|JackFailure);
    }
    jack_unlock_graph( this );
}

void engine::intclient_unload_request( jack_request_t *req )
{
    /* Called with the request_lock, but we need to call
     * jack_engine_handle_unload_client() *without* it. */

    if( !jack_uuid_empty( req->x.intclient.uuid ) ) {
	/* non-empty UUID */
	pthread_mutex_unlock( &request_lock );
	req->status = handle_unload_client_( req->x.intclient.uuid );
	pthread_mutex_lock( &request_lock );
    }
    else {
	VERBOSE( this, "invalid unload request");
	req->status = JackFailure;
    }
}

void engine::compute_port_total_latency_( jack_port_shared_t* port )
{
    if( port->in_use ) {
	port->total_latency = get_port_total_latency_( &internal_ports[port->id], 0,
						       !(port->flags & JackPortIsOutput) );
    }
}

void engine::client_fill_request_port_name_by_uuid_( jack_request_t *req )
{
    auto cFinder = std::find_if( clients.begin(),
				 clients.end(),
				 [&req] ( jack_client_internal_t * client ) {
				     return jack_uuid_compare( client->control->uuid, req->x.client_id ) == 0;
				 } );

    if( cFinder != clients.end() ) {
	jack_client_internal_t * client = *cFinder;
	snprintf( req->x.port_info.name, sizeof(req->x.port_info.name), "%s", client->control->name );
	req->status = 0;
    }
    else {
	req->status = -1;
    }
}

void engine::do_get_uuid_by_client_name_( jack_request_t *req )
{
    if( strcmp( req->x.name, "system" ) == 0 ) {
	/* request concerns the driver */
	if( driver ) {
	    jack_uuid_copy( &req->x.client_id, driver->internal_client->control->uuid );
	    req->status = 0;
	}
	else {
	    req->status = -1;
	}
	return;
    }

    auto cFinder = std::find_if( clients.begin(),
				 clients.end(),
				 [&req] ( jack_client_internal_t * client ) {
				     return strcmp( (const char*)client->control->name, req->x.name ) == 0;
				 } );

    if( cFinder != clients.end() ) {
	jack_client_internal_t * client = *cFinder;
	jack_uuid_copy( &req->x.client_id, client->control->uuid );
	req->status = 0;
    }
    else {
	req->status = -1;
    }
}

void engine::do_reserve_name_( jack_request_t *req )
{
    jack_reserved_name_t *reservation;

    auto nFinder = std::find_if( clients.begin(),
				 clients.end(),
				 [&req] ( jack_client_internal_t * client ) {
				     return !strcmp( (char*)client->control->name, req->x.reservename.name );
				 } );

    if( nFinder != clients.end() ) {
	req->status = -1;
	return;
    }

    reservation = (jack_reserved_name_t*)malloc( sizeof(jack_reserved_name_t) );
    if( reservation == nullptr ) {
	req->status = -1;
	return;
    }

    snprintf( reservation->name, sizeof(reservation->name), "%s", req->x.reservename.name );
    jack_uuid_copy( &reservation->uuid, req->x.reservename.uuid );
    reserved_client_names.push_back( reservation );

    req->status = 0;
}

void engine::do_session_reply_( jack_request_t *req )
{
    jack_uuid_t client_id;
    jack_client_internal_t *client;
    jack_uuid_t finalizer = JACK_UUID_EMPTY_INITIALIZER;

    jack_uuid_copy( &client_id, req->x.client_id );
    client = client_internal_by_id( client_id );
    jack_uuid_clear( &finalizer );

    req->status = 0;

    client->session_reply_pending = 0;

    if( session_reply_fd == -1 ) {
	jack_error( "spurious Session Reply" );
	return;
    }

    session_pending_replies -= 1;

    if( send_session_reply_( client ) ) {
	// maybe need to fix all client pendings.
	// but we will just get a set of spurious replies now.
	session_reply_fd = -1;
	return;
    }

    if( session_pending_replies == 0 ) {
	if( write( session_reply_fd, &finalizer, sizeof(finalizer)) < (ssize_t)sizeof(finalizer) ) {
	    jack_error( "cannot write SessionNotify result "
			"to client via fd = %d (%s)", 
			session_reply_fd, strerror(errno) );
	    req->status = -1;
	}
	session_reply_fd = -1;
    }
}

int engine::do_session_notify_( jack_request_t *req, int reply_fd )
{
    jack_event_t event;
  
    int reply;
    jack_uuid_t finalizer;
    struct stat sbuf;

    jack_uuid_clear( &finalizer );

    if( session_reply_fd != -1 ) {
	// we should have a notion of busy or somthing.
	// just sending empty reply now.
	goto send_final;
    }

    session_reply_fd = reply_fd;
    session_pending_replies = 0;

    event.type = SaveSession;
    event.y.n = req->x.session.type;
 	
    /* GRAPH MUST BE LOCKED : see callers of jack_engine_send_connection_notification() 
     */

    if( stat( req->x.session.path, &sbuf ) != 0 || !S_ISDIR( sbuf.st_mode ) ) {
	jack_error( "session parent directory (%s) does not exist", req->x.session.path );
	goto send_final;
    }

    for( jack_client_internal_t * client : clients ) {
	if( client->control->session_cbset ) {

	    // in case we only want to send to a special client.
	    // uuid assign is still complete. not sure if thats necessary.
	    if( (req->x.session.target[0] != 0) && strcmp(req->x.session.target, (char *)client->control->name) )
		continue;

	    /* the caller of jack_session_notify() is required to have created the session dir
	     */
                        
	    if( req->x.session.path[strlen(req->x.session.path)-1] == '/' ) {
		snprintf( event.x.name, sizeof(event.x.name), 
			  "%s%s/", req->x.session.path, client->control->name );
	    }
	    else {
		snprintf( event.x.name, sizeof(event.x.name),
			  "%s/%s/", req->x.session.path, client->control->name );
	    }
	    if( mkdir(event.x.name, 0777) != 0 ) {
		jack_error( "cannot create session directory (%s) for client %s: %s",
			    event.x.name, client->control->name, strerror(errno) );
		break;
	    }
	    reply = deliver_event( client, &event );

	    if( reply == 1 ) {
		// delayed reply
		session_pending_replies += 1;
		client->session_reply_pending = TRUE;
	    }
	    else if( reply == 2 ) {
		// immediate reply
		if( send_session_reply_( client ) ) {
		    goto error_out;
		}
	    }
	} 
    }

    if( session_pending_replies != 0 ) {
	return 0;
    }

  send_final:
    if( write( reply_fd, &finalizer, sizeof(finalizer)) < (ssize_t)sizeof(finalizer) ) {
	jack_error( "cannot write SessionNotify result "
		    "to client via fd = %d (%s)", 
		    reply_fd, strerror(errno) );
	goto error_out;
    }

    session_reply_fd = -1;
    return 0;
  error_out:
    return -3;
}

int engine::do_has_session_cb_( jack_request_t *req )
{
    jack_client_internal_t *client;
    int retval = -1;

    client = client_by_name( req->x.name );
    if( client == nullptr ) {
	goto out;
    }

    retval = client->control->session_cbset ? 1 : 0;
  out:
    return retval;
}

jack_port_internal_t * engine::get_port_by_name_( const char *name )
{
    jack_port_id_t id;

    /* Note the potential race on "in_use". Other design
       elements prevent this from being a problem.
    */

    for( id = 0; id < port_max; id++ ) {
	if( control->ports[id].in_use && jack_port_name_equals( &control->ports[id], name) ) {
	    return &internal_ports[id];
	}
    }

    return nullptr;
}

jack_port_id_t engine::get_free_port_()
{
    jack_port_id_t i;

    pthread_mutex_lock( &port_lock );

    for( i = 0; i < port_max; i++ ) {
	if( control->ports[i].in_use == 0 ) {
	    control->ports[i].in_use = 1;
	    break;
	}
    }
	
    pthread_mutex_unlock( &port_lock );
	
    if( i == port_max ) {
	return (jack_port_id_t)-1;
    }

    return i;
}

int engine::port_assign_buffer( jack_port_internal_t *port )
{
    jack_port_buffer_list_t *blist = port_buffer_list_( port );
    jack_port_buffer_info_t *bi;

    if( port->shared->flags & JackPortIsInput ) {
	port->shared->offset = 0;
	return 0;
    }
	
    pthread_mutex_lock( &blist->lock );

    if( blist->freelist_vector.size() == 0 ) {
	jack_port_type_info_t *port_type = port_type_info_( port );
	jack_error( "all %s port buffers in use!",
		    port_type->type_name );
	pthread_mutex_unlock( &blist->lock );
	return -1;
    }

    bi = blist->freelist_vector[0];
    buffer_list_remove_buffer_( blist->freelist_vector, bi );

    port->shared->offset = bi->offset;
    port->buffer_info = bi;

    pthread_mutex_unlock( &blist->lock );
    return 0;
}

jack_port_type_info_t * engine::port_type_info_( jack_port_internal_t *port )
{
    /* Returns a pointer to the port type information in the
       engine's shared control structure. 
    */
    return &control->port_types[port->shared->ptype_id];
}

int engine::drivers_stop()
{
    /* first stop the master driver */
    int retval = driver->stop( driver );

    /* now the slave drivers are stopped */
    for( jack_driver_t * sdriver : slave_drivers ) {
	sdriver->stop( sdriver );
    }

    return retval;
}

int engine::send_session_reply_( jack_client_internal_t *client )
{
    if( write( session_reply_fd, (const void *)&client->control->uuid, sizeof(client->control->uuid))
	< (ssize_t)sizeof(client->control->uuid) ) {
	jack_error( "cannot write SessionNotify result " 
		    "to client via fd = %d (%s)", 
		    session_reply_fd, strerror(errno) );
	return -1;
    }
    if( write( session_reply_fd, (const void *)client->control->name, sizeof(client->control->name))
	< (ssize_t)sizeof(client->control->name)) {
	jack_error( "cannot write SessionNotify result "
		    "to client via fd = %d (%s)", 
		    session_reply_fd, strerror(errno) );
	return -1;
    }
    if( write( session_reply_fd, (const void *)client->control->session_command, 
	       sizeof(client->control->session_command))
	< (ssize_t)sizeof(client->control->session_command)) {
	jack_error( "cannot write SessionNotify result "
		    "to client via fd = %d (%s)", 
		    session_reply_fd, strerror(errno) );
	return -1;
    }
    if( write( session_reply_fd, (const void *)(&client->control->session_flags), 
	       sizeof(client->control->session_flags))
	< (ssize_t)sizeof(client->control->session_flags)) {
	jack_error( "cannot write SessionNotify result "
		    "to client via fd = %d (%s)", 
		    session_reply_fd, strerror(errno) );
	return -1;
    }

    return 0;
}

void engine::buffer_list_remove_buffer_( std::vector<jack_port_buffer_info_t*> & buffer_list_free,
					jack_port_buffer_info_t * bi )
{
    auto bi_finder = std::find( buffer_list_free.begin(),
				buffer_list_free.end(),
				bi );

    if( bi_finder != buffer_list_free.end() ) {
	buffer_list_free.erase( bi_finder );
    }
    else {
	jack_error( "Failed to find buffer info to remove from buffer free list" );
    }
}

/* The driver invokes this callback both initially and whenever its
 * buffer size changes. 
 */
int engine::driver_set_buffer_size( jack_nframes_t nframes )
{
    int i;
    jack_event_t event;

    VERBOSE( this, "new buffer size %" PRIu32, nframes);

    control->buffer_size = nframes;
    if( driver ) {
	rolling_interval = jack_rolling_interval( driver->period_usecs );
    }

    for (i = 0; i < control->n_port_types; ++i) {
	if( resize_port_segment_( i, control->port_max) ) {
	    return -1;
	}
    }

    event.type = BufferSizeChange;
    event.x.n = control->buffer_size;
    deliver_event_to_all( &event );

    return 0;
}

int engine::resize_port_segment_( jack_port_type_id_t ptid,
				   unsigned long nports )
{
    jack_event_t event;
    jack_shmsize_t one_buffer;	/* size of one buffer */
    jack_shmsize_t size;		/* segment size */
    jack_port_type_info_t* port_type = &control->port_types[ptid];
    jack_shm_info_t* shm_info = &port_segment[ptid];

    one_buffer = jack_port_type_buffer_size( port_type, control->buffer_size );
    VERBOSE( this, "resizing port buffer segment for type %d, one buffer = %u bytes", ptid, one_buffer);

    size = nports * one_buffer;

    if( shm_info->attached_at == 0 ) {
	if( jack_shmalloc(size, shm_info) ) {
	    jack_error( "cannot create new port segment of %d"
			" bytes (%s)", 
			size,
			strerror(errno) );
	    return -1;
	}

	if( jack_attach_shm(shm_info) ) {
	    jack_error( "cannot attach to new port segment "
			"(%s)", strerror(errno) );
	    return -1;
	}

	control->port_types[ptid].shm_registry_index = shm_info->index;

    }
    else {

	/* resize existing buffer segment */
	if( jack_resize_shm(shm_info, size) ) {
	    jack_error( "cannot resize port segment to %d bytes,"
			" (%s)", size,
			strerror(errno) );
	    return -1;
	}
    }

    place_port_buffers_( ptid, one_buffer, size, nports, control->buffer_size );

#ifdef USE_MLOCK
    if( control->real_time ) {

	/* Although we've called mlockall(CURRENT|FUTURE), the
	 * Linux VM manager still allows newly allocated pages
	 * to fault on first reference.  This mlock() ensures
	 * that any new pages are present before restarting
	 * the process cycle.  Since memory locks do not
	 * stack, they can still be unlocked with a single
	 * munlockall().
	 */

	int rc = mlock( jack_shm_addr(shm_info), size );
	if( rc < 0 ) {
	    jack_error("JACK: unable to mlock() port buffers: "
		       "%s", strerror(errno));
	}
    }
#endif /* USE_MLOCK */

    /* Tell everybody about this segment. */
    event.type = AttachPortSegment;
    event.y.ptid = ptid;
    deliver_event_to_all( &event );

    /* XXX need to clean up in the evnt of failures */

    return 0;
}

void engine::place_port_buffers_( jack_port_type_id_t ptid,
				 jack_shmsize_t one_buffer,
				 jack_shmsize_t size,
				 unsigned long nports,
				 jack_nframes_t nframes )
{
    jack_shmsize_t offset;		/* shared memory offset */
    jack_port_buffer_info_t *bi;
    jack_port_buffer_list_t* pti = &port_buffers[ptid];
    jack_port_functions_t *pfuncs = jack_get_port_functions(ptid);

    pthread_mutex_lock( &pti->lock );
    offset = 0;
	
    if( pti->info ) {

	/* Buffer info array already allocated for this port
	 * type.  This must be a resize operation, so
	 * recompute the buffer offsets, but leave the free
	 * list alone.
	 */
	size_t i;

	bi = pti->info;
	while( offset < size ) {
	    bi->offset = offset;
	    offset += one_buffer;
	    ++bi;
	}

	/* update any existing output port offsets */
	for( i = 0; i < port_max; i++ ) {
	    jack_port_shared_t *port = &control->ports[i];
	    if( port->in_use &&
		(port->flags & JackPortIsOutput) &&
		port->ptype_id == ptid ) {
		bi = internal_ports[i].buffer_info;
		if( bi ) {
		    port->offset = bi->offset;
		}
	    }
	}

    }
    else {
	jack_port_type_info_t* port_type = &control->port_types[ptid];

	/* Allocate an array of buffer info structures for all
	 * the buffers in the segment.  Chain them to the free
	 * list in memory address order, offset zero must come
	 * first.
	 */
	bi = pti->info = (jack_port_buffer_info_t *)malloc(nports * sizeof(jack_port_buffer_info_t));

	while( offset < size ) {
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
	buffer_list_remove_buffer_( pti->freelist_vector, bi );
	port_type->zero_buffer_offset = bi->offset;
	if( ptid == JACK_AUDIO_PORT_TYPE ) {
	    silent_buffer = bi;
	}
    }
    /* initialize buffers */
    {
	size_t i;
	jack_shm_info_t *shm_info = &port_segment[ptid];
	char* shm_segment = (char *)jack_shm_addr(shm_info);

	bi = pti->info;
	for( i=0; i<nports; ++i, ++bi )
	    pfuncs->buffer_init( shm_segment + bi->offset, one_buffer, nframes );
    }

    pthread_mutex_unlock( &pti->lock );
}

int engine::run_cycle( jack::engine * engine_ptr, jack_nframes_t nframes, float delayed_usecs )
{
    jack_time_t now = engine_ptr->driver->last_wait_ust;
    jack_time_t dus = 0;
    jack_time_t p_usecs = engine_ptr->driver->period_usecs ;
    jack_nframes_t b_size = engine_ptr->control->buffer_size;
    jack_nframes_t left;
    jack_frame_timer_t* timer = &engine_ptr->control->frame_timer;

    if( engine_ptr->verbose ) {
	if( nframes != b_size ) {
	    VERBOSE( engine_ptr,
		     "late driver wakeup: nframes to process = %"
		     PRIu32 ".", nframes);
	}
    }

    /* Run as many cycles as it takes to consume nframes */

    for( left = nframes; left >= b_size; left -= b_size )
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

	if( timer->reset_pending ) {
	    // Adjust frame time after a discontinuity.
	    // The frames of the previous period.
	    timer->frames += b_size;
	    // The frames that were skipped.
	    dus = now - timer->next_wakeup; 
	    timer->frames += (dus * b_size) / p_usecs;
	}

	if( engine_ptr->first_wakeup || timer->reset_pending ) {
	    // First wakeup or after discontinuity.
	    // Initialiase the DLL.
	    timer->current_wakeup = now;
	    timer->next_wakeup = now + p_usecs;
	    timer->period_usecs = (float) p_usecs;
	    timer->filter_omega = timer->period_usecs * 7.854e-7f;
	    timer->initialized = 1;

	    // Reset both conditions.
	    engine_ptr->first_wakeup = 0;
	    timer->reset_pending = 0;
	}
	else {
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
	    timer->next_wakeup += (int64_t)floorf( timer->period_usecs + 1.41f * delta + 0.5f );
	}

	// maybe need a memory barrier here
	timer->guard2++;

	if( engine_ptr->run_one_cycle( b_size, delayed_usecs ) ) {
	    jack_error( "cycle execution failure, exiting" );
	    return EIO;
	}
    }

    return 0;
}

int engine::run_one_cycle( jack_nframes_t nframes, float delayed_usecs )
{
    int ret = -1;
    static int consecutive_excessive_delays = 0;

#define WORK_SCALE 1.0f

    if( !freewheeling && control->real_time && spare_usecs &&
	((WORK_SCALE * spare_usecs) <= delayed_usecs) ) {

	MESSAGE( "delay of %.3f usecs exceeds estimated spare"
		 " time of %.3f; restart ...\n",
		 delayed_usecs, WORK_SCALE * spare_usecs );
		
	if( ++consecutive_excessive_delays > 10 ) {
	    jack_error( "too many consecutive interrupt delays "
			"... engine pausing" );
	    return -1;	/* will exit the thread loop */
	}

	delay( this, delayed_usecs );
		
	return 0;

    }
    else {
	consecutive_excessive_delays = 0;
    }

    DEBUG( "trying to acquire read lock (FW = %d)", freewheeling );
    if( jack_try_rdlock_graph( this ) ) {
	VERBOSE( this, "lock-driven null cycle");
	if( !freewheeling ) {
	    driver->null_cycle( driver, nframes );
	}
	else {
	    /* don't return too fast */
	    usleep (1000);
	}
	return 0;
    }

    if( jack_trylock_problems( this ) ) {
	VERBOSE( this, "problem-lock-driven null cycle" );
	jack_unlock_graph( this );
	if( !freewheeling ) {
	    driver->null_cycle( driver, nframes );
	}
	else {
	    /* don't return too fast */
	    usleep (1000);
	}
	return 0;
    }

    if( problems || (timeout_count_threshold && ((size_t)timeout_count > (1 + timeout_count_threshold*1000/driver->period_usecs) ))) {
	VERBOSE( this, "problem-driven null cycle problems=%d", problems );
	jack_unlock_problems( this  );
	jack_unlock_graph( this );
	if( !freewheeling ) {
	    driver->null_cycle( driver, nframes );
	}
	else {
	    /* don't return too fast */
	    usleep (1000);
	}
	return 0;
    }

    jack_unlock_problems( this );
		
    if( !freewheeling ) {
	DEBUG( "waiting for driver read\n" );
	if( drivers_read_( nframes ) ) {
	    goto unlock;
	}
    }
	
    DEBUG( "run process\n" );

    if( process_( nframes ) != 0 ) {
	DEBUG( "engine process cycle failed" );
	check_client_status_();
    }
		
    if( !freewheeling ) {
	if( drivers_write_( nframes ) ) {
	    goto unlock;
	}
    }

    post_process_();

    if( delayed_usecs > control->max_delayed_usecs ) {
	control->max_delayed_usecs = delayed_usecs;
    }

    ret = 0;

  unlock:
    jack_unlock_graph( this );
    DEBUG("cycle finished, status = %d", ret);

    return ret;
}

int engine::drivers_read_( jack_nframes_t nframes )
{
    /* first read the slave drivers */
    for( jack_driver_t * sdriver : slave_drivers ) {
	sdriver->read( sdriver, nframes );
    }

    /* now the master driver is read */
    return driver->read( driver, nframes );
}

int engine::drivers_write_( jack_nframes_t nframes )
{
    /* first start the slave drivers */
    for( jack_driver_t * sdriver : slave_drivers ) {
	sdriver->write( sdriver, nframes );
    }

    /* now the master driver is written */
    return driver->write( driver, nframes );
}

int engine::process_( jack_nframes_t nframes )
{
    /* precondition: caller has graph_lock */

    process_errors = 0;

    for( jack_client_internal_t * client : clients ) {
	jack_client_control_t * ctl = client->control;
	ctl->state = NotTriggered;
	ctl->timed_out = 0;
	ctl->awake_at = 0;
	ctl->finished_at = 0;
    }

    vector<jack_client_internal_t*>::iterator current_iterator = clients.begin();
    vector<jack_client_internal_t*>::iterator end_marker = clients.end();
    for( ; process_errors == 0 && current_iterator != end_marker ; ) {
	jack_client_internal_t * client = *current_iterator;
	jack_client_control_t * ctl = client->control;

	DEBUG( "considering client %s for processing", ctl->name );

	if( !ctl->active ||
	    (!ctl->process_cbset && !ctl->thread_cb_cbset) ||
	    ctl->dead ) {
	    ++current_iterator;
	}
	else if( jack_client_is_internal( client ) ) {
	    current_iterator = process_internal_( current_iterator,
						  end_marker,
						  nframes );
	}
	else {
	    current_iterator = process_external_( current_iterator,
						  end_marker );
	}
    }

    return process_errors > 0;
}

int engine::check_client_status_()
{
    int err = 0;

    /* we are already late, or something else went wrong,
       so it can't hurt to check the existence of all
       clients.
    */
	
    for( jack_client_internal_t * client : clients ) {
	if( client->control->type == ClientExternal ) {
	    if( kill( client->control->pid, 0) ) {
		VERBOSE( this,
			 "client %s has died/exited",
			 client->control->name );
		client->error++;
		err++;
	    }
	    if( client->control->last_status != 0 ) {
		VERBOSE( this,
			 "client %s has nonzero process callback status (%d)\n",
			 client->control->name, client->control->last_status );
		client->error++;
		err++;
	    }
	}
		
	DEBUG( "client %s errors = %d", client->control->name,
	       client->error );
    }

    return err;
}

void engine::post_process_()
{
    /* precondition: caller holds the graph lock. */

    jack_transport_cycle_end( this );
    calc_cpu_load_();
    check_clients( 0 );
}

vector<jack_client_internal_t*>::iterator
engine::process_internal_( vector<jack_client_internal_t*>::iterator current_iterator,
			   vector<jack_client_internal_t*>::iterator end_marker,
			   jack_nframes_t nframes )
{
    jack_client_internal_t *client = *current_iterator;
    jack_client_control_t *ctl = client->control;

    /* internal client */

    DEBUG( "invoking an internal client's (%s) callbacks", ctl->name );
    ctl->state = Running;
    current_client = client;

    /* XXX how to time out an internal client? */

    if( ctl->sync_cb_cbset ) {
	jack_call_sync_client( client->private_client );
    }

    if( ctl->process_cbset )
    {
	if( client->private_client->process( nframes, client->private_client->process_arg ) ) {
	    jack_error( "internal client %s failed", ctl->name );
	    process_errors++;
	}
    }

    if( ctl->timebase_cb_cbset ) {
	jack_call_timebase_master( client->private_client );
    }

    ctl->state = Finished;

    if( process_errors ) {
	return end_marker;		/* will stop the loop */
    }
    else {
	return ++current_iterator;
    }
}

vector<jack_client_internal_t*>::iterator
engine::process_external_( vector<jack_client_internal_t*>::iterator current_iterator,
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

    current_client = client;

    DEBUG( "calling process() on an external subgraph, fd==%d",
	   client->subgraph_start_fd );

    if( write( client->subgraph_start_fd, &c, sizeof(c)) != sizeof(c) ) {
	jack_error( "cannot initiate graph processing (%s)",
		    strerror(errno) );
	process_errors++;
	signal_problems();
	return end_marker; /* will stop the loop */
    }

    then = jack_get_microseconds();

    if( freewheeling ) {
	poll_timeout_usecs = 250000; /* 0.25 seconds */
    }
    else {
	poll_timeout_usecs = (client_timeout_msecs > 0 ?
			      client_timeout_msecs * 1000 :
			      driver->period_usecs);
    }

  again:
    poll_timeout = 1 + poll_timeout_usecs / 1000;
    pfd[0].fd = client->subgraph_wait_fd;
    pfd[0].events = POLLERR|POLLIN|POLLHUP|POLLNVAL;

    DEBUG( "waiting on fd==%d for process() subgraph to finish (timeout = %d, period_usecs = %d)",
	   client->subgraph_wait_fd, poll_timeout, driver->period_usecs );

    if( (pollret = poll (pfd, 1, poll_timeout)) < 0 ) {
	jack_error( "poll on subgraph processing failed (%s)",
		    strerror(errno) );
	status = -1;
    }

    DEBUG( "\n\n\n\n\n back from subgraph poll, revents = 0x%x\n\n\n", pfd[0].revents );

    if( pfd[0].revents & ~POLLIN ) {
	jack_error( "subgraph starting at %s lost client",
		    client->control->name );
	status = -2;
    }

    if( pfd[0].revents & POLLIN ) {

	status = 0;

    }
    else if( status == 0 ) {

	/* no events, no errors, we woke up because poll()
	   decided that time was up ...
	*/

	if( freewheeling ) {
	    if( check_client_status_() ) {
		return end_marker;
	    }
	    else {
		/* all clients are fine - we're just not done yet. since
		   we're freewheeling, that is fine.
		*/
		goto again;
	    }
	}

#ifdef __linux
	if( linux_poll_bug_encountered( then, &poll_timeout_usecs) ) {
	    goto again;
	}

	if( poll_timeout_usecs < 200 ) {
	    VERBOSE( this, "FALSE WAKEUP skipped, remaining = %lld usec", poll_timeout_usecs );
	}
	else {
#endif
	    jack_error( "subgraph starting at %s timed out "
			"(subgraph_wait_fd=%d, status = %d, state = %s, pollret = %d revents = 0x%x)",
			client->control->name,
			client->subgraph_wait_fd, status,
			jack_client_state_name (client),
			pollret, pfd[0].revents );
	    status = 1;
#ifdef __linux
	}
#endif
    }

    now = jack_get_microseconds();

    if( status != 0 ) {
	VERBOSE( this, "at %" PRIu64
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
				    ctl->signalled_at): 0 );

	if( check_clients( 1 ) ) {
	    process_errors++;
	    return end_marker;		/* will stop the loop */
	}
    }
    else {
	timeout_count = 0;
    }


    DEBUG( "reading byte from subgraph_wait_fd==%d",
	   client->subgraph_wait_fd );

    if( read( client->subgraph_wait_fd, &c, sizeof(c) ) != sizeof (c)) {
	if( errno == EAGAIN ) {
	    jack_error( "pp: cannot clean up byte from graph wait "
			"fd (%s) - no data present" );
	}
	else {
	    jack_error( "pp: cannot clean up byte from graph wait "
			"fd (%s)", strerror(errno) );
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

void engine::calc_cpu_load_()
{
    jack_time_t cycle_end = jack_get_microseconds();
	
    /* store the execution time for later averaging */

    rolling_client_usecs[rolling_client_usecs_index++] = cycle_end -
	control->current_time.usecs;

    //jack_info ("cycle_end - control->current_time.usecs %ld",
    //	(long) (cycle_end - control->current_time.usecs));

    if( rolling_client_usecs_index >= JACK_ENGINE_ROLLING_COUNT ) {
	rolling_client_usecs_index = 0;
    }

    /* every so often, recompute the current maximum use over the
       last JACK_ENGINE_ROLLING_COUNT client iterations.
    */

    if( ++rolling_client_usecs_cnt % rolling_interval == 0 ) {
	float rmax_usecs = 0.0f;
	int i;

	for( i = 0; i < JACK_ENGINE_ROLLING_COUNT; i++ ) {
	    if( rolling_client_usecs[i] > rmax_usecs ) {
		rmax_usecs = rolling_client_usecs[i];
	    }
	}

	if( rmax_usecs > max_usecs) {
	    max_usecs = rmax_usecs;
	}

	if( rmax_usecs < driver->period_usecs ) {
	    spare_usecs = driver->period_usecs - rmax_usecs;
	}
	else {
	    spare_usecs = 0;
	}

	control->cpu_load = (1.0f - (spare_usecs / driver->period_usecs)) * 50.0f
	    + (control->cpu_load * 0.5f);

	VERBOSE( this, "load = %.4f max usecs: %.3f, spare = %.3f",
		 control->cpu_load,
		 max_usecs,
		 spare_usecs);
    }
}

int engine::check_clients( int with_timeout_check )
{
    /* CALLER MUST HOLD graph read lock */
    int errs = 0;

    for( jack_client_internal_t * client : clients ) {

	if( client->error ) {
	    VERBOSE( this, "client %s already marked with error = %d\n",
		     client->control->name, client->error);
	    errs++;
	    continue;
	}

	if( with_timeout_check ) {

	    /* we can only consider the timeout a client error if
	     * it actually woke up.  its possible that the kernel
	     * scheduler screwed us up and never woke up the
	     * client in time. sigh.
	     */
			
	    VERBOSE( this, "checking client %s: awake at %" PRIu64 " finished at %" PRIu64, 
		     client->control->name,
		     client->control->awake_at,
		     client->control->finished_at);
			
	    if( client->control->awake_at > 0 ) {
		if( client->control->finished_at == 0 ) {
		    jack_time_t now = jack_get_microseconds();

		    if( (now - client->control->awake_at) < driver->period_usecs ) {
			/* we give the client a bit of time, to finish the cycle
			 * we assume here, that we dont get signals delivered to this thread.
			 */
			struct timespec wait_time;
			wait_time.tv_sec = 0;
			wait_time.tv_nsec = (driver->period_usecs - (now - client->control->awake_at)) * 1000;
			VERBOSE( this, "client %s seems to have timed out. we may have mercy of %d ns.",
				 client->control->name, (int)wait_time.tv_nsec );
			nanosleep( &wait_time, nullptr );
		    }

		    if( client->control->finished_at == 0 ) {
			client->control->timed_out++;
			client->error++;
			errs++;
			VERBOSE( this, "client %s has timed out", client->control->name);
		    }
		    else {
			/*
			 * the client recovered. if this is a single occurence, thats probably fine.
			 * however, we increase the continuous_stream flag.
			 */
			timeout_count += 1;
		    }
		}
	    }
	}
    }
		
    if( errs ) {
	signal_problems();
    }

    return errs;
}

void engine::wake_server_thread_()
{
    char c = 0;
    /* we don't actually care if this fails */
    VERBOSE( this, "waking server thread" );
    write( cleanup_fifo[1], &c, 1 );
}

/* The driver invokes this callback both initially and whenever its
 * buffer size changes. 
 */
int engine::driver_buffer_size( jack::engine * engine_ptr, jack_nframes_t nframes )
{
    int i;
    jack_event_t event;

    VERBOSE( engine_ptr, "new buffer size %" PRIu32, nframes);

    engine_ptr->control->buffer_size = nframes;
    if( engine_ptr->driver ) {
	engine_ptr->rolling_interval = jack_rolling_interval( engine_ptr->driver->period_usecs );
    }

    for( i = 0; i < engine_ptr->control->n_port_types; ++i ) {
	if( engine_ptr->resize_port_segment_( i, engine_ptr->control->port_max ) ) {
	    return -1;
	}
    }

    event.type = BufferSizeChange;
    event.x.n = engine_ptr->control->buffer_size;
    engine_ptr->deliver_event_to_all( &event );

    return 0;
}

/* driver callback */
int engine::set_sample_rate( jack::engine * engine_ptr, jack_nframes_t nframes )
{
    jack_control_t *ectl = engine_ptr->control;

    ectl->current_time.frame_rate = nframes;
    ectl->pending_time.frame_rate = nframes;

    return 0;
}

void engine::delay( jack::engine * engine_ptr, float delayed_usecs )
{
    jack_event_t event;
	
    engine_ptr->control->frame_timer.reset_pending = 1;

    engine_ptr->control->xrun_delayed_usecs = delayed_usecs;

    if( delayed_usecs > engine_ptr->control->max_delayed_usecs ) {
	engine_ptr->control->max_delayed_usecs = delayed_usecs;
    }

    event.type = XRun;

    engine_ptr->deliver_event_to_all( &event );
}

void engine::driver_exit( engine * engine_ptr )
{
    jack_driver_t* driver = engine_ptr->driver;

    VERBOSE( engine_ptr, "stopping driver");
    driver->stop( driver );
    VERBOSE( engine_ptr, "detaching driver");
    driver->detach( driver, engine_ptr );

    /* tell anyone waiting that the driver exited. */
    kill( engine_ptr->wait_pid, SIGUSR2 );
	
    engine_ptr->driver = NULL;
}

void engine::driver_unload( jack_driver_t *driver )
{
    void* handle = driver->handle;
    driver->finish (driver);
    dlclose (handle);
}

}
