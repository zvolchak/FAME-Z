// Cuz it needs one

#ifndef FAMEZ_DOT_H
#define FAMEZ_DOT_H

#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/semaphore.h>
#include <linux/wait.h>

#include "genz_baseline.h"

#define FAMEZ_DEBUG			// See "Debug assistance" below

#define FAMEZ_NAME	"famez"
#define FZ		"famez: "	// pr_xxxx header
#define FZSP		"       "	// pr_xxxx header same length indent

#define FAMEZ_VERSION	FAMEZ_NAME " v0.8.2: modular w/bridge"

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
	uint64_t buflen,		// off 32:
		 peer_id,		// off 40: Convenience; set by server
		 last_responder,	// off 48: To assist stale stompage
		 peer_SID,		// off 56: Calculated in MSI-X...
		 peer_CID,		// off 64: ...from last_responder
		 pad[7];		// off 72
	char buf[];			// off 128 == globals->buf_offset
};

// The primary configuration/context data.
struct famez_config {
	struct list_head lister;
	atomic_t nr_users;				// User-space actors
	struct pci_dev *pdev;				// Paranoid reverse ptr
	void *teardown_lookup;				// Convenience backpointer
	uint64_t max_buflen;
	uint16_t my_id;					// match ringer field
	struct ivshmem_registers __iomem *regs;		// BAR0
	struct famez_globals __iomem *globals;		// BAR2
	struct famez_mailslot *my_slot;			// indexed by my_id
	void *IRQ_private;				// arch-dependent?

	// Per-config handshaking between doorbell/mail delivery and a
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

	struct genz_core_structure *core;
};

// https://stackoverflow.com/questions/39464028/device-specific-data-structure-with-platform-driver-and-character-device-interfa
// A lookup table to take advantage of misc_register putting its argument
// into file->private at open().  Fill in the blanks for each config and go.
// I modified the article's solution to treat it as a container pointer and
// just grab whatever field I want, it doesn't even have to be the first one.
// If I put the "primary key" structure as the first field, then I wouldn't
// even need container_of as the address is synonymous with both.

struct miscdev2config {
	struct miscdevice miscdev;	// full structure, not a ptr
	struct famez_config *config;	// what I want to recover
};

static inline struct famez_config *extract_config(struct file *file)
{
	struct miscdevice *encapsulated_miscdev = file->private_data;
	struct miscdev2config  *lookup = container_of(
		encapsulated_miscdev,	// the pointer to the member
		struct miscdev2config,	// the type of the container struct
		miscdev);		// the name of the member in the struct
	return lookup->config;
}

//-------------------------------------------------------------------------
// famez_pci_base.c - insmod/rmmod handling with pci_register probe()/remove()

extern int verbose;				// insmod parameter
extern struct list_head famez_active_list;
extern struct semaphore famez_active_sema;

//-------------------------------------------------------------------------
// famez_config.c - create/populate and destroy a config structure

// Linked in to famez.ko, used by various other source modules
struct famez_config *famez_config_create(struct pci_dev *);
void famez_config_destroy(struct famez_config *);
struct famez_mailslot __iomem *calculate_mailslot(struct famez_config *,
						  unsigned);

// Nothing EXPORTed

//.........................................................................
// famez_IVSHMSG.c - the actual messaging IO.

#define FAMEZ_SID_DEFAULT		27	// see twisted_server.py
#define FAMEZ_SID_CID_IS_PEER_ID	-42	// interpret cid as peer_id

// EXPORTed
extern struct famez_mailslot *famez_await_incoming(struct famez_config *, int);
extern void famez_release_incoming(struct famez_config *);
extern int famez_create_outgoing(int, int, char *, size_t, struct famez_config *);

//.........................................................................
// famez_???.c - handle interrupts from other FAME-Z peers (input). By arch:
// x86_64:	famez_MSI-X.c
// ARM64:	famez_MSI-X.c with assist from QEMU vfio modules
// RISCV:	not written yet

// EXPORTed
int famez_ISR_setup(struct pci_dev *);
void famez_ISR_teardown(struct pci_dev *);

//.........................................................................
// famez_register.c - accept end-driver requests to use FAME-Z.

// EXPORTed
extern int famez_misc_register(char *, const struct file_operations *);
extern int famez_misc_deregister(const struct file_operations *);

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
