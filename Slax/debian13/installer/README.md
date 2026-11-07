# Slax USB Installer

`../bootfiles/bootinst` is the native Slax executable. `bootinst.exe` is a
standalone 32-bit Windows executable targeting Windows XP and later. Both use
native windows, controls, scrollbars, file choosers and confirmation dialogs.
USB drives use rounded cards with a tinted device badge, selected background
and native radio buttons. A shared message slot keeps ISO information and errors
aligned beneath the path field. The supplied wizard's green sidebar and original PNG
branding are retained; fonts, buttons and window decorations follow the system.
The default client size is 832 × 446 logical pixels, with a 720 × 400 minimum.
Users can resize and maximize normally; the main content scrolls in short
windows. The sidebar stays visible at every supported size, and the footer remains accessible.

The complete wizard currently **simulates installation**. It never copies
files, formats, mounts, unmounts, partitions, ejects or changes boot records.
Confirmation, progress and the result identify the simulation explicitly.
The Windows manifest does not request elevation, since discovery is read-only.

Disk discovery runs outside the GUI thread and refreshes every two seconds.
Linux uses libudev USB ancestry; Windows uses SetupAPI and storage IOCTLs.
USB disks without partitions or letters are included; internal disks and
optical drives are excluded. Existing writable FAT32 space is reported.
Read-only, system/session and ISO-source disks cannot be used. Capacity,
free space, device identity and ISO availability are checked before starting.
Unplugging or replacing the target cancels the simulation with an error.
The erase checkbox changes validation and the simulated preparation phase.
It does not enable any write path.

ISO validation reads the ISO9660 directory records and checks the current
Slax layout (`slax/boot/vmlinuz` and `slax/modules/01-core.sb`) without mounting.
The required space is conservatively estimated from the complete ISO size.
An ISO filename can be supplied as the only command-line argument.
`--list-devices [ISO]` emits JSON; `--check-iso FILE` performs read-only validation.

The Linux GUI uses GTK 3, Pango/Cairo and libudev already installed in Slax,
plus the standard C/C++ libraries. Windows uses only system DLLs, including
GDI+ supplied with XP for the original logos and card decoration. The C++ and threading runtimes are linked statically.
No browser, .NET, WebView, PowerShell, WMI, redistributable runtime, external
font or internet connection is needed. New DPI functions are loaded only
when available. The PE subsystem version is 5.1. Both executables are stripped and compressed
with `upx --best`, then checked with `upx -t`; ultra-brute compression is not used.

Build both binaries on Debian with:

```sh
apt-get install --no-install-recommends g++ g++-mingw-w64-i686 python3 pkg-config libgtk-3-dev libudev-dev upx-ucl
sh Slax/debian13/installer/build
sh Slax/debian13/installer/check /path/to/slax.iso
```

Optional Linux hardware-discovery checks run as root on owned, RAM-backed
loop storage, with mocked USB devices instead of physical disks:

```sh
apt-get install --no-install-recommends umockdev gir1.2-umockdev-1.0 python3-gi e2fsprogs
sh Slax/debian13/installer/check-linux
```

Compiler, UPX and test packages belong to the build host only. The ordinary Slax ISO build
copies these prebuilt executables; it does not download assets or compile the
installer. Rebuild both binaries after source changes. `slax.ico` is the
original https://www.slax.org/favicon.ico; the two PNGs are the original images
embedded in the supplied HTML mockup.
