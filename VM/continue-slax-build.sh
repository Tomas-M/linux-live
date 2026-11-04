#!/bin/sh
set -eu
exec >> /tmp/slax-autobuild.log 2>&1
KIT=/root/linux-live
WORK=/var/tmp/slax-build-trixie
export TMPDIR=/tmp DEBIAN_FRONTEND=noninteractive
printf '\n[%s] Automatic build started on %s\n' "$(date -Is)" "$(uname -r)"

# Clear the previous completion marker before rebuilding the ISO.
rm -f "$WORK/SHA256SUMS"
sh "$KIT/Slax/debian13/build" trixie "$WORK"
# Publish completion only after the ISO build succeeds.
cd "$WORK"
test -s slax-amd64.iso
sha256sum slax-amd64.iso > SHA256SUMS.new
mv SHA256SUMS.new SHA256SUMS
du -h slax-amd64.iso
systemctl disable slax-autobuild.service
printf '[%s] Slax build completed successfully.\n' "$(date -Is)"
