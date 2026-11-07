#!/bin/sh
set -eu
# Configure the existing system tmpfs without hiding any active /tmp files.
awk '$1 ~ /^#/ || ($2 != "/tmp" && $2 != "/var/tmp")' /etc/fstab > /etc/fstab.tmpfs
printf 'tmpfs /tmp tmpfs mode=1777,nosuid,nodev,size=20G,nr_inodes=1m 0 0\n' >> /etc/fstab.tmpfs
printf 'tmpfs /var/tmp tmpfs mode=1777,nosuid,nodev,size=2G 0 0\n' >> /etc/fstab.tmpfs
mv /etc/fstab.tmpfs /etc/fstab
systemctl daemon-reload
mount -o remount,size=20G /tmp
mount -o remount,size=2G /var/tmp
findmnt -T /tmp -o TARGET,FSTYPE,SIZE,USED,OPTIONS
findmnt -T /var/tmp -o TARGET,FSTYPE,SIZE,USED,OPTIONS
grep -E '[[:space:]]/(var/)?tmp[[:space:]]' /etc/fstab
printf '\nSwap stays disabled:\n'
cat /proc/swaps
