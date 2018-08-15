#!/usr/bin/python3

import os
import sys

print(' '.join(sys.argv[1:]))
with open('/dev/famez_bridge_0a', 'w') as b:
    while True:
        b.write(' '.join(sys.argv[1:]))
