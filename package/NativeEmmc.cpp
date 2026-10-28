/*
 * Native eMMC block driver for the Xiaomi sirius (SDM710 / sdm670 SDCC1).
 *
 * The transport itself lives in sirius_emmc.c, which the boot probe also uses: that file owns the
 * controller bring-up, the 400 kHz card clock, the ADMA2 transfer and the cache maintenance the
 * data engine needs. This file only turns it into an IOService plus an IOBlockStorageDevice so
 * IOStorageFamily publishes IOMedia and the kernel can find the root window.
 *
 * Read-only by construction: the transfer path has no write command and reportWriteProtection()
 * says so.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <sys/param.h>

extern "C" {
/* From sirius_emmc.c. prepare() is idempotent and also fills total_blocks from the GPT. */
int sirius_emmc_prepare(void);
int sirius_emmc_read_block(unsigned long long lba, void *buffer);
/* Up to sirius_emmc_run_blocks blocks in one call: a single multi-block command when it works. */
int sirius_emmc_read_blocks(unsigned long long lba, unsigned count, void *buffer);
/* Writes, only inside the one window opened with sirius_emmc_set_write_window(). */
int sirius_emmc_set_write_window(unsigned long long first, unsigned long long blocks);
int sirius_emmc_write_blocks(unsigned long long lba, unsigned count, const void *buffer);
int sirius_emmc_flush(void);
extern const unsigned sirius_emmc_run_blocks;
extern unsigned long long sirius_emmc_total_blocks;
/* The kernel's managed physical range, which a loader-placed ramdisk has to lie inside. */
extern unsigned long gPhysBase, gPhysSize;
}

#define EMMC_BLOCK_SIZE 512

extern "C" void sirius_usb_log_char(char c);

/* The kernel console stops feeding the USB ring before bsd_init prints anything (see
 * sirius_emmc.c), so IOLog and printf never reach the host from here. The reports of this file
 * write the characters into the ring directly, which is the same path the eMMC probe uses. */
static void
emmc_log(const char *text) __attribute__((noinline));

static void
emmc_log(const char *text)
{
	while (*text != '\0') {
		sirius_usb_log_char(*text++);
	}
}

static void
emmc_log_dec(const char *label, uint64_t value)
{
	char digits[24];
	unsigned used = 0;
	emmc_log(label);
	if (value == 0) {
		sirius_usb_log_char('0');
	} else {
		while (value != 0 && used < sizeof(digits)) {
			digits[used++] = (char)('0' + (value % 10));
			value /= 10;
		}
		while (used != 0) {
			sirius_usb_log_char(digits[--used]);
		}
	}
	sirius_usb_log_char('\n');
}

static void
emmc_log_hex(const char *label, uint64_t value)
{
	static const char digits[] = "0123456789abcdef";
	emmc_log(label);
	for (int shift = 60; shift >= 0; shift -= 4) {
		sirius_usb_log_char(digits[(value >> shift) & 0xf]);
	}
	sirius_usb_log_char('\n');
}

/* Observability: the drivers above this one log through IOLog, which does not reach the USB
 * ring, so the only way to see how far the storage stack got is to report what it asks for.
 * nemmc_reads counts the transfers the stack issues, and nemmc_heartbeats is written by a thread
 * so a stalled kernel is distinguishable from one that simply has nothing to print. */
/* The transport keeps one bounce buffer and one descriptor table for every device (see
 * sirius_emmc.c), so transfers must be serialised across all block devices, not per device:
 * APFS mounts several volumes at once and would otherwise interleave into the same table. */

/* The transport keeps one staging buffer and one descriptor table for every device (see
 * sirius_emmc.c), so a transfer must be single-flight across all block devices: APFS mounts
 * several volumes and interleaves reads. This is a plain atomic flag with a delay, not an
 * IOLock: the read path can run where sleeping is not allowed, and an IOLock there unbalances
 * the preemption count ("Taking non-sleepable RW lock with preemption enabled", lock_rw.c:1400). */
static volatile uint32_t nemmc_transport_busy;

static void
nemmc_transport_lock_acquire(void)
{
	while (__atomic_exchange_n(&nemmc_transport_busy, 1, __ATOMIC_ACQ_REL) != 0) {
		IODelay(20);
	}
}

static void
nemmc_transport_lock_release(void)
{
	__atomic_store_n(&nemmc_transport_busy, 0, __ATOMIC_RELEASE);
}

volatile uint32_t nemmc_reads;
volatile uint32_t nemmc_blocks;
/* What the driver is doing right now: the heartbeat prints it once a transfer has been in flight
 * for more than a beat, which separates "stuck inside a transfer" from "the caller never asked
 * again" without guessing. */
volatile uint64_t nemmc_inflight_lba;
volatile uint32_t nemmc_inflight_count;
volatile uint32_t nemmc_inflight_beat;
static bool nemmc_panic_dumped;

extern "C" char boot_progress_panic_text[2048];
volatile uint32_t nemmc_heartbeats;

/* What a presented frame costs (package/LegacyDisplayProbe.cpp and the GopFramebuffer board fork): copies, their time, a read-only
 * pass over the source and the cache modes of both mappings. The panel shows only some of these and only while it repaints; the
 * console has them on every fourth beat. */
extern "C" volatile uint64_t native_legacy_copy_n, native_legacy_copy_last, native_legacy_copy_max, native_legacy_copy_sum
    __attribute__((weak_import));
extern "C" volatile uint64_t sirius_fb_read[2] __attribute__((weak_import));
extern "C" volatile uint32_t sirius_fb_source_cache, sirius_fb_scanout_cache, sirius_fb_source_wimg __attribute__((weak_import));

/* Post-exec probes (patches/xnu/sirius-post-exec-trace.patch). The init exec returns 0 and then
 * the stack asks for no block again, which cannot be told apart from "the launchd thread never
 * started" without looking at the other side of the exec. The kernel writes these counters from
 * the user-thread bootstrap, the pid 1 syscall path and the user fault path; the heartbeat below
 * prints them, so one boot says whether the image ran, how far, and where it stopped. */
extern "C" {
volatile int sirius_user_probe_armed;      /* set once the init exec succeeded */
volatile int sirius_user_faults;           /* user faults taken since then */
volatile int sirius_kernel_faults;         /* pid 1 faults taken in kernel mode */
/* Faults of the pid 1 thread, recorded in the handler and printed by the heartbeat: a kprintf
 * from inside the fault path can deadlock on the console the faulting thread already holds. */
volatile unsigned long long sirius_fault_log[8][5];
volatile int sirius_fault_log_count;
/* The environment of the last pid 1 fault: map, its pmap, the live TTBR0, the copyio recovery
 * entry, the fault address and its fault code. */
volatile unsigned long long sirius_fault_extra[10];
volatile unsigned long long sirius_fault_total;
volatile int sirius_init_syscalls;         /* syscalls entered by pid 1 */
volatile int sirius_init_syscall_returns;  /* ... and returned from */
volatile int sirius_init_asts;             /* bsd_ast runs for pid 1 */
volatile int sirius_user_thread_starts;    /* returns to user from a freshly made thread */
thread_t sirius_launchd_thread;            /* the thread the init exec starts */
thread_t sirius_init_thread;               /* the pid 1 thread that ran bsd_ast */

/* struct thread is private to the files compiled with MACH_KERNEL_PRIVATE, so the fields this
 * driver wants are read by two small accessors in osfmk/arm64/sleh.c. */
unsigned int sirius_thread_state(thread_t thread);
unsigned long long sirius_thread_wait_event(thread_t thread);
unsigned long long sirius_thread_user_pc(thread_t thread);
unsigned long long sirius_thread_kstack_word(thread_t thread, unsigned int index);

/* Called from thread_bootstrap_return, i.e. once per new user thread, before its first return
 * to user. Two lines mean the exec'd thread started; one means it never did. */
void
sirius_user_thread_probe(void)
{
	if (sirius_user_thread_starts < 8) {
		sirius_user_thread_starts++;
		emmc_log_hex("SIRIUS user thread start tid ", thread_tid(current_thread()));
		emmc_log_hex("SIRIUS user thread start pc ",
		    sirius_thread_user_pc(current_thread()));
	}
}
}

static unsigned int nemmc_stack_dumps;

/* PSTATE.PAN is set on every exception entry while SCTLR_EL1.SPAN is clear, and a set PAN makes
 * every privileged access to a user page fault. The kernel is built with CONFIG_XNU_NATIVE_PAN=y,
 * so it keeps PAN set and copyio.c opens it around each user access itself. Only report it here:
 * clearing it once never helped (the next exception entry sets it again), and with PAN support
 * compiled in a clear PAN at a context switch is a panic. The read is guarded by the feature
 * check: on cores without PAN the instruction is undefined. */
static int
nemmc_pan_implemented(void)
{
	uint64_t mmfr1;

	__asm__ volatile("mrs %0, ID_AA64MMFR1_EL1" : "=r"(mmfr1));
	return ((mmfr1 >> 20) & 0xf) != 0;
}

static void
nemmc_report_pan(void)
{
	uint64_t pan;

	emmc_log_dec("NEMMC pan implemented ", (uint64_t)nemmc_pan_implemented());
	if (!nemmc_pan_implemented()) {
		return;
	}
	/* PSTATE.PAN as a system register (S3_0_C4_C2_3): clang's named form does not accept it. */
	pan = (uint64_t)__builtin_arm_rsr("S3_0_C4_C2_3");
	emmc_log_dec("NEMMC pan ", pan);
}

static void
nemmc_dump_init_stack(void)
{
	thread_t thread = sirius_init_thread;

	nemmc_stack_dumps++;
	emmc_log_hex("SIRIUS init thread ", (uint64_t)(uintptr_t)thread);
	emmc_log_hex("SIRIUS init state/wait ",
	    ((uint64_t)sirius_thread_state(thread) << 40) |
	    (sirius_thread_wait_event(thread) & 0xffffffffffULL));
	/* A kernel stack is 16 KiB; scan 4 KiB of it and keep only the words that point into
	 * kernel text, which is the call chain the thread is sitting in. */
	for (unsigned int i = 0; i < 512; i++) {
		uint64_t word = sirius_thread_kstack_word(thread, i);

		if (word >= 0xfffffff007004000ULL && word < 0xfffffff010000000ULL) {
			emmc_log_hex("SIRIUS pc ", word);
		}
	}
}

/* The clock this core runs at: nothing in the port sets a CPU frequency, so the cores run at whatever the boot chain left. Count the
 * architectural PMU's cycle counter over 10 ms of the system counter with interrupts off (the thread cannot move cores meanwhile),
 * then put PMCR_EL0 and the counter enable back as they were. XNU does not use the architectural PMU on this board. */
static void
nemmc_log_cpu_clock(void)
{
	uint64_t mpidr, pmcr, enabled, frequency, cycles0, cycles1, ticks0, ticks1;
	boolean_t interrupts = ml_set_interrupts_enabled(FALSE);

	__asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
	__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(frequency));
	__asm__ volatile ("mrs %0, pmcr_el0" : "=r"(pmcr));
	__asm__ volatile ("mrs %0, pmcntenset_el0" : "=r"(enabled));
	/* E (bit 0) on, D (bit 3, count every 64th cycle) off; enable the cycle counter (bit 31). */
	__asm__ volatile ("msr pmcr_el0, %0" :: "r"((pmcr | 1ULL) & ~(1ULL << 3)));
	__asm__ volatile ("msr pmcntenset_el0, %0" :: "r"(1ULL << 31));
	__asm__ volatile ("isb");
	if (frequency == 0) {
		frequency = 19200000;
	}
	__asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r"(ticks0));
	__asm__ volatile ("mrs %0, pmccntr_el0" : "=r"(cycles0));
	do {
		__asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r"(ticks1));
	} while (ticks1 - ticks0 < frequency / 100);
	__asm__ volatile ("mrs %0, pmccntr_el0" : "=r"(cycles1));
	if (!(enabled & (1ULL << 31))) {
		__asm__ volatile ("msr pmcntenclr_el0, %0" :: "r"(1ULL << 31));
	}
	__asm__ volatile ("msr pmcr_el0, %0" :: "r"(pmcr));
	__asm__ volatile ("isb");
	ml_set_interrupts_enabled(interrupts);

	emmc_log_hex("NEMMC cpu mpidr ", mpidr & 0xffffff);
	emmc_log_dec("NEMMC cpu MHz ", (cycles1 - cycles0) * frequency / (ticks1 - ticks0) / 1000000);
}

/* The memory XNU got (DarwinBoot's window) and the free runs DarwinBoot saw before it placed it (db-runs on the command line,
 * drivers/DarwinBoot): the real layout that bounds owned_memory_size. */
extern "C" unsigned long gPhysBase, gPhysSize;
extern "C" int sirius_cpufreq_report(unsigned domain, uint32_t state[6], uint32_t *vote_mhz) __attribute__((weak_import));

static void
nemmc_log_memory(void)
{
	char runs[320];

	/* package/sirius_cpufreq.c: per cluster the enable bit, table rows, index before and requested (ff = none), the kHz of row 0
	 * and of the highest non-boost row, then the MHz the hardware currently votes. */
	for (unsigned domain = 0; &sirius_cpufreq_report && domain < 2; domain++) {
		uint32_t state[6], vote = 0;

		if (sirius_cpufreq_report(domain, state, &vote)) {
			emmc_log_dec("NEMMC cpufreq domain ", domain);
			emmc_log_hex("NEMMC cpufreq enable/rows/before/request ",
			    ((uint64_t)state[0] << 48) | ((uint64_t)state[1] << 32) | ((uint64_t)state[2] << 16) | state[3]);
			emmc_log_dec("NEMMC cpufreq row0 kHz ", state[4]);
			emmc_log_dec("NEMMC cpufreq best kHz ", state[5]);
			emmc_log_dec("NEMMC cpufreq vote MHz ", vote);
		}
	}
	emmc_log_hex("NEMMC window base ", gPhysBase);
	emmc_log_hex("NEMMC window size ", gPhysSize);
	if (PE_parse_boot_arg_str("db-runs", runs, sizeof(runs))) {
		emmc_log("NEMMC db-runs ");
		emmc_log(runs);
		emmc_log("\n");
	} else {
		emmc_log("NEMMC db-runs none\n");
	}
}

static void
nemmc_heartbeat_thread(void *, wait_result_t)
{
	for (;;) {
		/* The panic screen paints to the framebuffer and the banner then repaints over it, so a
		 * panic message is gone before anyone can read it. The screen code already copies the
		 * formatted message (with its return addresses) into this buffer, so mirror it into the
		 * USB ring the moment it appears: the ring survives, the screen does not. */
		if (!nemmc_panic_dumped && boot_progress_panic_text[0] != 0) {
			nemmc_panic_dumped = true;
			emmc_log("NEMMC PANIC TEXT BEGIN\n");
			for (unsigned i = 0; i < sizeof(boot_progress_panic_text) &&
			    boot_progress_panic_text[i] != 0; i++) {
				sirius_usb_log_char(boot_progress_panic_text[i]);
			}
			emmc_log("\nNEMMC PANIC TEXT END\n");
		}
		emmc_log_hex("NEMMC alive beats/reads ",
		    (((uint64_t)nemmc_heartbeats) << 32) | nemmc_reads);
		emmc_log_hex("NEMMC alive blocks ", nemmc_blocks);
		emmc_log_dec("NEMMC probe user thread starts ", sirius_user_thread_starts);
		emmc_log_dec("NEMMC probe user faults ", sirius_user_faults);
		emmc_log_dec("NEMMC probe init syscalls ", sirius_init_syscalls);
		emmc_log_dec("NEMMC probe init returns ", sirius_init_syscall_returns);
		emmc_log_dec("NEMMC probe init asts ", sirius_init_asts);
		emmc_log_dec("NEMMC probe init faults ", sirius_fault_total);
		if (&native_legacy_copy_n && &sirius_fb_read && (nemmc_heartbeats % 4) == 0) {
			uint64_t frequency = 0, copies = native_legacy_copy_n;

			__asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(frequency));
			if (frequency == 0) {
				frequency = 19200000;
			}
			emmc_log_dec("NEMMC fb copies ", copies);
			emmc_log_dec("NEMMC fb copy last us ", native_legacy_copy_last * 1000000 / frequency);
			emmc_log_dec("NEMMC fb copy mean us ", copies ? native_legacy_copy_sum / copies * 1000000 / frequency : 0);
			emmc_log_dec("NEMMC fb copy max us ", native_legacy_copy_max * 1000000 / frequency);
			emmc_log_dec("NEMMC fb read last us ", sirius_fb_read[0] * 1000000 / frequency);
			emmc_log_dec("NEMMC fb reads timed ", sirius_fb_read[1]);
			emmc_log_hex("NEMMC fb cache source/scanout/wimg ",
			    ((uint64_t)sirius_fb_source_cache << 40) | ((uint64_t)sirius_fb_scanout_cache << 20) | sirius_fb_source_wimg);
		}
		if ((nemmc_heartbeats % 16) == 0) {
			nemmc_log_cpu_clock();
			nemmc_log_memory();
		}
		{
			static int reported;

			if (sirius_fault_extra[0] != 0 && (nemmc_heartbeats % 8) == 0) {
				emmc_log_hex("SIRIUS fault map ", sirius_fault_extra[0]);
				emmc_log_hex("SIRIUS fault pmap ", sirius_fault_extra[1]);
				emmc_log_hex("SIRIUS fault ttbr0 ", sirius_fault_extra[2]);
				emmc_log_hex("SIRIUS fault recover ", sirius_fault_extra[3]);
				emmc_log_hex("SIRIUS fault addr ", sirius_fault_extra[4]);
				emmc_log_hex("SIRIUS fault code ", sirius_fault_extra[5]);
				emmc_log_hex("SIRIUS fault result ", sirius_fault_extra[6]);
				emmc_log_hex("SIRIUS fault pte ", sirius_fault_extra[7]);
				emmc_log_hex("SIRIUS fault par ", sirius_fault_extra[8]);
				emmc_log_hex("SIRIUS fault kernpte ", sirius_fault_extra[9]);
			}
			while (reported < sirius_fault_log_count && reported < 8) {
				emmc_log_hex("SIRIUS fault class ", sirius_fault_log[reported][0]);
				emmc_log_hex("SIRIUS fault esr ", sirius_fault_log[reported][1]);
				emmc_log_hex("SIRIUS fault far ", sirius_fault_log[reported][2]);
				emmc_log_hex("SIRIUS fault pc ", sirius_fault_log[reported][3]);
				emmc_log_dec("SIRIUS fault user ", sirius_fault_log[reported][4]);
				reported++;
			}
		}
		if (sirius_launchd_thread != nullptr) {
			emmc_log_hex("NEMMC launchd thread ", (uint64_t)(uintptr_t)sirius_launchd_thread);
			emmc_log_hex("NEMMC launchd state/wait ",
			    ((uint64_t)sirius_thread_state(sirius_launchd_thread) << 40) |
			    (sirius_thread_wait_event(sirius_launchd_thread) & 0xffffffffffULL));
		}
		/* The boot stops inside load_init_program_at_path: report where the pid 1 thread sits,
		 * a few times, by dumping the top of its kernel stack. The values are kernel text
		 * addresses, so the call chain can be symbolized against kernel.macho afterwards. */
		if (sirius_init_thread != nullptr && nemmc_stack_dumps < 3 &&
		    nemmc_heartbeats >= 20 + (uint32_t)nemmc_stack_dumps * 5) {
			nemmc_dump_init_stack();
		}
		uint64_t inflight = nemmc_inflight_lba;
		if (inflight != 0 && nemmc_heartbeats - nemmc_inflight_beat > 1) {
			emmc_log_hex("NEMMC inflight lba ", inflight);
			emmc_log_dec("NEMMC inflight count ", nemmc_inflight_count);
			emmc_log_dec("NEMMC inflight beats ", nemmc_heartbeats - nemmc_inflight_beat);
		}
		nemmc_heartbeats++;
		IOSleep(2000);
	}
	thread_terminate(current_thread());
}

static void
nemmc_report_media(void)
{
	OSDictionary *matching = IOService::serviceMatching("IOMedia");
	if (matching == nullptr) {
		emmc_log("NEMMC media: no matching dictionary\n");
		return;
	}
	OSIterator *iterator = IOService::getMatchingServices(matching);
	uint64_t count = 0;
	if (iterator != nullptr) {
		while (iterator->getNextObject() != nullptr) {
			count++;
		}
		iterator->release();
	}
	emmc_log_hex("NEMMC IOMedia count ", count);
	matching->release();
}

/* What the storage stack produced: every IOMedia with its UUID, size and BSD name. The layers
 * that create them log through IOLog, which does not reach the ring, so this is the only way to
 * see whether APFS published the OS volume of the image set. */
static void
nemmc_report_media_table(const char *stage)
{
	emmc_log(stage);
	OSDictionary *matching = IOService::serviceMatching("IOMedia");
	if (matching == nullptr) {
		return;
	}
	OSIterator *iterator = IOService::getMatchingServices(matching);
	unsigned index = 0;
	while (iterator != nullptr && index < 8) {
		IOService *media = OSDynamicCast(IOService, iterator->getNextObject());
		if (media == nullptr) {
			break;
		}
		OSString *uuid = OSDynamicCast(OSString, media->getProperty("UUID"));
		OSString *bsd = OSDynamicCast(OSString, media->getProperty("BSD Name"));
		OSNumber *size = OSDynamicCast(OSNumber, media->getProperty("Size"));
		emmc_log("NEMMC media uuid=");
		const char *text = uuid ? uuid->getCStringNoCopy() : "-";
		for (unsigned i = 0; text[i] != '\0' && i < 40; i++) {
			sirius_usb_log_char(text[i]);
		}
		emmc_log(" bsd=");
		text = bsd ? bsd->getCStringNoCopy() : "-";
		for (unsigned i = 0; text[i] != '\0' && i < 12; i++) {
			sirius_usb_log_char(text[i]);
		}
		sirius_usb_log_char('\n');
		emmc_log_dec("NEMMC media bytes ", size ? size->unsigned64BitValue() : 0);
		index++;
	}
	if (iterator != nullptr) {
		iterator->release();
	}
	matching->release();
	emmc_log_dec("NEMMC media count ", index);
}

/* The completion is fired from a thread call, never inside doAsyncReadWrite. Running it in the
 * caller's context re-enters IOStorageFamily while that stack still holds its own locks, which
 * ends in "Taking non-sleepable RW lock with preemption enabled" (lock_rw.c:1400). The interface
 * is asynchronous for exactly this reason, so the transfer and its completion both happen on the
 * thread call and the caller returns at once. */
struct NemmcRequest {
	IOMemoryDescriptor *buffer;
	UInt64 base;
	UInt64 block;
	UInt64 count;
	bool write;
	IOStorageCompletion completion;
	NemmcRequest *next;
};

/* Requests live on this queue and one worker thread drains it. Handing a request straight to a
 * thread call lost it whenever a call was already pending (a thread_call has a single slot and
 * thread_call_enter1 overwrites its parameter), and running the drain in interrupt context blocked
 * the queue outright once anything in it slept. */
static const unsigned NemmcRequestPoolSize = 32;
static NemmcRequest nemmc_pool[NemmcRequestPoolSize];
static NemmcRequest *nemmc_free_list;
static NemmcRequest *nemmc_pending_head;
static NemmcRequest *nemmc_pending_tail;
static volatile uint32_t nemmc_queue_busy;
static uint8_t *nemmc_bounce;

/* A dedicated kernel thread drains the queue. The thread call this used to run on is dispatched
 * in interrupt context: the completion, IOMemoryDescriptor::writeBytes() and the transfer itself
 * may block there, and a blocked interrupt thread simply stops servicing the queue - which looked
 * exactly like a hang (requests and block count frozen, no error, heartbeats still running). */
static thread_t nemmc_worker;

static void
nemmc_queue_lock(void)
{
	while (__atomic_exchange_n(&nemmc_queue_busy, 1, __ATOMIC_ACQ_REL) != 0) {
		IODelay(20);
	}
}

static void
nemmc_queue_unlock(void)
{
	__atomic_store_n(&nemmc_queue_busy, 0, __ATOMIC_RELEASE);
}

static void
nemmc_drain_queue(void)
{
	for (;;) {
		nemmc_queue_lock();
		NemmcRequest *request = nemmc_pending_head;
		if (request != nullptr) {
			nemmc_pending_head = request->next;
			if (nemmc_pending_head == nullptr) {
				nemmc_pending_tail = nullptr;
			}
		}
		nemmc_queue_unlock();
		if (request == nullptr) {
			return;
		}
		IOReturn result = kIOReturnSuccess;
		/* In runs of up to sirius_emmc_run_blocks: each run is one multi-block command, where a
		 * block at a time was one command per 512 bytes. */
		for (UInt64 done = 0; done < request->count;) {
			UInt64 run = request->count - done;
			if (run > sirius_emmc_run_blocks) {
				run = sirius_emmc_run_blocks;
			}
			UInt64 lba = request->base + request->block + done;
			/* The bounce buffer is shared by every request, and requests are drained on their
			 * callers' threads, so it stays under the transport lock until the copy into the
			 * request's memory is done. Released between the read and the copy, a second reader
			 * refilled it first and the first request received the other one's blocks: APFS
			 * reported data hash mismatches on the sealed SystemOS volume once userland read in
			 * parallel. The request's memory is prepared (wired) for the I/O, so the copy does not
			 * fault back into this driver. */
			nemmc_transport_lock_acquire();
			nemmc_inflight_lba = lba;
			nemmc_inflight_count = (uint32_t)request->count;
			nemmc_inflight_beat = nemmc_heartbeats;
			int read;
			UInt64 copied;
			if (request->write) {
				/* The other direction, same rule: the bounce stays under the lock from the
				 * copy out of the request to the end of the write. */
				copied = request->buffer->readBytes(done * EMMC_BLOCK_SIZE, nemmc_bounce,
				    run * EMMC_BLOCK_SIZE);
				read = copied == run * EMMC_BLOCK_SIZE ?
				    sirius_emmc_write_blocks(lba, (unsigned)run, nemmc_bounce) : -1;
			} else {
				read = sirius_emmc_read_blocks(lba, (unsigned)run, nemmc_bounce);
				copied = read == 0 ? request->buffer->writeBytes(done * EMMC_BLOCK_SIZE, nemmc_bounce,
				    run * EMMC_BLOCK_SIZE) : 0;
			}
			nemmc_transport_lock_release();
			nemmc_blocks += (uint32_t)run;
			if (read != 0) {
				emmc_log_hex(request->write ? "NEMMC write failed lba " : "NEMMC transfer failed lba ", lba);
				result = kIOReturnIOError;
				break;
			}
			if (copied != run * EMMC_BLOCK_SIZE) {
				result = kIOReturnIOError;
				break;
			}
			done += run;
		}
		nemmc_inflight_lba = 0;
		nemmc_inflight_count = 0;
		request->completion.action(request->completion.target, request->completion.parameter,
		    result, result == kIOReturnSuccess ? request->count * EMMC_BLOCK_SIZE : 0);
		nemmc_queue_lock();
		request->next = nemmc_free_list;
		nemmc_free_list = request;
		nemmc_queue_unlock();
	}
}

static void
nemmc_worker_thread(void *, wait_result_t)
{
	for (;;) {
		nemmc_queue_lock();
		bool empty = nemmc_pending_head == nullptr;
		nemmc_queue_unlock();
		if (empty) {
			assert_wait((event_t)&nemmc_worker, THREAD_UNINT);
			nemmc_queue_lock();
			empty = nemmc_pending_head == nullptr;
			nemmc_queue_unlock();
			if (empty) {
				thread_block(THREAD_CONTINUE_NULL);
			} else {
				clear_wait(current_thread(), THREAD_AWAKENED);
			}
			continue;
		}
		nemmc_drain_queue();
	}
}

class NativeEmmcBlock : public IOBlockStorageDevice
{
	OSDeclareDefaultStructors(NativeEmmcBlock);

	UInt64 fBase;
	UInt64 fBlocks;
	IOLock *fLock;
	uint8_t *fBounce;
	/* Non-null for the Data window: the loader-placed seed in RAM, served read-write. */
	uint8_t *fRam;
	/* The persistent Data window: eMMC blocks served read-write (the transport fences the writes). */
	bool fWritable;

	IOReturn ramReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
	    IOStorageCompletion *completion);

public:
	bool init(UInt64 base, UInt64 blocks);
	bool start(IOService *provider) override;
	void free() override;
	void useRam(uint8_t *ram);
	void useWritable();
	uint8_t *ram() const { return fRam; }
	bool isData() const { return fRam != nullptr || fWritable; }
	UInt64 blocks() const { return fBlocks; }

	IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
	IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
	UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
	char *getVendorString() override { return const_cast<char *>("Xiaomi"); }
	char *getProductString() override
	{
		return const_cast<char *>(fRam ? "sirius Data seed" : fWritable ? "sirius eMMC Data" : "sirius eMMC");
	}
	char *getRevisionString() override { return const_cast<char *>("1.0"); }
	char *getAdditionalDeviceInfoString() override
	{
		return const_cast<char *>(fRam ? "RAM, read-write, lost at power off" :
		    fWritable ? "SDCC1 eMMC, read-write Data window, ADMA2" : "SDCC1 eMMC, read-only, ADMA2");
	}
	IOReturn reportBlockSize(UInt64 *value) override
	{
		*value = EMMC_BLOCK_SIZE;
		return kIOReturnSuccess;
	}
	IOReturn reportEjectability(bool *value) override
	{
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn reportMaxValidBlock(UInt64 *value) override
	{
		*value = fBlocks ? fBlocks - 1 : 0;
		return kIOReturnSuccess;
	}
	IOReturn reportMediaState(bool *present, bool *changed) override
	{
		*present = fBlocks != 0;
		if (changed) {
			*changed = false;
		}
		return kIOReturnSuccess;
	}
	IOReturn reportRemovability(bool *value) override
	{
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn reportWriteProtection(bool *value) override
	{
		*value = !isData();
		return kIOReturnSuccess;
	}
	IOReturn doSynchronizeCache() override;
	IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
	IOReturn getWriteCacheState(bool *value) override
	{
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }

	IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
	    IOStorageAttributes *attributes, IOStorageCompletion *completion) override;
};

OSDefineMetaClassAndStructors(NativeEmmcBlock, IOBlockStorageDevice);

bool
NativeEmmcBlock::init(UInt64 base, UInt64 blocks)
{
	emmc_log("NativeEmmc: step 7b block base init\n");
	if (!IOBlockStorageDevice::init(nullptr)) {
		return false;
	}
	emmc_log("NativeEmmc: step 7c block base init done\n");
	/* The cap once kept a request from occupying the drain for minutes, when every block was its
	 * own CMD17 at the identification clock. The transfer now reads 256-block runs at the fast
	 * clock, so the cap only has to be a whole number of runs: 1 MiB, four runs, the chunk the
	 * kernel's signature hash cache reads the dyld shared cache in. */
	setProperty("IOMaximumBlockCountRead", 2048, 64);
	fBase = base;
	fBlocks = blocks;
	fRam = nullptr;
	fWritable = false;
	fLock = IOLockAlloc();
	fBounce = (uint8_t *)IOMalloc(EMMC_BLOCK_SIZE);
	if (!fLock || !fBounce) {
		return false;
	}
	return true;
}

bool
NativeEmmcBlock::start(IOService *provider)
{
	emmc_log("NativeEmmc: step 9a block base start\n");
	if (!fLock || !fBounce || !IOBlockStorageDevice::start(provider)) {
		return false;
	}
	emmc_log("NativeEmmc: step 9b block register\n");
	registerService();
	emmc_log("NativeEmmc: step 9c block registered\n");
	return true;
}

void
NativeEmmcBlock::useRam(uint8_t *ram)
{
	fRam = ram;
	setProperty("IOMaximumBlockCountWrite", 2048, 64);
}

void
NativeEmmcBlock::useWritable()
{
	fWritable = true;
	setProperty("IOMaximumBlockCountWrite", 2048, 64);
}

/* APFS asks for this at its checkpoints; with the card's cache on, that is when written blocks
 * have to reach the flash. */
IOReturn
NativeEmmcBlock::doSynchronizeCache()
{
	if (!fWritable) {
		return kIOReturnSuccess;
	}
	nemmc_transport_lock_acquire();
	int flushed = sirius_emmc_flush();
	nemmc_transport_lock_release();
	return flushed == 0 ? kIOReturnSuccess : kIOReturnIOError;
}

/* The Data window: copies between the request's memory and the seed in RAM, completed inline the
 * way the eMMC requests are (on the caller's work loop). */
IOReturn
NativeEmmcBlock::ramReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
    IOStorageCompletion *completion)
{
	UInt64 bytes = count * EMMC_BLOCK_SIZE;
	uint8_t *at = fRam + block * EMMC_BLOCK_SIZE;
	IOLockLock(fLock);
	UInt64 done = buffer->getDirection() == kIODirectionOut ? buffer->readBytes(0, at, bytes)
	    : buffer->writeBytes(0, at, bytes);
	IOLockUnlock(fLock);
	IOReturn result = done == bytes ? kIOReturnSuccess : kIOReturnIOError;
	completion->action(completion->target, completion->parameter, result, done);
	return kIOReturnSuccess;
}

void
NativeEmmcBlock::free()
{
	if (fBounce) {
		IOFree(fBounce, EMMC_BLOCK_SIZE);
		fBounce = nullptr;
	}
	fLock = nullptr;
	IOBlockStorageDevice::free();
}

IOReturn
NativeEmmcBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
    IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
	(void)attributes;
	if (!buffer || !completion || !completion->action || count == 0) {
		emmc_log("NEMMC refuse: no buffer or completion\n");
		return kIOReturnBadArgument;
	}
	if (block >= fBlocks || count > fBlocks - block) {
		emmc_log_hex("NEMMC refuse range block ", block);
		emmc_log_dec("NEMMC refuse range count ", count);
		emmc_log_dec("NEMMC refuse range blocks ", fBlocks);
		return kIOReturnBadArgument;
	}
	if (fRam != nullptr) {
		if ((buffer->getDirection() != kIODirectionIn && buffer->getDirection() != kIODirectionOut) ||
		    buffer->getLength() < count * EMMC_BLOCK_SIZE) {
			return kIOReturnBadArgument;
		}
		return ramReadWrite(buffer, block, count, completion);
	}
	bool write = buffer->getDirection() == kIODirectionOut;
	if (buffer->getDirection() != kIODirectionIn && !(write && fWritable)) {
		emmc_log_dec("NEMMC refuse direction ", (uint64_t)buffer->getDirection());
		return kIOReturnNotWritable;
	}
	if (buffer->getLength() < count * EMMC_BLOCK_SIZE) {
		emmc_log_dec("NEMMC refuse length ", (uint64_t)buffer->getLength());
		emmc_log_dec("NEMMC refuse length need ", count * EMMC_BLOCK_SIZE);
		return kIOReturnBadArgument;
	}

	uint32_t call = nemmc_reads++;
	if (call < 8 || (call & 0xffu) == 0) {
		emmc_log_hex("NEMMC read block ", block);
		emmc_log_hex("NEMMC read count ", count);
	}
	nemmc_queue_lock();
	NemmcRequest *request = nemmc_free_list;
	if (request != nullptr) {
		nemmc_free_list = request->next;
	}
	nemmc_queue_unlock();
	if (request == nullptr) {
		emmc_log("NEMMC refuse: request pool empty\n");
		return kIOReturnNoResources;
	}
	request->buffer = buffer;
	request->base = fBase;
	request->block = block;
	request->count = count;
	request->write = write;
	request->completion = *completion;
	request->next = nullptr;

	nemmc_queue_lock();
	if (nemmc_pending_tail != nullptr) {
		nemmc_pending_tail->next = request;
	} else {
		nemmc_pending_head = request;
	}
	nemmc_pending_tail = request;
	nemmc_queue_unlock();

	/* The completion must run on the caller's work loop: completing from another thread re-enters
	 * IOStorageFamily while this request's caller still holds the loop, and the queue stops there
	 * (block count frozen, no error, worker asleep). Inline completion is the default; the worker
	 * stays behind sirius_emmc_async=1 for comparison. */
	unsigned async = 0;
	PE_parse_boot_argn("sirius_emmc_async", &async, sizeof(async));
	if (async != 0) {
		thread_wakeup((event_t)&nemmc_worker);
		return kIOReturnSuccess;
	}
	nemmc_drain_queue();
	return kIOReturnSuccess;
}

class NativeEmmc : public IOService
{
	OSDeclareDefaultStructors(NativeEmmc);

	NativeEmmcBlock *fBlock;
	NativeEmmcBlock *fWindow[4];
	char fWindowName[4][33];
	unsigned fWindows;
	UInt64 fImageBytes;     /* the root image's size from the SHNGROOT manifest */
	int prepareWindows(UInt64 blocks);
	bool prepareDataWindow(UInt64 blocks);
	void prepareDataSeed();

public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	unsigned windowCount() const { return fWindows; }
	bool windowIs(unsigned index, const char *name) const;
	void publishWindow(unsigned index);
};

OSDefineMetaClassAndStructors(NativeEmmc, IOService);

/* The userdata partition starts with the SHNGROOT image-set manifest (scripts/unixnu/userdata.py):
 * "SHNGROOT", u32 version, u32 count, u64 image_bytes, then 128-byte entries
 * {char name[32]; u64 offset; u64 size; u32 block_size; u32 flags; u8 sha256[32]; u8 reserved[40]}
 * where offset is relative to the partition. The windows hold the unchanged OS and SystemOS APFS
 * images of the IPSW, so publishing each as its own read-only block device is what lets the
 * original APFS mount the system volume; this driver never interprets the images. */
#define USERDATA_FIRST_LBA 17301504ull   /* board.toml [userdata] first_lba */
#define USERDATA_LAST_LBA  122142686ull  /* board.toml [userdata] last_lba */
#define MANIFEST_BYTES     4096u

int
NativeEmmc::prepareWindows(UInt64 blocks)
{
	static uint8_t manifest[MANIFEST_BYTES];
	fWindows = 0;
	if (blocks <= USERDATA_FIRST_LBA) {
		emmc_log("NEMMC manifest: no userdata window\n");
		return 0;
	}
	/* These reads share the single staging buffer with every block device, so they take the
	 * transport lock like the request path does. */
	for (unsigned block = 0; block < MANIFEST_BYTES / EMMC_BLOCK_SIZE; block++) {
		nemmc_transport_lock_acquire();
		int read = sirius_emmc_read_block(USERDATA_FIRST_LBA + block,
		    manifest + block * EMMC_BLOCK_SIZE);
		nemmc_transport_lock_release();
		if (read != 0) {
			emmc_log("NEMMC manifest read failed\n");
			return 0;
		}
	}
	const char magic[8] = { 'S', 'H', 'N', 'G', 'R', 'O', 'O', 'T' };
	for (unsigned i = 0; i < sizeof(magic); i++) {
		if (manifest[i] != (uint8_t)magic[i]) {
			emmc_log("NEMMC manifest: no SHNGROOT\n");
			return 0;
		}
	}
	uint32_t version = (uint32_t)manifest[8] | ((uint32_t)manifest[9] << 8) |
	    ((uint32_t)manifest[10] << 16) | ((uint32_t)manifest[11] << 24);
	uint32_t count = (uint32_t)manifest[12] | ((uint32_t)manifest[13] << 8) |
	    ((uint32_t)manifest[14] << 16) | ((uint32_t)manifest[15] << 24);
	emmc_log_dec("NEMMC manifest version ", version);
	emmc_log_dec("NEMMC manifest windows ", count);
	fImageBytes = 0;
	for (unsigned i = 0; i < 8; i++) {
		fImageBytes |= (UInt64)manifest[16 + i] << (8 * i);
	}
	if (count > 4) {
		count = 4;
	}
	for (uint32_t index = 0; index < count; index++) {
		const uint8_t *entry = manifest + 0x18u + index * 128u;
		uint64_t offset = 0, size = 0;
		for (unsigned i = 0; i < 8; i++) {
			offset |= (uint64_t)entry[0x20u + i] << (8 * i);
			size |= (uint64_t)entry[0x28u + i] << (8 * i);
		}
		if (size == 0 || (offset % EMMC_BLOCK_SIZE) != 0 || (size % EMMC_BLOCK_SIZE) != 0 ||
		    offset / EMMC_BLOCK_SIZE >= blocks) {
			emmc_log("NEMMC manifest: malformed window\n");
			continue;
		}
		UInt64 base = USERDATA_FIRST_LBA + offset / EMMC_BLOCK_SIZE;
		UInt64 windowBlocks = size / EMMC_BLOCK_SIZE;
		if (base + windowBlocks > blocks) {
			emmc_log("NEMMC manifest: window outside the device\n");
			continue;
		}
		emmc_log("NEMMC window name=");
		for (unsigned i = 0; i < 32 && entry[i] != 0; i++) {
			sirius_usb_log_char((char)entry[i]);
		}
		sirius_usb_log_char('\n');
		emmc_log_dec("NEMMC window lba ", (uint64_t)base);
		emmc_log_dec("NEMMC window blocks ", windowBlocks);

		NativeEmmcBlock *window = new NativeEmmcBlock;
		if (window == nullptr || !window->init(base, windowBlocks)) {
			emmc_log("NEMMC window: allocation failed\n");
			continue;
		}
		/* Prepared only: the service is started by the publisher thread, the way ShengUfs
		 * publishes its root windows, so no service is created inside the caller's start. */
		if (!window->attach(this)) {
			emmc_log("NEMMC window: attach failed\n");
			window->release();
			continue;
		}
		for (unsigned i = 0; i < 32 && entry[i] != 0; i++) {
			fWindowName[fWindows][i] = (char)entry[i];
			if (i == 31) {
				fWindowName[fWindows][32] = '\0';
			}
		}
		fWindow[fWindows++] = window;
	}
	return (int)fWindows;
}

/* The persistent Data volume: an APFS container written into userdata right behind the root image,
 * at the next MiB (scripts/make-data-window-image.py, flashed once with fastboot). Published
 * read-write only with sirius-data-write=1 and only when an APFS container superblock (NXSB)
 * starts there; the window is the container's own size, and the transport refuses writes outside
 * it. Returns whether the window was prepared. */
bool
NativeEmmc::prepareDataWindow(UInt64 blocks)
{
	unsigned enabled = 0;
	if (!PE_parse_boot_argn("sirius-data-write", &enabled, sizeof(enabled)) || enabled != 1) {
		emmc_log("NEMMC data window: writes disabled (sirius-data-write=1 to enable)\n");
		return false;
	}
	if (fImageBytes == 0 || fWindows >= 4) {
		return false;
	}
	UInt64 first = USERDATA_FIRST_LBA + (fImageBytes + 0xfffffu) / 0x100000u * 2048u;
	static uint8_t head[EMMC_BLOCK_SIZE];
	nemmc_transport_lock_acquire();
	int read = sirius_emmc_read_block(first, head);
	nemmc_transport_lock_release();
	if (read != 0) {
		emmc_log_dec("NEMMC data window: read failed at lba ", first);
		return false;
	}
	/* nx_superblock_t: the object header, then nx_magic, nx_block_size and nx_block_count. */
	uint32_t blockSize = (uint32_t)head[36] | ((uint32_t)head[37] << 8) | ((uint32_t)head[38] << 16) |
	    ((uint32_t)head[39] << 24);
	uint64_t containerBlocks = 0;
	for (unsigned i = 0; i < 8; i++) {
		containerBlocks |= (uint64_t)head[40 + i] << (8 * i);
	}
	if (head[32] != 'N' || head[33] != 'X' || head[34] != 'S' || head[35] != 'B' || blockSize != 4096 ||
	    containerBlocks == 0) {
		emmc_log_dec("NEMMC data window: no APFS container at lba ", first);
		return false;
	}
	UInt64 windowBlocks = containerBlocks * (blockSize / EMMC_BLOCK_SIZE);
	if (first + windowBlocks > USERDATA_LAST_LBA + 1 || first + windowBlocks > blocks) {
		emmc_log_dec("NEMMC data window: container runs past userdata, blocks ", windowBlocks);
		return false;
	}
	if (sirius_emmc_set_write_window(first, windowBlocks) != 0) {
		emmc_log("NEMMC data window: the transport refused the write window\n");
		return false;
	}
	NativeEmmcBlock *window = new NativeEmmcBlock;
	if (window == nullptr || !window->init(first, windowBlocks)) {
		emmc_log("NEMMC data window: allocation failed\n");
		return false;
	}
	window->useWritable();
	if (!window->attach(this)) {
		emmc_log("NEMMC data window: attach failed\n");
		window->release();
		return false;
	}
	strlcpy(fWindowName[fWindows], "Data", sizeof(fWindowName[0]));
	fWindow[fWindows++] = window;
	emmc_log_dec("NEMMC data window lba ", first);
	emmc_log_dec("NEMMC data window blocks ", windowBlocks);
	return true;
}

/* The Data volume's seed, when the payload carries one (unixnu build payload --data-ramdisk):
 * DarwinBoot copies it into the kernel's window and names it in /chosen/memory-map/RAMDisk
 * (physical address, size). It becomes a read-write RAM window next to the eMMC ones, APFS
 * publishes its volume, and the owner Data mount finds that through native_ram_block_volume_seed()
 * below. Nothing is written back: the volume starts from the seed at every boot. */
void
NativeEmmc::prepareDataSeed()
{
	IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
	if (map == nullptr) {
		return;
	}
	OSData *range = OSDynamicCast(OSData, map->getProperty("RAMDisk"));
	uint64_t physical = 0, bytes = 0;
	if (range != nullptr && range->getLength() == 16) {
		memcpy(&physical, range->getBytesNoCopy(), 8);
		memcpy(&bytes, (const uint8_t *)range->getBytesNoCopy() + 8, 8);
	}
	map->release();
	if (bytes == 0) {
		emmc_log("NEMMC data seed: none in the payload\n");
		return;
	}
	/* Boot memory placed by the loader: it must lie inside the kernel's managed range. */
	if (bytes > 0x80000000ull || (physical & 4095) != 0 || (bytes & 4095) != 0 || physical < gPhysBase ||
	    bytes > gPhysSize || physical - gPhysBase > gPhysSize - bytes || fWindows >= 4) {
		emmc_log_hex("NEMMC data seed: unusable range at ", physical);
		emmc_log_hex("NEMMC data seed: bytes ", bytes);
		return;
	}
	uint8_t *ram = (uint8_t *)ml_static_ptovirt((vm_offset_t)physical);
	NativeEmmcBlock *window = new NativeEmmcBlock;
	if (ram == nullptr || window == nullptr || !window->init(0, bytes / EMMC_BLOCK_SIZE)) {
		emmc_log("NEMMC data seed: allocation failed\n");
		return;
	}
	window->useRam(ram);
	if (!window->attach(this)) {
		emmc_log("NEMMC data seed: attach failed\n");
		window->release();
		return;
	}
	strlcpy(fWindowName[fWindows], "Data", sizeof(fWindowName[0]));
	fWindow[fWindows++] = window;
	emmc_log_hex("NEMMC data seed at ", physical);
	emmc_log_dec("NEMMC data seed bytes ", bytes);
}

bool
NativeEmmc::windowIs(unsigned index, const char *name) const
{
	unsigned i = 0;
	for (; i < 32 && name[i] != '\0'; i++) {
		if (fWindowName[index][i] != name[i]) {
			return false;
		}
	}
	return name[i] == '\0' && fWindowName[index][i] == '\0';
}

void
NativeEmmc::publishWindow(unsigned index)
{
	if (index >= fWindows) {
		return;
	}
	emmc_log("NEMMC publish window=");
	for (unsigned i = 0; i < 32 && fWindowName[index][i] != '\0'; i++) {
		sirius_usb_log_char(fWindowName[index][i]);
	}
	sirius_usb_log_char('\n');
	if (!fWindow[index]->start(this)) {
		emmc_log("NEMMC publish: start failed\n");
	} else {
		emmc_log_dec("NEMMC published index ", index);
	}
}

/* The windows are published from here and not in start(): SystemOS first, the other windows
 * next and OS last, which is the order ShengUfs uses so the system volume of the image set is
 * available before the root volume is selected. */
static void
nemmc_window_publisher(void *owner, wait_result_t)
{
	NativeEmmc *driver = (NativeEmmc *)owner;
	unsigned count = driver->windowCount();
	unsigned order[4];
	unsigned used = 0;
	for (unsigned i = 0; i < count; i++) {
		if (driver->windowIs(i, "SystemOS")) {
			order[used++] = i;
		}
	}
	for (unsigned i = 0; i < count; i++) {
		if (!driver->windowIs(i, "SystemOS") && !driver->windowIs(i, "OS")) {
			order[used++] = i;
		}
	}
	for (unsigned i = 0; i < count; i++) {
		if (driver->windowIs(i, "OS")) {
			order[used++] = i;
		}
	}
	for (unsigned i = 0; i < used; i++) {
		driver->publishWindow(order[i]);
	}
	emmc_log_dec("NEMMC publisher done ", used);
	nemmc_report_media_table("NEMMC media table after publish");
	driver->release();
}

bool
NativeEmmc::start(IOService *provider)
{
	fBlock = nullptr;
	emmc_log("NativeEmmc: step 4 base start\n");
	nemmc_report_pan();
	if (!IOService::start(provider)) {
		return false;
	}
	emmc_log("NativeEmmc: step 5 prepare\n");
	if (sirius_emmc_prepare() != 0) {
		emmc_log("NativeEmmc: the eMMC did not come up\n");
		return false;
	}
	UInt64 blocks = sirius_emmc_total_blocks;
	if (blocks == 0) {
		emmc_log("NativeEmmc: no usable GPT on the device\n");
		return false;
	}
	emmc_log("NativeEmmc: step 6 allocate the block device\n");
	fBlock = new NativeEmmcBlock;
	emmc_log_hex("NEMMC step 7 block ", (uint64_t)(uintptr_t)fBlock);
	if (!fBlock || !fBlock->init(0, blocks)) {
		emmc_log("NativeEmmc: cannot allocate the block device\n");
		return false;
	}
	emmc_log("NativeEmmc: step 8 attach the block device\n");
	fBlock->attach(this);
	emmc_log("NativeEmmc: step 9 start the block device\n");
	if (!fBlock->start(this)) {
		emmc_log("NativeEmmc: the block device did not start\n");
		return false;
	}
	emmc_log("NEMMC published blocks:\n"); emmc_log_hex("NEMMC blocks ", blocks);
	/* Publishing the raw manifest windows is off by default: IOStorageFamily tears such a device
	 * down with a double release (see the panic in docs/HANDOFF.zh-CN.md), and the reference
	 * implementation only publishes windows it has verified first. sirius_emmc_win=1 enables the
	 * experiment without rebuilding the kernel. */
	unsigned windows = 0;
	fImageBytes = 0;
	bool persistent = false;
	if (PE_parse_boot_argn("sirius_emmc_win", &windows, sizeof(windows)) && windows != 0) {
		emmc_log_dec("NEMMC windows prepared ", (uint64_t)prepareWindows(blocks));
		persistent = prepareDataWindow(blocks);
	} else {
		emmc_log("NEMMC windows: disabled (sirius_emmc_win=1 to publish)\n");
	}
	/* One Data volume: the RAM seed only stands in when there is no persistent one. */
	if (!persistent) {
		prepareDataSeed();
	}
	emmc_log("NativeEmmc: step 10 register the driver\n");
	registerService();
	emmc_log("NativeEmmc: step 11 done\n");
	retain();
	thread_t publisher;
	if (kernel_thread_start(nemmc_window_publisher, this, &publisher) == KERN_SUCCESS) {
		thread_deallocate(publisher);
		emmc_log("NEMMC publisher started\n");
	} else {
		release();
		emmc_log("NEMMC publisher thread failed\n");
	}
	return true;
}

void
NativeEmmc::stop(IOService *provider)
{
	/* Published services are never released here. They are still members of the registry, and
	 * releasing an object a collection holds is exactly the over-release that panics with
	 * "A kext releasing a(n) IOBlockStorageDriver has corrupted the registry" (OSObject.cpp:224).
	 * The reference implementation keeps the same rule: started objects retain their owner
	 * references for the lifetime of the kernel. */
	fBlock = nullptr;
	for (unsigned i = 0; i < fWindows; i++) {
		fWindow[i] = nullptr;
	}
	fWindows = 0;
	IOService::stop(provider);
}

/* The owner SystemOS mount in kern_exec.c (offline-owner-systemos-mount.patch, built here with
 * XNU_BRINGUP_OWNER_CACHE) mounts the SystemOS cryptex as a union over / before launchd starts, so
 * dyld finds its shared cache. It asks the block driver to authorize the root and to name the device
 * of the SystemOS volume. The eMMC windows have no owner policy, so both answers are only given with
 * the explicit boot argument sirius-allow-unverified=1, as the uke port does for its UFS root. */
static int
nemmc_allow_unverified(void)
{
	static int answer = -1;
	unsigned allow = 0;

	if (answer < 0) {
		answer = PE_parse_boot_argn("sirius-allow-unverified", &allow, sizeof(allow)) && allow == 1;
	}
	return answer;
}

extern "C" int
native_verified_block_root_authorized(dev_t device)
{
	(void)device;
	return nemmc_allow_unverified();
}

/* The Data window (the RAM seed or the persistent eMMC window) a media object sits on, or nullptr
 * for one on a read-only eMMC window. */
static NativeEmmcBlock *
nemmc_data_window_of(IOService *media)
{
	unsigned depth = 0;
	for (IOService *provider = media; provider != nullptr && depth++ < 64; provider = provider->getProvider()) {
		NativeEmmcBlock *block = OSDynamicCast(NativeEmmcBlock, provider);
		if (block != nullptr) {
			return block->isData() ? block : nullptr;
		}
	}
	return nullptr;
}

/* The IOMedia whose UUID is uuid or, with uuid nullptr, the one APFS volume on the Data window (a
 * slice with a UUID), once APFS has published it: its BSD device and node, through ram the Data
 * window it sits on (nullptr for a read-only window), and through volume its UUID. APFS publishes
 * volume media after the root mounts, and launchd starts right after that, so this waits up to ten
 * seconds. */
static bool
nemmc_find_volume(const char *uuid, dev_t *device, char *path, size_t size, NativeEmmcBlock **ram,
    char *volume = nullptr)
{
	*device = NODEV;
	path[0] = '\0';
	*ram = nullptr;
	for (unsigned attempt = 0; attempt < 100; attempt++) {
		OSDictionary *matching = IOService::serviceMatching("IOMedia");
		bool ready = matching != nullptr;
		if (ready && uuid != nullptr) {
			OSDictionary *properties = OSDictionary::withCapacity(1);
			OSString *wanted = OSString::withCString(uuid);
			ready = properties != nullptr && wanted != nullptr && properties->setObject("UUID", wanted) &&
			    matching->setObject("IOPropertyMatch", properties);
			OSSafeReleaseNULL(wanted);
			OSSafeReleaseNULL(properties);
		}
		OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
		OSSafeReleaseNULL(matching);
		dev_t candidate = NODEV;
		char candidatePath[64] = {};
		char candidateVolume[37] = {};
		NativeEmmcBlock *window = nullptr;
		unsigned matches = 0;
		bool found = false;
		while (iterator != nullptr) {
			IOService *media = OSDynamicCast(IOService, iterator->getNextObject());
			if (media == nullptr) {
				break;
			}
			OSString *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
			NativeEmmcBlock *under = nemmc_data_window_of(media);
			if (uuid == nullptr && (under == nullptr || identity == nullptr ||
			    media->getProperty("Whole") == kOSBooleanTrue)) {
				continue;
			}
			if (++matches != 1) {
				found = false;
				break;
			}
			OSString *name = OSDynamicCast(OSString, media->getProperty("BSD Name"));
			OSNumber *major = OSDynamicCast(OSNumber, media->getProperty("BSD Major"));
			OSNumber *minor = OSDynamicCast(OSNumber, media->getProperty("BSD Minor"));
			if (name == nullptr || major == nullptr || minor == nullptr || name->getLength() == 0 ||
			    name->getLength() >= sizeof(candidatePath) - 6 || name->getLength() >= size - 6 ||
			    major->unsigned64BitValue() > 255 || minor->unsigned64BitValue() > 0xffffff) {
				continue;
			}
			candidate = (dev_t)makedev(major->unsigned32BitValue(), minor->unsigned32BitValue());
			snprintf(candidatePath, sizeof(candidatePath), "/dev/%s", name->getCStringNoCopy());
			strlcpy(candidateVolume, identity != nullptr ? identity->getCStringNoCopy() : "",
			    sizeof(candidateVolume));
			window = under;
			found = true;
		}
		OSSafeReleaseNULL(iterator);
		if (found && matches == 1) {
			*device = candidate;
			strlcpy(path, candidatePath, size);
			*ram = window;
			if (volume != nullptr) {
				strlcpy(volume, candidateVolume, 37);
			}
			return true;
		}
		IOSleep(100);
	}
	return false;
}

extern "C" int
native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
	if (uuid == nullptr || device == nullptr || path == nullptr || size < 6 || strlen(uuid) != 36) {
		return 0;
	}
	*device = NODEV;
	path[0] = '\0';
	if (!nemmc_allow_unverified()) {
		emmc_log("NEMMC owner volume refused: no sirius-allow-unverified\n");
		return 0;
	}
	NativeEmmcBlock *ram;
	if (!nemmc_find_volume(uuid, device, path, size, &ram)) {
		emmc_log("NEMMC owner volume not found\n");
		return 0;
	}
	emmc_log("NEMMC owner volume ");
	emmc_log(path);
	emmc_log("\n");
	return 1;
}

/* The owner Data mount (offline-owner-systemos-mount.patch, kern_exec.c built with
 * XNU_SIRIUS_DATA_SEED) asks for the device of the Data volume and the seed behind it. RamBlock
 * provides this on other boards; here it is the one APFS volume on NativeEmmc's Data window, the
 * persistent eMMC window or else the RAM seed, the only places a Data volume can be. Every seed
 * build formats a new volume with a new UUID, so a volume whose UUID differs from the kernel's
 * (CONFIG_XNU_OWNER_DATA_VOLUME_UUID) is still taken, and the difference is reported, rather than
 * every new seed needing a new kernel. base is the RAM seed's kernel address (0 on the eMMC); the
 * digest stays zero, as nothing on this board checks the seed. */
extern "C" int
native_ram_block_volume_seed(const char *uuid, dev_t *device, char *path, size_t size, uint64_t *base,
    uint64_t *bytes, uint8_t *digest)
{
	if (uuid == nullptr || device == nullptr || path == nullptr || size < 6 || strlen(uuid) != 36 ||
	    base == nullptr || bytes == nullptr || digest == nullptr) {
		return 0;
	}
	*base = 0;
	*bytes = 0;
	memset(digest, 0, 32);
	NativeEmmcBlock *window;
	char volume[37] = {};
	if (!nemmc_find_volume(nullptr, device, path, size, &window, volume) || window == nullptr) {
		*device = NODEV;
		path[0] = '\0';
		emmc_log("NEMMC data volume not found on a Data window\n");
		return 0;
	}
	if (strcmp(volume, uuid) != 0) {
		emmc_log("NEMMC data volume is ");
		emmc_log(volume);
		emmc_log(", the kernel names ");
		emmc_log(uuid);
		emmc_log("\n");
	}
	*base = (uint64_t)(uintptr_t)window->ram();
	*bytes = window->blocks() * EMMC_BLOCK_SIZE;
	emmc_log(window->ram() != nullptr ? "NEMMC data volume (RAM) " : "NEMMC data volume (eMMC) ");
	emmc_log(path);
	emmc_log("\n");
	return 1;
}

/* With the seed path compiled in, kern_exec.c's boot-time GPU addon install also asks the RAM block
 * provider for /chosen/memory-map/GpuAddon. Its declaration is weak, but the static kernel link
 * still needs a definition: this board's payload carries no addon, so there is none to report. */
extern "C" int
native_ram_block_gpu_addon(const uint8_t **data, uint64_t *bytes)
{
	if (data != nullptr) {
		*data = nullptr;
	}
	if (bytes != nullptr) {
		*bytes = 0;
	}
	return 0;
}

/* Entry point of bsd_init (patches/xnu/sirius-usb-console.patch). No personality is installed:
 * the board owns the controller, so the probe creates and starts the driver once, the way the
 * PMGR nub of this port is started. It runs after the console starts, so the numbered steps are
 * in the console ring even if the driver does not return. */
extern "C" void
sirius_native_emmc_probe(void)
{
	static NativeEmmc *device = nullptr;

	unsigned enabled = 1;
	if (PE_parse_boot_argn("sirius_emmc_drv", &enabled, sizeof(enabled)) && enabled == 0) {
		emmc_log("NativeEmmc: disabled by boot arg\n");
		return;
	}
	if (device != nullptr) {
		return;
	}
	if (nemmc_worker == nullptr) {
		nemmc_bounce = (uint8_t *)IOMalloc(sirius_emmc_run_blocks * EMMC_BLOCK_SIZE);
		if (nemmc_bounce == nullptr) {
			emmc_log("NativeEmmc: no bounce buffer\n");
			return;
		}
		nemmc_free_list = nullptr;
		for (unsigned i = NemmcRequestPoolSize; i > 0; i--) {
			nemmc_pool[i - 1].next = nemmc_free_list;
			nemmc_free_list = &nemmc_pool[i - 1];
		}
		nemmc_pending_head = nullptr;
		nemmc_pending_tail = nullptr;
		thread_t worker = nullptr;
		if (kernel_thread_start(nemmc_worker_thread, nullptr, &worker) != KERN_SUCCESS) {
			emmc_log("NativeEmmc: no worker thread\n");
			return;
		}
		nemmc_worker = worker;
		thread_deallocate(worker);
	}
	emmc_log("NativeEmmc: step 0 enter\n");
	NativeEmmc *candidate = new NativeEmmc;
	emmc_log_hex("NEMMC step 1 instance ", (uint64_t)(uintptr_t)candidate);
	if (candidate == nullptr || !candidate->init()) {
		emmc_log("NativeEmmc: allocation failed\n");
		return;
	}
	emmc_log("NativeEmmc: step 2 init done\n");
	IOService *platform = IOService::getPlatform();
	emmc_log_hex("NEMMC step 3 platform ", (uint64_t)(uintptr_t)platform);
	if (platform != nullptr && !candidate->attach(platform)) {
		emmc_log("NativeEmmc: attach to the platform failed\n");
		candidate->release();
		return;
	}
	if (!candidate->start(platform)) {
		emmc_log("NativeEmmc: the driver did not start\n");
		candidate->detach(platform);
		candidate->release();
		return;
	}
	device = candidate;
	nemmc_report_media();
	thread_t thread;
	if (kernel_thread_start(nemmc_heartbeat_thread, nullptr, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		emmc_log("NEMMC heartbeat started\n");
	} else {
		emmc_log("NEMMC heartbeat failed\n");
	}
}
