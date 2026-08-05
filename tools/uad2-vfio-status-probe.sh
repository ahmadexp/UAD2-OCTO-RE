#!/bin/sh
set -eu

BDF="0000:03:00.0"
DEVICE="/sys/bus/pci/devices/$BDF"
DRIVER="/sys/bus/pci/drivers/vfio-pci"
PROBE="/tmp/uad2-vfio-read-status"
MODE="${1-}"

if [ "$#" -gt 1 ]; then
    echo "refusing: expected at most one probe mode" >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "refusing: this wrapper must run as root" >&2
    exit 1
fi

if [ ! -d "$DEVICE" ] || [ -L "$DEVICE/driver" ]; then
    echo "refusing: endpoint is absent or already bound" >&2
    exit 1
fi

if [ "$(cat "$DEVICE/vendor")" != "0x1a00" ] || \
   [ "$(cat "$DEVICE/device")" != "0x0002" ] || \
   [ "$(cat "$DEVICE/subsystem_vendor")" != "0x1a00" ] || \
   [ "$(cat "$DEVICE/subsystem_device")" != "0x0005" ]; then
    echo "refusing: endpoint identity does not match the captured OCTO" >&2
    exit 1
fi

GROUP="$(basename "$(readlink -f "$DEVICE/iommu_group")")"
if [ "$GROUP" != "16" ]; then
    echo "refusing: expected IOMMU group 16, found $GROUP" >&2
    exit 1
fi

set -- "/sys/kernel/iommu_groups/$GROUP/devices/"*
if [ "$#" -ne 1 ] || [ "$(basename "$1")" != "$BDF" ]; then
    echo "refusing: IOMMU group is not isolated to the OCTO" >&2
    exit 1
fi

if [ ! -x "$PROBE" ]; then
    echo "refusing: VFIO probe is missing" >&2
    exit 1
fi

cleanup() {
    if [ -L "$DEVICE/driver" ] && \
       [ "$(basename "$(readlink -f "$DEVICE/driver")")" = "vfio-pci" ]; then
        echo "$BDF" > "$DRIVER/unbind" || true
    fi
    printf '\n' > "$DEVICE/driver_override" || true
}
trap cleanup EXIT HUP INT TERM

modprobe vfio-pci
printf '%s' "vfio-pci" > "$DEVICE/driver_override"
echo "$BDF" > /sys/bus/pci/drivers_probe

if [ ! -L "$DEVICE/driver" ] || \
   [ "$(basename "$(readlink -f "$DEVICE/driver")")" != "vfio-pci" ]; then
    echo "refusing: vfio-pci did not bind" >&2
    exit 1
fi

if [ -n "$MODE" ]; then
    "$PROBE" "$MODE"
else
    "$PROBE"
fi
