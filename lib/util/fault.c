/*
   Unix SMB/CIFS implementation.
   Critical Fault handling
   Copyright (C) Andrew Tridgell 1992-1998
   Copyright (C) Tim Prouty 2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "system/filesys.h"
#include "version.h"

#ifdef HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif


#ifdef HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

static struct {
	bool disabled;
	smb_panic_handler_t panic_handler;
} fault_state;


/*******************************************************************
setup variables used for fault handling
********************************************************************/
void fault_configure(smb_panic_handler_t panic_handler)
{
	fault_state.panic_handler = panic_handler;
}


/**
   disable setting up fault handlers
   This is used for the bind9 dlz module, as we
   don't want a Samba module in bind9 to override the bind
   fault handling
**/
_PUBLIC_ void fault_setup_disable(void)
{
	fault_state.disabled = true;
}


/*******************************************************************
report a fault
********************************************************************/
static void fault_report(int sig)
{
	static int counter;

	if (counter) _exit(1);

	counter++;

	DEBUGSEP(0);
	DEBUG(0,("INTERNAL ERROR: Signal %d in pid %d (%s)",sig,(int)sys_getpid(),SAMBA_VERSION_STRING));
	DEBUG(0,("\nPlease read the Trouble-Shooting section of the Samba HOWTO\n"));
	DEBUGSEP(0);

	smb_panic("internal error");

	/* smb_panic() never returns, so this is really redundent */
	exit(1);
}

/****************************************************************************
catch serious errors
****************************************************************************/
static void sig_fault(int sig)
{
	fault_report(sig);
}

/*******************************************************************
setup our fault handlers
********************************************************************/
void fault_setup(void)
{
	if (fault_state.disabled) {
		return;
	}
#ifdef SIGSEGV
	CatchSignal(SIGSEGV, sig_fault);
#endif
#ifdef SIGBUS
	CatchSignal(SIGBUS, sig_fault);
#endif
#ifdef SIGABRT
	CatchSignal(SIGABRT, sig_fault);
#endif
}

_PUBLIC_ const char *panic_action = NULL;

/*
   default smb_panic() implementation
*/
static void smb_panic_default(const char *why)
{
	int result;

	if (panic_action && *panic_action) {
		char pidstr[20];
		char cmdstring[200];
		strlcpy(cmdstring, panic_action, sizeof(cmdstring));
		snprintf(pidstr, sizeof(pidstr), "%d", (int) getpid());
		all_string_sub(cmdstring, "%d", pidstr, sizeof(cmdstring));
		DEBUG(0, ("smb_panic(): calling panic action [%s]\n", cmdstring));
		result = system(cmdstring);

		if (result == -1)
			DEBUG(0, ("smb_panic(): fork failed in panic action: %s\n",
				  strerror(errno)));
		else
			DEBUG(0, ("smb_panic(): action returned status %d\n",
				  WEXITSTATUS(result)));
	}
	DEBUG(0,("PANIC: %s\n", why));

#ifdef SIGABRT
	CatchSignal(SIGABRT, SIG_DFL);
#endif
	abort();
}


/**
   Something really nasty happened - panic !
**/
_PUBLIC_ void smb_panic(const char *why)
{
	if (fault_state.panic_handler) {
		fault_state.panic_handler(why);
		_exit(1);
	}
	smb_panic_default(why);
}
