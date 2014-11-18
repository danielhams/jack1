NULL=

MAINTAINERCLEANFILES    = Makefile.in

#
# slave driver to bridge to ALSA (sequencer) MIDI
#

AM_CFLAGS = $(JACK_CFLAGS)
AM_CXXFLAGS = $(JACK_CPPFLAGS)

driverdir = $(ADDONPP_DIR)

driver_LTLIBRARIES = jack_alsa_midi.la

jack_alsa_midi_la_LDFLAGS = -module -avoid-version @OS_LDFLAGS@
jack_alsa_midi_la_SOURCES = alsa_midi.cpp \
	port.cpp \
	port_hash.cpp \
	port_thread.cpp \
	list.c \
	$(NULL)

jack_alsa_midi_la_LIBADD = $(ALSA_LIBS) 

noinst_HEADERS = a2j.hpp \
	list.h \
	port.hpp \
	port_hash.hpp \
	port_thread.hpp \
	midi_pack.hpp \
	midi_unpack.hpp \
	$(NULL)
