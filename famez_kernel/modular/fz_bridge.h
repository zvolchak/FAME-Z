// Cuz it needs one

#ifndef FAMEZ_BRIDGE_DOT_H
#define FAMEZ_BRIDGE_DOT_H

#include <linux/list.h>
#include <linux/mutex.h>

#define FZBRIDGE_DEBUG			// See "Debug assistance" below

#define FZBRIDGE_NAME	"fzbridge"
#define FZBR		"fzbr: "	// pr_xxxx header
#define FZBRSP		"      "	// pr_xxxx header same length indent

#define FZBRIDGE_VERSION	FZBRIDGE_NAME " v0.1.0: gotta start somewhere"

// Just write support for now.
typedef struct {
	char *wbuf;			// kmalloc(max_msglen)
	struct mutex wbuf_mutex;
} bridge_buffers_t;

//-------------------------------------------------------------------------
// Debug support

#ifdef PR_V1		// Avoid "redefine" errors
#undef PR_V1
#undef PR_V2
#undef PR_V3
#undef PR_ENTER
#undef PR_EXIT
#endif

#ifdef FZBRIDGE_DEBUG
#define PR_V1(a...)	{ if (fzbridge_verbose) pr_info(FZBR a); }
#define PR_V2(a...)	{ if (fzbridge_verbose > 1) pr_info(FZBR a); }
#define PR_V3(a...)	{ if (fzbridge_verbose > 2) pr_info(FZBR a); }
#else
#define PR_V1(a...)
#define PR_V2(a...)
#define PR_V3(a...)
#endif

#define _F_		__FUNCTION__
#define PR_ENTER(a...)	{ if (fzbridge_verbose) { \
				pr_info(FZBR "enter %s: ", _F_); pr_cont(a); }}
#define PR_EXIT(a...)	{ if (famez_verbose) { \
				pr_info(FZBR "exit %s: ", _F_); pr_cont(a); }}

#endif
