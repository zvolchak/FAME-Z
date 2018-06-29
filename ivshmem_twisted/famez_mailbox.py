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

from os.path import stat as STAT     # for constants
from pdb import set_trace

class FAMEZ_MailBox(object):

    MAILBOX_MAX_SLOTS = 64
    MAILBOX_SLOT_SIZE = 512

    GLOBALS_SLOT_SIZE_OFFSET = 0    # in the space of mailslot 0
    GLOBALS_MESSAGE_OFFSET_OFFSET = 8
    GLOBALS_NSLOTS_OFFSET = 16

    MAILSLOT_NODENAME_OFFSET = 0
    MAILSLOT_NODENAME_SIZE = 32     # NULL padded
    MAILSLOT_MSGLEN_OFFSET = MAILSLOT_NODENAME_SIZE
    MAILSLOT_MESSAGE_OFFSET = 128   # To end of slot, currently 384 bytes
    MAILSLOT_MESSAGE_SIZE = 384

    #-----------------------------------------------------------------------
    # Globals at offset 0 (slot 0)
    # Server mailbox at slot (nSlots - 1), first 32 bytes are host name.

    def _populate(self):
        self.mm = mmap.mmap(self.fd, 0)

        # Empty it.  Simple code that's never too demanding on size
        data = b'\0' * (self.nSlots * self.MAILBOX_SLOT_SIZE)
        self.mm[0:len(data)] = data

        # Fill in the globals.  Must match the C struct in famez.ko.
        data = struct.pack('QQQ',
            self.MAILBOX_SLOT_SIZE,self. MAILSLOT_MESSAGE_OFFSET, self.nSlots)
        self.mm[0:len(data)] = data

        # My "hostname", zero-padded
        data = self.nodename.encode()
        index = (self.nSlots - 1) * self.MAILBOX_SLOT_SIZE
        self.mm[index:index + len(data)] = data

    #----------------------------------------------------------------------
    # Polymorphic.  Someday I'll learn about metaclasses.

    def __init__(self, pathORfd, nSlots=None, peer_id=-1, nodename=None):
        '''Server: Starts with string and slot count from command line.
           Client: starts with an fd and id.'''

        assert (self.MAILSLOT_MESSAGE_OFFSET + self.MAILSLOT_MESSAGE_SIZE
            == self.MAILBOX_SLOT_SIZE), 'Fix this NOW'

        if isinstance(pathORfd, int):
            assert peer_id > 0 and nodename is not None, 'Bad call, ump!'
            self.fd = pathORfd
            self.peer_id = peer_id
            self.nodename = nodename
            self._init_client_mailslot()
            return
        assert isinstance(pathORfd, str), 'Need a path OR open fd'
        path = pathORfd     # Match previously written code

        # One for global, one for server, 4 slots is max two clients
        assert 4 <= nSlots <= self.MAILBOX_MAX_SLOTS, 'Bad nSlots'

        size = nSlots * self.MAILBOX_SLOT_SIZE
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
        self.nodename = 'FAME-Z Server'
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
        # Returned a single element tuple that should be NUL-padded to end.
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

        index = slotnum * self.MAILBOX_SLOT_SIZE + self.MAILSLOT_MSGLEN_OFFSET
        self.mm[index:index + 8] = struct.pack('Q', msglen)
        index = slotnum * self.MAILBOX_SLOT_SIZE + self.MAILSLOT_MESSAGE_OFFSET
        self.mm[index:index + msglen] = msg
        self.mm[index + msglen] = 0	# The kernel will appreciate this :-)

    #----------------------------------------------------------------------
    # Called only by client.  mmap() the file, set hostname.

    def _init_client_mailslot(self):

        buf = os.fstat(self.fd)
        assert STAT.S_ISREG(buf.st_mode), 'Mailbox FD is not a regular file'
        assert 1 <= self.peer_id <= buf.st_size // self.MAILBOX_SLOT_SIZE, \
            'Slotnum %d is out of range' % peer_id
        self.mm = mmap.mmap(self.fd, 0)
        self.nSlots = struct.unpack(
            'Q',
            self.mm[self.GLOBALS_NSLOTS_OFFSET:self.GLOBALS_NSLOTS_OFFSET + 8]
        )[0]
        self.server_id = self.nSlots - 1

        # mailbox slot starts with nodename
        index = self.peer_id * self.MAILBOX_SLOT_SIZE
        zeros = b'\0' * self.MAILSLOT_NODENAME_SIZE
        self.mm[index:index + len(zeros)] = zeros
        tmp = self.nodename.encode()
        self.mm[index:index + len(tmp)] = tmp

###########################################################################


if __name__ == '__main__':
    mbox = FAMEZ_MailBox('/tmp/junk', 4)
