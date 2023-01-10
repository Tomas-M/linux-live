#!/bin/bash

UNION=$1
SLACKWARE=$2

TMP=/tmp/gcc-root$$
installpkg -root $TMP $SLACKWARE/d/gcc-12*.txz

if [ "$(uname -m)" = "x86_64" ]; then
   ARCH=64
else
   ARCH=""
fi

cp -a $TMP/usr/lib$ARCH/libquadmath.so.* $UNION/usr/lib$ARCH
rm -Rf $TMP

glib-compile-schemas $1/usr/share/glib-2.0/schemas/
