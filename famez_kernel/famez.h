// Cuz it needs one

#ifndef FAMEZ_DOT_H
#define FAMEZ_DOT_H

#define FAMEZ_NAME	"famez"
#define FZ		"famez: "	// pr_info header
#define FZSP		"       "	// pr_info header same length

// For PCI search
#define IVSHMEM_VENDOR	0x1af4	// RedHat
#define IVSHMEM_DEVICE	0x1110

// When stable, git commit, then git tag, then commit again (for the tag)
#define FAMEZ_VERSION	"famez v0.7.1: newfangled probe and interrupts"

#include <linux/delay.h>
#include <linux/configfs.h>
#include <linux/pci.h>

#define STREQ(s1, s2) (!strcmp(s1, s2))
#define STARTS(s1, s2) (!strncmp(s1, s2, strlen(s2)))

#define FAMEZ_MAX_CLIENTS	63	// + 1 for server == power of 2
#define FAMEZ_PEER_SERVER	0

#define FAMEZ_DEBUG			// See "Debug assistance" below

struct ivshmem_registers {		// BAR 0
	uint32_t	Rev1Reserved1,	// Rev 0: Interrupt mask
			Rev1Reserved2,	// Rev 0: Interrupt status
			IVPosition,	// My peer id
			Doorbell;	// Upper and lower half
};

struct ivshmem_msi_x_msi_pba {		// BAR 1: Not sure if this is needed?
	uint32_t junk;
};

// There are a power-of-two number of mailbox slots.  Slot 0 is reserved
// for global data; it's easy to find :-) and server ID 0 doesn't seem to
// work in the doorbell.  The last slot (with ID == nSlots - 1) is for the
// server.  The remaining slots are for client IDs 1 - (nSlots -2).

struct famez_globals {			// BAR 2: Start of IVSHMEM
	uint64_t slotsize, msg_offset, nSlots;
};

struct famez_mailslot {
	char nodename[32];
	uint64_t msglen;
	// padding in here, calculate at runtime...
	char *msg;				// ...via globals->msg_offset
};

// The IVSHMEM "vector" will map to an MSI-X "entry" value.  It is
// the lower 16 bits.  The combo must be assigned atomically.
union __attribute__ ((packed)) ringer {
	struct { uint16_t vector, peer; };
	uint32_t push;
};

struct famez_configuration {
	struct pci_dev *pci_dev;
	uint64_t max_msglen;				// Currently 384
	uint16_t my_id, server_id;			// match ringer.peer 
	struct ivshmem_registers __iomem *regs;		// BAR0
	struct ivshmem_msi_x_msi_pba __iomem *msix;	// BAR1
	struct famez_globals __iomem *globals;		// BAR2
	struct famez_mailslot *my_slot;			// Last slot of BAR2
	struct msix_entry *msix_entries;		// kzalloc an array
};

//-------------------------------------------------------------------------
// famez_base.c - globals from insmod parameters, then routines

extern int famez_verbose;

//-------------------------------------------------------------------------
// famez_config.c - early setup and late teardown of things

int famez_config(struct famez_configuration *config);
void famez_unconfig(struct famez_configuration *config);
int famez_sendmsg(uint32_t , char *, ssize_t, struct famez_configuration *);

//-------------------------------------------------------------------------
// famez_MSI-X.c

int famez_MSIX_setup(struct famez_configuration *, struct pci_dev *);
void famez_MSIX_teardown(struct famez_configuration *);

//-------------------------------------------------------------------------
// Debug assistance

#ifdef FAMEZ_DEBUG
#define PR_V1(a...)	{ if (famez_verbose) pr_info(a); }
#define PR_V2(a...)	{ if (famez_verbose > 1) pr_info(a); }
#define PR_V3(a...)	{ if (famez_verbose > 2) pr_info(a); }
#else
#define PR_V1(a...)
#define PR_V2(a...)
#define PR_V3(a...)
#endif

#define _F_		__FUNCTION__
#define PR_ENTER(a...)	{ if (famez_verbose) { \
				pr_info("r2h: enter %s: ", _F_); pr_cont(a); }}
#define PR_EXIT(a...)	{ if (famez_verbose) { \
				pr_info("r2h: exit %s: ", _F_); pr_cont(a); }}

#define PR_SLEEPMS(_txt, _ms) { pr_info("r2h: " _txt); msleep(_ms); }

//-------------------------------------------------------------------------
// During callgraph generation, "flipping" these values will create a
// more detailed map.  Otherwise use normal/idiot-proofing/performant values.

#ifdef CALLGRAPH
#define STATIC		
#define NOINLINE	noinline
#else
#define STATIC		static
#define NOINLINE
#endif

#endif
