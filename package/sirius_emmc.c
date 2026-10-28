/*
 * Sirius eMMC bring-up: the SDCC1 controller at 0x7c4000 as an SDHCI host.
 *
 * The board stores its boot images on eMMC, not UFS, so the boot chain has already identified the
 * card and left SDCC1 programmed: the card is in the transfer state and the host controller has a
 * card clock, a bus width and power. That makes the first milestone cheap - read registers, then
 * read a block over PIO, which needs no DMA and no SMMU programming at all.
 *
 * Register layout: SD Host Controller Simplified Specification 3.0. Values are accessed with the
 * width the specification gives each register (8, 16 or 32 bits).
 */

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef unsigned long vm_offset_t;
typedef unsigned long long uint64_t;
typedef uint64_t addr64_t;
typedef int boolean_t;
#define TRUE 1
typedef _Bool bool;
#define true 1
#define false 0

extern vm_offset_t ml_io_map(vm_offset_t phys, unsigned long size);
/* osfmk/pexpert: reads a boot argument value; the C++ side includes pexpert.h, this file does not. */
extern int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, unsigned int max_len);
/* Defined further down with the rest of the ADMA2 read path; the clock switch above uses it to
 * prove the new rate before anything depends on it. */
static int sdhci_read_block_adma2(uint32_t lba, uint8_t *out);
/* The probe reports every step of every transfer into the ring. Once the block driver takes over,
 * that reporting costs more than the transfer itself (five lines per 512-byte block), so
 * sirius_emmc_prepare() turns it off and only failures are reported from then on. */
static bool emmc_trace = true;
/* A spinning delay on purpose: delay() sleeps, and while the CPU is idle the GCC auto clock gating
 * turns the SDCC AHB clock off, after which every register reads back zero and no command ever
 * completes. */
static void
spin_us(unsigned usec)
{
	volatile unsigned ticks = usec * 100u;
	while (ticks > 0) {
		ticks--;
	}
}
extern void sirius_usb_log_char(char c);

/* The kernel console stops feeding the ring long before bsd_init prints anything, so a report that
 * only goes through printf never reaches the host. Writing the characters into the USB ring
 * directly makes the probe result part of the snapshot the host takes when it attaches. */
static void
ring_puts(const char *text)
{
	while (*text != '\0') {
		sirius_usb_log_char(*text++);
	}
}

static void
ring_hex(uint32_t value, unsigned digits)
{
	char buffer[9];
	static const char hex[] = "0123456789abcdef";
	if (digits > 8) {
		digits = 8;
	}
	for (unsigned i = 0; i < digits; i++) {
		buffer[digits - 1 - i] = hex[(value >> (4 * i)) & 0xfu];
	}
	buffer[digits] = '\0';
	ring_puts(buffer);
}

static void
ring_dec(uint32_t value)
{
	char buffer[11];
	unsigned i = 0;
	if (value == 0) {
		ring_puts("0");
		return;
	}
	while (value != 0) {
		buffer[i++] = (char)('0' + (value % 10));
		value /= 10;
	}
	while (i > 0) {
		sirius_usb_log_char(buffer[--i]);
	}
}

#define GCC_PA 0x00100000u
#define GCC_SIZE 0x00090000u

#define SDCC1_PA 0x007c4000u
#define SDCC1_SIZE 0x1000u

/* GCC branches that clock the controller. Offsets from gcc-sdm845.c, same block layout as sdm670. */
#define GCC_SDCC1_APPS_CBCR 0x26004u
#define GCC_SDCC1_AHB_CBCR 0x26008u
#define GCC_SDCC1_ICE_CBCR 0x2600cu
#define GCC_AGGRE_UFS_PHY_AXI_CBCR 0x82024u

/* SDCC1 apps RCG (cmd_rcgr 0x26028): CFG at +4 reports the selected source, the D/M/N counters
 * live at +8/+12/+16. The card clock on Qualcomm comes from here, not from the SDHCI divider. */
#define GCC_SDCC1_APPS_CMD 0x26028u
#define GCC_SDCC1_APPS_CFG 0x2602cu
#define GCC_SDCC1_APPS_M 0x26030u
#define GCC_SDCC1_APPS_N 0x26034u
#define GCC_SDCC1_APPS_D 0x26038u

/* Qualcomm SDHCI vendor registers (sdhci-msm.c): CORE_HC_MCLK_SEL and the pad power switch. */
/* sdhci-msm v5 vendor offsets (sdhci_msm_v5_offset). The 0x10c used earlier belongs to the older
 * MCI layout, which is why it read back zero. */
#define MSM_CORE_MCI_VERSION 0x318u
#define MSM_CORE_DLL_CONFIG 0x200u
#define MSM_CORE_DLL_STATUS 0x208u
#define MSM_CORE_VENDOR_SPEC 0x20cu
#define MSM_CORE_PWRCTL_STATUS 0x240u
#define MSM_CORE_VENDOR_SPEC3 0x250u
#define MSM_CORE_DLL_CONFIG_2 0x254u
#define MSM_CORE_DLL_USR_CTL 0x388u

/* SDHCI registers, byte offsets within the 4 KiB "hc" window. */
#define SDHCI_BLOCK_SIZE 0x04u
#define SDHCI_BLOCK_COUNT 0x06u
#define SDHCI_ARGUMENT 0x08u
#define SDHCI_TRANSFER_MODE 0x0cu
#define SDHCI_COMMAND 0x0eu
#define SDHCI_RESPONSE 0x10u
#define SDHCI_BUFFER 0x20u
#define SDHCI_PRESENT_STATE 0x24u
#define SDHCI_POWER_CONTROL 0x29u
#define SDHCI_HOST_CONTROL 0x28u
#define SDHCI_TIMEOUT_CONTROL 0x2eu
#define SDHCI_HOST_CONTROL2 0x3eu
#define SDHCI_DMA_ADDRESS 0x00u
#define SDHCI_CLOCK_CONTROL 0x2cu
#define SDHCI_SOFTWARE_RESET 0x2fu
#define SDHCI_INT_ENABLE 0x34u
#define SDHCI_INT_STATUS 0x30u
/* SDHCI 3.0 lays the interrupt registers out as: 0x30 NIS (bit15 = error summary),
 * 0x32 EIS, 0x34 NISE, 0x36 EISE, 0x38 NSIGEN, 0x3a ESIGEN, 0x3c Auto-CMD12 error status.
 * The earlier defines pointed at 0x38/0x3c, so every "err=" printed the signal-enable register
 * we ourselves had written 0xffff into, and the real error status was never read at all. */
#define SDHCI_ERR_INT_STATUS 0x32u
#define SDHCI_ERR_INT_ENABLE 0x36u
#define SDHCI_SIGNAL_ENABLE 0x38u
#define SDHCI_ERR_SIGNAL_ENABLE 0x3au
#define SDHCI_ADMA_ERROR_STATUS 0x54u
#define SDHCI_ADMA_ADDRESS 0x58u
#define SDHCI_ADMA_ADDRESS_HI 0x5cu
#define SDHCI_CAPABILITIES 0x40u
#define SDHCI_VERSION 0xfeu

#define SDHCI_RESET_ALL 0x01u
#define SDHCI_RESET_CMD 0x02u
#define SDHCI_RESET_DATA 0x04u

/* Present state bits used below. */
#define SDHCI_PS_CMD_INHIBIT (1u << 0)
#define SDHCI_PS_DAT_INHIBIT (1u << 1)
#define SDHCI_PS_CARD_INSERTED (1u << 16)
#define SDHCI_PS_BUFFER_READ_ENABLE (1u << 11)

/* Interrupt status bits. */
#define SDHCI_INT_CMD_COMPLETE (1u << 0)
#define SDHCI_INT_TRANSFER_COMPLETE (1u << 1)
#define SDHCI_INT_BUFFER_READ_READY (1u << 5)
#define SDHCI_INT_ERROR (1u << 15)

/* Command register fields. */
#define SDHCI_CMD_RESP_NONE (0u << 0)
#define SDHCI_CMD_RESP_136 (1u << 0)
#define SDHCI_CMD_RESP_48 (2u << 0)
#define SDHCI_CMD_RESP_48_BUSY (3u << 0)
#define SDHCI_CMD_CRC_CHECK (1u << 3)
#define SDHCI_CMD_INDEX_CHECK (1u << 4)
#define SDHCI_CMD_DATA_PRESENT (1u << 5)

/* Transfer mode fields. */
#define SDHCI_TM_READ (1u << 4)
#define SDHCI_TM_BLOCK_COUNT_ENABLE (1u << 1)

/* Clock control bits. */
#define SDHCI_CLK_INTERNAL_EN (1u << 0)
#define SDHCI_CLK_INTERNAL_STABLE (1u << 1)
#define SDHCI_CLK_SD_EN (1u << 2)

volatile uint32_t sirius_emmc_fail_int;
volatile uint32_t sirius_emmc_fail_err;
volatile uint32_t sirius_emmc_fail_state;
volatile uint32_t sirius_emmc_fail_resp;
volatile uint32_t sirius_emmc_fail_reset;
volatile uint32_t sirius_emmc_after_cmd_ver;
volatile uint32_t sirius_emmc_after_cmd_clk;
volatile uint32_t sirius_emmc_before_cmd_state;
volatile uint32_t sirius_emmc_dead_ver;
volatile uint32_t sirius_emmc_dead_clk;
volatile uint32_t sirius_emmc_dead_pwr;
volatile uint32_t sirius_emmc_dead_halts;
volatile uint32_t sirius_emmc_fail_clk;

static vm_offset_t emmc_base;
static vm_offset_t gcc_base;

static uint8_t
sdhci_read8(uint32_t offset)
{
	return *(volatile uint8_t *)(emmc_base + offset);
}

static uint16_t
sdhci_read16(uint32_t offset)
{
	return *(volatile uint16_t *)(emmc_base + offset);
}

static uint32_t
sdhci_read32(uint32_t offset)
{
	return *(volatile uint32_t *)(emmc_base + offset);
}

static void
sdhci_write8(uint32_t offset, uint8_t value)
{
	*(volatile uint8_t *)(emmc_base + offset) = value;
}

static void
sdhci_write16(uint32_t offset, uint16_t value)
{
	*(volatile uint16_t *)(emmc_base + offset) = value;
}

static void
sdhci_write32(uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(emmc_base + offset) = value;
}

static uint32_t
gcc_read(uint32_t offset)
{
	if (gcc_base == 0) {
		return 0xffffffffu;
	}
	return *(volatile uint32_t *)(gcc_base + offset);
}

/* Command and PIO helpers. Polling, no DMA: the first milestone only has to prove the card answers
 * and that a block can be read, and polling keeps the SMMU and the ADMA engine out of the picture. */
static int
sdhci_wait(uint32_t mask, unsigned timeout_us)
{
	for (unsigned waited = 0; waited < timeout_us; waited += 10) {
		/* Only the requested completion bits are waited for. The error summary bit in INT_STATUS
		 * latches spuriously on this controller (the error status register reads all ones), and
		 * bailing out on it aborted commands that were still on their way to completing. */
		uint16_t status = sdhci_read16(SDHCI_INT_STATUS);
		if ((status & mask) != 0) {
			return (int)(status & mask);
		}
		spin_us(10);
	}
	return 0;
}

static int
sdhci_command_capture(uint16_t command, uint32_t argument, uint32_t *response)
{
	unsigned waited = 0;
	while ((sdhci_read32(SDHCI_PRESENT_STATE) & (SDHCI_PS_CMD_INHIBIT | SDHCI_PS_DAT_INHIBIT)) != 0) {
		if (waited > 50000) {
			/* Something left the command line wedged (a previous command that never
			 * completed). Reset the command and data circuits, as the kernel does on timeout,
			 * and carry on - otherwise every later command is refused as well. */
			sdhci_write8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
			for (unsigned spin = 0; spin < 10000; spin++) {
				if ((sdhci_read8(SDHCI_SOFTWARE_RESET) & (SDHCI_RESET_CMD | SDHCI_RESET_DATA)) == 0) {
					break;
				}
				spin_us(10);
			}
			break;
		}
		spin_us(10);
		waited += 10;
	}
	/* One 32-bit write clears both halves: NIS (0x30) and EIS (0x32). Clearing EIS matters
	 * because its bits keep the NIS error summary latched forever. */
	sdhci_write32(SDHCI_INT_STATUS, 0xffffffffu);
	sdhci_write32(SDHCI_ARGUMENT, argument);
	sdhci_write16(SDHCI_COMMAND, command);
	/* A 136-bit identification response may take longer than the 100 ms used for short commands:
	 * wait a full second before declaring the command lost. */
	unsigned timeout = (command & 0x3u) == SDHCI_CMD_RESP_136 ? 1000000u : 100000u;
	if (sdhci_wait(SDHCI_INT_CMD_COMPLETE, timeout) <= 0) {
		return -1;
	}
	if (response != 0) {
		for (unsigned i = 0; i < 4; i++) {
			response[i] = sdhci_read32(SDHCI_RESPONSE + 4u * i);
		}
	}
	sdhci_write16(SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);
	return 0;
}

static int
sdhci_command(uint16_t command, uint32_t argument)
{
	/* Nothing may be in flight: a command left over from an earlier attempt would otherwise be
	 * reported as ours. */
	for (unsigned waited = 0; waited < 100000; waited += 10) {
		if ((sdhci_read32(SDHCI_PRESENT_STATE) & (SDHCI_PS_CMD_INHIBIT | SDHCI_PS_DAT_INHIBIT)) == 0) {
			break;
		}
		spin_us(10);
	}
	sdhci_write32(SDHCI_INT_STATUS, 0xffffffffu);
	sdhci_write32(SDHCI_ARGUMENT, argument);
	sirius_emmc_before_cmd_state = sdhci_read32(SDHCI_PRESENT_STATE);
	sdhci_write16(SDHCI_COMMAND, command);
	/* Read two registers that must not change - the version is a constant and the clock control
	 * was just programmed - to tell a dead register window from a rejected command. */
	sirius_emmc_after_cmd_ver = sdhci_read16(SDHCI_VERSION);
	sirius_emmc_after_cmd_clk = sdhci_read16(SDHCI_CLOCK_CONTROL);
	if (sdhci_wait(SDHCI_INT_CMD_COMPLETE, 100000) <= 0) {
		sirius_emmc_fail_int = sdhci_read16(SDHCI_INT_STATUS);
		sirius_emmc_fail_err = sdhci_read16(SDHCI_ERR_INT_STATUS);
		sirius_emmc_fail_state = sdhci_read32(SDHCI_PRESENT_STATE);
		sirius_emmc_fail_resp = sdhci_read32(SDHCI_RESPONSE);
		sirius_emmc_fail_reset = sdhci_read8(SDHCI_SOFTWARE_RESET);
		sirius_emmc_fail_clk = sdhci_read16(SDHCI_CLOCK_CONTROL);
		sirius_emmc_dead_ver = sdhci_read16(SDHCI_VERSION);
		sirius_emmc_dead_clk = sdhci_read16(SDHCI_CLOCK_CONTROL);
		sirius_emmc_dead_pwr = sdhci_read8(SDHCI_POWER_CONTROL);
		sirius_emmc_dead_halts = ((gcc_read(GCC_SDCC1_APPS_CBCR) >> 31) & 1u) |
		    (((gcc_read(GCC_SDCC1_AHB_CBCR) >> 31) & 1u) << 1) |
		    (((gcc_read(GCC_SDCC1_ICE_CBCR) >> 31) & 1u) << 2) |
		    (((gcc_read(GCC_AGGRE_UFS_PHY_AXI_CBCR) >> 31) & 1u) << 3);
		return -1;
	}
	sdhci_write16(SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);
	return 0;
}

/* The SDCC1 root clock was left in MND mode with empty counters (m=0 n=0 d=0xff), so the card
 * clock was meaningless and every command timed out. Program 400 kHz the way the Linux table does
 * for identification -BI_TCXO (19.2 MHz) / 12 * 1/4- by writing the MND registers and pulsing the
 * update bit. */
static int
emmc_set_card_clock_400k(void)
{
	if (gcc_base == 0) {
		return -1;
	}
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_CMD) = 0;      /* root disabled while editing */
	spin_us(10);
	/* src_sel = 0 (bi_tcxo), mode = MND (0), divider field unused in MND mode */
	/* Encode it the way clk-rcg2/U-Boot do, not with raw M/N/D: CFG carries the pre-divider and
	 * the mode, and N/D are stored complemented. CFG=0 would mean integer-divider mode, which
	 * bypasses M/N entirely - that is why the card ended up on ~19 MHz instead of 400 kHz. */
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_CFG) = 0x2017u;   /* pre_div 23, DUAL_EDGE, src 0 */
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_M) = 1u;
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_N) = 0xfcu;       /* ~(4 - 1) */
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_D) = 0xfbu;       /* ~4 */
	spin_us(10);
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_CMD) = 1u;     /* update */
	for (unsigned waited = 0; waited < 100000; waited += 100) {
		if ((gcc_read(GCC_SDCC1_APPS_CMD) & 1u) == 0) {
			break;
		}
		spin_us(100);
	}
	/* Make sure the branch is on; the controller only sees the clock when both are set. */
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_CBCR) = gcc_read(GCC_SDCC1_APPS_CBCR) | 1u;
	spin_us(100);
	return (gcc_read(GCC_SDCC1_APPS_CMD) & 1u) == 0 ? 0 : -1;
}

/* 25 MHz for the card clock, taken from the same table U-Boot uses for this SoC
 * (drivers/clk/qcom/clock-sdm670.c: F(25000000, CFG_CLK_SRC_GPLL0_EVEN, 6, 1, 2)). The RCG divider
 * field holds (div * 2 - 1) and DUAL_EDGE is set when n != m, so the 400 kHz entry the probe
 * already writes (CXO, div 12, m 1, n 4) is CFG 0x2017 and this one is CFG 0x260B. 25 MHz is still
 * inside the default-speed range, so it needs no CMD6 switch; 50 MHz would.
 *
 * Returns 0 when a sector read still returns the GPT signature afterwards. */
/* Reprogram the SDCC1 apps RCG and restart the controller clock on the new base. CFG carries the
 * source, the divider (div * 2 - 1) and, in bits 13:12, the MND mode: with mode 0 the counters are
 * bypassed, so M/N/D are only written when the entry uses them (clk-rcg2 does the same). */
static void
emmc_program_rate(uint32_t cfg, uint32_t m, uint32_t n, uint32_t d)
{
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_CMD) = 0;      /* root disabled while editing */
	spin_us(10);
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_CFG) = cfg;
	if ((cfg & 0x3000u) != 0) {
		*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_M) = m;
		*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_N) = n;
		*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_D) = d;
	}
	spin_us(10);
	*(volatile uint32_t *)(gcc_base + GCC_SDCC1_APPS_CMD) = 1u;     /* update */
	for (unsigned waited = 0; waited < 100000; waited += 100) {
		if ((gcc_read(GCC_SDCC1_APPS_CMD) & 1u) == 0) {
			break;
		}
		spin_us(100);
	}
	/* Restart the controller clock on the new base: internal clock, wait for stable, card clock. */
	sdhci_write16(SDHCI_CLOCK_CONTROL, 0);
	spin_us(100);
	sdhci_write16(SDHCI_CLOCK_CONTROL, SDHCI_CLK_INTERNAL_EN);
	for (unsigned waited = 0; waited < 100000; waited += 100) {
		if ((sdhci_read16(SDHCI_CLOCK_CONTROL) & SDHCI_CLK_INTERNAL_STABLE) != 0) {
			break;
		}
		spin_us(100);
	}
	sdhci_write16(SDHCI_CLOCK_CONTROL, SDHCI_CLK_INTERNAL_EN | SDHCI_CLK_SD_EN);
	spin_us(1000);
}

static int
emmc_set_card_clock_25m(void)
{
	if (gcc_base == 0) {
		return -1;
	}
	uint16_t before = sdhci_read16(SDHCI_CLOCK_CONTROL);
	emmc_program_rate(0x260bu, 1u, 0xfeu, 0xfeu);   /* src GPLL0_EVEN, div 6, DUAL_EDGE, m/n 1/2 */

	/* Prove it with the GPT signature before anybody depends on it. */
	uint8_t probe[512];
	if (sdhci_read_block_adma2(1, probe) != 0 || probe[0] != 'E' || probe[1] != 'F' ||
	    probe[2] != 'I' || probe[3] != ' ' || probe[4] != 'P' || probe[5] != 'A' ||
	    probe[6] != 'R' || probe[7] != 'T') {
		sdhci_write16(SDHCI_CLOCK_CONTROL, (uint16_t)(before & 0xff00u));
		return -1;
	}
	return 0;
}

/* The boot chain leaves SDCC1 clocked, but the branches get gated again once it goes idle: the
 * probe read clock=0x0407 and the block read found clock=0x0000, so every command was dropped for
 * want of a card clock. Re-enable the GCC branches and program the card clock before a transfer. */
static int
emmc_clock_on(uint16_t previous)
{
	static const uint32_t branches[] = {
		GCC_SDCC1_APPS_CBCR, GCC_SDCC1_AHB_CBCR, GCC_SDCC1_ICE_CBCR, GCC_AGGRE_UFS_PHY_AXI_CBCR
	};
	if (gcc_base == 0) {
		return -1;
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		if ((gcc_read(branches[i]) & (1u << 31)) != 0) {
			*(volatile uint32_t *)(gcc_base + branches[i]) = gcc_read(branches[i]) | 1u;
		}
	}
	for (unsigned waited = 0; waited < 100000; waited += 100) {
		if ((sdhci_read16(SDHCI_CLOCK_CONTROL) & (SDHCI_CLK_INTERNAL_EN | SDHCI_CLK_SD_EN)) ==
		    (SDHCI_CLK_INTERNAL_EN | SDHCI_CLK_SD_EN)) {
			return 0;
		}
		spin_us(100);
	}
	/* Program it again: enable the internal clock, wait for it to stabilise, then the card clock.
	 * The frequency selector keeps the value the boot chain used. */
	sdhci_write16(SDHCI_CLOCK_CONTROL, (uint16_t)((previous & 0xff00u) | SDHCI_CLK_INTERNAL_EN));
	for (unsigned waited = 0; waited < 100000; waited += 100) {
		if ((sdhci_read16(SDHCI_CLOCK_CONTROL) & SDHCI_CLK_INTERNAL_STABLE) != 0) {
			break;
		}
		spin_us(100);
	}
	uint16_t value = sdhci_read16(SDHCI_CLOCK_CONTROL);
	sdhci_write16(SDHCI_CLOCK_CONTROL, (uint16_t)(value | SDHCI_CLK_SD_EN));
	spin_us(1000);
	return (sdhci_read16(SDHCI_CLOCK_CONTROL) & SDHCI_CLK_SD_EN) != 0 ? 0 : -1;
}

static void
ring_fail(const char *stage)
{
	ring_puts("EMMC ");
	ring_puts(stage);
	ring_puts(" FAIL int=");
	ring_hex(sdhci_read16(SDHCI_INT_STATUS), 4);
	ring_puts(" eis=");
	ring_hex(sdhci_read16(SDHCI_ERR_INT_STATUS), 4);
	ring_puts(" st=");
	ring_hex(sdhci_read32(SDHCI_PRESENT_STATE), 8);
	ring_puts("\r\n");
}

extern vm_offset_t kvtophys(vm_offset_t va);
extern void flush_dcache64(addr64_t pa, unsigned long len, boolean_t phys);

#define SDHCI_TM_DMA (1u << 0)

/* SDHCI ignores writes to the transfer mode register while the command line is inhibited, so a
 * single wedged command silently poisons every transfer that follows. Clear the line first. */
static int
emmc_clear_command_line(void)
{
	if ((sdhci_read32(SDHCI_PRESENT_STATE) & (SDHCI_PS_CMD_INHIBIT | SDHCI_PS_DAT_INHIBIT)) == 0) {
		return 0;
	}
	sdhci_write8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
	for (unsigned waited = 0; waited < 20000; waited += 10) {
		if ((sdhci_read8(SDHCI_SOFTWARE_RESET) & (SDHCI_RESET_CMD | SDHCI_RESET_DATA)) == 0) {
			return 0;
		}
		spin_us(10);
	}
	return -1;
}

/* A page-aligned static buffer is physically contiguous, which is all SDMA needs. */
static uint8_t sdma_buffer[512] __attribute__((aligned(4096)));

/* The eMMC controller's DMA goes through the apps SMMU with stream id 0x140; without a matching
 * stream mapping the data engine faults and the transfer never completes (DAT line stays active
 * forever, no transfer-complete event). Install an exact-match bypass entry the way the boot chain
 * does for the storage stream. */
#define SMMU_PA 0x15000000u
#define SMMU_SIZE 0x000d0000u
#define EMMC_STREAM_ID 0x0140u

/* Read-only: which stream mapping does the boot chain leave for the storage stream id? Writing
 * SMR/S2CR while the SMMU is live is not safe from here, and a wrong write hangs the SoC. */
static uint32_t emmc_smmu_report;
static uint32_t emmc_smmu_fixed;
static vm_offset_t emmc_smmu_base;
volatile uint32_t sirius_emmc_gfsr;
volatile uint32_t sirius_emmc_pio_distinct;
static uint32_t emmc_smmu_scr0;

static int
emmc_smmu_probe(void)
{
	vm_offset_t smmu = ml_io_map(SMMU_PA, SMMU_SIZE);
	if (smmu == 0) {
		return -1;
	}
	emmc_smmu_base = smmu;
	/* SCR0 bit0 tells whether the SMMU is translating at all. If it is off, the ADMA descriptor
	 * fetch failed for a different reason and the IOMMU theory is wrong. */
	emmc_smmu_scr0 = *(volatile uint32_t *)(smmu + 0x000u);
	/* GFSR records global faults (0x48); clear it so anything found later belongs to our transfer. */
	*(volatile uint32_t *)(smmu + 0x048u) = *(volatile uint32_t *)(smmu + 0x048u);
	uint32_t idr0 = *(volatile uint32_t *)(smmu + 0x020u);
	unsigned count = idr0 & 0xffu;
	if (count == 0 || count > 128) {
		return -1;
	}
	for (unsigned i = 0; i < count; i++) {
		uint32_t smr = *(volatile uint32_t *)(smmu + 0x800u + 4u * i);
		if ((smr & (1u << 31)) == 0) {
			continue;
		}
		uint32_t id = smr & 0xffffu;
		if (id != EMMC_STREAM_ID) {
			continue;
		}
		uint32_t s2cr = *(volatile uint32_t *)(smmu + 0xc00u + 4u * i);
		emmc_smmu_report = ((uint32_t)(i & 0xffu) << 16) | ((s2cr & 0xffu) << 8) |
		    ((s2cr >> 16) & 3u);
		/* The stream is already matched and valid, only its action is wrong (abort). Flip that
		 * one field to bypass: no SMR write and no new slot, which is the smallest change that
		 * makes the storage DMA legal. Nothing of this stream is in flight - the command line is
		 * idle and no transfer is armed at this point. */
		/* Read-only. Writing this register twice has hung the SoC: the storage stream belongs to
		 * the boot chain's translation domain and changing it under a live DMA engine is not
		 * safe from here. Mapping our own buffer into that domain is the correct fix and needs
		 * a page table, which is a separate piece of work. */
		return 0;
	}
	emmc_smmu_report = 0xffffffffu;   /* no mapping for 0x140 at all */
	return -1;
}

/* ADMA2 with 64-bit addressing is the mode the boot chain uses, and the core may not implement
 * plain SDMA at all. One descriptor covers the whole 512-byte block: TRAN|VALID|END, length, and
 * the 64-bit address of the buffer. The table has to be 64-byte aligned. */
#define ADMA2_TRAN_VALID 0x21u
#define ADMA2_END 0x02u

/* 256 bytes so a 128-byte aligned slice always fits inside, whatever the physical layout. The
 * virtual alignment does not survive translation: the observed physical address of a 128-byte
 * aligned array was only 4-byte aligned, and the ADMA engine rejects a misaligned table with a
 * descriptor-fetch error. */
static uint8_t adma_desc[256] __attribute__((aligned(256)));

/* Everything a data command needs before the transfer is armed. U-Boot does the same two things
 * for every data command: force the data timeout control to 0x0e and set the block length. */
static void
emmc_prepare_transfer(void)
{
	if (sdhci_read8(SDHCI_TIMEOUT_CONTROL) != 0x0eu) {
		sdhci_write8(SDHCI_TIMEOUT_CONTROL, 0x0eu);
	}
	/* No SET_BLOCKLEN: an eMMC is block addressed and both U-Boot and Linux skip CMD16 here. */
}

/* One 512-byte data block for a single-block command: CMD17 (READ_SINGLE_BLOCK, argument = LBA)
 * or CMD8 (SEND_EXT_CSD, argument 0). */
static int
sdhci_read_adma2(uint32_t command_index, uint32_t argument, uint8_t *out)
{
	vm_offset_t buf_pa = kvtophys((vm_offset_t)sdma_buffer);
	vm_offset_t raw_pa = kvtophys((vm_offset_t)adma_desc);
	/* 32-bit ADMA2 carries 32-bit addresses: a kernel window above 4 GiB must fail here, not read
	 * into a truncated address. */
	if (buf_pa == 0 || raw_pa == 0 || (buf_pa >> 32) != 0 || ((raw_pa + 127u) >> 32) != 0) {
		return -1;
	}
	/* Align the descriptor table physically, not just virtually. */
	vm_offset_t desc_pa = (raw_pa + 127u) & ~(vm_offset_t)127u;
	uint8_t *desc = adma_desc + (desc_pa - raw_pa);
	if (emmc_clear_command_line() != 0) {
		return -1;
	}
	if (emmc_trace) {
		ring_puts("EMMC timeout-in=");
		ring_hex(sdhci_read8(SDHCI_TIMEOUT_CONTROL), 2);
		ring_puts("\r\n");
	}
	emmc_prepare_transfer();
	if (emmc_trace) {
		ring_puts("EMMC timeout-set=");
		ring_hex(sdhci_read8(SDHCI_TIMEOUT_CONTROL), 2);
		ring_puts("\r\n");
	}
	for (unsigned i = 0; i < 512; i++) {
		sdma_buffer[i] = 0;
	}
	for (unsigned i = 0; i < sizeof(adma_desc); i++) {
		adma_desc[i] = 0;
	}
	uint16_t cmd = (uint16_t)(ADMA2_TRAN_VALID | ADMA2_END);
	uint16_t len = 512;
	*(volatile uint16_t *)(desc + 0) = cmd;
	*(volatile uint16_t *)(desc + 2) = len;
	*(volatile uint32_t *)(desc + 4) = (uint32_t)buf_pa;
	*(volatile uint32_t *)(desc + 8) = 0;
	__asm__ volatile ("dsb sy" ::: "memory");

	/* 32-bit ADMA2: 8-byte descriptors and DMA select bits[4:3] = 0b10. The 64-bit variant made
	 * the engine walk past our END bit (the address register advanced by 0x24), so try the
	 * simpler encoding first - every address here fits in 32 bits. */
	sdhci_write8(SDHCI_HOST_CONTROL, (uint8_t)((sdhci_read8(SDHCI_HOST_CONTROL) & 0x27u) | 0x10u));
	/* 0x00 is the SDMA system address; the ADMA2 engine fetches its descriptor table from
	 * 0x58/0x5c. Writing the pointer to 0x00 meant this transfer never had a valid table. */
	/* The descriptor and the destination buffer were written through cached mappings; the data
	 * engine reads physical memory, so without this it sees the zeros that were there before and
	 * walks off the end of the table. */
	flush_dcache64((addr64_t)desc_pa, 128, TRUE);
	flush_dcache64((addr64_t)buf_pa, 512, TRUE);
	sdhci_write32(SDHCI_ADMA_ADDRESS, (uint32_t)desc_pa);
	sdhci_write32(SDHCI_ADMA_ADDRESS_HI, 0u);
	sdhci_write16(SDHCI_BLOCK_SIZE, 512u);
	sdhci_write16(SDHCI_BLOCK_COUNT, 1u);
	sdhci_write16(SDHCI_TRANSFER_MODE, SDHCI_TM_READ | SDHCI_TM_BLOCK_COUNT_ENABLE | SDHCI_TM_DMA);
	if ((sdhci_read16(SDHCI_TRANSFER_MODE) & (SDHCI_TM_READ | SDHCI_TM_DMA)) !=
	    (SDHCI_TM_READ | SDHCI_TM_DMA)) {
		return -1;
	}
	if (sdhci_command((uint16_t)((command_index << 8) | SDHCI_CMD_DATA_PRESENT | SDHCI_CMD_RESP_48 |
	    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), argument) != 0) {
		return -1;
	}
	if (emmc_trace) {
		/* Sample the four DAT line levels while the clock runs: if they never change the card
		 * is not driving data at all, which is a pad/hardware problem rather than a sampling
		 * one. */
		uint32_t seen = 0;
		unsigned transitions = 0;
		uint32_t previous = (sdhci_read32(SDHCI_PRESENT_STATE) >> 20) & 0xfu;
		for (unsigned i = 0; i < 2000; i++) {
			uint32_t now = (sdhci_read32(SDHCI_PRESENT_STATE) >> 20) & 0xfu;
			seen |= 1u << now;
			if (now != previous) {
				transitions++;
				previous = now;
			}
			if ((sdhci_read16(SDHCI_INT_STATUS) & SDHCI_INT_TRANSFER_COMPLETE) != 0) {
				break;
			}
			spin_us(50);
		}
		/* The card cannot send anything without the card clock, so record whether the
		 * controller keeps it running while the data phase is pending. */
		uint16_t clock_during = sdhci_read16(SDHCI_CLOCK_CONTROL);
		uint32_t gcc_during = gcc_read(GCC_SDCC1_APPS_CBCR);
		ring_puts("EMMC clock=");
		ring_hex(sdhci_read16(SDHCI_CLOCK_CONTROL), 4);
		ring_puts("\r\n");
		ring_puts("EMMC clk-during=");
		ring_hex(clock_during, 4);
		ring_puts(" gcc=");
		ring_hex(gcc_during, 8);
		ring_puts("\r\n");
		ring_puts("EMMC dat seen=");
		ring_hex(seen, 4);
		ring_puts(" trans=");
		ring_dec(transitions);
		ring_puts("\r\n");
	}
	if (sdhci_wait(SDHCI_INT_TRANSFER_COMPLETE, 2000000) <= 0) {
		/* 0x54: bits[1:0] say whether the descriptor was rejected (01), the transfer failed on
		 * the bus (10) or both (11). That separates a malformed table from an IOMMU/bus problem. */
			ring_puts("EMMC adma2 hc=");
		ring_hex(sdhci_read8(SDHCI_HOST_CONTROL), 2);
		ring_puts(" xf=");
		ring_hex(sdhci_read16(SDHCI_TRANSFER_MODE), 4);
		ring_puts(" bs=");
		ring_hex(sdhci_read16(SDHCI_BLOCK_SIZE), 4);
		ring_puts(" adma=");
		ring_hex(sdhci_read32(SDHCI_ADMA_ADDRESS), 8);
		ring_puts(" raw=");
		ring_hex((uint32_t)kvtophys((vm_offset_t)adma_desc), 8);
		ring_puts(" desc=");
		ring_hex((uint32_t)((kvtophys((vm_offset_t)adma_desc) + 127u) & ~(vm_offset_t)127u), 8);
		ring_puts(" d0=");
		ring_hex(*(volatile uint32_t *)(desc + 0), 8);
		ring_puts(" d1=");
		ring_hex(*(volatile uint32_t *)(desc + 4), 8);
		ring_puts("\r\n");
		/* Clear the latched status so one failed transfer cannot poison the next one. */
		sdhci_write8(SDHCI_ADMA_ERROR_STATUS, 0u);
		sdhci_write32(SDHCI_INT_STATUS, 0xffffffffu);
		ring_puts("EMMC adma2 adst=");
		ring_hex(sdhci_read8(SDHCI_ADMA_ERROR_STATUS), 2);
		ring_puts(" no TC int=");
		ring_hex(sdhci_read16(SDHCI_INT_STATUS), 4);
		ring_puts(" err=");
		ring_hex(sdhci_read16(SDHCI_ERR_INT_STATUS), 4);
		ring_puts("\r\n");
		return -1;
	}
	sdhci_write32(SDHCI_INT_STATUS, 0xffffffffu);
	flush_dcache64((addr64_t)buf_pa, 512, TRUE);
	for (unsigned i = 0; i < 512; i++) {
		out[i] = sdma_buffer[i];
	}
	return 0;
}

static int
sdhci_read_block_adma2(uint32_t lba, uint8_t *out)
{
	return sdhci_read_adma2(17u, lba, out);
}

/* Single-block read over SDMA: the boot chain left the controller in a DMA mode where the buffer
 * port only returns one stale word, so the block is fetched by the data engine into memory
 * instead of through the FIFO. */
static int
sdhci_read_block_sdma(uint32_t lba, uint8_t *out)
{
	vm_offset_t pa = kvtophys((vm_offset_t)sdma_buffer);
	if (pa == 0) {
		return -1;
	}
	for (unsigned i = 0; i < 512; i++) {
		sdma_buffer[i] = 0;
	}
	if (emmc_clear_command_line() != 0) {
		return -1;
	}
	emmc_smmu_probe();
	sdhci_write32(SDHCI_DMA_ADDRESS, (uint32_t)pa);
	sdhci_write16(SDHCI_BLOCK_SIZE, 512u);
	sdhci_write16(SDHCI_BLOCK_COUNT, 1u);
	sdhci_write16(SDHCI_TRANSFER_MODE, SDHCI_TM_READ | SDHCI_TM_BLOCK_COUNT_ENABLE | SDHCI_TM_DMA);
	/* If the mode write did not land the transfer would run with the boot chain's settings, so
	 * treat that as a failure instead of misreading whatever comes back. */
	uint16_t mode = sdhci_read16(SDHCI_TRANSFER_MODE);
	if ((mode & (SDHCI_TM_READ | SDHCI_TM_DMA)) != (SDHCI_TM_READ | SDHCI_TM_DMA)) {
		return -1;
	}
	uint32_t r1[4] = { 0, 0, 0, 0 };
	int cmd_ok = sdhci_command_capture((uint16_t)((17u << 8) | SDHCI_CMD_DATA_PRESENT |
	    SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), lba, r1);
	if (cmd_ok != 0) {
		ring_puts("EMMC cmd17 fail int=");
		ring_hex(sdhci_read16(SDHCI_INT_STATUS), 4);
		ring_puts(" err=");
		ring_hex(sdhci_read16(SDHCI_ERR_INT_STATUS), 4);
		ring_puts(" st=");
		ring_hex(sdhci_read32(SDHCI_PRESENT_STATE), 8);
		ring_puts("\r\n");
		return -1;
	}
	int tc = sdhci_wait(SDHCI_INT_TRANSFER_COMPLETE, 2000000);
	if (tc <= 0) {
		ring_puts("EMMC smmu140=");
		ring_hex(emmc_smmu_report, 8);
		if (emmc_smmu_base != 0) {
			sirius_emmc_gfsr = *(volatile uint32_t *)(emmc_smmu_base + 0x048u);
			ring_puts(" gfsr=");
			ring_hex(sirius_emmc_gfsr, 8);
		}
		ring_puts(" scr0=");
		ring_hex(emmc_smmu_scr0, 8);
		ring_puts(" fixed=");
		ring_dec(emmc_smmu_fixed);
		ring_puts(" r1=");
		ring_hex(r1[0], 8);
		ring_puts(" cmd17 ok, no TC int=");
		ring_hex(sdhci_read16(SDHCI_INT_STATUS), 4);
		ring_puts(" err=");
		ring_hex(sdhci_read16(SDHCI_ERR_INT_STATUS), 4);
		ring_puts(" st=");
		ring_hex(sdhci_read32(SDHCI_PRESENT_STATE), 8);
		ring_puts("\r\n");
		return -1;
	}
	sdhci_write32(SDHCI_INT_STATUS, 0xffffffffu);
	for (unsigned i = 0; i < 512; i++) {
		out[i] = sdma_buffer[i];
	}
	return 0;
}

/* Single-block read in PIO mode. Returns 0 when the whole 512-byte block was collected. */
static int
sdhci_read_block(uint32_t lba, uint8_t *out)
{
	/* Drop the SDHCI divider to one. The boot chain left 4 in bits[15:8], which halves the clock
	 * twice more on top of the 400 kHz programmed at the root, and a card that slow accepts
	 * commands but never streams data. The spec requires the SD clock to be off while the divider
	 * is written, otherwise the write is simply ignored. */
	{
		uint16_t ctl = sdhci_read16(SDHCI_CLOCK_CONTROL);
		sdhci_write16(SDHCI_CLOCK_CONTROL, (uint16_t)(ctl & ~SDHCI_CLK_SD_EN));
		spin_us(1000);
		sdhci_write16(SDHCI_CLOCK_CONTROL, (uint16_t)((ctl & 0x00c0u) | SDHCI_CLK_INTERNAL_EN));
		for (unsigned waited = 0; waited < 20000; waited += 100) {
			if ((sdhci_read16(SDHCI_CLOCK_CONTROL) & SDHCI_CLK_INTERNAL_STABLE) != 0) {
				break;
			}
			spin_us(100);
		}
		sdhci_write16(SDHCI_CLOCK_CONTROL, (uint16_t)((sdhci_read16(SDHCI_CLOCK_CONTROL) & 0x00c0u) |
		    SDHCI_CLK_INTERNAL_EN | SDHCI_CLK_SD_EN));
		spin_us(1000);
	}

	/* The board wires vqmmc to a 1.8 V rail (sdm710-xiaomi-sirius.dts), so the card's I/O runs at
	 * 1.8 V while the controller was left at 3.0 V. Commands survive that mismatch, multi-line
	 * data does not, which is exactly what the empty DAT lines look like. */
	{
		uint8_t pc = sdhci_read8(SDHCI_POWER_CONTROL);
		ring_puts("EMMC power-in=");
		ring_hex(pc, 2);
		ring_puts(" hc2=");
		ring_hex(sdhci_read8(SDHCI_HOST_CONTROL2), 2);
		ring_puts("\r\n");
		sdhci_write8(SDHCI_POWER_CONTROL, 0x0bu);            /* 1.8 V */
		uint8_t hc2 = sdhci_read8(SDHCI_HOST_CONTROL2);
		sdhci_write8(SDHCI_HOST_CONTROL2, (uint8_t)(hc2 | 0x08u));   /* 1.8 V signalling */
		spin_us(1000);
		ring_puts("EMMC power-out=");
		ring_hex(sdhci_read8(SDHCI_POWER_CONTROL), 2);
		ring_puts(" hc2=");
		ring_hex(sdhci_read8(SDHCI_HOST_CONTROL2), 2);
		ring_puts("\r\n");
	}

	/* Keep the bus width and high-speed bit the boot chain set, and only clear the DMA select.
	 * Switching to 1-bit here hung the boot: the probe runs before the USB console starts, so a
	 * stall in it costs the only log channel. */
	uint8_t hostctl = sdhci_read8(SDHCI_HOST_CONTROL);
	/* Bus width and high-speed stay; only the DMA select goes to SDMA for the buffer-port path. */
	sdhci_write8(SDHCI_HOST_CONTROL, (uint8_t)(hostctl & ~0x18u));
	if (emmc_clear_command_line() != 0) {
		return -1;
	}
	sdhci_write16(SDHCI_BLOCK_SIZE, 512u);
	sdhci_write16(SDHCI_BLOCK_COUNT, 1u);
	sdhci_write16(SDHCI_TRANSFER_MODE, SDHCI_TM_READ | SDHCI_TM_BLOCK_COUNT_ENABLE);
	/* PIO needs no DMA, so the SMMU - whose storage stream is configured to abort - stays out of
	 * the picture. Verify the mode write landed; SDHCI ignores it while the line is inhibited. */
	if ((sdhci_read16(SDHCI_TRANSFER_MODE) & SDHCI_TM_READ) == 0) {
		ring_puts("EMMC pio mode write ignored\r\n");
		return -1;
	}
	/* The command index lives in bits 13:8 (SDHCI_MAKE_CMD in Linux); writing it into the flag
	 * bits sends command 0 and the card never answers. */
	if (sdhci_command((uint16_t)((17u << 8) | SDHCI_CMD_DATA_PRESENT | SDHCI_CMD_RESP_48 |
	    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), lba) != 0) {
		return -1;      /* CMD17, READ_SINGLE_BLOCK */
	}
	/* PIO drain, the way the SDHCI spec describes it: the Buffer Read Enable bit in the present
	 * state says a word is available, and the transfer is done when all 512 bytes are out. Reading
	 * one word per interrupt-status poll lost data and reported failure even though the sector had
	 * arrived. */
	unsigned received = 0;
	unsigned idle = 0;
	uint32_t first_word = 0;
	unsigned distinct = 0;
	uint32_t seen_words[8] = { 0 };
	unsigned seen_count = 0;
	while (received < 512) {
		uint32_t state = sdhci_read32(SDHCI_PRESENT_STATE);
		if ((state & SDHCI_PS_BUFFER_READ_ENABLE) != 0) {
			uint32_t value = sdhci_read32(SDHCI_BUFFER);
			if (received == 0) {
				first_word = value;
				seen_words[seen_count < 8 ? seen_count : 7] = value;
				if (seen_count < 8) {
					seen_count++;
				}
				distinct = 1;
			} else {
				bool known = false;
				for (unsigned k = 0; k < seen_count && k < 8; k++) {
					if (seen_words[k] == value) {
						known = true;
					}
				}
				if (!known) {
					if (seen_count < 8) {
						seen_words[seen_count++] = value;
					}
					distinct++;
				}
			}
			out[received + 0] = (uint8_t)(value & 0xffu);
			out[received + 1] = (uint8_t)((value >> 8) & 0xffu);
			out[received + 2] = (uint8_t)((value >> 16) & 0xffu);
			out[received + 3] = (uint8_t)((value >> 24) & 0xffu);
			received += 4;
			idle = 0;
			continue;
		}
		if ((sdhci_read16(SDHCI_INT_STATUS) & SDHCI_INT_TRANSFER_COMPLETE) != 0) {
			sdhci_write16(SDHCI_INT_STATUS, SDHCI_INT_TRANSFER_COMPLETE);
			break;
		}
		if (++idle > 200000) {
			break;
		}
		spin_us(10);
	}
	sdhci_write32(SDHCI_INT_STATUS, 0xffffffffu);
	sirius_emmc_pio_distinct = distinct;
	if (received == 512 && distinct < 4) {
		/* The buffer port kept handing back the same word: this is a stale FIFO, not a block. */
		return -1;
	}
	return received == 512 ? 0 : -1;
}

/* Published to the boot-progress panel: apart from the block read the probe is read-only, and its
 * result is the only evidence that the controller is reachable from XNU at all.
 * Steps: 1 mapped, 2 registers read, 3 card present, 4 block read failed, 5 read but no GPT,
 * 6 GPT header found. */
volatile uint32_t sirius_emmc_step;
volatile uint32_t sirius_emmc_version;
volatile uint32_t sirius_emmc_caps;
volatile uint32_t sirius_emmc_state;
volatile uint32_t sirius_emmc_clock;
volatile uint32_t sirius_emmc_power;
volatile uint32_t sirius_emmc_halts;
volatile uint32_t sirius_emmc_signature;

/* Forward declaration: the GPT walk below reports the first sector of the userdata window. */
int sirius_emmc_read_block(unsigned long long lba, void *buffer);

void
sirius_emmc_probe(void)
{
	sirius_emmc_step = 0;
	if (emmc_base == 0) {
		sirius_emmc_step = 1;
		emmc_base = ml_io_map(SDCC1_PA, SDCC1_SIZE);
		gcc_base = ml_io_map(GCC_PA, GCC_SIZE);
	}
	if (emmc_base == 0 || gcc_base == 0) {
		return;
	}

	/* The AHB and card clocks must be running before the register window is trustworthy. */
	uint32_t apps = gcc_read(GCC_SDCC1_APPS_CBCR);
	uint32_t ahb = gcc_read(GCC_SDCC1_AHB_CBCR);
	uint32_t ice = gcc_read(GCC_SDCC1_ICE_CBCR);
	uint32_t bus = gcc_read(GCC_AGGRE_UFS_PHY_AXI_CBCR);
	sirius_emmc_halts = ((apps >> 31) & 1u) | (((ahb >> 31) & 1u) << 1) |
	    (((ice >> 31) & 1u) << 2) | (((bus >> 31) & 1u) << 3);

	sirius_emmc_version = sdhci_read16(SDHCI_VERSION);
	sirius_emmc_caps = sdhci_read32(SDHCI_CAPABILITIES);
	sirius_emmc_state = sdhci_read32(SDHCI_PRESENT_STATE);
	sirius_emmc_clock = sdhci_read16(SDHCI_CLOCK_CONTROL);
	sirius_emmc_power = sdhci_read8(SDHCI_POWER_CONTROL);
	sirius_emmc_step = 2;

	ring_puts("EMMC rcg cfg=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_CFG), 8);
	ring_puts(" m=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_M), 8);
	ring_puts(" n=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_N), 8);
	ring_puts(" d=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_D), 8);
	ring_puts(" vspec=");
	ring_hex(sdhci_read32(MSM_CORE_VENDOR_SPEC), 8);
	ring_puts(" corever=");
	ring_hex(sdhci_read32(MSM_CORE_MCI_VERSION), 8);
	ring_puts(" dll=");
	ring_hex(sdhci_read32(MSM_CORE_DLL_CONFIG), 8);
	ring_puts(" dllst=");
	ring_hex(sdhci_read32(MSM_CORE_DLL_STATUS), 8);
	ring_puts("\r\n");

	ring_puts("EMMC probe step=");
	ring_dec(sirius_emmc_step);
	ring_puts(" ver=");
	ring_hex(sirius_emmc_version, 4);
	ring_puts(" caps=");
	ring_hex(sirius_emmc_caps, 8);
	ring_puts(" state=");
	ring_hex(sirius_emmc_state, 8);
	ring_puts(" clock=");
	ring_hex(sirius_emmc_clock, 4);
	ring_puts(" power=");
	ring_hex(sirius_emmc_power, 2);
	ring_puts(" halts=");
	ring_hex(sirius_emmc_halts, 1);
	ring_puts("\r\n");

	emmc_smmu_probe();
	ring_puts("EMMC smmu140=");
	ring_hex(emmc_smmu_report, 8);
	ring_puts(" scr0=");
	ring_hex(emmc_smmu_scr0, 8);
	ring_puts(" gfsr=");
	ring_hex(emmc_smmu_base != 0 ? *(volatile uint32_t *)(emmc_smmu_base + 0x048u) : 0u, 8);
	ring_puts("\r\n");

	/* The board wires vqmmc to a 1.8 V rail, so the card I/O is 1.8 V even though the controller
	 * was left at 3.0 V. Commands survive that mismatch, multi-line data does not. */
	{
		uint8_t power = sdhci_read8(SDHCI_POWER_CONTROL);
		uint8_t hc2 = sdhci_read8(SDHCI_HOST_CONTROL2);
		sdhci_write8(SDHCI_POWER_CONTROL, 0x0bu);
		sdhci_write8(SDHCI_HOST_CONTROL2, (uint8_t)(hc2 | 0x08u));
		spin_us(1000);
		ring_puts("EMMC power ");
		ring_hex(power, 2);
		ring_puts("->");
		ring_hex(sdhci_read8(SDHCI_POWER_CONTROL), 2);
		ring_puts(" hc2 ");
		ring_hex(hc2, 2);
		ring_puts("->");
		ring_hex(sdhci_read8(SDHCI_HOST_CONTROL2), 2);
		ring_puts("\r\n");
	}

	sdhci_write32(SDHCI_INT_ENABLE, 0xffffffffu);
	sdhci_write16(SDHCI_SIGNAL_ENABLE, 0x0000u);
	sdhci_write8(SDHCI_TIMEOUT_CONTROL, 0x0eu);

	ring_puts("EMMC set400k=");
	ring_puts(emmc_set_card_clock_400k() == 0 ? "ok" : "fail");
	ring_puts(" cfg=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_CFG), 8);
	ring_puts(" m=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_M), 8);
	ring_puts(" n=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_N), 8);
	ring_puts(" d=");
	ring_hex(gcc_read(GCC_SDCC1_APPS_D), 8);
	ring_puts("\r\n");

	/* The boot chain left the card selected with RCA 1 in the transfer state; CMD13 answers right
	 * away, which is the cheapest proof that the command channel works. */
	uint32_t status[4] = { 0, 0, 0, 0 };
	int rca_live = -1;
	for (uint32_t rca = 1; rca <= 8; rca++) {
		if (sdhci_command_capture((uint16_t)((13u << 8) | SDHCI_CMD_RESP_48 |
		    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), rca << 16, status) == 0) {
			rca_live = (int)rca;
			break;
		}
	}
	ring_puts("EMMC rca=");
	ring_dec(rca_live < 0 ? 0u : (uint32_t)rca_live);
	ring_puts(" st=");
	ring_hex(status[0], 8);
	ring_puts(" cur=");
	ring_dec((status[0] >> 9) & 0xfu);
	ring_puts("\r\n");
	if (rca_live < 0) {
		sirius_emmc_step = 3;
		return;
	}

	/* CMD17 is only *executed* by a selected card; an addressed but deselected card still answers
	 * CMD13, which is how a card can look alive while every read returns nothing. */
	{
		uint32_t sel[4] = { 0, 0, 0, 0 };
		int sel_ok = sdhci_command_capture((uint16_t)((7u << 8) | SDHCI_CMD_RESP_48_BUSY |
		    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), rca_live << 16, sel);
		ring_puts("EMMC cmd7=");
		ring_puts(sel_ok == 0 ? "ok" : "fail");
		ring_puts(" r1=");
		ring_hex(sel[0], 8);
		ring_puts("\r\n");
	}

	uint8_t sector[512];
	/* The card drives the data lines now that the I/O voltage matches, so read it through the
	 * buffer port: PIO needs no descriptors and no DMA engine at all. */
	int read_ok = sdhci_read_block(1, sector);
	if (read_ok != 0) {
		read_ok = sdhci_read_block_adma2(1, sector);
	}
	ring_puts("EMMC read=");
	ring_puts(read_ok == 0 ? "ok" : "fail");
	/* Optional: leave the identification clock for 25 MHz (the U-Boot table's entry for this SoC),
	 * so the root mount and the launchd exec do not crawl at 400 kHz. Off unless asked, and it
	 * keeps 400 kHz if the GPT signature does not read back afterwards. */
	if (read_ok == 0) {
		unsigned hs = 0;
		if (PE_parse_boot_argn("sirius_emmc_hs", &hs, sizeof(hs)) && hs != 0) {
			int fast = emmc_set_card_clock_25m();
			ring_puts("EMMC clock25=");
			ring_puts(fast == 0 ? "ok" : "fallback");
			ring_puts("\r\n");
		}
	}
	ring_puts(" sig=");
	for (unsigned i = 0; i < 8; i++) {
		sirius_usb_log_char((sector[i] >= 0x20 && sector[i] < 0x7f) ? (char)sector[i] : '.');
	}
	ring_puts("\r\n");

	/* State 5 means the card is waiting to stream a block the host never clocked out; state 4
	 * means the transfer never started at all. */
	(void)emmc_clear_command_line();
	uint32_t after[4] = { 0, 0, 0, 0 };
	int st_ok = sdhci_command_capture((uint16_t)((13u << 8) | SDHCI_CMD_RESP_48 |
	    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), 1u << 16, after);
	ring_puts("EMMC pio-distinct=");
	ring_dec(sirius_emmc_pio_distinct);
	ring_puts("\r\n");
	ring_puts("EMMC after=");
	ring_puts(st_ok == 0 ? "ok" : "fail");
	ring_puts(" st=");
	ring_hex(after[0], 8);
	ring_puts(" cur=");
	ring_dec((after[0] >> 9) & 0xfu);
	ring_puts("\r\n");

	if (read_ok == 0) {
		sirius_emmc_signature = ((uint32_t)sector[0] << 24) | ((uint32_t)sector[1] << 16) |
		    ((uint32_t)sector[2] << 8) | (uint32_t)sector[3];
		sirius_emmc_step = 6;

		/* GPT header: entry array LBA at 0x48, entry count at 0x50, entry size at 0x54. */
		uint32_t entries_lba = (uint32_t)sector[0x48] | ((uint32_t)sector[0x49] << 8) |
		    ((uint32_t)sector[0x4a] << 16) | ((uint32_t)sector[0x4b] << 24);
		uint32_t entries = (uint32_t)sector[0x50] | ((uint32_t)sector[0x51] << 8) |
		    ((uint32_t)sector[0x52] << 16) | ((uint32_t)sector[0x53] << 24);
		uint32_t entry_size = (uint32_t)sector[0x54] | ((uint32_t)sector[0x55] << 8) |
		    ((uint32_t)sector[0x56] << 16) | ((uint32_t)sector[0x57] << 24);
		ring_puts("EMMC gpt lba=");
		ring_dec(entries_lba);
		ring_puts(" count=");
		ring_dec(entries);
		ring_puts(" size=");
		ring_dec(entry_size);
		ring_puts("\r\n");

		uint8_t table[512];
		if (entry_size >= 128u && entry_size <= 512u && entries != 0) {
			unsigned per_sector = 512u / entry_size;
			unsigned shown = 0;
			/* The table spans several sectors (four 128-byte entries per sector), so walk
			 * them instead of reporting only the first four partitions. */
			for (unsigned sector_index = 0; sector_index * per_sector < entries &&
			    shown < 84; sector_index++) {
				if (sdhci_read_block_adma2(entries_lba + sector_index, table) != 0) {
					break;
				}
			for (unsigned e = 0; e < per_sector && shown < 84; e++) {
				const uint8_t *entry = table + e * entry_size;
				bool used = false;
				for (unsigned i = 0; i < 16; i++) {
					if (entry[i] != 0) {
						used = true;
					}
				}
				if (!used) {
					continue;
				}
				uint32_t first = (uint32_t)entry[0x20] | ((uint32_t)entry[0x21] << 8) |
				    ((uint32_t)entry[0x22] << 16) | ((uint32_t)entry[0x23] << 24);
				uint32_t last = (uint32_t)entry[0x28] | ((uint32_t)entry[0x29] << 8) |
				    ((uint32_t)entry[0x2a] << 16) | ((uint32_t)entry[0x2b] << 24);
				char name[37];
				unsigned name_len = 0;
				for (unsigned i = 0; i < 36; i++) {
					uint16_t ch = (uint16_t)entry[0x38u + 2u * i] |
					    ((uint16_t)entry[0x39u + 2u * i] << 8);
					if (ch == 0) {
						break;
					}
					name[name_len++] = ch < 0x80 ? (char)ch : '?';
				}
				name[name_len] = '\0';
				ring_puts("EMMC part first=");
				ring_dec(first);
				ring_puts(" last=");
				ring_dec(last);
				ring_puts(" name=");
				for (unsigned i = 0; i < name_len; i++) {
					sirius_usb_log_char(name[i]);
				}
				ring_puts("\r\n");
				/* The root device is matched on the volume UUID, so the unique GUID of the
				 * userdata entry is what /chosen/root-matching has to name. */
				if (name_len == 8 && name[0] == 'u' && name[1] == 's' && name[2] == 'e' &&
				    name[3] == 'r' && name[4] == 'd' && name[5] == 'a' &&
				    name[6] == 't' && name[7] == 'a') {
					ring_puts("EMMC userdata guid=");
					for (unsigned i = 0; i < 16; i++) {
						ring_hex(entry[0x10u + i], 2);
					}
					ring_puts("\r\n");
				}
				shown++;
			}
			}
		}
		/* What the root actually is: the first sector of the userdata window. APFS keeps its
		 * container magic at offset 32 of block zero. */
		{
			static uint8_t head[512];
			if (sirius_emmc_read_block(17301504ull, head) != 0) {
				ring_puts("EMMC window read=fail\r\n");
			} else {
				ring_puts("EMMC window head=");
				for (unsigned i = 0; i < 16; i++) {
					ring_hex(head[i], 2);
				}
				ring_puts(" magic=");
				for (unsigned i = 32; i < 36; i++) {
					char c = (char)head[i];
					sirius_usb_log_char(c >= 0x20 && c < 0x7f ? c : '.');
				}
				ring_puts("\r\n");
			}
		}
		return;
	}
	sirius_emmc_step = 4;
}


/* ------------------------------------------------------------------------- *
 * Public interface for the IOKit block driver.
 *
 * The probe above is a diagnostic path that reports into the USB console ring.
 * The driver needs the same hardware setup without the reporting, plus a plain
 * block read it can call from doAsyncReadWrite().
 * ------------------------------------------------------------------------- */

static bool sirius_emmc_ready;

/* Filled by sirius_emmc_prepare() from the GPT header: the drive the block driver publishes. */
unsigned long long sirius_emmc_total_blocks;

/* ------------------------------------------------------------------------- *
 * Speed. The driver used to read one block per CMD17 at the 400 kHz identification clock
 * (sirius_emmc_prepare() programmed it after the probe's 25 MHz switch): about 770 blocks/s on
 * the 8-bit bus, so verifying the 3 GB dyld shared cache took two hours. Two things fix that:
 *  - multi-block reads, one CMD18 with Auto-CMD12 for up to EMMC_RUN_BLOCKS blocks, the buffer
 *    described piece by piece in an ADMA2 table;
 *  - a faster card clock, climbed one verified step at a time (boot argument sirius_emmc_hs:
 *    1 = 25 MHz, 2 = up to 50 MHz, 3 = up to 100 MHz).
 * Each step is proven against blocks read at the identification clock before anything depends on
 * it, and a failure falls back to the last setting that read back correctly.
 * ------------------------------------------------------------------------- */

#define SDHCI_TM_AUTO_CMD12 (1u << 2)
#define SDHCI_TM_MULTI (1u << 5)
/* Data timeout, data CRC, data end bit, Auto CMD and ADMA errors (EIS bits 4, 5, 6, 8, 9). */
#define SDHCI_EIS_DATA_ERRORS 0x0370u

#define EMMC_RUN_BLOCKS 256u
#define EMMC_RUN_BYTES (EMMC_RUN_BLOCKS * 512u)
/* One descriptor per page, two where a virtual page straddles two physical ones. */
#define EMMC_RUN_DESCRIPTORS (2u * (EMMC_RUN_BYTES / 4096u) + 1u)
#define EMMC_CHECK_BLOCKS 64u

const unsigned sirius_emmc_run_blocks = EMMC_RUN_BLOCKS;
volatile uint32_t sirius_emmc_rate_khz = 400u;

static uint8_t emmc_run_buffer[EMMC_RUN_BYTES] __attribute__((aligned(4096)));
static uint8_t emmc_run_table[1024] __attribute__((aligned(1024)));
static uint8_t emmc_reference[EMMC_CHECK_BLOCKS * 512u];
static uint32_t emmc_rca;
static bool emmc_multi;
static bool emmc_cache_on;
static uint16_t emmc_last_eis;
/* EIS bits a known-good transfer already leaves set; they are not treated as errors. */
static uint16_t emmc_eis_baseline;

static const struct {
	uint32_t khz;
	uint32_t cfg, m, n, d;
	uint8_t timing;         /* the EXT_CSD HS_TIMING the rate needs at least */
} emmc_rates[] = {
	{ 25000u, 0x260bu, 1u, 0xfeu, 0xfeu, 0u },      /* GPLL0_EVEN / 6 * 1/2: default speed */
	{ 50000u, 0x060bu, 0u, 0u, 0u, 1u },            /* GPLL0_EVEN / 6: high speed */
	{ 100000u, 0x010bu, 0u, 0u, 0u, 2u },           /* GPLL0_MAIN / 6: HS200 timing, no tuning */
};

/* Walks emmc_run_buffer in physically contiguous pieces, cleans and invalidates each piece and,
 * when table is given, writes one ADMA2 descriptor per piece. Returns the piece count, 0 when the
 * buffer cannot be described (no translation, above 4 GiB, too many pieces). */
static unsigned
emmc_run_walk(unsigned bytes, uint8_t *table)
{
	vm_offset_t va = (vm_offset_t)emmc_run_buffer;
	unsigned entries = 0;
	for (unsigned done = 0; done < bytes;) {
		vm_offset_t pa = kvtophys(va + done);
		if (pa == 0 || (pa >> 32) != 0 || entries == EMMC_RUN_DESCRIPTORS) {
			return 0;
		}
		unsigned span = 4096u - (unsigned)(pa & 4095u);
		unsigned vspan = 4096u - (unsigned)((va + done) & 4095u);
		if (vspan < span) {
			span = vspan;
		}
		if (span > bytes - done) {
			span = bytes - done;
		}
		flush_dcache64((addr64_t)pa, span, TRUE);
		if (table != 0) {
			uint8_t *desc = table + 8u * entries;
			*(volatile uint16_t *)(desc + 0) = ADMA2_TRAN_VALID;
			*(volatile uint16_t *)(desc + 2) = (uint16_t)span;
			*(volatile uint32_t *)(desc + 4) = (uint32_t)pa;
		}
		entries++;
		done += span;
	}
	return entries;
}

/* Until a write has been programmed the card stays busy (programming state 7); the next command
 * has to wait for READY_FOR_DATA in the transfer state (CMD13). */
static int
emmc_wait_programmed(void)
{
	uint32_t status[4] = { 0, 0, 0, 0 };
	for (unsigned waited = 0; waited < 50000; waited++) {
		if (sdhci_command_capture((uint16_t)((13u << 8) | SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC_CHECK |
		    SDHCI_CMD_INDEX_CHECK), emmc_rca << 16, status) == 0 && (status[0] & (1u << 8)) != 0 &&
		    ((status[0] >> 9) & 0xfu) == 4u) {
			/* OUT_OF_RANGE, ADDRESS_MISALIGN, WP_VIOLATION, CARD_ECC_FAILED, CC_ERROR, ERROR:
			 * the bits that mean the write itself failed. ILLEGAL_COMMAND is left out, as the
			 * abort emmc_recover() sends to an idle card sets it harmlessly. */
			return (status[0] & 0xc4380000u) == 0 ? 0 : -1;
		}
		spin_us(100);
	}
	return -1;
}

/* One data command on emmc_run_buffer: CMD18/CMD25 with Auto-CMD12 for a run, CMD17/CMD24 for a
 * single block. A write's data has to be in emmc_run_buffer already. Quiet: the caller decides
 * what a failure means. */
static int
emmc_run_transfer(uint32_t lba, unsigned blocks, bool write)
{
	if (blocks == 0 || blocks > EMMC_RUN_BLOCKS) {
		return -1;
	}
	vm_offset_t raw = kvtophys((vm_offset_t)emmc_run_table);
	if (raw == 0) {
		return -1;
	}
	vm_offset_t table_pa = (raw + 127u) & ~(vm_offset_t)127u;
	uint8_t *table = emmc_run_table + (table_pa - raw);
	unsigned entries = emmc_run_walk(blocks * 512u, table);
	/* The table itself must be one physical run: the engine walks it by physical address. */
	if (entries == 0 || (table_pa - raw) + 8u * entries > sizeof(emmc_run_table) ||
	    kvtophys((vm_offset_t)(table + 8u * entries - 1u)) != table_pa + 8u * entries - 1u) {
		return -1;
	}
	*(volatile uint16_t *)(table + 8u * (entries - 1u)) = (uint16_t)(ADMA2_TRAN_VALID | ADMA2_END);
	__asm__ volatile ("dsb sy" ::: "memory");
	flush_dcache64((addr64_t)table_pa, 8u * entries, TRUE);
	if (emmc_clear_command_line() != 0) {
		return -1;
	}
	emmc_prepare_transfer();
	sdhci_write8(SDHCI_HOST_CONTROL, (uint8_t)((sdhci_read8(SDHCI_HOST_CONTROL) & 0x27u) | 0x10u));
	sdhci_write32(SDHCI_ADMA_ADDRESS, (uint32_t)table_pa);
	sdhci_write32(SDHCI_ADMA_ADDRESS_HI, 0u);
	sdhci_write16(SDHCI_BLOCK_SIZE, 512u);
	sdhci_write16(SDHCI_BLOCK_COUNT, (uint16_t)blocks);
	uint16_t mode = (uint16_t)(SDHCI_TM_BLOCK_COUNT_ENABLE | SDHCI_TM_DMA);
	if (!write) {
		mode |= SDHCI_TM_READ;
	}
	if (blocks > 1) {
		mode |= SDHCI_TM_MULTI | SDHCI_TM_AUTO_CMD12;
	}
	sdhci_write16(SDHCI_TRANSFER_MODE, mode);
	if ((sdhci_read16(SDHCI_TRANSFER_MODE) & mode) != mode) {
		return -1;
	}
	uint32_t index = write ? (blocks > 1 ? 25u : 24u) : (blocks > 1 ? 18u : 17u);
	if (sdhci_command((uint16_t)((index << 8) | SDHCI_CMD_DATA_PRESENT | SDHCI_CMD_RESP_48 |
	    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), lba) != 0) {
		return -1;
	}
	/* Generous enough for the slowest clock this may run at (400 kHz, 1.3 ms per block), and for
	 * a write's busy signalling after the last block. */
	int done = sdhci_wait(SDHCI_INT_TRANSFER_COMPLETE, 200000u + 20000u * blocks);
	emmc_last_eis = sdhci_read16(SDHCI_ERR_INT_STATUS);
	sdhci_write32(SDHCI_INT_STATUS, 0xffffffffu);
	if (done <= 0 || (emmc_last_eis & SDHCI_EIS_DATA_ERRORS & (uint16_t)~emmc_eis_baseline) != 0) {
		return -1;
	}
	if (write) {
		return emmc_wait_programmed();
	}
	(void)emmc_run_walk(blocks * 512u, 0);
	return 0;
}

static int
emmc_run_read(uint32_t lba, unsigned blocks)
{
	return blocks < 2 ? -1 : emmc_run_transfer(lba, blocks, false);
}

/* After a failed transfer: reset the command and data circuits and stop the card if it is still
 * sending, so the next command finds it in the transfer state. */
static void
emmc_recover(void)
{
	sdhci_write8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
	for (unsigned waited = 0; waited < 20000; waited += 10) {
		if ((sdhci_read8(SDHCI_SOFTWARE_RESET) & (SDHCI_RESET_CMD | SDHCI_RESET_DATA)) == 0) {
			break;
		}
		spin_us(10);
	}
	/* CMD12 (STOP_TRANSMISSION) as an abort command, R1b. A card that is already in the transfer
	 * state rejects it, which is harmless. */
	if (sdhci_command((uint16_t)((12u << 8) | (3u << 6) | SDHCI_CMD_RESP_48_BUSY |
	    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), 0u) == 0) {
		(void)sdhci_wait(SDHCI_INT_TRANSFER_COMPLETE, 100000);
	}
	sdhci_write32(SDHCI_INT_STATUS, 0xffffffffu);
	(void)emmc_clear_command_line();
}

static bool
emmc_matches_reference(const uint8_t *data, unsigned first, unsigned blocks)
{
	const uint8_t *expected = emmc_reference + 512u * first;
	for (unsigned i = 0; i < 512u * blocks; i++) {
		if (data[i] != expected[i]) {
			return false;
		}
	}
	return true;
}

/* Is the current card clock usable? The reference range read back through the path the driver
 * will use (runs when they work, single blocks otherwise) must match byte for byte. */
static bool
emmc_rate_ok(void)
{
	if (emmc_multi) {
		if (emmc_run_read(0u, EMMC_CHECK_BLOCKS) != 0) {
			emmc_recover();
			return false;
		}
		return emmc_matches_reference(emmc_run_buffer, 0u, EMMC_CHECK_BLOCKS);
	}
	uint8_t block[512];
	for (unsigned i = 0; i < EMMC_CHECK_BLOCKS; i++) {
		if (sdhci_read_block_adma2(i, block) != 0) {
			emmc_recover();
			return false;
		}
		if (!emmc_matches_reference(block, i, 1u)) {
			return false;
		}
	}
	return true;
}

/* CMD6 SWITCH, write byte HS_TIMING (EXT_CSD 185) = 1: the card leaves default-speed timing so the
 * clock may exceed 26 MHz. R1b, so wait for the end of busy, then check SWITCH_ERROR and the state
 * with CMD13. */
static bool
emmc_switch_hs(void)
{
	if (sdhci_command((uint16_t)((6u << 8) | SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC_CHECK |
	    SDHCI_CMD_INDEX_CHECK), (3u << 24) | (185u << 16) | (1u << 8)) != 0) {
		emmc_recover();
		return false;
	}
	(void)sdhci_wait(SDHCI_INT_TRANSFER_COMPLETE, 500000);
	sdhci_write32(SDHCI_INT_STATUS, 0xffffffffu);
	uint32_t status[4] = { 0, 0, 0, 0 };
	if (sdhci_command_capture((uint16_t)((13u << 8) | SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC_CHECK |
	    SDHCI_CMD_INDEX_CHECK), emmc_rca << 16, status) != 0) {
		return false;
	}
	return (status[0] & (1u << 7)) == 0 && ((status[0] >> 9) & 0xfu) == 4u;
}

static void
emmc_speed_up(void)
{
	unsigned want = 0;
	if (!PE_parse_boot_argn("sirius_emmc_hs", &want, sizeof(want)) || want == 0) {
		return;
	}
	if (want > sizeof(emmc_rates) / sizeof(emmc_rates[0])) {
		want = sizeof(emmc_rates) / sizeof(emmc_rates[0]);
	}
	/* The reference: the first blocks of the disk at the identification clock, single blocks,
	 * the path every boot so far has read the root through. */
	for (unsigned i = 0; i < EMMC_CHECK_BLOCKS; i++) {
		if (sdhci_read_block_adma2(i, emmc_reference + 512u * i) != 0) {
			ring_puts("EMMC fast: no reference read, staying at 400 kHz\r\n");
			emmc_recover();
			return;
		}
	}
	uint8_t ext[512];
	unsigned timing = 0;
	if (sdhci_read_adma2(8u, 0u, ext) == 0) {
		timing = ext[185];
		emmc_cache_on = (ext[33] & 1u) != 0;     /* CACHE_CTRL: writes may sit in the card's cache */
		ring_puts("EMMC ext_csd rev=");
		ring_dec(ext[192]);
		ring_puts(" width=");
		ring_dec(ext[183]);
		ring_puts(" timing=");
		ring_dec(ext[185]);
		ring_puts(" type=");
		ring_hex(ext[196], 2);
		ring_puts(" cache=");
		ring_dec(ext[33] & 1u);
		ring_puts(" hc=");
		ring_hex(sdhci_read8(SDHCI_HOST_CONTROL), 2);
		ring_puts(" hc2=");
		ring_hex(sdhci_read8(SDHCI_HOST_CONTROL2), 2);
		ring_puts("\r\n");
	} else {
		ring_puts("EMMC ext_csd read failed\r\n");
		emmc_recover();
	}
	/* Runs are proven at the identification clock first, so a failure here says nothing about
	 * the clock. The first run ignores the error status; if its data is right, whatever error
	 * bits it left are this controller's baseline, and a second run has to pass with them. */
	emmc_eis_baseline = 0xffffu;
	if (emmc_run_read(0u, EMMC_CHECK_BLOCKS) == 0 &&
	    emmc_matches_reference(emmc_run_buffer, 0u, EMMC_CHECK_BLOCKS)) {
		emmc_eis_baseline = (uint16_t)(emmc_last_eis & SDHCI_EIS_DATA_ERRORS);
		emmc_multi = emmc_run_read(0u, EMMC_CHECK_BLOCKS) == 0 &&
		    emmc_matches_reference(emmc_run_buffer, 0u, EMMC_CHECK_BLOCKS);
	}
	if (!emmc_multi) {
		emmc_recover();
	}
	ring_puts("EMMC multi=");
	ring_puts(emmc_multi ? "ok" : "off");
	ring_puts(" eis=");
	ring_hex(emmc_last_eis, 4);
	ring_puts(" baseline=");
	ring_hex(emmc_eis_baseline, 4);
	ring_puts("\r\n");

	unsigned good = 0;              /* rates proven so far; 0 = still the identification clock */
	bool last_failed = false;
	for (unsigned step = 0; step < want; step++) {
		if (timing < emmc_rates[step].timing) {
			/* Default timing allows 26 MHz at most: ask the card for high speed first. HS200
			 * needs tuning, so a card that is not already in it stops below 100 MHz. */
			if (emmc_rates[step].timing == 1u && timing == 0u && emmc_switch_hs()) {
				timing = 1u;
			} else {
				break;
			}
		}
		emmc_program_rate(emmc_rates[step].cfg, emmc_rates[step].m, emmc_rates[step].n,
		    emmc_rates[step].d);
		bool ok = emmc_rate_ok();
		ring_puts("EMMC rate ");
		ring_dec(emmc_rates[step].khz);
		ring_puts(" kHz ");
		ring_puts(ok ? "ok" : "failed");
		ring_puts("\r\n");
		if (!ok) {
			last_failed = true;
			break;
		}
		good = step + 1u;
	}
	/* Back to the last rate that read back correctly, and prove it once more. */
	if (last_failed && good != 0u) {
		emmc_program_rate(emmc_rates[good - 1u].cfg, emmc_rates[good - 1u].m,
		    emmc_rates[good - 1u].n, emmc_rates[good - 1u].d);
		if (!emmc_rate_ok()) {
			good = 0u;
		}
	}
	if (good == 0u) {
		(void)emmc_set_card_clock_400k();
		if (!emmc_rate_ok()) {
			emmc_multi = false;
		}
		sirius_emmc_rate_khz = 400u;
	} else {
		sirius_emmc_rate_khz = emmc_rates[good - 1u].khz;
	}
	ring_puts("EMMC fast rate=");
	ring_dec(sirius_emmc_rate_khz);
	ring_puts(" kHz multi=");
	ring_puts(emmc_multi ? "on" : "off");
	ring_puts(" run=");
	ring_dec(EMMC_RUN_BLOCKS);
	ring_puts("\r\n");
}

/* Idempotent: maps the windows, sets the I/O voltage, programs the 400 kHz card clock, finds the
 * card's RCA and selects it. Returns 0 when a block read can be issued. */
int
sirius_emmc_prepare(void)
{
	if (sirius_emmc_ready) {
		return 0;
	}
	if (emmc_base == 0) {
		emmc_base = ml_io_map(SDCC1_PA, SDCC1_SIZE);
		gcc_base = ml_io_map(GCC_PA, GCC_SIZE);
	}
	if (emmc_base == 0 || gcc_base == 0) {
		return -1;
	}
	uint8_t hc2 = sdhci_read8(SDHCI_HOST_CONTROL2);
	sdhci_write8(SDHCI_POWER_CONTROL, 0x0bu);
	sdhci_write8(SDHCI_HOST_CONTROL2, (uint8_t)(hc2 | 0x08u));
	spin_us(1000);
	sdhci_write32(SDHCI_INT_ENABLE, 0xffffffffu);
	sdhci_write16(SDHCI_SIGNAL_ENABLE, 0x0000u);
	sdhci_write8(SDHCI_TIMEOUT_CONTROL, 0x0eu);
	if (emmc_set_card_clock_400k() != 0) {
		return -1;
	}
	/* Last usable LBA from the GPT header bounds the block device. */
	{
		uint8_t header[512];
		if (sdhci_read_block_adma2(1, header) == 0) {
			unsigned long long last = 0;
			for (unsigned i = 0; i < 8; i++) {
				last |= (unsigned long long)header[0x30 + i] << (8 * i);
			}
			sirius_emmc_total_blocks = last + 1;
		}
	}
	/* sdhci_command_capture() stores all four response words. */
	uint32_t status[4] = { 0, 0, 0, 0 };
	for (uint32_t rca = 1; rca <= 8; rca++) {
		if (sdhci_command_capture((uint16_t)((13u << 8) | SDHCI_CMD_RESP_48 |
		    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), rca << 16, status) == 0) {
			(void)sdhci_command_capture((uint16_t)((7u << 8) | SDHCI_CMD_RESP_48_BUSY |
			    SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK), rca << 16, status);
			emmc_rca = rca;
			sirius_emmc_ready = true;
			/* From here on the block driver reads: quiet transfers, and the clock this file
			 * just reset to 400 kHz goes back up as far as it reads back correctly. */
			emmc_trace = false;
			emmc_speed_up();
			return 0;
		}
	}
	return -1;
}

/* Reads one 512-byte block. The caller's buffer may be any alignment: the transfer goes into the
 * page-aligned static buffer and is copied out. */
int
sirius_emmc_read_block(uint64_t lba, void *buffer)
{
	if (!sirius_emmc_ready && sirius_emmc_prepare() != 0) {
		return -1;
	}
	if (lba > 0xffffffffu) {
		return -1;
	}
	uint8_t sector[512];
	if (sdhci_read_block_adma2((uint32_t)lba, sector) != 0) {
		return -1;
	}
	for (unsigned i = 0; i < 512; i++) {
		((uint8_t *)buffer)[i] = sector[i];
	}
	return 0;
}

/* Reads count (1..sirius_emmc_run_blocks) consecutive 512-byte blocks: one multi-block command
 * when runs work, single blocks otherwise. A failed run turns runs off for the rest of the boot
 * and the same request is retried block by block. */
int
sirius_emmc_read_blocks(uint64_t lba, unsigned count, void *buffer)
{
	if (!sirius_emmc_ready && sirius_emmc_prepare() != 0) {
		return -1;
	}
	if (count == 0 || count > EMMC_RUN_BLOCKS || lba + count - 1u > 0xffffffffu) {
		return -1;
	}
	uint8_t *out = (uint8_t *)buffer;
	if (emmc_multi && count > 1) {
		if (emmc_run_read((uint32_t)lba, count) == 0) {
			__builtin_memcpy(out, emmc_run_buffer, 512u * count);
			return 0;
		}
		emmc_multi = false;
		ring_puts("EMMC run failed lba=");
		ring_hex((uint32_t)lba, 8);
		ring_puts(" count=");
		ring_dec(count);
		ring_puts(" eis=");
		ring_hex(emmc_last_eis, 4);
		ring_puts(", single blocks from now on\r\n");
		emmc_recover();
	}
	for (unsigned i = 0; i < count; i++) {
		if (sdhci_read_block_adma2((uint32_t)lba + i, out + 512u * i) != 0) {
			return -1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------- *
 * Writes, for the persistent Data volume only. Two fences: the window the block driver opens once
 * (the Data container behind the root image), and, whatever that window says, the userdata
 * partition minus its first MiB (the SHNGROOT manifest). Nothing else on this card is writable.
 * ------------------------------------------------------------------------- */

#define EMMC_USERDATA_FIRST_LBA 17301504ull     /* board.toml [userdata] first_lba */
#define EMMC_USERDATA_LAST_LBA 122142686ull     /* board.toml [userdata] last_lba */

static uint64_t emmc_write_first, emmc_write_end;      /* [first, end); end 0 = no window */
volatile uint32_t sirius_emmc_writes;
volatile uint32_t sirius_emmc_write_failures;

/* Opens the write window once; a later call cannot move or widen it. */
int
sirius_emmc_set_write_window(uint64_t first, uint64_t blocks)
{
	if (emmc_write_end != 0 || blocks == 0 || first < EMMC_USERDATA_FIRST_LBA + 2048u ||
	    first + blocks - 1u > EMMC_USERDATA_LAST_LBA) {
		return -1;
	}
	emmc_write_first = first;
	emmc_write_end = first + blocks;
	ring_puts("EMMC write window lba=");
	ring_dec((uint32_t)first);
	ring_puts(" blocks=");
	ring_dec((uint32_t)blocks);
	ring_puts(" cache=");
	ring_dec(emmc_cache_on ? 1u : 0u);
	ring_puts("\r\n");
	return 0;
}

int
sirius_emmc_write_blocks(uint64_t lba, unsigned count, const void *buffer)
{
	if (!sirius_emmc_ready || count == 0 || count > EMMC_RUN_BLOCKS || emmc_write_end == 0 ||
	    lba < emmc_write_first || lba + count > emmc_write_end ||
	    lba < EMMC_USERDATA_FIRST_LBA + 2048u || lba + count - 1u > EMMC_USERDATA_LAST_LBA) {
		sirius_emmc_write_failures++;
		return -1;
	}
	const uint8_t *in = (const uint8_t *)buffer;
	sirius_emmc_writes++;
	if (emmc_multi && count > 1) {
		__builtin_memcpy(emmc_run_buffer, in, 512u * count);
		if (emmc_run_transfer((uint32_t)lba, count, true) == 0) {
			return 0;
		}
		emmc_multi = false;
		ring_puts("EMMC write run failed lba=");
		ring_hex((uint32_t)lba, 8);
		ring_puts(" eis=");
		ring_hex(emmc_last_eis, 4);
		ring_puts(", single blocks from now on\r\n");
		emmc_recover();
	}
	for (unsigned i = 0; i < count; i++) {
		__builtin_memcpy(emmc_run_buffer, in + 512u * i, 512u);
		if (emmc_run_transfer((uint32_t)lba + i, 1u, true) != 0) {
			emmc_recover();
			sirius_emmc_write_failures++;
			return -1;
		}
	}
	return 0;
}

/* Makes written data durable: with the card's cache on, CMD6 FLUSH_CACHE (EXT_CSD 32 = 1). */
int
sirius_emmc_flush(void)
{
	if (!sirius_emmc_ready || !emmc_cache_on) {
		return 0;
	}
	if (sdhci_command((uint16_t)((6u << 8) | SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC_CHECK |
	    SDHCI_CMD_INDEX_CHECK), (3u << 24) | (32u << 16) | (1u << 8)) != 0) {
		emmc_recover();
		return -1;
	}
	(void)sdhci_wait(SDHCI_INT_TRANSFER_COMPLETE, 2000000);
	sdhci_write32(SDHCI_INT_STATUS, 0xffffffffu);
	return emmc_wait_programmed();
}
