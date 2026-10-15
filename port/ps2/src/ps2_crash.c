/*
PS2_CRASH.C

What happens when the EE takes an exception (an address error, a TLB fault, a
bad instruction): the registers are saved, the faulting thread carries on in
ps2_crash_report on a stack of its own (the exception's return address is set
to it), and the report is written to host:crash.txt and to the console (ps2client prints it).

ps2link's own exception handler parks the EE on a register dump, and the EE
then serves nothing (ps2client's dumpmem and scrdump wait for it), so a crash
on hardware is a photograph of a TV. This handler replaces it for the common
faults so that ps2client, which serves host:, receives the report.

The addresses in the report are for mips64r5900el-ps2-elf-addr2line -f -e
halo.elf. The stack scan lists every word of the stack that is an address in
the code; most are old returns, the nearest are the call chain.
*/

#include <kernel.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <tamtypes.h>
#include <unistd.h>

#define STACK_SIZE 16384
#define REPORT_SIZE 6144
#define STACK_SCAN_WORDS 768
#define STACK_SCAN_LISTED 40

/* the code is at the start of the ELF's image (the linker's _ftext and _fdata
   are not symbols the stub check (tools/ps2/unimplemented_stubs.py) allows);
   the range takes in some of the data after it, which only adds noise */
#define CODE_BEGIN 0x100000u
#define CODE_END 0x500000u

/* the layout ps2_crash_vector writes: 32 registers (the low 64 bits), then
   the coprocessor 0 registers; the offsets are in the assembler below */
struct crash_state
{
	u64 gpr[32];
	u32 epc;
	u32 cause;
	u32 bad_address;
	u32 status;
};

struct crash_state ps2_crash_state __attribute__((aligned(64)));
unsigned char ps2_crash_stack[STACK_SIZE] __attribute__((aligned(64)));
static char report[REPORT_SIZE];
static int report_length;
static volatile int crashed;

extern void ps2_crash_vector(void);

__asm__(
	".text\n"
	".set push\n"
	".set noreorder\n"
	".set noat\n"
	".align 4\n"
	".globl ps2_crash_vector\n"
	".type ps2_crash_vector, @function\n"
	"ps2_crash_vector:\n"
	"lui $26, %hi(ps2_crash_state)\n"
	"addiu $26, $26, %lo(ps2_crash_state)\n"
	"sd $1, 8($26)\n"
	"sd $2, 16($26)\n"
	"sd $3, 24($26)\n"
	"sd $4, 32($26)\n"
	"sd $5, 40($26)\n"
	"sd $6, 48($26)\n"
	"sd $7, 56($26)\n"
	"sd $8, 64($26)\n"
	"sd $9, 72($26)\n"
	"sd $10, 80($26)\n"
	"sd $11, 88($26)\n"
	"sd $12, 96($26)\n"
	"sd $13, 104($26)\n"
	"sd $14, 112($26)\n"
	"sd $15, 120($26)\n"
	"sd $16, 128($26)\n"
	"sd $17, 136($26)\n"
	"sd $18, 144($26)\n"
	"sd $19, 152($26)\n"
	"sd $20, 160($26)\n"
	"sd $21, 168($26)\n"
	"sd $22, 176($26)\n"
	"sd $23, 184($26)\n"
	"sd $24, 192($26)\n"
	"sd $25, 200($26)\n"
	"sd $28, 224($26)\n"
	"sd $29, 232($26)\n"
	"sd $30, 240($26)\n"
	"sd $31, 248($26)\n"
	"mfc0 $8, $14\n"
	"sw $8, 256($26)\n"
	"mfc0 $8, $13\n"
	"sw $8, 260($26)\n"
	"mfc0 $8, $8\n"
	"sw $8, 264($26)\n"
	"mfc0 $8, $12\n"
	"sw $8, 268($26)\n"
	"lui $29, %hi(ps2_crash_stack + 16384 - 64)\n"
	"addiu $29, $29, %lo(ps2_crash_stack + 16384 - 64)\n"
	"lui $8, %hi(ps2_crash_report)\n"
	"addiu $8, $8, %lo(ps2_crash_report)\n"
	"mtc0 $8, $14\n"
	"sync.p\n"
	"eret\n"
	"nop\n"
	".size ps2_crash_vector, .-ps2_crash_vector\n"
	".set pop\n");

static void say(const char *format, ...)
{
	va_list arguments;
	int room = REPORT_SIZE - report_length;

	if (room > 1)
	{
		int n;

		va_start(arguments, format);
		n = vsnprintf(report + report_length, room, format, arguments);
		va_end(arguments);
		if (n > 0)
		{
			report_length += n < room ? n : room - 1;
		}
	}
}

static const char *exception_name(unsigned code)
{
	switch (code)
	{
	case 1: return "TLB modified";
	case 2: return "TLB load/instruction fetch";
	case 3: return "TLB store";
	case 4: return "address error, load/instruction fetch";
	case 5: return "address error, store";
	case 6: return "bus error, instruction fetch";
	case 7: return "bus error, data";
	case 10: return "reserved instruction";
	case 12: return "arithmetic overflow";
	case 13: return "trap";
	}
	return "exception";
}

void ps2_crash_report(void)
{
	static const char *const names[32] = {
		"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
		"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
	const struct crash_state *s = &ps2_crash_state;
	const u32 *stack = (const u32 *)(u32)s->gpr[29];
	unsigned i, listed = 0;
	int file;

	if (crashed)
	{
		/* a second fault while reporting: nothing more can be done */
		for (;;)
		{
			SleepThread();
		}
	}
	crashed = 1;

	say("EE exception: %s (code %u) in thread %d\n", exception_name((s->cause >> 2) & 31),
		(s->cause >> 2) & 31, GetThreadId());
	say("EPC %08x  Cause %08x  BadVAddr %08x  Status %08x\n", s->epc, s->cause, s->bad_address, s->status);
	if (s->cause & 0x80000000)
	{
		say("the faulting instruction is in a branch delay slot: EPC is the branch, the fault is at EPC+4\n");
	}
	for (i = 0; i < 32; i += 2)
	{
		say("%-4s %08x  %-4s %08x\n", names[i], (u32)s->gpr[i], names[i + 1], (u32)s->gpr[i + 1]);
	}
	say("code addresses on the stack above sp, nearest first:\n");
	if (((u32)stack & 3) == 0 && (u32)stack >= 0x100000 && (u32)stack < 0x2000000)
	{
		for (i = 0; i < STACK_SCAN_WORDS && listed < STACK_SCAN_LISTED && (u32)(stack + i) < 0x2000000; i++)
		{
			u32 word = stack[i];

			if (word >= CODE_BEGIN && word < CODE_END && (word & 3) == 0)
			{
				say("%08x%s", word, ++listed % 6 == 0 ? "\n" : " ");
			}
		}
	}
	say("\n");

	file = open("host:crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (file >= 0)
	{
		write(file, report, report_length);
		close(file);
	}
	fputs(report, stderr);

	for (;;)
	{
		SleepThread();
	}
}

/* The self-test: configure.py with HALO_PS2_CRASH_TEST=null, unaligned or
trap makes the build fault once, right after the handlers are in, so
that a report can be read (PCSX2 handles a null pointer's TLB refill and
ignores an unaligned access itself: only hardware tests those two). */
#if defined(HALO_PS2_CRASH_TEST_NULL) || defined(HALO_PS2_CRASH_TEST_UNALIGNED) || defined(HALO_PS2_CRASH_TEST_TRAP)
static void ps2_crash_self_test(void)
{
	volatile unsigned *pointer;

#if defined(HALO_PS2_CRASH_TEST_NULL)
	pointer = (volatile unsigned *)0;
	*pointer = 1;
#elif defined(HALO_PS2_CRASH_TEST_UNALIGNED)
	static unsigned char bytes[16] __attribute__((aligned(16)));
	/* the address goes through a volatile so that the compiler cannot see it
	is odd and write the store as swl/swr, which never faults */
	volatile unsigned long address = (unsigned long)bytes + 1;

	pointer = (volatile unsigned *)address;
	*pointer = 1;
#else
	__asm__ volatile("teq $0, $0");
#endif
	(void)pointer;
}
#endif

/* before main, with the other constructors: ps2link's handlers are replaced */
static void __attribute__((constructor)) ps2_crash_install(void)
{
	static const int codes[] = {1, 2, 3, 4, 5, 6, 7, 10, 12, 13};
	unsigned i;

	for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++)
	{
		SetVCommonHandler(codes[i], (void *)ps2_crash_vector);
	}
	/* a load or store through an unmapped address (a null pointer) is a TLB refill */
	SetVTLBRefillHandler(2, (void *)ps2_crash_vector);
	SetVTLBRefillHandler(3, (void *)ps2_crash_vector);
#if defined(HALO_PS2_CRASH_TEST_NULL) || defined(HALO_PS2_CRASH_TEST_UNALIGNED) || defined(HALO_PS2_CRASH_TEST_TRAP)
	ps2_crash_self_test();
#endif
}
