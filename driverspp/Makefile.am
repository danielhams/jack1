MAINTAINERCLEANFILES = Makefile.in

if HAVE_ALSA
ALSA_DIR = alsa
else
ALSA_DIR =
endif

if HAVE_ALSA_MIDI
ALSA_MIDI_DIR = alsa_midi
else
ALSA_MIDI_DIR =
endif

SUBDIRS = $(ALSA_MIDI_DIR) $(ALSA_DIR) dummy netjack
DIST_SUBDIRS = alsa alsa_midi dummy netjack
