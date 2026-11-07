Slax
====

The CD/DVD is already bootable.

To boot Slax from a USB drive:

1. Copy the entire slax directory from this disc to the root of a USB drive
   formatted as FAT32.

2. Open the slax/boot directory on that USB drive.

3. Run the installer for your operating system:

   Windows: double-click bootinst.bat and approve administrator access.
   Linux:   run sudo sh bootinst.sh from a terminal opened in that directory.
            GRUB's BIOS installation tools must be installed on the Linux system.

4. Wait for the installer to finish. Restart the computer and select the USB
   drive in its boot menu.
