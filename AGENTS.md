# Project notes

Read these notes before changing build, boot or VM code. Keep them current when
an agreed workflow changes. The dated status below is a snapshot; verify it
before reporting progress.

## Scope and working rules

- Write all code, comments, documentation and product strings in English,
  even when Tomas speaks Czech. Conversation may stay in Czech.
- Linux Live Kit is generic: do not restrict it to Debian, amd64 or Slax.
  Its 32-bit binaries and support for other distributions are intentional.
- Slax currently targets Debian 13 on amd64. Slax recipes and branding belong
  under `Slax/debian13`; reusable functionality belongs in Live Kit.
- Slax must use Breeze cursors. The installed white variant is Breeze Light
  under `/usr/share/icons/breeze_cursors`; select it for X11 and GTK and in
  the default cursor theme. All cursor configuration belongs in `02-xorg`,
  not `03-desktop`. Register its `cursor.theme` with `update-alternatives`
  at priority 100 so GTK's Adwaita entry at priority 90 cannot replace it.
  Software cursor rendering must preserve this theme.
- Put identical boot assets in shared `bootfiles`, not in both boot trees.
  Keep only Slax-specific overrides in `Slax/debian13/bootfiles`.
- Keep shell changes portable and use POSIX syntax where the script uses
  `/bin/sh`. Existing Bash module tools can remain Bash.
- `bootinst.bat` must remain available as `/slax/boot/bootinst.bat`. Preserve
  its Linux dispatch to `bootinst.sh` and Windows XP-or-newer support without
  requiring PowerShell. Keep Slax's optional text installer logo.
- Preserve unrelated uncommitted changes. Do not stage or commit unless asked.
- For a small change in an existing module, update the source and repack only
  that module in tmpfs, then recreate the ISO. Do not rebuild Debian and every
  application merely to change one configuration file.

## Work VM

- Use VirtualBox, not QEMU, for the current work VM. The `qemu` directory name
  is historical. Run OS windows visibly (`--type gui`). Only background
  collectors/helpers may be hidden.
- VirtualBox CLI: `C:/Program Files/Oracle/VirtualBox/VBoxManage.exe`.
- Work VM: `Slax Debian work`, 16 vCPUs, 24 GB RAM, root account, Xfce desktop.
  Slax itself uses Fluxbox; these are separate environments.
- Active disk: `qemu/virtualbox/debian-work.vdi`. Never remove it when cleaning
  test files. Shut the work VM down normally before host restart.
- Network: bridged through `Realtek PCIe GbE Family Controller #2`.
  Guest address last verified as `10.0.0.48`; this is a DHCP lease.
- Guest checkout: `/root/linux-live`. Local and guest checkouts are separate;
  transfer relevant changes before building and verify their contents match.
- SSH uses root and the local `qemu/id_ed25519` key. Host keys are stored in
  `qemu/known_hosts`. The console password is in `qemu/credentials.txt`.
  Never copy credentials or private keys into notes, logs or Git.
- System `/tmp` is a 20 GB tmpfs. Both kernel and Slax builds use it. Do not
  add separate RAM mounts when ordinary directories under `/tmp` suffice.
- Swap is permanently disabled, including initramfs resume. Keep it disabled.
- VMSVGA can accept clicks while its hardware cursor is invisible. Use the
  `99-vmsvga-cursor.conf` override from Slax's Xorg rootcopy on the work VM
  as well: match `vmwgfx`, explicitly select `Driver "vmware"`, and set
  `HWCursor=false`. OutputClass without a Driver prevented Xorg from starting.
  Activation requires restarting graphics; get approval before ending an
  active desktop session with open applications.
- Stop only our unneeded test VMs/processes. Leave unrelated VMs untouched.

## Kernel and full Slax build

- Version pins and hashes live in `Slax/debian13/aufs-kernel-compile/versions`.
  Read that file rather than assuming a version is still the latest.
- AUFS is built into the custom Slax kernel. Compile and install this kernel
  on the build host, reboot into it, then build Slax with matching modules.
- The production configuration excludes DWARF, BTF and other auxiliary debug
  metadata. Keep compilation in tmpfs and retain completed `.deb` packages
  on the guest's persistent Linux filesystem.
- The custom release has a `-slax` suffix. Use its rebuilt modules; do not
  promise compatibility with binary modules for Debian's stock kernel.
  Tomas declined a universal external-module ABI audit during each build.
- Run as root on amd64 Debian:

  ```sh
  sh Slax/debian13/build trixie /var/tmp/slax-build-trixie
  ```

- This reuses completed kernel packages. If it installs a kernel, reboot and
  run the command again. The optional VM service automates that sequence.
- `buildmods` creates a minimal Debian with debootstrap inside a private mount
  namespace and an AUFS union. Each module gets its own filesystem branch.
  Service startup inside the chroot is blocked with `policy-rc.d`.
- Application compilation happens in disposable branches. Copy out the
  resulting packages, remove those branches, then install the packages into
  the runtime branch. Compiler dependencies must not enter the final modules.
- Run `Slax/debian13/cleanup` after the core and each runtime module.
- Keep functional AUFS whiteouts. Omit only bookkeeping and mounted build
  data when creating SquashFS modules.
- `auplink flush` is a build-time operation needed before freezing/detaching
  branches. It is not needed in the shipped initramfs or Slax runtime.
  Exported UAPI headers for compiling `auplink` are userspace headers, not
  kernel `EXPORT_SYMBOL` exports. Use the packaged `linux-libc-dev` headers.
- Current util-linux remount options use `mod=PATH=ro+wh`, `add=0:PATH=rw`
  and `del=PATH`. Live Kit's BusyBox mount uses its existing colon syntax;
  that syntax was tested with the new kernel and must not be changed blindly.
- Final output is `/var/tmp/slax-build-trixie/slax-amd64.iso`, plus package
  manifests and `kernel/*.deb`. Staging `/tmp/slax-build.*/media/slax/modules`
  is retained after a successful Slax build, together with filesystem branches,
  artifacts and other build files. Print the build directory at completion;
  do not delete it automatically. Failed Slax builds still clean their staging.
  Finished `.sb` modules are also inside the ISO at `/slax/modules`.
- Keep output directories on the persistent filesystem, not dangling links
  to temporary build trees. Kernel compilation still cleans its temporary
  sources and objects. Retained Slax files in tmpfs disappear on reboot.

## VM automation and incremental changes

- `qemu/start.cmd` opens the visible work VM. `qemu/start.ps1` also starts
  `collect-slax-build.py`, which downloads only the completed ISO and verifies
  its SHA-256 before publishing `qemu/slax-build-trixie/slax-amd64.iso`.
- Guest helper: `/root/continue-slax-build.sh`; service:
  `/etc/systemd/system/slax-autobuild.service`. Their local sources are in
  `qemu`. The service disables itself after successful completion.
- Clear the old `SHA256SUMS` completion marker before a new automated build.
  Publish it only after the replacement ISO is complete.
- Stop the build through its systemd service rather than using broad
  `killall`. Stop a stale collector before replacing its output.
- Local helpers `qemu/guest.py`, `audit-work-copy.py`,
  `repack-xorg-only.sh` and `start-vbox-boot-test.ps1` exist in this workspace
  but are Git-ignored. Do not assume they exist in a fresh clone.
- `guest.py` runs a local shell file on the guest, or transfers files:

  ```text
  python qemu/guest.py local-script.sh
  python qemu/guest.py put local-file /root/linux-live/relative-file
  python qemu/guest.py get /remote/file local-file
  ```

- For incremental module edits, extract the finished ISO into `/tmp`, unpack
  only the affected `.sb`, apply the matching source change, repack and run
  the extracted `slax/boot/grub/makeiso.sh`.
- Slax's module command is `sb`, with `sb2dir` and `dir2sb` underneath it.
  Install these existing tools on the host when useful. A directory ending
  in `.sb` is packed without adding its directory name inside the module.
  `sb2dir` uses `-no-xattrs`; if a module has meaningful extended attributes,
  unpack with plain `unsquashfs` to preserve them, then use `sb` to repack.
- Absolute symlinks in unpacked modules can resolve into the build host.
  Do not copy a regular file onto one blindly. Use `update-alternatives --root`
  for module alternatives and replace links explicitly when needed.
- For changes affecting boot, verify both BIOS and UEFI all the way to the
  desktop without manually specifying kernel/initrd or starting X. Boot menu
  success alone is not enough. For changes unrelated to boot, such as cursor
  themes or desktop configuration, one VM startup is sufficient.
- Scratch VM: `Slax boot test`. The local launcher accepts `-Firmware bios`
  or `-Firmware uefi`. Stop it and allow VirtualBox to release its session
  before changing firmware. Stop test VMs after verification.

## Boot and session UI

- Use GRUB 2 for BIOS and UEFI CD/USB boot, plus GRUB PXE boot. Preserve the
  `/slax` layout and the generic `mylinux` layout; do not hardcode Slax into
  shared loader generation.
- Windows boot setup must use bundled GRUB/EFI binaries from shared Live Kit
  assets. Linux may use the system's GRUB tools. USB setup creates the root
  `EFI/BOOT` directory from the bundled loaders.
- Require FAT12, FAT16 or FAT32 before changing USB boot records. Install BIOS
  first, then mark the GPT partition as an ESP. Never format the user's disk
  or change its partition layout as part of boot setup.
- Use `gfxmode=auto`, retain the framebuffer mode, and center the unscaled
  Slax logo with menu items below it. Avoid frames, tables and extra decoration.
  Keep the boot help/countdown near the bottom across the available display.
  F1 help is deferred; do not add a visible F1 menu entry.
- Boot should remain visually quiet. The custom framebuffer program is
  `tools/fbmenu.c`, compiled statically with musl; its source contains build
  instructions. Avoid adding external graphics programs to the initramfs.
- Session selection retains the Slax logo and shows each session's name,
  date and size on one row. Highlight the entire selected row. No frame,
  arrow-only selection or
  `1/20` counters. Footer: `Up/Down: select     Enter: confirm     Esc: cancel`.
- Esc in the session picker cancels persistence: no session is created and
  `last_session.txt` is not updated. Elsewhere during boot, Esc dismisses the
  splash and reveals the log. Keep a text menu when framebuffer is unavailable
  and verify the graphical layout at 800x600 as well as larger resolutions.
- Running dots cycle through `.`, `..`, `...`, ` ..`, `  .`, then a blank
  frame before restarting. Preserve small-screen support.

## Last verified state: 2026-10-10

- Work VM is running `7.2.9-slax`; AUFS is built in, swap is off, `/tmp` is
  tmpfs. Completed kernel image, headers and libc-dev packages are retained.
- `qemu/slax-build-trixie/slax-amd64.iso` is complete (522,344,448 bytes,
  about 499 MiB). Its download was SHA-256 verified. The baseline passed BIOS
  and UEFI boot to the Fluxbox desktop; later desktop edits use one VM test.
- The `xorg.service` installation line is `WantedBy=graphical.target`, without
  brackets around `WantedBy`. It was corrected in source and by repacking
  only `02-xorg.sb`; the full rebuild was stopped as unnecessary.
- Module sizes reported by `du -h`: core 135M, firmware 97M, Xorg 58M,
  desktop 35M, apps 4.9M, Chromium 128M. Cleanup and compiler isolation passed.
- Local and guest build inputs matched across 641 files. The test VM was
  stopped; only the work VM remained running. No files were staged or committed.
- The staging tree of this ISO was already removed before Tomas requested
  retention. Future successful Slax builds keep their `/tmp/slax-build.*` tree.
- A retained incremental workspace now exists at `/tmp/slax-repack.9E8hff`;
  it contains unpacked `02-xorg.sb` and `03-desktop.sb` directories and
  `media/slax/modules`.
- The invisible VMSVGA cursor was fixed with `HWCursor=false` in the scoped
  Xorg override. The work VM's graphics were restarted with Tomas's approval,
  and the cursor was visibly rendered on its login screen.
