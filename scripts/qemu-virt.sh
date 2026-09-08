#!/bin/sh
# Compatibility wrapper: the VM launcher moved to vm/run.sh.
#   scripts/qemu-virt.sh <image.img> [extra qemu args...]
# maps to
#   vm/run.sh run --display serial --image <image.img> -- [extra qemu args...]
# QEMU / QEMU_EFI_CODE / QEMU_MEMORY / QEMU_ACCEL are honoured for old scripts.
set -eu
IMG=${1:?usage: qemu-virt.sh <image.img> [extra qemu args...]}
shift || true
echo "qemu-virt.sh: deprecated, use vm/run.sh (see vm/README.md)" >&2
[ -n "${QEMU:-}" ] && export MARYPI_VM_QEMU="$QEMU"
[ -n "${QEMU_EFI_CODE:-}" ] && export MARYPI_VM_EFI_CODE="$QEMU_EFI_CODE"
RUN="$(dirname "$0")/../vm/run.sh"
if [ -n "${QEMU_ACCEL:-}" ]; then
    exec "$RUN" run --display serial --image "$IMG" --accel "$QEMU_ACCEL" ${QEMU_MEMORY:+--memory "$QEMU_MEMORY"} -- "$@"
fi
exec "$RUN" run --display serial --image "$IMG" ${QEMU_MEMORY:+--memory "$QEMU_MEMORY"} -- "$@"
