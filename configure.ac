#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.
#
# Copyright (C) 2005,2006 by Stefan Siegl <stesie@brokenpipe.de>, Germany

AC_PREREQ(2.59)
AC_INIT(libfuse, 0.1, hurdextras-hackers@nongnu.org)
AC_REVISION($Revision: 1.9 $)
AC_CONFIG_SRCDIR(src/main.c)
AM_CONFIG_HEADER(config.h)

# Initialize automake
AM_INIT_AUTOMAKE(libfuse, 0.1)

# Checks for programs.
m4_ifdef([LT_INIT], [LT_INIT], [AC_PROG_LIBTOOL])
AC_PROG_CC
AC_PROG_MAKE_SET

# compatibility for automake < 1.8
if test -z "$mkdir_p"; then
        mkdir_p="../mkinstalldirs"
        AC_SUBST(mkdir_p)
fi

# Checks for libraries.
AC_CHECK_LIB([pthread], [pthread_rwlock_init])
AC_CHECK_LIB([ports], [ports_create_port])
AC_CHECK_LIB([fshelp], [fshelp_touch])
AC_CHECK_LIB([iohelp], [iohelp_initialize_conch])
#AC_CHECK_LIB([netfs], [netfs_startup])

LIBS="-lnetfs $LIBS"
CFLAGS="$CFLAGS -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -W -ggdb"

# Checks for header files.

# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.

AC_CONFIG_FILES([fuse.pc Makefile]
		[include/Makefile src/Makefile]
		[example-25/Makefile
		 example-26/Makefile])
AC_OUTPUT
