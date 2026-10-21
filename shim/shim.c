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

static uint64_t hvc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("hvc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/*
 * SMP probe (boot-arg-free: built in, runs before the loader). A started
 * core records its MPIDR and exception level, then switches itself off
 * again through the same conduit, so xnu can CPU_ON it later.
 */
#define PSCI_CPU_ON	0xc4000003UL
#define PSCI_CPU_OFF	0x84000002UL
extern char secondary_entry[];
static volatile uint64_t sec_seen[8], sec_el[8], sec_conduit;

void secondary_main(uint64_t idx)
{
	idx &= 7;
	sec_el[idx] = rsr("CurrentEL") >> 2;
	sec_seen[idx] = rsr("mpidr_el1") | (1ULL << 63);
	__asm__ volatile("dsb sy" ::: "memory");
	if (sec_conduit)
		hvc(PSCI_CPU_OFF, 0, 0, 0);
	else
		smc(PSCI_CPU_OFF, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

static void wait_ms(unsigned ms)
{
	uint64_t f = rsr("cntfrq_el0");
	if (f == 0)
		f = 26000000;
	uint64_t end = now() + f / 1000 * ms;
	while (now() < end)
		;
}

static void smp_probe(void)
{
	uintptr_t entry = (uintptr_t)secondary_entry;
	con_hex("SEC ENTRY", entry);
	/*
	 * Boot MPIDR is 0x81000000: MT=1, so the core number is in Aff1. Try
	 * the DT-style target (core in Aff0) and the MT-style one (Aff1), each
	 * through hvc (the DT's PSCI conduit) and smc.
	 */
	for (int i = 1; i <= 3; i++) {
		const uint64_t targets[2] = { (uint64_t)i, (uint64_t)i << 8 };
		for (int t = 0; t < 2 && !sec_seen[i]; t++) {
			for (int c = 1; c >= 0 && !sec_seen[i]; c--) {
				char l[] = "CPUx t... ...";
				l[3] = (char)('0' + i);
				l[6] = t ? '1' : '0';
				l[7] = t ? '0' : '0';
				l[8] = t ? '0' : (char)('0' + i);
				if (t) l[6] = (char)('0' + i);
				l[10] = c ? 'h' : 's';
				l[11] = c ? 'v' : 'm';
				l[12] = 'c';
				sec_conduit = (uint64_t)c;
				__asm__ volatile("dsb sy" ::: "memory");
				uint32_t rc = (uint32_t)(c ? hvc(PSCI_CPU_ON, targets[t], entry, (uint64_t)i)
				                            : smc(PSCI_CPU_ON, targets[t], entry, (uint64_t)i));
				wait_ms(300);
				con_hex2(l, "rc=", rc, "seen=", (uint32_t)sec_seen[i] | (uint32_t)(sec_el[i] << 28));
			}
		}
	}
	con_str("SMP", "probe done, xnu in 20s");
	wait_ms(20000);
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
	stage = "smp probe";
	smp_probe();
	con_skip();
	stage = "loader";
	const char *why = loader_boot(dtb);
	/* Only reached on error. */
	con_str("LOADER", why);
	stage = "done";
	reset(60);
}
