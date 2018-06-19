#!/usr/bin/python3

# https://twistedmatrix.com/documents/current/core/howto/defer.html

import sys
import time

from twisted.internet import reactor, defer

def cbString2(arg0):
    print('cbString("{}")'.format(arg0))
    return arg0 * 2

def cbInt(arg0):
    print('cbInt("{}")'.format(arg0))
    return int(arg0)

def shutdown():
    print('In shutdown')
    reactor.stop()

# Somewhere to park everything, like callback chains.
print('Adding callbacks')
deferred = defer.Deferred()
deferred.addCallback(cbString2)
deferred.addCallback(cbInt)

# Set up events, starting with the head of the deferred CB chain.
reactor.callLater(3, deferred.callback, sys.argv[1])
reactor.callLater(6, shutdown)

print('Starting the reactor')
reactor.run()
