NULL=

MAINTAINERCLEANFILES = Makefile.in jackd.1 jack_md5.hpp

if USE_CAPABILITIES
CAP_PROGS = jackstart
CAP_LIBS = -lcap
install-exec-hook:
	if [ "`id -u`" = "0" ]; \
	then	chown root.root $(DESTDIR)$(bindir)/jackd; \
			chmod 555 $(DESTDIR)$(bindir)/jackd; \
			chown root.root $(DESTDIR)$(bindir)/jackstart; \
			chmod 4555 $(DESTDIR)$(bindir)/jackstart; \
	else    echo "You must be root to set ownership and permissions"; \
			echo " for jackd and jackstart."; \
	fi
else
CAP_PROGS =
CAP_LIBS =
install-exec-hook:
	@echo "Nothing to make for $@."
endif

bin_PROGRAMS = jackd $(CAP_PROGS)

AM_CFLAGS = $(JACK_CFLAGS) -DJACK_LOCATION=\"$(bindir)\"
AM_CXXFLAGS = $(JACK_CPPFLAGS) -DJACK_LOCATION=\"$(bindir)\" $(BOOST_CPPFLAGS) $(BOOST_PROGRAM_OPTIONS_CPPFLAGS)

jackd_SOURCES = jackd.cpp \
	$(NULL)

jackd_LDFLAGS = $(BOOST_LDFLAGS) $(BOOST_PROGRAM_OPTIONS_LDFLAGS)

jackd_LDADD = libjackserver.la $(CAP_LIBS) @OS_LDFLAGS@ $(top_builddir)/libjack/libjack.la $(BOOST_LIBS) $(BOOST_PROGRAM_OPTIONS_LIBS)

noinst_HEADERS = jack_md5.hpp \
	jack_options_parser.hpp \
	jack_signals.hpp \
	jack_drivers.hpp \
	jack_lock_helpers.hpp \
	md5.hpp \
	md5_loc.hpp \
	clientengine.hpp \
	transengine.hpp \
	$(NULL)

BUILT_SOURCES = jack_md5.hpp

jack_md5.hpp: jackd
if STRIPPED_JACKD
	strip -R .note -R .comment .libs/jackd
endif
	echo "#define JACKD_MD5_SUM \"`md5sum .libs/jackd | awk '{print $$1}'`\"" > jack_md5.hpp

jackstart_SOURCES = jackstart.cpp md5.cpp
jackstart_LDFLAGS = -lcap

lib_LTLIBRARIES	= libjackserver.la

libjackserver_la_CFLAGS = $(AM_CFLAGS)
libjackserver_la_CXXFLAGS = $(AM_CXXFLAGS)

libjackserver_la_SOURCES = \
	jack_options_parser.cpp \
	jack_signals.cpp \
	jack_drivers.cpp \
	jack_engine.cpp \
	clientengine.cpp \
	transengine.cpp \
	controlapi.cpp \
	$(NULL)
libjackserver_la_LIBADD  = -ldb @OS_LDFLAGS@ $(top_builddir)/libjack/libjack.la
libjackserver_la_LDFLAGS  = -export-dynamic -version-info @JACK_SO_VERSION@

man_MANS = jackd.1 jackstart.1
EXTRA_DIST = $(man_MANS)

dist-hook:
	rm -f $(distdir)/jack_md5.hpp
distclean-local:
	rm -f $(top_builddir)/jackd/jack_md5.hpp
