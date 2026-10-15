/*
PS2_VBLANK.C

The PS2's vertical blank (ps2_vblank.h), and the scheduling of the game's
threads.

The GS's vertical blank start interrupt signals a semaphore; a thread one
priority above the game's threads counts the blank, wakes the threads
waiting for it, flips the presented frame and calls the game's callback, as
the Xbox's Direct3D calls it each vertical blank (the Linux build does it
from a thread too, port/linux/src/d3d8_gl.c).

The EE's kernel does not share time between threads of one priority: a
thread runs until it waits or a higher one is ready. The Xbox's kernel
does, and the game's threads (main's, the loader's, input's) expect it.
So every game thread has one priority, GAME_THREAD_PRIORITY, and each
vertical blank rotates that priority's ready queue, giving each game thread
its turn 60 times a second (50 on PAL). ps2sdk starts its pthreads, which
the Win32 CreateThread makes (port/ps2/src/xbox_kernel.c, whose
SetThreadPriority changes nothing), at that priority; main's thread starts
higher and is moved down to it before main runs.
*/

#include "ps2_vblank.h"

/* the EE kernel's, not the Win32 ones (halo_ps2_prefix.h) */
#undef CreateThread
#undef ResumeThread
#include <kernel.h>
#include <pthread.h>

/* measured: ps2sdk (v2.0.0) starts a pthread at 92, main's thread at 1 */
#define GAME_THREAD_PRIORITY 92
#define VBLANK_THREAD_PRIORITY (GAME_THREAD_PRIORITY - 1)

static int interrupt_semaphore = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static unsigned long blank_count;
static unsigned long pending_flips;
static volatile unsigned int flip_count;
static ps2_vblank_callback callback;

static s32 vblank_start_handler(s32 cause)
{
	(void)cause;
	iSignalSema(interrupt_semaphore);
	/* as Sony's documentation asks, at the very end of an interrupt handler */
	ExitHandler();
	return 0;
}

static void *vblank_thread(void *unused)
{
	(void)unused;
	ChangeThreadPriority(GetThreadId(), VBLANK_THREAD_PRIORITY);
	for (;;)
	{
		ps2_vblank_callback current;

		WaitSema(interrupt_semaphore);
		/* the next game thread's turn */
		RotateThreadReadyQueue(GAME_THREAD_PRIORITY);
		pthread_mutex_lock(&lock);
		blank_count++;
		if (pending_flips)
		{
			pending_flips--;
			flip_count++;
		}
		current = callback;
		pthread_cond_broadcast(&condition);
		pthread_mutex_unlock(&lock);
		if (current)
			current(0);
	}
	return NULL;
}

static void vblank_start(void)
{
	static int started = 0;
	ee_sema_t semaphore = { 0 };
	pthread_t thread;

	if (started)
		return;
	started = 1;
	/* one pending blank at most: a thread that falls behind skips blanks */
	semaphore.max_count = 1;
	interrupt_semaphore = CreateSema(&semaphore);
	pthread_create(&thread, NULL, vblank_thread, NULL);
	AddIntcHandler(INTC_VBLANK_S, vblank_start_handler, 0);
	EnableIntc(INTC_VBLANK_S);
}

/* before main: main's thread joins the game's priority, and the vertical
blank (which shares the time between them) starts */
static void __attribute__((constructor)) ps2_threads_initialize(void)
{
	ChangeThreadPriority(GetThreadId(), GAME_THREAD_PRIORITY);
	vblank_start();
}

void ps2_vblank_set_callback(ps2_vblank_callback new_callback)
{
	vblank_start();
	pthread_mutex_lock(&lock);
	callback = new_callback;
	pthread_mutex_unlock(&lock);
}

void ps2_vblank_wait(void)
{
	unsigned long count;

	vblank_start();
	pthread_mutex_lock(&lock);
	count = blank_count;
	while (blank_count == count)
		pthread_cond_wait(&condition, &lock);
	pthread_mutex_unlock(&lock);
}

void ps2_vblank_present(void)
{
	vblank_start();
	pthread_mutex_lock(&lock);
	pending_flips++;
	pthread_mutex_unlock(&lock);
}

volatile unsigned int *ps2_vblank_flip_count(void)
{
	return &flip_count;
}
