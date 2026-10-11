#!/bin/sh
set -eu
swapoff -a
awk '$1 ~ /^#/ || $3 != "swap"' /etc/fstab > /etc/fstab.noswap
mv /etc/fstab.noswap /etc/fstab
systemctl mask swap.target
systemctl daemon-reload
# Prevent the initramfs from looking for a hibernation image in the old swap.
printf 'RESUME=none\n' > /etc/initramfs-tools/conf.d/resume
update-initramfs -u -k all
printf '\nSwap after disabling:\n'
cat /proc/swaps
free -h
systemctl is-enabled swap.target || true
printf '\nPermanent mount configuration:\n'
cat /etc/fstab
