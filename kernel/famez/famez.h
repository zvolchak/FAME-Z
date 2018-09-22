// Cuz it needs one

#ifndef FAMEZ_DOT_H
#define FAMEZ_DOT_H

#include <linux/list.h>
#include <linux/pci.h>
#include <linux/semaphore.h>
#include <linux/wait.h>

#include "genz_baseline.h"

#define FAMEZ_DEBUG			// See "Debug assistance" below

#define FAMEZ_NAME	"FAME-Z"
#define FZ		"famez: "	// pr_xxxx header
#define FZSP		"       "	// pr_xxxx header same length indent
#define DEFAULT_CCLASS	"FAME-Zadapter"	// no spaces

#define FAMEZ_VERSION	FAMEZ_NAME " v0.9.0: using Gen-Z subsystem"

struct ivshmem_registers {		// BAR 0
	uint32_t	Rev1Reserved1,	// Rev 0: Interrupt mask
			Rev1Reserved2,	// Rev 0: Interrupt status
			IVPosition,	// My peer id
			Doorbell;	// Upper and lower half
};

struct ivshmem_msi_x_table_pba {	// BAR 1: Not mapped, not used.  YET.
	uint32_t junk1, junk2;
};

// The famez_server.py controls the mailbox slot size and number of slots
// (and therefore the total file size).  It gives these numbers to this driver.
// There are always a power-of-two number of mailbox slots, indexed by IVSHMSG
// client ID.  Slot 0 is reserved for global data cuz it's easy to find :-)
// Besides, ID 0 doesn't seem to work in the QEMU doorbell mechanism.  The
// last slot (with ID == nClients + 1) is for the Python server.  The remaining
// slots are for client IDs 1 through nClients.

struct famez_globals {			// BAR 2: Start of IVSHMEM
	uint64_t slotsize, buf_offset, nClients, nEvents, server_id;
};

// Use only uint64_t and keep the buf[] on a 32-byte alignment for this:
// od -Ad -w32 -c -tx8 /dev/shm/famez_mailbox
struct __attribute__ ((packed)) famez_mailslot {
	char nodename[32];		// off  0: of the owning client
	char cclass[32];		// off 32: of the owning client
	uint64_t buflen,		// off 64:
		 peer_id,		// off 72: Convenience; set by server
		 last_responder,	// off 80: To assist stale stompage
		 peer_SID,		// off 88: Calculated in MSI-X...
		 peer_CID,		// off 96: ...from last_responder
		 pad[3];		// off 104
	char buf[];			// off 128 == globals->buf_offset
};

// The primary configuration/context data.
struct famez_adapter {
	struct list_head lister;
	atomic_t nr_users;				// User-space actors
	struct pci_dev *pdev;				// Paranoid reverse ptr
	int slot;					// pdev->devfn >> 3
	uint64_t max_buflen;
	uint16_t my_id;					// match ringer field
	struct ivshmem_registers __iomem *regs;		// BAR0
	struct famez_globals __iomem *globals;		// BAR2
	struct famez_mailslot *my_slot;			// indexed by my_id
	void *IRQ_private;				// arch-dependent?

	// Per-adapter handshaking between doorbell/mail delivery and a
	// driver read().  Doorbell comes in and sets the pointer then
	// issues a wakeup.  read() follows the pointer then sets it
	// to NULL for next one.  Since reading is more of a one-to-many
	// relationship this module can hold the one.

	struct famez_mailslot *incoming_slot;
	struct wait_queue_head incoming_slot_wqh;
	spinlock_t incoming_slot_lock;

	// Writing is many to one, so support buffers etc are the
	// responsibility of that module, managed by open() & release().
	void *outgoing;

	struct genz_core_structure *core;		// Primary data structure
	struct genz_char_device *genz_chrdev;		// Convenience backpointers
	void *teardown;
};

//-------------------------------------------------------------------------
// famez_pci.c - insmod/rmmod handling with pci_register probe()/remove()

extern int verbose;				// insmod parameter
extern struct list_head famez_adapter_list;
extern struct semaphore famez_adapter_sema;

//-------------------------------------------------------------------------
// famez_adapter.c - create/populate and destroy an adapter structure

// Linked in to famez.ko, used by various other source modules
struct famez_adapter *famez_adapter_create(struct pci_dev *);
void famez_adapter_destroy(struct famez_adapter *);
struct famez_mailslot __iomem *calculate_mailslot(
	struct famez_adapter *, unsigned);

// Nothing EXPORTed

//.........................................................................
// famez_IVSHMSG.c - the actual messaging IO.

#define FAMEZ_SID_DEFAULT		27	// see twisted_server.py
#define FAMEZ_SID_CID_IS_PEER_ID	-42	// interpret cid as peer_id

// EXPORTed
extern struct famez_mailslot *famez_await_incoming(struct famez_adapter *, int);
extern void famez_release_incoming(struct famez_adapter *);
extern int famez_create_outgoing(int, int, char *, size_t, struct famez_adapter *);

//.........................................................................
// famez_???.c - handle interrupts from other FAME-Z peers (input). By arch:
// x86_64:	famez_MSI-X.c
// ARM64:	famez_MSI-X.c with assist from QEMU vfio modules
// RISCV:	not written yet

irqreturn_t famez_link_request(struct famez_mailslot __iomem *,
			       struct famez_adapter *);

// EXPORTed
int famez_ISR_setup(struct pci_dev *);
void famez_ISR_teardown(struct pci_dev *);

//.........................................................................
// famez_register.c - accept end-driver requests to use FAME-Z.

// EXPORTed
extern int famez_register(unsigned, const struct file_operations *);
extern int famez_unregister(const struct file_operations *);

//-------------------------------------------------------------------------
// Legibility assistance

// linux/pci.h missed one
#ifndef pci_resource_name
#define pci_resource_name(dev, bar) (char *)((dev)->resource[(bar)].name)
#endif

#define CARDLOC(ptr) (pci_resource_name(ptr, 1))

#define STREQ(s1, s2) (!strcmp(s1, s2))
#define STREQ_N(s1, s2, lll) (!strncmp(s1, s2, lll))
#define STARTS(s1, s2) (!strncmp(s1, s2, strlen(s2)))

//-------------------------------------------------------------------------
// Debug assistance

#ifdef FAMEZ_DEBUG
#define PR_V1(a...)	{ if (verbose) pr_info(FZ a); }
#define PR_V2(a...)	{ if (verbose > 1) pr_info(FZ a); }
#define PR_V3(a...)	{ if (verbose > 2) pr_info(FZ a); }
#else
#define PR_V1(a...)
#define PR_V2(a...)
#define PR_V3(a...)
#endif

#define _F_		__FUNCTION__
#define PR_ENTER(a...)	{ if (verbose) { \
				pr_info(FZ "enter %s: ", _F_); pr_cont(a); }}
#define PR_EXIT(a...)	{ if (verbose) { \
				pr_info(FZ "exit %s: ", _F_); pr_cont(a); }}

#define PR_SLEEPMS(_txt, _ms) { pr_info(FZ " " _txt); msleep(_ms); }

#endif
