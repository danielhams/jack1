#include <config.h>

#include "jack_options_parser.hpp"

#include <iostream>
#include <cinttypes>
#include <vector>
#include <sstream>

#include "internal.h"

namespace jack
{

using std::cout;
using std::endl;
using std::vector;
using std::string;
using std::stringstream;
using std::ostream;

namespace po = boost::program_options;

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
    show_temporary(false),
    client_timeout(0),
    unlock_memory(false),
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


    //    os << "internal_clients(" << jo.internal_clients << ")" << endl;
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
    os << "client_timeout(" << jo.client_timeout << ")" << endl;
    os << "unlock_memory(" << jo.unlock_memory << ")" << endl;
    os << "verbose(" << jo.verbose << ")" << endl;
    //    os << "slave_drivers(" << jo.slave_drivers << ")" << endl;
    os << "no_zombies(" << jo.no_zombies << ")" << endl;

    return os;
}

jack_options_parser::jack_options_parser( int argc, char ** argv, bool debug ) :
    all_options_(),
    visible_options_("Standard Options"),
    hidden_options_("Additional Options")
{
    cout << "Options parser called!" << endl;

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
        ( "timeout,t", po::value<uint32_t>(), "Client timeout in msecs" )
        ( "port-max,p", po::value<uint32_t>(), "Port maximum" )
        ( "no-sanity-checks,N", "Skip sanity checks on launch" )
        ( "verbose,v", "Verbose messages" )
        ( "clock-source,c", po::value<string>(), "Clock source [ h(pet) | s(system) ]" )
        ( "replace-registry", "Replace registry" )
        ( "realtime-priority,P", po::value<uint32_t>(), "The priority when realtime" )
        ( "silent,s", "Silent" )
        ( "version,V", "Show version" )
        ( "nozombies,Z", "No zombies" )
        ;

    hidden_options_.add_options()
#ifdef HAVE_ZITA_BRIDGE_DEPS
        ( "alsa-add,A", po::value<vector<string>>(), "Add alsa zita bridge")
#endif
        ( "help,h", "Show this help message" )
        ( "tmpdir-location,l", "Output temporary directory" )
        ( "midi-bufsize,M", po::value<uint32_t>(), "Midi buffer size" )
        ( "sync,S", "Sync" )
        ( "temporary,T", "Temporary" )
        ( "slave-driver,X", po::value<string>(), "Slave driver to use" )
        ( "timeout-thres,C", po::value<uint32_t>(), "Timeout threshold" )
        ;

    all_options_.add(visible_options_);
    all_options_.add(hidden_options_);

    // For C++, we'll create a fake argc and argv
    // containing all the parameters up and including the first driver
    // and argument so we don't have to validate the driver options
    // and can leave that to them
    int32_t fake_argc = 0;
    for( ; fake_argc < argc ; ++fake_argc ) {
        char * val = argv[fake_argc];
        if( val[0] == '-' ) {
            if( val[1] == 'd' ) {
                if( val[2] == '\0' ) {
                    // -d args
                    fake_argc = (fake_argc + 2 <= argc ? fake_argc + 2 : argc );
                    break;
                } else {
                    // -dargs
                    fake_argc = (fake_argc + 1 <= argc ? fake_argc + 1 : argc );
                    break;
                }
            } else if( val[1] == '-' && val[2] == 'd' ) {
                // --driver args
                // Make the driver arg available, too
                fake_argc = (fake_argc + 2 <= argc ? fake_argc + 2 : argc );
                break;
            }
        }
    }

    cout << "Have " << fake_argc << " non-driver arguments" << endl;
    vector<char*> fake_argv( fake_argc );
    for( int32_t i = 0 ; i < fake_argc ; ++i ) {
        fake_argv[i] = argv[i];
        cout << "Setup fake argv " << i << " to be " << fake_argv[i] << endl;
    }

    int32_t driver_argc = argc - fake_argc;
    cout << "Have " << driver_argc << " driver arguments" << endl;
    vector<char*> driver_argv( driver_argc );
    for( int32_t i = 0 ; i < driver_argc ; ++i ) {
        driver_argv[i] = argv[fake_argc + i];
        cout << "Setup driver argv " << i << " to be " << driver_argv[i] << endl;
    }

    // Assume true, and set to false when errors encountered.
    options_.success = true;

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
                cout << "It's a string - " << *v << endl;
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
        cout << "Overriding any options error as either empty or help specified." << endl;
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
                cout << "Found zita alsa device entry: " << zita_alsa_device << endl;
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
                    cout << "(1)PPAdded '" << options_.internal_clients.back() << "' to the internal client list" << endl;
                }
                if( need_playback ) {
                    stringstream ss( stringstream::out );
                    ss << device_name << "_play:zalsa_out/-dhw:" << device_name;
                    options_.internal_clients.push_back( ss.str() );
                    cout << "(2)PPAdded '" << options_.internal_clients.back() << "' to the internal client list" << endl;
                }
            }
        }
#endif

        if( vm.count("clock-source") > 0 ) {
            string cs_str = vm["clock-source"].as<string>();
            if( cs_str.length() < 2 ) {
                options_.success = false;
                options_.error_message = "Unknown or missing clock source: " + cs_str;
            }
            else {
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
        }

        if( vm.count("timeout-thres") > 0 ) {
            int32_t tt_int = vm["timeout-thres"].as<int32_t>();
        }

        if( vm.count("driver") > 0 ) {
            string driver_str = vm["driver"].as<string>();
            options_.driver = driver_str;
        }
        else {
            options_.success = false;
            options_.error_message = "A driver must be specified";
        }

        if( vm.count("frame-time-offset") > 0 ) {
            int32_t fto_int = vm["frame-time-offset"].as<int32_t>();
            options_.frame_time_offset = JACK_MAX_FRAMES - fto_int;
        }

        if( vm.count("temporary") > 0 ) {
            options_.show_temporary = true;
        }

        if( vm.count("memory-lock") > 0 ) {
        }

        if( vm.count("midi-bufsize") > 0 ) {
        }

        if( vm.count("name") > 0 ) {
        }

        if( vm.count("no-sanity-checks") > 0 ) {
        }

        if( vm.count("port-max") > 0 ) {
            
        }

        if( vm.count("realtime-priority") > 0 ) {
        }

        if( vm.count("no-realtime") > 0 ) {
        }

        if( vm.count("realtime") > 0 ) {
        }

        if( vm.count("silent") > 0 ) {
        }

        if( vm.count("temporary") > 0 ) {
        }

        if( vm.count("timeout") > 0 ) {
        }

        if( vm.count("unlock") > 0 ) {
        }

        if( vm.count("version") > 0 ) {
        }

        if( vm.count("slave-driver") > 0 ) {
        }

        if( vm.count("nozombies") > 0 ) {
        }
        // --help should have already been handled
    }

    cout << "Debugging of jack_options: " << options_ << endl;

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
