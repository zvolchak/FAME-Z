// Cuz it needs one

#ifndef FAMEZ_DOT_H
#define FAMEZ_DOT_H

#include <linux/list.h>
#include <linux/mutex.h>

#define FAMEZ_DEBUG			// See "Debug assistance" below

#define FAMEZ_NAME	"famez"
#define FZ		"famez: "	// pr_xxxx header
#define FZSP		"       "	// pr_xxxx header same length indent

#define FAMEZ_VERSION	FAMEZ_NAME " v0.7.5: endgame chardev"

// Used from user context, sleeping is allowed
#define FAMEZ_LOCK_STRUCT	struct mutex
#define FAMEZ_LOCK_INIT(LLL)	mutex_init(LLL)
#define FAMEZ_LOCK(LLL)		mutex_lock_interruptible(LLL)
#define FAMEZ_UNLOCK(LLL)	mutex_unlock(LLL)

typedef struct {			// BAR 0
	uint32_t	Rev1Reserved1,	// Rev 0: Interrupt mask
			Rev1Reserved2,	// Rev 0: Interrupt status
			IVPosition,	// My peer id
			Doorbell;	// Upper and lower half
} ivshmem_registers_t;

typedef struct {			// BAR 1: Not mapped, not used.  YET.
	uint32_t junk;
} ivshmem_msi_x_table_pba_t;

// The famez_server.py controls the mailbox slot size and number of slots
// (and therefore the total file size).  It gives these numbers to this driver.
// There are always a power-of-two number of mailbox slots, indexed by IVSHMSG
// client ID.  Slot 0 is reserved for global data; it's easy to find :-); 
// besides, ID 0 doesn't seem to work in the doorbell.  The last slot (with 
// ID == nSlots - 1) is for the Python server.  The remaining slots are for
// client IDs 1 through (nSlots - 2).

typedef struct {			// BAR 2: Start of IVSHMEM
	uint64_t slotsize, msg_offset, nSlots;
} famez_globals_t;

typedef struct {
	int num;
	void *target;
	char *info;
} famez_BARtab_t;

// Use only uint64_t and keep the msg[] on a 32-byte alignment for this:
// od -Ad -w32 -c -tx8 /dev/shm/famez_mailbox
typedef struct __attribute__ ((packed)) {
	char nodename[32];		// off  0: of the owning client
	uint64_t msglen,		// off 32:
		 peer_id,		// off 40: Convenience; set by server
		 pad1, pad2;
	char msg[];			// off 64: globals->msg_offset
} famez_mailslot_t;

// The IVSHMEM "vector" will map to an MSI-X "entry" value.  "vector" is
// the lower 16 bits and the combo must be assigned atomically.

typedef union __attribute__ ((packed)) {
	struct { uint16_t vector, peer; };
	uint32_t Doorbell;
} ivshmsg_ringer_t;

// The primary configuration/context data.
typedef struct {
	struct list_head lister;
	atomic_t nr_users;				// User-space actors
	struct pci_dev *pdev;				// Paranoid reverse ptr
	void *teardown_lookup;				// Convenience backpointer
	uint64_t max_msglen;
	uint16_t my_id, server_id;			// match ringer.peer 
	ivshmem_registers_t __iomem *regs;		// BAR0
	famez_globals_t __iomem *globals;		// BAR2
	famez_mailslot_t *my_slot;			// indexed by my_id
	struct msix_entry *msix_entries;		// pci.h: kzalloc array

	// Per-config handshaking between doorbell/mail delivery and a
	// driver read().  Doorbell comes in and sets the pointer then
	// issues a wakeup.  read() follows the pointer then sets it
	// to NULL for next one.  Since reading is more of a one-to-many
	// relationship this module can hold the one.

	famez_mailslot_t *legible_slot;
	struct wait_queue_head legible_slot_wqh;
	FAMEZ_LOCK_STRUCT legible_slot_lock;

	// Writing is many to one, so support buffers etc are the
	// responsibility of that module.
	void *writer_support;

} famez_configuration_t;

//-------------------------------------------------------------------------
// famez_config.c - insmod and later probe() setup; final teardown of rmmod

extern int famez_verbose;				// insmod parameter

int famez_sendstring(uint32_t , char *, famez_configuration_t *);

//-------------------------------------------------------------------------
// famez_MSI-X.c - handle interrupts from other FAME-Z peers

int famez_MSIX_setup(struct pci_dev *);
void famez_MSIX_teardown(struct pci_dev *);

//-------------------------------------------------------------------------
// famez_bridge.c - a device file with simple Gen-Z bridge capabilities.

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
#define PR_V1(a...)	{ if (famez_verbose) pr_info(FZ a); }
#define PR_V2(a...)	{ if (famez_verbose > 1) pr_info(FZ a); }
#define PR_V3(a...)	{ if (famez_verbose > 2) pr_info(FZ a); }
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

// #define spin_lock(AAA)
// #define spin_unlock(AAA)

#endif
