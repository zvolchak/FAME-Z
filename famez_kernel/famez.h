// Cuz it needs one

#ifndef FAMEZ_DOT_H
#define FAMEZ_DOT_H

#include <linux/list.h>

#define FAMEZ_NAME	"famez"
#define FZ		"famez: "	// pr_xxxx header
#define FZSP		"       "	// pr_xxxx header same length indent

#define FAMEZ_VERSION	FAMEZ_NAME " v0.7.3: early chardev work"

#define FAMEZ_DEBUG			// See "Debug assistance" below

struct ivshmem_registers {		// BAR 0
	uint32_t	Rev1Reserved1,	// Rev 0: Interrupt mask
			Rev1Reserved2,	// Rev 0: Interrupt status
			IVPosition,	// My peer id
			Doorbell;	// Upper and lower half
};

struct ivshmem_msi_x_table_pba {	// BAR 1: Not mapped, not used.  YET.
	uint32_t junk;
};

// The famez_server.py controls the mailbox slot size and number of slots
// (and therefore the total file size).  It gives these numbers to this driver.
// There are always a power-of-two number of mailbox slots, indexed by IVSHMSG
// client ID.  Slot 0 is reserved for global data; it's easy to find :-); 
// besides, ID 0 doesn't seem to work in the doorbell.  The last slot (with 
// ID == nSlots - 1) is for the Python server.  The remaining slots are for
// client IDs 1 through (nSlots - 2).

struct famez_globals {			// BAR 2: Start of IVSHMEM
	uint64_t slotsize, msg_offset, nSlots;
};

struct famez_mailslot {
	char nodename[32];		// of the owning client
	uint64_t msglen;
	// padding in here, calculate at runtime...
	char *msg;			// ...via globals->msg_offset
};

// The IVSHMEM "vector" will map to an MSI-X "entry" value.  It is
// the lower 16 bits.  The combo must be assigned atomically.
union __attribute__ ((packed)) ringer {
	struct { uint16_t vector, peer; };
	uint32_t push;
};

struct famez_configuration {
	struct list_head lister;
	atomic_t nr_users;				// User-space actors
	struct pci_dev *pdev;				// Paranoid reverse ptr
	void *teardown_lookup;				// Convenience backpointer
	uint64_t max_msglen;
	char *scratch_msg;				// kmalloc(max_msglen)
	uint16_t my_id, server_id;			// match ringer.peer 
	struct ivshmem_registers __iomem *regs;		// BAR0
	struct ivshmem_msi_x_msi_pba __iomem *UNUSED;	// BAR1
	struct famez_globals __iomem *globals;		// BAR2
	struct famez_mailslot *my_slot;			// indexed by my_id
	struct msix_entry *msix_entries;		// kzalloc an array
	struct famez_mailslot buffer_slot;		// temp copy for read()
	spinlock_t buffer_slot_lock;
};

//-------------------------------------------------------------------------
// famez_config.c - insmod and later probe() setup; final teardown of rmmod

extern int famez_verbose;				// insmod parameter

int famez_sendstring(uint32_t , char *, struct famez_configuration *);

//-------------------------------------------------------------------------
// famez_MSI-X.c - handle interrupts from other FAME-Z peers

int famez_MSIX_setup(struct pci_dev *);
void famez_MSIX_teardown(struct pci_dev *);

//-------------------------------------------------------------------------
// famez_bridge.c - a device file with simple Gen-Z bridge capabilities.

extern wait_queue_head_t bridge_reader_wait;

int famez_bridge_setup(struct pci_dev *);
void famez_bridge_teardown(struct pci_dev *);

//-------------------------------------------------------------------------
// Legibility and debug assistance

// linux/pci.h missed one
#ifndef pci_resource_name
#define pci_resource_name(dev, bar) (char *)((dev)->resource[(bar)].name)
#endif

#define CARDLOC(ptr) (pci_resource_name(ptr, 1))

#define STREQ(s1, s2) (!strcmp(s1, s2))
#define STREQ_N(s1, s2, lll) (!strncmp(s1, s2, lll))
#define STARTS(s1, s2) (!strncmp(s1, s2, strlen(s2)))

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
				pr_info(FZ "enter %s: ", _F_); pr_cont(a); }}
#define PR_EXIT(a...)	{ if (famez_verbose) { \
				pr_info(FZ "exit %s: ", _F_); pr_cont(a); }}

#define PR_SLEEPMS(_txt, _ms) { pr_info(FZ " " _txt); msleep(_ms); }

//-------------------------------------------------------------------------
// During callgraph generation, "flipping" these values will create a
// more detailed map.  Otherwise use normal/idiot-proofing/performant values.

#define CALLGRAPH

#ifdef CALLGRAPH
#define STATIC		
#define NOINLINE	noinline
#else
#define STATIC		static
#define NOINLINE
#endif

#endif
