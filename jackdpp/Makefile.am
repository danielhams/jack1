NULL=

MAINTAINERCLEANFILES = Makefile.in jackdpp.1 jackpp_md5.h

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
AM_CPPFLAGS = $(JACK_CPPFLAGS) -DJACK_LOCATION=\"$(bindir)\" $(BOOST_CPPFLAGS) $(BOOST_PROGRAM_OPTIONS_CPPFLAGS)

jackdpp_SOURCES = jackdpp.cpp \
	jack_options_parser.cpp \
	$(NULL)

jackdpp_LDFLAGS = $(BOOST_LDFLAGS) $(BOOST_PROGRAM_OPTIONS_LDFLAGS)

jackdpp_LDADD = libjackserverpp.la $(CAP_LIBS) @OS_LDFLAGS@ $(BOOST_LIBS) $(BOOST_PROGRAM_OPTIONS_LIBS)

noinst_HEADERS = jackpp_md5.h md5.h md5_loc.h \
		 clientengine.h transengine.h

BUILT_SOURCES = jackpp_md5.h

jackpp_md5.h: jackdpp
if STRIPPED_JACKD
	strip -R .note -R .comment .libs/jackdpp
endif
	echo "#define JACKDPP_MD5_SUM \"`md5sum .libs/jackdpp | awk '{print $$1}'`\"" > jackpp_md5.h

jackstartpp_SOURCES = jackstartpp.c md5.c
jackstartpp_LDFLAGS = -lcap

lib_LTLIBRARIES	= libjackserverpp.la

libjackserverpp_la_CFLAGS = $(AM_CFLAGS)

libjackserverpp_la_SOURCES = engine.cpp \
	clientengine.c \
	transengine.c \
	controlapi.c \
	../libjackpp/systemtest.c \
	../libjackpp/sanitycheck.c \
	../libjackpp/client.c \
	../libjackpp/driver.c \
	../libjackpp/intclient.c \
	../libjackpp/messagebuffer.c \
	../libjackpp/pool.c \
	../libjackpp/port.c \
	../libjackpp/midiport.c \
	../libjackpp/ringbuffer.c \
	../libjackpp/shm.c \
	../libjackpp/thread.c \
	../libjackpp/time.c \
	../libjackpp/transclient.c \
	../libjackpp/unlock.c \
	../libjackpp/uuid.c \
	../libjackpp/metadata.c \
	$(NULL)
libjackserverpp_la_LIBADD  = simd.lo -ldb @OS_LDFLAGS@ 
libjackserverpp_la_LDFLAGS  = -export-dynamic -version-info @JACK_SO_VERSION@

simd.lo: $(srcdir)/../libjackpp/simd.c
	$(LIBTOOL) --mode=compile $(CC) -I$(top_builddir) $(JACK_CORE_CFLAGS) $(SIMD_CFLAGS) -c -o simd.lo $(srcdir)/../libjackpp/simd.c

man_MANS = jackdpp.1 jackstartpp.1
EXTRA_DIST = $(man_MANS)

dist-hook:
	rm -f $(distdir)/jackpp_md5.h
distclean-local:
	rm -f $(top_builddir)/jackdpp/jackpp_md5.h
