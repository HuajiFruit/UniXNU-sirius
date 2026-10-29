// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * Sirius touch bring-up: the ST FTS521 touch controller (FingerTip, I2C address 0x49) on QUP1 SE1 ("i2c9", 0xa84000).
 *
 * Every value comes from 57UU/linux-sirius (feat/sirius-7.2): arch/arm64/boot/dts/qcom/sdm710-xiaomi-sirius.dts and sdm670.dtsi
 * for the wiring, drivers/i2c/busses/i2c-qcom-geni.c and drivers/soc/qcom/qcom-geni-se.c for the controller, gcc-sdm845.c for the
 * clocks, pinctrl-sdm670.c for the pins and drivers/input/touchscreen/fts_521 for the chip protocol.
 *
 * Wiring: SDA/SCL on gpio6/gpio7 (function qup9), interrupt on gpio125 (active low), reset on gpio99, the I/O rail on gpio26
 * (tp_vddio_vreg, already on from boot) and the analog rail on PM660L LDO6 (vreg_l6b_3p3, an RPMh regulator this port cannot
 * vote for yet). The SE runs from XO (19.2 MHz) with the Linux 400 kHz divider set.
 *
 * sirius-touch=1 starts one kernel thread from the late display probe. It votes the QUP clocks on, programs the pins, resets the
 * chip, reads its chip ID (0x36 0x39 for an FTM5), and if that answers it turns sensing on and polls the event FIFO. Contacts are
 * published through native_touch_* (the board side of ShengHidInput) and every step is a TOUCH console line. The SE is touched only
 * after its clocks report running: an unclocked register access stalls the bus.
 */

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef unsigned long vm_offset_t;
typedef unsigned long long uint64_t;

extern vm_offset_t ml_io_map(vm_offset_t phys, unsigned long size);
extern int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, unsigned int max_len);
extern int printf(const char *format, ...);
extern void IOSleep(unsigned int milliseconds);
extern int kernel_thread_start(void (*continuation)(void *, int), void *parameter, void **new_thread);
extern void thread_deallocate(void *thread);

#define GCC_BASE            0x00100000u
#define GCC_QUP1_CBCR_PAGE  0x00018000u     /* wrap1 AHB branches and the SE1 branch and RCG */
#define GCC_VOTE_PAGE       0x00052000u
#define GCC_M_AHB_CBCR      0x00cu          /* gcc_qupv3_wrap_1_m_ahb_clk halt register (0x1800c) */
#define GCC_S_AHB_CBCR      0x010u          /* gcc_qupv3_wrap_1_s_ahb_clk (0x18010) */
#define GCC_S1_CBCR         0x144u          /* gcc_qupv3_wrap1_s1_clk (0x18144) */
#define GCC_S1_CMD_RCGR     0x148u          /* gcc_qupv3_wrap1_s1_clk_src (0x18148), CFG at +4 */
#define GCC_APCS_VOTE_1     0x00cu          /* 0x5200c: bit 20 m_ahb, bit 21 s_ahb, bit 23 s1 */
#define GCC_VOTE_QUP1_S1    ((1u << 20) | (1u << 21) | (1u << 23))
#define CBCR_CLK_OFF        (1u << 31)

#define TLMM_BASE           0x03400000u
#define TLMM_NORTH          0x00500000u
#define TLMM_WEST           0x00100000u
#define TLMM_CFG_PULL_UP    3u
#define TLMM_CFG_FUNC(f)    ((uint32_t)(f) << 2)
#define TLMM_CFG_DRIVE(ma)  ((uint32_t)((ma) / 2 - 1) << 6)
#define TLMM_CFG_OE         (1u << 9)
#define TLMM_IO_IN          (1u << 0)
#define TLMM_IO_OUT         (1u << 1)

#define QUP1_WRAPPER        0x00ac0000u
#define SE_BASE             0x00a84000u     /* QUP1 SE1 */
#define SE_SIZE             0x4000u
#define GENI_FORCE_DEFAULT_REG  0x20
#define GENI_OUTPUT_CTRL        0x24
#define SE_GENI_CGC_CTRL        0x28
#define SE_GENI_STATUS          0x40
#define GENI_SER_M_CLK_CFG      0x48
#define GENI_IF_DISABLE_RO      0x64
#define GENI_FW_REVISION_RO     0x68
#define SE_GENI_CLK_SEL         0x7c
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE_EN     0x258
#define SE_GENI_TX_PACKING_CFG0 0x260
#define SE_GENI_TX_PACKING_CFG1 0x264
#define SE_I2C_TX_TRANS_LEN     0x26c
#define SE_I2C_RX_TRANS_LEN     0x270
#define SE_I2C_SCL_COUNTERS     0x278
#define SE_GENI_RX_PACKING_CFG0 0x284
#define SE_GENI_RX_PACKING_CFG1 0x288
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_CMD_CTRL_REG  0x604
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_EN        0x614
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFOn        0x700
#define SE_GENI_RX_FIFOn        0x780
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_TX_WATERMARK_REG 0x80c
#define SE_GENI_RX_WATERMARK_REG 0x810
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814
#define SE_DMA_TX_IRQ_CLR       0xc44
#define SE_DMA_RX_IRQ_CLR       0xd44
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_HW_PARAM_0           0xe24
#define SE_DMA_GENERAL_CFG      0xe30

#define M_CMD_DONE          (1u << 0)
#define M_CMD_OVERRUN       (1u << 1)
#define M_ILLEGAL_CMD       (1u << 2)
#define M_CMD_FAILURE       (1u << 3)
#define M_CMD_CANCEL        (1u << 4)
#define M_CMD_ABORT         (1u << 5)
#define M_GP_IRQ_1          (1u << 10)      /* NACK */
#define M_GP_IRQ_3          (1u << 12)      /* bus protocol error */
#define M_GP_IRQ_4          (1u << 13)      /* arbitration lost */
#define M_RX_FIFO_WATERMARK (1u << 26)
#define M_RX_FIFO_LAST      (1u << 27)
#define M_TX_FIFO_WATERMARK (1u << 30)
#define M_I2C_ERRORS        (M_CMD_OVERRUN | M_ILLEGAL_CMD | M_CMD_FAILURE | M_GP_IRQ_1 | M_GP_IRQ_3 | M_GP_IRQ_4)
#define M_GENI_CMD_ABORT    (1u << 1)
#define I2C_WRITE           1u
#define I2C_READ            2u
#define I2C_STOP_STRETCH    (1u << 2)
#define I2C_SLAVE(a)        (((uint32_t)(a) & 0x7f) << 9)
#define GENI_SE_I2C         3u

#define FTS_ADDRESS         0x49
#define FTS_EVENT_SIZE      8
#define FTS_READ_EVENT      0x86            /* FIFO_CMD_READONE over I2C */
#define FTS_HW_REG          0xfa            /* FTS_CMD_HW_REG_R/W over I2C, then a 4-byte address */
#define FTS_SCAN_MODE       0xa0
#define EVT_NOEVENT         0x00
#define EVT_CONTROLLER_READY 0x03
#define EVT_ENTER           0x13
#define EVT_MOTION          0x23
#define EVT_LEAVE           0x33
#define EVT_ERROR           0xf3
#define FTS_X_MAX           1080
#define FTS_Y_MAX           2244
/* ShengHidInput normalises X by 30480 and Y by 20320 (the sheng touch core's axes); publishing in those units keeps its scaling
 * exact for this panel's 1080 x 2244. */
#define HID_X_UNITS         30480
#define HID_Y_UNITS         20320

static volatile uint32_t *gcc_cbcr, *gcc_vote, *se;
static vm_offset_t tlmm_north, tlmm_west;

/* The board side of ShengHidInput: a finger set behind a seqlock and a sink to wake its work loop. */
volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
static void (*touch_sink)(void *);
static void *touch_sink_context;
void
native_touch_set_sink(void (*sink)(void *), void *context)
{
	touch_sink_context = context;
	__asm__ volatile ("dmb ish" ::: "memory");
	touch_sink = sink;
}

/* [0] stage reached, [1] chip id, [2] fw version, [3] events read, [4] contacts begun, [5] I2C errors, [6] last M_IRQ status */
volatile uint32_t sirius_touch_state[8];

static void
spin_us(unsigned usec)
{
	volatile unsigned ticks = usec * 100u;
	while (ticks > 0) {
		ticks--;
	}
}

static inline uint32_t se_read(uint32_t offset) { return se[offset / 4]; }
static inline void se_write(uint32_t offset, uint32_t value) { se[offset / 4] = value; __asm__ volatile ("dsb sy" ::: "memory"); }

static volatile uint32_t *
tlmm_cfg(unsigned gpio)
{
	vm_offset_t tile = (gpio == 26) ? tlmm_west : tlmm_north;
	return (volatile uint32_t *)(tile + 0x1000u * gpio);
}

static void
gpio_output(unsigned gpio, int high)
{
	volatile uint32_t *cfg = tlmm_cfg(gpio);
	cfg[1] = high ? TLMM_IO_OUT : 0;
	cfg[0] = TLMM_CFG_FUNC(0) | TLMM_CFG_DRIVE(16) | TLMM_CFG_OE;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static int
gpio_input(unsigned gpio)
{
	return (tlmm_cfg(gpio)[1] & TLMM_IO_IN) != 0;
}

/* One FIFO-mode transfer, polled; the SE interrupt (SPI 354) is never enabled. Returns 0, or the M_IRQ status bits of the error. */
static uint32_t
i2c_transfer(uint32_t op, const uint8_t *tx, uint8_t *rx, unsigned length, uint32_t params)
{
	unsigned done = 0, words = 0, spins = 0;
	uint32_t status = 0, seen = 0;
	se_write(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	if (op == I2C_WRITE) {
		se_write(SE_I2C_TX_TRANS_LEN, length);
	} else {
		se_write(SE_I2C_RX_TRANS_LEN, length);
	}
	se_write(SE_GENI_M_CMD0, (op << 27) | I2C_SLAVE(FTS_ADDRESS) | params);
	if (op == I2C_WRITE) {
		se_write(SE_GENI_TX_WATERMARK_REG, 1);
	}
	for (spins = 0; spins < 20000; ++spins) {
		status = se_read(SE_GENI_M_IRQ_STATUS);
		seen |= status;
		if (op == I2C_WRITE && (status & M_TX_FIFO_WATERMARK) && done < length) {
			for (words = 0; words < 8 && done < length; ++words) {
				uint32_t value = 0;
				for (unsigned b = 0; b < 4 && done < length; ++b) {
					value |= (uint32_t)tx[done++] << (8 * b);
				}
				se_write(SE_GENI_TX_FIFOn, value);
			}
			if (done == length) {
				se_write(SE_GENI_TX_WATERMARK_REG, 0);
			}
		}
		if (op == I2C_READ && (status & (M_RX_FIFO_WATERMARK | M_RX_FIFO_LAST))) {
			uint32_t count = se_read(SE_GENI_RX_FIFO_STATUS) & 0x1ffffffu;
			for (words = 0; words < count; ++words) {
				uint32_t value = se_read(SE_GENI_RX_FIFOn);
				for (unsigned b = 0; b < 4 && done < length; ++b) {
					rx[done++] = (uint8_t)(value >> (8 * b));
				}
			}
		}
		if (status) {
			se_write(SE_GENI_M_IRQ_CLEAR, status);
		}
		if (status & M_CMD_DONE) {
			break;
		}
		spin_us(5);
	}
	/* A short read can complete within one poll: take whatever the RX FIFO still holds. */
	if (op == I2C_READ) {
		uint32_t count = se_read(SE_GENI_RX_FIFO_STATUS) & 0x1ffffffu;
		for (words = 0; words < count && words < 16; ++words) {
			uint32_t value = se_read(SE_GENI_RX_FIFOn);
			for (unsigned b = 0; b < 4 && done < length; ++b) {
				rx[done++] = (uint8_t)(value >> (8 * b));
			}
		}
	}
	sirius_touch_state[6] = seen;
	sirius_touch_state[7] = done;
	if (!(seen & M_CMD_DONE) || (seen & M_I2C_ERRORS)) {
		if (!(seen & M_CMD_DONE)) {
			/* Linux geni_i2c_abort_xfer: abort the command and wait for its acknowledgement. */
			se_write(SE_GENI_M_CMD_CTRL_REG, M_GENI_CMD_ABORT);
			for (spins = 0; spins < 2000 && !(se_read(SE_GENI_M_IRQ_STATUS) & M_CMD_ABORT); ++spins) {
				spin_us(5);
			}
			se_write(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
		}
		se_write(SE_GENI_TX_WATERMARK_REG, 0);
		sirius_touch_state[5] = sirius_touch_state[5] + 1;
		return seen | (1u << 31);
	}
	return 0;
}

/* Write tx, then read rx with a repeated start (Linux: two messages, the first with STOP_STRETCH). */
static uint32_t
fts_write_read(const uint8_t *tx, unsigned tx_length, uint8_t *rx, unsigned rx_length)
{
	uint32_t error = i2c_transfer(I2C_WRITE, tx, 0, tx_length, rx_length ? I2C_STOP_STRETCH : 0);
	if (error || !rx_length) {
		return error;
	}
	return i2c_transfer(I2C_READ, 0, rx, rx_length, 0);
}

static uint32_t
fts_read_event(uint8_t event[FTS_EVENT_SIZE])
{
	static const uint8_t command[] = { FTS_READ_EVENT };
	return fts_write_read(command, sizeof(command), event, FTS_EVENT_SIZE);
}

static int
clocks_on(void)
{
	gcc_vote[GCC_APCS_VOTE_1 / 4] = gcc_vote[GCC_APCS_VOTE_1 / 4] | GCC_VOTE_QUP1_S1;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (unsigned i = 0; i < 1000; ++i) {
		if (!(gcc_cbcr[GCC_M_AHB_CBCR / 4] & CBCR_CLK_OFF) && !(gcc_cbcr[GCC_S_AHB_CBCR / 4] & CBCR_CLK_OFF) &&
		    !(gcc_cbcr[GCC_S1_CBCR / 4] & CBCR_CLK_OFF)) {
			break;
		}
		spin_us(10);
	}
	uint32_t m = gcc_cbcr[GCC_M_AHB_CBCR / 4], s = gcc_cbcr[GCC_S_AHB_CBCR / 4], c = gcc_cbcr[GCC_S1_CBCR / 4];
	uint32_t cmd = gcc_cbcr[GCC_S1_CMD_RCGR / 4], cfg = gcc_cbcr[GCC_S1_CMD_RCGR / 4 + 1];
	printf("TOUCH clk vote=%08x m_ahb=%08x s_ahb=%08x s1=%08x rcg cmd=%08x cfg=%08x\n", gcc_vote[GCC_APCS_VOTE_1 / 4], m, s, c, cmd, cfg);
	if ((m | s | c) & CBCR_CLK_OFF) {
		return 0;
	}
	/* XO, no divider, no M/N: the 19.2 MHz the Linux 400 kHz counters assume. */
	if ((cfg & 0x71fu) != 0) {
		gcc_cbcr[GCC_S1_CMD_RCGR / 4 + 1] = cfg & ~0x371fu;
		gcc_cbcr[GCC_S1_CMD_RCGR / 4] = cmd | 1u;
		__asm__ volatile ("dsb sy" ::: "memory");
		for (unsigned i = 0; i < 1000 && (gcc_cbcr[GCC_S1_CMD_RCGR / 4] & 1u); ++i) {
			spin_us(10);
		}
		printf("TOUCH rcg now cmd=%08x cfg=%08x\n", gcc_cbcr[GCC_S1_CMD_RCGR / 4], gcc_cbcr[GCC_S1_CMD_RCGR / 4 + 1]);
	}
	return 1;
}

static int
se_setup(void)
{
	uint32_t fw = se_read(GENI_FW_REVISION_RO), disable = se_read(GENI_IF_DISABLE_RO), param = se_read(SE_HW_PARAM_0);
	uint32_t depth = (param >> 16) & 0x3f;
	printf("TOUCH se fw=%08x proto=%u fifo_disable=%u tx_depth=%u status=%08x\n", fw, (fw >> 8) & 0xff, disable & 1, depth,
	    se_read(SE_GENI_STATUS));
	if (((fw >> 8) & 0xff) != GENI_SE_I2C || (disable & 1) || depth < 2) {
		return 0;
	}
	/* geni_se_init(): clear and route the interrupts, clock gating on, FIFO (not DMA) mode, watermarks. */
	se_write(SE_GSI_EVENT_EN, 0);
	se_write(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	se_write(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	se_write(SE_DMA_TX_IRQ_CLR, 0xffffffffu);
	se_write(SE_DMA_RX_IRQ_CLR, 0xffffffffu);
	se_write(SE_GENI_CGC_CTRL, se_read(SE_GENI_CGC_CTRL) | 0x7fu);
	se_write(SE_DMA_GENERAL_CFG, se_read(SE_DMA_GENERAL_CFG) | 0xfu);
	se_write(GENI_OUTPUT_CTRL, 0x7fu);
	se_write(GENI_FORCE_DEFAULT_REG, 1);
	se_write(SE_IRQ_EN, se_read(SE_IRQ_EN) | 0xfu);
	se_write(SE_GENI_DMA_MODE_EN, se_read(SE_GENI_DMA_MODE_EN) & ~1u);
	se_write(SE_GENI_RX_WATERMARK_REG, depth - 1);
	se_write(SE_GENI_RX_RFR_WATERMARK_REG, depth);
	se_write(SE_GENI_M_IRQ_EN, 0x7fffffffu);  /* every status bit visible except the secondary-sequencer summary */
	/* geni_se_config_packing(8 bits, 4 per word, msb_to_lsb = true) for both directions, as i2c-qcom-geni.c: I2C sends each byte
	 * MSB first. (LSB-first packing put every byte on the wire bit-reversed: the chip acked its address and read back zeros.) */
	se_write(SE_GENI_TX_PACKING_CFG0, 0x0007f8feu);
	se_write(SE_GENI_TX_PACKING_CFG1, 0x000ffefeu);
	se_write(SE_GENI_RX_PACKING_CFG0, 0x0007f8feu);
	se_write(SE_GENI_RX_PACKING_CFG1, 0x000ffefeu);
	se_write(SE_GENI_BYTE_GRAN, 0);
	/* qcom_geni_i2c_conf() with geni_i2c_clk_map_19p2mhz for 400 kHz: divider 2, t_high 5, t_low 11, t_cycle 22. */
	se_write(SE_GENI_CLK_SEL, 0);
	se_write(GENI_SER_M_CLK_CFG, (2u << 4) | 1u);
	se_write(SE_I2C_SCL_COUNTERS, (5u << 20) | (11u << 10) | 22u);
	return 1;
}

/* Contacts by FTS touch id (0..15): the published set is rebuilt from it after every batch of events. */
static int contact_x[16], contact_y[16];
static uint16_t contact_down;

static void
publish_contacts(void)
{
	unsigned count = 0;
	native_touch_seq = native_touch_seq + 1;
	__asm__ volatile ("dmb ish" ::: "memory");
	for (unsigned id = 0; id < 16 && count < 10; ++id) {
		if (contact_down & (1u << id)) {
			native_touch_ids[count] = (int)id;
			native_touch_xs[count] = contact_x[id] * HID_X_UNITS / FTS_X_MAX;
			native_touch_ys[count] = contact_y[id] * HID_Y_UNITS / FTS_Y_MAX;
			++count;
		}
	}
	native_touch_count = count;
	__asm__ volatile ("dmb ish" ::: "memory");
	native_touch_seq = native_touch_seq + 1;
	void (*sink)(void *) = touch_sink;
	if (sink) {
		sink(touch_sink_context);
	}
}

/* fts_enter_pointer_event_handler: id in the high nibble of byte 1, X = byte 2 | low nibble of byte 3 << 8,
 * Y = byte 4 << 4 | high nibble of byte 3. */
static int
handle_event(const uint8_t *event)
{
	unsigned id = event[1] >> 4;
	int x = (((int)event[3] & 0x0f) << 8) | event[2], y = ((int)event[4] << 4) | ((event[3] & 0xf0) >> 4);
	switch (event[0]) {
	case EVT_ENTER:
	case EVT_MOTION:
		if (!(contact_down & (1u << id))) {
			sirius_touch_state[4] = sirius_touch_state[4] + 1;
			printf("TOUCH down id=%u x=%d y=%d\n", id, x, y);
		} else if ((sirius_touch_state[3] & 31) == 0) {
			printf("TOUCH move id=%u x=%d y=%d\n", id, x, y);
		}
		contact_down |= (uint16_t)(1u << id);
		contact_x[id] = x;
		contact_y[id] = y;
		return 1;
	case EVT_LEAVE:
		if (contact_down & (1u << id)) {
			printf("TOUCH up id=%u x=%d y=%d\n", id, x, y);
		}
		contact_down &= (uint16_t)~(1u << id);
		return 1;
	case EVT_ERROR:
		printf("TOUCH chip error event %02x %02x %02x %02x\n", event[1], event[2], event[3], event[4]);
		return 0;
	default:
		return 0;
	}
}

static void
touch_thread(void *parameter, int wait_result)
{
	(void)parameter;
	(void)wait_result;
	uint8_t data[FTS_EVENT_SIZE];
	gcc_cbcr = (volatile uint32_t *)ml_io_map(GCC_BASE + GCC_QUP1_CBCR_PAGE, 0x1000);
	gcc_vote = (volatile uint32_t *)ml_io_map(GCC_BASE + GCC_VOTE_PAGE, 0x1000);
	tlmm_north = ml_io_map(TLMM_BASE + TLMM_NORTH, 0x80000);   /* up to gpio127 */
	tlmm_west = ml_io_map(TLMM_BASE + TLMM_WEST, 0x20000);
	volatile uint32_t *wrapper = (volatile uint32_t *)ml_io_map(QUP1_WRAPPER, 0x1000);
	if (!gcc_cbcr || !gcc_vote || !tlmm_north || !tlmm_west || !wrapper) {
		printf("TOUCH map failed\n");
		return;
	}
	sirius_touch_state[0] = 1;
	if (!clocks_on()) {
		printf("TOUCH QUP1 SE1 clocks stay off; the SE is not touched\n");
		return;
	}
	sirius_touch_state[0] = 2;
	printf("TOUCH qup1 hw_ver=%08x\n", wrapper[1]);
	se = (volatile uint32_t *)ml_io_map(SE_BASE, SE_SIZE);
	if (!se || !se_setup()) {
		printf("TOUCH SE1 is not an I2C engine with a FIFO; stopping\n");
		return;
	}
	sirius_touch_state[0] = 3;
	/* Pins: SDA/SCL to qup9 with pull-ups, the interrupt input pulled up, the I/O rail on, then a reset pulse. */
	tlmm_cfg(6)[0] = TLMM_CFG_FUNC(1) | TLMM_CFG_DRIVE(2) | TLMM_CFG_PULL_UP;
	tlmm_cfg(7)[0] = TLMM_CFG_FUNC(1) | TLMM_CFG_DRIVE(2) | TLMM_CFG_PULL_UP;
	tlmm_cfg(125)[0] = TLMM_CFG_FUNC(0) | TLMM_CFG_DRIVE(16) | TLMM_CFG_PULL_UP;
	gpio_output(26, 1);
	gpio_output(99, 0);
	IOSleep(10);
	gpio_output(99, 1);
	IOSleep(150);
	printf("TOUCH pins sda/scl cfg=%08x/%08x irq=%u reset=%u vddio=%u\n", tlmm_cfg(6)[0], tlmm_cfg(7)[0], gpio_input(125),
	    gpio_input(99), gpio_input(26));

	static const uint8_t chip_id[] = { FTS_HW_REG, 0x20, 0x00, 0x00, 0x00 };   /* ADDR_DCHIP_ID, then the firmware version */
	uint8_t id[8] = { 0 };
	uint32_t error = fts_write_read(chip_id, sizeof(chip_id), id, sizeof(id));
	printf("TOUCH chip id error=%08x irq=%08x read=%u bytes %02x %02x %02x %02x %02x %02x %02x %02x\n", error, sirius_touch_state[6],
	    sirius_touch_state[7], id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
	if (error) {
		printf("TOUCH no answer at 0x49: if it NACKs, the analog rail (PM660L LDO6) is likely off\n");
		return;
	}
	sirius_touch_state[1] = ((uint32_t)id[0] << 8) | id[1];
	sirius_touch_state[2] = ((uint32_t)id[4] << 8) | id[5];
	sirius_touch_state[0] = 4;

	/* fts_system_reset: 0x80 to ADDR_SYSTEM_RESET, then wait for EVT_ID_CONTROLLER_READY. */
	static const uint8_t reset[] = { FTS_HW_REG, 0x20, 0x00, 0x00, 0x24, 0x80 };
	error = fts_write_read(reset, sizeof(reset), 0, 0);
	int ready = 0;
	for (unsigned i = 0; i < 50 && !error && !ready; ++i) {
		IOSleep(10);
		if (!fts_read_event(data) && data[0] == EVT_CONTROLLER_READY) {
			ready = 1;
		}
	}
	printf("TOUCH reset error=%08x ready=%d\n", error, ready);
	/* Sense on: SCAN_MODE_ACTIVE with ACTIVE_MULTI_TOUCH (fts_mode_handler). */
	static const uint8_t sense_on[] = { FTS_SCAN_MODE, 0x00, 0x01 };
	error = fts_write_read(sense_on, sizeof(sense_on), 0, 0);
	printf("TOUCH sense on error=%08x irq=%u\n", error, gpio_input(125));
	sirius_touch_state[0] = 5;

	for (;;) {
		int changed = 0;
		for (unsigned n = 0; n < 32; ++n) {
			if (fts_read_event(data) || data[0] == EVT_NOEVENT) {
				break;
			}
			sirius_touch_state[3] = sirius_touch_state[3] + 1;
			changed |= handle_event(data);
		}
		if (changed) {
			publish_contacts();
		}
		IOSleep(contact_down ? 8 : 16);
	}
}

/* Called from the late display probe (package/LegacyDisplayProbe.cpp). */
void
sirius_touch_start(void)
{
	unsigned enabled = 0;
	void *thread = 0;
	PE_parse_boot_argn("sirius-touch", &enabled, sizeof(enabled));
	if (enabled != 1) {
		return;
	}
	if (kernel_thread_start(touch_thread, 0, &thread) == 0) {
		thread_deallocate(thread);
	} else {
		printf("TOUCH thread start failed\n");
	}
}
