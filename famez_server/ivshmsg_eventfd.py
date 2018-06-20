#!/usr/bin/python3

# This work is licensed under the terms of the GNU GPL, version 2 or
# (at your option) any later version.  See the LICENSE file in the
# top-level directory.

# Rocky Craig <rocky.craig@hpe.com>

# Routine names here mirror those in qemu/contrib/ivshmem-[client|server].
# The IVSHMEM communications protocol is based on 8-byte integers and an
# optional 4-byte file descriptor.  Twisted transport.sendFileDescriptor
# gets the packing sizes wrong so do it right.

import errno
import os
import struct
import sys

from twisted.internet import reactor as TIreactor   # should be same everywhere
from twisted.internet.interfaces import IReadDescriptor

from zope.interface import implementer

###########################################################################
# See qemu/util/event_notifier-posix.c for routine names and models; only
# the ones used by ivshmem-server.c are recreated here.  Also see
# https://sgros-students.blogspot.com/2013/05/calling-eventfd-from-python.html

from ctypes import cdll

class IVSHMEM_Event_Notifier(object):  # Probably overkill

    # /usr/include/x86_64-linux-gnu/bits/eventfd.h
    EFD_SEMAPHORE = 0o00000001
    EFD_CLOEXEC =   0o02000000
    EFD_NONBLOCK =  0o00004000

    _libc = cdll.LoadLibrary('libc.so.6')

    def __init__(self, init_val=0, active=False):
        self.rfd = self.wfd = self._libc.eventfd(
            init_val, self.EFD_NONBLOCK | self.EFD_CLOEXEC)
        assert self.rfd >= 0, 'eventfd() failed'
        if active:
            self.incr()

    def incr(self, delta=1):
        '''Corresponds to set() in C code, this is more descriptive.'''
        delta = int(delta)
        assert delta > 0, 'delta must be positive'
        bval = struct.pack('Q', delta)
        while True:
            try:
                return os.write(self.wfd, bval) == len(bval)
            except InterruptedError as e:   # handled interally at 3.5
                continue
            except OSError as e:
                if e.errno == errno.EINTR:
                    continue
                if e.errno == errno.EAGAIN: # would block
                    return False
                raise

    def reset(self):
        '''Without EFD_SEMAPHORE, reset if non-zero, else EAGAIN (NONBLOCK).'''
        while True:
            try:
                junk = os.read(self.rfd, 8)    # reset
                if len(junk) == 8:
                    return True, struct.unpack('Q', junk)
                return False, None
            except InterruptedError as e:      # handled interally at 3.5
                continue
            except OSError as e:
                if e.errno == errno.EINTR:
                    continue
                if e.errno == errno.EAGAIN: # would block
                    return False, None
                raise

    def get_fd(self):   # I'd love to hear this story...
        return self.rfd

    def cleanup(self):
        try:
            os.close(self.wfd)
        except Exception as e:
            pass
        self.rfd = self.wfd = -1


def ivshmem_event_notifier_list(count):
    return [ IVSHMEM_Event_Notifier() for _ in range(count) ]

###########################################################################
# https://stackoverflow.com/questions/28449455/integrating-hid-access-with-evdev-on-linux-with-python-twisted
# It also implicitly realizes IFDbizmumble


@implementer(IReadDescriptor)
class EventfdReader(object):

    def __init__(self, eventobj, callback):
        assert isinstance(eventobj, IVSHMEM_Event_Notifier), 'Bad object'
        self.eventobj = eventobj
        self.eventobj.last_value = None
        self.callback = callback

    def fileno(self):
        return self.eventobj.get_fd()   # Might as well use it

    def  logPrefix(self):
        return 'ServerEvent%d' % self.fileno()

    def doRead(self):
        fired, value = self.eventobj.reset()
        if fired:
            self.eventobj.last_value = value
            self.callback(self.eventobj)

    def connectionLost(self, reason):
        TIreactor.removeReader(self)  # Paranoid?  EAGAIN?  Use destroy()?
        self.eventobj.cleanup()
        self.eventobj = None
        self.callback = None

    def start(self):
        '''Convenience, not in twisted classes.'''
        TIreactor.addReader(self)

    def destroy(self):
        '''Convenience, not in twisted classes.'''
        TIreactor.removeReader(self)
        self.loseConnection()

