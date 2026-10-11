# Project notes

Read these notes before changing build, boot or VM code. Keep them current when
an agreed workflow changes. Verify the current build and VM state before
reporting progress.

## Scope and working rules

- Write all code, comments, documentation and product strings in English,
  even when Tomas speaks Czech. Conversation may stay in Czech.
- Slax is distributed to many users on different systems. Tomas's local VM
  is a reproduction environment for a reported bug, not the target environment
  that a product fix may assume. Every Slax fix must work generally.
- Fix the underlying problem in the shipped code or configuration. Never make
  a Slax fix depend on Tomas's computer, VM identity, local paths, addresses,
  host setup or display settings just to make the local test pass.
- When a fix needs a hardware or platform condition, detect the affected
  driver, capability or standard generically. It must apply to every user with
  that condition and preserve operation on other supported systems.
- Keep adjustments needed only for the development VM in its local setup or
  development helpers. Do not ship those adjustments as Slax product fixes.
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
- Preserve graphics driver autodetection. The generic Xorg OutputClass in
  `02-xorg/rootcopy/etc/X11/xorg.conf.d/20-software-cursor.conf` uses
  `SWcursor=true` and `HWcursor=false` because drivers use different option
  names. It must not contain a `Driver` entry or select a particular GPU.
- Keep xterm's `allowSendEvents` disabled. Enabling it disables application
  mouse tracking, making clicks fail in Midnight Commander and other terminal
  applications. The setting belongs in `02-xorg/rootcopy/root/.Xresources`.
- Put identical boot assets in shared `bootfiles`, not in both boot trees.
  Keep only Slax-specific overrides in `Slax/debian13/bootfiles`.
- Keep shell changes portable and use POSIX syntax where the script uses
  `/bin/sh`. Existing Bash module tools can remain Bash.
- Match Tomas's surrounding script style: three-space indentation, spaces in
  arithmetic and expressions, and blank lines between related logical blocks.
  Inspect nearby code before changing `livekitlib` or other existing scripts.
  Prefer familiar `if ... then ... fi` blocks and `while true`. Do not introduce
  `case`, `while :`, or standalone `[ ... ]` guards in newly written shell code.
- `bootinst.bat` must remain available as `/slax/boot/bootinst.bat`. Preserve
  its Linux dispatch to `bootinst.sh` and Windows XP-or-newer support without
  requiring PowerShell. Keep Slax's optional text installer logo.
- Preserve unrelated uncommitted changes. Do not stage or commit unless asked.
- For a small change in an existing module, update the source and repack only
  that module in tmpfs, then recreate the ISO. Do not rebuild Debian and every
  application merely to change one configuration file.

## Work VM

These settings describe the local development environment. Product changes
must follow the general Slax repair rules above.

- Use VirtualBox for the current work VM. VM files and helpers belong in `VM`.
  Run OS windows visibly (`--type gui`). Only background collectors/helpers
  may be hidden.
- VirtualBox CLI: `C:/Program Files/Oracle/VirtualBox/VBoxManage.exe`.
- Work VM: `Slax Debian work`, 16 vCPUs, 24 GB RAM, root account, Xfce desktop.
  Slax itself uses Fluxbox; these are separate environments.
- Active disk: `VM/virtualbox/debian-work.vdi`. Never remove it when cleaning
  test files. Shut the work VM down normally before host restart.
- This is a dynamic 64 GB VDI. For compaction, preserve the work VM's tmpfs:
  freeze its filesystem, save its state, then use `zerofree` from a temporary
  RAM-only maintenance VM with host I/O caching enabled and the work filesystem
  unmounted. Detach the disk before `VBoxManage modifymedium --compact`, then
  resume and thaw the work VM. Do not run fsck or change its filesystem metadata
  externally while its saved kernel still owns that filesystem.
- SSH can disconnect during save/resume. If necessary, thaw from the existing
  guest terminal with `fsfreeze -u /` and restart `networking` if DHCP was lost.
- Network: bridged through `Realtek PCIe GbE Family Controller #2`.
  Guest address last verified as `10.0.0.48`; this is a DHCP lease.
- Guest checkout: `/root/linux-live`. Local and guest checkouts are separate;
  transfer relevant changes before building and verify their contents match.
- SSH uses root and the local `VM/id_ed25519` key. Host keys are stored in
  `VM/known_hosts`. The console password is in `VM/credentials.txt`.
  Never copy credentials or private keys into notes, logs or Git.
- System `/tmp` is a 20 GB tmpfs. Both kernel and Slax builds use it. Do not
  add separate RAM mounts when ordinary directories under `/tmp` suffice.
- Swap is permanently disabled, including initramfs resume. Keep it disabled.
- VMSVGA can accept clicks while its hardware cursor is invisible. Use Slax's
  generic `20-software-cursor.conf` on the work VM as well. Its OutputClass
  sets `SWcursor=true` and `HWcursor=false` without selecting any driver or
  matching a particular GPU. Do not force the `vmware` driver in Slax.
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
- Apply core and module `rootcopy` only after cleanup and runtime package
  installation. Keep setup that depends on copied files in the `copy` phase.
- Normalize copied systemd service files to mode 644 in their recipes. Set
  filesystem branch roots to mode 755 before packing: rootcopy may carry
  group-writable directory modes, which cause AUFS permission warnings.
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

- `VM/start.cmd` opens the visible work VM. `VM/start.ps1` also starts
  `collect-slax-build.py`, which downloads only the completed ISO and verifies
  its SHA-256 before publishing `VM/slax-build-trixie/slax-amd64.iso`.
- Guest helper: `/root/continue-slax-build.sh`; service:
  `/etc/systemd/system/slax-autobuild.service`. Their local sources are in
  `VM`. The service disables itself after successful completion.
- Clear the old `SHA256SUMS` completion marker before a new automated build.
  Publish it only after the replacement ISO is complete.
- Stop the build through its systemd service rather than using broad
  `killall`. Stop a stale collector before replacing its output.
- Local helpers `VM/guest.py`, `audit-work-copy.py`,
  `repack-xorg-only.sh` and `start-vbox-boot-test.ps1` exist in this workspace
  but are Git-ignored. Do not assume they exist in a fresh clone.
- `guest.py` runs a local shell file on the guest, or transfers files:

  ```text
  python VM/guest.py local-script.sh
  python VM/guest.py put local-file /root/linux-live/relative-file
  python VM/guest.py get /remote/file local-file
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
  before changing firmware. Stop test VMs after verification. Power off VMs
  running Slax from CD directly with `controlvm ... poweroff`; no clean shutdown
  is needed. Shut the Debian work VM down normally.
- Its disposable session disk is `VM/session-test.vdi`, a dynamic 20 GB VDI
  with a FAT32 data partition. Sessions are under `/media/sda1/changes` and
  use dynfilefs with an XFS filesystem inside split `changes.dat` files.
  The test launcher attaches this existing disk at SATA port 1 and the ISO
  at port 0. Do not recreate or format the session disk during startup.
  Keep it separate from `VM/virtualbox/debian-work.vdi`; Tomas plans to
  delete the session test disk after testing large-session duplication.
- Enable the test VM's audio output with the host's default audio driver and
  an HDA controller. Audio was previously disabled in its local launcher.
- Enable the test VM's bridged network through the active Ethernet adapter,
  using an Intel PRO/1000 MT Desktop (`82540EM`) card with its cable connected.
  The local launcher previously used `nic1=none`, leaving Slax with no network
  device. Keep this host configuration in the local VM helpers.
- Slax's `guest` account needs membership in `audio`: Chromium and VLC run
  under that account and access ALSA directly, without a sound server.
- Boot error audit: VMSVGA's `vmwgfx` hypervisor warning and the SSH generator's
  missing AF_VSOCK CID also occur when resuming without duplication. The copied
  session reaches the desktop with no failed systemd units. The ALSA restore
  rule in Debian's unmodified `alsa-utils` 1.2.14-1 has a missing
  `alsa_restore_std` label. These are separate findings, not failures of
  session copying. The copied service and module-root permission warnings
  are fixed in the build recipes.
- For boot videos, VirtualBox's built-in recording can capture the visible
  scratch VM from power-on. Stop recording after the desktop appears, then
  convert its WebM to MP4 with local ffmpeg. Keep the console readable and
  record Esc revealing the boot log when investigating post-copy errors.
  Recording must be off by default. After a requested recording, stop the
  recorder and save the disabled setting. The scratch launcher explicitly
  disables recording before startup.

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
- Both Slax GRUB themes use `desktop-image: "black.png"`. Keep this tiny black
  background: using only `desktop-color` caused `null src bitmap` when Enter
  interrupted the countdown or selected a menu entry. The logo remains a
  separate unscaled image.
- During the five-second countdown, show only `Press any key` with the timer
  below the logo. Enter immediately boots the configured default; any other
  key opens the full menu. Both screens source `grub/entries.cfg` and inherit
  the same exported default, which need not be the first entry.
- The full menu has only `[Enter] boot` and `[Tab] cmdline` on the penultimate
  row. The small `anykey.mod` extension handles countdown keys and maps Tab to
  GRUB's editor only while the graphical menu owns input. Restore input before
  the editor or kernel starts and re-arm it when the menu refreshes. Each boot
  entry also calls `anykey stop`. Its source and build helper are
  `tools/grub-anykey.c` and `tools/grub-anykey.create`. Compile against the
  matching GRUB version and platform flags. Keep bundled modules in
  `Slax/debian13/bootfiles/grub/extra/{i386-pc,x86_64-efi}` so USB boot setup
  does not remove them when replacing GRUB's standard module directories.
- Boot should remain visually quiet. The custom framebuffer program is
  `tools/fbmenu.c`, compiled statically with musl; its source contains build
  instructions. Avoid adding external graphics programs to the initramfs.
- Session selection retains the Slax logo and shows each session's name,
  date and size on one row. Highlight the entire selected row. No frame,
  arrow-only selection or `1/20` counters. Footer:
  `Up/Down: select     Enter: confirm     F4: duplicate     Esc: cancel`.
- Embed DejaVu Sans Mono at 12 px in fbmenu. Use 28 px row spacing, 16 px
  horizontal row padding and a 28 px gap below the logo. Center the menu within
  the available display; keep its width between 480 and 720 px where it fits.
  Leave an extra 16 px blank line between headings and menu/input content.
  Anchor the metadata column to the right. The shell formats human-readable
  session sizes with `%9s`, keeping their right edge aligned and dates fixed.
  Always use one decimal place, a space and a unit, with KB as the smallest
  unit (including `0.0 KB`); scale larger values to MB, GB and at most TB.
  Menu column positions and pixel rendering belong to `draw_menu()` in fbmenu.
- Numeric session directories remain their identifiers. Store the display name
  in each directory's `boot/${LIVEKITNAME}_session.txt` (`boot/slax_session.txt`
  for Slax); create `boot` when saving a new or duplicated session. Show the
  name without a numeric prefix.
  Unnamed sessions use `Session #ID`. Empty and duplicate names are allowed.
  New and duplicated sessions ask for a name using `fbmenu --input` under the
  logo. Suggest `Initial session` for the first session and the source name
  for a duplicate. Use `New session name` for new sessions and
  `Name the duplicated session` for copies. Esc in the name input returns to
  the session picker without creating a session or updating `last_session.txt`. Only Esc
  in the session picker cancels persistence. Left/Right move the input caret
  across UTF-8 characters; Backspace at the beginning does nothing. Prefilled
  text starts with the caret at its end, without an initial selection or a
  different input mode. Strip metadata line endings without adding a space or
  trimming intentional spaces. Compose input changes off screen and update only
  the field; do not clear/redraw the logo and entire display for each key.
- The tty decoder waits 100 ms after a single Esc. Double Esc cancels immediately;
  Alt combinations and unsupported escape sequences are consumed and ignored.
  Preserve recognized navigation/function keys and complete UTF-8 characters.
- Slax's session chooser boot entry includes `automount`, like the other
  persistent boot entries, so session disks remain accessible under `/media`.
- `fbmenu --menu ... --duplicate` returns `duplicate:ID` on F4. F4 on the new
  session row returns `new`. Copy an existing session only after its new name
  is confirmed, stage it in a hidden directory and activate it after the copy
  succeeds. Preserve source contents, links and permissions; leave the original
  unchanged. During the copy, show `Duplicating session...` below the logo with
  the existing animated dots, using `fbmenu --status`. On success, show the final
  copied MB as boot continues; clear the message on failure. Keep Esc able to
  reveal the log. Use `cp -a` without verbose filename output; keep copy errors
  on stderr without corrupting the returned session ID. Check both native
  storage and FAT-backed session files.
- During duplication, record `df -Pk` used space on the target filesystem before
  copying and subtract that baseline from subsequent measurements. Show
  `Duplicating session... N.N MB` through `fbmenu --status` every five seconds; write
  progress to stderr only every ten seconds. This includes allocated space for
  small files and metadata, plus any concurrent writes on the same filesystem.
  Wait for the monitor's last request, then always measure and print the final
  total after a successful copy, even if it took less than ten seconds. Leave
  that total visible in the graphical status as boot continues. Clear the
  activity on failure. Keep `cp -a`, its exit status and the session ID intact.
- Dynfilefs's `virtual.dat` is created with XFS. Mount it explicitly with
  `-t xfs`, including when growing it, to avoid probing it with NTFS and logging
  misleading signature errors. Native persistent directories still use their
  own filesystem.
- Keep dynfilefs's FUSE mount separate from the loopback XFS mount. In
  `persistent_changes`, FUSE mounts at `$2.dynfilefs` and XFS at `$2`. For the
  usual live root these are `/memory/changes.dynfilefs` and `/memory/changes`.
  This prevents shutdown from remounting XFS with the hidden FUSE mount's
  `user_id` and `group_id` options. The storage format and session files stay
  unchanged; filesystem creation and growth use the separate `virtual.dat`.
- Keep HTTP FUSE storage at `$DATAMNT.httpfs` and its loop-mounted data at
  `$DATAMNT`. Local `from=ISO` uses the separate `/memory/iso` mount; failed
  searches must unmount the ISO before its source filesystem. Stop after a
  device mount fails, so retries cannot inspect or stack stale filesystems.
  Explicit session devices mount at `/memory/persistence`, with a single
  bind mount of the selected directory at the existing changes path. RAM
  copying releases its loop device before unmounting the backing filesystem.
- Esc in the session picker cancels persistence: no session is created and
  `last_session.txt` is not updated. Elsewhere during boot, Esc dismisses the
  splash and reveals the log. Keep a text menu when framebuffer is unavailable
  and verify the graphical layout at 800x600 as well as larger resolutions.
- Running dots cycle through `.`, `..`, `...`, ` ..`, `  .`, then a blank
  frame before restarting. Preserve small-screen support. Advance only on a
  dedicated monotonic one-shot timer, holding each frame for 250 ms; keyboard
  input and boot requests must not advance the animation. Redraw only the dots.
- Preserve the animation's frame and live timer across the initramfs/root exec
  handoff. FD 5 carries the ready acknowledgement; FD 6 carries the timer.
  Do not repaint the already visible logo or reset the timer during handoff.
