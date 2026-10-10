Debian work Linux
=================

Run start.cmd to open the VM in a visible QEMU window.
Shut down Debian normally before closing QEMU.

The VM uses 4 CPU cores, 4 GB RAM and a 64 GB expandable disk.
Software emulation is used because Windows Hypervisor is not enabled.

Account: root
The generated console password is stored locally in credentials.txt.
SSH: ssh -i qemu/id_ed25519 -p 2222 root@127.0.0.1
Working checkout inside the VM: /root/linux-live
Use SSH/SFTP to transfer changes between the Windows and Linux checkouts.

Local GRUB boot test images use a minimal BusyBox filesystem to verify
the Live Kit boot process. Results are recorded in boot-test-results.txt.

QEMU runtime, Debian ISO, VM disk and credentials stay local and are
excluded from Git. Keep a backup of debian.qcow2 while the VM is off.
