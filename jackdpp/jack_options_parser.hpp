/*
    Copyright (C) 2014- Daniel Hams
    
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
#ifndef JACK_OPTIONS_PARSER_HPP
#define JACK_OPTIONS_PARSER_HPP

#include <string>
#include <vector>

#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-local-typedefs"
#endif
#include <boost/program_options.hpp>
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#pragma GCC diagnostic pop
#endif

namespace jack
{
struct jack_options {
	jack_options();
	std::vector<std::string> internal_clients;
	int clock_source;
	int timeout_threshold;
	std::string driver;
	int frame_time_offset;
	bool memory_locked;
	int midi_buffer_size;
	std::string server_name;
	bool sanity_checks;
	int port_max;
	bool replace_registry;
	bool realtime;
	int realtime_priority;
	bool silent;
	bool synchronous;
	bool temporary;
	bool show_temporary;
	int client_timeout;
	bool unlock_memory;
	bool verbose;
	bool show_help;
	bool show_version;
	std::vector<std::string> slave_drivers;
	bool no_zombies;

	bool success;
	std::string error_message;
};

class jack_options_parser {
public:
	jack_options_parser( int argc, char ** argv, bool debug = false );

	inline jack_options & get_parsed_options() { return options_; };
	inline int get_driver_argc() { return driver_argc_; };
	inline char ** get_driver_argv() { return &driver_argv_[0]; };
	void display_usage();

private:
	boost::program_options::options_description all_options_;
	boost::program_options::options_description visible_options_;
	boost::program_options::options_description hidden_options_;
	jack_options options_;

	uint32_t driver_argc_;
	std::vector<char*> driver_argv_;
};
}

#endif
