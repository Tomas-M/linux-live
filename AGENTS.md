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
- Slax uses the bundled Windows Modern Light cursor theme at size 32 for X11
  and GTK. Its converted Xcursor files and MIT license live in `02-xorg/rootcopy`;
  do not download or convert them during a build. `02-xorg/cursor-aliases` lists
  symlinks created by `copy`. Preserve the original blue wait and progress
  animations, including their premultiplied alpha and timing.
  Do not install Breeze. Keep cursor defaults in `02-xorg` and register the
  bundled `cursor.theme` with update-alternatives at priority 100, above
  Adwaita's 90. Desktop startup uses the named watch cursor from these defaults.
- `fbvboxpointer` reads the active vboxguest absolute-pointer position before
  showing the startup cursor and after resolution changes. Xorg otherwise starts
  at the screen center and jumps to the host position on the first input event.
  Query the driver's standard interface and integration capability flags; do not
  assume a VM name, screen size or local pointer position. Other systems and
  relative input are unaffected. Do not synthesize clicks or change mouse modes.
- Preserve graphics driver and cursor autodetection; do not force software
  cursors or a particular GPU driver. The kernel recipe applies
  `vmwgfx-legacy-cursor-limit.patch`: guest-backed cursors on older SVGA hosts
  retain their 64px limit when the newer cursor-MOB limit register is absent.
  Keep MOB cursor validation unchanged. This fixes invisible hardware cursors
  without the software rendering that caused cursor clipping in VirtualBox.
  `vmwgfx-legacy-cursor-hotspot.patch` also includes Xorg's legacy cursor-bypass
  hotspot in guest-backed cursor images, matching the position calculation.
  Keep theme hotspots and Fluxbox border cursors unchanged; the driver must pass
  the actual hotspot to the host instead of shifting the artwork.
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
- Free tmpfs by deleting unneeded, inactive build/test workspaces. Do not
  archive or move them to persistent storage unless explicitly requested;
  building in RAM is intended to avoid unnecessary disk writes. Moving
  temporary data to disk grows the dynamic VDI, and deleting it afterward
  does not shrink that host file; recovering the space requires compaction.
- Swap is permanently disabled, including initramfs resume. Keep it disabled.
- The work VM may still have the old vmwgfx module loaded and a local software
  cursor override. Remove that override when activating the corrected module;
  do not ship it in Slax. Get approval before restarting an active work desktop
  session with open applications.
- Stop only our unneeded test VMs/processes. Leave unrelated VMs untouched.

## Kernel and full Slax build

- Version pins and hashes live in `Slax/debian13/aufs-kernel-compile/versions`.
  Read that file rather than assuming a version is still the latest.
- `KERNELPACKAGEVERSION` distinguishes patched kernel packages from stale cached
  builds without changing the kernel ABI release. Advance it when kernel patches
  change, and keep image, headers and libc-dev package versions together.
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
- `buildmods` creates a minimal Debian with `debootstrap --variant=minbase`
  inside a private mount namespace and an AUFS union. Cleanup preserves English
  man pages and removes info, documentation and most translations. Each module
  gets its own filesystem branch.
  Service startup inside the chroot is blocked with `policy-rc.d`.
- ConnMan provides NTP synchronization. Do not install `ntpsec-ntpdate` in core;
  it pulls in an otherwise unused Python runtime and its dependencies.
- Keep `01-firmware` focused on getting users online over Wi-Fi or Ethernet,
  including MediaTek/Ralink and Atheros USB adapters. Do not add dedicated
  graphics or audio firmware packages to the base image; users can install
  those after connecting. Keep small legacy network packages for compatibility.
- Core needs `grub2-common` and `grub-pc-bin` for Linux USB BIOS setup. Keep
  `grub-efi-amd64-bin` and `mtools` only on the build host: UEFI USB setup copies
  the bundled loader, and ISO recreation reuses the bundled EFI image.
- Back up completed kernel `.deb` packages and their checksums to
  `VM/slax-build-trixie/kernel` on the Windows host. Restore that directory to
  the build output's `kernel` directory when reinstalling the work VM.
- Application compilation happens in disposable branches. Copy out the
  resulting packages, remove those branches, then install the packages into
  the runtime branch. Compiler dependencies must not enter the final modules.
  Apply newly registered Fluxbox patches with `quilt push -a` before
  building; an extracted Debian source can retain a preparation marker that
  makes dpkg-source skip newly appended patches.
- `03-desktop/fbinstall.c` is the GTK2 one-click APT installer, built into
  `slax-desktop-tools`. Use `fbinstall [--user guest] ICON NAME "PACKAGE..."
  "COMMAND [ARGS...]" [ARGS...]`. Available commands start before GTK or APT.
  Missing applications install immediately, without a size threshold or confirmation.
  Launchers pass the application name; the installer supplies the shared explanation
  that it will be downloaded and installed, followed by "please wait...".
  Closing the installer cancels preparation, downloads or installation. Terminate
  its APT process group, including dpkg and package scripts, and force termination
  after one second if needed. Cancellation must not retry or launch the application.
  Run APT directly rather than a separate size calculation or simulated installation.
  File and URL arguments stay separate from command quoting.
  It uses existing package lists, refreshes once if a package cannot be resolved
  or an archive returns HTTP 404, and installs without recommends. It launches
  the command only on success. APT's status fd supplies download and installation
  percentages; updating and preparation pulse. Command arguments use quoting
  without an implicit shell.
  Installation errors show a scrollable APT log and an OK button.
  Keep installer and error windows centered when their contents
  change size, using `GTK_WIN_POS_CENTER_ALWAYS` and `GDK_GRAVITY_CENTER` so
  the window-manager frame is included in the centered position.
- Run `Slax/debian13/cleanup` after the core and each runtime module.
- Apply core and module `rootcopy` only after cleanup and runtime package
  installation. Keep setup that depends on copied files in the `copy` phase.
- Keep Slax application desktop overrides in their module's
  `rootcopy/usr/local/share/applications`, with the exact Debian desktop filename
  (including case). Preserve the package's `/usr/share/applications` original:
  XDG directory precedence keeps one launcher and retains Slax's command after
  APT upgrades. Update Favorites and MIME associations when changing desktop IDs.
- Keep selected applet and application icons in their module's `rootcopy`.
  Selected application launcher artwork uses distinct `slax-*` names in
  hicolor's `apps` directories, bundled at 16, 22, 24, 32, 48, 64, 96 and 128 px.
  Native application icon names are listed in each module's `icon-aliases`;
  `copy` installs those PNGs under `/usr/local/share/icons/hicolor` so APT cannot
  replace them. Refresh this local icon cache through the final module too.
  SciTE reads `/usr/share/pixmaps/Sci48M.png`; xterm uses its bundled XPM through
  `XTerm*iconHint`. Keep Chromium, Session action icons and status tray icons unchanged.
  `03-desktop` includes `librsvg2-common` so GTK can decode SVG icons. Refresh
  GTK's icon caches after restoring files in `03-desktop`, `04-apps` and
  `05-chromium`. Put ConnMan's regular tray icons in Adwaita's `16x16/legacy`
  directory too: GTK may prefer Adwaita's symbolic icons over inherited hicolor
  artwork even when a normal icon lookup finds the hicolor PNG.
  Network, volume and battery status icons come from Debian's AdwaitaLegacy
  theme; larger feh icons are rendered from their original SVG assets.
- Xarchiver requests a 24 px window icon. Store its selected 128 px artwork
  in the 24 px icon path too; GTK retains its resolution without rebuilding
  Xarchiver. Keep feh's desktop entry available for icon lookup and MIME
  handling with `NoDisplay=true`; xlunch must respect that field.
- Maintain xlunch changes in the sibling `../xlunch` repository. Slax downloads
  the tagged upstream release (currently `v5.8.0`); do not carry a duplicate
  local xlunch patch. Commit the fix and version update separately there,
  regenerate `docs/usage.txt` from the compiled program and tag the release.
  xlunch uses libXcursor to load the system-themed hand for clickable controls,
  respecting the configured theme and size. Other areas inherit the desktop
  cursor; do not restore fixed-size X font cursors.
- xlunch's desktop catalog and prepared icon pixels live in `~/.cache/xlunch`
  (or `$XDG_CACHE_HOME/xlunch`). Keep the cache uncompressed. Its low-priority
  background refresh must stop before launching a selection. `05-chromium/copy`
  prebuilds root and guest snapshots with `--rebuild-cache`, after all desktop
  entries and icons are present. Match the runtime profile, locale, PATH and
  XDG environment; refresh the final module cache after changing these inputs.
  Incremental icon changes must also refresh the final GTK icon cache in
  `05-chromium`: its cache overrides caches from earlier modules.
- Slax's Focus footer uses `align=grid` and `bottom_margin=0`. This places
  the help 28 px lower than the shared margin and gives the icon grid the same
  additional height while leaving the bottom-left close button unobstructed.
  Its grid uses `fade_height=64` for clipped tiles at both viewport edges;
  each fade shrinks away at the beginning or end of the list.
- Keep `slax-rmmounts.service` active after its oneshot cleanup with
  `RemainAfterExit=yes`. Transient automount services otherwise pull in sysinit
  again and repeatedly rerun the cleanup until systemd's start limit is hit.
- Slax's `fbappselect` starts xlunch with `/etc/xlunch/focus.ini` by default.
  Keep Slax branding, Session commands and terminal/GUI command routing in its
  profile and `fbcommand`; xlunch itself must remain distribution-independent.
- The xlunch header shows only `/etc/xlunch/slax_text.png`, with empty text
  and caption. Keep `icon_size=140` for the wordmark's native 140x49 size;
  12 px padding aligns its canvas with the sidebar items. The header follows
  the image's aspect ratio. Do not restore
  the old round clover icon or a separate Slax text label.
- Optional installer desktop entries declare `X-Xlunch-Executable` with their
  actual command or absolute executable path. Slax's Focus profile uses
  `grid.uninstalled_icon_opacity=100` and `grid.installed_first=true`.
  Available apps come first; every icon retains its original opacity. Missing
  apps show the 24 px `grid.install_icon` arrow.
  Place it in the tile's upper-right corner, with `grid.install_icon_padding=12`.
  `grid.install_icon_hover_only=false` and `grid.install_icon_opacity=30` show
  it at 30 percent opacity without hover, and fully opaque on hover or keyboard
  focus. Focus opacity values run from 0 (invisible) to 100 (opaque). Keep these
  settings configurable, independently of grid padding and the classic interface.
  Use a standalone down arrow with two detached bars above its shaft, without
  a circle or tray; use white artwork with crisp pixel edges and no outline.
  Keep help text unchanged. Availability is derived from the
  executable even with cached metadata; do not persist installation status.
  Its footer uses `align=grid`, leaving the lower left corner for the close button.
  Capture the live 52 px panel launcher with scrot before opening xlunch, so the
  close button preserves its actual icon, theme and background; remove the
  temporary image after xlunch closes. Do not substitute a power icon there.
  `fblogout` and `logout.dsv` remain available for the Fluxbox Exit / Logout entry.
  Desktop entries launch through `fbstartupnotify --desktop`; `fbinstall`
  and Fluxbox pass the actual command and arguments to the same compiled helper.
  Its on-demand monitor follows startup notification IDs, matching application
  windows and a 30-second timeout independently for concurrent launches.
  During launches, replace ordinary named arrows with the theme's native animated
  `progress` cursor through XFixes. Text, hand, resize and custom cursors stay intact;
  feedback may disappear over those areas. Do not use a moving overlay or grab input.
  The X server animates and moves the combined arrow and spinner together. Track newly
  displayed arrows, restore their themed names when all launches finish, and restore
  them on monitor shutdown. Launchers also restore arrows if the monitor disconnects.
  Capture the initial client list and active window in the launcher before spawning
  the application; publish them on an unmapped X window identified by its startup ID.
  The monitor must not take a later snapshot that already includes the new window.
  A shared WM_CLIENT_LEADER can retain an old startup ID, so it must not prevent
  matching new windows from an already running application by process or class.
  Keep desktop-file execution in GIO, including quoting, field codes and D-Bus.
- Optional application launchers and their bundled icon sizes live in `04-apps`
  rootcopy. `optional-apps.tsv` records independently resolved unpacked sizes
  against clean Slax, without recommends; every entry is below 500 MB.
  Keep canonical Debian desktop IDs and MIME declarations in `/usr/local/share`
  and refresh its desktop database after rootcopy. Remove the desktop and its
  `slax-*.png` icon to remove an uninstalled application from the catalog.
  `fbliveapp` runs guest applications with separate D-Bus sessions and grants
  X11 access only to that local user. VLC and selected Internet applications
  use `fbinstall --user guest`. Set `LANG=C.UTF-8` in `02-xorg`'s `/etc/locale.conf`;
  Debian 13 redirects `/etc/default/locale` to this canonical file at boot.
- Keep LXTask optional: its Task Manager launcher installs `lxtask` through
  `fbinstall`. Display Xarchiver as `X Archiver` and Remmina as `Remote Desktop`.
  Display GParted as `Partition Manager` and VLC as `Video Player`.
  Hide Favorites with `navigation.favorites.visible=false` in Slax's Focus profile;
  keep All apps, the application categories and Session available.
- The automount udev rule queues a transient systemd service with `--no-block`.
  Pass ACTION, DEVNAME and ID_FS_TYPE as arguments to `slax-automount`; do not
  mount synchronously inside udev, which blocks GParted's `udevadm settle`.
- xlunch keeps each fitted label and its measured width until the cell width
  changes. Repeated Imlib2 measurements shifted `Terminal` by one pixel on hover;
  partial redraws must preserve label geometry, including for the first entry.
- The Fluxbox patch shows themed resize cursors on all four borders and corners.
  Hover and dragging share border hit testing; restore the inherited cursor when
  leaving the frame, and do not advertise resizing on fixed-size or shaded windows.
  Frame pointer motion without a pressed button must not invoke drag bindings.
  Resize the frame and clients continuously during a drag, without an outline.
  Use root pointer coordinates and the initial frame geometry to avoid drift;
  preserve application size hints, including character-cell increments.
  Advertise `_NET_WM_STATE_FOCUSED` and publish it for the focused client,
  including focus loss and tab switches. GTK4 otherwise treats all windows
  as active and never applies its inactive decoration colors.
  Window buttons support optional `.hover.pixmap` and `.unfocus.hover.pixmap`
  resources, with distinct pressed artwork. `window.restore` supplies the
  maximize button's artwork on maximized windows. Hover must clear on leave,
  including a cancelled click, and preserve the current focus palette.
  Disable the theme's corner cutouts on fully maximized and fullscreen frames.
  Refresh the shape on state changes even when the geometry stays unchanged.
  Set the opaque alpha byte on 32-bit ARGB window borders; theme RGB pixels
  otherwise produce incorrect colors around server-decorated GTK4 windows.
  Keep the frame background pixmap `None` so destroying a client preserves its
  last pixels for compositor fade-out instead of replacing them with white.
  The patch also fixes `Menu::updateMenu()`: child windows and backing
  pixmaps use the recomputed menu width, so changing column count cannot clip
  the resolution list. Verify both columns at 800x600 and the menu at 1024x768.
  Give the title-bar context menu the `fluxbox-window-menu` role so it stays
  square; regular `fluxbox-menu` windows retain Picom's rounded upper corners.
  Slax defaults to the patched `RandomPlacement` policy. Place the full frame
  and external tabs inside the selected monitor's work area, respecting panel
  struts. Oversized windows keep their upper-left corner accessible. Preserve
  explicit application positions and dialog placement handled before this policy.
  Fluxbox's SIGUSR1 restarts it with exec, preserving its PID, Xorg and applications;
  SIGUSR2 reloads configuration. Do not confuse these signals.
- GTK 3 scrollbar overrides live in
  `03-desktop/rootcopy/usr/share/slax/gtk-3.0/scrollbars.css`: white tracks and
  junctions, a faint horizontal thumb, and Chromium-gray vertical thumbs with
  triangular arrows. Root, guest and `/etc/skel` import this shared file through
  `.config/gtk-3.0/gtk.css`. Preserve the selected GTK theme and its dark variant;
  replacing it with a light-only theme also changes the taskbar. Keep the CSS
  and its tiny SVG arrows in rootcopy, applied after cleanup. Cursor defaults
  remain in `02-xorg/rootcopy/etc/gtk-3.0/settings.ini`.
- Match window frames to Chromium's light palette: focused `#d3e3fd`, inactive
  `#dde3e9`, 8 px upper corners, square lower corners and 4 px borders. Keep Fluxbox titles compact at
  28 px and leave application content themes unchanged. Shared GTK 3/4 rules
  are in `03-desktop/rootcopy/usr/share/slax/gtk-{3,4}.0/windows.css`, imported
  by root, guest and `/etc/skel`. Hide borders and use square corners on
  maximized/fullscreen GTK windows. Keep Fluxbox's `window.roundCorners` empty;
  Picom supplies the antialiased mask instead of Fluxbox's binary shape cutouts.
  Window buttons use bare gray symbols, with circles only on hover/press.
  The four Adwaita symbolic window icons and Fluxbox's 22 px button pixmaps
  belong to `03-desktop/rootcopy`; refresh the icon cache after copying them.
  Enable `gtk-dialogs-use-header` in GTK 3 and GTK 4 settings so native file
  dialogs retain the same header style. No XSettings daemon is needed.
- GTK 3/4 file choosers default to 800 by 500 pixels through
  `03-desktop/rootcopy/usr/share/glib-2.0/schemas/90-slax-filechooser.gschema.override`.
  Compile schemas in `03-desktop/copy` after rootcopy. Keep this a system default;
  users can resize dialogs and GTK remembers their choice. Do not ship a binary
  user dconf database with unrelated preferences.
- Application CSS belongs to `04-apps/rootcopy/usr/share/slax/gtk-3.0/apps`.
  The small `04-apps/gtk-app-style.c` GTK module loads only the stylesheet named
  for the running program. Compile it in the disposable build branch, install
  it in `04-apps/copy` after cleanup and enable it there. Keep native themes,
  application commands and launchers unchanged; styles must work from xlunch,
  Fluxbox and the terminal without leaking a theme environment to child apps.
- Keep PCManFM's standard Debian package. The GTK module hides its root warning
  and adjacent separator when the main toolbar is realized, identifying the
  stock warning icon without translated text. Do not rebuild PCManFM for this.
- Use Docklike Taskbar for grouped application icons and hover previews.
  Build its Debian package with `docklike-preview-sizing.diff`: every thumbnail
  uses the same fraction of the panel monitor's work-area height, preserving
  aspect ratios. Thumbnail widths determine cards and long titles are ellipsized.
  Set this height fraction with `previewScale` in `docklike-2.rc` (default 0.2).
  Cap it with `previewMaxHeight` in logical pixels (default 160; 0 disables the cap).
  `docklike-preview-peek.diff` sets a temporary `_SLAX_PREVIEW_DIM` property on
  other clients while a visible window's preview is hovered. Picom dims those
  application windows, retaining focus, stacking and minimization. Remove the
  properties on leave, click, menu hide and item destruction. Minimized previews
  do not trigger the effect. Preserve the panel and preview popup's normal opacity.
  Its defaults live in `03-desktop/rootcopy/root/.config/xfce4/panel/docklike-2.rc`.
  Keep the panel's `icon-size` automatic so xlunch retains its launcher size;
  set only Docklike's icons to 32 px. Its native indicator uses blue for the
  active group and gray for other running applications. The shared
  `03-desktop/rootcopy/usr/share/slax/gtk-3.0/panel.css` adds a lighter active
  background. Import it for root, guest and new users alongside scrollbar styles.
- Start the panel and Picom on a near-black (`#010101`) root pixmap, then
  show the wallpaper after the panel's initial redraw. Keep its 84 percent
  opacity. The Slax theme uses `background: unset`; `none` restores the last
  background at startup. `fbscreensize` runs `sh ~/.fehbg` to restore the image
  and scaling mode: Fluxbox menus use `DISPLAY=:0.0` while startup uses `:0`,
  so `fbsetbg -l` can select a different record. Do not ship a lastwallpaper file.
- Picom replaces Compton and uses `/etc/picom.conf` at startup and after
  resolution changes. Build its Debian package with `picom-top-corners.diff`;
  install only the rebuilt package in the runtime branch. Remove Debian's
  unused `python3` dependency from its control file: Picom and picom-inspect
  are native programs and picom-trans is a POSIX shell script. Python belongs
  only in the disposable build branch when required by build tools.
  `xrender-round-top-only` preserves square lower corners in XRender masks.
  Use XRender without OpenGL shaders and repaint full frames to avoid stale
  translucent pixels. Round WM frames and opaque normal/dialog windows; opaque
  client-decorated GTK windows cannot draw transparent corners themselves.
  ARGB client-decorated windows retain their own corners. Keep frames and Chromium opaque, fade normal/dialog
  windows in and out, and shadow normal/dialog/dock windows. Fully maximized and
  fullscreen windows have neither corner masks nor shadows. Preserve panel
  transparency and remove Picom's desktop/autostart launchers during cleanup.
- PCManFM uses the opened folder's icon in the tasklist. Preserve the original
  `folder.png` artwork in `04-apps`, including the Adwaita and gnome theme paths.
  Keep the selected File Manager artwork for `slax-file-manager` and
  `system-file-manager`; do not alias it to the generic `folder` icon.
- Normalize copied systemd service files to mode 644 in their recipes. Set
  filesystem branch roots to mode 755 before packing: rootcopy may carry
  group-writable directory modes, which cause AUFS permission warnings.
  In `03-desktop/copy`, set all commands from `rootcopy/usr/bin` to mode 755;
  checkout permissions alone cannot guarantee that helpers such as
  `fbscreensize` remain executable.
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

- Slax AI is a separate project in the sibling `../slax-ai` checkout:
  `client` contains the local Rust agent and embedded browser UI; `server`
  contains the remote Rust/SQLite backend. Its development rules live there.
  Slax build integration is deferred; no AI module is currently included in
  the build recipes. Keep the server and its credentials outside the ISO.

- `VM/start.cmd` opens the visible work VM. `VM/start.ps1` also starts
  `collect-slax-build.py`, which downloads only the completed ISO and verifies
  its SHA-256 before publishing `VM/slax-build-trixie/slax-amd64.iso`.
- Guest helper: `/root/continue-slax-build.sh`; service:
  `/etc/systemd/system/slax-autobuild.service`. Their local sources are in
  `VM`. The service disables itself after successful completion.
- Clear the old `SHA256SUMS` completion marker before a new automated build.
  Publish it only after the replacement ISO is complete.
- Before replacing a local ISO, detach it from the stopped test VM and check
  whether Windows also mounted that specific ISO. Dismount that image if needed;
  a Windows CD mount can keep the file locked after VirtualBox releases it.
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
  before changing firmware. Stop test VMs after verification. Use Slax's normal
  shutdown whenever persistence or automount can have writable disks mounted,
  even when booting from CD. `sync` does not replace unmounting. Use VirtualBox
  `controlvm ... poweroff` only when no writable filesystems remain mounted.
  Shut the Debian work VM down normally.
- Inject test keystrokes only after GRUB has started. Early input during POST
  caused a local SeaBIOS keyboard error. Keep automation timing fixes outside
  Slax. To test sessions on the separate FAT32 disk while booting from CD,
  append `perchdir=/dev/sda1/changes` in GRUB's command-line editor.
- The scratch VM boots the current distribution ISO at SATA port 0 without
  a hard disk. Attach separate disposable media explicitly for persistence
  tests; the launcher must not create or format disks. Never use the work
  VM's disk for those tests. Do not create a separate local test ISO or add
  VM-specific arguments to the distribution boot configuration. Use GRUB's
  command-line editor for an external target such as `perchdir=/dev/sda1/changes`.
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
- Keep tty1's getty ordered after `xorg.service`, whose `ExecStartPost` waits
  for X client connections. `Xdetect` must pass the service's display and VT
  arguments to `startx`. Try native hardware autodetection first; if startup
  fails, run `Xorg -configure` and retry. Do not probe before the first start.
- When Xorg switches away from tty1, fbmenu must preserve its graphics and
  pixels instead of restoring text and a blinking cursor. Getty restores tty1
  after X is ready, or after the graphics startup has failed.
  Use the VT_PROCESS release handshake; canonical input alone does not mean
  getty has taken over, because init resets termios too. Slax's optional
  `/usr/libexec/fbmenu-driver` keeps animation running until graphics modules load.
  `02-xorg/copy` derives an override of Debian's `80-drivers.rules`, preserving
  other rules and the normal loader for non-graphics modules. Identify graphics
  through module dependency paths, not specific GPU names. Bracket loading with
  `fbmenu --pause` / `--continue`; count overlapping loads and resume on errors.
  Release the firmware mapping before loading drivers so it cannot pin display
  memory. Clear the dots during this pause. Without the optional helper, release
  at root handoff and wait for the framebuffer name to change before reopening.
  After reopening, activate the driver's reported mode with FB_ACTIVATE_FORCE:
  fbcon skips mode setup while the VT is graphical. Do not select a resolution.
- The getty drop-in runs `slax-fbcon-palette` on all text consoles, using
  near-black (#010101) instead of zero-filled black. This avoids the stale
  desktop pixels observed with vmwgfx without patching or recompiling the kernel.
  Apply this palette globally, without restricting the driver or pixel format.
  Restore the visible login cursor there too; the boot configuration hides it.
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
- `perchdir=ask` lists sessions from `/$LIVEKITNAME/changes` on all detected
  writable filesystems, preserving a custom boot-data directory on the boot
  filesystem. Show the device beside each session to disambiguate names.
  New sessions, including `perchdir=new`, first ask for a writable partition,
  showing its device, label, filesystem and free/total space even without sessions.
  Format both sizes in the same unit, for example `54.3/64.0 GB free`.
  Disk and name cancellation return to the session picker; only cancelling
  that picker disables persistence. Create directories only after the name is
  confirmed. Keep this flow in `livekitlib`; `fbmenu` remains a generic menu.
  Release unselected scan mounts and retain the chosen disk at
  `/memory/persistence`. Duplication stays on the source disk. Automatic
  `perchdir=resume` still uses the boot medium; explicit device paths stay scoped
  to their selected directory. Never format or repartition a discovery target.
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
- Update `initramfs/static/mount.dynfilefs` from the sibling project's
  `../dynfilefs/static/dynfilefs`, then synchronize the work VM and repack
  Slax's initramfs. The current static 32-bit x86 binary is DynFileFS 5.01,
  using storage format 500. It cannot open the old format 400 sessions.
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
- Shutdown keeps optical media attached in virtual machines, detected from
  the CPU hypervisor flag, virtual-machine DMI identifiers or sysfs hypervisor
  information. Keep this check in the initramfs shutdown script without runtime
  package dependencies. Physical CD/DVD boots retain eject and tray closing;
  filesystem unmounting must run in both cases.
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
