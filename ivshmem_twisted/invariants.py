###########################################################################
# Cleaner than class variables, allows code reuse, and also leverages
# support "downstream" for interrupt handling.

try:
    from ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader
except ImportError as e:
    from .ivshmem_eventfd import ivshmem_event_notifier_list, EventfdReader

###########################################################################


class ServerInvariant(object):

    def __init__(self, args=None):
        if args is None:        # Called from client, filled in gradually
            self.nClients = 0   # Total peers including me, excluding server
            self.nEvents = 0
            self.server_id = 0
            return

        self.logmsg = args.logmsg    # Often-used
        self.logerr = args.logerr
        self.nClients = args.nClients
        self.server_id = args.nClients + 1   # This is me!
        self.nEvents = args.nClients + 2
        self.mailbox = args.mailbox
        self.defaultSID = 27
        if args.smart:
            self.server_SID0 = self.defaultSID
            self.server_CID0 = self.server_id * 100
        else:
            self.server_SID0 = 0
            self.server_CID0 = 0
        self.recycled = {}

        # It's really "clients" (QEMU and [famez|ivshmem]_client) but
        # the original code is written around a "peer_list".  Keep that
        # convention for easier comparisons.  An OrderedDict would be
        # bigger help but would bend that code-following convention.
        self.peer_list = []

        # Non-standard addition to IVSHMEM server role: this server can be
        # interrupted and messaged to particpate in client activity.
        # It will get looped even if it's empty (silent mode).
        self.notifiers = []

        # Usually create eventfds for receiving messages in IVSHMSG and
        # set up a callback.  This early arming is not a race condition
        # as the peer for which this is destined has not yet been told
        # of the fds it would use to trigger here.

        if not args.silent and not self.notifiers:
            self.notifiers = ivshmem_event_notifier_list(self.nEvents)
            # self is really just a way to get to the server singletons.
            # The actual client doing the sending needs to be fished out
            # via its "num" vector.
            for i, N in enumerate(self.notifiers):
                N.num = i
                tmp = EventfdReader(N, self.ServerCallback, self)
                if i:   # Technically it blocks mailslot 0, the globals
                    tmp.start()

