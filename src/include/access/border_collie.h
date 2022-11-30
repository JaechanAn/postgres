/*-------------------------------------------------------------------------
 *
 * border_collie.h
 *	  API for Postgres border-collie features.
 *
 * Copyright (c) 2015-2022, PostgreSQL Global Development Group
 *
 * src/include/access/border_collie.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BORDER_COLLIE_H 
#define BORDER_COLLIE_H

typedef uint64_t Flag;

extern PGDLLIMPORT int NBorderCollieFlags;

extern Size BorderCollieFlagsSize(void);
extern void BorderCollieFlagsInit(void);

#define GetBorderCollieFlag(id) (&BorderCollieFlag[(id)])
#define SetBorderCollieFlag(id, val) ( /* Your content... */ )

#endif							/* BORDER_COLLIE_H */
