#!/bin/sh
AUTOMAKE_FLAGS="$AUTOMAKE_FLAGS -a " #- automatically add missing files

echo "Running aclocal $ACLOCAL_FLAGS"
aclocal $ACLOCAL_FLAGS || exit 1

echo "Running autoconf $AUTOCONF_FLAGS"
autoconf $AUTOCONF_FLAGS || exit 1

if grep "A[MC]_CONFIG_HEADER" configure.ac >/dev/null; then
    echo "Running autoheader"
    autoheader || exit 1
fi

if [ -f Makefile.am ]; then
    echo "Running automake $AUTOMAKE_FLAGS"
    automake $AUTOMAKE_FLAGS || exit 1
fi

if grep "AM_PROG_LIBTOOL" configure.ac >/dev/null; then
    echo "Running libtoolize"
    libtoolize || exit 1
fi

./configure "$@" && ( echo ; echo "Now type \`make' to compile." )


