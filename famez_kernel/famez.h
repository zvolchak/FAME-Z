// Cuz it needs one

#ifndef FAMEZ_DOT_H
#define FAMEZ_DOT_H

#define FAMEZ_NAME	"famez"

// For PCI search
#define IVSHMEM_VENDOR	0x1af4	// RedHat
#define IVSHMEM_DEVICE	0x1110

// When stable, git commit, then git tag, then commit again (for the tag)
#define FAMEZ_VERSION	"famez git version v0.6: doesn't even compile"

#include <linux/delay.h>
#include <linux/mmzone.h>
#include <linux/configfs.h>
#include <linux/list.h>

#define FAMEZ_DEBUG

#ifdef FAMEZ_DEBUG
#define PR_VERBOSE1(a...)	{ if (famez_verbose) pr_info(a); }
#define PR_VERBOSE2(a...)	{ if (famez_verbose > 1) pr_info(a); }
#define PR_VERBOSE3(a...)	{ if (famez_verbose > 2) pr_info(a); }
#else
#define PR_VERBOSE1(a...)
#define PR_VERBOSE2(a...)
#define PR_VERBOSE3(a...)
#endif

#define _F_		__FUNCTION__
#define PR_ENTER(a...)	{ if (famez_verbose) { \
				pr_info("r2h: enter %s: ", _F_); pr_cont(a); }}
#define PR_EXIT(a...)	{ if (famez_verbose) { \
				pr_info("r2h: exit %s: ", _F_); pr_cont(a); }}

#define PR_SLEEPMS(_txt, _ms) { pr_info("r2h: " _txt); msleep(_ms); }

#define STREQ(s1, s2) (!strcmp(s1, s2))
#define STARTS(s1, s2) (!strncmp(s1, s2, strlen(s2)))

struct famez_configuration {
	struct pci_dev *pci_dev;
	struct resource *res_registers, *res_mailbox;	// convenience
	void *registers, *mailbox			// after mapping
};

//-------------------------------------------------------------------------
// famez_base.c - globals from insmod parameters, then routines

extern int famez_verbose;

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
