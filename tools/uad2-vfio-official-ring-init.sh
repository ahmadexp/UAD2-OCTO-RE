#!/bin/sh
set -eu

SOURCE=tools/vfio_official_ring_init.c
REMOTE_SOURCE=/tmp/vfio_official_ring_init.c
REMOTE_BINARY=/tmp/uad2-vfio-official-ring-init
REMOTE_WRAPPER=/tmp/uad2-vfio-official-ring-init-remote.sh
TARGET=${UAD2_TARGET:?set UAD2_TARGET to the SSH target, for example user@uad2-host}

scp "$SOURCE" "$TARGET:$REMOTE_SOURCE"
scp tools/uad2-vfio-official-ring-init-remote.sh "$TARGET:$REMOTE_WRAPPER"
ssh -tt "$TARGET" "cc -O2 -Wall -Wextra -Werror -o $REMOTE_BINARY $REMOTE_SOURCE && chmod +x $REMOTE_WRAPPER && sudo $REMOTE_WRAPPER"
