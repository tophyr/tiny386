/*
 * A step count bounds the work cpui386_step does, REP iterations included.
 *
 * A real-mode program fills 65535 bytes with rep stosb and then counts up in
 * BX. The test calls cpui386_step once from the REP, raises an interrupt, and
 * steps again; the handler records BX. The REP is far longer than the step
 * count, so no instruction after it may run in that first step, and the handler
 * must see BX = 0. A CPU that counts the REP as one step goes on to run the BX
 * loop for the rest of the step count first. The test also checks that the
 * fill completes and that cpu->cycle counts the iterations.
 */

#include <stdio.h>
#include <string.h>

#include "i386.h"

#define MEM_SIZE (2 << 20)
#define CODE 0x7c00
#define CLD_IP (CODE + 0x18) /* the cld before rep stosb in code[] */
#define HANDLER 0x7e00
#define FILL 0x10000         /* ES = 0x1000, DI = 0 */
#define COUNT 0xffff
#define STEPS 1000

static char mem[MEM_SIZE];

static const unsigned char code[] = {
	0x31, 0xc0,             /* xor ax, ax */
	0x8e, 0xd8,             /* mov ds, ax */
	0x8e, 0xd0,             /* mov ss, ax */
	0xbc, 0x00, 0x70,       /* mov sp, 0x7000 */
	0xb8, 0x00, 0x10,       /* mov ax, 0x1000 */
	0x8e, 0xc0,             /* mov es, ax */
	0x31, 0xff,             /* xor di, di */
	0x31, 0xdb,             /* xor bx, bx */
	0xb9, 0xff, 0xff,       /* mov cx, 0xffff */
	0xb0, 0x55,             /* mov al, 0x55 */
	0xfb,                   /* sti; the next instruction runs in the same step */
	0xfc,                   /* cld */
	0xf3, 0xaa,             /* rep stosb */
	0x89, 0x0e, 0x00, 0x05, /* mov [0x500], cx */
	0x43,                   /* inc bx */
	0xeb, 0xfd,             /* jmp to the inc bx */
};

static const unsigned char handler[] = {
	0x89, 0x1e, 0x02, 0x05, /* mov [0x502], bx */
	0xfe, 0x06, 0x04, 0x05, /* inc byte [0x504] */
	0xcf,                   /* iret */
};

static int read_irq(void *pic)
{
	(void) pic;
	return 8;
}

static unsigned word(unsigned addr)
{
	return (unsigned char) mem[addr] | (unsigned char) mem[addr + 1] << 8;
}

static unsigned ip(CPUI386 *cpu)
{
	CPUI386_State st;
	cpui386_get_state(cpu, &st);
	return st.ip;
}

int main(void)
{
	CPU_CB *cb;
	CPUI386 *cpu = cpui386_new(3, mem, MEM_SIZE, &cb);
	cb->pic_read_irq = read_irq;

	static const unsigned char reset[] = { 0xea, 0x00, 0x7c, 0x00, 0x00 }; /* jmp 0:0x7c00 */
	memcpy(mem + 0xffff0, reset, sizeof(reset));
	memcpy(mem + CODE, code, sizeof(code));
	memcpy(mem + HANDLER, handler, sizeof(handler));
	mem[8 * 4] = HANDLER & 0xff;  /* interrupt 8 -> 0:HANDLER */
	mem[8 * 4 + 1] = HANDLER >> 8;
	mem[0x500] = mem[0x501] = 0xff; /* CX after the REP; 0 once written */
	cpui386_reset(cpu);

	/* Run one step at a time until the cld has run; the REP is next.
	   cpui386_get_state reports the address of the last instruction run. */
	for (int i = 0; i < 100 && ip(cpu) != CLD_IP; i++)
		cpui386_step(cpu, 1);
	if (ip(cpu) != CLD_IP) {
		printf("FAIL: did not reach the REP instruction\n");
		return 1;
	}

	long cycle = cpui386_get_cycle(cpu);
	cpui386_step(cpu, STEPS);
	cpui386_raise_irq(cpu);
	for (int i = 0; i < 1000 && mem[0x504] == 0; i++)
		cpui386_step(cpu, STEPS);
	for (int i = 0; i < 1000 && word(0x500) != 0; i++)
		cpui386_step(cpu, STEPS);

	int failures = 0;
	unsigned bx_in_handler = word(0x502);
	if (mem[0x504] != 1) {
		printf("FAIL: the interrupt handler ran %d times, not once\n", mem[0x504]);
		failures++;
	} else if (bx_in_handler != 0) {
		printf("FAIL: %u iterations of the loop after the REP ran before the interrupt; "
		       "the step count ran out during the REP\n", bx_in_handler);
		failures++;
	}
	if (word(0x500) != 0) {
		printf("FAIL: CX = %u after the REP\n", word(0x500));
		failures++;
	}
	for (unsigned i = 0; i < COUNT; i++) {
		if ((unsigned char) mem[FILL + i] != 0x55) {
			printf("FAIL: byte %u of the fill is %#x\n", i, (unsigned char) mem[FILL + i]);
			failures++;
			break;
		}
	}
	if ((unsigned char) mem[FILL + COUNT] != 0) {
		printf("FAIL: the fill ran past its end\n");
		failures++;
	}
	if (cpui386_get_cycle(cpu) - cycle < COUNT) {
		printf("FAIL: the cycle count grew by %ld for %u iterations\n",
		       cpui386_get_cycle(cpu) - cycle, COUNT);
		failures++;
	}
	if (failures == 0)
		printf("ok: the interrupt came before any instruction after the REP\n");
	return failures != 0;
}
