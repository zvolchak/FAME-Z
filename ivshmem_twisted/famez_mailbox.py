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
# of 16 (including the C terminating NULL) on 16 byte boundaries.  Then it
# all looks good in "od -Ad -c" and even better in "od -Ad -c -tu8".

import mmap
import os
import struct
from time import sleep as SLEEP
from time import time as NOW

from os.path import stat as STAT     # for constants
from pdb import set_trace

class FAMEZ_MailBox(object):

    MAILBOX_MAX_SLOTS = 64
    MAILBOX_SLOT_SIZE = 256         # 128 of metadata, 128 of message

    GLOBALS_SLOT_SIZE_OFFSET = 0    # in the space of mailslot 0
    GLOBALS_MESSAGE_OFFSET_OFFSET = 8
    GLOBALS_NSLOTS_OFFSET = 16

    # Metadata currently takes up 32 + 8 + 4 = 44 bytes
    MAILSLOT_NODENAME_OFFSET = 0
    MAILSLOT_NODENAME_SIZE = 32     # NULL padded
    MAILSLOT_MSGLEN_OFFSET = MAILSLOT_NODENAME_SIZE         # uint64_t, so...
    MAILSLOT_PEER_ID_OFFSET = MAILSLOT_MSGLEN_OFFSET + 8    # uint32_t, so...
    MAILSLOT_NEXT_BLAHBLAH = MAILSLOT_PEER_ID_OFFSET + 4
    MAILSLOT_MESSAGE_OFFSET = 128
    MAILSLOT_MESSAGE_SIZE = 128

    #-----------------------------------------------------------------------
    # Globals at offset 0 (slot 0)
    # Each slot (1 through nSlots-1) has a peer_id
    # Server mailbox at slot (nSlots - 1): first 32 bytes are host name.

    def _populate(self):
        self.mm = mmap.mmap(self.fd, 0)

        # Empty it.  Simple code that's never too demanding on size
        data = b'\0' * (self.nSlots * self.MAILBOX_SLOT_SIZE)
        self.mm[0:len(data)] = data

        # Fill in the globals.  Must match the C struct in famez.ko.
        data = struct.pack('QQQ',
            self.MAILBOX_SLOT_SIZE,self. MAILSLOT_MESSAGE_OFFSET, self.nSlots)
        self.mm[0:len(data)] = data

        # Set the peer_id for each slot as a C integer.  While python client
        # can discern a sender, the famez.ko driver needs it "inband".

        for slot in range(1, 8):
            index = slot * self.MAILBOX_SLOT_SIZE + self.MAILSLOT_PEER_ID_OFFSET
            packed_peer = struct.pack('i', slot)   # uint32_t
            self.mm[index:index + 4] = packed_peer

        # My "hostname", zero-padded
        data = self.nodename.encode()
        index = (self.nSlots - 1) * self.MAILBOX_SLOT_SIZE
        self.mm[index:index + len(data)] = data

    #----------------------------------------------------------------------
    # Polymorphic.  Someday I'll learn about metaclasses.

    def __init__(self, pathORfd, nSlots=None, client_id=-1, nodename=None):
        '''Server: Starts with string and slot count from command line.
           Client: starts with an fd and id.'''

        assert (self.MAILSLOT_MESSAGE_OFFSET + self.MAILSLOT_MESSAGE_SIZE
            == self.MAILBOX_SLOT_SIZE), 'Fix this NOW'

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
        size = self.MAILBOX_MAX_SLOTS * self.MAILBOX_SLOT_SIZE
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

    def pickup_from_slot(self, slotnum, asbytes=False):
        '''Return the nodename and message.'''
        assert 1 <= slotnum <= self.server_id, \
            'Slotnum is out of domain 1 - %d' % (self.server_id)
        index = slotnum * self.MAILBOX_SLOT_SIZE     # start of nodename
        nodename, msglen = struct.unpack('32sQ', self.mm[index:index + 40])
        nodename = nodename.split(b'\0', 1)[0].decode()
        index += self.MAILSLOT_MESSAGE_OFFSET
        fmt = '%ds' % msglen
        msg = struct.unpack(fmt, self.mm[index:index + msglen])

        # The message is copied so mark the mailslot length zero as permission
        # for the requester to send another message (not necessarily to me).

        index = slotnum * self.MAILBOX_SLOT_SIZE + self.MAILSLOT_MSGLEN_OFFSET
        self.mm[index:index + 8] = struct.pack('Q', 0)

        # Clean up the message copyout, which is a single element tuple
        # that should be NUL-padded to end.
        msg = msg[0].split(b'\0', 1)[0]
        if not asbytes:
            msg = msg.decode()
        return nodename, msg

    #----------------------------------------------------------------------

    def place_in_slot(self, slotnum, msg):
        assert 1 <= slotnum <= self.server_id, \
            'Slotnum is out of domain 1 - %d' % (self.server_id)
        if isinstance(msg, str):
            msg = msg.encode()
        assert isinstance(msg, bytes), 'msg must be string or bytes'
        msglen = len(msg)   # It's bytes now
        assert msglen < self.MAILSLOT_MESSAGE_SIZE, 'Message too long'

        # The previous responder needs to clear the msglen to indicate it
        # has pulled the message out of the sender's mailbox.
        index = slotnum * self.MAILBOX_SLOT_SIZE + self.MAILSLOT_MSGLEN_OFFSET
        stop = NOW() + 1.2
        while NOW() < stop and struct.unpack('Q', self.mm[index:index+8])[0]:
            print('psuedo-HW not ready to send')
            SLEEP(0.1)

        self.mm[index:index + 8] = struct.pack('Q', msglen)
        index = slotnum * self.MAILBOX_SLOT_SIZE + self.MAILSLOT_MESSAGE_OFFSET
        self.mm[index:index + msglen] = msg
        self.mm[index + msglen] = 0	# The kernel will appreciate this :-)

    #----------------------------------------------------------------------
    # Called by Python client on graceful shutdowns, and always by server
    # when a peer dies.  This is mostly for QEMU crashes so the nodename
    # is not accidentally when a QEMU restarts, before loading famez.ko.

    def clear_my_mailslot(self, nodenamebytes=None, override_id=None):
        id = override_id if override_id else self.my_id
        assert 1 <= id <= self.server_id, 'slot is bad: %d' % id
        index = id * self.MAILBOX_SLOT_SIZE
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
            self.mm[self.GLOBALS_NSLOTS_OFFSET:self.GLOBALS_NSLOTS_OFFSET + 8]
        )[0]
        assert 4 <= self.nSlots <= 64, 'nSlots is bad: %d' % self.nSlots
        self.server_id = self.nSlots - 1

        # mailbox slot starts with nodename
        self.clear_my_mailslot(nodenamebytes=self.nodename.encode())

###########################################################################


if __name__ == '__main__':
    mbox = FAMEZ_MailBox('/tmp/junk', 4)
