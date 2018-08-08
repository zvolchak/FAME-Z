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

from ivshmem_twisted.twisted_server import FactoryIVSHMSGServer

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
    parser.add_argument('--nSlots', '-n', metavar='<integer>',
        help='Number of actor slots; max peer count is two fewer',
        default=4
    )
    parser.add_argument('--recycle', '-r',
        help='On client death do not advise peers; reuse eventfds on respawn',
        action='store_true',
        default=False
    )
    parser.add_argument('--silent', '-s',
        help='Do NOT participate in EventFDs/mailbox as another peer',
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

    # Positional arguments
    parser.add_argument('SID', metavar='<SubnetID>',
        help='Subnet ID overseen by this server',
        type=int,
        nargs=1,
    )

    # Generate the object and postprocess some of the fields.
    args = parser.parse_args(cmdline_args)
    args.SID = args.SID[0]          # nargs parser returns a list
    args.nSlots = int(args.nSlots)
    assert not '/' in args.mailbox, 'mailbox cannot have slashes'
    assert not os.path.exists(args.socketpath), 'Remove %s' % args.socketpath

    return args

###########################################################################
# MAIN


def forever(cmdline_args=None):
    if cmdline_args is None:
        cmdline_args = sys.argv[1:]  # When being explicit, strip prog name
    try:
        args = parse_cmdline(cmdline_args)
    except Exception as e:
        raise SystemExit(str(e))

    if not args.foreground:
        raise NotImplementedError('Gotta run it in the foreground for now')
        if args.verbose:
            print(Daemonize.__doc__)    # The website is WRONG
        d = Daemonize('famez_server', '/dev/null', None, auto_close_fds=None)
        d.start()
    server = FactoryIVSHMSGServer(args)
    server.run()

###########################################################################


if __name__ == '__main__':
    forever()

