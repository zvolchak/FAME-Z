#!/usr/bin/python3

# This work is licensed under the terms of the GNU GPL, version 2 or
# (at your option) any later version.  See the COPYING file in the
# top-level directory.

# Rocky Craig <rjsnoose@gmail.com>

import sys

from twisted.python import log as TPlog
from twisted.python.logfile import DailyLogFile

from twisted.internet import error as TIError
from twisted.internet import reactor as TIreactor

from twisted.internet.endpoints import UNIXServerEndpoint

from twisted.internet.protocol import Factory as TIPFactory
from twisted.internet.protocol import Protocol as TIPProtocol

from ivshmsg_sendrecv import ivshmem_send_one_msg
from ivshmsg_eventfd import ivshmem_event_notifier_list

class ProtocolIVSHMSG(TIPProtocol):

    # See qemu/docs/specs/ivshmem-spec.txt::Client-Server protocol and
    # qemu/contrib/ishmem-server.c::ivshmem_server_handle_new_conn() calling
    # qemu/contrib/ishmem-server.c::ivshmem_server_send_initial_info(), then
    # qemu/contrib/ivshmem-client.c::ivshmem_client_connect()

    IVSHMEM_PROTOCOL_VERSION = 0

    _next_peer_id = 1   # Docs say zero okay, not my experience
    _peer_list = []     # Map transport fds to list of eventfds

    def __init__(self, factory):
        assert isinstance(factory, TIPFactory), 'arg0 is not my Factory'
        self.logmsg = factory.args.logmsg
        self.logerr = factory.args.logerr
        self.mailbox_fd = factory.args.mailbox_fd
        self.nVectors = factory.args.nVectors
        self.socketpath = factory.args.socketpath
        self.verbose = factory.args.verbose
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
        server_peer_list = self._peer_list
        if len(server_peer_list) > 15:   # cuz server makes 16
            self.logerr('Max clients reached (15)')
            peer.send_initial_info(False)
            return

        # Server line 175: create specified number of eventfds, mixed with
        # Server line 183: send peer id and shm fd
        try:
            peer.vectors = ivshmem_event_notifier_list(self.nVectors)
        except Exception as e:
            self.logerr('Event notifiers failed: %s' % str(e))
            peer.send_initial_info(False)
            return
        peer.send_initial_info()

        # Server line 189: advertise the new peer to others.  Note that
        # this new "peer" has not yet been added to the list.
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
            if self in self._peer_list:     # Only if everything was done
                self._peer_list.remove(self)
                for other_peer in self._peer_list:
                    ivshmem_send_one_msg(other_peer.transport.socket, self.id)

            for vector in self.vectors:
                vector.cleanup()

        except Exception as e:
            self.logerr('Closing peer transports failed: %s' % str(e))

    def create_new_peer_id(self):
        self.id = self._next_peer_id
        self.__class__._next_peer_id += 1

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
        ivshmem_send_one_msg(thesocket, -1, self.mailbox_fd)


class FactoryIVSHMSG(TIPFactory):

    def __init__(self, args):
        '''Normally the Endpoint and listen() call is done explicitly,
           interwoven with passing this constructor.  This approach hides
           all the twisted things in this module.'''

        # Pass command line args to ProtocolIVSHMSG, then open logging.
        self.args = args
        if args.foreground:
            TPlog.startLogging(sys.stdout, setStdout=False)
        else:
            logfile = '%s.log' % args.socketpath
            print('Logging to %s' % logfile, file=sys.stderr)
            TPlog.startLogging( DailyLogFile.fromFullPath(logfile),
                setStdout=True)
        args.logmsg = TPlog.msg
        args.logerr = TPlog.err

        # By Twisted version 18, "mode=" is deprecated and you should just
        # inherit the tacky bit from the parent directory.  wantPID creates
        # <path>.lock as a symlink to "PID".
        E = UNIXServerEndpoint(
            TIreactor,
            args.socketpath,
            mode=0o666,
            wantPID=True)
        E.listen(self)
        args.logmsg('Listening on %s' % args.socketpath)

    def buildProtocol(self, useless_addr):
        # Docs mislead, have to explicitly pass something to get persistent
        # state across protocol/transport invocations.  Send this factory.
        return ProtocolIVSHMSG(self)

    def run(self):
        TIreactor.run()

