#!/bin/sh
set -eu

BDF="0000:03:00.0"
DEVICE="/sys/bus/pci/devices/$BDF"
DRIVER="/sys/bus/pci/drivers/vfio-pci"
PROBE="/tmp/uad2-vfio-full-query"

MODE="${1-}"
PAYLOAD="${2-}"
MUTATION_OFFSET="${3-}"
if [ "$#" -gt 3 ] || \
   { [ "$MODE" = "--post-official-bill" ] && [ "$#" -ne 2 ]; } || \
   { [ "$MODE" = "--post-official-bill-flip" ] && [ "$#" -ne 3 ]; } || \
   { [ "$MODE" = "--post-official-bill-dsp" ] && [ "$#" -ne 3 ]; } || \
   { [ "$MODE" != "--post-official-bill" ] && \
     [ "$MODE" != "--post-official-bill-flip" ] && \
     [ "$MODE" != "--post-official-bill-dsp" ] && [ "$#" -gt 1 ]; } || \
   { [ -n "$MODE" ] && [ "$MODE" != "--connect" ] && \
   [ "$MODE" != "--connect-query-027" ] && \
   [ "$MODE" != "--post-official" ] && \
   [ "$MODE" != "--post-official-bill" ] && \
   [ "$MODE" != "--post-official-bill-flip" ] && \
   [ "$MODE" != "--post-official-bill-dsp" ]; } || \
   [ "$(id -u)" -ne 0 ]; then
	echo "refusing: invalid full-query mode, arguments, or privileges" >&2
	exit 1
fi
if [ "$MODE" = "--post-official-bill" ] || \
   [ "$MODE" = "--post-official-bill-flip" ] || \
   [ "$MODE" = "--post-official-bill-dsp" ]; then
	EXPECTED_SHA256="0c353512fb27ed961b4e0746de7f1bbc462447f6e2c0263bc6209cda7b7718d0"
	if [ ! -f "$PAYLOAD" ] || \
	   [ "$(sha256sum "$PAYLOAD" | awk '{print $1}')" != "$EXPECTED_SHA256" ]; then
		echo "refusing: Bill command target is not the exact captured object" >&2
		exit 1
	fi
fi
if [ "$MODE" = "--post-official-bill-dsp" ]; then
	case "$MUTATION_OFFSET" in
		0|1|2|3|4|5|6|7) ;;
		*) echo "refusing: target DSP must be 0 through 7" >&2; exit 1 ;;
	esac
fi
if [ "$MODE" = "--post-official-bill-flip" ]; then
	case "$MUTATION_OFFSET" in
		28|75|76|123|124|459) ;;
		*) echo "refusing: mutation must be an approved body-boundary offset" >&2; exit 1 ;;
	esac
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
COMMAND="$(od -An -tu2 -j4 -N2 "$DEVICE/config")"
if [ $((COMMAND & 4)) -ne 0 ]; then
    echo "refusing: bus mastering was enabled before VFIO bind" >&2
    exit 1
fi
GROUP="$(basename "$(readlink -f "$DEVICE/iommu_group")")"
set -- "/sys/kernel/iommu_groups/$GROUP/devices/"*
if [ "$GROUP" != "16" ] || [ "$#" -ne 1 ] || \
   [ "$(basename "$1")" != "$BDF" ]; then
    echo "refusing: OCTO is not isolated in expected IOMMU group 16" >&2
    exit 1
fi
if [ ! -x "$PROBE" ]; then
    echo "refusing: Experiment 014 binary is missing" >&2
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
if [ "$MODE" = "--post-official-bill" ]; then
	"$PROBE" "$MODE" "$PAYLOAD"
elif [ "$MODE" = "--post-official-bill-flip" ]; then
	"$PROBE" "$MODE" "$PAYLOAD" "$MUTATION_OFFSET"
elif [ "$MODE" = "--post-official-bill-dsp" ]; then
	"$PROBE" "$MODE" "$PAYLOAD" "$MUTATION_OFFSET"
elif [ -n "$MODE" ]; then
    "$PROBE" "$MODE"
else
    "$PROBE"
fi
