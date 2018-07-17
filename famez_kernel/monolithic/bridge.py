#!/usr/bin/python3

import os
import sys

from pdb import set_trace

while True:
    with open('/dev/famez0a_bridge', 'w') as b:
        b.write(' '.join(sys.argv[1:]))


