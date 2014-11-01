NULL=

MAINTAINERCLEANFILES    = Makefile.in

if USE_POSIX_SHM
install-exec-hook:
	@echo "Nothing to make for $@."
else
install-exec-hook:
	@echo "Removing JACK shared memory registry key " $(JACK_SEMAPHORE_KEY)
	-ipcrm -M $(JACK_SEMAPHORE_KEY)
endif

SOURCE_FILES = \
	client.cpp \
	driver.cpp \
	intclient.cpp \
	messagebuffer.cpp \
	pool.cpp \
	port.cpp \
	metadata.cpp \
	midiport.cpp \
	ringbuffer.cpp \
	sanitycheck.cpp \
	systemtest.cpp \
	shm.cpp \
	thread.cpp \
	time.cpp \
	transclient.cpp \
	unlock.cpp \
	uuid.cpp \
	jack_pp_debug.cpp \
	jack_utils.cpp \
	$(NULL)

simd.lo: $(srcdir)/simd.cpp
	$(LIBTOOL) --mode=compile $(CXX) -I$(top_builddir) $(JACK_CORE_CPPFLAGS) $(SIMD_CFLAGS) -c -o simd.lo $(srcdir)/simd.cpp

lib_LTLIBRARIES	= libjackpp.la

noinst_HEADERS	= local.hpp

AM_CFLAGS = $(JACK_CFLAGS) -DJACK_LOCATION=\"$(bindir)\"
AM_CXXFLAGS = $(JACK_CPPFLAGS) -DJACK_LOCATION=\"$(bindir)\"

libjackpp_la_CFLAGS = $(AM_CFLAGS)
libjackpp_la_CXXFLAGS = $(AM_CXXFLAGS)

libjackpp_la_SOURCES = $(SOURCE_FILES)
libjackpp_la_LIBADD  = simd.lo -ldb @OS_LDFLAGS@
libjackpp_la_LDFLAGS  = -export-dynamic -version-info @JACK_SO_VERSION@
