#!/bin/sh
set -eu

BDF="0000:03:00.0"
DEVICE="/sys/bus/pci/devices/$BDF"
DRIVER="/sys/bus/pci/drivers/vfio-pci"
PROBE="/tmp/uad2-vfio-realverb-sequence"

MODE=""
TARGET_DSP=""
if [ "$#" -eq 1 ] && [ "$1" = "--cleanup" ]; then
	MODE="$1"
	shift
elif [ "$#" -eq 2 ] &&
     { [ "$1" = "--allocation" ] || [ "$1" = "--process" ]; }; then
	MODE="$1"
	shift
elif [ "$#" -eq 3 ] &&
     { [ "$1" = "--process-dsp" ] ||
       [ "$1" = "--process-impulse-dsp" ] ||
       [ "$1" = "--process-stream-dsp" ]; }; then
	MODE="$1"
	TARGET_DSP="$2"
	shift 2
fi
if [ "$(id -u)" -ne 0 ] ||
   { [ "$MODE" = "--cleanup" ] && [ "$#" -ne 0 ]; } ||
   { [ "$MODE" != "--cleanup" ] &&
     { [ "$#" -ne 1 ] || [ ! -d "$1" ]; }; }; then
	echo "refusing: provide one private Experiment 029 capture directory as root" >&2
	exit 1
fi
CAPTURE_ROOT="${1-}"
if [ "$MODE" != "--cleanup" ] &&
   [ "$(basename "$CAPTURE_ROOT")" != "experiment-029-deadline-safe-sequence" ]; then
	echo "refusing: capture directory name is not experiment-029-deadline-safe-sequence" >&2
	exit 1
fi
if [ "$MODE" != "--cleanup" ] &&
	[ "${UAD2_ALLOW_ONE_SHOT_RESOURCE_PASS-}" != \
	  "YES_I_ACCEPT_OFFICIAL_REACTIVATION_MAY_BE_REQUIRED" ]; then
	echo "refusing: set the one-shot resource-pass acknowledgement" >&2
	exit 1
fi
if [ "$MODE" = "--process" ] &&
   [ "${UAD2_ALLOW_BOUNDED_PROCESS-}" != \
     "YES_I_ACCEPT_ONE_64_SAMPLE_REALVERB_PROCESS" ]; then
	echo "refusing: set the bounded Process acknowledgement" >&2
	exit 1
fi
if [ "$MODE" = "--process-dsp" ] ||
   [ "$MODE" = "--process-impulse-dsp" ] ||
   [ "$MODE" = "--process-stream-dsp" ]; then
	case "$TARGET_DSP" in
		0|1|2|3|4|5|6|7) ;;
		*) echo "refusing: target DSP must be 0 through 7" >&2; exit 1 ;;
	esac
	if [ "${UAD2_ALLOW_BOUNDED_PROCESS-}" != \
	     "YES_I_ACCEPT_ONE_64_SAMPLE_REALVERB_PROCESS" ]; then
		echo "refusing: set the bounded Process acknowledgement" >&2
		exit 1
	fi
fi

verify_chunk() {
	RELATIVE_PATH="$1"
	EXPECTED_SHA256="$2"
	CHUNK="$CAPTURE_ROOT/$RELATIVE_PATH"
	if [ ! -f "$CHUNK" ] ||
	   [ "$(sha256sum "$CHUNK" | awk '{print $1}')" != "$EXPECTED_SHA256" ]; then
		echo "refusing: private command target does not match $RELATIVE_PATH" >&2
		exit 1
	fi
}

if [ "$MODE" != "--cleanup" ]; then
	verify_chunk boundary-0000/command-0002-target.bin 0c353512fb27ed961b4e0746de7f1bbc462447f6e2c0263bc6209cda7b7718d0
	verify_chunk boundary-0002/command-0003-target.bin 6d91985233b00edfad9c3e93c17bb75c7e51922d11076fec03d3ab23f3c99e21
	verify_chunk boundary-0004/command-0004-target.bin 87512f74b674462241144deca658f59b165cee7e3d5635b1e0864ffecd284fb1
	verify_chunk boundary-0006/command-0005-target.bin 0c3561eb68e83169d38e58cd70d6bc9464afbd4efc2b3ac34bc339e19cffe79a
	verify_chunk boundary-0008/command-0006-target.bin e2f918628e4c4882e142113a86f3ee9fcf8034a5ec48c35b663bf0f839f60ed0
	verify_chunk boundary-0010/command-0007-target.bin bd62be051299bfea36e54119643fea6089423aea6a0b060b8269d4f572c2c78e
	verify_chunk boundary-0012/command-0008-target.bin 203be752a4d3fd8a11739484d3ad0af9ac26359fd1bec5850ef38dd0e1fd45ac
	verify_chunk boundary-0014/command-0009-target.bin f4c041ab7b0aa19ab9f32235e1b51f4db5b3f7f88fb3f526254b2a906ff96123
	verify_chunk boundary-0016/command-0010-target.bin 07177bd6b15ba0dceafad3ba3d12147f0f87040cdd0b291abb1bca9cfdeb5783
	verify_chunk boundary-0016/command-0011-target.bin e813ef001a733114e9d775e4b8a5f4f0a36e889088ed6cf9162ee12675ff69de
	verify_chunk boundary-0018/command-0012-target.bin 884f3682122e2dad85895eda6df3545c657c8b0dc2c65d71ddd68a3ae130e7d5
	verify_chunk boundary-0018/command-0013-target.bin 09a0f5d881656044220c0dfe9e8568411ea5e41c192e357aa49872bd37efda75
	verify_chunk boundary-0020/command-0014-target.bin 8e6f5a294bde687a7eeeab2d5a447666d289080d666c63f5e72724531caa6106
	verify_chunk boundary-0022/command-0015-target.bin ea339b46f1a8d871ac8122d01ce151289a415aa0fd23e6ff0d0840411338667b
	verify_chunk boundary-0024/command-0016-target.bin 17d79a7db9af334375b2f8b68f9640413f08becb04f52f59e4b8f3bf2b702914
	verify_chunk boundary-0024/command-0017-target.bin 91a2ef99d9afd44c3001c68b6a7b396cfb18410ed6e0c13ac2730afe5f623da9
fi
if [ "$MODE" = "--allocation" ] || [ "$MODE" = "--process" ] ||
   [ "$MODE" = "--process-dsp" ] ||
   [ "$MODE" = "--process-impulse-dsp" ] ||
   [ "$MODE" = "--process-stream-dsp" ]; then
	verify_chunk boundary-0058/command-0051-target.bin fe326c8a6a7d0b40e7958c14debe8ea1bc8d0fb160f7816bbaf9899b83590e84
fi

if [ ! -d "$DEVICE" ] || [ -L "$DEVICE/driver" ]; then
	echo "refusing: endpoint is absent or already bound" >&2
	exit 1
fi
if [ "$(cat "$DEVICE/vendor")" != "0x1a00" ] ||
   [ "$(cat "$DEVICE/device")" != "0x0002" ] ||
   [ "$(cat "$DEVICE/subsystem_vendor")" != "0x1a00" ] ||
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
if [ "$GROUP" != "16" ] || [ "$#" -ne 1 ] ||
   [ "$(basename "$1")" != "$BDF" ]; then
	echo "refusing: OCTO is not isolated in expected IOMMU group 16" >&2
	exit 1
fi
if [ ! -x "$PROBE" ]; then
	echo "refusing: Experiments 032-034 binary is missing" >&2
	exit 1
fi

cleanup() {
	if [ -L "$DEVICE/driver" ] &&
	   [ "$(basename "$(readlink -f "$DEVICE/driver")")" = "vfio-pci" ]; then
		echo "$BDF" > "$DRIVER/unbind" || true
	fi
	printf '\n' > "$DEVICE/driver_override" || true
}
trap cleanup EXIT HUP INT TERM

modprobe vfio-pci
printf '%s' "vfio-pci" > "$DEVICE/driver_override"
echo "$BDF" > /sys/bus/pci/drivers_probe
if [ ! -L "$DEVICE/driver" ] ||
   [ "$(basename "$(readlink -f "$DEVICE/driver")")" != "vfio-pci" ]; then
	echo "refusing: vfio-pci did not bind" >&2
	exit 1
fi

if [ "$MODE" = "--cleanup" ]; then
	"$PROBE" "$MODE"
	exit 0
fi

set -- \
	"$CAPTURE_ROOT/boundary-0000/command-0002-target.bin" \
	"$CAPTURE_ROOT/boundary-0002/command-0003-target.bin" \
	"$CAPTURE_ROOT/boundary-0004/command-0004-target.bin" \
	"$CAPTURE_ROOT/boundary-0006/command-0005-target.bin" \
	"$CAPTURE_ROOT/boundary-0008/command-0006-target.bin" \
	"$CAPTURE_ROOT/boundary-0010/command-0007-target.bin" \
	"$CAPTURE_ROOT/boundary-0012/command-0008-target.bin" \
	"$CAPTURE_ROOT/boundary-0014/command-0009-target.bin" \
	"$CAPTURE_ROOT/boundary-0016/command-0010-target.bin" \
	"$CAPTURE_ROOT/boundary-0016/command-0011-target.bin" \
	"$CAPTURE_ROOT/boundary-0018/command-0012-target.bin" \
	"$CAPTURE_ROOT/boundary-0018/command-0013-target.bin" \
	"$CAPTURE_ROOT/boundary-0020/command-0014-target.bin" \
	"$CAPTURE_ROOT/boundary-0022/command-0015-target.bin" \
	"$CAPTURE_ROOT/boundary-0024/command-0016-target.bin" \
	"$CAPTURE_ROOT/boundary-0024/command-0017-target.bin"
if [ "$MODE" = "--allocation" ] || [ "$MODE" = "--process" ] ||
   [ "$MODE" = "--process-dsp" ] ||
   [ "$MODE" = "--process-impulse-dsp" ] ||
   [ "$MODE" = "--process-stream-dsp" ]; then
	if [ "$MODE" = "--process-dsp" ] ||
	   [ "$MODE" = "--process-impulse-dsp" ] ||
	   [ "$MODE" = "--process-stream-dsp" ]; then
		"$PROBE" "$MODE" "$TARGET_DSP" "$@" \
			"$CAPTURE_ROOT/boundary-0058/command-0051-target.bin"
	else
		"$PROBE" "$MODE" "$@" \
			"$CAPTURE_ROOT/boundary-0058/command-0051-target.bin"
	fi
else
	"$PROBE" "$@"
fi
