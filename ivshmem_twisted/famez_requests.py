#!/usr/bin/python3

# Common routines for both server (switch) and clients which drill down
# on messages retrieved by parsing and generating a response.

import os
import sys

from collections import OrderedDict
from pprint import pprint

try:
    from famez_mailbox import FAMEZ_MailBox
except ImportError as e:
    from .famez_mailbox import FAMEZ_MailBox

###########################################################################
# Create a subroutine name out of the elements passed in.  Return
# the remainder.  Start with the least-specific construct.


def _unprocessed(client, *args, **kwargs):
    return False
    cmdlineargs = getattr(client, 'args', client.SI.args)
    SI = getattr(client, 'SI', False)
    if cmdlineargs.verbose and SI:
        SI.logmsg('NOOP', args, kwargs)
    return False


def chelsea(elements, verbose=0):
    entry = ''          # They begin with a leading '_', wait for it...
    G = globals()
    for i, e in enumerate(elements):
        e = e.replace('-', '_')         # Such as 'Link CTL Peer-Attribute'
        entry += '_%s' % e
        if verbose:
            print('Looking for %s()...' % entry, end='', file=sys.stderr)
        if entry in G:
            args = elements[i + 1:]
            if verbose:
                print('found it->%s' % str(args), file=sys.stderr)
            return G[entry], args
        if verbose:
            print('NOPE', file=sys.stderr)
    return _unprocessed, elements

###########################################################################


def CSV2dict(oneCSVstr):
    kv = {}
    elems = oneCSVstr.strip().split(',')
    for e in elems:
        try:
            KeV = e.strip().split('=')
            kv[KeV[0].strip()] = KeV[1].strip()
        except Exception as e:
            continue
    return kv

###########################################################################
# Might belong in famez_mailbox
# FIXME: make mailbox.fill() a class method and import the class to reach it.

_next_tag = 1

_tagged = OrderedDict()     # By tag, just store receiver now

def _send_response(peer, response, responder_id, responder_EN, tagged=False):
    global _next_tag

    if tagged:
        response += ',Tag=%d' % _next_tag
        _tagged[str(_next_tag)] = '%d.%d!%s' % (peer.SID, peer.CID, response)
        _next_tag += 1
    FAMEZ_MailBox.fill(responder_id, response)
    responder_EN.incr()
    return True     # FIXME: is there anything to detect?

###########################################################################
# Gen-Z 1.0 "6.8 Standalone Acknowledgment"
# Received by server/switch


def _Standalone_Acknowledgment(responder, args, responder_id, responder_EN):
    retval = True
    try:
        kv = CSV2dict(args[0])
        del _tagged[kv['Tag']]
    except Exception as e:
        print('UNTAGGING %d:%s FAILED' % (responder_id, responder.nodename),
            file=sys.stderr)
        retval = False
    if _tagged:
        print('Outstanding tags:', file=sys.stderr)
        pprint(_tagged, stream=sys.stderr)
    return retval


def _send_SA(responder, tag, reason, responder_id, responder_EN):
    response = 'Standalone Acknowledgment Tag=%s,Reason=%s' % (tag, reason)
    return _send_response(responder, response, responder_id, responder_EN)

###########################################################################
# Gen-Z 1.0 "11.11 Link CTL" subfield
# Sent by clients


def send_LinkACK(responder, details, responder_id, responder_EN, nack=False):
    if nack:
        response = 'Link CTL NAK %s' % details
    else:
        response = 'Link CTL ACK %s' % details
    return _send_response(responder, response, responder_id, responder_EN)

###########################################################################
# Gen-Z 1.0 "6.10.1 P2P Core..."
# Received by client, only really expecting RFC data


def _CTL_Write(responder, args, responder_id, responder_EN):
    kv = CSV2dict(args[0])
    if int(kv['Space']) != 0:
        return False
    responder.SID0 = int(kv['SID'])
    responder.CID0 = int(kv['CID'])
    responder.linkattrs['State'] = 'configured'
    return _send_SA(responder, kv['Tag'], 'OK', responder_id, responder_EN)

###########################################################################
# Gen-Z 1.0 "11.6 Link RFC"
# Received by switch


def _Link_RFC(responder, args, responder_id, responder_EN):
    if not responder.SI.args.smart:
        responder.SI.logmsg('I am not a manager')
        return False
    try:
        kv = CSV2dict(args[0])
        delay = kv['TTC'].lower()
    except (IndexError, KeyError) as e:
        responder.SI.logmsg('%d: Link RFC missing TTC' % responder.id)
        return False
    if not 'us' in delay:  # greater than cycle time of this server
        responder.SI.logmsg('Delay %s is too long, dropping request' % delay)
        return False
    responder.SID = responder.SI.default_SID  # Track the tag
    responder.CID = responder.id * 100
    response = 'CTL-Write Space=0,PFMSID=%d,PFMCID=%d,SID=%d,CID=%d' % (
        responder.SI.server_SID0, responder.SI.server_CID0,
        responder.SID, responder.CID)
    return _send_response(
        responder, response, responder_id, responder_EN, tagged=True)

###########################################################################
# Gen-Z 1.0 "11.11 Link CTL"
# Entered on both client and server responses.

def _Link_CTL(responder, args, responder_id, responder_EN):
    '''Subelements should be empty.'''
    arg0 = args[0] if len(args) else ''
    if len(args) == 1:
        if arg0 == 'Peer-Attribute':
            attrs = 'C-Class=%s,SID0=%d,CID0=%d' % (
                responder.SI.C_Class,
                responder.SID0,
                responder.CID0)
            return send_LinkACK(responder, attrs, responder_id, responder_EN)

    if arg0 == 'ACK' and len(args) == 2:
        # FIXME: correlation ala _tagged?  How do I know it's peer attrs?
        # FIXME: add a key to the response...
        responder.peerattrs = CSV2dict(args[1])
        return True

    if arg0 == 'NAK':
        # FIXME: do I track the sender ala _tagged and deal with it?
        print('Got a NAK, not sure what to do with it.', file=sys.stderr)
        return False

    responder.SI.logmsg('Got %s from %d' % (str(args), responder.id))
    return False

###########################################################################
# Finally a home


def _ping(responder, args, responder_id, responder_EN):
    return _send_response(responder, 'pong', responder_id, responder_EN)

###########################################################################
# Chained from actual EventReader callback in twisted_server.py.
# Commands streams are case-sensitive, read the spec.
# Return True if successfully parsed and processed.


def request_handler(request, responder, responder_id, responder_EN):
    elements = request.split()
    try:
        handler, args = chelsea(elements, responder.SI.args.verbose)
        return handler(responder, args, responder_id, responder_EN)
    except KeyError as e:
        responder.SI.logmsg('KeyError: %s' % str(e))
    except Exception as e:
        responder.SI.logmsg(str(e))
    return False

###########################################################################


if __name__ == '__main__':
    set_trace()
    pass
