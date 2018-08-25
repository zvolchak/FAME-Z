// Only the beginning

#ifndef GENZ_BASELINE_DOT_H
#define GENZ_BASELINE_DOT_H

#include <linux/list.h>
#include <linux/mutex.h>

// Minimum proscribed data structures are listed in
// Gen-Z 1.0 "8.13.1 Grouping: Baseline Structures" and 
//           "8.13.2 Grouping: Routing/Fabric Structures"
// Definitions below ending in "_structure" are merely pertinent fields.
// Those ending in "_format" are the packed binary layout.

// Gen-Z 1.0 "8.14 Core Structure"

#define GENZ_CORE_STRUCTURE_ALLOC_COMP_DEST_TABLE	0x0001
#define GENZ_CORE_STRUCTURE_ALLOC_ALL			0xffff

struct genz_core_structure {
	int32_t CID0, SID0,	// 0 if unassigned, -1 if unused
	    PMCID,		// If I am the fabric manager
	    PFMCID, PFMSID,	// If someone else is the FM
	    SFMCID, SFMSID;
	struct genz_component_destination_table_structure *comp_dest_table;
};

// Gen-Z 1.0 "8.15 Opcode Set Structure"
struct genz_opcode_set_structure {
	int HiMom;
};

// Gen-Z 1.0 "8.16 Interface Structure"
struct genz_interface_structure {
	uint32_t Version, InterfaceID,
		 HVS, HVE,
		 I_Status,
		 PeerIntefaceID,
		 PeerBaseC_Class,
		 PeerCID, PeerSID,
		 PeerState;
};

struct genz_core_structure *genz_core_structure_create(uint64_t flags);
void genz_core_structure_destroy(struct genz_core_structure *);

#endif
