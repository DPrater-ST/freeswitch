include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_deepgram_transcribe

mod_LTLIBRARIES = mod_deepgram_transcribe.la
mod_deepgram_transcribe_la_SOURCES  = mod_deepgram_transcribe.c dg_transcribe_glue.cpp ws_client.c
mod_deepgram_transcribe_la_CFLAGS   = $(AM_CFLAGS)
mod_deepgram_transcribe_la_CXXFLAGS = $(AM_CXXFLAGS) -std=c++14
mod_deepgram_transcribe_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_deepgram_transcribe_la_LDFLAGS  = -avoid-version -module -no-undefined -shared -lcrypto
