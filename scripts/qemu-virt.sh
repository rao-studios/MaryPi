#!/bin/sh
# Boot a MaryPi image under QEMU's "virt" machine: GICv2, PL011 UART, generic
# timer and EDK2 UEFI. ACPI is disabled so EDK2 publishes the FDT the booter
# reads (with ACPI on, ArmVirtQemu hides the device tree). This is the ravynOS arm64 kernel bring-up test bed;
# QEMU has no Raspberry Pi 5 model.
#
#   swift run marypi build-image --qemu --out /tmp/q.img
#   scripts/qemu-virt.sh /tmp/q.img
#
# Environment:
#   QEMU            path to qemu-system-aarch64 (default: from PATH or Homebrew)
#   QEMU_ACCEL      tcg (default, Cortex-A76 model) or hvf (Apple Silicon, -cpu host)
#   QEMU_EFI_CODE   EDK2 code image (default: <qemu share>/edk2-aarch64-code.fd)
#   QEMU_MEMORY     guest RAM in MiB (default 2048)
#   MARYPI_QEMU_WORK where the writable UEFI variable store lives
set -eu

IMG=${1:?usage: qemu-virt.sh <image.img> [extra qemu args...]}
shift || true

QEMU=${QEMU:-$(command -v qemu-system-aarch64 || true)}
if [ -z "$QEMU" ]; then
    QEMU=/opt/homebrew/bin/qemu-system-aarch64
fi
if [ ! -x "$QEMU" ]; then
    echo "qemu-system-aarch64 not found; brew install qemu" >&2
    exit 1
fi

SHARE=$(cd "$(dirname "$(readlink -f "$QEMU")")/../share/qemu" && pwd)
CODE=${QEMU_EFI_CODE:-$SHARE/edk2-aarch64-code.fd}
VARS_TEMPLATE=$SHARE/edk2-arm-vars.fd
WORK=${MARYPI_QEMU_WORK:-$HOME/Library/Caches/MaryPi/qemu}
mkdir -p "$WORK"
if [ ! -f "$WORK/vars.fd" ]; then
    cp "$VARS_TEMPLATE" "$WORK/vars.fd"
fi

ACCEL=${QEMU_ACCEL:-tcg}
if [ "$ACCEL" = hvf ]; then
    CPU=host
else
    CPU=cortex-a76
fi

echo "qemu-virt: $QEMU ($ACCEL, -cpu $CPU) image=$IMG uefi=$CODE" >&2
echo "qemu-virt: serial console is this terminal; Ctrl-A X quits" >&2
exec "$QEMU" \
    -M virt,gic-version=2,acpi=off -cpu "$CPU" -smp 1 -m "${QEMU_MEMORY:-2048}" -accel "$ACCEL" \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file="$WORK/vars.fd" \
    -drive file="$IMG",format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -serial mon:stdio -nographic \
    "$@"
