#!/usr/bin/python3

# Message has already been retrieved

from pdb import set_trace

def _switch_link(selph, elems):
    if not elems[0].startswith('ttcus='):
        selph.logmsg('%d: Link RFC missing TTCuS' % selph.id)
        return False
    try:
        uS = int(elems[1].split('=')[1])
    except Exception as e:
        uS = 999999
    if uS > 1000:  # 1 ms, about the cycle time of this server
        selph.logmsg('Delay == %duS, dropping request' % uS)
    else:
        selph.mailbox.fill(
            selph.my_id,
            'CtrlWrite:SID=%d,CID=%d' % (selph.SID, selph.CID))
        selph.vectors[selph.my_id].incr()
    return True

###########################################################################


def _no_sub(selph, nodename, msg):
    selph.logmsg('Message was unprocessed')
    return False

###########################################################################
# Chained from actual EventReader callback in twisted_server.py.


def switch_handler(selph, nodename, msg):
    '''Return True if successfully parsed and processed.'''

    elems = msg.lower.split(':')
    subhandler = globals().get('_switch_' + elems[0], _no_sub)
    return subhandler(selph, elems[1:])

###########################################################################


if __name__ == '__main__':
    set_trace()
    pass
