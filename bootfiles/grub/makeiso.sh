#!/bin/sh
# Create a CD/DVD and raw-disk bootable ISO using the bundled GRUB loaders.
set -e

TARGET=${1:?Usage: makeiso.sh output.iso}
TARGET="$(cd "$(dirname "$TARGET")" && pwd -P)/$(basename "$TARGET")"
shift
BOOT=$(cd "$(dirname "$0")/.." && pwd -P)
NAME=$(basename "$(dirname "$BOOT")")
cd "$BOOT/../../"

TEMP=$(mktemp -d "${TMPDIR:-/tmp}/livekit-iso.XXXXXX")

# Release our temporary FAT mount before removing the image, including on failure.
finish()
{
   STATUS=$?
   trap - 0
   if mountpoint -q "$TEMP/mnt"; then
      umount "$TEMP/mnt" || exit 1
   fi
   rm -r "$TEMP"
   exit "$STATUS"
}
trap finish 0
trap 'exit 130' INT
trap 'exit 143' TERM

# Recreate EFI media from the bundled loaders, without requiring an existing ESP.
SIZE=$(du -sk "$BOOT/EFI" | cut -f1)
# Allow both FATs plus 128 KiB for directories and free space, rounded to 64 KiB.
SIZE=$(( (SIZE + SIZE / 128 + 128 + 63) / 64 * 64 ))
if [ "$SIZE" -lt 2222 ]; then
   SIZE=2222
fi
# Keep the cluster count in the FAT16 range, including larger generic loader sets.
SECTORS=1
while [ "$(( SIZE * 2 / SECTORS ))" -ge 65525 ]; do
   SECTORS=$(( SECTORS * 2 ))
done
dd if=/dev/zero of="$TEMP/efi.img" bs=1024 count="$SIZE" 2>/dev/null
mkfs.fat -F 16 -s "$SECTORS" -S 512 "$TEMP/efi.img" >/dev/null
mkdir "$TEMP/mnt"
mount -o loop "$TEMP/efi.img" "$TEMP/mnt"
cp -r "$BOOT/EFI" "$TEMP/mnt/"
umount "$TEMP/mnt"

# Keep the CD read-ahead padding inside the setup partition, not in a GPT Gap entry.
mkdir "$TEMP/usb-space"
tar -xJf "$BOOT/grub/usb-space.tar.xz" -C "$TEMP/usb-space"
SIZE=$(du -sk "$TEMP/usb-space" | cut -f1)
SIZE=$(( (SIZE + SIZE / 128 + 128 + 63) / 64 * 64 ))
if [ "$SIZE" -lt 2222 ]; then
   SIZE=2222
fi
SECTORS=1
while [ "$(( SIZE * 2 / SECTORS ))" -ge 65525 ]; do
   SECTORS=$(( SECTORS * 2 ))
done
LABEL=USB-SPACE
dd if=/dev/zero of="$TEMP/setup.img" bs=1024 count="$SIZE" 2>/dev/null
mkfs.fat -F 16 -s "$SECTORS" -S 512 -n "$LABEL" "$TEMP/setup.img" >/dev/null
mount -o loop "$TEMP/setup.img" "$TEMP/mnt"
cp -r "$TEMP/usb-space/"* "$TEMP/mnt/"
umount "$TEMP/mnt"
dd if=/dev/zero bs=1024 count=300 >> "$TEMP/setup.img" 2>/dev/null

set -- -m "$NAME/boot/grub/efi.img" -e --interval:appended_partition_2::: -no-emul-boot -o "$TARGET" -graft-points "$@" .
# Expose the ISO filesystem and EFI loader as separate, non-overlapping partitions.
set -- -partition_offset 16 -append_partition 2 0xef "$TEMP/efi.img" -append_partition 3 0x06 "$TEMP/setup.img" -appended_part_as_gpt "$@"
set -- -no-pad -iso_mbr_part_type EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 "$@"
if [ -f "$NAME/boot/grub/cdboot.img" ]; then
   set -- -b "$NAME/boot/grub/cdboot.img" -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info -eltorito-alt-boot "$@"
   set -- --grub2-mbr "$BOOT/grub/boot_hybrid.img" --mbr-force-bootable "$@"
fi
set -- -as mkisofs -J -R -A "$NAME" -V "$NAME" -c "$NAME/boot/grub/boot.cat" "$@" --
if [ -n "${XORRISO_PATH_LIST:-}" ]; then
   set -- "$@" -pathspecs on -backslash_codes with_quoted_input -quoted_path_list "$XORRISO_PATH_LIST"
fi
# Stock signed loaders can read their bootstrap through either EFI or ISO media.
# Hide only the root copy from Joliet; keep boot/EFI available for USB setup.
set -- "$@" -map "$BOOT/EFI" /EFI -hide joliet /EFI --
xorriso "$@"

# Windows honors hidden/no-drive-letter bits only for its Basic Data partition type.
# Preserve GRUB's exact MBR, including the active dummy entry sfdisk would remove.
dd if="$TARGET" of="$TEMP/mbr" bs=512 count=1 2>/dev/null
ATTRS=$(sfdisk --part-attrs "$TARGET" 1)
sfdisk --part-attrs "$TARGET" 1 "$ATTRS,GUID:62,GUID:63" >/dev/null
dd if="$TEMP/mbr" of="$TARGET" bs=512 count=1 conv=notrunc 2>/dev/null
