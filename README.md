## Linux Live Kit Improved.
This set of scripts will help you to
build your own Live Kit distro. This project is based on
the original Linux Live Kit. <http://linux-live.org>


## You will need to have the following installed:
  * squashfs-tools
  * genisoimage and/or mkisofs (optional)
  * zip

## Before you build:

- Store Linux Live Kit in a directory which is not going to be included
  in your live distro, else it would be copied to it.
  The best practice is to make a directory such
  as for example: '/tmp/a', and put all the files there.

  Make sure to extract and use it on a POSIX-compatible
  filesystem (for ex: EXT4), since it will create symlinks 
  and such that might not be compatible in for example: FAT 
  and such other filesystems.

- Before you start building your Live Kit, please consider
  editing the ./.config file, and tweak it to suit your needs.
  Most importantly, change the LIVEKITNAME variable.

- Make sure you are pointing to the right kernel. If in doubt, 
  change the path in ./.config. Your kernel must support both 
  SquashFS and AUFS, or else you will get an error both while 
  building, and when you actually boot your Live Kit.
  
  On most Linux distro-s, the kernel is in /vmlinuz, a
  symlink to /boot/vmlinuz-* or even /boot/vmlinux-*


  Debian Jessie's kernel supports both AUFS and SquashFS out-of-the-box.
  It's recommended to use Debian Jessie as the base OS, but the choice is yours.

- It's recommended to replace boot background in
  bootfiles/bootpic.png and edit syslinux.cfg to fit your needs.
  
  When editing the configuration file, make sure to keep all paths
  pointing to /boot/, since it will be replaced to /LIVEKITNAME/boot/
  during the build session.

- Linux Live Kit comes with precompiled static binaries in ./initramfs
  directory. Those may be outdated but will work. You may replace them
  by your own statically linked binaries, if you know how to compile them.

- If you want to boot your Live Kit from a CD, you'll need to recompile
  syslinux.bin and/or isolinux.bin else it won't be able to boot 
  your Live Kit from directory "/LIVEKITNAME".
 
  There is a script prepared for you which will do all of the building. 
  Simply go to directory ./tools/ and run isolinux.bin-update, it will rebuild 
  isolinux.bin automatically by downloading SysLinux sources, patching them using 
  your actual LIVEKITNAME and recompiling. This step is not needed if you only need 
  the ZIP archive version.

- If you have tmpfs mounted on /tmp, make sure you have enough (or maybe even
  a lot of) RAM, since Live Kit will store lots of data to the target.
  If you don't have enough (or even went out of) RAM, make sure
  that /tmp is a regular on-disk directory.

- If you want to include your own bundules, then the 'include\_bund/' directory
  might come in handy! Just make sure that the file extension will match
  with your chosen .BEXT extension.

When you're ready, run the ./build script to build your Live Kit Distro.
  The script will:
  + Build both ISO and ZIP files for you in /tmp
  + Do all the hard work for you.
  + Will automatically check for errors.
  + Will warn you if you don't have the correct packages.

NOTE:
WHEN YOU DOWNLOAD THIS KIT, MAKE SURE THAT ./build AND ./livekitlib IS
EXECUTABLE!
