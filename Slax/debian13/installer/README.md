# Slax Installer

`../installers/slax-install-linux` runs in Slax. `slax-install.exe` is a standalone 32-bit
Windows executable for Windows XP and later. Both perform real installations.
The ISO places both executables in `/slax/installer`.
The desktop module also installs the unpacked Linux executable in `/usr/bin`
and provides a Slax Installer entry in the xlunch application menu.
Its icon combines the original Slax clover, a hard drive, a down arrow and a USB
drive. Bundled icon sizes stop at 64 pixels.
The GTK 3 and Win32 wizard uses native windows, controls and dialogs, with Slax
branding and a permanent sidebar. Initial client size is 832 × 540 logical
pixels, fitting four normal drive cards with a small reserve; minimum is
720 × 400. The list expands when the window grows.

## Wizard

The wizard always starts with Welcome, explaining source selection, USB or local
disk installation, preservation or whole-disk erasure and confirmation before
writes. Both frontends share the same copy and five steps: Welcome, Slax source,
Target drive, Installation and Finish. Next opens source selection; Back returns
to Welcome without clearing the source. Automatic source checks may finish in
the background but never advance Welcome or show an error dialog above it.

The source path supports typing and paste. Browse opens a native menu with
ISO image and Slax directory choices and a visible dropdown arrow. Align the
menu's right edge with Browse on both platforms. Both native
folder pickers accept the Slax directory itself or a parent containing `slax`;
validation normalizes the path and inventories the same required boot files and
modules. Directory copies retain original file paths when creating EFI assets.
Edited paths are validated after a 300 ms pause in typing. Browse and automatic
source detection validate immediately. Next stays disabled while checking, but
the source path and Browse remain usable. Superseded results are discarded, and
background validation never changes the path being edited or opens error dialogs.
Next revalidates the source and displays any error in the native dialog.
An automatically detected source is checked immediately; its metadata
appears when source selection opens. Only the user's Next opens target selection.
Back returns to the editable source path.
Source validation failures open a native modal error dialog with the reason and
recovery instructions. Dismissing it keeps the source path editable on step 2;
the page itself shows only source metadata, preceded by a small green check after
successful validation. Editing the path or choosing another source clears both
the metadata and check until that source passes validation. Short source and target check status
appears in a fixed footer slot aligned with the content; it never moves the drive
list or whole-disk erase checkbox.
Windows uses optional native VirtDisk attachment metadata; Linux follows the
ISO loop device. On Linux, the resolved executable's filesystem also identifies
its optical device through udev, including execution through symlinks or bind
mounts from any working directory. The installer reads that device directly as
an ISO and validates it with the normal Slax payload checks. Optical
aliases such as `/dev/cdrom` are accepted too; other raw block devices are not.
Missing metadata leaves the path available for manual entry.
When launched from the installed application menu entry, Linux checks the Slax data
under `/run/initramfs/memory/toram`, `iso/slax` and `data/slax`, using the same
validation while keeping the source page open. Copy-to-RAM retains the complete installation source.
The footer contains Back and Next; the native window close control exits the
wizard. Closing remains disabled during disk writes.

Discovery runs outside the GUI thread and retains the complete physical-disk
inventory. The menu shows partitions strictly larger than 1,000,000,000 bytes.
A whole disk is hidden when it has a partition above this limit; unformatted
partitions are hidden when a formatted partition above it exists on that disk.
Bare disks and disks with only small partitions remain whole-disk choices.
Windows sorts drive letters first, then other targets; Linux sorts full device
paths. Every card has a square icon badge for hard, USB or virtual storage,
using Partition Manager's artwork. The second line begins with the bold target
path (`C:/`, `#0` or `/dev/sda5`), followed by capacity, filesystem and free space
with middle-dot separators. Wheel events at list boundaries do not repaint
unchanged cards.

The target page shows device information without inline eligibility errors.
Next always opens a native dialog: a neutral error explaining why installation
is unavailable, or a short, plain-language confirmation of the actual plan.
The checkbox reads **Erase entire selected disk (including all other partitions
on it)**. When checked, every visible row on the last clicked physical disk is
highlighted. Unchecking restores only the last clicked row, or no selection.
Hidden sibling partitions still appear in erase warnings.
Target Next keeps the target page visible during plan checks, with footer status
and a busy cursor. Native errors and confirmation dialogs appear above that page;
cancellation or a failed check leaves the selection available there. Only approval
and successful plan revalidation open the installation progress page.
The GUI never waits for scanner shutdown. ISO checks also show immediate status.
Checks, preparation and installation use the system's arrow-with-busy cursor:
Windows `IDC_APPSTARTING` and GTK's themed `progress` cursor when available.
Normal cursors return when work finishes or a confirmation needs user input.
Installation progress stays on one line in both frontends, shortening long paths
with an ellipsis while keeping the filename end visible and the progress bar fixed.
Completion and error states use the bundled green check and muted-red warning
symbols, including native error dialogs, instead of operating-system status icons.

## Preserving files and existing systems

The installer creates a new `slax` directory, verifies files with SHA-256 and
publishes the directory only after copying succeeds. An existing `slax` file or
directory is never replaced; it can contain saved sessions. Move or rename it
first. BIOS and UEFI are planned independently:

| Target | Available installation paths |
| --- | --- |
| FAT, NTFS, ext2/3/4 or XFS partition on MBR | BIOS in safe empty boot space; UEFI through a same-disk ESP or a newly added ESP in free space. |
| Supported partition on GPT | BIOS through an empty BIOS boot partition or a new one in free space; UEFI through an existing or new ESP. |
| Windows XP BIOS system | Keep the original MBR boot code and boot.ini default; append an NTLDR entry pointing at a verified sector file which starts Slax GRUB. |
| Newer Windows BIOS system | Use native BCDEdit to append a boot-sector entry without changing the default OS. Display the text boot menu for 10 seconds. |
| Windows UEFI system | Keep Microsoft files and entries; a signed GRUB text selector defaults to Windows and offers Slax Linux separately. |
| Recognized existing GRUB | Append Slax Linux through its standard custom.cfg hook, retaining existing entries and defaults. BIOS loads the Slax configuration; UEFI chainloads its signed shim. |
| Unrecognized existing loader | Keep it behind the default entry of a separate text selector, without rewriting its private configuration. |
| FAT target without space for a separate ESP | EFI files may share the data filesystem. A whole-device FAT filesystem can receive removable UEFI files without a partition table. |
| Non-FAT target without a usable ESP or room for one | BIOS-only installation when a safe BIOS location exists; otherwise an explanation before approval. |
| Native 4096-byte-sector disk | UEFI only; the stock BIOS loader requires 512-byte logical sectors. |

Btrfs and exFAT additionally require an ISO advertising their initramfs drivers
in `slax/boot/grub/filesystems.txt`. Windows can preserve only filesystems its
own drivers expose. GRUB checks the real filesystem and compares the copied
boot files before BIOS activation; unsupported features cannot silently replace
a working MBR. Filesystem and partition UUIDs must be stable and unique.

The small BIOS core loads matching external modules from the new Slax folder.
Its prefix follows the disk selected by the BIOS and the approved partition
number, so it does not assume a fixed BIOS disk number. A traditional MBR gap
must be entirely empty; occupied original embedding areas are retained intact.
Preserving an existing GPT also checks BIOS space reserved before FirstUsableLBA.
An identical existing Slax MBR/core can be reused without consuming another extent.
Otherwise it uses an empty BIOS boot partition or creates one in verified free space.
On readable data-only disks, the confirmed plan warns that any existing BIOS loader
will be replaced. Only bounded embedding space or an unformatted BIOS partition
may be overwritten; existing filesystems and partition boundaries stay intact.
Detected operating systems and unreadable or ambiguous partitions retain the
preservation rules. Original MBR and overwritten core bytes are saved for recovery.
If necessary, MBR can add a small type 0xDA BIOS partition in a zero-filled free
extent outside every existing partition. No existing filesystem is resized.

Preserve-mode raw writes are restricted to the approved boot and metadata
ranges. Existing filesystems stay mounted and are never raw-written. Original
MBR/table records are saved in `slax/boot/grub/boot-backup`; normal activation
errors restore the original boot metadata. NTLDR and BOOTMGR retain their MBR
code; their new root-level sector file points at the reserved GRUB core. Native
GRUB integration does not need a second embedded core. Existing menu text is
saved under boot-backup; boot.ini/custom.cfg replacement is atomic.
Fallback selectors are plain text, default to the previous system, and offer
Slax Linux as a separate choice. The normal Slax configuration from the ISO
keeps its logo, five-second countdown and four-entry Escape menu unchanged.

UEFI prefers an existing writable FAT ESP on the same disk. If none is usable,
the installer may create a separate FAT32 ESP in verified empty unallocated
space. Only when neither is possible can a FAT data filesystem host EFI itself.
`EFI/Slax/<installation-id>` holds the signed shim, signed GRUB, kernel,
initramfs and menu, without replacing another vendor's files. Signed Slax UEFI
boot uses Debian's existing Microsoft trust chain and keeps verification enabled.

Adding an internal firmware entry requires writable runtime variables in a
64-bit UEFI session. A new selector entry retains every original firmware entry.
Windows Boot Manager appears first in the outer text menu when present on this
ESP. Other active same-ESP loaders without binary load options can appear there too;
entries with unsupported options remain available in the firmware boot menu.
Preparing removable media works from BIOS sessions too, including XP.
Unknown EFI fallback loaders are kept. An identical Microsoft fallback may be
backed up atomically before Slax's fallback is published.

Encrypted filesystems, Windows file encryption, RAID/LVM/storage-pool members,
dynamic or spanned volumes, unsupported filesystems, read-only media and
insufficient space are explained before approval. Raw NTFS boot headers also
detect an encrypted volume that Windows has unlocked. Linux checks NTFS
hibernation before obtaining writable access. NTFS boot/persistence sources
start read-only and become writable only when the complete hibernation header
is absent or zero; no force-mount option is used.

## Erasing a whole disk

Erase means the complete physical parent, including all sibling drive letters
and hidden partitions. The confirmation lists them. The usual layout is GPT:
reserved BIOS boot space before the 1 MiB-aligned FAT32 ESP (256 MiB with
512-byte sectors or 512 MiB with 4096-byte sectors), then a full-capacity FAT32
Slax data partition. The GPT's first usable LBA starts at the ESP, protecting
the preceding embedding space without a separate raw BIOS partition that
Windows could expose and offer to format.

Actual Windows XP, including XP x64, uses MBR with active Slax data first and a
small type 0xEF ESP second. This keeps the Slax folder accessible on XP USB media.
The same compatible layout serves removable media on Windows before 10 build
15063, which exposes only the first partition, and older hosts without GPT data
support. It requires 512-byte logical sectors and at most 2 TiB. Other Windows
configurations and Linux retain GPT even when started using BIOS. Compatibility
mode does not change capability detection. Both layouts support signed UEFI;
512-byte disks also receive BIOS boot.

The native FAT32 formatter supports volumes above Windows' 32 GiB formatting
limit. FAT32 sector/cluster limits, files above 4 GiB and inadequate space are
rejected before writing. The running system, live session, ISO and executable
disks cannot be erased. All sibling volumes must be safely locked/unmounted
before erase writes; preserve installs do not lock or unmount live data volumes.

Disk handles stay pinned across confirmation. Linux plans hold read-only handles;
writable handles open only under the disk lock after approval and close before
unlocking, so replacing a plan cannot trigger a competing udev probe.
The ISO is reopened between steps;
source file IDs, timestamps and content changes are not compared. Table integrity,
geometry, media-generation tokens when available, free space, approved empty
ranges and firmware state are rechecked. Closing cannot interrupt installation.
Hardware faults and power loss cannot be transactional; failure messages report
possible partial Slax files or partial erasure, with recovery metadata retained.

## Runtime and build

Windows uses only system DLLs, including XP's GDI+. C++ and threading runtimes
are static. Linux uses GTK 3, GLib, libudev, libblkid, standard C/C++ libraries
and stock GRUB tools already present in Slax. No browser, .NET, WebView,
PowerShell, WMI, VBS, downloaded font or installation-time internet is required.
Administrator/root access is needed. Optional Windows APIs are loaded dynamically;
the PE subsystem is 5.1. The ISO reader rejects unsafe names, links, alias
collisions and invalid extents. `--list-devices [ISO]` and `--check-iso FILE`
are read-only diagnostics.

Build both binaries on Debian:

```sh
apt-get install --no-install-recommends g++ g++-mingw-w64-i686 make bison flex gettext python3 pkg-config libgtk-3-dev libudev-dev libblkid-dev upx-ucl
sh Slax/debian13/installer/build
```

The build strips both executables, packs only the Windows executable with
`upx --best` (without ultra-brute) and runs `upx -t` on it. Linux remains an
unpacked ELF executable so the desktop module's SquashFS can compress it.
The build downloads and SHA-256 verifies the official
GNU GRUB 2.12 Windows ZIP for its filesystem verifier and matching BIOS modules.
The official Windows mkimage lacks XZ support, so `build-windows-grub` compiles
the unmodified GNU utility with static, threadless liblzma from separately
pinned GNU/XZ source archives. This fits small BIOS gaps without adding a DLL.
Downloaded inputs stay in the temporary build directory. Only their embedded
copy is distributed in the Windows installer; provenance/licenses live under
`bootfiles/grub`. Ordinary ISO builds copy the prebuilt installers. The ICO is
from https://www.slax.org/favicon.ico. The sidebar uses its exact 32 × 32 clover
frame: Windows loads the ICO natively and selects a larger frame at higher DPI;
GTK uses the extracted PNG. The wordmark PNG came from the supplied mockup.
The hard-drive, USB, layer-group, check and triangle-exclamation icons are Font Awesome Free 6.7.2 by
Fonticons, Inc., copied from Partition Manager under CC BY 4.0. Original SVGs,
license and embedded PNGs live in `icons`; `python3 icons/render` regenerates
the 96-pixel PNGs with `rsvg-convert` when changing artwork. Ordinary installer
builds require no image converter, font package or additional runtime library.

## Verification

```sh
apt-get install --no-install-recommends xorriso
sh Slax/debian13/installer/check /path/to/slax.iso
```

The sanitizer suite writes owned sparse files with fake firmware, exercising
ISO parsing, hashing, GPT/MBR/FAT32, 4 KiB sectors, BIOS multiboot, preservation,
new boot partitions, occupied embedding areas, rollback, encryption detection,
UEFI entry parsing and the wizard's confirmation handoff. `test-native.cpp`
accepts only owned Linux loop devices under `/tmp/slax-installer-native.*`.
`check-linux` tests discovery using mocked hardware and an owned loop.
`test-source.cpp` checks source detection, full ISO validation and remaining on
the source step for mounted ISO/CD fixtures, including symlinks, bind mounts
and optical media without a valid Slax payload. It performs no installation writes.
Native GUI fixtures cover resizing, DPI, text alignment, four-card defaults,
whole-disk highlighting, restored selection and wheel-boundary repainting.

Destructive Windows tests belong only in designated VMs. Never automate write
confirmation against host drives. Boot tests must cover BIOS, UEFI Secure Boot,
and both Slax and the previous Windows entry after preserving an OS installation.
