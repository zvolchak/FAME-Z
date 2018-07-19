#!/usr/bin/python3

import os
import sys

while True:
    with open('/dev/famez0a_bridge', 'w') as b:
        b.write(' '.join(sys.argv[1:]))
