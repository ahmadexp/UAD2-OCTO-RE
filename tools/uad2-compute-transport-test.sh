#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

BDF=${UAD2_BDF:-0000:03:00.0}
MODULE_PATH=${UAD2_COMPUTE_MODULE:-./uad2_compute.ko}
CONTROL_PATH=${UAD2_COMPUTE_CTL:-./uad2ctl}
DEVICE_PATH=/sys/bus/pci/devices/$BDF
LOADED=0
STARTED=0

fail()
{
	echo "uad2-compute-transport-test: $*" >&2
	exit 1
}

read_hex()
{
	tr 'A-F' 'a-f' < "$1"
}

cleanup()
{
	status=$?
	trap - EXIT HUP INT TERM
	if [ "$STARTED" -eq 1 ] && [ -e /dev/uad2_compute0 ]; then
		"$CONTROL_PATH" stop || status=1
	fi
	if [ "$LOADED" -eq 1 ]; then
		rmmod uad2_compute || status=1
	fi
	exit "$status"
}
trap cleanup EXIT HUP INT TERM

[ "$(id -u)" -eq 0 ] || fail "run as root"
[ -d "$DEVICE_PATH" ] || fail "$BDF is absent"
[ -f "$MODULE_PATH" ] || fail "module not found: $MODULE_PATH"
[ -x "$CONTROL_PATH" ] || fail "control tool not executable: $CONTROL_PATH"
[ "$(read_hex "$DEVICE_PATH/vendor")" = "0x1a00" ] || fail "vendor mismatch"
[ "$(read_hex "$DEVICE_PATH/device")" = "0x0002" ] || fail "device mismatch"
[ "$(read_hex "$DEVICE_PATH/subsystem_vendor")" = "0x1a00" ] ||
	fail "subsystem vendor mismatch"
[ "$(read_hex "$DEVICE_PATH/subsystem_device")" = "0x0005" ] ||
	fail "subsystem device mismatch"
[ ! -L "$DEVICE_PATH/driver" ] || fail "endpoint is already bound"

GROUP=$(readlink -f "$DEVICE_PATH/iommu_group")
[ -n "$GROUP" ] || fail "endpoint has no IOMMU group"
set -- "$GROUP"/devices/*
[ "$#" -eq 1 ] && [ "$(basename "$1")" = "$BDF" ] ||
	fail "IOMMU group is not exclusive"

insmod "$MODULE_PATH"
LOADED=1
[ -e /dev/uad2_compute0 ] || fail "driver did not create the device node"

echo "phase=bound"
"$CONTROL_PATH" info
"$CONTROL_PATH" status

"$CONTROL_PATH" start
STARTED=1
echo "phase=started"
"$CONTROL_PATH" info
"$CONTROL_PATH" status

dsp=0
while [ "$dsp" -lt 8 ]; do
	"$CONTROL_PATH" reset "$dsp"
	dsp=$((dsp + 1))
done
echo "phase=all_resets_complete"
"$CONTROL_PATH" status

"$CONTROL_PATH" stop
STARTED=0
echo "phase=stopped"
"$CONTROL_PATH" info
"$CONTROL_PATH" status

rmmod uad2_compute
LOADED=0
[ ! -L "$DEVICE_PATH/driver" ] || fail "endpoint remained bound"
echo "phase=unbound"
