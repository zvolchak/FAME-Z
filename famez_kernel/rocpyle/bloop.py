#!/usr/bin/python3

import os
import sys
import time

fast = os.getenv('FAST', '0').lower() not in ('0', 'n', 'no', 'false')
print('FAST mode:', fast)

from pdb import set_trace

target = int(sys.argv[1])
mult = int(sys.argv[2])
dev = sys.argv[3]

assert 1 <= target <= 32, 'target out of range'
assert 1 <= mult, 'mult out of range'
if mult > 18:
    print('Multiplier capped')
    mult = 18

gbuf = ('%d:' % target) + '0123456789' * mult
if not fast:
    gbuf = gbuf.encode()
gbuflen = len(gbuf)
print('START', time.ctime())    # has it's own LF
delta = time.time()
try:
    w = 0
    while True:
        if fast:    # System buffering in parallel from close
            with open(dev, 'w') as bridge:
                assert bridge.write(gbuf) == gbuflen
        else:       # Fully synchronous, same as never closing
            with open(dev, 'wb', 0) as bridge:
                assert bridge.write(gbuf) == gbuflen
        w += 1
except KeyboardInterrupt as e:
    pass
except Exception as e:
    print(str(e))

delta = int(time.time() - delta)
if not delta:
    raise SystemExit('Run it longer')
print('\nSTOP ', time.ctime())    # has it's own LF

bytes = w * mult * 10
print("\n%d writes, %d bytes in %d seconds = %d w/s, %d b/s" %
    (w, bytes, delta, int (w/delta), int(bytes/delta)))

raise SystemExit(0)
