#!/bin/sh
set -eu

BDF="0000:03:00.0"
DEVICE="/sys/bus/pci/devices/$BDF"
DRIVER="/sys/bus/pci/drivers/uio_pci_generic"
READER="/tmp/uad2-uio-mmio-read.py"
PROFILE="${1:-identity}"

case "$PROFILE" in
    identity|dsp-status|ring-registers|dma-control) ;;
    *)
        echo "refusing: unsupported profile $PROFILE" >&2
        exit 1
        ;;
esac

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

if [ ! -f "$READER" ]; then
    echo "refusing: reader is missing" >&2
    exit 1
fi

cleanup() {
    if [ -L "$DEVICE/driver" ] && \
       [ "$(basename "$(readlink -f "$DEVICE/driver")")" = "uio_pci_generic" ]; then
        echo "$BDF" > "$DRIVER/unbind" || true
    fi
    printf '\n' > "$DEVICE/driver_override" || true
}
trap cleanup EXIT HUP INT TERM

modprobe uio_pci_generic
printf '%s' "uio_pci_generic" > "$DEVICE/driver_override"
echo "$BDF" > /sys/bus/pci/drivers_probe

if [ ! -L "$DEVICE/driver" ] || \
   [ "$(basename "$(readlink -f "$DEVICE/driver")")" != "uio_pci_generic" ]; then
    echo "refusing: uio_pci_generic did not bind" >&2
    exit 1
fi

if [ "$PROFILE" = "dsp-status" ]; then
    python3 "$READER" \
        --bdf "$BDF" \
        --profile dsp-status \
        --samples 2 \
        --interval-ms 250 \
        --acknowledge-read-side-effects
elif [ "$PROFILE" = "ring-registers" ]; then
    python3 "$READER" \
        --bdf "$BDF" \
        --profile ring-registers \
        --samples 1 \
        --only-nonzero \
        --acknowledge-read-side-effects
elif [ "$PROFILE" = "dma-control" ]; then
    python3 "$READER" \
        --bdf "$BDF" \
        --profile dma-control \
        --samples 2 \
        --interval-ms 250 \
        --acknowledge-read-side-effects
else
    python3 "$READER" \
        --bdf "$BDF" \
        --profile identity \
        --samples 1 \
        --acknowledge-read-side-effects
fi
