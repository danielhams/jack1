NULL=

MAINTAINCLEANFILES = Makefile.in

AM_CFLAGS = $(JACK_CFLAGS) 
AM_CXXFLAGS = $(JACK_CPPFLAGS) 

plugindir = $(ADDON_DIR)

plugin_LTLIBRARIES = jack_net.la

jack_net_la_LDFLAGS = -module -avoid-version @NETJACK_LIBS@

jack_net_la_CXXFLAGS = @NETJACK_CFLAGS@
jack_net_la_SOURCES = \
	net_driver.cpp \
	netjack_packet.cpp \
	netjack.cpp \
	$(NULL)

jack_net_la_LIBADD = $(top_builddir)/libjack/libjack.la

noinst_HEADERS = \
	netjack.hpp \
	net_driver.hpp \
	netjack_packet.hpp \
	$(NULL)
