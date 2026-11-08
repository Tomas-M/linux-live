Slax
====

The CD/DVD is already bootable.

Install Slax to a USB drive or hard disk
--------------------------------------

Open slax/boot on this disc and start the installer:

   Windows XP or later: run bootinst.exe as administrator.
   Slax:                run bootinst as root.

Choose the Slax ISO image and target, then review the installation plan.
No internet connection is needed.

Without "Erase entire disk", supported existing filesystems and files are kept.
An existing slax directory must first be moved or renamed. Non-FAT filesystems
need an existing FAT EFI System Partition on the same disk. Internal-disk
multiboot requires Windows or Slax started in 64-bit UEFI mode. Existing Windows
boot files and entries are retained, and Windows appears in the Slax boot menu.

"Erase entire disk" deletes ALL partitions and files on the physical disk,
including its other drive letters. Review every listed partition carefully.
The installer creates GPT, BIOS boot, FAT32 EFI and FAT32 Slax data partitions.
Native 4096-byte-sector disks support UEFI boot only. When run in Windows XP,
the installer instead creates MBR with FAT32 Slax data first and a small FAT32
EFI partition second, retaining both BIOS and UEFI boot. This XP layout supports
512-byte-sector disks up to 2 TiB; newer Windows and Slax continue to use GPT.

The installer explains unsupported configurations before writing. Keep the
computer powered and both source and target connected until it finishes.
