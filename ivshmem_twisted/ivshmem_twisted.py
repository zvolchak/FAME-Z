#!/usr/bin/python3

# This work is licensed under the terms of the GNU GPL, version 2 or
# (at your option) any later version.  See the LICENSE file in the
# top-level directory.

# Rocky Craig <rocky.craig@hpe.com>

import argparse
import grp
import os
import sys

from twisted.python import log as TPlog
from twisted.python.logfile import DailyLogFile

from twisted.internet import error as TIError
from twisted.internet import reactor as TIreactor

from twisted.internet.endpoints import UNIXServerEndpoint

from twisted.internet.protocol import Factory as TIPFactory
from twisted.internet.protocol import Protocol as TIPProtocol

try:
    from ivshmem_sendrecv import ivshmem_send_one_msg
    from ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader
except ImportError as e:
    from .ivshmem_sendrecv import ivshmem_send_one_msg
    from .ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader


class ProtocolIVSHMSG(TIPProtocol):

    # See qemu/docs/specs/ivshmem-spec.txt::Client-Server protocol and
    # qemu/contrib/ivshmem-server.c::ivshmem_server_handle_new_conn() calling
    # qemu/contrib/ivshmem-server.c::ivshmem_server_send_initial_info(), then
    # qemu/contrib/ivshmem-client.c::ivshmem_client_connect()

    IVSHMEM_PROTOCOL_VERSION = 0

    next_peer_id = 1   # Docs say zero okay, not my experience
    peer_list = []     # Map transport fds to list of eventfds
    args = None        # Invariant across instances
    logmsg = None
    logerr = None
    # Non-standard addition to IVSHMEM protocol: this server can be
    # interrupted and messaged, and here is where the Gen-Z magic
    # will occur.
    famez_vectors = None
    FAMEZ_SERVER_ID = 4242

    def __init__(self, factory):
        # First do class-level initialization of singletons
        if self.args is None:
            assert isinstance(factory, TIPFactory), 'arg0 not my Factory'
            self.__class__.args = factory.args          # Seldom-used
            self.__class__.logmsg = self.args.logmsg    # Often-used
            self.__class__.logerr = self.args.logerr
            self.__class__.famez_vectors = ivshmem_event_notifier_list(10)

            # Create eventfds for receiving messages in IVSHMEM fashion
            # then add a callback.  The object right now is the eventfd
            # itself, don't send "self" because this is destined to
            # be a true peer object.  Pad out the eventfd object with
            # attributes to assist the callback.
            for i, famez_vector in enumerate(self.famez_vectors):
                famez_vector.num = i
                famez_vector.logmsg = self.logmsg   # work on this...
                tmp = EventfdReader(famez_vector, self.ERcallback).start()

        # Finish with the actual instance attributes
        self.create_new_peer_id()

    def logPrefix(self):    # This override works after instantiation
        return 'ProtocolIVSHMSG'

    def dataReceived(self, data):
        ''' TNSH :-) '''
        self.logerr('dataReceived')
        raise NotImplementedError(self)

    def connectionMade(self):
        self.logmsg(
            'Peer id %d @ socket %d' % (self.id, self.transport.fileno()))

        # Introduce variables that resemble the QEMU "ivshmem-server" code.
        peer = self
        server_peer_list = self.peer_list
        if len(server_peer_list) > 15:   # cuz server makes 16
            self.logerr('Max clients reached (15)')
            peer.send_initial_info(False)
            return

        # Server line 175: create specified number of eventfds, mixed with
        # Server line 183: send peer id and shm fd
        try:
            peer.vectors = ivshmem_event_notifier_list(self.args.nVectors)
        except Exception as e:
            self.logerr('Event notifiers failed: %s' % str(e))
            peer.send_initial_info(False)
            return
        peer.send_initial_info()

        # Server line 189: advertise the new peer to others.  Note that
        # this new "peer" has not yet been added to the list, so this
        # loop is not traversed for the first client.
        for other_peer in server_peer_list:
            for peer_vector in peer.vectors:
                ivshmem_send_one_msg(
                    other_peer.transport.socket,
                    peer.id,
                    peer_vector.wfd)

        # Server line 197: advertise the other peers to the new one
        for other_peer in server_peer_list:
            for other_peer_vector in other_peer.vectors:
                ivshmem_send_one_msg(
                    peer.transport.socket,
                    other_peer.id,
                    other_peer_vector.wfd)

        # Non-standard voodoo: advertise this server to the new one.
        if True:
            for famez_vector in self.famez_vectors:
                ivshmem_send_one_msg(
                    peer.transport.socket,
                    self.FAMEZ_SERVER_ID,
                    famez_vector.wfd)

        # Server line 205: advertise the new peer to itself
        for peer_vector in peer.vectors:
            ivshmem_send_one_msg(
                peer.transport.socket,
                peer.id,
                peer_vector.get_fd())   # Must be a good story here...

        # Oh yeah
        server_peer_list.append(peer)

    def connectionLost(self, reason):
        '''Tell the other peers that I have died.'''
        if reason.check(TIError.ConnectionDone) is None:    # Dirty
            txt = 'Dirty'
            logit = self.logerr
        else:
            txt = 'Clean'
            logit = self.logmsg
        logit('%s disconnect from peer id %d' % (txt, self.id))
        try:
            if self in self.peer_list:     # Only if everything was done
                self.peer_list.remove(self)
                for other_peer in self.peer_list:
                    ivshmem_send_one_msg(other_peer.transport.socket, self.id)

            for vector in self.vectors:
                vector.cleanup()

        except Exception as e:
            self.logerr('Closing peer transports failed: %s' % str(e))

    def create_new_peer_id(self):
        '''These tend to stay well below FAMEZ_SERVER_ID.'''
        self.id = self.next_peer_id
        self.__class__.next_peer_id += 1

    def send_initial_info(self, ok=True):
        # Server line 103 and client line 210:
        # Protocol version, with no explicit mention of an fd.
        thesocket = self.transport.socket
        if not ok:
            # Will violate the version check and bomb the client.
            ivshmem_send_one_msg(thesocket, -1)
            self.transport.loseConnection()
            self.id = -1
            return
        ivshmem_send_one_msg(thesocket, self.IVSHMEM_PROTOCOL_VERSION)

        # Server line 111 and client line 217:
        # our index/id and explicit need for fd == -1 on client side
        ivshmem_send_one_msg(thesocket, self.id)

        # Server line 119 and client line 225:
        # -1 "index" and a real FD that still seems to be used.  It
        # may supersede the QEMU 2.5 explicit declaration of ivshmem.
        ivshmem_send_one_msg(thesocket, -1, self.args.mailbox_fd)

    @staticmethod
    def ERcallback(vectorobj):
        peer_id = 43
        vectorobj.logmsg('CALLBACK %d peer %d' % (vectorobj.num, peer_id))

###########################################################################
# The IVSHMEM protocol practiced by QEMU demands a memory-mappable file
# descriptor as part of the initial exchange, so give it one.  The mailbox
# is a shared common area.  In non-silent mode, each client gets 8k, max
# 31 extra clients after server area which starts at zero, thus 256k.


def _prepare_mailbox(path, size=262144):
    '''Starts with mailbox base name, returns an fd to open file.'''

    oldumask = os.umask(0)
    gr_name = 'libvirt-qemu'
    if '/' not in path:
        path = '/dev/shm/' + path
    try:
        gr_gid = grp.getgrnam(gr_name).gr_gid
        if not os.path.isfile(path):
            fd = os.open(path, os.O_RDWR | os.O_CREAT, mode=0o666)
            os.posix_fallocate(fd, 0, size)
            os.fchown(fd, -1, gr_gid)
        else:   # Re-condition and re-use
            STAT = os.path.stat         # for constants
            lstat = os.lstat(path)
            assert STAT.S_ISREG(lstat.st_mode), 'not a regular file'
            assert lstat.st_size >= size, 'size is < 128k'
            if lstat.st_gid != gr_gid:
                print('Changing %s to group %s' % (path, gr_name))
                os.chown(path, -1, gr_gid)
            if lstat.st_mode & 0o660 != 0o660:  # at least
                print('Changing %s to permissions 666' % path)
                os.chmod(path, 0o666)
            fd = os.open(path, os.O_RDWR)
    except Exception as e:
        raise SystemExit('Problem with %s: %s' % (path, str(e)))

    os.umask(oldumask)
    return fd

###########################################################################
# Normally the Endpoint and listen() call is done explicitly,
# interwoven with passing this constructor.  This approach hides
# all the twisted things in this module.

class FactoryIVSHMSG(TIPFactory):

    _required_arg_defaults = {
        'foreground':   True,       # Only affects logging choice in here
        'logfile':      '/tmp/ivshmem_log',
        'mailbox':      'ivshmem_mailbox',  # Will end up in /dev/shm
        'mailbox_size': 262144,             # See _prepare_mailbox above
        'nVectors':     1,
        'silent':       True,       # Does NOT participate in eventfds/mailbox
        'socketpath':   '/tmp/ivshmem_socket',
        'verbose':      0,
    }

    def __init__(self, args=None):
        '''Args must be an object with the following attributes:
           foreground, logfile, mailbox, nVectors, silent, socketpath, verbose
           Suitable defaults will be supplied.'''

        # Pass command line args to ProtocolIVSHMSG, then open logging.
        if args is None:
            args = argparse.Namespace()
        for arg, default in self._required_arg_defaults.items():
            setattr(args, arg, getattr(args, arg, default))
        if not hasattr(args, 'mailbox_fd'):     # They could open it themselves
            args.mailbox_fd = _prepare_mailbox(args.mailbox, args.mailbox_size)

        self.args = args
        if args.foreground:
            TPlog.startLogging(sys.stdout, setStdout=False)
        else:
            print('Logging to %s' % args.logfile, file=sys.stderr)
            TPlog.startLogging(
                DailyLogFile.fromFullPath(args.logfile),
                setStdout=True)     # "Pass-through" explicit print() for debug
        args.logmsg = TPlog.msg
        args.logerr = TPlog.err

        # By Twisted version 18, "mode=" is deprecated and you should just
        # inherit the tacky bit from the parent directory.  wantPID creates
        # <path>.lock as a symlink to "PID".
        E = UNIXServerEndpoint(
            TIreactor,
            args.socketpath,
            mode=0o666,         # Deprecated at Twisted 18
            wantPID=True)
        E.listen(self)
        args.logmsg('Listening on %s' % args.socketpath)

    def buildProtocol(self, useless_addr):
        # Docs mislead, have to explicitly pass something to get persistent
        # state across protocol/transport invocations.  Send this factory.
        return ProtocolIVSHMSG(self)

    def run(self):
        TIreactor.run()

