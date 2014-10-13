NULL=

MAINTAINERCLEANFILES = Makefile.in jackdpp.1 jackpp_md5.hpp

if USE_CAPABILITIES
CAP_PROGS = jackstartpp
CAP_LIBS = -lcap
install-exec-hook:
	if [ "`id -u`" = "0" ]; \
	then	chown root.root $(DESTDIR)$(bindir)/jackdpp; \
			chmod 555 $(DESTDIR)$(bindir)/jackdpp; \
			chown root.root $(DESTDIR)$(bindir)/jackstartpp; \
			chmod 4555 $(DESTDIR)$(bindir)/jackstartpp; \
	else    echo "You must be root to set ownership and permissions"; \
			echo " for jackdpp and jackstartpp."; \
	fi
else
CAP_PROGS =
CAP_LIBS =
install-exec-hook:
	@echo "Nothing to make for $@."
endif

bin_PROGRAMS = jackdpp $(CAP_PROGS)

AM_CFLAGS = $(JACK_CFLAGS) -DJACK_LOCATION=\"$(bindir)\"
AM_CXXFLAGS = $(JACK_CPPFLAGS) -DJACK_LOCATION=\"$(bindir)\" $(BOOST_CPPFLAGS) $(BOOST_PROGRAM_OPTIONS_CPPFLAGS)

jackdpp_SOURCES = jackdpp.cpp \
	$(NULL)

jackdpp_LDFLAGS = $(BOOST_LDFLAGS) $(BOOST_PROGRAM_OPTIONS_LDFLAGS)

jackdpp_LDADD = libjackserverpp.la $(CAP_LIBS) @OS_LDFLAGS@ $(BOOST_LIBS) $(BOOST_PROGRAM_OPTIONS_LIBS)

noinst_HEADERS = jackpp_md5.hpp \
	jack_options_parser.hpp \
	jack_cpp_utils.hpp \
	jack_drivers.hpp \
	jack_engine.hpp \
	md5.hpp \
	md5_loc.hpp \
	clientengine.hpp \
	transengine.hpp \
	$(NULL)

BUILT_SOURCES = jackpp_md5.hpp

jackpp_md5.hpp: jackdpp
if STRIPPED_JACKD
	strip -R .note -R .comment .libs/jackdpp
endif
	echo "#define JACKDPP_MD5_SUM \"`md5sum .libs/jackdpp | awk '{print $$1}'`\"" > jackpp_md5.hpp

jackstartpp_SOURCES = jackstartpp.cpp md5.cpp
jackstartpp_LDFLAGS = -lcap

lib_LTLIBRARIES	= libjackserverpp.la

libjackserverpp_la_CFLAGS = $(AM_CFLAGS)

libjackserverpp_la_SOURCES = \
	jack_options_parser.cpp \
	jack_cpp_utils.cpp \
	jack_drivers.cpp \
	jack_engine.cpp \
	clientengine.cpp \
	transengine.cpp \
	controlapi.cpp \
	../libjackpp/systemtest.cpp \
	../libjackpp/sanitycheck.cpp \
	../libjackpp/client.cpp \
	../libjackpp/driver.cpp \
	../libjackpp/intclient.cpp \
	../libjackpp/messagebuffer.cpp \
	../libjackpp/pool.cpp \
	../libjackpp/port.cpp \
	../libjackpp/midiport.cpp \
	../libjackpp/ringbuffer.cpp \
	../libjackpp/shm.cpp \
	../libjackpp/thread.cpp \
	../libjackpp/time.cpp \
	../libjackpp/transclient.cpp \
	../libjackpp/unlock.cpp \
	../libjackpp/uuid.cpp \
	../libjackpp/metadata.cpp \
	$(NULL)
libjackserverpp_la_LIBADD  = simd.lo -ldb @OS_LDFLAGS@ 
libjackserverpp_la_LDFLAGS  = -export-dynamic -version-info @JACK_SO_VERSION@

simd.lo: $(srcdir)/../libjackpp/simd.cpp
	$(LIBTOOL) --mode=compile $(CXX) -I$(top_builddir) $(JACK_CORE_CPPFLAGS) $(SIMD_CFLAGS) -c -o simd.lo $(srcdir)/../libjackpp/simd.cpp

man_MANS = jackdpp.1 jackstartpp.1
EXTRA_DIST = $(man_MANS)

dist-hook:
	rm -f $(distdir)/jackpp_md5.hpp
distclean-local:
	rm -f $(top_builddir)/jackdpp/jackpp_md5.hpp
