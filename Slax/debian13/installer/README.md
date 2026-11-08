# Slax Installer

`../bootfiles/bootinst` runs in Slax. `bootinst.exe` is a standalone 32-bit
Windows executable for Windows XP and later. Both perform real installations.
The native GTK 3 and Win32 wizard retains Slax branding, drive cards and the
fixed step sidebar. Default client size is 832 × 480 logical pixels; minimum
is 720 × 400. Windows can be resized and maximized normally.
The ISO path supports typing and clipboard paste. Typing and Browse only fill
the path; Next starts asynchronous ISO validation. An automatically detected
source ISO is validated immediately and opens the target step on success.
Back returns to the editable path without advancing again; invalid automatic
sources remain on the ISO step with an explanation. Cancel has a wider gap
from the Back/Next pair.
Windows reads native ISO attachment metadata when available (Windows 8 and
later); Linux follows the executable's ISO loop device to its backing file.
If the original file cannot be resolved, the path remains available for manual
entry. Explicit command-line paths take precedence. Validation feedback keeps
summary, reason and recovery on separate lines beside a muted-red square.

## Targets and installation

Discovery runs outside the GUI thread. It lists internal and USB physical
disks, their individual partitions and Windows drive letters, including disks
with no partition table. The menu shows partitions strictly larger than
1,000,000,000 bytes. A whole disk is hidden when it has any partition above
this limit. Unformatted partitions are hidden when a formatted partition above
the limit exists on the same disk. Complete discovery metadata is retained for
preflight and whole-disk erase warnings. Bare disks and disks with only small
partitions remain whole-disk targets, labeled #0, #1, etc. on Windows. Lists
expand with the window instead of stopping at a fixed height.
Windows sorts drive letters alphabetically before the remaining targets;
Linux sorts by full device path.
Linux badges show complete device paths such as `/dev/sda2` and
`/dev/nvme0n1p2`. Their shared width follows the text and preserves aligned
device descriptions; unusually long paths wrap within the badge.
Targets need at least 1 GB total capacity and enough
actual free space for the selected payload. Optical, network, loop and zram
storage are not installation targets. Read-only and layered storage report
why they cannot be used. All sibling partitions remain available to the
confirmation even when they are too small to hold Slax.

Without **Erase entire disk**, existing partitions and files are retained.
The installer creates a new `slax` directory, verifies every copied file with
SHA-256 and publishes it after copying completes. An existing `slax` directory
is never replaced; it may contain saved sessions. Rename or move it first.

FAT filesystems can host their own removable UEFI loaders. NTFS, ext2/3/4 and
XFS require an existing writable FAT EFI System Partition on the same disk.
Btrfs and exFAT additionally require a Slax ISO advertising their initramfs
drivers in `slax/boot/grub/filesystems.txt`. Windows can preserve only volumes
its own filesystem drivers can access. Multi-device Btrfs, RAID, LVM, dynamic
volumes and storage pools are rejected rather than altering their metadata.
Encrypted volumes and filesystems without a stable, unique UUID are rejected.

Internal-disk multiboot requires a session booted using 64-bit UEFI and writable
firmware variables. A separate `EFI/Slax/<installation-id>` contains the signed
shim, signed GRUB, kernel, initramfs and full menu. Microsoft boot files, BCD
and existing firmware entries are retained. Slax is added first in BootOrder;
its complete menu has a visible 10-second timeout and a Windows Boot Manager
entry when present. Secure Boot stays enabled. An unknown removable fallback
loader is not overwritten. An identical Windows fallback can be backed up
atomically before the Slax fallback is published; Microsoft files stay intact.

A simple non-system MBR FAT partition with at least 1 MiB before it can also
receive BIOS GRUB without changing its partition table. Existing Windows BIOS
loaders are not replaced. Other preserve configurations are UEFI-only, as
explained in the confirmation. NTFS boot and persistence sources are mounted
read-only first and remounted writable only after checking the Windows
hibernation header; a failed check or remount leaves them read-only. No force
mount option is used. Linux installation checks the Windows hibernation header
before obtaining writable access and refuses active or unverifiable hibernation.

With **Erase entire disk**, the physical parent is repartitioned, including
all its other partitions. The confirmation lists their names and drive letters.
The new GPT contains a 1 MiB BIOS boot partition, a FAT32 ESP (256 MiB with
512-byte sectors, 512 MiB with 4096-byte sectors), and one FAT32 data partition
using the remaining capacity. Native formatting supports FAT32 above 32 GiB,
within its sector and cluster limits. 512-byte disks support BIOS and UEFI;
native 4096-byte disks support UEFI.
When the installer runs in Windows XP (including XP x64), erasure instead
creates MBR with an active FAT32 Slax data partition first and a small FAT32
ESP second (type 0xEF). GRUB BIOS uses the reserved gap before the data
partition; signed UEFI loaders remain in the ESP. Data comes first so XP can
expose it on removable USB media. This XP-only layout requires 512-byte
logical sectors and a disk no larger than 2 TiB. Newer Windows and Linux keep
GPT even when booted in BIOS mode; compatibility mode does not select XP.

The running system/live-session disk and disks containing the ISO or installer
cannot be erased. All sibling filesystems must be safely unmounted/locked
before raw writes begin. Disk handles remain pinned across confirmation;
identity, layout, geometry, media-generation counters when available, source
identity, space and firmware state are rechecked. Closing cannot interrupt a
running installation. Errors identify possible partial Slax files or partial
erasure; a hardware failure or power loss cannot be made transactional.

## Runtime and build

Windows uses only system DLLs, including XP's GDI+. C++ and threading runtimes
are linked statically. Linux uses GTK 3, GLib, libudev, libblkid and standard
C/C++ libraries already present in Slax, plus its stock GRUB BIOS tools.
No browser, .NET, WebView, PowerShell, WMI, VBS, downloaded font or internet
connection is required. Administrator/root access is required for installation.
New Windows APIs are loaded only when available; the PE subsystem is 5.1.

The ISO9660/Rock Ridge reader inventories the complete Slax tree without
mounting the ISO. It rejects unsafe names, links, duplicate aliases and invalid
extents. An ISO filename can be supplied as the only argument.
`--list-devices [ISO]` and `--check-iso FILE` are read-only diagnostics.

Build both binaries on Debian:

```sh
apt-get install --no-install-recommends g++ g++-mingw-w64-i686 python3 pkg-config libgtk-3-dev libudev-dev libblkid-dev upx-ucl
sh Slax/debian13/installer/build
```

Both executables are stripped, packed with `upx --best` and checked with
`upx -t`; ultra-brute compression is not used. The Windows build embeds the
official GNU GRUB 2.12 mkimage utility and its matching i386-pc modules.
The installer build requires internet access: it downloads the release ZIP
from GNU's kernel.org mirror and verifies its pinned SHA256 before embedding
the tools directly from the archive. These binary inputs are not stored in
the repository. The ISO contains their embedded copy in `bootinst.exe`, with
license and provenance under `slax/boot/grub`.
Ordinary Slax builds copy the prebuilt executables; rebuild both after changes.
The ICO comes from https://www.slax.org/favicon.ico and the original PNGs from
the supplied wizard mockup.

## Verification

```sh
apt-get install --no-install-recommends xorriso
sh Slax/debian13/installer/check /path/to/slax.iso
```

The sanitizer suite exercises real ISO parsing, extraction, GPT/FAT32 writes,
4 KiB sectors, Unicode, capacity limits, existing-file preservation, Windows
multiboot, firmware rollback, disconnection and native-wizard confirmation
handoff. Writes are confined to owned sparse files with fake firmware.
`test-native.cpp` additionally tests real Linux storage APIs, accepting only
owned loop devices backed by `/tmp/slax-installer-native.*` fixtures.

Optional discovery tests use mocked hardware and an owned RAM-backed loop:

```sh
apt-get install --no-install-recommends umockdev gir1.2-umockdev-1.0 python3-gi e2fsprogs
sh Slax/debian13/installer/check-linux
```

Destructive Windows tests belong only in explicitly dedicated VMs. Never run
GUI confirmation automation against host drives. Boot tests must cover BIOS,
UEFI Secure Boot, and both Slax and Windows entries after a preserve install.
