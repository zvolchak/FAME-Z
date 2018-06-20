#!/usr/bin/python3

# This work is licensed under the terms of the GNU GPL, version 2 or
# (at your option) any later version.  See the LICENSE file in the
# top-level directory.

# Rocky Craig <rocky.craig@hpe.com>

import argparse
import grp
import os
import sys

from daemonize import Daemonize

from ivshmsg_twisted import FactoryIVSHMSG

###########################################################################


def parse_cmdline(cmdline_args):
    '''cmdline_args does NOT lead with the program name.  Single-letter
       arguments reflect the stock "ivshmem-server".'''
    parser = argparse.ArgumentParser(
        description='FAME-Z server files and vector counts',
        epilog='Options reflect those in the QEMU "ivshmem-server".'
    )
    parser.add_argument('-?', action='help')  # -h and --help are built in
    parser.add_argument('--daemon', '-D',
        help='Run in background, log to file (default: foreground/stdout)',
        # The twisted module expectes the attribute 'foreground'...
        dest='foreground',
        action='store_false',   # ...so reverse the polarity, Scotty
        default=True
    )
    parser.add_argument('--logfile', '-L', metavar='<name>',
        help='Pathname of logfile for use in daemon mode',
        default='/tmp/famez_log'
    )
    parser.add_argument('--mailbox', '-M', metavar='<name>',
        help='Name of mailbox that exists in POSIX shared memory',
        default='famez_mailbox'
    )
    parser.add_argument('--nVectors', '-n', metavar='<integer>',
        help='Number of interrupt vectors per client (8 max)',
        default=4
    )
    parser.add_argument('--silent', '-s',
        help='Do NOT participate in EventFDs as another peer',
        action='store_true',
        default=False
    )
    parser.add_argument('--socketpath', '-S', metavar='/path/to/socket',
        help='Absolute path to UNIX domain socket (will be created)',
        default='/tmp/famez_socket'
    )
    parser.add_argument('--verbose', '-v',
        help='Specify multiple times to increase verbosity',
        default=0,
        action='count'
    )
    args = parser.parse_args(cmdline_args)

    # Idiot checking.
    args.nVectors = int(args.nVectors)
    assert 1 <= args.nVectors <= 8, 'nVectors not in range 1-8'
    assert not '/' in args.mailbox, 'mailbox cannot have slashes'
    assert not os.path.exists(args.socketpath), \
        'Remove %s' % args.socketpath

    # Mailbox is shared common area.  Each client gets 8k, max 15
    # clients, server area starts at zero, thus 128k.
    args.mailbox = '/dev/shm/' + args.mailbox
    return args

###########################################################################


def prepare_mailbox(abspath):
    '''Starts with mailbox file name, returns an fd to open file.'''
    oldumask = os.umask(0)
    gr_name = 'libvirt-qemu'
    try:
        gr_gid = grp.getgrnam(gr_name).gr_gid
        if not os.path.isfile(abspath):
            fd = os.open(abspath, os.O_RDWR | os.O_CREAT, mode=0o666)
            os.posix_fallocate(fd, 0, 131072)
            os.fchown(fd, -1, gr_gid)
        else:   # Re-condition and re-use
            STAT = os.path.stat         # for constants
            lstat = os.lstat(abspath)
            assert STAT.S_ISREG(lstat.st_mode), 'not a regular file'
            assert lstat.st_size >= 131072, 'size is < 128k'
            if lstat.st_gid != gr_gid:
                print('Changing %s to group %s' % (abspath, gr_name))
                os.chown(abspath, -1, gr_gid)
            if lstat.st_mode & 0o660 != 0o660:  # at least
                print('Changing %s to permissions 666' % abspath)
                os.chmod(abspath, 0o666)
            fd = os.open(abspath, os.O_RDWR)
    except Exception as e:
        raise SystemExit('Problem with %s: %s' % (abspath, str(e)))

    os.umask(oldumask)
    return fd

###########################################################################
# MAIN


def forever(cmdline_args=None):
    if cmdline_args is None:
        cmdline_args = sys.argv[1:]  # When being explicit, strip prog name
    try:
        args = parse_cmdline(cmdline_args)
        args.mailbox_fd = prepare_mailbox(args.mailbox)
    except Exception as e:
        raise SystemExit(str(e))
    set_trace()
    if not args.foreground:
        raise NotImplementedError('Gotta run it in the foreground for now')
        if args.verbose:
            print(Daemonize.__doc__)    # The website is WRONG
        d = Daemonize('famez_server', '/dev/null', None, auto_close_fds=None)
        d.start()
    server = FactoryIVSHMSG(args)
    server.run()

###########################################################################


if __name__ == '__main__':
    from pdb import set_trace
    forever()

