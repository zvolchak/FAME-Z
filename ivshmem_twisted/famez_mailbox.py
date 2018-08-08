#!/usr/bin/python3

# The IVSHMEM protocol practiced by QEMU demands a memory-mappable file
# descriptor as part of the initial exchange, so give it one.  The mailbox
# is a shared common area.  In non-silent mode it's split into "nSlots"
# slots: slot 0 is a global read-only area populated below, the final
# entry "nSlots-1" is the server mailbox, and the middle "nSlots-2" entries
# are for that many clients.  Right now a mailbox is 512 bytes:
# 128 bytes of metadata for a sent message
# Then 384 of message buffer.
# famez.ko will read the global data to understand the mailbox layout.

# All numbers are unsigned long long (8 bytes).  All strings are multiples
# of 16 (including the C terminating NULL) on 32-byte boundaries.  Then it
# all looks good in "od -Ad -c" and even better in "od -Ad -c -tu8".

import mmap
import os
import struct
from time import sleep as SLEEP
from time import time as NOW

from os.path import stat as STAT    # for constants
from pdb import set_trace

class FAMEZ_MailBox(object):

    MAILBOX_MAX_SLOTS = 64
    MAILBOX_SLOTSIZE = 256          # 64 of metadata, 192 of message

    GLOBALS_SLOTSIZE_off = 0        # in the space of mailslot 0
    GLOBALS_MSG_OFFSET_off = 8
    GLOBALS_NSLOTS_off = 16

    # Metadata (front of famez_mailslot_t) is char[32] plus a few uint64_t.
    # The actual message space starts after that, 32-byte aligned, which
    # makes this look pretty: od -Ad -c -tx8 /dev/shm/famez_mailbox.
    MAILSLOT_NODENAME_off = 0
    MAILSLOT_NODENAME_SIZE = 32                     # NULL terminated
    MAILSLOT_MSGLEN_off = MAILSLOT_NODENAME_SIZE
    MAILSLOT_PEER_ID_off = MAILSLOT_MSGLEN_off + 8
    MAILSLOT_LAST_RESPONDER_off = MAILSLOT_PEER_ID_off + 8
    MAILSLOT_MSG_off = 64                           # Currently 1 pad
    MAILSLOT_MAX_MSGLEN = 192

    #-----------------------------------------------------------------------
    # Globals at offset 0 (slot 0)
    # Each slot (1 through nSlots-1) has a peer_id
    # Server mailbox at slot (nSlots - 1): first 32 bytes are host name.

    def _populate(self):
        self.mm = mmap.mmap(self.fd, 0)

        # Empty it.  Simple code that's never too demanding on size
        data = b'\0' * (self.nSlots * self.MAILBOX_SLOTSIZE)
        self.mm[0:len(data)] = data

        # Fill in the globals.  Must match the C struct in famez.ko.
        data = struct.pack('QQQ',
            self.MAILBOX_SLOTSIZE, self.MAILSLOT_MSG_off, self.nSlots)
        self.mm[0:len(data)] = data

        # Set the peer_id for each slot as a C integer.  While python client
        # can discern a sender, the famez.ko driver needs it "inband".

        for slot in range(1, self.nSlots):
            index = slot * self.MAILBOX_SLOTSIZE + self.MAILSLOT_PEER_ID_off
            packed_peer = struct.pack('Q', slot)   # uint64_t
            # print('------- index = %d, size =  = %d' %
                # (index, len(packed_peer)))
            self.mm[index:index + len(packed_peer)] = packed_peer

        # My "hostname", zero-padded
        data = self.nodename.encode()
        index = (self.nSlots - 1) * self.MAILBOX_SLOTSIZE
        self.mm[index:index + len(data)] = data

    #----------------------------------------------------------------------
    # Polymorphic.  Someday I'll learn about metaclasses.

    def __init__(self, pathORfd, nSlots=None, client_id=-1, nodename=None):
        '''Server: Starts with string and slot count from command line.
           Client: starts with an fd and id.'''

        assert (self.MAILSLOT_MSG_off + self.MAILSLOT_MAX_MSGLEN
            == self.MAILBOX_SLOTSIZE), 'Fix this NOW'

        if isinstance(pathORfd, int):
            assert client_id > 0 and nodename is not None, 'Bad call, ump!'
            self.fd = pathORfd
            self.my_id = client_id
            self.nodename = nodename
            self._init_client_mailslot()
            return
        assert isinstance(pathORfd, str), 'Need a path OR open fd'
        assert client_id == -1, 'Cannot assign client id to server'
        path = pathORfd     # Match previously written code

        # One for global, one for server, 4 slots is max two clients
        assert 4 <= nSlots <= self.MAILBOX_MAX_SLOTS, 'Bad nSlots'

        # Go for the max to simple libvirt domain XML
        size = self.MAILBOX_MAX_SLOTS * self.MAILBOX_SLOTSIZE
        gr_gid = -1     # Makes no change.  Try Debian, CentOS, other
        for gr_name in ('libvirt-qemu', 'libvirt', 'libvirtd'):
            try:
                gr_gid = grp.getgrnam(gr_name).gr_gid
                break
            except Exception as e:
                pass

        if '/' not in path:
            path = '/dev/shm/' + path
        oldumask = os.umask(0)
        try:
            if not os.path.isfile(path):
                fd = os.open(path, os.O_RDWR | os.O_CREAT, mode=0o666)
                os.posix_fallocate(fd, 0, size)
                os.fchown(fd, -1, gr_gid)
            else:   # Re-condition and re-use
                lstat = os.lstat(path)
                assert STAT.S_ISREG(lstat.st_mode), 'not a regular file'
                assert lstat.st_size >= size, \
                    'existing size (%d) is < required (%d)' % (
                        lstat.st_size, size)
                if lstat.st_gid != gr_gid and gr_gid > 0:
                    print('Changing %s to group %s' % (path, gr_name))
                    os.chown(path, -1, gr_gid)
                if lstat.st_mode & 0o660 != 0o660:  # at least
                    print('Changing %s to permissions 666' % path)
                    os.chmod(path, 0o666)
                fd = os.open(path, os.O_RDWR)
        except Exception as e:
            raise RuntimeError('Problem with %s: %s' % (path, str(e)))

        os.umask(oldumask)

        # Just some stuff that's probably very handy
        self.path = path            # Final absolute path
        self.fd = fd
        self.nSlots = nSlots
        self.server_id = nSlots - 1   # Because that's the rule
        self.nodename = 'Z-Server'
        self._populate()

    #----------------------------------------------------------------------
    # Dig the mail and node name out of the slot for peer_id (1:1 mapping).
    # It's not so much (passively) receivng mail as it is actively getting.

    def empty(self, peer_id, asbytes=False, clear=True):
        '''Return the nodename and message.'''
        assert 1 <= peer_id <= self.server_id, \
            'Slotnum is out of domain 1 - %d' % (self.server_id)
        index = peer_id * self.MAILBOX_SLOTSIZE     # start of nodename
        nodename, msglen = struct.unpack('32sQ', self.mm[index:index + 40])
        nodename = nodename.split(b'\0', 1)[0].decode()
        index += self.MAILSLOT_MSG_off
        fmt = '%ds' % msglen
        msg = struct.unpack(fmt, self.mm[index:index + msglen])

        # The message is copied so mark the mailslot length zero as handshake
        # to the requester that its mailbox has been emptied.

        if clear:
            index = peer_id * self.MAILBOX_SLOTSIZE + self.MAILSLOT_MSGLEN_off
            self.mm[index:index + 8] = struct.pack('Q', 0)

        # Clean up the message copyout, which is a single element tuple
        # that has a NUL at msglen.
        # msg = msg[0].split(b'\0', 1)[0] # only valid for pure stringsA
        msg = msg[0][:msglen]
        if not asbytes:
            msg = msg.decode()
        return nodename, msg

    #----------------------------------------------------------------------
    # Post a message to the indicated mailbox slot but don't kick the
    # EventFD.  First, this routine doesn't know about them and second,
    # keeping it a separate operation facilitates spoofing in the client.

    def fill(self, sender_id, msg):
        assert 1 <= sender_id <= self.server_id, \
            'Peer ID is out of domain 1 - %d' % (self.server_id)
        if isinstance(msg, str):
            msg = msg.encode()
        assert isinstance(msg, bytes), 'msg must be string or bytes'
        msglen = len(msg)   # It's bytes now
        assert msglen < self.MAILSLOT_MAX_MSGLEN, 'Message too long'

        # The previous responder needs to clear the msglen to indicate it
        # has pulled the message out of the sender's mailbox.
        index = sender_id * self.MAILBOX_SLOTSIZE + self.MAILSLOT_MSGLEN_off
        stop = NOW() + 1.0
        while NOW() < stop and struct.unpack('Q', self.mm[index:index+8])[0]:
            SLEEP(0.1)
        if NOW() >= stop:
            print('pseudo-HW not ready to receive wait limit: stomping')

        self.mm[index:index + 8] = struct.pack('Q', msglen)
        index = sender_id * self.MAILBOX_SLOTSIZE + self.MAILSLOT_MSG_off
        self.mm[index:index + msglen] = msg
        # For Justin: NUL-terminate the message.
        self.mm[index + msglen] = 0

    #----------------------------------------------------------------------
    # Called by Python client on graceful shutdowns, and always by server
    # when a peer dies.  This is mostly for QEMU crashes so the nodename
    # is not accidentally when a QEMU restarts, before loading famez.ko.

    def clear_my_mailslot(self, nodenamebytes=None, override_id=None):
        id = override_id if override_id else self.my_id
        assert 1 <= id <= self.server_id, 'slot is bad: %d' % id
        index = id * self.MAILBOX_SLOTSIZE
        zeros = b'\0' * self.MAILSLOT_NODENAME_SIZE
        self.mm[index:index + len(zeros)] = zeros
        if nodenamebytes:
            assert len(nodenamebytes) < self.MAILSLOT_NODENAME_SIZE
            self.mm[index:index + len(nodenamebytes)] = nodenamebytes

    #----------------------------------------------------------------------
    # Called only by client.  mmap() the file, set hostname.  Many of the
    # parameters must be retrieved from the globals area of the mailbox.

    def _init_client_mailslot(self):
        buf = os.fstat(self.fd)
        assert STAT.S_ISREG(buf.st_mode), 'Mailbox FD is not a regular file'
        self.mm = mmap.mmap(self.fd, 0)
        self.nSlots = struct.unpack(
            'Q',
            self.mm[self.GLOBALS_NSLOTS_off:self.GLOBALS_NSLOTS_off + 8]
        )[0]
        assert 4 <= self.nSlots <= 64, 'nSlots is bad: %d' % self.nSlots
        self.server_id = self.nSlots - 1

        # mailbox slot starts with nodename
        self.clear_my_mailslot(nodenamebytes=self.nodename.encode())

###########################################################################


if __name__ == '__main__':
    mbox = FAMEZ_MailBox('/tmp/junk', 4)
