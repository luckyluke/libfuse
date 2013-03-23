## Process this file with automake to produce Makefile.in

noinst_PROGRAMS = fusexmp fusexmp_fh null hello 
# low-level api not supported: hello_ll

fusexmp_SOURCES = fusexmp.c
fusexmp_fh_SOURCES = fusexmp_fh.c
null_SOURCES = null.c
hello_SOURCES = hello.c

LDADD = ../src/libfuse.la
AM_CPPFLAGS = -DFUSE_USE_VERSION=26 -I$(top_srcdir) -I$(top_srcdir)/include
