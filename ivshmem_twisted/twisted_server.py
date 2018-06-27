#!/usr/bin/python3

# This work is licensed under the terms of the GNU GPL, version 2 or
# (at your option) any later version.  See the LICENSE file in the
# top-level directory.

# Rocky Craig <rocky.craig@hpe.com>

import argparse
import grp
import mmap
import os
import struct
import sys

from collections import OrderedDict

from twisted.python import log as TPlog
from twisted.python.logfile import DailyLogFile

from twisted.internet import error as TIError
from twisted.internet import reactor as TIreactor

from twisted.internet.endpoints import UNIXServerEndpoint

from twisted.internet.protocol import ServerFactory as TIPServerFactory
from twisted.internet.protocol import Protocol as TIPProtocol

try:
    from ivshmem_sendrecv import ivshmem_send_one_msg
    from ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader
    from famez_mailbox import prepare_mailbox, MAILBOX_MAX_SLOTS, pickup_from_slot, place_in_slot
except ImportError as e:
    from .ivshmem_sendrecv import ivshmem_send_one_msg
    from .ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader
    from .famez_mailbox import prepare_mailbox, MAILBOX_MAX_SLOTS, pickup_from_slot, place_in_slot

# Don't use peer ID 0, certain documentation implies it's reserved.  Put the
# server ID at the end of the list (nSlots-1) and use the middle for clients.

IVSHMEM_UNUSED_ID = 0

###########################################################################
# See qemu/docs/specs/ivshmem-spec.txt::Client-Server protocol and
# qemu/contrib/ivshmem-server.c::ivshmem_server_handle_new_conn() calling
# qemu/contrib/ivshmem-server.c::ivshmem_server_send_initial_info(), then
# qemu/contrib/ivshmem-client.c::ivshmem_client_connect()


class ProtocolIVSHMSGServer(TIPProtocol):

    IVSHMEM_PROTOCOL_VERSION = 0

    args = None        # Invariant across instances
    logmsg = None
    logerr = None
    peer_list = []     # Map peer IDs to list of eventfds
    server_id = None

    # Non-standard addition to IVSHMEM server role: this server can be
    # interrupted and messaged to particpate in client activity.
    famez_notifiers = None

    def __init__(self, factory):
        # First do class-level initialization of singletons
        if self.args is None:
            assert isinstance(factory, TIPServerFactory), 'arg0 not my Factory'
            self.__class__.args = factory.args          # Seldom-used
            self.__class__.logmsg = self.args.logmsg    # Often-used
            self.__class__.logerr = self.args.logerr
            self.__class__.server_id = self.args.nSlots - 1
            self.__class__.mailbox_mm = mmap.mmap(self.args.mailbox_fd, 0)

            # Usually create eventfds for receiving messages in IVSHMSG and
            # set up a callback.  This early arming is not a race condition
            # as the peer for which this is destined has not yet been told
            # of the fds it would use to trigger here.
            print(self.args, file=sys.stderr)
            if not self.args.silent:
                self.__class__.famez_notifiers = ivshmem_event_notifier_list(
                    self.args.nSlots)
                for i, this_notifier in enumerate(self.famez_notifiers):
                    this_notifier.num = i
                    tmp = EventfdReader(this_notifier, self.ERcallback, self)
                    tmp.start()

        self.create_new_peer_id()   # Check if it worked after connection is up

    def logPrefix(self):    # This override works after instantiation
        return 'ProtocolIVSHMSG'

    def dataReceived(self, data):
        ''' TNSH :-) '''
        self.logerr('dataReceived')
        raise NotImplementedError(self)

    # Use variables that resemble the QEMU "ivshmem-server.c":  peer == self.
    # If errors occur early enough, send a bad revision to the client so it
    # terminates the connection.
    def connectionMade(peer):
        peer.logmsg(
            'Peer id %d @ socket %d' % (peer.id, peer.transport.fileno()))
        if peer.id == -1:
            peer.logerr('Max clients reached')
            peer.send_initial_info(False)   # client complains but with grace
            return
        server_peer_list = peer.peer_list

        # Server line 175: create specified number of eventfds.  These are
        # shared with all other clients who use them to signal each other.
        try:
            peer.vectors = ivshmem_event_notifier_list(peer.args.nSlots)
        except Exception as e:
            peer.logerr('Event notifiers failed: %s' % str(e))
            peer.send_initial_info(False)
            return

        # Server line 183: send version, peer id, shm fd
        peer.send_initial_info()

        # Server line 189: advertise the new peer to others.  Note that
        # this new peer has not yet been added to the list; this loop is
        # NOT traversed for the first peer to connect.
        for other_peer in server_peer_list:
            for peer_vector in peer.vectors:
                ivshmem_send_one_msg(
                    other_peer.transport.socket,
                    peer.id,
                    peer_vector.wfd)

        # Server line 197: advertise the other peers to the new one.
        for other_peer in server_peer_list:
            for other_peer_vector in other_peer.vectors:
                ivshmem_send_one_msg(
                    peer.transport.socket,
                    other_peer.id,
                    other_peer_vector.wfd)

        # Non-standard voodoo: advertise me (this server) to the new one.
        # It's just one more grouping in the previous batch.
        if peer.famez_notifiers:
            peer.logmsg('FAMEZ voodoo: sending server (ID=%d) notifiers' %
                peer.server_id)
            for server_vector in peer.famez_notifiers:
                ivshmem_send_one_msg(
                    peer.transport.socket,
                    peer.server_id,
                    server_vector.wfd)

        # Server line 205: advertise the new peer to itself, ie, send the
        # eventfds it needs for receiving messages.  This final batch
        # where the embedded peer.id matches the initial_info id is the
        # sentinel that communications are finished.
        for peer_vector in peer.vectors:
            ivshmem_send_one_msg(
                peer.transport.socket,
                peer.id,
                peer_vector.get_fd())   # Must be a good story here...

        # Oh yeah
        server_peer_list.append(peer)

    def connectionLost(self, reason):
        '''Tell the other peers that this one has died.'''
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

    def send_initial_info(peer, ok=True):   # keep the convention self=="peer"
        # Protocol version without fd.
        thesocket = peer.transport.socket
        if not ok:  # Violate the version check and bomb the client.
            ivshmem_send_one_msg(thesocket, -1)
            peer.transport.loseConnection()
            peer.id = -1
            return
        ivshmem_send_one_msg(thesocket, peer.IVSHMEM_PROTOCOL_VERSION)

        # The client's id, without an fd.
        ivshmem_send_one_msg(thesocket, peer.id)

        # -1 for data and the fd of the ivshmem file.  Using this protocol
        # a valid fd is required.
        ivshmem_send_one_msg(thesocket, -1, peer.args.mailbox_fd)

    @staticmethod
    def ERcallback(vectorobj):
        selph = vectorobj.cbdata
        nodename, msg = pickup_from_slot(selph.mailbox_mm, vectorobj.num)
        selph.logmsg('"%s" (%d) -> "%s"' % (nodename, vectorobj.num, msg))
        if msg == 'ping':
            place_in_slot(selph.mailbox_mm, selph.server_id, 'PONG')
            for peer in selph.peer_list:
                if peer.id == vectorobj.num:
                    break
            else:   # The peer disappeared?
                selph.logmsg('Disappeering act')
                return
            peer.vectors[selph.server_id].incr()

###########################################################################
# Normally the Endpoint and listen() call is done explicitly,
# interwoven with passing this constructor.  This approach hides
# all the twisted things in this module.


class FactoryIVSHMSGServer(TIPServerFactory):

    _required_arg_defaults = {
        'foreground':   True,       # Only affects logging choice in here
        'logfile':      '/tmp/ivshmem_log',
        'mailbox':      'ivshmem_mailbox',  # Will end up in /dev/shm
        'nSlots':       4,
        'silent':       False,      # Does participate in eventfds/mailbox
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
        return ProtocolIVSHMSGServer(self)

    def run(self):
        TIreactor.run()

