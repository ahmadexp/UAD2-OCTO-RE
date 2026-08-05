#!/bin/sh
set -eu

BDF="0000:03:00.0"
DEVICE="/sys/bus/pci/devices/$BDF"
PROBE="/tmp/uad2-mmio-read.py"

if [ "$(id -u)" -ne 0 ]; then
    echo "refusing: this wrapper must run as root" >&2
    exit 1
fi

if [ ! -d "$DEVICE" ]; then
    echo "refusing: PCI endpoint $BDF is absent" >&2
    exit 1
fi

if [ -L "$DEVICE/driver" ]; then
    echo "refusing: a kernel driver is bound" >&2
    exit 1
fi

if [ "$(cat "$DEVICE/vendor")" != "0x1a00" ] || \
   [ "$(cat "$DEVICE/device")" != "0x0002" ] || \
   [ "$(cat "$DEVICE/subsystem_device")" != "0x0005" ]; then
    echo "refusing: endpoint identity does not match the captured OCTO" >&2
    exit 1
fi

if [ "$(cat "$DEVICE/enable")" != "0" ]; then
    echo "refusing: endpoint was already enabled, state is not the baseline" >&2
    exit 1
fi

if [ ! -f "$PROBE" ]; then
    echo "refusing: $PROBE is missing" >&2
    exit 1
fi

cleanup() {
    echo 0 > "$DEVICE/enable" || true
}
trap cleanup EXIT HUP INT TERM

echo 1 > "$DEVICE/enable"

# pci_enable_device() must enable memory decoding but not bus mastering. The
# Python probe independently verifies both command bits before mapping BAR0.
python3 "$PROBE" \
    --bdf "$BDF" \
    --profile identity \
    --acknowledge-read-side-effects
