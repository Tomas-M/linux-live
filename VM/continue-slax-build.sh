#!/bin/sh
set -eu
exec >> /root/slax-autobuild.log 2>&1
KIT=/root/linux-live
WORK=/var/tmp/slax-build-trixie
. "$KIT/Slax/debian13/aufs-kernel-compile/versions"
export TMPDIR=/tmp DEBIAN_FRONTEND=noninteractive
printf '\n[%s] Automatic build started on %s\n' "$(date -Is)" "$(uname -r)"

# Wait for the existing compiler before reusing its packages.
echo 'Waiting for the running kernel compiler.'
mkdir -p "$WORK/kernel"
flock "$WORK/kernel/compile.lock" true
# Clear the previous completion marker before rebuilding the ISO.
rm -f "$WORK/SHA256SUMS"
sh "$KIT/Slax/debian13/build" trixie "$WORK"
if [ "$(uname -r)" != "$KERNELRELEASE" ]; then
   test -s "/boot/vmlinuz-$KERNELRELEASE"
   test -s "/boot/initrd.img-$KERNELRELEASE"
   echo "Kernel installed; rebooting into $KERNELRELEASE."
   systemctl reboot --no-block
   exit 0
fi

# Publish completion only after the ISO build succeeds.
cd "$WORK"
test -s slax-amd64.iso
sha256sum slax-amd64.iso > SHA256SUMS.new
mv SHA256SUMS.new SHA256SUMS
du -h slax-amd64.iso
systemctl disable slax-autobuild.service
printf '[%s] Slax build completed successfully.\n' "$(date -Is)"
