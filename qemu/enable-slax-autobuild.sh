set -eu
sh -n /root/continue-slax-build.sh
systemd-analyze verify /etc/systemd/system/slax-autobuild.service
cd /root/linux-live
for script in Slax/debian13/build Slax/debian13/buildmods Slax/debian13/cleanup Slax/debian13/modules/*/build Slax/debian13/modules/*/compile; do sh -n "$script"; done
systemctl daemon-reload
systemctl enable slax-autobuild.service
systemctl start --no-block slax-autobuild.service
systemctl show slax-autobuild.service -p ActiveState -p SubState -p MainPID
systemctl is-enabled slax-autobuild.service
tail -n 4 /root/slax-autobuild.log