// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * CPU clocks through the SDM670/710 cpufreq hardware (Linux qcom-cpufreq-hw, "qcom,sdm670-cpufreq-hw"): one register window
 * per cluster, 0x17d43000 for the Cortex-A55s (cpu0-5) and 0x17d45800 for the Cortex-A75s (cpu6-7). The firmware fills each
 * window's frequency table; the OS asks for a level by writing its index to PERF_STATE, and the hardware sets voltage, clock and
 * its own limits. Nothing in this port does that otherwise, so the A75s stay at the 300 MHz the boot chain left (index 0).
 *
 * sirius-cpufreq=1 requests the highest table entry that is not a boost (single-core turbo) entry in both clusters, once, at
 * EARLY_BOOT. Without the argument the table is only read. The results stay in sirius_cpufreq_report() for the USB heartbeat
 * (package/NativeEmmc.cpp), because the kernel console does not reach the USB ring this early.
 */
#include <stdint.h>
#include <kern/startup.h>
#include <pexpert/pexpert.h>
#include <mach/vm_types.h>

extern vm_offset_t ml_io_map(vm_offset_t phys, unsigned long size);

#define CPUFREQ_WINDOW      0x1400u
#define CPUFREQ_ENABLE      0x000u      /* bit 0: the firmware runs this domain in hardware */
#define CPUFREQ_FREQ_LUT    0x110u      /* row i at 0x110 + 32 * i: [31:30] source, [18:16] core count, [7:0] L value */
#define CPUFREQ_CURRENT     0x704u      /* [9:0] L value of the current vote */
#define CPUFREQ_PERF_STATE  0x920u      /* the requested table index */
#define CPUFREQ_LUT_ROW     32u
#define CPUFREQ_LUT_ROWS    40u
#define CPUFREQ_XO_KHZ      19200u      /* source 1: L value x XO */
#define CPUFREQ_ALT_KHZ     300000u     /* source 0: GPLL0 / 2 */
#define CPUFREQ_TURBO_CORES 1u          /* a row for one core only is a boost entry */

static const uint64_t cpufreq_domain_pa[2] = { 0x17d43000ULL, 0x17d45800ULL };
static volatile uint32_t *cpufreq_base[2];
static unsigned cpufreq_mode;
static uint32_t cpufreq_best[2] = { 0xff, 0xff };

/* Per domain: [0] enable bit, [1] table rows, [2] index before, [3] index requested (0xff = none), [4] kHz of row 0,
 * [5] kHz of the chosen (highest non-boost) row. */
static volatile uint32_t cpufreq_state[2][6];

static uint32_t
cpufreq_read(unsigned domain, uint32_t offset)
{
	return cpufreq_base[domain][offset / 4];
}

static void
sirius_cpufreq_start(void)
{
	unsigned mode = 0;

	PE_parse_boot_argn("sirius-cpufreq", &mode, sizeof(mode));
	cpufreq_mode = mode;
	for (unsigned domain = 0; domain < 2; domain++) {
		uint32_t previous = 0, rows = 0, best = 0xff, bestKhz = 0, firstKhz = 0;

		cpufreq_state[domain][3] = 0xff;
		cpufreq_base[domain] = (volatile uint32_t *)ml_io_map((vm_offset_t)cpufreq_domain_pa[domain], CPUFREQ_WINDOW);
		if (cpufreq_base[domain] == NULL) {
			continue;
		}
		cpufreq_state[domain][0] = cpufreq_read(domain, CPUFREQ_ENABLE) & 1u;
		for (uint32_t row = 0; row < CPUFREQ_LUT_ROWS; row++) {
			uint32_t data = cpufreq_read(domain, CPUFREQ_FREQ_LUT + row * CPUFREQ_LUT_ROW);
			uint32_t khz = (data >> 30) ? CPUFREQ_XO_KHZ * (data & 0xffu) : CPUFREQ_ALT_KHZ;

			/* Two equal rows end the table (Linux qcom_cpufreq_hw_read_lut). */
			if (row > 0 && khz == previous) {
				break;
			}
			if (row == 0) {
				firstKhz = khz;
			}
			if (((data >> 16) & 7u) != CPUFREQ_TURBO_CORES) {
				best = row;
				bestKhz = khz;
			}
			previous = khz;
			rows = row + 1;
		}
		cpufreq_state[domain][1] = rows;
		cpufreq_state[domain][2] = cpufreq_read(domain, CPUFREQ_PERF_STATE);
		cpufreq_state[domain][4] = firstKhz;
		cpufreq_state[domain][5] = bestKhz;
		cpufreq_best[domain] = best;
		if (mode == 1 && cpufreq_state[domain][0] && best != 0xff) {
			cpufreq_base[domain][CPUFREQ_PERF_STATE / 4] = best;
			__asm__ volatile ("dsb sy" ::: "memory");
			cpufreq_state[domain][3] = best;
		}
	}
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, sirius_cpufreq_start);

/*
 * The A75 domain reads enable 0 at EARLY_BOOT, yet under Linux on this phone the same hardware runs the A75s at 2208 MHz, so the
 * domain is likely enabled only once cpu6-7 are up. The heartbeat calls this after the secondary cores have started: it refreshes
 * the enable bit and makes the request a domain did not get at EARLY_BOOT.
 */
void
sirius_cpufreq_retry(void)
{
	for (unsigned domain = 0; domain < 2; domain++) {
		if (cpufreq_base[domain] == NULL) {
			continue;
		}
		cpufreq_state[domain][0] = cpufreq_read(domain, CPUFREQ_ENABLE) & 1u;
		if (cpufreq_mode == 1 && cpufreq_state[domain][0] && cpufreq_state[domain][3] == 0xff && cpufreq_best[domain] != 0xff) {
			cpufreq_base[domain][CPUFREQ_PERF_STATE / 4] = cpufreq_best[domain];
			__asm__ volatile ("dsb sy" ::: "memory");
			cpufreq_state[domain][3] = cpufreq_best[domain];
		}
	}
}

/* For the heartbeat: the six state words of a domain and the MHz of its current vote (0 when the window is not mapped). */
int
sirius_cpufreq_report(unsigned domain, uint32_t state[6], uint32_t *vote_mhz)
{
	if (domain > 1) {
		return 0;
	}
	for (unsigned i = 0; i < 6; i++) {
		state[i] = cpufreq_state[domain][i];
	}
	*vote_mhz = cpufreq_base[domain] ? (cpufreq_read(domain, CPUFREQ_CURRENT) & 0x3ffu) * CPUFREQ_XO_KHZ / 1000u : 0;
	return 1;
}
