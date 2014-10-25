NULL=

MAINTAINERCLEANFILES=Makefile.in

AM_CFLAGS = $(JACK_CFLAGS)
AM_CXXFLAGS = $(JACK_CPPFLAGS)

plugindir = $(ADDONPP_DIR)

plugin_LTLIBRARIES = jack_alsa.la

jack_alsa_la_LDFLAGS = -module -avoid-version
jack_alsa_la_SOURCES = 		\
	alsa_driver.cpp 	\
	generic_hw.cpp 		\
	memops.cpp 		\
	hammerfall.cpp		\
	hdsp.cpp		\
	ice1712.cpp		\
	usx2y.cpp		\
	$(NULL)

noinst_HEADERS = \
	alsa_driver.hpp		\
	generic.hpp		\
	hammerfall.hpp		\
	hdsp.hpp		\
	ice1712.hpp		\
	usx2y.hpp		\
	$(NULL)

#jack_alsa_la_LIBADD = $(ALSA_LIBS) $(top_builddir)/jackdpp/libjackserverpp.la $(top_builddir)/libjackpp/libjackpp.la
jack_alsa_la_LIBADD = $(ALSA_LIBS) $(top_builddir)/jackdpp/libjackserverpp.la
