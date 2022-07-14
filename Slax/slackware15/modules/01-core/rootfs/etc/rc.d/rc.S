#!/bin/sh
#
# /etc/rc.d/rc.S:  System initialization script.
#
# Mostly written by:  Patrick J. Volkerding, <volkerdi@slackware.com>
#

PATH=/sbin:/usr/sbin:/bin:/usr/bin

# Try to mount /proc:
/sbin/mount -v proc /proc -n -t proc 2> /dev/null

# Mount sysfs next, if the kernel supports it:
if [ -d /sys ]; then
  if grep -wq sysfs /proc/filesystems ; then
    if ! grep -wq sysfs /proc/mounts ; then
      /sbin/mount -v sysfs /sys -n -t sysfs
    fi
  fi
fi

# If /run exists, mount a tmpfs on it (unless the
# initrd has already done so):
if [ -d /run ]; then
  if ! grep -wq "tmpfs /run tmpfs" /proc/mounts ; then
    /sbin/mount -v -n -t tmpfs tmpfs /run -o mode=0755
  fi
fi

# Load the loop device kernel module:
if [ -x /etc/rc.d/rc.loop ]; then
  . /etc/rc.d/rc.loop start
fi

# Initialize udev to manage /dev entries and hotplugging for 3.x kernels.
# You may turn off udev by making the /etc/rc.d/rc.udev file non-executable
# or giving the "nohotplug" option at boot, but realize that if you turn off
# udev that you will have to load all the kernel modules that you need
# yourself (possibly in /etc/rc.d/rc.modules, which does not promise to list
# all of them), and make any additional device nodes that you need in the
# /dev directory.  Even USB and IEEE1394 devices will need to have the
# modules loaded by hand if udev is not used.  So use it.  :-)
if grep -wq sysfs /proc/mounts && grep -q tmpfs /proc/filesystems ; then
  if ! grep -wq nohotplug /proc/cmdline ; then
    if [ -x /etc/rc.d/rc.udev ]; then
      /bin/sh /etc/rc.d/rc.udev start
    fi
  fi
fi

# Mount Control Groups filesystem interface:
if grep -wq cgroup /proc/filesystems ; then
  if [ -d /sys/fs/cgroup ]; then
    mount -t cgroup cgroup /sys/fs/cgroup
  else
    mkdir -p /dev/cgroup
    mount -t cgroup cgroup /dev/cgroup
  fi
fi

# Initialize the Logical Volume Manager.
# This won't start unless we find /etc/lvmtab (LVM1) or
# /etc/lvm/backup/ (LVM2).  This is created by /sbin/vgscan, so to
# use LVM you must run /sbin/vgscan yourself the first time (and
# create some VGs and LVs).
if [ -r /etc/lvmtab -o -d /etc/lvm/backup ]; then
  echo "Initializing LVM (Logical Volume Manager):"
  # Check for device-mapper support.
  if ! grep -wq device-mapper /proc/devices ; then
    # Try to load a device-mapper kernel module:
    /sbin/modprobe -q dm-mod
  fi
  # Scan for new volume groups:
  /sbin/vgscan --mknodes --ignorelockingfailure 2> /dev/null
  if [ $? = 0 ]; then
    # Make volume groups available to the kernel.
    # This should also make logical volumes available.
    /sbin/vgchange -ay --ignorelockingfailure
  fi
fi

# Open any volumes created by cryptsetup:
if [ -f /etc/crypttab -a -x /sbin/cryptsetup ]; then
  # First, check for device-mapper support.
  if ! grep -wq device-mapper /proc/devices ; then
    # If device-mapper exists as a module, try to load it.
    # Try to load a device-mapper kernel module:
    /sbin/modprobe -q dm-mod
  fi
  # NOTE: we only support LUKS formatted volumes (except for swap)!
  cat /etc/crypttab | grep -v "^#" | grep -v "^$" | while read line; do
    eval LUKSARRAY=( $line )
    LUKS="${LUKSARRAY[0]}"
    DEV="${LUKSARRAY[1]}"
    PASS="${LUKSARRAY[2]}"
    OPTS="${LUKSARRAY[3]}"
    LUKSOPTS=""
    if echo $OPTS | grep -wq ro ; then LUKSOPTS="${LUKSOPTS} --readonly" ; fi

    # Skip LUKS volumes that were already unlocked (in the initrd):
    /sbin/cryptsetup status $LUKS 2>/dev/null | head -n 1 | grep -q "is active" && continue
    if /sbin/cryptsetup isLuks $DEV 2>/dev/null ; then
      echo "Unlocking LUKS crypt volume '${LUKS}' on device '$DEV':"
      if [ -n "${PASS}" ]; then
        if [ -f ${PASS} ]; then
          /sbin/cryptsetup ${LUKSOPTS} --key-file=${PASS} luksOpen $DEV $LUKS
        elif [ "${PASS}" != "none" ]; then
          # A password field of 'none' indicates a line for swap:
          echo "${PASS}" | /sbin/cryptsetup ${LUKSOPTS} luksOpen $DEV $LUKS
        fi
      else
        /sbin/cryptsetup ${LUKSOPTS} luksOpen $DEV $LUKS </dev/tty0 >/dev/tty0 2>&1
      fi
    elif echo $OPTS | grep -wq swap ; then
      # If any of the volumes is to be used as encrypted swap,
      # then encrypt it using a random key and run mkswap:
      echo "Creating encrypted swap on device '$DEV' mapped to '${LUKS}':"
      /sbin/cryptsetup --cipher=aes --key-file=/dev/urandom --key-size=256 create $LUKS $DEV
      mkswap /dev/mapper/$LUKS
    fi
  done
fi

# Enable swapping:
/sbin/swapon -a 2> /dev/null

# Start FUSE, if requested:
if [ -x /etc/rc.d/rc.fuse ]; then
  sh /etc/rc.d/rc.fuse start
fi

# Set the system time from the hardware clock using hwclock --hctosys.
if [ -x /sbin/hwclock ]; then
  # Check for a broken motherboard RTC clock (where ioports for rtc are
  # unknown) to prevent hwclock causing a hang:
  if ! grep -q -w rtc /proc/ioports ; then
    CLOCK_OPT="--directisa"
  fi
  if grep -wq "^UTC" /etc/hardwareclock ; then
    echo -n "Setting system time from the hardware clock (UTC): "
    /sbin/hwclock $CLOCK_OPT --utc --hctosys
  else
    echo -n "Setting system time from the hardware clock (localtime): "
    /sbin/hwclock $CLOCK_OPT --localtime --hctosys
  fi
  date
fi

# Test to see if the root partition is read-only, like it ought to be.
READWRITE=no
if touch /fsrwtestfile 2>/dev/null; then
  rm -f /fsrwtestfile
  READWRITE=yes
else
  echo "Testing root filesystem status:  read-only filesystem"
fi

# See if a forced filesystem check was requested at shutdown:
if [ -r /etc/forcefsck ]; then
  FORCEFSCK="-f"
fi

# Check the root filesystem:
if [ ! $READWRITE = yes ]; then
  RETVAL=0
  if [ ! -r /etc/fastboot ]; then
    echo "Checking root filesystem:"
    /sbin/fsck $FORCEFSCK -C -a /
    RETVAL=$?
  fi
  # An error code of 2 or higher will require a reboot.
  if [ $RETVAL -ge 2 ]; then
    # An error code equal to or greater than 4 means that some errors
    # could not be corrected.  This requires manual attention, so we
    # offer a chance to try to fix the problem in single-user mode:
    if [ $RETVAL -ge 4 ]; then
      echo
      echo "***********************************************************"
      echo "*** An error occurred during the root filesystem check. ***"
      echo "*** You will now be given a chance to log into the      ***"
      echo "*** system in single-user mode to fix the problem.      ***"
      echo "***                                                     ***"
      echo "*** If you are using the ext2 filesystem, running       ***"
      echo "*** 'e2fsck -v -y <partition>' might help.              ***"
      echo "***********************************************************"
      echo
      echo "Once you exit the single-user shell, the system will reboot."
      echo
      PS1="(Repair filesystem) \#"; export PS1
      sulogin
    else # With an error code of 2 or 3, reboot the machine automatically:
      echo
      echo "***********************************"
      echo "*** The filesystem was changed. ***"
      echo "*** The system will now reboot. ***"
      echo "***********************************"
      echo
    fi
    echo "Unmounting file systems."
    /sbin/umount -a -r
    /sbin/mount -n -o remount,ro /
    echo "Rebooting system."
    sleep 2
    reboot -f
  fi
  # Remount the root filesystem in read-write mode
  echo "Remounting root device with read-write enabled."
  /sbin/mount -w -v -n -o remount /
  if [ $? -gt 0 ] ; then
    echo
    echo "Attempt to remount root device as read-write failed!  This is going to"
    echo "cause serious problems."
    echo 
    echo "If you're using the UMSDOS filesystem, you **MUST** mount the root partition"
    echo "read-write!  You can make sure the root filesystem is getting mounted "
    echo "read-write with the 'rw' flag to Loadlin:"
    echo
    echo "loadlin vmlinuz root=/dev/hda1 rw   (replace /dev/hda1 with your root device)"
    echo
    echo "Normal bootdisks can be made to mount a system read-write with the rdev command:"
    echo
    echo "rdev -R /dev/fd0 0"
    echo
    echo "You can also get into your system by using a boot disk with a command like this"
    echo "on the LILO prompt line:  (change the root partition name as needed)"
    echo 
    echo "LILO: mount root=/dev/hda1 rw"
    echo
    echo "Please press ENTER to continue, then reboot and use one of the above methods to"
    echo -n "get into your machine and start looking for the problem. " 
    read junk; 
  fi
else
  echo "Testing root filesystem status:  read-write filesystem"
  echo
  echo "*** ERROR: Root partition has already been mounted read-write. Cannot check!"
  echo
  echo "For filesystem checking to work properly, your system must initially mount"
  echo "the root partition as read only. Please modify your kernel with 'rdev' so that"
  echo "it does this. If you're booting with LILO, add a line:"
  echo
  echo "   read-only"
  echo
  echo "to the Linux section in your /etc/lilo.conf and type 'lilo' to reinstall it."
  echo
  echo "If you boot from a kernel on a floppy disk, put it in the drive and type:"
  echo "   rdev -R /dev/fd0 1"
  echo
  echo "If you boot from a bootdisk, or with Loadlin, you can add the 'ro' flag."
  echo
  echo "This will fix the problem *AND* eliminate this annoying message. :^)"
  echo
  echo -n "Press ENTER to continue. "
  read junk;
fi # Done checking root filesystem


# Any /etc/mtab that exists here is old, so we start with a new one:
/bin/rm -f /etc/mtab{,~,.tmp} && /bin/touch /etc/mtab

# Add entry for / to /etc/mtab:
/sbin/mount -f -w /

# Add /proc and /sys mounts to /etc/mtab:
if [ -d /proc/sys ]; then
  /sbin/mount -f proc /proc -t proc
fi
if [ -d /sys/bus ]; then
  /sbin/mount -f sysfs /sys -t sysfs
fi

# Configure ISA Plug-and-Play devices:
if [ -r /etc/isapnp.conf ]; then
  if [ -x /sbin/isapnp ]; then
    /sbin/isapnp /etc/isapnp.conf
  fi
fi

# This loads any kernel modules that are needed.  These might be required to
# use your ethernet card, sound card, or other optional hardware.
# Priority is given first to a script named "rc.modules.local", then
# to "rc.modules-$FULL_KERNEL_VERSION", and finally to the plain "rc.modules".
# Note that if /etc/rc.d/rc.modules.local is found, then that will be the ONLY
# rc.modules script the machine will run, so make sure it has everything in
# it that you need.
if [ -x /etc/rc.d/rc.modules.local -a -r /proc/modules ]; then
  echo "Running /etc/rc.d/rc.modules.local:"
  /bin/sh /etc/rc.d/rc.modules.local
elif [ -x /etc/rc.d/rc.modules-$(uname -r) -a -r /proc/modules ]; then
  echo "Running /etc/rc.d/rc.modules-$(uname -r):"
  . /etc/rc.d/rc.modules-$(uname -r)
elif [ -x /etc/rc.d/rc.modules -a -r /proc/modules -a -L /etc/rc.d/rc.modules ]; then
  echo "Running /etc/rc.d/rc.modules -> $(readlink /etc/rc.d/rc.modules):"
  . /etc/rc.d/rc.modules
elif [ -x /etc/rc.d/rc.modules -a -r /proc/modules ]; then
  echo "Running /etc/rc.d/rc.modules:"
  . /etc/rc.d/rc.modules
fi

# Configure runtime kernel parameters:
if [ -x /sbin/sysctl -a -r /etc/sysctl.conf ]; then
  /sbin/sysctl -e -p /etc/sysctl.conf
fi

# Check all the non-root filesystems:
if [ ! -r /etc/fastboot ]; then
  echo "Checking non-root filesystems:"
  /sbin/fsck $FORCEFSCK -C -R -A -a
fi

# Mount usbfs only if it is found in /etc/fstab:
if grep -wq usbfs /proc/filesystems; then
  if ! grep -wq usbfs /proc/mounts ; then
    if grep -wq usbfs /etc/fstab; then
      /sbin/mount -v /proc/bus/usb
    fi
  fi
fi

# Mount non-root file systems in fstab, but not NFS or SMB 
# because TCP/IP is not yet configured, and not proc or sysfs
# because those have already been mounted.  Also check that
# devpts is not already mounted before attempting to mount
# it.  With a 2.6.x or newer kernel udev mounts devpts.
# We also need to wait a little bit to let USB and other
# hotplugged devices settle (sorry to slow down the boot):
echo "Mounting non-root local filesystems:"
# TM: we do not need this
#sleep 3
if /bin/grep -wq devpts /proc/mounts ; then
  /sbin/mount -a -v -t nonfs,nosmbfs,nocifs,noproc,nosysfs,nodevpts
else
  /sbin/mount -a -v -t nonfs,nosmbfs,nocifs,noproc,nosysfs
fi

# Enable swapping again.  This is needed in case a swapfile is used,
# as it can't be enabled until the filesystem it resides on has been
# mounted read-write.
/sbin/swapon -a 2> /dev/null

# Clean up some temporary files:
rm -f /var/run/* /var/run/*/* /var/run/*/*/* /etc/nologin \
  /etc/dhcpc/*.pid /etc/forcefsck /etc/fastboot \
  /var/state/saslauthd/saslauthd.pid \
  /tmp/.Xauth* 1> /dev/null 2> /dev/null
  ( cd /var/log/setup/tmp && rm -rf * )
  ( cd /tmp && rm -rf kde-[a-zA-Z]* ksocket-[a-zA-Z]* hsperfdata_[a-zA-Z]* plugtmp* )

# Create /tmp/{.ICE-unix,.X11-unix} if they are not present:
if [ ! -e /tmp/.ICE-unix ]; then
  mkdir -p /tmp/.ICE-unix
  chmod 1777 /tmp/.ICE-unix
fi
if [ ! -e /tmp/.X11-unix ]; then
  mkdir -p /tmp/.X11-unix
  chmod 1777 /tmp/.X11-unix
fi

# Create a fresh utmp file:
touch /var/run/utmp
chown root:utmp /var/run/utmp
chmod 664 /var/run/utmp

# Update the current kernel level in the /etc/motd (Message Of The Day) file,
# if the first line of that file begins with the word 'Linux'.
# You are free to modify the rest of the file as you see fit.
if [ -x /bin/sed ]; then
  /bin/sed -i "{1s/^Linux.*/$(/bin/uname -sr)\./}" /etc/motd
fi

# If there are SystemV init scripts for this runlevel, run them.
if [ -x /etc/rc.d/rc.sysvinit ]; then
  . /etc/rc.d/rc.sysvinit
fi

# Run serial port setup script:
# CAREFUL!  This can make some systems hang if the rc.serial script isn't
# set up correctly.  If this happens, you may have to edit the file from a
# boot disk, and/or set it as non-executable:
if [ -x /etc/rc.d/rc.serial ]; then
  sh /etc/rc.d/rc.serial start
fi

# Carry an entropy pool between reboots to improve randomness.
if [ -f /etc/random-seed ]; then
  echo "Using /etc/random-seed to initialize /dev/urandom."
  cat /etc/random-seed > /dev/urandom
fi
# Use the pool size from /proc, or 512 bytes:
if [ -r /proc/sys/kernel/random/poolsize ]; then
  dd if=/dev/urandom of=/etc/random-seed count=1 bs=$(cat /proc/sys/kernel/random/poolsize) 2> /dev/null
else
  dd if=/dev/urandom of=/etc/random-seed count=1 bs=512 2> /dev/null
fi
chmod 600 /etc/random-seed

