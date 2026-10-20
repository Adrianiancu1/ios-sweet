/*
 * y2s boot shim for the Galaxy S20+ (SM-G986B, Exynos 990).
 *
 * S-Boot (unchanged) loads boot.img like an Android kernel: this image is
 * the "kernel", the boot.img ramdisk is the xnu payload (tools/mkpayload.py).
 * Entry at EL1 (Samsung's hypervisor owns EL2), MMU and D-cache off,
 * x0 = DTB with /chosen linux,initrd-start/end. loader.c then acts as iBoot.
 *
 * On any error the reason stays on screen for 60 s, then the phone resets.
 * An unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"

#define PSCI_SYSTEM_RESET	0x84000009UL
/* PMU SWRESET (Exynos 990 PMU 0x15860000 + 0x3a00, bit 1) */
#define PMU_SWRESET		0x15863a00UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static const char *stage;	/* set at runtime: no absolute pointers in data */

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	__asm__ volatile("isb" ::: "memory");
	return rsr("cntpct_el0");
}

static void delay_s(unsigned s)
{
	uint64_t f = rsr("cntfrq_el0");
	if (f == 0)
		f = 26000000;	/* S20 timer clock (live.dts) */
	uint64_t end = now() + f * s;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s >= 60 ? "in 60s" : "in 20s");
	delay_s(after_s);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	/* PSCI returned: direct PMU software reset. */
	volatile uint32_t *sw = (volatile uint32_t *)PMU_SWRESET;
	*sw |= 2;
	__asm__ volatile("dsb sy" ::: "memory");
	for (;;)
		__asm__ volatile("wfe");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_clear(BLUE);
	/* One string + offset: a switch can become a table of absolute pointers. */
	static const char kinds[] = "SYNC\0\0\0IRQ\0\0\0\0FIQ\0\0\0\0SERROR";
	con_str("PANIC", kinds + 7 * (slot & 3));
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(60);
}

/* No secondaries are started yet; keep the symbol start.S references. */
void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main(uint64_t dtb)
{
	stage = "loader";
	con_clear(RED);
	con_str("SHIM", "y2s (Galaxy S20+): xnu loader");
	con_hex2("EL", "cur=", rsr("CurrentEL") >> 2, "mpidr=", rsr("mpidr_el1"));
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(*(volatile uint32_t *)dtb) : 0);
	con_hex2("TIMER", "frq=", rsr("cntfrq_el0"), 0, 0);
	con_skip();
	const char *why = loader_boot(dtb);
	/* Only reached on error. */
	con_str("LOADER", why);
	stage = "done";
	reset(60);
}
