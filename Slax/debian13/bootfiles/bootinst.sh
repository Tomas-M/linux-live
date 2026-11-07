#!/bin/sh
# Prepare USB BIOS boot and the standard UEFI removable-media directory.
set -e

BOOT=$(cd "$(dirname "$0")" && pwd -P)
if [ "$(id -u)" != 0 ]; then
   exec sudo sh "$BOOT/bootinst.sh" "$@"
fi
if [ -f "$BOOT/grub/bootlogo.txt" ]; then cat "$BOOT/grub/bootlogo.txt"; fi

# With no argument, use the parent disk of the partition containing this script.
PARTITION=$(grub-probe --target=device "$BOOT")
BOOT_DISK=$(grub-probe --target=disk "$BOOT")
DISK=${1:-$BOOT_DISK}
case "$DISK" in /*) ;; *) DISK="/dev/$DISK" ;; esac
if [ ! -b "$DISK" ]; then
   echo "Cannot determine the USB disk. Usage: bootinst.sh /dev/whole-usb-disk" >&2
   exit 1
fi
case "$(grub-probe --device --target=drive "$DISK")" in
   *,*) echo "Specify a whole USB disk, not a partition." >&2; exit 1 ;;
esac
DISK=$(grub-probe --device --target=disk "$DISK")

# Verify the selected disk contains these files and does not contain the host root.
if [ "$DISK" != "$BOOT_DISK" ]; then
   echo "The boot directory is not on $DISK." >&2
   exit 1
fi
ROOT_DISKS=$(grub-probe --target=disk /)
if printf '%s\n' "$ROOT_DISKS" | grep -qxF "$DISK"; then
   echo "Refusing to install on the disk containing the running system." >&2
   exit 1
fi

# Check the filesystem before copying files or updating any boot records.
TYPE=$(grub-probe --target=fs "$BOOT")
if [ "$TYPE" != fat ]; then
   echo "UEFI boot requires FAT12, FAT16 or FAT32. No changes were made." >&2
   exit 1
fi

# Install BIOS first; still prepare UEFI if BIOS embedding is unavailable.
echo "Installing GRUB 2 BIOS boot on $DISK."
BIOS_STATUS=0
grub-install --target=i386-pc --boot-directory="$BOOT" "$DISK" || BIOS_STATUS=$?
# Walk physical parent directories to locate the containing filesystem root.
MOUNT=$BOOT
while [ "$MOUNT" != / ]; do
   PARENT=$(dirname "$MOUNT")
   [ "$(grub-probe --target=device "$PARENT")" = "$PARTITION" ] || break
   MOUNT=$PARENT
done
mkdir -p "$MOUNT/EFI/BOOT"
cp -a "$BOOT/EFI/." "$MOUNT/EFI/"
echo "UEFI boot files installed in $MOUNT/EFI/BOOT."

[ "$BIOS_STATUS" -eq 0 ] || exit "$BIOS_STATUS"
echo "BIOS boot installation finished."
