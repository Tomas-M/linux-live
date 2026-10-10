Unmodified GNU GRUB 2.12 Windows BIOS installer
=============================================

grub-install.exe and i386-pc files come from the official GNU release:
https://ftp.gnu.org/gnu/grub/grub-2.12-for-windows.zip
https://ftp.gnu.org/gnu/grub/grub-2.12-for-windows.zip.sig

The detached signature was verified against the release key fingerprint:
BE5C 2320 9ACD DACE B20D B0A2 8C81 89F1 988C 2166
Published by the maintainer in the official release announcement:
https://lists.gnu.org/archive/html/grub-devel/2023-12/msg00052.html

SHA256 (official ZIP):
e98bf808fcb6836a7e50d0f9483900116b28c46c0b9743cc956e314a7fdea9bd
SHA256 (grub-install.exe):
99cf1213badaa91d7536c21d609e9f13478ecf23c2915ce4aa63ddbe8e2629de

Only the BIOS installer and its matching i386-pc images/modules are included.
Debugger helpers, EFI platforms, fonts, locales and other utilities are omitted.
The build generates EFI loaders separately using the build host's GRUB tools.
The generated kit bundles those loaders in boot/EFI/BOOT. bootinst uses
explicit paths to this EXE and its matching BIOS modules; it does not need
GRUB installed on Windows, a GRUB PATH entry or any runtime downloads.
The EXE imports only Windows system DLLs: ADVAPI32, KERNEL32, msvcrt,
SHELL32 and USER32. No separate runtime DLL package is required.
bootinst.bat uses Windows Script Host (VBScript and WMI), available from
Windows XP onward. PowerShell is not required. Run with administrator rights;
Vista and later request elevation through the Windows shell.
MBR keeps the FAT partition type so XP can still access the USB drive.
GPT setup needs Windows 7 or later; Windows 8 and later use Storage WMI,
while Windows 7 uses the system DiskPart utility.
VBScript must be enabled if Windows Script Host or its engine was removed.

License: GNU GPL version 3 or later, reproduced in COPYING.
The corresponding unmodified source code is available from GNU:
https://ftp.gnu.org/gnu/grub/grub-2.12.tar.xz
https://ftp.gnu.org/gnu/grub/grub-2.12.tar.xz.sig
