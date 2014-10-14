/*
    (Original file engine.h)
    Copyright (C) 2001-2003 Paul Davis
	(Modifications for C++ engine.hpp)
	Copyright (C) 2014 Daniel Hams
	    
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
#ifndef __jack_engine_hpp__
#define __jack_engine_hpp__

#include <jack/jack.h>
#include <internal.hpp>
#include <driver_interface.hpp>
#include <driver.hpp>

#include <vector>
#include <memory>
#include <string>

struct _jack_driver;
struct _jack_client_internal;
struct _jack_port_internal;

/* Structures is allocated by the engine in local memory to keep track
 * of port buffers and connections.
 */
typedef struct {
    jack_shm_info_t* shm_info;
    jack_shmsize_t   offset;
} jack_port_buffer_info_t;

/* The engine keeps an array of these in its local memory. */
typedef struct _jack_port_internal {
    struct _jack_port_shared *shared;
    JSList                   *connections;
    jack_port_buffer_info_t  *buffer_info;
} jack_port_internal_t;

/* The engine's internal port type structure. */
typedef struct _jack_port_buffer_list {
    pthread_mutex_t          lock;	/* only lock within server */
    JSList	            *freelist;	/* list of free buffers */
    jack_port_buffer_info_t *info;	/* jack_buffer_info_t array */
} jack_port_buffer_list_t;

typedef struct _jack_reserved_name {
    jack_uuid_t uuid;
    char name[JACK_CLIENT_NAME_SIZE];
} jack_reserved_name_t;

#define JACKD_WATCHDOG_TIMEOUT 10000
#define JACKD_CLIENT_EVENT_TIMEOUT 2000

/* The main engine structure in local memory. */
struct _jack_engine {
    jack_control_t        *control;

    std::vector<jack_driver_desc_t*> drivers;
    struct _jack_driver   *driver;
    jack_driver_desc_t    *driver_desc;
    JSList                *driver_params;

    std::vector<jack_driver_t*> slave_drivers;

    /* these are "callbacks" made by the driver backend */
    int  (*set_buffer_size) (struct _jack_engine *, jack_nframes_t frames);
    int  (*set_sample_rate) (struct _jack_engine *, jack_nframes_t frames);
    int  (*run_cycle)	    (struct _jack_engine *, jack_nframes_t nframes,
			     float delayed_usecs);
    void (*delay)	    (struct _jack_engine *, float delayed_usecs);
    void (*transport_cycle_start) (struct _jack_engine *, jack_time_t time);
    void (*driver_exit)     (struct _jack_engine *);
    jack_time_t (*get_microseconds)(void);
    /* "private" sections starts here */

    /* engine serialization -- use precedence for deadlock avoidance */
    pthread_mutex_t request_lock; /* precedes client_lock */
    pthread_rwlock_t client_lock;
    pthread_mutex_t port_lock;
    pthread_mutex_t problem_lock; /* must hold write lock on client_lock */
    int		    process_errors;
    int		    period_msecs;

    /* Time to wait for clients in msecs.  Used when jackd is run
     * without realtime priority enabled. */
    int		    client_timeout_msecs;

    /* info on the shm segment containing this->control */

    jack_shm_info_t control_shm;

    /* address-space local port buffer and segment info,
       indexed by the port type_id
    */
    jack_port_buffer_list_t port_buffers[JACK_MAX_PORT_TYPES];
    jack_shm_info_t         port_segment[JACK_MAX_PORT_TYPES];

    unsigned int    port_max;
    pthread_t	    server_thread;

    int		    fds[2];
    int		    cleanup_fifo[2];
    size_t	    pfd_size;
    size_t	    pfd_max;
    struct pollfd  *pfd;
    char	    fifo_prefix[PATH_MAX+1];
    int		   *fifo;
    unsigned long   fifo_size;

    /* session handling */
    int		    session_reply_fd;
    int		    session_pending_replies;

    unsigned long   external_client_cnt;
    int		    rtpriority;
    volatile char   freewheeling;
    volatile char   stop_freewheeling;
    jack_uuid_t     fwclient;
    pthread_t       freewheel_thread;
    char	    verbose;
    char	    do_munlock;
    const char	   *server_name;
    char	    temporary;
    int		    reordered;
    int		    feedbackcount;
    int             removing_clients;
    pid_t           wait_pid;
    int             nozombies;
    int             timeout_count_threshold;
    volatile int    problems;
    volatile int    timeout_count;
    volatile int    new_clients_allowed;

    /* these lists are protected by `client_lock' */
    JSList	   *clients;
    JSList	   *clients_waiting;
    JSList	   *reserved_client_names;

    std::vector<jack_port_internal_t> internal_ports;
    jack_client_internal_t  *timebase_client;
    jack_port_buffer_info_t *silent_buffer;
    jack_client_internal_t  *current_client;

#define JACK_ENGINE_ROLLING_COUNT 32
#define JACK_ENGINE_ROLLING_INTERVAL 1024

    jack_time_t rolling_client_usecs[JACK_ENGINE_ROLLING_COUNT];
    int		    rolling_client_usecs_cnt;
    int		    rolling_client_usecs_index;
    int		    rolling_interval;
    float	    max_usecs;
    float	    spare_usecs;

    int first_wakeup;

#ifdef JACK_USE_MACH_THREADS
    /* specific resources for server/client real-time thread communication */
    mach_port_t servertask, bp;
    int portnum;
#endif

    /* used for port names munging */
    int audio_out_cnt;
    int audio_in_cnt;
    int midi_out_cnt;
    int midi_in_cnt;
};

/* public functions */
/*

// D Hams
// This is stuff I'm moving into jack_engine.[h|c]pp

jack_engine_t  *jack_engine_new (int real_time, int real_time_priority,
				 int do_mlock, int do_unlock,
				 const char *server_name, int temporary,
				 int verbose, int client_timeout,
				 unsigned int port_max,
                                 pid_t waitpid, jack_nframes_t frame_time_offset, int nozombies,
				 int timeout_count_threshold,
				 JSList *drivers);

void		jack_engine_delete (jack_engine_t *);

int		jack_engine_load_driver (jack_engine_t *engine,
					 jack_driver_desc_t * driver_desc,
					 JSList * driver_params);
int		jack_engine_load_slave_driver (jack_engine_t *engine,
					       jack_driver_desc_t * driver_desc,
					       JSList * driver_params);
int jack_drivers_start (jack_engine_t *engine);

int jack_use_driver (jack_engine_t *engine, struct _jack_driver *driver);

void jack_engine_sort_graph (jack_engine_t *engine);

int jack_engine_deliver_event (jack_engine_t *, jack_client_internal_t *, const jack_event_t *, ...);


// private engine functions (are used by clients)
void		jack_engine_reset_rolling_usecs (jack_engine_t *engine);
int		internal_client_request (void* ptr, jack_request_t *request);
int		jack_get_fifo_fd (jack_engine_t *engine,
				  unsigned int which_fifo);
*/

#endif
