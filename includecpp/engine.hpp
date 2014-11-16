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

constexpr const int32_t JACKD_WATCHDOG_TIMEOUT = 10000;
constexpr const int32_t JACKD_CLIENT_EVENT_TIMEOUT = 2000;

constexpr const int32_t JACK_ENGINE_ROLLING_COUNT = 32;
constexpr const int32_t JACK_ENGINE_ROLLING_INTERVAL = 1024;

/* Structures allocated by the engine in local memory to keep track
 * of port buffers and connections.
 */

/* The engine's internal port type structure. */
typedef struct _jack_port_buffer_list {
    pthread_mutex_t          lock;	/* only lock within server */
    std::vector<jack_port_buffer_info_t*> freelist_vector; /* list of free buffers */
    jack_port_buffer_info_t *info;	/* jack_buffer_info_t array */
} jack_port_buffer_list_t;

typedef struct _jack_reserved_name {
    jack_uuid_t uuid;
    char name[JACK_CLIENT_NAME_SIZE];
} jack_reserved_name_t;

typedef jack_driver_t * (*jack_driver_info_init_callback_t)(jack_client_t*, const JSList *);
typedef void (*jack_driver_info_finish_callback_t)(jack_driver_t*);

typedef struct _jack_driver_info {
    jack_driver_info_init_callback_t initialize;
    jack_driver_info_finish_callback_t finish;
    char           (*client_name);
    dlhandle       handle;
} jack_driver_info_t;

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
    int  (*run_cycle)	    (struct _jack_engine *, jack_nframes_t nframes, float delayed_usecs);
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

    /* these are protected by `client_lock' */
    std::vector<jack_client_internal_t*> clients;
    std::vector<jack_reserved_name_t*> reserved_client_names;

    // Actually fixed size based on options
    std::vector<jack_port_internal_t> internal_ports;
    jack_client_internal_t  *timebase_client;
    jack_port_buffer_info_t *silent_buffer;
    jack_client_internal_t  *current_client;

    jack_time_t rolling_client_usecs[JACK_ENGINE_ROLLING_COUNT];
    int		    rolling_client_usecs_cnt;
    int		    rolling_client_usecs_index;
    int		    rolling_interval;
    float	    max_usecs;
    float	    spare_usecs;

    int first_wakeup;

    /* used for port names munging */
    int audio_out_cnt;
    int audio_in_cnt;
    int midi_out_cnt;
    int midi_in_cnt;
};

namespace jack
{

struct engine;

struct engine {
    typedef int (*set_buffer_size_callback)(engine *, jack_nframes_t frames);
    typedef int (*set_sample_rate_callback)(engine *, jack_nframes_t frames);
    typedef int (*run_cycle_callback)      (engine *, jack_nframes_t nframes, float delayed_usecs);
    typedef void (*delay_callback)         (engine *, float delayed_usecs);
    typedef void (*transport_cycle_start_callback) (engine *, jack_time_t time);
    typedef void (*driver_exit_callback)   (engine *);
    typedef jack_time_t (*get_microseconds_callback)(void);

    jack_control_t        *control;

    std::vector<jack_driver_desc_t*> drivers;
    struct _jack_driver   *driver;
    jack_driver_desc_t    *driver_desc;
    JSList                *driver_params;

    std::vector<jack_driver_t*> slave_drivers;

    /* these are "callbacks" made by the driver backend */
    set_buffer_size_callback set_buffer_size_c;
    set_sample_rate_callback set_sample_rate_c;
    run_cycle_callback run_cycle_c;
    delay_callback delay_c;
    transport_cycle_start_callback transport_cycle_start_c;
    driver_exit_callback driver_exit_c;
    get_microseconds_callback get_microseconds_c;

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
    bool            realtime;
    int		    rtpriority;
    volatile bool   freewheeling;
    volatile bool   stop_freewheeling;
    jack_uuid_t     fwclient;
    pthread_t       freewheel_thread;
    bool	    verbose;
    bool            memory_locked;
    bool	    do_munlock;
    const std::string server_name;
    bool	    temporary;
    int		    reordered;
    int		    feedbackcount;
    bool            removing_clients;
    pid_t           wait_pid;
    bool            nozombies;
    int             timeout_count_threshold;
    int             frame_time_offset;
    volatile int    problems;
    volatile int    timeout_count;
    volatile bool   new_clients_allowed;

    /* these are protected by `client_lock' */
    std::vector<jack_client_internal_t*> clients;
    std::vector<jack_reserved_name_t*> reserved_client_names;

    // Actually fixed size based on options
    std::vector<jack_port_internal_t> internal_ports;
    jack_client_internal_t  *timebase_client;
    jack_port_buffer_info_t *silent_buffer;
    jack_client_internal_t  *current_client;

    jack_time_t rolling_client_usecs[JACK_ENGINE_ROLLING_COUNT];
    int		    rolling_client_usecs_cnt;
    int		    rolling_client_usecs_index;
    int		    rolling_interval;
    float	    max_usecs;
    float	    spare_usecs;

    bool first_wakeup;

    /* used for port names munging */
    int audio_out_cnt;
    int audio_in_cnt;
    int midi_out_cnt;
    int midi_in_cnt;

    engine(
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
	const std::vector<jack_driver_desc_t*> & loaded_drivers );

    int init();

    void signal_problems();
    int linux_poll_bug_encountered( jack_time_t then, jack_time_t * required );

    void cleanup();

    void deliver_event_to_all( jack_event_t * event );
    int deliver_event( jack_client_internal_t * client,
		       const jack_event_t * event,
		       ... );

    int drivers_start();
    int load_driver( jack_driver_desc_t * driver_desc,
		     JSList * driver_params_jsl );
    int use_driver( jack_driver_t * driver );
    int load_slave_driver( jack_driver_desc_t * driver_desc,
			   JSList * driver_params_jsl );
    int add_slave_driver( jack_driver_t * sdriver );
    int unload_slave_driver( jack_driver_t * sdriver );
    void slave_driver_remove( jack_driver_t * sdriver );

    void property_change_notify( jack_property_change_t change,
				 jack_uuid_t uuid,
				 const char * key );

    void client_registration_notify( const char * name, int yn );

    jack_client_internal_t * setup_client(
	ClientType type, char *name, jack_uuid_t uuid,
	jack_options_t options, jack_status_t *status,
	int client_fd, const char *object_path, const char *object_data);
    jack_client_t * internal_client_alloc( jack_client_control_t *cc );
    void client_internal_delete( jack_client_internal_t * client );
    jack_client_internal_t * create_driver_client_internal( char *name );
    void remove_client_internal( jack_client_internal_t *client );
    jack_client_internal_t * client_internal_by_id( jack_uuid_t id );
    jack_client_internal_t * client_by_name( const char * name );
    int client_name_reserved( const char * name );
    int generate_unique_name( char *name );

    int do_stop_freewheeling( int engine_exiting );

    ~engine();

    void reset_rolling_usecs();
    int set_sample_rate( jack_nframes_t nframes );
    int get_fifo_fd( unsigned int which_fifo );
    void transport_init();

    void port_clear_connections( jack_port_internal_t * port );
    void port_registration_notify( jack_port_id_t port_id, int yn );
    void port_release( jack_port_internal_t *port );

    int rechain_graph();

private:
    jack_driver_info_t * load_driver_so_( jack_driver_desc_t * driver_desc );
    int load_client_( jack_client_internal_t *client,
		      const char *so_name );
    void client_internal_unload_( jack_client_internal_t * client );

    void zombify_client_( jack_client_internal_t * client );
    jack_client_internal_t * setup_client_control_(
	int fd, ClientType type, const char *name, jack_uuid_t uuid );

    void client_disconnect_ports_( jack_client_internal_t *client );
    int client_do_deactivate_( jack_client_internal_t *client,
			       int sort_graph );
    void client_truefeed_remove_(
	std::vector<jack_client_internal_t*> & truefeeds,
	jack_client_internal_t * client_to_remove );
    void client_sortfeed_remove_(
	std::vector<jack_client_internal_t*> & sortfeeds,
	jack_client_internal_t * client_to_remove );

    int client_name_invalid_( char *name, jack_options_t options,
			      jack_status_t *status );

    jack_port_buffer_list_t * port_buffer_list_( jack_port_internal_t *port );
    int port_disconnect_internal_( jack_port_internal_t *srcport,
				   jack_port_internal_t *dstport );
    void port_connection_remove_(
	std::vector<jack_connection_internal_t*> & connections,
	jack_connection_internal_t * to_remove );
    void compute_all_port_total_latencies_();
    jack_nframes_t get_port_total_latency_(
	jack_port_internal_t *port, int hop_count, int toward_port );

    void sort_graph_();
    void check_acyclic_();
    void compute_new_latency_();

    int send_connection_notification_(
	jack_uuid_t client_id,
	jack_port_id_t self_id,
	jack_port_id_t other_id,
	int connected );

    void notify_all_port_interested_clients_(
	jack_uuid_t src,
	jack_uuid_t dst,
	jack_port_id_t a,
	jack_port_id_t b,
	int connected );

    void clear_fifos_();

    void ensure_uuid_unique_( jack_uuid_t uuid );
};

}
#endif
