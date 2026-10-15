/*
PS2_MEMORY.C

The Xbox's contiguous ("physical") memory on the PS2: XPhysicalAlloc and
the page protection calls, which the Linux build gives the Xbox's memory
window at 0x80000000 (port/linux/src/xbox_memory.c).

A PS2 program has neither: the EE's memory past 0x80000000 is the kernel's,
and the program runs without page protection. Each block comes from the
heap, page-aligned and zeroed, where the program's other memory is. The game
asks for the game state and the tag, texture and sound caches once, at
start (physical_memory_map.c), so they are at the same addresses each run of
a build, which saved games need (they are images of the game state); the
map's tags are moved to wherever the tag cache is
(port/ps2/game/tag_relocate.c). Physical and virtual addresses are the
same, which the PS2's Direct3D keeps in its resources
(port/ps2/src/ps2_d3d_null.c).

Each block is logged: the boot's log is the memory map.
*/

#include "platform.h"

#include <malloc.h>
#include <string.h>
#include <unistd.h>

#define PAGE_SIZE_BYTES 0x1000UL

/* the EE's 32 MB less the thread stack at the top (the same 0x20000 the fixed
block check below leaves) */
#define MEMORY_LIMIT (0x2000000UL - 0x20000UL)

static unsigned long allocated_bytes;
/* the heap grows up to the lowest block at an address of its own */
static unsigned long heap_ceiling = MEMORY_LIMIT;

/* the memory ledger: what is left between the heap's end and the first fixed
block (or the stack), so a boot's log shows each block's cost against the 32 MB (docs/ps2_kernel_plan.md,
K1; the IOP's 2 MB is added when K2 loads modules) */
static void memory_ledger(void)
{
	unsigned long heap_end = (unsigned long)sbrk(0);

	platform_log("memory: %lu KB taken by blocks, heap ends at 0x%08lx, %ld KB left for the heap below 0x%08lx",
		allocated_bytes / 1024, heap_end, (long)(heap_ceiling - heap_end) / 1024, heap_ceiling);
}

LPVOID WINAPI XPhysicalAlloc(SIZE_T size, ULONG_PTR physical_address, ULONG_PTR alignment, DWORD protect)
{
	unsigned long boundary = alignment > PAGE_SIZE_BYTES ? (unsigned long)alignment : PAGE_SIZE_BYTES;
	void *result;

	(void)protect;
	if (physical_address != (ULONG_PTR)-1)
	{
		/* a block at an address of its own: the end of the memory the heap
		leaves (tools/ps2_build.py) up to the thread stack's */
		if (physical_address < (ULONG_PTR)sbrk(0) || physical_address + size > MEMORY_LIMIT)
		{
			platform_log("memory: no room for %lu bytes at 0x%08lx (the heap ends at 0x%08lx)", (unsigned long)size,
				(unsigned long)physical_address, (unsigned long)sbrk(0));
			SetLastError(ERROR_NOT_ENOUGH_MEMORY);
			return NULL;
		}
		memset((void *)physical_address, 0, size);
		allocated_bytes += size;
		if (physical_address < heap_ceiling)
			heap_ceiling = physical_address;
		platform_log("memory: %lu bytes at 0x%08lx-0x%08lx (fixed)", (unsigned long)size, (unsigned long)physical_address,
			(unsigned long)physical_address + (unsigned long)size);
		memory_ledger();
		return (LPVOID)physical_address;
	}
	result = memalign(boundary, size ? size : 1);
	if (!result)
	{
		platform_log("memory: no room for %lu bytes (%lu allocated)", (unsigned long)size, allocated_bytes);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	memset(result, 0, size);
	allocated_bytes += size;
	platform_log("memory: %lu bytes at 0x%08lx-0x%08lx", (unsigned long)size, (unsigned long)result,
		(unsigned long)result + (unsigned long)size);
	memory_ledger();
	return result;
}

VOID WINAPI XPhysicalFree(LPVOID address)
{
	/* (a block at an address of its own is not the heap's) */
	if ((unsigned long)address < (unsigned long)sbrk(0))
		free(address);
}

/* no page protection: everything stays readable and writable */

BOOL WINAPI VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
	(void)address;
	(void)size;
	(void)new_protect;
	if (old_protect)
		*old_protect = PAGE_READWRITE;
	return TRUE;
}

VOID WINAPI XPhysicalProtect(LPVOID address, SIZE_T size, DWORD new_protect)
{
	(void)address;
	(void)size;
	(void)new_protect;
}

DWORD WINAPI XQueryMemoryProtect(LPVOID address)
{
	(void)address;
	return PAGE_READWRITE;
}

/* the file layer copies reads into the Linux build's memory window through
a buffer, for its page protection (xbox_files.c): the PS2 has neither */
BOOL platform_is_contiguous(const void *address)
{
	(void)address;
	return FALSE;
}

/* the PS2 plays no Custom Edition map, which the Linux build reads into a
tag cache of its own at 0x40440000 */
void *halo_custom_edition_tag_cache(void)
{
	return NULL;
}
