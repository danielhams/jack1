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

#include <config.h>

#include "jack_options_parser.hpp"

#include <iostream>
#include <cinttypes>
#include <vector>
#include <sstream>

#include "internal.hpp"

namespace jack
{

using std::cout;
using std::endl;
using std::vector;
using std::string;
using std::stringstream;
using std::ostream;

namespace po = boost::program_options;

constexpr const char * empty_driver_name = "";

jack_options::jack_options() :
    internal_clients(),
    clock_source( JACK_TIMER_SYSTEM_CLOCK ),
    timeout_threshold(250),
    driver(),
    frame_time_offset(0),
    memory_locked(true),
    midi_buffer_size(0),
    server_name(),
    sanity_checks(true),
    port_max(256),
    replace_registry(false),
    realtime(true),
    realtime_priority(10),
    silent(false),
    synchronous(),
    temporary(false),
    show_temporary(false),
    client_timeout(0),
    unlock_gui_memory(false),
    verbose(false),
    show_help(false),
    show_version(false),
    slave_drivers(),
    no_zombies(false),
    success( true ),
    error_message()
{
}

ostream & operator<<( ostream & os, jack_options & jo )
{
    os << "Jack Options Structure:" << endl;
    os << "success(" << jo.success << ")" << endl;
    os << "error_message(" << jo.error_message << ")" << endl;
    os << "show_help(" << jo.show_help << ")" << endl;
    os << "show_version(" << jo.show_version << ")" << endl;
    os << "show_temporary(" << jo.show_temporary << ")" << endl;


    for( const string & ic : jo.internal_clients ) {
	os << "internal_client(" << ic << ")" << endl;
    }
    os << "clock_source(" << jo.clock_source << ")" << endl;
    os << "timeout_threshold(" << jo.timeout_threshold << ")" << endl;
    os << "driver(" << jo.driver << ")" << endl;
    os << "frame_time_offset(" << jo.frame_time_offset << ")" << endl;
    os << "memory_locked(" << jo.memory_locked << ")" << endl;
    os << "midi_buffer_size(" << jo.midi_buffer_size << ")" << endl;
    os << "server_name(" << jo.server_name << ")" << endl;
    os << "sanity_checks(" << jo.sanity_checks << ")" << endl;
    os << "port_max(" << jo.port_max << ")" << endl;
    os << "replace_registry(" << jo.replace_registry << ")" << endl;
    os << "realtime(" << jo.realtime << ")" << endl;
    os << "realtime_priority(" << jo.realtime_priority << ")" << endl;
    os << "silent(" << jo.silent << ")" << endl;
    os << "synchronous(" << jo.synchronous << ")" << endl;
    os << "temporary(" << jo.temporary << ")" << endl;
    os << "client_timeout(" << jo.client_timeout << ")" << endl;
    os << "unlock_gui_memory(" << jo.unlock_gui_memory << ")" << endl;
    os << "verbose(" << jo.verbose << ")" << endl;
    for( const string & sd : jo.slave_drivers ) {
	os << "slave_drivers(" << sd << ")" << endl;
    }
    os << "no_zombies(" << jo.no_zombies << ")" << endl;

    return os;
}

jack_options_parser::jack_options_parser( int argc, char ** argv, bool debug ) :
    all_options_(),
    visible_options_("Standard Options"),
    hidden_options_("Additional Options"),
    driver_argc_(0),
    driver_argv_()
{
    visible_options_.add_options()
	( "driver,d", po::value<string>(),
#ifdef __APPLE__
	  "Backend (coreaudio, dummy, net, or portaudio)"
#else
	  "Backend (alsa, dummy, freebob, firewire, net, oss, sun, or portaudio)"
#endif
	    )
	( "no-realtime,r", "Don't run realtime" )
	( "realtime,R", "Run realtime" )
	( "name,n", po::value<string>(), "Server name" )
	( "internal-client,I", po::value<vector<string>>(), "Specify an internal client" )
	( "no-mlock,m", "No memory lock" )
	( "unlock,u", "Unlock memory" )
	( "timeout,t", po::value<string>(), "Client timeout in msecs" )
	( "port-max,p", po::value<string>(), "Port maximum" )
	( "no-sanity-checks,N", "Skip sanity checks on launch" )
	( "verbose,v", "Verbose messages" )
	( "clock-source,c", po::value<string>(), "Clock source [ h(pet) | s(system) ]" )
	( "replace-registry", "Replace registry" )
	( "realtime-priority,P", po::value<string>(), "The priority when realtime" )
	( "silent,s", "Silent" )
	( "version,V", "Show version" )
	( "nozombies,Z", "No zombies" )
	;

    hidden_options_.add_options()
#ifdef HAVE_ZITA_BRIDGE_DEPS
	( "alsa-add,A", po::value<vector<string>>(), "Add alsa zita bridge")
#endif
	( "help,h", "Show this help message" )
	( "tmpdir-location,l", "Output temporary directory location" )
	( "midi-bufsize,M", po::value<string>(), "Midi buffer size" )
	( "sync,S", "Sync" )
	( "temporary,T", "Temporary" )
	( "slave-driver,X", po::value<vector<string>>(), "Slave driver to use" )
	( "timeout-thres,C", po::value<string>(), "Timeout threshold" )
	;

    all_options_.add(visible_options_);
    all_options_.add(hidden_options_);

    // For C++, we'll create a fake argc and argv
    // containing all the parameters up and including the first driver
    // and argument so we don't have to validate the driver options
    // and can leave that to them
    uint32_t orig_argc = argc;
    uint32_t fake_argc = 0;
    for( ; fake_argc < orig_argc ; ++fake_argc ) {
	char * val = argv[fake_argc];
	if( val[0] == '-' ) {
	    if( val[1] == 'd' ) {
		if( val[2] == '\0' ) {
		    // -d args
		    fake_argc = (fake_argc + 2 <= orig_argc ? fake_argc + 2 : orig_argc );
		    break;
		} else {
		    // -dargs
		    fake_argc = (fake_argc + 1 <= orig_argc ? fake_argc + 1 : orig_argc );
		    break;
		}
	    } else if( val[1] == '-' && val[2] == 'd' ) {
		// --driver args
		// Make the driver arg available, too
		fake_argc = (fake_argc + 2 <= orig_argc ? fake_argc + 2 : orig_argc );
		break;
	    }
	}
    }

    vector<char*> fake_argv( fake_argc );
	
    for( uint32_t i = 0 ; i < fake_argc ; ++i ) {
	fake_argv[i] = argv[i];
    }

    driver_argc_ = argc - fake_argc;

    // If we have a driver (and thus some arguments) make the driver name be
    // the first argument
    // We'll leave it blank here then use what it parses out as
    // and fill it in
    driver_argc_ = driver_argc_ + 1;

    driver_argv_.resize( driver_argc_ );

    driver_argv_[0] = (char*)empty_driver_name;

    for( uint32_t i = 0 ; i < driver_argc_ - 1 ; ++i ) {
	uint32_t da_index = i + 1;
	driver_argv_[da_index] = argv[fake_argc + i];
    }

    // Assume true, and set to false when errors encountered.
    options_.success = true;

    // Hack to pick up the -X supplied as driver arguments (alsa)
    for( uint32_t i = 1 ; i < driver_argc_ ; ++i ) {
	if( strcmp( driver_argv_[i], "-Xseq") == 0 ) {
	    options_.slave_drivers.push_back("alsa_midi");
	}
	else if( strcmp( driver_argv_[i], "-X") == 0 ) {
	    if( i + 1 < driver_argc_ ) {
		if( strcmp( driver_argv_[i+1], "seq" ) == 0 ) {
		    options_.slave_drivers.push_back("alsa_midi");
		}
	    }
	    else {
		options_.success = false;
		options_.error_message = "Unknown slave specified as part of driver: " + string(driver_argv_[i]);
	    }
	}
    }

    po::variables_map vm;

    try {
	po::store( po::parse_command_line( fake_argc, &fake_argv[0], all_options_), vm );
	po::notify(vm);
    }
    catch( po::unknown_option & uo ) {
	options_.success = false;
	options_.error_message = uo.what();
	options_.show_help = true;
    }
    catch( po::error & ro ) {
	options_.success = false;
	options_.error_message = ro.what();
	options_.show_help = true;
    }

    if( debug ) {
	cout << "Beginning parsed variable map dump..." << endl;
	for( auto & option_variable_entry : vm ) {
	    cout << "Found an option variable: " << option_variable_entry.first << endl;
	    auto & value = option_variable_entry.second.value();

	    if( auto v = boost::any_cast<uint32_t>(&value)) {
		cout << "It's a uint32_t - " << *v << endl;
	    }
	    else if( auto v = boost::any_cast<string>(&value)) {
		string & sv = *v;
		if( sv.length() > 0 ) {
		    cout << "It's a string - " << *v << endl;
		}
	    }
	    else if( auto v = boost::any_cast<vector<string>>(&value)) {
		cout << "It's a vector<string>" << endl;
		vector<string> & vec = *v;
		for( string & vs : vec ) {
		    cout << "One string value: " << vs << endl;
		}
	    }
	}
    }

    // Moved this out from the "standard" handling so that --help --verbose
    // will display all options, not just the standard ones.
    if( vm.count("verbose") > 0 ) {
	options_.verbose = true;
    }

    // If we didn't get any arguments or one of them was "help"
    // Just display the usage.
    if( argc < 2 || vm.count("help") > 0 ) {
	// In the case the help switch was present, don't consider
	// any parse errors
	options_.show_help = true;
	options_.success = true;
	options_.error_message = "";
    }

    if( !options_.show_help && options_.success ) {
#ifdef HAVE_ZITA_BRIDGE_DEPS
	if( vm.count("alsa-add") > 0 ) {
	    vector<string> zita_alsa_devices = vm["alsa-add"].as<vector<string>>();
	    for( string & zita_alsa_device : zita_alsa_devices ) {
		string::size_type percent_pos = zita_alsa_device.find( '%' );
		bool need_capture = false;
		bool need_playback = false;
		string device_name;
		if( percent_pos == string::npos ) {
		    need_capture = true;
		    need_playback = true;
		    device_name = zita_alsa_device;
		}
		else {
		    size_t char_pos = percent_pos + 1;
		    device_name = zita_alsa_device.substr(0,percent_pos);
		    if( char_pos < zita_alsa_device.length() ) {
			if( zita_alsa_device[ char_pos ] == 'c' ) {
			    need_capture = true;
			}
			else if( zita_alsa_device[ char_pos ] == 'p' ) {
			    need_playback = true;
			}
			else {
			    cout << "Expected capture/playback character for zita bridge missing" << endl;
			    exit(1);
			}
		    }
		    else {
			cout << "Expected capture/playback character for zita bridge missing" << endl;
			exit(1);
		    }
		}
		if( need_capture ) {
		    stringstream ss( stringstream::out );
		    ss << device_name << "_rec:zalsa_in/-dhw:" << device_name;
		    options_.internal_clients.push_back( ss.str() );
		}
		if( need_playback ) {
		    stringstream ss( stringstream::out );
		    ss << device_name << "_play:zalsa_out/-dhw:" << device_name;
		    options_.internal_clients.push_back( ss.str() );
		}
	    }
	}
#endif

	if( vm.count("clock-source") > 0 ) {
	    string cs_str = vm["clock-source"].as<string>();
	    if( cs_str[0] == 's' || cs_str[0] == 'c' ) {
		options_.clock_source = JACK_TIMER_SYSTEM_CLOCK;
	    }
	    else if( cs_str[0] == 'h' ) {
		options_.clock_source = JACK_TIMER_HPET;
	    }
	    else {
		options_.success = false;
		options_.error_message = "Unknown clock source specified: " + cs_str;
	    }
	}

	if( vm.count("timeout-thres") > 0 ) {
	    string tt_str = vm["timeout-thres"].as<string>();
	    options_.timeout_threshold = atoi(tt_str.c_str());
	}

	if( vm.count("driver") > 0 ) {
	    string driver_str = vm["driver"].as<string>();
	    options_.driver = driver_str;
	    driver_argv_[0] = (char*)options_.driver.c_str();
	}
	else {
	    options_.success = false;
	    options_.error_message = "A driver must be specified";
	}

	if( vm.count("frame-time-offset") > 0 ) {
	    string fto_str = vm["frame-time-offset"].as<string>();
	    options_.frame_time_offset = JACK_MAX_FRAMES - atoi(fto_str.c_str());
	}

	if( vm.count("temporary") > 0 ) {
	    options_.temporary = true;
	}

	if( vm.count("tmpdir-location") > 0 ) {
	    options_.show_temporary = true;
	}

	if( vm.count("memory-lock") > 0 ) {
	    options_.memory_locked = false;
	}

	if( vm.count("midi-bufsize") > 0 ) {
	    string mbs_str = vm["midi-bufsize"].as<string>();
	    options_.midi_buffer_size = (unsigned int)atol(mbs_str.c_str());
	}

	if( vm.count("name") > 0 ) {
	    string server_name_str = vm["server_name"].as<string>();
	    options_.server_name = server_name_str;
	}

	if( vm.count("no-sanity-checks") > 0 ) {
	    options_.sanity_checks = false;
	}

	if( vm.count("port-max") > 0 ) {
	    string pm_str = vm["port-max"].as<string>();
	    options_.port_max = (unsigned int)atol(pm_str.c_str());
	}

	if( vm.count("realtime-priority") > 0 ) {
	    string rp_str = vm["realtime-priority"].as<string>();
	    options_.realtime_priority = (unsigned int)atol(rp_str.c_str());
	}

	if( vm.count("no-realtime") > 0 ) {
	    options_.realtime = false;
	}

	if( vm.count("realtime") > 0 ) {
	    options_.realtime = true;
	}

	if( vm.count("silent") > 0 ) {
	    options_.silent = true;
	}

	if( vm.count("timeout") > 0 ) {
	    string ct_str =vm["timeout"].as<string>();
	    options_.client_timeout = atoi(ct_str.c_str());
	}

	if( vm.count("unlock") > 0 ) {
	    options_.unlock_gui_memory = true;
	}

	if( vm.count("version") > 0 ) {
	    options_.show_version = true;
	}

	if( vm.count("slave-driver") > 0 ) {
	    vector<string> sd_vec = vm["slave-driver"].as<vector<string>>();
	    options_.slave_drivers.insert( options_.slave_drivers.end(), sd_vec.begin(), sd_vec.end() );
	}

	if( vm.count("nozombies") > 0 ) {
	    options_.no_zombies = true;
	}
	// --help should have already been handled
    }

    if( !options_.realtime && options_.client_timeout == 0 ) {
	// Moved from jack_main
	options_.client_timeout = 500; /* 0.5 sec; usable when non realtime. */
    }

    if( debug ) {
	cout << options_ << endl;
	for( uint32_t i = 0 ; i < driver_argc_ ; ++i )
	{
	    cout << "Going out driver argv " << i << " is " << driver_argv_[i] << endl;
	}
    }
}

void jack_options_parser::display_usage()
{
    if( options_.verbose ) {
	cout << all_options_ << endl;
    }
    else {
	cout << visible_options_ << endl;
    }
}

}
