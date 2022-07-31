#!/bin/bash

UNION=$1
SLACKWARE=$2

TMP=/tmp/gcc-root$$
installpkg -root $TMP $SLACKWARE/d/gcc-11*.txz
cp -a $TMP/usr/lib64/libquadmath.so.0* $UNION/usr/lib64
rm -Rf $TMP

glib-compile-schemas $1/usr/share/glib-2.0/schemas/
