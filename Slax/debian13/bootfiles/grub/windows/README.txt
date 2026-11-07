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
Slax supplies Debian's signed shim and GRUB EFI loaders separately.
The Slax media bundles those loaders in slax/boot/EFI/BOOT. These BIOS tools
remain available for the native installer's later boot-writing implementation.
The current bootinst.exe preview only simulates installation and does not run
grub-install.exe or change the target disk.
The EXE imports only Windows system DLLs: ADVAPI32, KERNEL32, msvcrt,
SHELL32 and USER32. No separate runtime DLL package is required.
The native preview targets Windows XP onward and uses standard Windows
controls and storage APIs. It does not require administrator access,
VBScript, WMI, PowerShell or additional runtime DLLs.

License: GNU GPL version 3 or later, reproduced in COPYING.
The corresponding unmodified source code is available from GNU:
https://ftp.gnu.org/gnu/grub/grub-2.12.tar.xz
https://ftp.gnu.org/gnu/grub/grub-2.12.tar.xz.sig
