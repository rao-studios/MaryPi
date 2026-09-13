#!/bin/sh
# Mary's dev loop in the VM, the way the desktop launcher has one for the compositor:
# with maryos.ui=dev on the kernel command line (ui.sh --dev), sewnd, threadd and
# maryd run from the tree `make mary` writes to the host share (/mnt/maryos-out/mary)
# and restart whenever that binary changes, so a C edit shows up without rebuilding
# the image. Without dev mode each is its /usr/bin copy. VM images only: the Pi never
# gets this. Runs inside the chroot at the vm target stage.
set -eu

mkdir -p /usr/lib/maryos
cat > /usr/lib/maryos/mary-dev-run <<'RUNNER'
#!/bin/sh
# mary-dev-run NAME [ARGS]: NAME from /mnt/maryos-out/mary in dev mode, restarted when
# it changes; otherwise /usr/bin/NAME (hooks/vm/20-mary-dev.sh).
set -u
name=$1
shift
DEV=/mnt/maryos-out/mary
dev=0
for arg in $(cat /proc/cmdline); do [ "$arg" = maryos.ui=dev ] && dev=1; done
if [ "$dev" != 1 ] || [ ! -x "$DEV/usr/bin/$name" ]; then
    exec "/usr/bin/$name" "$@"
fi
mtime() { stat -c %Y "$1" 2>/dev/null || echo 0; }
while :; do
    bin=$DEV/usr/bin/$name
    stamp=$(mtime "$bin")
    echo "mary-dev-run: starting $bin"
    "$bin" "$@" &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        sleep 1
        if [ "$(mtime "$bin")" != "$stamp" ]; then
            sleep 1
            echo "mary-dev-run: $name changed; restarting"
            kill -TERM "$pid" 2>/dev/null
            break
        fi
    done
    wait "$pid" 2>/dev/null || sleep 2
done
RUNNER
chmod 755 /usr/lib/maryos/mary-dev-run

for unit in sewnd threadd; do
    mkdir -p "/etc/systemd/system/$unit.service.d"
    printf '[Service]\nExecStart=\nExecStart=/usr/lib/maryos/mary-dev-run %s\n' "$unit" > "/etc/systemd/system/$unit.service.d/10-dev.conf"
done
mkdir -p /etc/systemd/user/maryd.service.d
printf '[Service]\nExecStart=\nExecStart=/usr/lib/maryos/mary-dev-run maryd\n' > /etc/systemd/user/maryd.service.d/10-dev.conf
