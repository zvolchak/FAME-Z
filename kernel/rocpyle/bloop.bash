#!/bin/bash

if [ $# -ne 3 -o "$1" = "-h" -o $1 = "-?" ]; then
	echo "usage: `basename $0` target multiplier dev" >&2
	echo "	Multiplier is number of 10-byte buffers (max 18)" >&2
	exit 1
fi

set -u

###########################################################################
# Construct the buffer.  Max write is currently 192 bytes

TARGET=$1
MULT=$2
DEV=$3

[ $TARGET -lt 1 -o $TARGET -gt 32 ] && echo "$TARGET OOB" >&2 && exit 1
[ ! -w "$DEV" ] && echo "$DEV not writeable" >&2 && exit 1
[ $MULT -lt 1 ] && echo "$Mulitplier OOB" >&2 && exit 1
[ $MULT -gt 18 ] && echo "Multiplier capped" && MULT=18

GBUF="$TARGET:"
for ((i=0; i<$MULT; i++)); do GBUF="${GBUF}0123456789" ; done
let LEN=$MULT*10

echo "Each write is $LEN bytes; press control-C to stop"

###########################################################################
# Go until interrupt (control-C)

trap "break" SIGINT
W=0
START=`date +%s`
while true; do
	echo -n $GBUF > $DEV || break
	let W++
done

NOW=`date +%s`

let DELTA=$NOW-$START
[ $DELTA -eq 0 ] && echo Let it run longer >&2 && exit 1

let WPS=$W/$DELTA
let B=$W*LEN
let WPS=$W/$DELTA
let BPS=$B/$DELTA

echo
echo "$W writes, $B bytes in $DELTA secs = $WPS w/s, $BPS b/s"

exit 0
