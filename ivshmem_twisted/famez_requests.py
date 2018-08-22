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
    if client.SI.args.verbose:
        client.SI.logmsg('NOOP', args, kwargs)
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
# Here instead of famez_mailbox to manage the tag.  Can be called as a
# "discussion initiator" usually from the REPL interpreters, or as a
# response to a received command from the callbacks.

_next_tag = 1

_tagged = OrderedDict()     # By tag, just store receiver now


def send_payload(peer, response,
        sender_id=None, sender_EN=None, tag=None):
    global _next_tag

    if sender_id is None:   # Not currently used, responder_id is pre-filled
        sender_id = peer.responder_id
    if sender_EN is None:   # Ditto
        sender_EN = peer.responder_EN
    if tag is not None:     # zero-length string can trigger this
        response += ',Tag=%d' % _next_tag
        _tagged[str(_next_tag)] = '%d.%d!%s|%s' % (
            peer.SID0, peer.CID0, response, tag)
        _next_tag += 1
    FAMEZ_MailBox.fill(sender_id, response)
    sender_EN.incr()
    return True     # FIXME: is there anything to detect?

###########################################################################
# Gen-Z 1.0 "6.8 Standalone Acknowledgment"
# Received by server/switch


def _Standalone_Acknowledgment(responder, args):
    retval = True
    tag = False
    try:
        kv = CSV2dict(args[0])
        stamp, tag = _tagged[kv['Tag']].split('|')
        del _tagged[kv['Tag']]
        tag = tag.strip()
        kv = CSV2dict(tag)
    except KeyError as e:
        responder.SI.trace('UNTAGGING %d:%s FAILED' %
            (responder.responder_id, responder.nodename))
        retval = False
        kv = {}

    afterACK = kv.get('AfterACK', False)
    if afterACK:
        send_payload(responder, afterACK)

    if _tagged:
        print('Outstanding tags:', file=sys.stderr)
        pprint(_tagged, stream=sys.stderr)
    return retval


def _send_SA(responder, tag, reason):
    response = 'Standalone Acknowledgment Tag=%s,Reason=%s' % (tag, reason)
    return send_payload(responder, response)

###########################################################################
# Gen-Z 1.0 "11.11 Link CTL" subfield
# Sent by clients


def send_LinkACK(responder, details, nack=False):
    if nack:
        response = 'Link CTL NAK %s' % details
    else:
        response = 'Link CTL ACK %s' % details
    return send_payload(responder, response)

###########################################################################
# Gen-Z 1.0 "6.10.1 P2P Core..."
# Received by client, only really expecting RFC data


def _CTL_Write(responder, args):
    kv = CSV2dict(args[0])
    if int(kv['Space']) != 0:
        return False
    responder.SID0 = int(kv['SID'])
    responder.CID0 = int(kv['CID'])
    responder.linkattrs['State'] = 'configured'
    return _send_SA(responder, kv['Tag'], 'OK')

###########################################################################
# Gen-Z 1.0 "11.6 Link RFC"
# Received by switch


def _Link_RFC(responder, args):
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
    response = 'CTL-Write Space=0,PFMSID=%d,PFMCID=%d,SID=%d,CID=%d' % (
        responder.SI.server_SID0, responder.SI.server_CID0,
        responder.SID0, responder.CID0)
    return send_payload(
        responder, response, tag='AfterACK=Link CTL Peer-Attribute')

###########################################################################
# Gen-Z 1.0 "11.11 Link CTL"
# Entered on both client and server responses.

def _Link_CTL(responder, args):
    '''Subelements should be empty.'''
    arg0 = args[0] if len(args) else ''
    if len(args) == 1:
        if arg0 == 'Peer-Attribute':
            SID0 = responder.SID0
            CID0 = responder.CID0
            attrs = 'C-Class=%s,SID0=%d,CID0=%d' % (
                responder.SI.C_Class, SID0, CID0)
            return send_LinkACK(responder, attrs)

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


def _ping(responder, args):
    return send_payload(responder, 'pong')

###########################################################################
# Chained from actual EventReader callback in twisted_server.py.
# Commands streams are case-sensitive, read the spec.
# Return True if successfully parsed and processed.


def handle_request(request, requester_name, responder):
    trace = '\n%10s@%d->"%s"' % (
        requester_name, responder.requester_id, request)
    responder.SI.trace(trace)

    elements = request.split()
    try:
        handler, args = chelsea(elements, responder.SI.args.verbose)
        return handler(responder, args)
    except KeyError as e:
        responder.SI.logmsg('KeyError: %s' % str(e))
    except Exception as e:
        responder.SI.logmsg(str(e))
    return False
