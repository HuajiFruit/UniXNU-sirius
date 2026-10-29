// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * Sirius Adreno 616 bring-up, phase 1: the GPU's CX side (no firmware, no GX).
 *
 * Values from 57UU/linux-sirius (feat/sirius-7.2): sdm670.dtsi (gpu 0x05000000, gmu 0x0506a000, gpucc 0x05090000,
 * adreno_smmu 0x05040000 and their clocks), drivers/clk/qcom/gcc-sdm845.c and gpucc-sdm845.c (branches, the GMU RCG, the CX
 * GDSC), drivers/clk/qcom/gdsc.c (the enable sequence) and drivers/gpu/drm/msm/registers/adreno/a6xx_gmu.xml (GMU registers,
 * dword offsets from the GPU base: byte address = 0x05000000 + 4 * offset, gmu_read()).
 *
 * The order follows a6xx_gmu_resume() and the clock framework: GCC sources and branches first (an unprogrammed branch has
 * stalled this SoC), then the CX GDSC (VOTABLE, status in gds_hw_ctrl), then the GMU clock source at 200 MHz (GPLL0 main div
 * / 1.5, rcg2_shared: force the root on around the update) and its cx_gmu and cxo branches. After that it only reads: GMU state
 * registers, the GMU always-on counter twice, and the GPU SMMU's global ID registers. Nothing writes the GMU or either SMMU.
 *
 * sirius-gpu=1 runs it once on a kernel thread from the late display probe; every step is a GPU console line.
 */

typedef unsigned int uint32_t;
typedef unsigned long vm_offset_t;

extern vm_offset_t ml_io_map(vm_offset_t phys, unsigned long size);
extern int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, unsigned int max_len);
extern int printf(const char *format, ...);
extern void IOSleep(unsigned int milliseconds);
extern int kernel_thread_start(void (*continuation)(void *, int), void *parameter, void **new_thread);
extern void thread_deallocate(void *thread);

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
#define GMU_CORE_FW_VERSION     0x1d3f8u
#define GMU_CM3_SYSRESET        0x1f800u
#define GMU_CM3_BOOT_CONFIG     0x1f801u
#define GMU_CM3_FW_INIT_RESULT  0x1f81cu
#define GMU_ALWAYS_ON_COUNTER_L 0x1f888u
#define GMU_SPTPRAC_PWR_CLK_STATUS 0x1f8d0u
#define GMU_RPMH_POWER_STATE    0x1f8ecu
#define GMU_HFI_CTRL_STATUS     0x1f980u

#define GPU_SMMU_BASE           0x05040000u /* SMMUv2 global register space 0: sCR0, then IDR0..IDR7 at 0x20 */

static volatile uint32_t *gcc_page[4], *gpucc, *gmu, *smmu;
volatile uint32_t sirius_gpu_state[8];      /* [0] phase reached, [1] gmu fw version, [2] AO ticks over 10 ms, [3] SMMU IDR0 */

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
	 * until it runs. */
	return branch_on(gcc_reg(GCC_GPU_CFG_AHB_CBCR), "gpu_cfg_ahb") && branch_on(gcc_reg(GCC_GPU_MEMNOC_GFX_CBCR), "gpu_memnoc_gfx") &&
	       branch_on(gcc_reg(GCC_GPU_SNOC_DVM_CBCR), "gpu_snoc_dvm_gfx") && branch_on(gcc_reg(GCC_DDRSS_GPU_AXI_CBCR), "ddrss_gpu_axi");
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
		printf("GPU a GCC branch stays off; GPU CC is not touched\n");
		return;
	}
	sirius_gpu_state[0] = 2;
	if (!cx_gdsc_on()) {
		printf("GPU CX GDSC does not power up; stopping\n");
		return;
	}
	sirius_gpu_state[0] = 3;
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
#define GMU(dword) (gmu[GMU_REG(dword) / 4])
	uint32_t ao0 = GMU(GMU_ALWAYS_ON_COUNTER_L);
	IOSleep(10);
	uint32_t ao1 = GMU(GMU_ALWAYS_ON_COUNTER_L);
	sirius_gpu_state[1] = GMU(GMU_CORE_FW_VERSION);
	sirius_gpu_state[2] = ao1 - ao0;
	printf("GPU gmu sysreset=%08x boot_config=%08x fw_init=%08x fw_version=%08x hfi_ctrl=%08x sptprac=%08x rpmh_state=%08x\n",
	    GMU(GMU_CM3_SYSRESET), GMU(GMU_CM3_BOOT_CONFIG), GMU(GMU_CM3_FW_INIT_RESULT), sirius_gpu_state[1], GMU(GMU_HFI_CTRL_STATUS),
	    GMU(GMU_SPTPRAC_PWR_CLK_STATUS), GMU(GMU_RPMH_POWER_STATE));
	printf("GPU gmu always-on %08x -> %08x: %u ticks in 10 ms (19.2 MHz: about 192000)\n", ao0, ao1, ao1 - ao0);
#undef GMU
	sirius_gpu_state[3] = smmu[0x20 / 4];
	printf("GPU smmu scr0=%08x idr0=%08x idr1=%08x idr2=%08x idr7=%08x\n", smmu[0], smmu[0x20 / 4], smmu[0x24 / 4], smmu[0x28 / 4],
	    smmu[0x3c / 4]);
	sirius_gpu_state[0] = 5;
	printf("GPU phase 1 done: CX powered, GMU and GPU SMMU readable\n");
}

/* Called from the late display probe (package/LegacyDisplayProbe.cpp). */
void
sirius_gpu_start(void)
{
	unsigned enabled = 0;
	void *thread = 0;
	PE_parse_boot_argn("sirius-gpu", &enabled, sizeof(enabled));
	if (enabled != 1) {
		return;
	}
	if (kernel_thread_start(gpu_thread, 0, &thread) == 0) {
		thread_deallocate(thread);
	} else {
		printf("GPU thread start failed\n");
	}
}
