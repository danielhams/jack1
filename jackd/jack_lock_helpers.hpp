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

#ifndef JACK_LOCK_HELPERS_HPP
#define JACK_LOCK_HELPERS_HPP

#define jack_rdlock_graph(e) { DEBUG ("acquiring graph read lock"); if (pthread_rwlock_rdlock (&e->client_lock)) abort(); }
#define jack_lock_graph(e) { DEBUG ("acquiring graph write lock"); if (pthread_rwlock_wrlock (&e->client_lock)) abort(); }
#define jack_try_rdlock_graph(e) pthread_rwlock_tryrdlock (&e->client_lock)
#define jack_unlock_graph(e) { DEBUG ("release graph lock"); if (pthread_rwlock_unlock (&e->client_lock)) abort(); }

#define jack_trylock_problems(e) pthread_mutex_trylock (&e->problem_lock)
#define jack_lock_problems(e) { DEBUG ("acquiring problem lock"); if (pthread_mutex_lock (&e->problem_lock)) abort(); }
#define jack_unlock_problems(e) { DEBUG ("release problem lock"); if (pthread_mutex_unlock (&e->problem_lock)) abort(); }

#endif
