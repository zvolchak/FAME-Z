#!/usr/bin/python3

# The IVSHMEM protocol practiced by QEMU demands a memory-mappable file
# descriptor as part of the initial exchange, so give it one.  The mailbox
# is a shared common area.  In non-silent mode it's split into "nSlots"
# slots: slot 0 is a global read-only area populated below, the final
# entry "nSlots-1" is the server mailbox, and the middle "nSlots-2" entries
# are for that many clients.  Right now a mailbox is 512 byts: 128 bytes of
# metadata for a sent message, and 384 of message buffer.  famez.ko will
# read the global data to understand the mailbox layout.

# All numbers are unsigned long long (8 bytes).  All strings are multiples
# of 16 (including the C terminating NULL) on 16 byte boundaries.  Then it
# all looks good in "od -Ad -c" and even better in "od -Ad -c -tu8".

import mmap
import os
import struct

from pdb import set_trace

MAILBOX_SLOTSIZE = 512
MAILBOX_MAX_SLOTS = 64

def _populate_mailbox(fd, nSlots):
    mapped = mmap.mmap(fd, 0)
    data = b'\0' * (nSlots * MAILBOX_SLOTSIZE)      # Never very big
    mapped[0:len(data)] = data
    data = struct.pack('QQ', MAILBOX_SLOTSIZE, nSlots)
    mapped[0:len(data)] = data
    mapped.close()

def prepare_mailbox(path, nSlots=MAILBOX_MAX_SLOTS):
    '''Starts with mailbox base name, returns an fd to a populated file.'''

    size = nSlots * MAILBOX_SLOTSIZE
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
            STAT = os.path.stat         # for constants
            lstat = os.lstat(path)
            assert STAT.S_ISREG(lstat.st_mode), 'not a regular file'
            assert lstat.st_size >= size, \
                'existing size (%d) is < required (%d)' % (lstat.st_size, size)
            if lstat.st_gid != gr_gid and gr_gid > 0:
                print('Changing %s to group %s' % (path, gr_name))
                os.chown(path, -1, gr_gid)
            if lstat.st_mode & 0o660 != 0o660:  # at least
                print('Changing %s to permissions 666' % path)
                os.chmod(path, 0o666)
            fd = os.open(path, os.O_RDWR)
    except Exception as e:
        raise SystemExit('Problem with %s: %s' % (path, str(e)))

    os.umask(oldumask)
    _populate_mailbox(fd, nSlots)
    return fd

if __name__ == '__main__':
    fd = prepare_mailbox('/tmp/junk', 4)
    _populate_mailbox(fd, 4)
