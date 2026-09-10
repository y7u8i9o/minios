#!/bin/sh
# The default peer of a case with `nic dgram`: netpeer counting the frames
# the guest sends. The harness passes PEER_READY, PEER_LOG, PEER_PID and
# NETPEER; the arguments select another netpeer mode (peer-count.sh echo).
exec "$NETPEER" --ready "$PEER_READY" --log "$PEER_LOG" --pid "$PEER_PID" --mode "${1:-count}"
