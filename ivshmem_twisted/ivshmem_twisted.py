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
    from famez_mailbox import prepare_mailbox, MAILBOX_MAX_SLOTS
except ImportError as e:
    from .ivshmem_sendrecv import ivshmem_send_one_msg
    from .ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader
    from .famez_mailbox import prepare_mailbox, MAILBOX_MAX_SLOTS

# Don't use peer ID 0, certain documentation implies it's reserved.  Put the
# server ID at the end of the list (nSlots-1) and use the middle for clients.

IVSHMEM_UNUSED_ID = 0

###########################################################################


class ProtocolIVSHMSG(TIPProtocol):

    # See qemu/docs/specs/ivshmem-spec.txt::Client-Server protocol and
    # qemu/contrib/ivshmem-server.c::ivshmem_server_handle_new_conn() calling
    # qemu/contrib/ivshmem-server.c::ivshmem_server_send_initial_info(), then
    # qemu/contrib/ivshmem-client.c::ivshmem_client_connect()

    IVSHMEM_PROTOCOL_VERSION = 0

    args = None        # Invariant across instances
    logmsg = None
    logerr = None
    peer_list = []     # Map transport fds to list of eventfds
    server_id = None

    # Non-standard addition to IVSHMEM server role: this server can be
    # interrupted and messaged to particpate in client activity.
    server_vectors = None

    def __init__(self, factory):
        # First do class-level initialization of singletons
        if self.args is None:
            assert isinstance(factory, TIPFactory), 'arg0 not my Factory'
            self.__class__.args = factory.args          # Seldom-used
            self.__class__.logmsg = self.args.logmsg    # Often-used
            self.__class__.logerr = self.args.logerr
            self.__class__.server_id = self.args.nSlots - 1

            if not self.args.silent:
                # Create eventfds for receiving messages in IVSHMEM fashion
                # then add a callback.  The object right now is the eventfd
                # itself, don't send "self" because this is destined to
                # be a true peer object.  Pad out the eventfd object with
                # attributes to assist the callback.
                self.__class__.server_vectors = ivshmem_event_notifier_list(
                    self.args.nSlots)
                for i, server_vector in enumerate(self.server_vectors):
                    server_vector.num = i
                    server_vector.logmsg = self.logmsg   # work on this...
                    tmp = EventfdReader(server_vector, self.ERcallback).start()

        # Finish with any actual instance attributes.
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

        if len(server_peer_list) >= self.args.nSlots - 2 or self.id == -1:
            self.logerr('Max clients reached (%d)' % IVSHMEM_MAX_CLIENTS)
            peer.send_initial_info(False)   # client complains but with grace
            return

        # Server line 175: create specified number of eventfds, mixed with
        # Server line 183: send peer id and shm fd
        try:
            peer.vectors = ivshmem_event_notifier_list(self.args.nSlots)
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

        # Non-standard voodoo: advertise me (this server) to the new peer.
        if not self.args.silent:
            for server_vector in self.server_vectors:
                ivshmem_send_one_msg(
                    peer.transport.socket,
                    self.server_id,
                    server_vector.wfd)

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
        '''Does not occur often, don't sweat the performance.'''
        max_clients = self.args.nSlots - 2  # 1 unused, 1 for the server
        if len(self.peer_list) >= max_clients:
            self.id = -1    # sentinel
            return
        current_ids = frozenset((p.id for p in self.peer_list))
        if not current_ids:
            self.id = 1
            return
        max_ids = frozenset((range(self.args.nSlots))) - \
                  frozenset((IVSHMEM_UNUSED_ID, self.server_id ))
        self.id = sorted(max_ids - current_ids)[0]

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
        vectorobj.logmsg('CALLBACK %d' % (vectorobj.num))

###########################################################################
# Normally the Endpoint and listen() call is done explicitly,
# interwoven with passing this constructor.  This approach hides
# all the twisted things in this module.

class FactoryIVSHMSG(TIPFactory):

    _required_arg_defaults = {
        'foreground':   True,       # Only affects logging choice in here
        'logfile':      '/tmp/ivshmem_log',
        'mailbox':      'ivshmem_mailbox',  # Will end up in /dev/shm
        'nSlots':       4,
        'silent':       True,       # Does NOT participate in eventfds/mailbox
        'socketpath':   '/tmp/ivshmem_socket',
        'verbose':      0,
    }

    def __init__(self, args=None):
        '''Args must be an object with the following attributes:
           foreground, logfile, mailbox, nSlots, silent, socketpath, verbose
           Suitable defaults will be supplied.'''

        # Pass command line args to ProtocolIVSHMSG, then open logging.
        if args is None:
            args = argparse.Namespace()
        for arg, default in self._required_arg_defaults.items():
            setattr(args, arg, getattr(args, arg, default))
        args.mailbox_fd = prepare_mailbox(args.mailbox, args.nSlots)

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

