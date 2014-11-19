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

#include "jack_signals.hpp"

namespace jack
{

static void do_nothing_handler (int sig)
{
    /* this is used by the child (active) process, but it never
       gets called unless we are already shutting down after
       another signal.
    */
    char buf[64];
    snprintf (buf, sizeof(buf),
	      "received signal %d during shutdown (ignored)\n", sig);
    write (1, buf, strlen (buf));
}

sigset_t jack_signals_create()
{
    sigset_t signals;

    /* ensure that we are in our own process group so that
       kill (SIG, -pgrp) does the right thing.
    */

    setsid();

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    /* what's this for?

       POSIX says that signals are delivered like this:

       * if a thread has blocked that signal, it is not
       a candidate to receive the signal.
       * of all threads not blocking the signal, pick
       one at random, and deliver the signal.

       this means that a simple-minded multi-threaded program can
       expect to get POSIX signals delivered randomly to any one
       of its threads,

       here, we block all signals that we think we might receive
       and want to catch. all "child" threads will inherit this
       setting. if we create a thread that calls sigwait() on the
       same set of signals, implicitly unblocking all those
       signals. any of those signals that are delivered to the
       process will be delivered to that thread, and that thread
       alone. this makes cleanup for a signal-driven exit much
       easier, since we know which thread is doing it and more
       importantly, we are free to call async-unsafe functions,
       because the code is executing in normal thread context
       after a return from sigwait().
    */

    sigemptyset(&signals);
    sigaddset(&signals, SIGHUP);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGQUIT);
    sigaddset(&signals, SIGPIPE);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGUSR1);
    sigaddset(&signals, SIGUSR2);

    /* all child threads will inherit this mask unless they
     * explicitly reset it
     */

    pthread_sigmask(SIG_BLOCK, &signals, 0);

    return signals;
}

sigset_t jack_signals_block()
{
    sigset_t signals;
    sigset_t oldsignals;

    sigemptyset(&signals);
    sigaddset(&signals, SIGHUP);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGQUIT);
    sigaddset(&signals, SIGPIPE);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGUSR1);
    sigaddset(&signals, SIGUSR2);

    pthread_sigmask(SIG_BLOCK, &signals, &oldsignals);

    return oldsignals;
}

void jack_signals_unblock( sigset_t & signals )
{
    pthread_sigmask( SIG_UNBLOCK, &signals, 0 );
}

void jack_signals_install_do_nothing_action( sigset_t & signals )
{
    /* install a do-nothing handler because otherwise pthreads
       behaviour is undefined when we enter sigwait.
    */

    struct sigaction action;
    sigset_t all_signals;
    int i;

    sigfillset( &all_signals );
    action.sa_handler = do_nothing_handler;
    action.sa_mask = all_signals;
    action.sa_flags = SA_RESTART|SA_RESETHAND;

    for( i = 1; i < NSIG; i++ ) {
	if( sigismember( &signals, i ) ) {
	    sigaction( i, &action, 0 );
	} 
    }
}

int jack_signals_wait( sigset_t & signals, jack::engine * engine )
{
    int sig;
    bool waiting = true;

    while( waiting ) {
	sigwait( &signals, &sig );

	jack_info( "jack main caught signal %d", sig );

	switch( sig ) {
	    case SIGUSR1:
		if( engine != nullptr ) {
                    engine->dump_configuration( 1 );
		}
		break;
	    case SIGUSR2:
		/* driver exit */
		waiting = false;
		break;
	    default:
		waiting = false;
		break;
	}
    }
    return sig;
}

}
