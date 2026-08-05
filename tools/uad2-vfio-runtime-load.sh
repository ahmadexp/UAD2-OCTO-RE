#!/bin/sh
set -eu

BDF="0000:03:00.0"
DEVICE="/sys/bus/pci/devices/$BDF"
DRIVER="/sys/bus/pci/drivers/vfio-pci"
LOADER="/tmp/uad2-vfio-runtime-load"
EXPECTED_SHA256="f503787c0f253fc9713a47ae7e15adff242a6dde647dae7cb8ab6550ed976447"

if [ "$#" -ne 1 ] || [ "$(id -u)" -ne 0 ]; then
    echo "refusing: exact HBUT path and root are required" >&2
    exit 1
fi
if [ "${UAD2_ALLOW_PERSISTENT_FIRMWARE_EXPERIMENT:-}" != \
     "YES_I_ACCEPT_CARD_FIRMWARE_RISK" ]; then
    echo "refusing: HBUT may update persistent card firmware" >&2
    echo "set UAD2_ALLOW_PERSISTENT_FIRMWARE_EXPERIMENT=YES_I_ACCEPT_CARD_FIRMWARE_RISK only after informed review" >&2
    exit 1
fi
FIRMWARE="$1"
if [ ! -f "$FIRMWARE" ] || [ -L "$FIRMWARE" ]; then
    echo "refusing: firmware must be a regular non-symlink file" >&2
    exit 1
fi
ACTUAL_SHA256="$(sha256sum "$FIRMWARE" | awk '{print $1}')"
if [ "$ACTUAL_SHA256" != "$EXPECTED_SHA256" ]; then
    echo "refusing: firmware SHA-256 does not match the exact OCTO artifact" >&2
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
    echo "refusing: endpoint identity does not match the OCTO" >&2
    exit 1
fi

COMMAND="$(od -An -tu2 -j4 -N2 "$DEVICE/config")"
if [ $((COMMAND & 4)) -ne 0 ]; then
    echo "refusing: bus mastering was enabled before VFIO bind" >&2
    exit 1
fi
GROUP="$(basename "$(readlink -f "$DEVICE/iommu_group")")"
set -- "/sys/kernel/iommu_groups/$GROUP/devices/"*
if [ "$GROUP" != "16" ] || [ "$#" -ne 1 ] || [ "$(basename "$1")" != "$BDF" ]; then
    echo "refusing: OCTO is not isolated in expected IOMMU group 16" >&2
    exit 1
fi
if [ ! -x "$LOADER" ]; then
    echo "refusing: runtime loader binary is missing" >&2
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

"$LOADER" "$FIRMWARE"
