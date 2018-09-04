// Link-level messages, mostly from the switch (IVSHMSG server).
// It may hijack and finish off the message.

#include "famez.h"

// See famez_requests.py:_Link_CTL(), etc for required formats.
// I'm skipping the tracker FZT for now.

#define LINK_CTL_PEER_ATTRIBUTE \
	"Link CTL Peer-Attribute"

#define LINK_CTL_ACK \
	"Link CTL ACK C-Class=%s,SID0=%d,CID0=%d"

#define CTL_WRITE_0_SID_CID \
	"CTL-Write Space=0,PFMSID=%d,PFMCID=%d,SID=%d,CID=%d,Tag=%d"

#define STANDALONE_ACKNOWLEDGMENT \
	"Standalone Acknowledgment Tag=%d,Reason=OK"

//-------------------------------------------------------------------------
// This is called in interrupt context with the incoming_slot->lock held.

irqreturn_t famez_link_request(struct famez_mailslot __iomem *incoming_slot,
			       struct famez_adapter *adapter)
{
	uint32_t PFMSID, PFMCID, SID, CID, tag;
	char outbuf[128];

	// These are all fixed values now, but someday...
	incoming_slot->peer_SID = FAMEZ_SID_DEFAULT;
	incoming_slot->peer_CID = incoming_slot->peer_id * 100;

	// Simple proof-of-life, must be an exact match.
	if (incoming_slot->buflen == 4 &&
	    STREQ_N(incoming_slot->buf, "ping", 4)) {
		incoming_slot->buflen = 0;	// buf received
		spin_unlock(&(adapter->incoming_slot_lock));
		famez_create_outgoing(
			incoming_slot->peer_id,
			FAMEZ_SID_CID_IS_PEER_ID,
			"pong", 4,
			adapter);
		return IRQ_HANDLED;
	}

	if (STREQ_N(incoming_slot->buf, LINK_CTL_PEER_ATTRIBUTE,
		strlen(LINK_CTL_PEER_ATTRIBUTE))) {
		incoming_slot->buflen = 0;	// buf received
		spin_unlock(&(adapter->incoming_slot_lock));
		sprintf(outbuf, LINK_CTL_ACK,
			adapter->core->Base_C_Class_str,
			adapter->core->SID0,
			adapter->core->CID0);
		famez_create_outgoing(
			incoming_slot->peer_id,
			FAMEZ_SID_CID_IS_PEER_ID,
			outbuf, strlen(outbuf),
			adapter);
		return IRQ_HANDLED;
	}

	if (sscanf(incoming_slot->buf, CTL_WRITE_0_SID_CID,
		   &PFMSID, &PFMCID, &SID, &CID, &tag) == 5) {
		incoming_slot->buflen = 0;	// buf received
		spin_unlock(&(adapter->incoming_slot_lock));
		adapter->core->PFMSID = PFMSID;
		adapter->core->PFMCID = PFMCID;
		adapter->core->SID0 = SID;
		adapter->core->CID0 = CID;
		adapter->core->PMCID = -1;
		sprintf(outbuf, STANDALONE_ACKNOWLEDGMENT, tag);
		famez_create_outgoing(
			incoming_slot->peer_id,
			FAMEZ_SID_CID_IS_PEER_ID,
			outbuf, strlen(outbuf),
			adapter);
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}
