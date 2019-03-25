# Makefile.am
# Used by automake and configure to create Makefile.
#
# Copyright (C) 2005,2006 Stefan Siegl <stesie@brokenpipe.de>, Germany

lib_LTLIBRARIES = libfuse.la

libfuse_la_LDFLAGS = -version-number 1:0:0 \
	-Wl,--version-script,$(srcdir)/fuse_versionscript
libfuse_la_SOURCES = 	\
	main.c		\
	netfs.c		\
	netnode.c	\
	node.c		\
	fuse_opt.c

noinst_HEADERS = \
	fuse_i.h

DEFAULT_INCLUDES = -I. -I$(top_srcdir) -I$(top_srcdir)/include
