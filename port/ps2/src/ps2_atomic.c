/*
PS2_ATOMIC.C

The atomic operations GCC calls for the EE, which has no LL/SC to build them
from: the Win32 Interlocked calls (port/ps2/src/xbox_kernel.c), the
debug log's file (source/cseries/errors.c) and the tick counter's start.

The EE has one core, and its threads change only at an interrupt or a call
into the kernel: each operation runs with interrupts off (DIntr), and turns
them back on only if they were on, so an interrupt handler may call it too.
*/

#include <kernel.h>

typedef unsigned int u32_atomic;
typedef unsigned long long u64_atomic;

#define ATOMIC_BEGIN() int interrupts_were_on = DIntr()
#define ATOMIC_END() do { if (interrupts_were_on) EIntr(); } while (0)

/* (each operation stands alone: one that called another, as the sum of
__sync_fetch_and_add_4's result and its operand, GCC turned back into the
builtin __sync_add_and_fetch_4, whose body then jumped to itself) */

u32_atomic __sync_fetch_and_add_4(volatile void *pointer, u32_atomic value)
{
	volatile u32_atomic *target = pointer;
	u32_atomic old;
	ATOMIC_BEGIN();

	old = *target;
	*target = old + value;
	ATOMIC_END();
	return old;
}

u32_atomic __sync_add_and_fetch_4(volatile void *pointer, u32_atomic value)
{
	volatile u32_atomic *target = pointer;
	u32_atomic result;
	ATOMIC_BEGIN();

	result = *target + value;
	*target = result;
	ATOMIC_END();
	return result;
}

u32_atomic __sync_sub_and_fetch_4(volatile void *pointer, u32_atomic value)
{
	volatile u32_atomic *target = pointer;
	u32_atomic result;
	ATOMIC_BEGIN();

	result = *target - value;
	*target = result;
	ATOMIC_END();
	return result;
}

u32_atomic __sync_lock_test_and_set_4(volatile void *pointer, u32_atomic value)
{
	volatile u32_atomic *target = pointer;
	u32_atomic old;
	ATOMIC_BEGIN();

	old = *target;
	*target = value;
	ATOMIC_END();
	return old;
}

u32_atomic __sync_val_compare_and_swap_4(volatile void *pointer, u32_atomic expected, u32_atomic value)
{
	volatile u32_atomic *target = pointer;
	u32_atomic old;
	ATOMIC_BEGIN();

	old = *target;
	if (old == expected)
		*target = value;
	ATOMIC_END();
	return old;
}

/* libatomic's form: the memory orders are always kept (interrupts off is
the strongest) */
_Bool __atomic_compare_exchange_8(volatile void *pointer, void *expected, u64_atomic value, int success_order,
	int failure_order)
{
	volatile u64_atomic *target = pointer;
	u64_atomic *expected_value = expected;
	_Bool exchanged;
	ATOMIC_BEGIN();

	(void)success_order;
	(void)failure_order;
	exchanged = *target == *expected_value;
	if (exchanged)
		*target = value;
	else
		*expected_value = *target;
	ATOMIC_END();
	return exchanged;
}
