#!/bin/sh
set -eu

BDF="${UAD2_BDF:-0000:03:00.0}"
EXPECTED_GROUP="${UAD2_IOMMU_GROUP:-16}"
EXPECTED_WINDOWS_SHA256="a61adeab895ef5a4db436e0a7011c92a2ff17bb0357f58b13bbc4062e535e7b9"
EXPECTED_ACK="YES_CAPTURE_OFFICIAL_DRIVER_LIFECYCLE"
CODE_FD="/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"

usage() {
    echo "usage: $0 WINDOWS_ISO TOOLS_ISO SYSTEM_QCOW2 OUTPUT_FAT OVMF_VARS TRACE_EVENTS" >&2
    exit 2
}

[ "$#" -eq 6 ] || usage
[ "$(id -u)" -eq 0 ] || {
    echo "refusing: root is required for VFIO binding" >&2
    exit 1
}
[ "${UAD2_ALLOW_OFFICIAL_DRIVER_CAPTURE:-}" = "$EXPECTED_ACK" ] || {
    echo "refusing: set UAD2_ALLOW_OFFICIAL_DRIVER_CAPTURE=$EXPECTED_ACK after reviewing the lifecycle risk" >&2
    exit 1
}

WINDOWS_ISO="$1"
TOOLS_ISO="$2"
SYSTEM_DISK="$3"
OUTPUT_DISK="$4"
VARS_FD="$5"
TRACE_EVENTS="$6"
DEVICE="/sys/bus/pci/devices/$BDF"
VFIO_DRIVER="/sys/bus/pci/drivers/vfio-pci"
STATE_DIR="$(dirname "$SYSTEM_DISK")/swtpm-state"
TPM_SOCKET="$(dirname "$SYSTEM_DISK")/swtpm.sock"
TRACE_FILE="$(dirname "$SYSTEM_DISK")/qemu-vfio.trace"
TPM_LOG="$(dirname "$SYSTEM_DISK")/swtpm.log"
SERIAL_LOG="$(dirname "$SYSTEM_DISK")/guest-serial.log"

for path in "$WINDOWS_ISO" "$TOOLS_ISO" "$SYSTEM_DISK" "$OUTPUT_DISK" "$VARS_FD" "$TRACE_EVENTS" "$CODE_FD"; do
    if [ ! -f "$path" ] || [ -L "$path" ]; then
        echo "refusing: required regular non-symlink file is absent: $path" >&2
        exit 1
    fi
done

actual_windows_sha256="$(sha256sum "$WINDOWS_ISO" | awk '{print $1}')"
if [ "$actual_windows_sha256" != "$EXPECTED_WINDOWS_SHA256" ]; then
    echo "refusing: Microsoft ISO SHA-256 mismatch" >&2
    echo "expected $EXPECTED_WINDOWS_SHA256" >&2
    echo "observed $actual_windows_sha256" >&2
    exit 1
fi

if [ ! -d "$DEVICE" ]; then
    echo "refusing: endpoint $BDF is absent" >&2
    exit 1
fi
if [ "$(cat "$DEVICE/vendor")" != "0x1a00" ] ||
   [ "$(cat "$DEVICE/device")" != "0x0002" ] ||
   [ "$(cat "$DEVICE/subsystem_vendor")" != "0x1a00" ] ||
   [ "$(cat "$DEVICE/subsystem_device")" != "0x0005" ]; then
    echo "refusing: endpoint identity is not the validated OCTO profile" >&2
    exit 1
fi
if [ -L "$DEVICE/driver" ]; then
    echo "refusing: endpoint is already bound to a host driver" >&2
    exit 1
fi
command_word="$(od -An -tu2 -j4 -N2 "$DEVICE/config")"
if [ $((command_word & 5)) -ne 0 ]; then
    echo "refusing: PCI I/O or bus mastering was enabled before VFIO bind" >&2
    exit 1
fi
group="$(basename "$(readlink -f "$DEVICE/iommu_group")")"
set -- "/sys/kernel/iommu_groups/$group/devices/"*
if [ "$group" != "$EXPECTED_GROUP" ] || [ "$#" -ne 1 ] || [ "$(basename "$1")" != "$BDF" ]; then
    echo "refusing: endpoint is not alone in expected IOMMU group $EXPECTED_GROUP" >&2
    exit 1
fi

mkdir -p "$STATE_DIR"
rm -f "$TPM_SOCKET"
: > "$TRACE_FILE"
: > "$TPM_LOG"
: > "$SERIAL_LOG"

cleanup() {
    if [ -n "${SWTPM_PID:-}" ]; then
        kill "$SWTPM_PID" 2>/dev/null || true
        wait "$SWTPM_PID" 2>/dev/null || true
    fi
    if [ -L "$DEVICE/driver" ] &&
       [ "$(basename "$(readlink -f "$DEVICE/driver")")" = "vfio-pci" ]; then
        echo "$BDF" > "$VFIO_DRIVER/unbind" || true
    fi
    printf '\n' > "$DEVICE/driver_override" || true
}
trap cleanup EXIT HUP INT TERM

modprobe vfio-pci
printf '%s' vfio-pci > "$DEVICE/driver_override"
echo "$BDF" > /sys/bus/pci/drivers_probe
if [ ! -L "$DEVICE/driver" ] ||
   [ "$(basename "$(readlink -f "$DEVICE/driver")")" != "vfio-pci" ]; then
    echo "refusing: vfio-pci did not bind" >&2
    exit 1
fi

swtpm socket \
    --tpm2 \
    --tpmstate "dir=$STATE_DIR" \
    --ctrl "type=unixio,path=$TPM_SOCKET" \
    --log "file=$TPM_LOG,level=20" \
    --terminate &
SWTPM_PID=$!

attempt=0
while [ "$attempt" -lt 5 ]; do
    [ -S "$TPM_SOCKET" ] && break
    sleep 1
    attempt=$((attempt + 1))
done
[ -S "$TPM_SOCKET" ] || {
    echo "refusing: swtpm control socket did not appear" >&2
    exit 1
}

echo "Windows ISO SHA-256: $actual_windows_sha256"
echo "Tools ISO SHA-256: $(sha256sum "$TOOLS_ISO" | awk '{print $1}')"
echo "Output FAT SHA-256 before capture: $(sha256sum "$OUTPUT_DISK" | awk '{print $1}')"
echo "Starting isolated reference-driver guest; HMP monitor is 127.0.0.1:4444"

qemu-system-x86_64 \
    -name uad2-official-reference \
    -enable-kvm \
    -machine q35,smm=on \
    -cpu host \
    -smp 8,sockets=1,cores=8,threads=1 \
    -m 8192 \
    -global driver=cfi.pflash01,property=secure,value=on \
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$CODE_FD" \
    -drive "if=pflash,format=raw,unit=1,file=$VARS_FD" \
    -chardev "socket,id=chrtpm,path=$TPM_SOCKET" \
    -tpmdev emulator,id=tpm0,chardev=chrtpm \
    -device tpm-tis,tpmdev=tpm0 \
    -drive "if=none,id=osdisk,format=qcow2,file=$SYSTEM_DISK" \
    -device ide-hd,drive=osdisk,bus=ide.0 \
    -drive "if=none,id=windowscd,format=raw,readonly=on,file=$WINDOWS_ISO" \
    -device ide-cd,drive=windowscd,bus=ide.1 \
    -drive "if=none,id=toolscd,format=raw,readonly=on,file=$TOOLS_ISO" \
    -device ide-cd,drive=toolscd,bus=ide.2 \
    -drive "if=none,id=outputdisk,format=raw,file=$OUTPUT_DISK,cache=none" \
    -device ide-hd,drive=outputdisk,bus=ide.3 \
    -device pcie-root-port,id=uad2port,chassis=1,slot=1 \
    -device "vfio-pci,host=$BDF,bus=uad2port,x-no-mmap=on,x-no-kvm-intx=on,x-no-kvm-msi=on,x-no-kvm-msix=on" \
    -netdev user,id=net0,restrict=on \
    -device e1000e,netdev=net0 \
    -device qemu-xhci \
    -device usb-tablet \
    -vga std \
    -display none \
    -vnc 127.0.0.1:1 \
    -serial "file:$SERIAL_LOG" \
    -monitor tcp:127.0.0.1:4444,server=on,wait=off \
    -boot once=d,menu=on \
    -trace "events=$TRACE_EVENTS,file=$TRACE_FILE"

echo "Output FAT SHA-256 after capture: $(sha256sum "$OUTPUT_DISK" | awk '{print $1}')"
