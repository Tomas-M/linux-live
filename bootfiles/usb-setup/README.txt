Disk Setup - make the unused USB space available

After a live ISO image is written directly to a USB drive, part of the drive
may remain unused. Disk Setup turns that remaining space into a normal FAT32
drive named USB-DATA, accessible from Windows and Linux.

How to use it

1. In Windows 10 or 11, open the DISK-SETUP drive.
2. Double-click disk-setup.exe and allow administrator access.
3. Check the USB drive name and the size of the new data drive.
4. Click OK to prepare the space, or Cancel to leave the disk unchanged.
5. Keep the USB drive connected until the completion message appears.

The completion message shows the new drive letter. The live system, boot files
and DISK-SETUP partition are preserved. Any old contents in the unused space
are replaced when the new filesystem is created.

Which disk is changed

Disk Setup works only on the physical USB disk containing the executable.
There is no disk picker, and it never chooses another disk automatically.
Run it directly from DISK-SETUP, not from a copy on your Windows disk or another
USB drive. A system or boot disk, an unsupported live image, an unexpected
partition layout, or a disk without enough unused space is refused.

Unused space means the unallocated space after the flashed live image. It does
not mean free space inside an existing partition. Other existing partitions
are not resized or reformatted. Repeating setup leaves USB-DATA unchanged.

Requirements and limits

- Windows 10 or later with administrator access.
- A supported Linux Live Kit hybrid image written directly to a USB disk.
- 512-byte logical sectors and between 64 MiB and 2 TiB of remaining space.
- FAT32 supports individual files smaller than 4 GiB.

The executable is a native 32-bit Win32 application using Windows system DLLs.
It does not need .NET, PowerShell, a compiler or an extra runtime installation.
Nothing is compiled on your computer. Its FAT32 formatter also handles drives
larger than Windows' traditional 32 GiB formatting limit.
