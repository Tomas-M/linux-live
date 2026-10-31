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
if [ "$SIZE" -lt 2112 ]; then
   SIZE=2112
fi
# Keep the cluster count in the FAT16 range, including larger generic loader sets.
SECTORS=1
while [ "$(( SIZE * 2 / SECTORS ))" -ge 65525 ]; do
   SECTORS=$(( SECTORS * 2 ))
done
dd if=/dev/zero of="$TEMP/efi.img" bs=1024 count="$SIZE" 2>/dev/null
if command -v mformat >/dev/null 2>&1; then
   mformat -c "$SECTORS" -M 512 -i "$TEMP/efi.img" ::
   mcopy -i "$TEMP/efi.img" -s "$BOOT/EFI" ::/
else
   mkfs.fat -F 16 -s "$SECTORS" -S 512 "$TEMP/efi.img" >/dev/null
   mkdir "$TEMP/mnt"
   mount -o loop "$TEMP/efi.img" "$TEMP/mnt"
   cp -r "$BOOT/EFI" "$TEMP/mnt/"
   umount "$TEMP/mnt"
fi

set -- -m "$NAME/boot/grub/efi.img" -e --interval:appended_partition_2::: -no-emul-boot -o "$TARGET" -graft-points "$@" .
# Expose the ISO filesystem and EFI loader as separate, non-overlapping partitions.
set -- -partition_offset 16 -append_partition 2 0xef "$TEMP/efi.img" -appended_part_as_gpt "$@"
if [ -f "$NAME/boot/grub/cdboot.img" ]; then
   set -- -b "$NAME/boot/grub/cdboot.img" -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info -eltorito-alt-boot "$@"
   set -- --grub2-mbr "$BOOT/grub/boot_hybrid.img" --mbr-force-bootable "$@"
fi
set -- -as mkisofs -J -R -A "$NAME" -V "$NAME" -c "$NAME/boot/grub/boot.cat" "$@"
if [ -n "${XORRISO_PATH_LIST:-}" ]; then
   set -- "$@" -- -pathspecs on -backslash_codes with_quoted_input -quoted_path_list "$XORRISO_PATH_LIST"
fi
xorriso "$@"
