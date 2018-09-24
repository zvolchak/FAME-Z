// Only the beginning

#ifndef GENZ_BASELINE_DOT_H
#define GENZ_BASELINE_DOT_H

#include <linux/device.h>
#include <linux/list.h>

#define DRV_NAME	"Gen-Z"
#define DRV_VERSION	"0.1"

#define GZNAMFMTSIZ	64

#define __unused __attribute__ ((unused))

/**
  * Gen-Z 1.0 Appendix C, Component Class Encodings.
  * NB == Non-Bootable
  * NC == Non-Coherent
  */

enum genz_component_class_encodings {
	GENZ_CCE_RESERVED_SHALL_NOT_BE_USED = 0x0,
	GENZ_CCE_MEMORY_P2P_CORE,
	GENZ_CCE_MEMORY_EXPLICIT_OPCLASS,
	GENZ_CCE_INTEGRATED_SWITCH,
	GENZ_CCE_ENC_EXP_SWITCH,
	GENZ_CCE_FABRIC_SWITCH,
	GENZ_CCE_PROCESSOR,
	GENZ_CCE_PROCESSOR_NB,
	GENZ_CCE_ACCELERATOR_NB_NC = 0x8,
	GENZ_CCE_ACCELERATOR_NB,
	GENZ_CCE_ACCELERATOR_NC,
	GENZ_CCE_ACCELERATOR,
	GENZ_CCE_IO_NB_NC,
	GENZ_CCE_IO_NB,
	GENZ_CCE_IO_NC,
	GENZ_CCE_IO,
	GENZ_CCE_BLOCK_STORAGE = 0x10,
	GENZ_CCE_BLOCK_STORAGE_NB,
	GENZ_CCE_TRANSPARENT_ROUTER,
	GENZ_CCE_MULTI_CLASS,
	GENZ_CCE_DISCRETE_BRIDGE,
	GENZ_CCE_INTEGRATED_BRIDGE = 0x15,
	GENZ_CCE_TOO_BIG,
};

struct genz_device {
	char namefmt[GZNAMFMTSIZ];
	struct list_head lister;
	uint64_t flags;
	struct device dev;
	void *private_data;
};
#define to_genz_dev(pDeV) container_of(pDeV, struct genz_device, dev)

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

enum genz_core_structure_optional_substructures {
	GENZ_CORE_STRUCTURE_ALLOC_COMP_DEST_TABLE =	1 << 0,
	GENZ_CORE_STRUCTURE_ALLOC_XYZZY_TABLE =		1 << 1,
	GENZ_CORE_STRUCTURE_ALLOC_ALL =			(1 << 2) - 1
};

struct genz_core_structure {
	unsigned CCE;
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

struct device *genz_find_bus_by_instance(int);

//-------------------------------------------------------------------------
// genz_class.c

int genz_classes_init(void);
void genz_classes_destroy(void);

// EXPORTed

extern struct class *genz_class_getter(unsigned);

#endif
