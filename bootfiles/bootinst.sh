#!/bin/sh
# Setup booting from disk (USB or harddrive)
# Requires: fdisk, df, tail, tr, cut, dd, sed

# change working directory to dir from which we are started
CWD="$(pwd)"
BOOT="$(dirname "$0")"
cd "$BOOT"

# find out device and mountpoint
PART="$(df . | tail -n 1 | tr -s " " | cut -d " " -f 1)"
DEV="$(echo "$PART" | sed -r "s:[0-9]+\$::" | sed -r "s:([0-9])[a-z]+\$:\\1:i")"   #"

# Try to use installed extlinux binary and fallback to extlinux.exe only
# if no installed extlinux is not found at all.
EXTLINUX="$(which extlinux 2>/dev/null)"
if [ "$EXTLINUX" = "" ]; then
   EXTLINUX="./extlinux.exe"
fi

"$EXTLINUX" --install "$BOOT"

if [ $? -ne 0 ]; then
   echo "Error installing boot loader."
   echo "Read the errors above and press enter to exit..."
   read junk
   exit 1
fi


if [ "$DEV" != "$PART" ]; then
   # Setup MBR on the first block
   dd bs=440 count=1 conv=notrunc if="$BOOT/mbr.bin" of="$DEV" 2>/dev/null

   # Toggle a bootable flag
   PART="$(echo "$PART" | sed -r "s:.*[^0-9]::")"
   (
      fdisk -l "$DEV" | fgrep "*" | fgrep "$DEV" | cut -d " " -f 1 \
        | sed -r "s:.*[^0-9]::" | xargs -I '{}' echo -ne "a\n{}\n"
      echo -ne "a\n$PART\nw\n"
   ) | fdisk $DEV >/dev/null 2>&1
fi

echo "Boot installation finished."
cd "$CWD"
