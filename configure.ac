#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.
#
# Copyright (C) 2005 by Stefan Siegl <ssiegl@gmx.de>, Germany

AC_PREREQ(2.59)
AC_INIT(fuse4hurd, 0.1, ssiegl@gmx.de)
AC_REVISION($Revision: 1.2 $)
AC_CONFIG_SRCDIR(main.c)
AM_CONFIG_HEADER(config.h)

# Initialize automake
AM_INIT_AUTOMAKE(fuse4hurd, 0.1)

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
AC_CHECK_LIB([threads], [rwlock_init])
AC_CHECK_LIB([ports], [hurd_ihash_init])
AC_CHECK_LIB([fshelp], [fshelp_touch])
AC_CHECK_LIB([iohelp], [iohelp_initialize_conch])
#AC_CHECK_LIB([netfs], [netfs_startup])

LIBS="-lnetfs $LIBS"
CFLAGS="$CFLAGS -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Wall -W -ggdb"

# Checks for header files.

# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.

AC_CONFIG_FILES([Makefile example/Makefile example-22/Makefile])
AC_OUTPUT
