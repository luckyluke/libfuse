## Process this file with automake to produce Makefile.in

noinst_PROGRAMS = fusexmp fusexmp_fh null hello

fusexmp_SOURCES = fusexmp.c
fusexmp_fh_SOURCES = fusexmp_fh.c
null_SOURCES = null.c
hello_SOURCES = hello.c

LDADD = ../libfuse.la
AM_CPPFLAGS = -DFUSE_USE_VERSION=23 -I$(top_srcdir)
