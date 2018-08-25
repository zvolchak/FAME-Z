// Only the beginning

#ifndef GENZ_ROUTING_FABRIC_DOT_H
#define GENZ_ROUTING_FABRIC_DOT_H

#include <linux/list.h>
#include <linux/mutex.h>

// Minimum proscribed data structures are listed in
// Gen-Z 1.0 "8.13.1 Grouping: Baseline Structures" and 
//           "8.13.2 Grouping: Routing/Fabric Structures"
// Definitions below ending in "_structure" are merely pertinent fields.
// Those ending in "_format" are the packed binary layout.

// Gen-Z 1.0 "8.29 Component Destination Table Structure"
struct genz_component_destination_table_structure {
	int HiMom;
};

// Gen-Z 1.0 "8.xx Single-Subnet Destination Table Structure"
struct genz_single_subnet_destination_table_structure {
	int HiMom;
};


#endif
