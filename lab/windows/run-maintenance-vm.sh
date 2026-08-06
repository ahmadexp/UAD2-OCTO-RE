#!/bin/sh
set -eu

CODE_FD="/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"

usage() {
    echo "usage: $0 SYSTEM_QCOW2 TOOLS_ISO OVMF_VARS" >&2
    exit 2
}

[ "$#" -eq 3 ] || usage
SYSTEM_DISK="$1"
TOOLS_ISO="$2"
VARS_FD="$3"
STATE_DIR="$(dirname "$SYSTEM_DISK")/swtpm-state"
TPM_SOCKET="$(dirname "$SYSTEM_DISK")/swtpm-maintenance.sock"

for path in "$SYSTEM_DISK" "$TOOLS_ISO" "$VARS_FD" "$CODE_FD"; do
    if [ ! -f "$path" ] || [ -L "$path" ]; then
        echo "refusing: required regular non-symlink file is absent: $path" >&2
        exit 1
    fi
done

mkdir -p "$STATE_DIR"
rm -f "$TPM_SOCKET"

cleanup() {
    if [ -n "${SWTPM_PID:-}" ]; then
        kill "$SWTPM_PID" 2>/dev/null || true
        wait "$SWTPM_PID" 2>/dev/null || true
    fi
    rm -f "$TPM_SOCKET"
}
trap cleanup EXIT HUP INT TERM

swtpm socket \
    --tpm2 \
    --tpmstate "dir=$STATE_DIR" \
    --ctrl "type=unixio,path=$TPM_SOCKET" \
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

echo "Starting card-free maintenance guest; HMP monitor is 127.0.0.1:4444"
echo "Tools ISO SHA-256: $(sha256sum "$TOOLS_ISO" | awk '{print $1}')"

qemu-system-x86_64 \
    -name uad2-card-free-maintenance \
    -enable-kvm \
    -machine q35,smm=on \
    -cpu host \
    -smp 4,sockets=1,cores=4,threads=1 \
    -m 4096 \
    -global driver=cfi.pflash01,property=secure,value=on \
    -drive "if=pflash,format=raw,unit=0,readonly=on,file=$CODE_FD" \
    -drive "if=pflash,format=raw,unit=1,file=$VARS_FD" \
    -chardev "socket,id=chrtpm,path=$TPM_SOCKET" \
    -tpmdev emulator,id=tpm0,chardev=chrtpm \
    -device tpm-tis,tpmdev=tpm0 \
    -drive "if=none,id=osdisk,format=qcow2,file=$SYSTEM_DISK" \
    -device ide-hd,drive=osdisk,bus=ide.0 \
    -drive "if=none,id=toolscd,format=raw,readonly=on,file=$TOOLS_ISO" \
    -device ide-cd,drive=toolscd,bus=ide.1 \
    -device qemu-xhci \
    -device usb-tablet \
    -vga std \
    -display none \
    -vnc 127.0.0.1:1 \
    -monitor tcp:127.0.0.1:4444,server=on,wait=off \
    -boot c
