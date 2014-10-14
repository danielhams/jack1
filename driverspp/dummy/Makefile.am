MAINTAINERCLEANFILES=Makefile.in

AM_CFLAGS = $(JACK_CFLAGS)
AM_CXXFLAGS = $(JACK_CPPFLAGS)

plugindir = $(ADDONPP_DIR)

plugin_LTLIBRARIES = jack_dummy.la

jack_dummy_la_LDFLAGS = -module -avoid-version
jack_dummy_la_SOURCES = dummy_driver.cpp

noinst_HEADERS = dummy_driver.hpp

jack_dummy_la_LIBADD = $(top_builddir)/jackdpp/libjackserverpp.la
