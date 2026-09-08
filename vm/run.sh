#!/bin/sh
# MaryPi VM launcher: boots a MaryPi image under QEMU's "virt" machine with
# EDK2 UEFI on arm64 macOS or Linux, in a window or on the terminal.
#
#   vm/run.sh run    [--image IMG] [--profile NAME] [--accel auto|hvf|kvm|tcg]
#                    [--display window|serial|none] [--memory MiB] [--detach]
#                    [--dry-run] [--print-argv] [--no-tty-serial] [-- qemu args]
#   vm/run.sh stop   [--profile NAME]
#   vm/run.sh status [--profile NAME]
#   vm/run.sh serial [--profile NAME]
#   vm/run.sh doctor [--profile NAME]
#
# Profiles live in vm/profiles/<name>.conf; state (UEFI variables, disk image,
# serial.log, QMP socket, pid) in vm/state/<profile>/ inside a checkout, or in
# ~/Library/Caches/MaryPi/vm (macOS) / $XDG_CACHE_HOME/marypi/vm (Linux).
#
# Environment: MARYPI_VM_QEMU, MARYPI_VM_EFI_CODE, MARYPI_VM_EFI_VARS,
#              MARYPI_VM_DIR, MARYPI_VM_STATE
#
# The argument order produced here is mirrored by MaryPiKit's QEMUCommand and
# checked by RunShParityTests; keep the two in sync.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
VM_DIR=${MARYPI_VM_DIR:-$SCRIPT_DIR}
HOST_OS=$(uname -s)
HOST_ARCH=$(uname -m)
PFLASH_SIZE=67108864

die() { printf 'vm: %s\n' "$*" >&2; exit 1; }
warn() { printf 'vm: %s\n' "$*" >&2; }
usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

# ---------------------------------------------------------------- state root
if [ -n "${MARYPI_VM_STATE:-}" ]; then
    STATE_ROOT=$MARYPI_VM_STATE
elif [ -f "$VM_DIR/../Package.swift" ]; then
    STATE_ROOT=$VM_DIR/state
elif [ "$HOST_OS" = Darwin ]; then
    STATE_ROOT=$HOME/Library/Caches/MaryPi/vm
else
    STATE_ROOT=${XDG_CACHE_HOME:-$HOME/.cache}/marypi/vm
fi

# ---------------------------------------------------------------- arguments
CMD=${1:-}
[ -n "$CMD" ] || usage 1
shift
PROFILE=qemu-virt
IMAGE=
ACCEL=auto
DISPLAY_MODE=window
MEMORY=
DETACH=0
DRY_RUN=0
PRINT_ARGV=0
NO_TTY_SERIAL=0
while [ $# -gt 0 ]; do
    case $1 in
        --image) IMAGE=$2; shift 2 ;;
        --image=*) IMAGE=${1#*=}; shift ;;
        --profile) PROFILE=$2; shift 2 ;;
        --profile=*) PROFILE=${1#*=}; shift ;;
        --accel) ACCEL=$2; shift 2 ;;
        --accel=*) ACCEL=${1#*=}; shift ;;
        --display) DISPLAY_MODE=$2; shift 2 ;;
        --display=*) DISPLAY_MODE=${1#*=}; shift ;;
        --memory) MEMORY=$2; shift 2 ;;
        --memory=*) MEMORY=${1#*=}; shift ;;
        --detach) DETACH=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --print-argv) PRINT_ARGV=1; shift ;;
        --no-tty-serial) NO_TTY_SERIAL=1; shift ;;
        --) shift; break ;;
        -h|--help) usage 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
done
# remaining "$@" are passed through to QEMU

# ---------------------------------------------------------------- profile
PROFILE_FILE=$VM_DIR/profiles/$PROFILE.conf
[ -f "$PROFILE_FILE" ] || die "no profile $PROFILE (looked in $VM_DIR/profiles)"
# shellcheck disable=SC1090
. "$PROFILE_FILE"
for v in VM_NAME VM_MACHINE VM_GIC VM_CPU_TCG VM_CPU_HW VM_SMP VM_MEMORY_MIB VM_DISK_DEVICE VM_DEVICES; do
    eval "[ -n \"\${$v:-}\" ]" || die "profile $PROFILE does not set $v"
done
VM_MACHINE_OPTS=${VM_MACHINE_OPTS:-}
VM_EXTRA_ARGS=${VM_EXTRA_ARGS:-}
STATE=$STATE_ROOT/$PROFILE

# ---------------------------------------------------------------- host tools
realpath_of() {
    readlink -f "$1" 2>/dev/null || printf '%s' "$1"
}

find_qemu() {
    QEMU=${MARYPI_VM_QEMU:-}
    [ -n "$QEMU" ] || QEMU=$(command -v qemu-system-aarch64 2>/dev/null || true)
    for d in /opt/homebrew/bin /usr/local/bin /usr/bin; do
        [ -n "$QEMU" ] && break
        [ -x "$d/qemu-system-aarch64" ] && QEMU=$d/qemu-system-aarch64
    done
    [ -n "$QEMU" ] || die "qemu-system-aarch64 not found (brew install qemu / apt install qemu-system-arm)"
}

file_size() {
    wc -c < "$1" | tr -d ' '
}

# Sets EFI_CODE, EFI_VARS_TEMPLATE (may be empty) and EFI_PAD (1 when the
# code image is not a 64 MiB pflash image and must be padded in state/).
find_firmware() {
    EFI_CODE=${MARYPI_VM_EFI_CODE:-}
    EFI_VARS_TEMPLATE=${MARYPI_VM_EFI_VARS:-}
    EFI_PAD=0
    if [ -z "$EFI_CODE" ]; then
        share=$(dirname "$(realpath_of "$QEMU")")/../share/qemu
        for dir in "$share" /opt/homebrew/share/qemu /usr/local/share/qemu /usr/share/qemu; do
            if [ -f "$dir/edk2-aarch64-code.fd" ]; then
                EFI_CODE=$(cd "$dir" && pwd)/edk2-aarch64-code.fd
                EFI_VARS_TEMPLATE=$(cd "$dir" && pwd)/edk2-arm-vars.fd
                break
            fi
        done
    fi
    if [ -z "$EFI_CODE" ] && [ -f /usr/share/AAVMF/AAVMF_CODE.fd ]; then
        EFI_CODE=/usr/share/AAVMF/AAVMF_CODE.fd
        EFI_VARS_TEMPLATE=/usr/share/AAVMF/AAVMF_VARS.fd
    fi
    if [ -z "$EFI_CODE" ] && [ -f /usr/share/edk2/aarch64/QEMU_EFI-pflash.raw ]; then
        EFI_CODE=/usr/share/edk2/aarch64/QEMU_EFI-pflash.raw
        EFI_VARS_TEMPLATE=/usr/share/edk2/aarch64/vars-template-pflash.raw
    fi
    if [ -z "$EFI_CODE" ]; then
        for f in /usr/share/qemu-efi-aarch64/QEMU_EFI.fd /usr/share/edk2/aarch64/QEMU_EFI.fd; do
            [ -f "$f" ] && { EFI_CODE=$f; break; }
        done
    fi
    [ -n "$EFI_CODE" ] || die "no EDK2 AArch64 firmware found (brew install qemu / apt install qemu-efi-aarch64); set MARYPI_VM_EFI_CODE"
    if [ "$DRY_RUN" = 0 ] && [ "$PRINT_ARGV" = 0 ]; then
        [ -f "$EFI_CODE" ] || die "firmware $EFI_CODE does not exist"
        [ "$(file_size "$EFI_CODE")" = "$PFLASH_SIZE" ] || EFI_PAD=1
    fi
    [ -n "$EFI_VARS_TEMPLATE" ] && [ ! -f "$EFI_VARS_TEMPLATE" ] && EFI_VARS_TEMPLATE=
    return 0
}

host_gic_v2() {
    [ -d /sys/firmware/devicetree/base ] || return 1
    for f in $(find /sys/firmware/devicetree/base -name compatible 2>/dev/null); do
        grep -qs -e 'arm,gic-400' -e 'arm,cortex-a15-gic' -e 'arm,gic-v2' "$f" && return 0
    done
    return 1
}

hv_support() {
    [ "$HOST_OS" = Darwin ] && [ "$HOST_ARCH" = arm64 ] && [ "$(sysctl -n kern.hv_support 2>/dev/null)" = 1 ]
}

kvm_usable() {
    [ "$HOST_OS" = Linux ] && [ "$HOST_ARCH" = aarch64 ] && [ -w /dev/kvm ]
}

# Sets ACCEL (resolved) and CPU. The same table lives in VMHost.resolveAccel.
choose_accel() {
    case $ACCEL in
        auto)
            if [ "$VM_GIC" = 3 ]; then
                if hv_support; then ACCEL=hvf
                elif kvm_usable; then ACCEL=kvm
                else ACCEL=tcg; fi
            elif kvm_usable && host_gic_v2; then
                ACCEL=kvm
            else
                ACCEL=tcg
            fi ;;
        hvf)
            [ "$VM_GIC" = 3 ] || die "QEMU cannot use hvf with a GICv2 guest (HVF does not support GICv2 emulation). Use --accel tcg or --profile qemu-virt-gicv3." ;;
        kvm)
            [ "$VM_GIC" = 3 ] || host_gic_v2 || warn "kvm with a GICv2 guest needs a host GIC with v2 compatibility; QEMU may refuse" ;;
        tcg) ;;
        *) die "unknown --accel $ACCEL (auto|hvf|kvm|tcg)" ;;
    esac
    case $ACCEL in
        tcg) CPU=$VM_CPU_TCG ;;
        *) CPU=$VM_CPU_HW ;;
    esac
}

# Sets BACKEND for the display mode; may downgrade window -> none.
choose_display() {
    case $DISPLAY_MODE in
        window)
            if [ "$HOST_OS" = Darwin ]; then
                BACKEND=cocoa
            elif [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
                warn "no DISPLAY or WAYLAND_DISPLAY; running without a window (serial.log only)"
                DISPLAY_MODE=none; BACKEND=none
            else
                help=$("$QEMU" -display help 2>/dev/null || true)
                case $help in
                    *gtk*) BACKEND=gtk ;;
                    *sdl*) BACKEND=sdl ;;
                    *) warn "this QEMU has no gtk/sdl display; running without a window"; DISPLAY_MODE=none; BACKEND=none ;;
                esac
            fi ;;
        serial|none) BACKEND=none ;;
        *) die "unknown --display $DISPLAY_MODE (window|serial|none)" ;;
    esac
}

pad_to_pflash() {
    [ "$(file_size "$1")" -ge "$PFLASH_SIZE" ] && return 0
    dd if=/dev/null of="$1" bs=1 seek="$PFLASH_SIZE" 2>/dev/null
}

qmp_socket_path() {
    QMP=$STATE/qmp.sock
    if [ "${#QMP}" -gt 100 ]; then
        QMP=${TMPDIR:-/tmp}/marypi-vm-$PROFILE.sock
        QMP_PATH_FILE=$STATE/qmp.path
    else
        QMP_PATH_FILE=
    fi
}

read_qmp_path() {
    if [ -f "$STATE/qmp.path" ]; then
        QMP=$(cat "$STATE/qmp.path")
    else
        QMP=$STATE/qmp.sock
    fi
}

pid_alive() {
    [ -n "${1:-}" ] && kill -0 "$1" 2>/dev/null
}

prepare_state() {
    mkdir -p "$STATE"
    if [ ! -f "$STATE/vars.fd" ]; then
        if [ -n "$EFI_VARS_TEMPLATE" ]; then
            cp "$EFI_VARS_TEMPLATE" "$STATE/vars.fd"
        else
            : > "$STATE/vars.fd"
        fi
    fi
    pad_to_pflash "$STATE/vars.fd"
    if [ "$EFI_PAD" = 1 ]; then
        [ -f "$STATE/code.fd" ] || cp "$EFI_CODE" "$STATE/code.fd"
        pad_to_pflash "$STATE/code.fd"
        EFI_CODE_USED=$STATE/code.fd
    else
        EFI_CODE_USED=$EFI_CODE
    fi
    if [ -f "$STATE/qemu.pid" ] && ! pid_alive "$(cat "$STATE/qemu.pid")"; then
        rm -f "$STATE/qemu.pid" "$STATE/qmp.sock"
    fi
    if [ -f "$STATE/qemu.pid" ]; then
        die "a VM for profile $PROFILE is already running (pid $(cat "$STATE/qemu.pid")); vm/run.sh stop --profile $PROFILE"
    fi
    [ -n "$QMP_PATH_FILE" ] && printf '%s\n' "$QMP" > "$QMP_PATH_FILE"
    printf '\n==== marypi vm run %s %s/%s image=%s ====\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$ACCEL" "$BACKEND" "$IMAGE" >> "$STATE/serial.log"
}

reverse_words() {
    r=
    for w in "$@"; do r="$w${r:+ $r}"; done
    printf '%s' "$r"
}

shell_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

# Builds the QEMU argv around the passthrough args "$@" and runs it. The
# argument list is assembled back to front (POSIX sh has no arrays), so
# the resulting order is: qemu, machine, cpu, memory, firmware, disk,
# devices, display, serial, qmp, pidfile, profile extras, passthrough.
build_and_run() {
    serial_log=$STATE/serial.log
    # shellcheck disable=SC2086
    set -- $VM_EXTRA_ARGS "$@"
    set -- -qmp "unix:$QMP,server=on,wait=off" -pidfile "$STATE/qemu.pid" "$@"
    # QEMU may only take over the terminal when it will be the foreground
    # process (we exec it below); a stdio chardev in a background process
    # group is stopped by SIGTTOU before the window ever appears.
    serial_tty=0
    if [ -t 0 ] && [ "$NO_TTY_SERIAL" = 0 ] && [ "$DETACH" = 0 ]; then
        serial_tty=1
    fi
    case $DISPLAY_MODE in
        window)
            if [ "$serial_tty" = 1 ]; then
                set -- -display "$BACKEND" -chardev "stdio,id=serial0,logfile=$serial_log,signal=off" -serial chardev:serial0 "$@"
            else
                set -- -display "$BACKEND" -chardev "file,id=serial0,path=$serial_log" -serial chardev:serial0 "$@"
            fi ;;
        serial)
            if [ "$serial_tty" = 1 ]; then
                set -- -display none -chardev "stdio,id=serial0,mux=on,logfile=$serial_log,signal=off" -serial chardev:serial0 -mon chardev=serial0,mode=readline "$@"
            else
                set -- -display none -monitor none -chardev "file,id=serial0,path=$serial_log" -serial chardev:serial0 "$@"
            fi ;;
        none)
            set -- -display none -monitor none -chardev "file,id=serial0,path=$serial_log" -serial chardev:serial0 "$@" ;;
    esac
    # shellcheck disable=SC2046
    for dev in $(reverse_words $VM_DEVICES); do
        set -- -device "$dev" "$@"
    done
    set -- "$QEMU" -name "ravynOS-$VM_NAME" \
        -M "$VM_MACHINE,gic-version=$VM_GIC${VM_MACHINE_OPTS:+,$VM_MACHINE_OPTS}" \
        -accel "$ACCEL" -cpu "$CPU" -smp "$VM_SMP" -m "${MEMORY:-$VM_MEMORY_MIB}" \
        -drive "if=pflash,format=raw,readonly=on,file=$EFI_CODE_USED" \
        -drive "if=pflash,format=raw,file=$STATE/vars.fd" \
        -drive "file=$IMAGE,format=raw,if=none,id=hd0" \
        -device "$VM_DISK_DEVICE,drive=hd0" \
        "$@"

    if [ "$PRINT_ARGV" = 1 ]; then
        printf '%s\n' "$@"
        return 0
    fi
    if [ "$DRY_RUN" = 1 ]; then
        for a in "$@"; do printf '%s ' "$(shell_quote "$a")"; done
        printf '\n'
        return 0
    fi
    printf 'vm: %s (%s, -cpu %s, display %s)\n' "$QEMU" "$ACCEL" "$CPU" "$BACKEND" >&2
    printf 'vm: serial log: %s\n' "$serial_log" >&2
    if [ "$DETACH" = 1 ]; then
        nohup "$@" > "$STATE/qemu.out" 2>&1 &
        printf 'vm: started in the background (pid %s); vm/run.sh status|serial|stop --profile %s\n' "$!" "$PROFILE" >&2
        return 0
    fi
    if [ "$serial_tty" = 1 ] && [ "$DISPLAY_MODE" = serial ]; then
        printf 'vm: serial console is this terminal; Ctrl-A X quits\n' >&2
    elif [ "$serial_tty" = 0 ]; then
        printf 'vm: no terminal for the serial console; follow it with: vm/run.sh serial --profile %s\n' "$PROFILE" >&2
    fi
    exec "$@"
}

cmd_run() {
    find_qemu
    find_firmware
    choose_accel
    choose_display
    qmp_socket_path
    if [ -z "$IMAGE" ]; then
        IMAGE=$STATE/disk.img
        [ -f "$IMAGE" ] || die "no --image given and $IMAGE does not exist; build one with 'marypi build-image --qemu --out $IMAGE'"
    fi
    case $IMAGE in /*) ;; *) IMAGE=$(pwd)/$IMAGE ;; esac
    if [ "$DRY_RUN" = 0 ] && [ "$PRINT_ARGV" = 0 ]; then
        [ -f "$IMAGE" ] || die "image $IMAGE does not exist"
        prepare_state
    else
        EFI_CODE_USED=$EFI_CODE
        [ "$EFI_PAD" = 1 ] && EFI_CODE_USED=$STATE/code.fd
    fi
    build_and_run "$@"
}

qmp_quit() {
    [ -S "$QMP" ] || return 1
    if command -v python3 > /dev/null 2>&1; then
        python3 - "$QMP" <<'PY' 2>/dev/null
import json, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(sys.argv[1])
f = s.makefile("rw")
f.readline()
for cmd in ("qmp_capabilities", "quit"):
    f.write(json.dumps({"execute": cmd}) + "\n"); f.flush(); f.readline()
PY
        return $?
    fi
    if command -v nc > /dev/null 2>&1; then
        printf '{"execute":"qmp_capabilities"}\n{"execute":"quit"}\n' | nc -U "$QMP" > /dev/null 2>&1
        return $?
    fi
    return 1
}

cmd_stop() {
    read_qmp_path
    pid=$(cat "$STATE/qemu.pid" 2>/dev/null || true)
    if ! pid_alive "$pid"; then
        rm -f "$STATE/qemu.pid" "$STATE/qmp.sock"
        printf 'vm: no VM running for profile %s\n' "$PROFILE" >&2
        return 0
    fi
    qmp_quit || kill -TERM "$pid" 2>/dev/null || true
    i=0
    while pid_alive "$pid" && [ $i -lt 80 ]; do sleep 0.1; i=$((i + 1)); done
    if pid_alive "$pid"; then
        kill -TERM "$pid" 2>/dev/null || true
        i=0
        while pid_alive "$pid" && [ $i -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
    fi
    pid_alive "$pid" && kill -KILL "$pid" 2>/dev/null
    rm -f "$STATE/qemu.pid" "$STATE/qmp.sock"
    printf 'vm: stopped pid %s\n' "$pid" >&2
}

cmd_status() {
    read_qmp_path
    pid=$(cat "$STATE/qemu.pid" 2>/dev/null || true)
    printf 'profile:  %s\nstate:    %s\n' "$PROFILE" "$STATE"
    if pid_alive "$pid"; then
        printf 'status:   running (pid %s)\n' "$pid"
    else
        printf 'status:   not running\n'
    fi
    [ -S "$QMP" ] && printf 'qmp:      %s\n' "$QMP"
    if [ -f "$STATE/serial.log" ]; then
        printf 'serial:   %s (%s bytes)\n' "$STATE/serial.log" "$(file_size "$STATE/serial.log")"
        tail -n 3 "$STATE/serial.log" | tr -d '\r' | sed 's/^/          | /'
    fi
}

cmd_serial() {
    [ -f "$STATE/serial.log" ] || die "no serial log yet at $STATE/serial.log"
    exec tail -n +1 -f "$STATE/serial.log"
}

cmd_doctor() {
    find_qemu
    printf 'qemu:      %s\n' "$QEMU"
    "$QEMU" --version 2>/dev/null | head -1 | sed 's/^/           /'
    find_firmware
    printf 'firmware:  %s%s\n' "$EFI_CODE" "$([ "$EFI_PAD" = 1 ] && printf ' (padded to 64 MiB in state/)')"
    printf 'vars:      %s\n' "${EFI_VARS_TEMPLATE:-<none, starts empty>}"
    printf 'profile:   %s (GICv%s, %s)\n' "$PROFILE" "$VM_GIC" "$PROFILE_FILE"
    choose_accel
    printf 'accel:     %s (-cpu %s)' "$ACCEL" "$CPU"
    hv_support && printf '; hvf available'
    kvm_usable && printf '; kvm available'
    printf '\n'
    choose_display
    printf 'display:   %s -> %s\n' "$DISPLAY_MODE" "$BACKEND"
    printf 'state:     %s\n' "$STATE"
    printf 'profiles:  %s\n' "$(ls "$VM_DIR/profiles" | sed 's/\.conf$//' | tr '\n' ' ')"
}

case $CMD in
    run) cmd_run "$@" ;;
    stop) cmd_stop ;;
    status) cmd_status ;;
    serial) cmd_serial ;;
    doctor) cmd_doctor ;;
    -h|--help|help) usage 0 ;;
    *) die "unknown command: $CMD (run|stop|status|serial|doctor)" ;;
esac
