#include "postgres.h"

#include "access/border_collie.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

Flag *BorderCollieFlags;

Size
BorderCollieFlagsSize(void)
{
	Size size = 0;

	size = add_size(size, NBorderCollieFlags * sizeof(Flag));

	return size;
}

void
BorderCollieFlagsInit(void)
{
	Size size = BorderCollieFlagsSize();
	bool found;

	/*
	 * Create or attach to the shared memory state, including hash table
	 */
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

	BorderCollieFlags = (Flag *)ShmemInitStruct(
		"Border Collie Flags", NBorderCollieFlags * sizeof(Flag), &found);

	if (!found)
	{
		/*
		 * First time through, so initialize.
		 */
		MemSet(BorderCollieFlags, 0, size);
		Assert(BorderCollieFlags != NULL);
	}

	LWLockRelease(AddinShmemInitLock);
}
