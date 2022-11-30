/*-------------------------------------------------------------------------
 *
 * border_collie_process.c
 *
 * Border-collie Process Implementation
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/border_collie_process.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "access/border_collie.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/border_collie_process.h"
#include "postmaster/interrupt.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "storage/standby.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"

/*
 * GUC parameters
 */
int BorderCollieDelay = 1000; /* milliseconds */

typedef struct
{
	pid_t border_collie_pid; /* PID (0 if not started) */
} BorderCollieShmemStruct;

static BorderCollieShmemStruct *BorderCollieShmem = NULL;

/* Prototypes for private functions */
static void HandleBorderCollieProcessInterrupts(void);

static void BorderCollieProcessInit(void);

/*
 * Main entry point for border-collie process
 *
 * This is invoked from AuxiliaryProcessMain, which has already created the
 * basic execution environment, but not enabled signals yet.
 */
void
BorderCollieProcessMain(void)
{
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext border_collie_context;

	uint32_t tick = 0;

	BorderCollieShmem->border_collie_pid = MyProcPid;

	/*
	 * Properly accept or ignore signals the postmaster might send us
	 *
	 * We have no particular use for SIGINT at the moment, but seems
	 * reasonable to treat like SIGTERM.
	 */
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGINT, SignalHandlerForShutdownRequest);
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	pqsignal(SIGQUIT, SignalHandlerForCrashExit);
	pqsignal(SIGALRM, SIG_IGN);
	pqsignal(SIGPIPE, SIG_IGN);
	pqsignal(SIGUSR1, procsignal_sigusr1_handler);
	pqsignal(SIGUSR2, SIG_IGN); /* not used */

	/*
	 * Reset some signals that are accepted by postmaster but not here
	 */
	pqsignal(SIGCHLD, SIG_DFL);

	/* We allow SIGQUIT (quickdie) at all times */
	sigdelset(&BlockSig, SIGQUIT);

	/*
	 * Create a memory context that we will do all our work in.  We do this so
	 * that we can reset the context during error recovery and thereby avoid
	 * possible memory leaks.  Formerly this code just ran in
	 * TopMemoryContext, but resetting that would be a really bad idea.
	 */
	border_collie_context = AllocSetContextCreate(
		TopMemoryContext, "Border Collie", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(border_collie_context);

	/*
	 * If an exception is encountered, processing resumes here.
	 */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		/* Since not using PG_TRY, must reset error stack by hand */
		error_context_stack = NULL;

		/* Prevent interrupts while cleaning up */
		HOLD_INTERRUPTS();

		/* Report the error to the server log */
		EmitErrorReport();

		/*
		 * These operations are really just a minimal subset of
		 * AbortTransaction().  We don't have very many resources to worry
		 * about in walwriter, but we do have LWLocks, and perhaps buffers?
		 */
		LWLockReleaseAll();
		ConditionVariableCancelSleep();
		pgstat_report_wait_end();
		AbortBufferIO();
		UnlockBuffers();
		ReleaseAuxProcessResources(false);
		AtEOXact_Buffers(false);
		AtEOXact_SMgr();
		AtEOXact_Files(false);
		AtEOXact_HashTables(false);

		/*
		 * Now return to normal top-level context and clear ErrorContext for
		 * next time.
		 */
		MemoryContextSwitchTo(border_collie_context);
		FlushErrorState();

		/* Flush any leaked data in the top-level context */
		MemoryContextResetAndDeleteChildren(border_collie_context);

		/* Now we can allow interrupts again */
		RESUME_INTERRUPTS();

		/*
		 * Sleep at least 1 second after any error.  A write error is likely
		 * to be repeated, and we don't want to be filling the error logs as
		 * fast as we can.
		 */
		pg_usleep(1000000L);

		/*
		 * Close all open files after any error.  This is helpful on Windows,
		 * where holding deleted files open causes various strange errors.
		 * It's not clear we need it elsewhere, but shouldn't hurt.
		 */
		smgrcloseall();
	}

	/* We can now handle ereport(ERROR) */
	PG_exception_stack = &local_sigjmp_buf;

	/*
	 * Unblock signals (they were blocked when the postmaster forked us)
	 */
	PG_SETMASK(&UnBlockSig);

	/*
	 * Advertise our latch that backends can use to wake us up while we're
	 * sleeping.
	 */
	ProcGlobal->bordercollieLatch = &MyProc->procLatch;

	/* Initialize EBI-tree process's local variables */
	BorderCollieProcessInit();

	/*
	 * Loop forever
	 */
	for (;;)
	{
		long cur_timeout;

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		HandleBorderCollieProcessInterrupts();

		cur_timeout = BorderCollieDelay;

		ereport(LOG, (errmsg("[BorderCollie] %d second", tick)));

		++tick;

		(void)WaitLatch(MyLatch,
						WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						cur_timeout, WAIT_EVENT_BORDER_COLLIE_MAIN);
	}
}

/*
 * Process any new interrupts.
 */
static void
HandleBorderCollieProcessInterrupts(void)
{
	if (ProcSignalBarrierPending)
		ProcessProcSignalBarrier();

	if (ConfigReloadPending)
	{
		ConfigReloadPending = false;
		ProcessConfigFile(PGC_SIGHUP);
	}

	if (ShutdownRequestPending)
	{
		/* Any cleanups on shutdown should be done here */

		/* Normal exit from the border collie process is here */
		proc_exit(0); /* done */
	}
}

/*
 * Initialize the border collie process.
 */
static void
BorderCollieProcessInit(void)
{
	/* Any initialization if necessary */
}

/* --------------------------------
 *		communication with backends
 * --------------------------------
 */

/*
 * BorderCollieShmemSize
 *		Compute space required for border collie related shared memory
 */
Size
BorderCollieShmemSize(void)
{
	Size size = 0;

	size = add_size(size, sizeof(BorderCollieShmemStruct));

	size = add_size(size, BorderCollieFlagsSize());

	return size;
}

/*
 * BorderCollieShmemInit
 *		Allocate and initialize border collie related shared memory
 */
void
BorderCollieShmemInit(void)
{
	Size size = BorderCollieShmemSize();
	bool found;

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	BorderCollieShmem = (BorderCollieShmemStruct *)ShmemInitStruct(
		"Border Collie Data", sizeof(BorderCollieShmemStruct), &found);

	if (!found)
	{
		/*
		 * First time through, so initialize.
		 */
		MemSet(BorderCollieShmem, 0, size);
		Assert(BorderCollieShmem != NULL);

		/* Init variables */
	}

	LWLockRelease(AddinShmemInitLock);

	/*
	 * Do NOT move this into AddinShmemInitLock above. This function
	 * internally holds the same lock.
	 */
	BorderCollieFlagsInit();
}
