#!/bin/bash
# Build the y2s shim and wrap it in a Galaxy S20+ (SM-G986B) boot.img v2.
#
#   shim/build.sh PAYLOAD.bin [UNPACK_DIR]
#
# PAYLOAD.bin: tools/mkpayload.py output (kernel + ADT), appended to the shim
# image (out/Image) for the phone; out/Image.bare is the shim alone (QEMU
# --shim passes the payload as initrd instead). UNPACK_DIR: the unpacked
# stock boot.img (default ~/ios-s20/unpack): its dtb and ramdisk are used
# unchanged, as in the bare-metal tests S-Boot booted (S-Boot did not start
# an image whose ramdisk was the 16 MB payload).
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
PAYLOAD=${1:?usage: shim/build.sh PAYLOAD.bin [UNPACK_DIR]}
UNPACK=${2:-$HOME/ios-s20/unpack}
DTB=$UNPACK/dtb
RAMDISK=$UNPACK/ramdisk
[ -f "$DTB" ] && [ -f "$RAMDISK" ] || { echo "error: need $DTB and $RAMDISK" >&2; exit 1; }
mkdir -p out
for f in start.S shim.c console.c fdt.c loader.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever S-Boot puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/fdt.o out/loader.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
# binutils < 2.39 (Mint 21 / Ubuntu 22.04) has no --no-warn-rwx-segments.
LDWARN=""
aarch64-linux-gnu-ld --help 2>/dev/null | grep -q no-warn-rwx-segments && LDWARN="-Wl,--no-warn-rwx-segments"
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none $LDWARN -Wl,-N \
	out/start.o out/shim.o out/console.o out/fdt.o out/loader.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# Pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
# The loader looks for the payload at _end, i.e. right after the padded image.
cp out/Image out/Image.bare
cat "$PAYLOAD" >> out/Image
mkbootimg --header_version 2 --os_version 14.0.0 --os_patch_level 2024-10 \
	--kernel out/Image --ramdisk "$RAMDISK" --dtb "$DTB" \
	--pagesize 0x00000800 --base 0x00000000 --kernel_offset 0x10008000 \
	--ramdisk_offset 0x11000000 --second_offset 0x00000000 --tags_offset 0x10000100 \
	--dtb_offset 0x0000000010000000 --board '' --cmdline '' -o out/y2s-boot.img
# Some mkbootimg builds silently drop --os_patch_level (the first S20 images
# had 0 and S-Boot never started them; every image it did start had 2024-10).
# Set os_version 14.0.0 + patch level 2024-10 in the header directly.
python3 - out/y2s-boot.img <<'PY'
import struct, sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
assert b[:8] == b'ANDROID!', 'not a boot image'
want = (14 << 25) | (0 << 18) | (0 << 11) | ((2024 - 2000) << 4) | 10
struct.pack_into('<I', b, 44, want)
open(p, 'wb').write(b)
print('os_version field: %#x' % struct.unpack_from('<I', b, 44)[0])
PY
ls -l out/Image.bare out/Image out/y2s-boot.img
