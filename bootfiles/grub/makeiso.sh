#!/bin/sh
# Create an ISO from this media tree without rebuilding its GRUB loaders.
set -e

TARGET=${1:?Usage: makeiso.sh output.iso}
TARGET="$(cd "$(dirname "$TARGET")" && pwd -P)/$(basename "$TARGET")"
shift
BOOT=$(cd "$(dirname "$0")/.." && pwd -P)
NAME=$(basename "$(dirname "$BOOT")")
cd "$BOOT/../../"

set -- -e "$NAME/boot/grub/efi.img" -no-emul-boot -o "$TARGET" -graft-points "$@" .
if [ -f "$NAME/boot/grub/cdboot.img" ]; then
   set -- -b "$NAME/boot/grub/cdboot.img" -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info -eltorito-alt-boot "$@"
fi
set -- -as mkisofs -J -R -A "$NAME" -V "$NAME" -c "$NAME/boot/grub/boot.cat" "$@"
if [ -n "${XORRISO_PATH_LIST:-}" ]; then
   set -- "$@" -- -pathspecs on -backslash_codes with_quoted_input -quoted_path_list "$XORRISO_PATH_LIST"
fi
xorriso "$@"
