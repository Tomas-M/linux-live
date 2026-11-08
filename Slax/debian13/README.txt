Slax
====

The CD/DVD is already bootable.

Install Slax to a USB drive or hard disk
--------------------------------------

Open slax on this disc and start the installer:

   Windows XP or later: run slax-install.exe as administrator.
   Slax:                run slax-install-linux as root.

Choose the Slax ISO image and target, then review the installation plan.
No internet connection is needed.

Without erasing, supported existing filesystems and files are kept. An existing
slax directory must first be moved or renamed. BIOS multiboot keeps a backup of
the original loader and adds Slax to a supported existing system menu, keeping
the original system as the default. Other loaders use a separate text selector.
Selecting Slax starts its normal logo and countdown; Escape opens its boot menu.
UEFI uses an existing EFI
partition, or creates one in unused space without moving existing partitions.
If neither is possible, an existing FAT target can hold the EFI files itself.
Existing system boot files and firmware entries are retained. Adding an internal
UEFI boot entry requires a session started in 64-bit UEFI mode; preparing a
separate bootable USB drive does not. The plan explains the available boot modes.

"Erase entire selected disk" deletes ALL partitions and files on the physical disk,
including its other drive letters. Review every listed partition carefully.
The installer creates GPT, BIOS boot, FAT32 EFI and FAT32 Slax data partitions.
Native 4096-byte-sector disks support UEFI boot only. Windows XP, and older Windows
versions limited to one removable partition, use MBR with FAT32 Slax data first
and a small FAT32 EFI partition second. This keeps the data accessible in those
Windows versions while retaining BIOS and UEFI boot. This compatible layout
supports 512-byte-sector disks up to 2 TiB. Other systems normally use GPT.

The installer explains unsupported configurations before writing. Keep the
computer powered and both source and target connected until it finishes.
