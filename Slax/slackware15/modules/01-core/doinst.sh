#!/bin/sh
# $1 = union directory
echo "$SLAXVERSION" >$1/etc/slax-version
touch $1/var/lock/001lock # make sure 001-core bundle will never be deactivated
chmod ago-x $1/etc/rc.d/rc.sshd
ln -s /run/initramfs/bin/busybox $1/usr/bin/vi
mkdir $1/var/spool/rwho

# do not update module dependencies. If user adds new modules, he should run depmod manually
chmod ugo-x $1/etc/rc.d/rc.modules
chmod ugo-x $1/etc/rc.d/rc.setterm

# syslinux c32 files not need
rm -Rf $1/usr/share/syslinux

# unneeded, or wont work without dependency anyway
rm $1/usr/sbin/cifs.idmap

# ntp unneeded stuff, we want only ntpdate
rm $1/usr/sbin/ntpsnmpd
rm $1/usr/sbin/ntpq
rm $1/usr/sbin/ntpdc
rm $1/usr/sbin/ntpd
rm $1/usr/sbin/ntp-keygen

ln -s /run/initramfs/bin/busybox $1/usr/bin/nslookup # better than 2MB big bind

# sg3 utils (needed lib for sdparm, not needed binaries)
rm $1/usr/bin/sg_*
rm $1/usr/bin/sgm_dd
rm $1/usr/bin/sgp_dd

# update ca certificates in current system and copy generated symlinks to union
# apparently the running system must have the certificates database updated
# to the same version as we're installing here in Slax. I can live with that.
rm /etc/ssl/certs/*
/usr/sbin/update-ca-certificates --fresh
cp -a /etc/ssl/certs/* $1/etc/ssl/certs
