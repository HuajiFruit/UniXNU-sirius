// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * Sirius Adreno 616 bring-up. Phase 1: the GPU's CX side (no firmware, no GX). Phase 2: the GMU's firmware boot.
 *
 * Values from 57UU/linux-sirius (feat/sirius-7.2): sdm670.dtsi (gpu 0x05000000, gmu 0x0506a000, gpucc 0x05090000,
 * adreno_smmu 0x05040000 and their clocks), drivers/clk/qcom/gcc-sdm845.c and gpucc-sdm845.c (branches, the GMU RCG, the CX
 * GDSC), drivers/clk/qcom/gdsc.c (the enable sequence) and drivers/gpu/drm/msm/registers/adreno/a6xx_gmu.xml (GMU registers,
 * dword offsets from the GPU base: byte address = 0x05000000 + 4 * offset, gmu_read()).
 *
 * The order follows a6xx_gmu_resume() and the clock framework: the GCC GPLL0 votes and gpu_cfg_ahb (the GPU CC's register
 * interface) first, then the CX GDSC (VOTABLE, status in gds_hw_ctrl), then the GCC branches into the CX domain (memnoc_gfx,
 * snoc_dvm_gfx, ddrss_gpu_axi), then the GMU clock source at 200 MHz (GPLL0 main div / 1.5, rcg2_shared: force the root on
 * around the update) and its cx_gmu and cxo branches. After that phase 1 only reads: GMU state registers, the GMU always-on
 * counter twice, and the GPU SMMU's global ID registers and stream mapping.
 *
 * Phase 2 is a6xx_gmu_resume() for a legacy (A630-family, HFI v1) GMU from a6xx_gmu.c and a6xx_hfi.c, without the GPU (GX) side:
 *  1. 64 KiB physically contiguous below 4 GiB: three page-table pages, then the GMU's dummy page and its 16 KiB debug, log
 *     and HFI buffers at iova 0x60000000.. (its uncached range, a6xx_gmu_memory_alloc). Cached, cleaned and invalidated around
 *     every exchange (the walker and the GMU read it non-cacheable).
 *  2. The GPU SMMU: context bank 1 (bank 0 stays for the GPU, whose CP switches TTBR0 there) as a stage-1 AArch64 context with
 *     4 KiB pages, then S2CR 1 and SMR 1 for stream 5 (sdm670.dtsi gmu iommus). The boot chain leaves this SMMU empty (phase
 *     2a); Linux programs it from scratch the same way (arm-smmu.c). An ATS1PR translation checks the tables first.
 *  3. The HFI queue table (a6xx_hfi_init), the RSCC and PDC sequences (a6xx_gmu_rpmh_init), the firmware into ITCM
 *     (a6xx_gmu_fw_load, legacy), the boot registers (a6xx_gmu_fw_start), the CM3 out of reset (a6xx_gmu_start), the GX rail
 *     vote and SPTPRAC (legacy), HFI_CTRL_INIT, then the HFI v1 INIT and FW_VERSION messages (a6xx_hfi_start_v1).
 * The firmware is a630_gmu.bin, embedded at build time by scripts/build-sirius-gpu.py (pinned commit and SHA-256). The GMU is
 * left running in its boot state (OOB BOOT_SLUMBER set); the perf and bandwidth tables, GX, SQE and the CP are phase 3.
 *
 * sirius-gpu=1 runs phase 1, =2 phases 1 and 2, once on a kernel thread from the late display probe; every step is a GPU
 * console line, and phase 2's result is the panel's GPU7 row.
 */

typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long vm_offset_t;

extern vm_offset_t ml_io_map(vm_offset_t phys, unsigned long size);
extern int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, unsigned int max_len);
extern int printf(const char *format, ...);
extern int snprintf(char *buffer, unsigned long size, const char *format, ...);
extern void IOSleep(unsigned int milliseconds);
extern void IODelay(unsigned int microseconds);
extern int kernel_thread_start(void (*continuation)(void *, int), void *parameter, void **new_thread);
extern void thread_deallocate(void *thread);
extern void flush_dcache64(uint64_t addr, unsigned count, int phys);
extern void *sirius_gpu_dma_alloc(uint32_t bytes, uint64_t *physical);   /* package/LegacyDisplayProbe.cpp */
extern const uint32_t sirius_gmu_fw[];                                    /* a630_gmu.bin, scripts/build-sirius-gpu.py */
extern const uint32_t sirius_gmu_fw_words;
extern char boot_progress_ext_wide[2][112] __attribute__((weak_import));

#define GCC_BASE                0x00100000u
#define GCC_DDRSS_GPU_AXI_CBCR  0x44038u
#define GCC_APCS_VOTE           0x52004u    /* bit 15 gcc_gpu_gpll0_clk_src, bit 16 gcc_gpu_gpll0_div_clk_src */
#define GCC_GPU_CFG_AHB_CBCR    0x71004u
#define GCC_GPU_MEMNOC_GFX_CBCR 0x7100cu
#define GCC_GPU_SNOC_DVM_CBCR   0x71018u
#define CBCR_ENABLE             (1u << 0)
#define CBCR_CLK_OFF            (1u << 31)

#define GPUCC_BASE              0x05090000u
#define GPUCC_GX_GDSCR          0x100cu
#define GPUCC_CX_GDSCR          0x106cu
#define GPUCC_CX_GMU_CBCR       0x1098u
#define GPUCC_CXO_CBCR          0x109cu
#define GPUCC_GMU_CMD_RCGR      0x1120u     /* CFG at +4 */
#define GPUCC_CX_GDS_HW_CTRL    0x1540u
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_PWR_ON             (1u << 31)
#define RCG_UPDATE              (1u << 0)
#define RCG_ROOT_EN             (1u << 1)
#define RCG_ROOT_OFF            (1u << 31)
#define RCG_GMU_200MHZ          ((6u << 8) | 2u)   /* source 6 = GPLL0_OUT_MAIN_DIV, divider field 2 = 1.5 */

#define GPU_BASE                0x05000000u
#define GMU_BASE                0x0506a000u
#define GMU_SIZE                0x30000u
#define GMU_REG(dword)          ((dword) * 4u - (GMU_BASE - GPU_BASE))
#define GMU_GX_SPTPRAC_POWER_CONTROL 0x1a881u
#define GMU_CM3_ITCM_START      0x1b400u
#define GMU_CM3_DTCM_START      0x1c400u
#define GMU_BOOT_SLUMBER_OPTION 0x1cbf8u
#define GMU_GX_VOTE_IDX         0x1cbf9u
#define GMU_MX_VOTE_IDX         0x1cbfau
#define GMU_CORE_FW_VERSION     0x1d3f8u    /* DTCM_START + 0xff8 */
#define GMU_ICACHE_CONFIG       0x1f400u
#define GMU_DCACHE_CONFIG       0x1f401u
#define GMU_SYS_BUS_CONFIG      0x1f40fu
#define GMU_CM3_SYSRESET        0x1f800u
#define GMU_CM3_BOOT_CONFIG     0x1f801u
#define GMU_CM3_FW_INIT_RESULT  0x1f81cu
#define GMU_CM3_CFG             0x1f82du
#define GMU_ALWAYS_ON_COUNTER_L 0x1f888u
#define GMU_PWR_COL_INTER_FRAME_CTRL 0x1f8c0u
#define GMU_SPTPRAC_PWR_CLK_STATUS 0x1f8d0u
#define GMU_RPMH_CTRL           0x1f8e8u    /* RPMH_INTERFACE 0, LLC 4, DDR 8, MX 9, CX 10, GFX 11 vote enables */
#define GMU_RPMH_POWER_STATE    0x1f8ecu
#define GMU_PWR_COL_CP_MSG      0x1f900u
#define GMU_PWR_COL_CP_RESP     0x1f901u
#define GMU_HFI_CTRL_STATUS     0x1f980u
#define GMU_HFI_SFR_ADDR        0x1f982u
#define GMU_HFI_QTBL_INFO       0x1f984u
#define GMU_HFI_QTBL_ADDR       0x1f985u
#define GMU_HFI_CTRL_INIT       0x1f986u
#define GMU_GMU2HOST_INTR_CLR   0x1f991u
#define GMU_GMU2HOST_INTR_INFO  0x1f992u    /* bit 0 MSGQ */
#define GMU_GMU2HOST_INTR_MASK  0x1f993u
#define GMU_HOST2GMU_INTR_SET   0x1f994u
#define GMU_GENERAL_7           0x1f9ccu
#define GMU_AO_HOST_INTERRUPT_CLR  0x23b04u
#define GMU_AO_HOST_INTERRUPT_MASK 0x23b06u
#define GMU_AHB_FENCE_RANGE_0   0x23b11u
#define GMU_RSCC_OFFSET         0x23000u    /* gmu->rscc = gmu->mmio + 0x23000 before the A650 family; dword offsets from it */
#define GMU_CHIP_ID             0x06015000u /* qcom,adreno-615.0 (0x06010500) in the GMU's layout (a6xx_gmu_fw_start) */
#define GMU_OOB_BOOT_SLUMBER_SET (1u << 22)
#define GMU_OOB_BOOT_SLUMBER_ACK (1u << 30)

#define GPU_PDC_BASE            0x0b280000u /* sdm670.dtsi gmu reg "gmu_pdc" */
#define GPU_PDC_SEQ_BASE        0x0b480000u /* "gmu_pdc_seq" */
#define PDC_GPU_ENABLE_PDC      0x1140u
#define PDC_GPU_SEQ_START_ADDR  0x1148u
#define PDC_GPU_TCS1_CONTROL    0x1572u     /* then CMD_ENABLE_BANK, CMD_WAIT_FOR_CMPL_BANK, CMD0 MSGID/ADDR/DATA, a command every 4 */
#define PDC_GPU_TCS3_CONTROL    0x15d6u

#define CMD_DB_BASE             0x85fe0000u
#define CMD_DB_SIZE             0x20000u
#define RPMH_LEVEL_SVS_L1       192u        /* the GX rail level of the boot vote: 430 MHz, an OPP of every speed bin */

#define GPU_SMMU_BASE           0x05040000u /* SMMUv2 global register space 0: sCR0, then IDR0..IDR7 at 0x20 */
#define SMMU_GMU_BANK           1u
#define SMMU_GMU_GROUP          1u
#define GMU_STREAM              5u

#define GMU_IOVA                0x60000000u /* the dummy page; Linux places the rest from 0x60001000 in this order */
#define GMU_DEBUG_IOVA          0x60001000u
#define GMU_LOG_IOVA            0x60005000u
#define GMU_HFI_IOVA            0x60009000u
#define GMU_IOVA_PAGES          13u
#define DMA_TABLES              0x3000u     /* L1, L2, L3 */
#define DMA_BYTES               0x10000u
#define PTE_TABLE               3ull
/* AttrIndx 0 (MAIR0 0x44: normal non-cacheable), AP[1] unprivileged read/write, outer shareable, AF, nG (io-pgtable-arm). */
#define PTE_PAGE                (3ull | (1ull << 6) | (2ull << 8) | (1ull << 10) | (1ull << 11))

static volatile uint32_t *gcc_page[4], *gpucc, *gmu, *smmu;
static volatile uint32_t *smmu_whole;
static uint32_t smmu_page, smmu_global_pages;
static unsigned gpu_mode;
volatile uint32_t sirius_gpu_state[8];      /* [0] phase reached, [1] gmu fw version, [2] AO ticks over 10 ms, [3] SMMU IDR0,
                                             * [4] FW_INIT_RESULT, [5] HFI_CTRL_STATUS, [6] HFI firmware version */

#define GMU(dword) (gmu[GMU_REG(dword) / 4])
#define RSCC(dword) (gmu[GMU_RSCC_OFFSET / 4 + (dword)])

static void
spin_us(unsigned usec)
{
	volatile unsigned ticks = usec * 100u;
	while (ticks > 0) {
		ticks--;
	}
}

/* GCC registers used here sit in four pages: 0x44000, 0x52000, 0x71000 (the map covers one page each). */
static volatile uint32_t *
gcc_reg(uint32_t offset)
{
	unsigned page = offset >= 0x71000u ? 3 : offset >= 0x52000u ? 2 : 1;
	return gcc_page[page] + (offset & 0xfffu) / 4;
}

static int
branch_on(volatile uint32_t *cbcr, const char *name)
{
	if (!(*cbcr & CBCR_ENABLE)) {
		*cbcr = *cbcr | CBCR_ENABLE;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	for (unsigned i = 0; i < 500 && (*cbcr & CBCR_CLK_OFF); ++i) {
		spin_us(10);
	}
	uint32_t value = *cbcr;
	printf("GPU clk %s cbcr=%08x %s\n", name, value, (value & CBCR_CLK_OFF) ? "OFF" : "on");
	return !(value & CBCR_CLK_OFF);
}

static int
gcc_on(void)
{
	volatile uint32_t *vote = gcc_reg(GCC_APCS_VOTE);
	printf("GPU gcc vote=%08x cfg_ahb=%08x memnoc=%08x snoc_dvm=%08x ddr_axi=%08x\n", *vote, *gcc_reg(GCC_GPU_CFG_AHB_CBCR),
	    *gcc_reg(GCC_GPU_MEMNOC_GFX_CBCR), *gcc_reg(GCC_GPU_SNOC_DVM_CBCR), *gcc_reg(GCC_DDRSS_GPU_AXI_CBCR));
	*vote = *vote | (1u << 15) | (1u << 16);
	__asm__ volatile ("dsb sy" ::: "memory");
	spin_us(50);
	/* The GPU CC's own register interface is behind gcc_gpu_cfg_ahb_clk (CLK_IS_CRITICAL in Linux): nothing in GPU CC is read
	 * until it runs. The other GPU branches feed the CX domain and stay CLK_OFF while it is collapsed (first boot: memnoc_gfx
	 * 0x80004221 after enabling), so they follow the CX GDSC, as the GMU's and the SMMU's clocks do in Linux. */
	return branch_on(gcc_reg(GCC_GPU_CFG_AHB_CBCR), "gpu_cfg_ahb");
}

/* After the CX GDSC: the GMU's axi/memnoc clocks and the GPU SMMU's bus clock (BRANCH_VOTED memnoc_gfx and ddrss_gpu_axi,
 * BRANCH_HALT snoc_dvm_gfx). Returns whether the SMMU's bus clock runs. */
static int
gpu_bus_on(void)
{
	int memnoc = branch_on(gcc_reg(GCC_GPU_MEMNOC_GFX_CBCR), "gpu_memnoc_gfx");
	branch_on(gcc_reg(GCC_GPU_SNOC_DVM_CBCR), "gpu_snoc_dvm_gfx");
	branch_on(gcc_reg(GCC_DDRSS_GPU_AXI_CBCR), "ddrss_gpu_axi");
	return memnoc;
}

#define GPUCC(offset) (gpucc[(offset) / 4])

/* gdsc_init() wait values, then gdsc_toggle_logic(GDSC_ON): clear SW_COLLAPSE, 1 us, poll PWR_ON in gds_hw_ctrl. */
static int
cx_gdsc_on(void)
{
	uint32_t before = GPUCC(GPUCC_CX_GDSCR), status = GPUCC(GPUCC_CX_GDS_HW_CTRL);
	printf("GPU cx gdscr=%08x hw_ctrl=%08x gx gdscr=%08x\n", before, status, GPUCC(GPUCC_GX_GDSCR));
	if (status & GDSC_PWR_ON) {
		return 1;
	}
	uint32_t value = before & ~((0xfu << 20) | (0xfu << 16) | (0xfu << 12));
	value |= (0x2u << 20) | (0x8u << 16) | (0x8u << 12);   /* EN_REST 2, EN_FEW 8, CLK_DIS 8 (gpu_cx_gdsc.clk_dis_wait_val) */
	GPUCC(GPUCC_CX_GDSCR) = value & ~GDSC_SW_COLLAPSE;
	__asm__ volatile ("dsb sy" ::: "memory");
	spin_us(2);
	for (unsigned i = 0; i < 500 && !(GPUCC(GPUCC_CX_GDS_HW_CTRL) & GDSC_PWR_ON); ++i) {
		spin_us(10);
	}
	status = GPUCC(GPUCC_CX_GDS_HW_CTRL);
	printf("GPU cx gdsc on: gdscr=%08x hw_ctrl=%08x %s\n", GPUCC(GPUCC_CX_GDSCR), status, (status & GDSC_PWR_ON) ? "on" : "OFF");
	return (status & GDSC_PWR_ON) != 0;
}

/* clk_rcg2_shared_enable(): force the root on, configure (CFG then UPDATE), release the force. */
static int
gmu_clock_on(void)
{
	volatile uint32_t *cmd = &GPUCC(GPUCC_GMU_CMD_RCGR), *cfg = &GPUCC(GPUCC_GMU_CMD_RCGR + 4);
	printf("GPU gmu rcg cmd=%08x cfg=%08x cx_gmu=%08x cxo=%08x\n", *cmd, *cfg, GPUCC(GPUCC_CX_GMU_CBCR), GPUCC(GPUCC_CXO_CBCR));
	*cmd = *cmd | RCG_ROOT_EN;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (unsigned i = 0; i < 500 && (*cmd & RCG_ROOT_OFF); ++i) {
		spin_us(10);
	}
	*cfg = (*cfg & ~0x371fu) | RCG_GMU_200MHZ;
	*cmd = *cmd | RCG_UPDATE;
	__asm__ volatile ("dsb sy" ::: "memory");
	for (unsigned i = 0; i < 500 && (*cmd & RCG_UPDATE); ++i) {
		spin_us(10);
	}
	uint32_t updated = *cmd;
	int ok = branch_on(&GPUCC(GPUCC_CX_GMU_CBCR), "cx_gmu") && branch_on(&GPUCC(GPUCC_CXO_CBCR), "gpu_cxo");
	*cmd = *cmd & ~RCG_ROOT_EN;
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("GPU gmu rcg now cmd=%08x (after update %08x) cfg=%08x\n", *cmd, updated, *cfg);
	return ok && !(updated & RCG_UPDATE);
}

/* ---------------------------------------------------------------------------------------------------------------------- */
/* Phase 2: the GMU firmware boot.                                                                                          */

static volatile uint32_t *dma;
static uint64_t dma_pa;
static uint32_t hfi_seqnum;

/* Waits up to timeout_us for (*reg & mask) == want, as gmu_poll_timeout(). */
static int
poll32(volatile uint32_t *reg, uint32_t mask, uint32_t want, unsigned timeout_us)
{
	for (unsigned waited = 0;; waited += 10) {
		if ((*reg & mask) == want) {
			return 1;
		}
		if (waited >= timeout_us) {
			return 0;
		}
		IODelay(10);
	}
}

/* Clean and invalidate to the point of coherency: after the CPU writes, before it reads what the GMU or the walker wrote. */
static void
dma_sync(uint32_t offset, uint32_t bytes)
{
	flush_dcache64(dma_pa + offset, bytes, 1);
}

#define DMA_OFFSET(iova) (DMA_TABLES + (iova) - GMU_IOVA)

static volatile uint32_t *
gmu_mem(uint32_t iova)
{
	return dma + DMA_OFFSET(iova) / 4;
}

/* cmd-db (cmd-db.c): the header (version, magic, 8 x struct rsc_hdr of 16 bytes, checksum, reserved) is 0x90 bytes, then the
 * data; a slave's entries (24 bytes: id[8], priority[2], addr, len, offset) start at data + header_offset, an entry's aux data
 * at data + the slave's data_offset + the entry's offset. */
static uint32_t
cmd_db_byte(volatile uint32_t *db, uint32_t offset)
{
	return (db[offset / 4] >> (8 * (offset & 3))) & 0xff;
}

static uint32_t
cmd_db_u16(volatile uint32_t *db, uint32_t offset)
{
	return cmd_db_byte(db, offset) | (cmd_db_byte(db, offset + 1) << 8);
}

static int
cmd_db_entry(volatile uint32_t *db, const char *name, uint32_t *addr, uint32_t *aux, uint32_t *aux_len)
{
	if (db[1] != 0x0c0330dbu) {
		printf("GPU cmd-db magic %08x, not db 30 03 0c\n", db[1]);
		return 0;
	}
	unsigned char query[8] = { 0 };
	for (unsigned c = 0; c < 8 && name[c]; ++c) {
		query[c] = (unsigned char)name[c];
	}
	for (unsigned slave = 0; slave < 8; ++slave) {
		uint32_t header = 8 + 16 * slave;
		if (!cmd_db_u16(db, header)) {
			break;
		}
		uint32_t entries = 0x90 + cmd_db_u16(db, header + 2), data = 0x90 + cmd_db_u16(db, header + 4);
		uint32_t count = cmd_db_u16(db, header + 6);
		for (unsigned i = 0; i < count && entries + 24 * (i + 1) <= CMD_DB_SIZE; ++i) {
			uint32_t entry = entries + 24 * i, match = 1;
			for (unsigned c = 0; c < 8 && match; ++c) {
				match = cmd_db_byte(db, entry + c) == query[c];
			}
			if (match) {
				*addr = db[(entry + 16) / 4];
				*aux_len = cmd_db_u16(db, entry + 20);
				*aux = data + cmd_db_u16(db, entry + 22);
				return *aux + *aux_len <= CMD_DB_SIZE;
			}
		}
	}
	return 0;
}

/* a6xx_gmu_rpmh_arc_votes_init() for one level: the first gfx.lvl index at or above it, and the mx.lvl index that matches
 * (or the highest non-zero one below it). */
static int
gx_rail_vote(uint32_t level, uint32_t *gx, uint32_t *mx)
{
	volatile uint32_t *db = (volatile uint32_t *)ml_io_map(CMD_DB_BASE, CMD_DB_SIZE);
	uint32_t gfx_addr, gfx, gfx_len, mx_addr, mxl, mx_len, cx_addr, cxl, cx_len;
	if (!db || !cmd_db_entry(db, "gfx.lvl", &gfx_addr, &gfx, &gfx_len) || !cmd_db_entry(db, "mx.lvl", &mx_addr, &mxl, &mx_len)) {
		printf("GPU cmd-db: no gfx.lvl/mx.lvl\n");
		return 0;
	}
	if (!cmd_db_entry(db, "cx.lvl", &cx_addr, &cxl, &cx_len)) {
		cx_addr = 0;
	}
	/* The PDC sequence below uses Linux's addresses for the A615 (mx 0x30010, cx 0x30000, gfx 0x30080): logged to compare. */
	printf("GPU cmd-db cx.lvl addr=%05x mx.lvl addr=%05x levels=%u gfx.lvl addr=%05x levels=%u\n", cx_addr, mx_addr, mx_len / 2,
	    gfx_addr, gfx_len / 2);
	char text[96];
	unsigned at = 0;
	for (unsigned j = 0; j < gfx_len / 2 && j < 16 && at < sizeof(text) - 6; ++j) {
		at += snprintf(text + at, sizeof(text) - at, " %u", cmd_db_u16(db, gfx + 2 * j));
	}
	text[at] = 0;
	printf("GPU cmd-db gfx.lvl:%s\n", text);
	unsigned p = gfx_len / 2, s = 0;
	for (unsigned j = 0; j < gfx_len / 2; ++j) {
		if (cmd_db_u16(db, gfx + 2 * j) >= level) {
			p = j;
			break;
		}
	}
	if (p == gfx_len / 2) {
		printf("GPU cmd-db: level %u not in gfx.lvl\n", level);
		return 0;
	}
	for (unsigned j = 0; j < mx_len / 2; ++j) {
		uint32_t value = cmd_db_u16(db, mxl + 2 * j);
		if (value >= level) {
			s = j;
			break;
		} else if (value) {
			s = j;
		}
	}
	*gx = p;
	*mx = s;
	printf("GPU gx rail vote level %u: gfx index %u (%u) mx index %u (%u)\n", level, p, cmd_db_u16(db, gfx + 2 * p), s,
	    cmd_db_u16(db, mxl + 2 * s));
	return 1;
}

/* Stage-1 tables for the GMU's iovas, context bank 1, then the stream mapping for stream 5 (arm_smmu_write_context_bank,
 * arm_smmu_write_s2cr, arm_smmu_write_smr). Returns whether ATS1PR translates the HFI iova to its page. */
static int
smmu_map_gmu(void)
{
	volatile uint64_t *l1 = (volatile uint64_t *)dma, *l2 = l1 + 512, *l3 = l2 + 512;
	l1[GMU_IOVA >> 30] = (dma_pa + 0x1000) | PTE_TABLE;
	l2[(GMU_IOVA >> 21) & 0x1ff] = (dma_pa + 0x2000) | PTE_TABLE;
	for (unsigned i = 0; i < GMU_IOVA_PAGES; ++i) {
		l3[((GMU_IOVA >> 12) & 0x1ff) + i] = (dma_pa + DMA_TABLES + 0x1000ull * i) | PTE_PAGE;
	}
	dma_sync(0, DMA_TABLES);

	volatile uint32_t *gr0 = smmu_whole, *gr1 = smmu_whole + smmu_page / 4;
	volatile uint32_t *cb = smmu_whole + (smmu_global_pages + SMMU_GMU_BANK) * smmu_page / 4;
	cb[0x00 / 4] = 0;                                     /* SCTLR: off while it is set up */
	gr1[(0x800 + 4 * SMMU_GMU_BANK) / 4] = 1;             /* CBA2R: VA64 */
	gr1[(0x000 + 4 * SMMU_GMU_BANK) / 4] = 0x1f300;       /* CBAR: S1 translate, S2 bypass; BPSHCFG NSH, MEMATTR WB */
	cb[0x10 / 4] = (7u << 15) | 1u;                       /* TCR2: SEP upstream, PASIZE 36 bits (IDR2.OAS) */
	cb[0x30 / 4] = (1u << 23) | (2u << 12) | 32u;         /* TCR: EPD1, SH0 outer, IRGN0/ORGN0 non-cacheable, 4 KiB, T0SZ 32 */
	*(volatile uint64_t *)&cb[0x20 / 4] = dma_pa | ((uint64_t)SMMU_GMU_BANK << 48);   /* TTBR0: L1, ASID = bank */
	*(volatile uint64_t *)&cb[0x28 / 4] = 0;               /* TTBR1 */
	cb[0x38 / 4] = 0xf404ff44u;                           /* MAIR0: NC, WBWA, device nGnRE, inner NC outer WB */
	cb[0x3c / 4] = 0;
	cb[0x58 / 4] = cb[0x58 / 4];                          /* FSR: write one to clear */
	__asm__ volatile ("dsb sy" ::: "memory");
	cb[0x618 / 4] = 0;                                    /* TLBIALL, then TLBSYNC */
	cb[0x7f0 / 4] = 0;
	int synced = poll32(&cb[0x7f4 / 4], 1, 0, 10000);
	cb[0x00 / 4] = 0x1027;                                /* SCTLR: M, TRE, AFE, CFRE (abort, no stall), ASIDPNE; no fault IRQ */
	gr0[(0xc00 + 4 * SMMU_GMU_GROUP) / 4] = SMMU_GMU_BANK;              /* S2CR: translate through bank 1 */
	gr0[(0x800 + 4 * SMMU_GMU_GROUP) / 4] = (1u << 31) | GMU_STREAM;   /* SMR: valid, mask 0, stream 5 */
	gr0[0x68 / 4] = 0;                                    /* TLBIALLNSNH, then TLBGSYNC */
	gr0[0x70 / 4] = 0;
	int gsynced = poll32(&gr0[0x74 / 4], 1, 0, 10000);
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("GPU smmu cb%u cbar=%08x cba2r=%08x sctlr=%08x tcr=%08x tcr2=%08x ttbr0=%08x%08x mair0=%08x fsr=%08x sync=%d/%d\n",
	    SMMU_GMU_BANK, gr1[(0x000 + 4 * SMMU_GMU_BANK) / 4], gr1[(0x800 + 4 * SMMU_GMU_BANK) / 4], cb[0], cb[0x30 / 4],
	    cb[0x10 / 4], cb[0x24 / 4], cb[0x20 / 4], cb[0x38 / 4], cb[0x58 / 4], synced, gsynced);
	printf("GPU smmu smr%u=%08x s2cr%u=%08x scr0=%08x\n", SMMU_GMU_GROUP, gr0[(0x800 + 4 * SMMU_GMU_GROUP) / 4], SMMU_GMU_GROUP,
	    gr0[(0xc00 + 4 * SMMU_GMU_GROUP) / 4], gr0[0]);

	/* ATS1PR (arm_smmu_iova_to_phys_hard): IDR0.ATOSNS is clear, so the bank translates on request. */
	*(volatile uint64_t *)&cb[0x800 / 4] = GMU_HFI_IOVA;
	int done = poll32(&cb[0x8f0 / 4], 1, 0, 10000);
	uint64_t par = *(volatile uint64_t *)&cb[0x50 / 4];
	uint64_t want = dma_pa + DMA_OFFSET(GMU_HFI_IOVA);
	int ok = done && !(par & 1) && (par & 0x0000fffffffff000ull) == want;
	printf("GPU smmu ats1pr %08x -> par=%08x%08x want %08x%08x %s (fsr=%08x)\n", GMU_HFI_IOVA, (uint32_t)(par >> 32), (uint32_t)par,
	    (uint32_t)(want >> 32), (uint32_t)want, ok ? "ok" : done ? "MISMATCH" : "TIMEOUT", cb[0x58 / 4]);
	return ok;
}

/* a6xx_hfi_init(): the table header, then the command (id 0), response (4) and debug (5) queues, 4 KiB each after it. */
static void
hfi_init(void)
{
	volatile uint32_t *table = gmu_mem(GMU_HFI_IOVA);
	table[0] = 0;                       /* version */
	table[1] = 24 + 3 * 48;             /* size, bytes as Linux writes it */
	table[2] = 24 / 4;                  /* qhdr0_offset */
	table[3] = 48 / 4;                  /* qhdr_size */
	table[4] = 3;                       /* num_queues */
	table[5] = 3;                       /* active_queues */
	static const uint32_t ids[3] = { 0, 4, 5 };
	for (unsigned q = 0; q < 3; ++q) {
		volatile uint32_t *header = table + 6 + 12 * q;
		header[0] = 1;                                  /* status */
		header[1] = GMU_HFI_IOVA + 0x1000 * (q + 1);    /* iova */
		header[2] = (10u << 8) | ids[q];                /* type */
		header[3] = 0x1000 / 4;                         /* size in dwords */
		header[4] = 0;                                  /* msg_size */
		header[5] = 0;                                  /* dropped */
		header[6] = 1;                                  /* rx_watermark */
		header[7] = 1;                                  /* tx_watermark */
		header[8] = 1;                                  /* rx_request */
		header[9] = 0;                                  /* tx_request */
		header[10] = 0;                                 /* read_index */
		header[11] = 0;                                 /* write_index */
	}
	dma_sync(DMA_OFFSET(GMU_HFI_IOVA), 0x4000);
}

/* a6xx_hfi_send_msg() and a6xx_hfi_wait_for_ack(), polled: returns the response's error field (0 = ok) or -1, with up to
 * `payload_words` of its payload. */
static int
hfi_send(uint32_t id, uint32_t *message, uint32_t words, uint32_t *payload, uint32_t payload_words)
{
	volatile uint32_t *table = gmu_mem(GMU_HFI_IOVA);
	volatile uint32_t *command = table + 6, *response = table + 6 + 12;
	volatile uint32_t *command_data = table + 0x1000 / 4, *response_data = table + 0x2000 / 4;
	uint32_t seqnum = ++hfi_seqnum % 0xfff;
	message[0] = (seqnum << 20) | (0u << 16) | (words << 8) | id;     /* HFI_MSG_CMD */
	dma_sync(DMA_OFFSET(GMU_HFI_IOVA), 0x3000);
	uint32_t index = command[11];
	for (unsigned i = 0; i < words; ++i) {
		command_data[index] = message[i];
		index = (index + 1) % 1024;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	command[11] = index;
	dma_sync(DMA_OFFSET(GMU_HFI_IOVA), 0x3000);
	GMU(GMU_HOST2GMU_INTR_SET) = 1;
	for (unsigned tries = 0; tries < 4; ++tries) {
		if (!poll32(&GMU(GMU_GMU2HOST_INTR_INFO), 1, 1, 1000000)) {
			printf("GPU hfi msg %u seq %u: no reply (info=%08x)\n", id, seqnum, GMU(GMU_GMU2HOST_INTR_INFO));
			return -1;
		}
		GMU(GMU_GMU2HOST_INTR_CLR) = 1;
		dma_sync(DMA_OFFSET(GMU_HFI_IOVA), 0x3000);
		while (response[10] != response[11]) {
			uint32_t read = response[10], reply[19] = { 0 }, header = response_data[read], size = (header >> 8) & 0xff;
			for (unsigned i = 0; i < size; ++i) {
				if (i < 19) {
					reply[i] = response_data[read];
				}
				read = (read + 1) % 1024;
			}
			response[10] = read;
			dma_sync(DMA_OFFSET(GMU_HFI_IOVA), 0x3000);
			if ((header & 0xff) == 100) {                               /* HFI_F2H_MSG_ERROR */
				printf("GPU hfi firmware error %u\n", reply[1]);
				continue;
			}
			if (size < 3 || ((reply[1] >> 20) & 0xfff) != seqnum) {
				printf("GPU hfi unexpected reply header=%08x ret=%08x\n", header, reply[1]);
				continue;
			}
			for (unsigned i = 0; i < payload_words && i < 16; ++i) {
				payload[i] = reply[3 + i];
			}
			printf("GPU hfi msg %u seq %u: reply header=%08x error=%u payload=%08x\n", id, seqnum, header, reply[2], reply[3]);
			return (int)reply[2];
		}
	}
	printf("GPU hfi msg %u seq %u: interrupts without its reply\n", id, seqnum);
	return -1;
}

/* a6xx_gmu_rpmh_init(): the GPU RSC's PDC handshake and sequencer, then the PDC's sequencer and its sleep (TCS1) and wake (TCS3)
 * commands to the mx, cx and gfx ARC resources. */
static int
gmu_rpmh_init(void)
{
	volatile uint32_t *pdc = (volatile uint32_t *)ml_io_map(GPU_PDC_BASE, 0x10000);
	volatile uint32_t *seq = (volatile uint32_t *)ml_io_map(GPU_PDC_SEQ_BASE, 0x10000);
	if (!pdc || !seq) {
		printf("GPU pdc map failed\n");
		return 0;
	}
	RSCC(0x0004) = 1u << 24;            /* RSC_STATUS0_DRV0: disable SDE clock gating */
	RSCC(0x000b) = 1;                   /* PDC_SLAVE_ID_DRV0 */
	RSCC(0x000e) = 0;                   /* HIDDEN_TCS_CMD0 DATA, ADDR; two more commands, 2 apart */
	RSCC(0x000d) = 0;
	RSCC(0x0010) = 0;
	RSCC(0x000f) = 0;
	RSCC(0x0012) = 0x80000000u;
	RSCC(0x0011) = 0;
	RSCC(0x0100) = 0;                   /* OVERRIDE_START_ADDR */
	RSCC(0x0008) = 0x4520;              /* PDC_SEQ_START_ADDR */
	RSCC(0x0009) = 0x4510;              /* PDC_MATCH_VALUE_LO */
	RSCC(0x000a) = 0x4514;              /* PDC_MATCH_VALUE_HI */
	static const uint32_t rscc_seq[5] = { 0xa7a506a0, 0xa1e6a6e7, 0xa2e081e1, 0xe9a982e2, 0x0020e8a8 };
	static const uint32_t pdc_seq[5] = { 0xfebea1e1, 0xa5a4a3a2, 0x8382a6e0, 0xbce3e284, 0x002081fc };
	for (unsigned i = 0; i < 5; ++i) {
		RSCC(0x0180 + i) = rscc_seq[i];     /* SEQ_MEM_0_DRV0 */
		seq[i] = pdc_seq[i];                /* PDC_GPU_SEQ_MEM_0 */
	}
	static const uint32_t addr[3] = { 0x30010, 0x30000, 0x30080 };
	static const uint32_t sleep[3] = { 1, 0, 0 }, wake[3] = { 2, 3, 3 };
	static const uint32_t tcs[2] = { PDC_GPU_TCS1_CONTROL, PDC_GPU_TCS3_CONTROL };
	for (unsigned t = 0; t < 2; ++t) {
		pdc[tcs[t] + 1] = 7;                /* CMD_ENABLE_BANK */
		pdc[tcs[t] + 2] = 0;                /* CMD_WAIT_FOR_CMPL_BANK */
		pdc[tcs[t]] = 0;                    /* CONTROL */
		for (unsigned c = 0; c < 3; ++c) {
			pdc[tcs[t] + 3 + 4 * c] = 0x10108;
			pdc[tcs[t] + 4 + 4 * c] = addr[c];
			pdc[tcs[t] + 5 + 4 * c] = t ? wake[c] : sleep[c];
		}
	}
	pdc[PDC_GPU_SEQ_START_ADDR] = 0;
	pdc[PDC_GPU_ENABLE_PDC] = 0x80000001u;
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("GPU rscc seq0=%08x slave=%08x pdc enable=%08x tcs1 addr2=%05x tcs3 data1=%x seq0=%08x\n", RSCC(0x0180), RSCC(0x000b),
	    pdc[PDC_GPU_ENABLE_PDC], pdc[PDC_GPU_TCS1_CONTROL + 12], pdc[PDC_GPU_TCS3_CONTROL + 9], seq[0]);
	return 1;
}

static void
gmu_report(const char *result)
{
	if (&boot_progress_ext_wide) {
		snprintf(boot_progress_ext_wide[0], sizeof(boot_progress_ext_wide[0]),
		    "GMU phase=%u init=%08x hfi=%x fw=%08x ver=%08x rpmh=%x %s", sirius_gpu_state[0], sirius_gpu_state[4],
		    sirius_gpu_state[5], GMU(GMU_CORE_FW_VERSION), sirius_gpu_state[6], GMU(GMU_RPMH_POWER_STATE), result);
	}
	printf("GPU phase 2: %s (phase=%u)\n", result, sirius_gpu_state[0]);
}

static void
gmu_boot(void)
{
	uint32_t words = sirius_gmu_fw_words;
	printf("GPU gmu firmware %u bytes, first %08x %08x\n", words * 4, sirius_gmu_fw[0], sirius_gmu_fw[1]);
	if (!words || words > 0x8000 / 4) {
		gmu_report("firmware is not 1..32 KiB");
		return;
	}
	dma = (volatile uint32_t *)sirius_gpu_dma_alloc(DMA_BYTES, &dma_pa);
	if (!dma || !dma_pa || (dma_pa & 0xfff)) {
		gmu_report("no DMA memory");
		return;
	}
	for (unsigned i = 0; i < DMA_BYTES / 4; ++i) {
		dma[i] = 0;
	}
	dma_sync(0, DMA_BYTES);
	printf("GPU dma va=%p pa=%08x%08x %u KiB\n", (void *)dma, (uint32_t)(dma_pa >> 32), (uint32_t)dma_pa, DMA_BYTES / 1024);
	if (!smmu_map_gmu()) {
		gmu_report("SMMU context does not translate");
		return;
	}
	sirius_gpu_state[0] = 6;
	hfi_init();
	uint32_t gx = 0, mx = 0;
	if (!gx_rail_vote(RPMH_LEVEL_SVS_L1, &gx, &mx)) {
		gmu_report("no GX rail vote");
		return;
	}

	/* a6xx_gmu_resume(): interrupts stay masked (nothing here takes them), then a cold boot. */
	GMU(GMU_AO_HOST_INTERRUPT_CLR) = ~0u;
	GMU(GMU_AO_HOST_INTERRUPT_MASK) = ~0u;
	GMU(GMU_GMU2HOST_INTR_CLR) = ~0u;
	GMU(GMU_GMU2HOST_INTR_MASK) = ~0u;
	GMU(GMU_GENERAL_7) = 1;                     /* TCM retention */
	if (!gmu_rpmh_init()) {
		gmu_report("no PDC");
		return;
	}
	/* a6xx_gmu_fw_load(), legacy: the image into ITCM through the register window, the CM3 held in reset. */
	GMU(GMU_CM3_SYSRESET) = 1;
	for (unsigned i = 0; i < words; ++i) {
		GMU(GMU_CM3_ITCM_START + i) = sirius_gmu_fw[i];
	}
	unsigned mismatches = 0;
	for (unsigned i = 0; i < words; ++i) {
		mismatches += GMU(GMU_CM3_ITCM_START + i) != sirius_gmu_fw[i];
	}
	printf("GPU gmu itcm %u words written, %u read back different\n", words, mismatches);
	sirius_gpu_state[0] = 7;

	/* a6xx_gmu_fw_start() */
	GMU(GMU_CM3_FW_INIT_RESULT) = 0;
	GMU(GMU_CM3_BOOT_CONFIG) = 2;
	GMU(GMU_HFI_QTBL_ADDR) = GMU_HFI_IOVA;
	GMU(GMU_HFI_QTBL_INFO) = 1;
	GMU(GMU_AHB_FENCE_RANGE_0) = (1u << 31) | (0xau << 18) | 0xa0u;
	GMU(GMU_CM3_CFG) = 0x4052;
	GMU(GMU_HFI_SFR_ADDR) = GMU_CHIP_ID;
	GMU(GMU_PWR_COL_CP_MSG) = GMU_LOG_IOVA | (0x4000 / 0x1000 - 1);
	/* a6xx_gmu_power_config(), idle level ACTIVE: no inter-frame or SPTP power collapse. */
	GMU(GMU_SYS_BUS_CONFIG) = 1;
	GMU(GMU_ICACHE_CONFIG) = 1;
	GMU(GMU_DCACHE_CONFIG) = 1;
	GMU(GMU_PWR_COL_INTER_FRAME_CTRL) = 0x9c40400;
	GMU(GMU_RPMH_CTRL) = GMU(GMU_RPMH_CTRL) | (1u << 0) | (1u << 4) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11);

	/* a6xx_gmu_start(): the reset value it waits for depends on the firmware's version word; take either. */
	uint32_t marker = GMU(GMU_CM3_DTCM_START + 0xff8);
	GMU(GMU_CM3_SYSRESET) = 1;
	GMU(GMU_PWR_COL_CP_RESP) = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	GMU(GMU_CM3_SYSRESET) = 0;
	uint32_t result = 0;
	int started = 0;
	for (unsigned waited = 0; waited <= 100000 && !started; waited += 100) {
		result = GMU(GMU_CM3_FW_INIT_RESULT);
		started = result == 0xbabeface || (result & 0x1ff) == 0x100;
		if (!started) {
			IODelay(100);
		}
	}
	sirius_gpu_state[4] = result;
	printf("GPU gmu start: marker=%08x (expects %s) fw_init=%08x %s fw_version=%08x\n", marker,
	    marker <= 0x20010004 ? "babeface" : "0x100", result, started ? "up" : "TIMEOUT", GMU(GMU_CORE_FW_VERSION));
	if (!started) {
		gmu_report("FW_INIT_RESULT timeout");
		return;
	}
	sirius_gpu_state[0] = 8;

	/* a6xx_gmu_gfx_rail_on(), a6xx_sptprac_enable(): legacy only. */
	GMU(GMU_BOOT_SLUMBER_OPTION) = 0;
	GMU(GMU_GX_VOTE_IDX) = gx;
	GMU(GMU_MX_VOTE_IDX) = mx;
	GMU(GMU_HOST2GMU_INTR_SET) = GMU_OOB_BOOT_SLUMBER_SET;
	int rail = poll32(&GMU(GMU_GMU2HOST_INTR_INFO), GMU_OOB_BOOT_SLUMBER_ACK, GMU_OOB_BOOT_SLUMBER_ACK, 10000);
	uint32_t info = GMU(GMU_GMU2HOST_INTR_INFO);
	GMU(GMU_GMU2HOST_INTR_CLR) = GMU_OOB_BOOT_SLUMBER_ACK;
	GMU(GMU_GX_SPTPRAC_POWER_CONTROL) = 0x778000;
	int sptprac = poll32(&GMU(GMU_SPTPRAC_PWR_CLK_STATUS), 0x38, 0x28, 1000);
	printf("GPU gmu gx rail %s (info=%08x) rpmh_state=%08x gx gdscr=%08x sptprac %s status=%08x\n", rail ? "acked" : "TIMEOUT", info,
	    GMU(GMU_RPMH_POWER_STATE), GPUCC(GPUCC_GX_GDSCR), sptprac ? "on" : "TIMEOUT", GMU(GMU_SPTPRAC_PWR_CLK_STATUS));

	/* a6xx_gmu_hfi_start() */
	GMU(GMU_HFI_CTRL_INIT) = 1;
	int hfi = poll32(&GMU(GMU_HFI_CTRL_STATUS), 1, 1, 100000);
	sirius_gpu_state[5] = GMU(GMU_HFI_CTRL_STATUS);
	printf("GPU gmu hfi ctrl status=%08x %s\n", sirius_gpu_state[5], hfi ? "up" : "TIMEOUT");
	if (!hfi) {
		gmu_report("HFI queues do not start");
		return;
	}
	sirius_gpu_state[0] = 9;

	/* a6xx_hfi_start_v1(): INIT with the debug buffer and a cold boot, then FW_VERSION (supported 1.10). */
	uint32_t init[5] = { 0, 0, GMU_DEBUG_IOVA, 0x4000, 1 };
	uint32_t version[2] = { 0, (1u << 28) | (1u << 19) | (1u << 17) };
	uint32_t reply[16] = { 0 };
	if (hfi_send(0, init, 5, 0, 0) != 0) {
		gmu_report("HFI INIT failed");
		return;
	}
	if (hfi_send(1, version, 2, reply, 1) != 0) {
		gmu_report("HFI FW_VERSION failed");
		return;
	}
	sirius_gpu_state[6] = reply[0];
	sirius_gpu_state[0] = 10;
	dma_sync(DMA_OFFSET(GMU_LOG_IOVA), 0x40);
	volatile uint32_t *log = gmu_mem(GMU_LOG_IOVA);
	printf("GPU gmu log %08x %08x %08x %08x\n", log[0], log[1], log[2], log[3]);
	gmu_report("GMU up, HFI v1 answers");
}

static void
gpu_thread(void *parameter, int wait_result)
{
	(void)parameter;
	(void)wait_result;
	gcc_page[1] = (volatile uint32_t *)ml_io_map(GCC_BASE + 0x44000u, 0x1000);
	gcc_page[2] = (volatile uint32_t *)ml_io_map(GCC_BASE + 0x52000u, 0x1000);
	gcc_page[3] = (volatile uint32_t *)ml_io_map(GCC_BASE + 0x71000u, 0x1000);
	gpucc = (volatile uint32_t *)ml_io_map(GPUCC_BASE, 0x2000);
	if (!gcc_page[1] || !gcc_page[2] || !gcc_page[3] || !gpucc) {
		printf("GPU map failed\n");
		return;
	}
	sirius_gpu_state[0] = 1;
	if (!gcc_on()) {
		printf("GPU gpu_cfg_ahb stays off; GPU CC is not touched\n");
		return;
	}
	sirius_gpu_state[0] = 2;
	if (!cx_gdsc_on()) {
		printf("GPU CX GDSC does not power up; stopping\n");
		return;
	}
	sirius_gpu_state[0] = 3;
	int bus = gpu_bus_on();
	if (!gmu_clock_on()) {
		printf("GPU the GMU clock does not run; the GMU is not read\n");
		return;
	}
	sirius_gpu_state[0] = 4;

	gmu = (volatile uint32_t *)ml_io_map(GMU_BASE, GMU_SIZE);
	smmu = (volatile uint32_t *)ml_io_map(GPU_SMMU_BASE, 0x1000);
	if (!gmu || !smmu) {
		printf("GPU gmu/smmu map failed\n");
		return;
	}
	uint32_t ao0 = GMU(GMU_ALWAYS_ON_COUNTER_L);
	IOSleep(10);
	uint32_t ao1 = GMU(GMU_ALWAYS_ON_COUNTER_L);
	sirius_gpu_state[1] = GMU(GMU_CORE_FW_VERSION);
	sirius_gpu_state[2] = ao1 - ao0;
	printf("GPU gmu sysreset=%08x boot_config=%08x fw_init=%08x fw_version=%08x hfi_ctrl=%08x sptprac=%08x rpmh_state=%08x\n",
	    GMU(GMU_CM3_SYSRESET), GMU(GMU_CM3_BOOT_CONFIG), GMU(GMU_CM3_FW_INIT_RESULT), sirius_gpu_state[1], GMU(GMU_HFI_CTRL_STATUS),
	    GMU(GMU_SPTPRAC_PWR_CLK_STATUS), GMU(GMU_RPMH_POWER_STATE));
	printf("GPU gmu always-on %08x -> %08x: %u ticks in 10 ms (19.2 MHz: about 192000)\n", ao0, ao1, ao1 - ao0);
	/* An unclocked SMMU register read can stall the bus: only with its bus clock running. */
	if (!bus) {
		printf("GPU gpu_memnoc_gfx stays off; the GPU SMMU is not read\n");
		return;
	}
	sirius_gpu_state[3] = smmu[0x20 / 4];
	printf("GPU smmu scr0=%08x idr0=%08x idr1=%08x idr2=%08x idr7=%08x\n", smmu[0], smmu[0x20 / 4], smmu[0x24 / 4], smmu[0x28 / 4],
	    smmu[0x3c / 4]);
	/* Phase 2a, still read-only: how the boot chain left the stream mapping, before the GMU (stream 5) needs a context.
	 * SMMUv2: SMR[n] at 0x800 + 4n and S2CR[n] at 0xc00 + 4n (GR0), CBAR[n] at 0x1000 + 4n and CBA2R[n] at 0x1800 + 4n (GR1);
	 * context bank n at (2 << NUMPAGENDXB) pages + n pages, with SCTLR 0x0, TTBR0 0x20, TCR 0x30, MAIR0 0x38, FSR 0x58. */
	uint32_t idr0 = smmu[0x20 / 4], idr1 = smmu[0x24 / 4];
	uint32_t groups = idr0 & 0xff, banks = idr1 & 0xff;
	smmu_page = (idr1 >> 31) ? 0x10000u : 0x1000u;
	smmu_global_pages = 2u << ((idr1 >> 28) & 7);
	smmu_whole = (volatile uint32_t *)ml_io_map(GPU_SMMU_BASE, 0x10000);
	if (!smmu_whole || (smmu_global_pages + banks) * smmu_page > 0x10000u || groups > 16 || banks > 16) {
		printf("GPU smmu layout groups=%u banks=%u page=%x global_pages=%u not dumped\n", groups, banks, smmu_page,
		    smmu_global_pages);
		return;
	}
	for (uint32_t n = 0; n < groups; ++n) {
		printf("GPU smmu smr%u=%08x s2cr%u=%08x\n", n, smmu_whole[(0x800 + 4 * n) / 4], n, smmu_whole[(0xc00 + 4 * n) / 4]);
	}
	for (uint32_t n = 0; n < banks; ++n) {
		volatile uint32_t *cb = smmu_whole + (smmu_global_pages + n) * smmu_page / 4;
		printf("GPU smmu cb%u cbar=%08x cba2r=%08x sctlr=%08x ttbr0=%08x%08x tcr=%08x mair0=%08x fsr=%08x\n", n,
		    smmu_whole[(0x1000 + 4 * n) / 4], smmu_whole[(0x1800 + 4 * n) / 4], cb[0], cb[0x24 / 4], cb[0x20 / 4], cb[0x30 / 4],
		    cb[0x38 / 4], cb[0x58 / 4]);
	}
	sirius_gpu_state[0] = 5;
	printf("GPU phase 1 done: CX powered, GMU and GPU SMMU readable\n");
	if (gpu_mode < 2) {
		return;
	}
	/* Phase 2 needs a free group and bank 1 (2a: the boot chain leaves all of them empty). */
	if (groups <= SMMU_GMU_GROUP || banks <= SMMU_GMU_BANK || (smmu_whole[(0x800 + 4 * SMMU_GMU_GROUP) / 4] & (1u << 31)) ||
	    smmu_whole[(smmu_global_pages + SMMU_GMU_BANK) * smmu_page / 4]) {
		gmu_report("SMMU group/bank 1 is in use");
		return;
	}
	gmu_boot();
}

/* Called from the late display probe (package/LegacyDisplayProbe.cpp). */
void
sirius_gpu_start(void)
{
	unsigned enabled = 0;
	void *thread = 0;
	PE_parse_boot_argn("sirius-gpu", &enabled, sizeof(enabled));
	if (enabled != 1 && enabled != 2) {
		return;
	}
	gpu_mode = enabled;
	if (kernel_thread_start(gpu_thread, 0, &thread) == 0) {
		thread_deallocate(thread);
	} else {
		printf("GPU thread start failed\n");
	}
}
