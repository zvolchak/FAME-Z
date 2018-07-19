#!/usr/bin/python3

import os
import sys

print(' '.join(sys.argv[1:]))
with open('/dev/famez0a_bridge', 'w') as b:
    while True:
        print(' '.join(sys.argv[1:]))
        b.write(' '.join(sys.argv[1:]))
        raise SystemExit(0)
