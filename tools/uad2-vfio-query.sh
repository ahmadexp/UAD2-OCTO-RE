#!/bin/sh
set -eu

SOURCE=tools/vfio_query_command.c
MODE=
case "${1:-}" in
    --with-interrupt-gates)
        MODE=$1
        ;;
    "")
        ;;
    *)
        SOURCE=$1
        MODE=${2:-}
        ;;
esac
case "$MODE" in
    ""|--with-interrupt-gates)
        ;;
    *)
        echo "usage: $0 [source.c] [--with-interrupt-gates]" >&2
        exit 1
        ;;
esac
REMOTE_MODE=
if [ "$MODE" = "--with-interrupt-gates" ]; then
    REMOTE_MODE=experiment-010
fi
REMOTE_SOURCE=/tmp/vfio_query_command.c
REMOTE_BINARY=/tmp/vfio_query_command
REMOTE_WRAPPER=/tmp/uad2-vfio-query-remote.sh
TARGET=${UAD2_TARGET:?set UAD2_TARGET to the SSH target, for example user@uad2-host}

scp "$SOURCE" "$TARGET:$REMOTE_SOURCE"
scp tools/uad2-vfio-query-remote.sh "$TARGET:$REMOTE_WRAPPER"
ssh -tt "$TARGET" "cc -O2 -Wall -Wextra -Werror -o $REMOTE_BINARY $REMOTE_SOURCE && chmod +x $REMOTE_WRAPPER && sudo $REMOTE_WRAPPER $REMOTE_MODE"
