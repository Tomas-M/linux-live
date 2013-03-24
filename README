Linux Live Kit
==============

Use this set of scripts to turn your existing preinstalled Linux
distribution into a Live Kit (formely known as Live CD).
Make sure to extract and use it on a posix-compatible filesystem,
since it creates some (sym)links and such.

* Before you start building your Kit, edit the file ./.config
  Most importantly change the LIVEKITNAME variable.

* Make sure your kernel is in /boot/vmlinuz

* You may also wish to replace boot graphics in ./bootfiles/bootlogo.png
  and reorganize isolinux.cfg to fit your needs (when editing the file,
  keep all paths in /boot/, it will be rellocated during LiveKit creation)

* If you plan to boot your Live Kit from CD, you need to recompile
  syslinux.bin else it won't be able to boot your Live Kit from directory
  LIVEKITNAME. There is a script prepared for you which will handle all
  of that. Simply go to directory ./tools/ and run isolinux.bin.update ...
  it will update ./bootfiles/isolinux.bin automatically by downloading
  isolinux sources, patching them using your actual LIVEKITNAME and
  recompiling. This step is not needed if you plan to boot from USB only.

* If you have tmpfs mounted on /tmp, make sure you have enough RAM
  since LiveKit will store lots of data there. If you are low on RAM,
  make sure /tmp is a regular on-disk directory.

* When done, run the ./build script to create your Live Kit
  - it will create ISO and TAR files for you in /tmp
  - make sure you have enough free space in /tmp to handle it


Author: Tomas M. <http://www.linux-live.org>
