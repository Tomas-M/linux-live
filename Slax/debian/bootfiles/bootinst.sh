#!/bin/sh
#
#     This script will setup booting from disk (USB or harddrive)
#
#     If you see this file in a text editor instead of getting it executed,
#     then it is missing executable permissions (chmod). You can try to set
#     exec permissions for this file by using:  chmod a+x bootinst.sh
#     Alternatively, you may try to run bootinst.bat file instead
#
#     Scrolling down will reveal the actual code of this script.
#




















































# if we're running this from X, re-run the script in konsole or xterm
if [ "$DISPLAY" != "" ]; then
   if [ "$1" != "--rex" -a "$2" != "--rex" ]; then
      konsole --nofork -e /bin/sh $0 --rex 2>/dev/null || xterm -e /bin/sh $0 --rex 2>/dev/null || /bin/sh $0 --rex 2>/dev/null
      exit
   fi
fi

# make sure I am root
if [ "$UID" != "0" -a "$UID" != "" ]; then
   echo ""
   echo "You are not root. You must run bootinst script as root."
   echo "The bootinst script needs direct access to your boot device."
   echo "Use sudo or kdesudo or similar wrapper to execute this."
   read junk
   exit 1
fi

# change working directory to dir from which we are started
CWD="$(pwd)"
BOOT="$(dirname "$0")"
BOOT="$(realpath "$BOOT" 2>/dev/null || echo $BOOT)"
cd "$BOOT"

# find out device and mountpoint
PART="$(df . | tail -n 1 | tr -s " " | cut -d " " -f 1)"
DEV="$(echo "$PART" | sed -r "s:[0-9]+\$::" | sed -r "s:([0-9])[a-z]+\$:\\1:i")"   #"

# check if disk is already bootable. Mostly for Windows discovery
if [ "$(fdisk -l "$DEV" | fgrep "$DEV" | fgrep "*")" != "" ]; then
   echo ""
   echo "Partition $PART seems to be located on a physical disk,"
   echo "which is already bootable. If you continue, your drive $DEV"
   echo "will boot only Slax by default."
   echo "Press [Enter] to continue, or [Ctrl+C] to abort..."
   read junk
fi

ARCH=$(uname -m)
if [ "$ARCH" = "x86_64" ]; then ARCH=64; else ARCH=32; fi
EXTLINUX=extlinux.x$ARCH

if [ ! -x ./$EXTLINUX ]; then
   # extlinux is not executable. There are two possible reasons:
   # either the fs is mounted with noexec, or file perms are wrong.
   # Try to fix both, no fail on error yet
   mount -o remount,exec $DEV
   chmod a+x ./$EXTLINUX
fi

if [ ! -x ./$EXTLINUX ]; then
   # extlinux is still not executable. As a last try, copy it to .exe
   # because the partition may be mounted with showexec option, from which
   # we probably can't escape by remount
   cp -f ./$EXTLINUX ./extlinux.exe
   EXTLINUX=extlinux.exe
fi

# install syslinux bootloader
echo "* attempting to install bootloader to $BOOT..."

./"$EXTLINUX" --install "$BOOT"

if [ $? -ne 0 ]; then
   echo "Error installing boot loader."
   echo "Read the errors above and press enter to exit..."
   read junk
   exit 1
fi

if [ "$DEV" != "$PART" ]; then
   # Setup MBR on the first block
   echo "* setup MBR on $DEV"
   dd bs=440 count=1 conv=notrunc if="$BOOT/mbr.bin" of="$DEV" 2>/dev/null

   # Toggle bootable flags
   echo "* set bootable flag for $PART"
   PART="$(echo "$PART" | sed -r "s:.*[^0-9]::")"
   (
      fdisk -l "$DEV" | fgrep "*" | fgrep "$DEV" | cut -d " " -f 1 \
        | sed -r "s:.*[^0-9]::" | xargs -I '{}' echo -ne "a\n{}\n"
      echo a
      echo $PART
      echo w
   ) | fdisk $DEV >/dev/null 2>&1
fi

echo "Boot installation finished."
echo "Press Enter..."
read junk
cd "$CWD"
