USB Disk Setup - use the remaining space on your Slax USB drive

If you are reading this file on the DISK-SETUP drive, the Slax download
(an ISO file, also used to create a CD or DVD) was written directly to
your USB flash drive. This lets you start Slax from the USB drive, but
only the space needed by the image is ready to use. On a larger USB drive,
much of its capacity is left unused.

After writing the Slax image, your USB drive is arranged like this:

  START OF USB                                                 END OF USB
  +------------------+-----------+------------+--------------------------+
  | Slax             | UEFI boot | DISK-SETUP | Unused space             |
  | ISO9660          | FAT16     | FAT16      | No partition yet         |
  | ~512MiB          | ~4MiB     | ~2 MiB     | Most of a larger USB     |
  +------------------+-----------+------------+--------------------------+
                                  You are here

  Schematic only: widths are not proportional to actual sizes.
  Small areas used for boot information and spacing are omitted.

You can make this remaining space available for your own files:

- In Windows, double-click disk-setup.exe on this drive. After you confirm,
  it prepares the unused space and makes it available as a new drive named
  USB-DATA. Slax and its boot files are kept. Once the program has checked
  that USB-DATA works, it removes the small DISK-SETUP drive you are reading
  this file from.

- In Linux, use a partition editor to create a new partition in the unused
  space, then format it as FAT32 to share files with both Linux and Windows.
  Keep the existing Slax and UEFI boot partitions. Using the terminal, you
  can use cfdisk command to create the partition, then format it with mkfs.fat.
