/*-------------------------------------------------------------------------
 *
 * border_collie_process.h
 *	  header file for integrated border-collie
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/border_collie_process.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BORDER_COLLIE_PROCESS_H
#define BORDER_COLLIE_PROCESS_H

#include "c.h"

extern void BorderCollieProcessMain(void) pg_attribute_noreturn();

extern Size BorderCollieShmemSize(void);
extern void BorderCollieShmemInit(void);

#endif							/* BORDER_COLLIE_PROCESS_H */
