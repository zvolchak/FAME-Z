// Only the beginning

#ifndef GENZ_BASELINE_DOT_H
#define GENZ_BASELINE_DOT_H

#include <linux/device.h>
#include <linux/list.h>

#define DRV_NAME	"Gen-Z"
#define DRV_VERSION	"0.1"

#define GZNAMFMTSIZ	64

#define __unused __attribute__ ((unused))

#define GENZ_CCE_DISCRETE_BRIDGE	0x14

struct genz_private {			// Just following netdev
	int junk;
};

struct genz_device {
	char namefmt[GZNAMFMTSIZ];
	struct list_head lister;
	uint64_t flags;
	struct device dev;		// for to_genz_dev
	struct genz_priv *priv;
};
#define to_genz_dev(pPp) container_of(pPp, struct genz_device, dev)

struct genz_device_ops {
	int (*init)(struct genz_device *genz_dev);
	void (*uninit)(struct genz_device *genz_dev);
};

// Minimum proscribed data structures are listed in
// Gen-Z 1.0 "8.13.1 Grouping: Baseline Structures" and 
//           "8.13.2 Grouping: Routing/Fabric Structures"
// Definitions below ending in "_structure" are merely pertinent fields.
// Those ending in "_format" are the packed binary layout.

// Gen-Z 1.0 "8.14 Core Structure"

#define GENZ_CORE_STRUCTURE_ALLOC_COMP_DEST_TABLE	0x0001
#define GENZ_CORE_STRUCTURE_ALLOC_ALL			0xffff

struct genz_core_structure {
	char Base_C_Class_str[32];
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

//-------------------------------------------------------------------------
// genz_bus.c

extern struct bus_type genz_bus;

//-------------------------------------------------------------------------
// genz_class.c

int genz_classes_init(void);
void genz_classes_destroy(void);

// EXPORTed

extern struct class *genz_class_getter(unsigned);

#endif
