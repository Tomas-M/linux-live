Debian work Linux
=================

Run start.cmd to open the work VM in a visible VirtualBox window.
The VM uses 16 virtual CPUs, 24 GB RAM and a 64 GB expandable disk.
Networking is bridged through the active Realtek Ethernet adapter.
Guest swap is disabled permanently, including initramfs resume.

Account: root
The console password is stored locally in credentials.txt.
SSH: ssh -i VM/id_ed25519 root@10.0.0.48
Working checkout inside the VM: /root/linux-live
Use SSH/SFTP to transfer changes between the Windows and Linux checkouts.

The system /tmp is a 20 GB tmpfs. Both kernel and Slax builds use it.
The production kernel excludes DWARF and BTF debug information.
The automatic build installs the kernel, reboots, and builds Slax.
The launcher starts the collector, which downloads the finished ISO
into slax-build-trixie. No periodic status monitor is installed.
The build helper scripts and service definition are included in Git.
For a new work VM, run configure-system-tmp.sh and disable-work-vm-swap.sh
as root. Copy continue-slax-build.sh to /root and slax-autobuild.service
to /etc/systemd/system, then run enable-slax-autobuild.sh.

The active VM disk is virtualbox/debian-work.vdi. Shut Debian down normally
before closing VirtualBox or backing up its disk. VM files and credentials
stay local and are excluded from Git.

The local start-vbox-boot-test.ps1 helper starts the Slax boot test VM
from the current ISO without a hard disk. Attach disposable test media
explicitly when testing persistent sessions; never use the work VM disk.
