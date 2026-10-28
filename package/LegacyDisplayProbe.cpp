// SPDX-License-Identifier: BSD-2-Clause-Patent
// sirius GOP framebuffer display probe and lifecycle integration.
// Adapted from sheng / asteroids for the Xiaomi Mi 8 SE (SDM710).
#include <stdarg.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSData.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <machine/machine_routines.h>

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" void native_gop_presenter_static_init(void) __asm__("__GLOBAL__sub_I_NativeGOPPresenter.cpp");
extern "C" IOService *native_gop_presenter_create(void);

static IOService *pendingLegacyDisplay;
static IOService *activeGOPPresenter, *activeGOPNub;
static IOService *gpuHandoffPresenter;
extern "C" bool native_gop_gpu_offer(IOService *, void *display, IOMemoryDescriptor *, uint64_t stride, uint32_t width, uint32_t height);
extern "C" void native_gop_presenter_set_source(IOService *presenter, IOService *nub);
volatile uint32_t native_legacy_gpu_fallbacks, native_legacy_gpu_last_status;
static IOService *activeLegacyDisplay;

static IOMemoryMap *diagnosticAnchorMapping;
extern "C" int boot_progress_pulse_anchor(uintptr_t, uint64_t, unsigned, unsigned, unsigned) __attribute__((weak_import));

static IORegistryEntry *unitScaleChosen;
static bool unitScaleAdded, unitRotationAdded;

static void finishUnitScalePolicy(bool keep) {
    if (unitScaleChosen && !keep) {
        if (unitScaleAdded) unitScaleChosen->removeProperty("display-scale");
        if (unitRotationAdded) unitScaleChosen->removeProperty("display-rotation");
    }
    OSSafeReleaseNULL(unitScaleChosen);
    unitScaleAdded = false;
    unitRotationAdded = false;
}

static bool publishOrMatchWord(IORegistryEntry *entry, const char *key, uint32_t value, bool &added) {
    OSObject *previous = entry->getProperty(key);
    if (previous) {
        OSData *data = OSDynamicCast(OSData, previous);
        uint32_t word = 0;
        if (!data || data->getLength() != sizeof(word)) return false;
        memcpy(&word, data->getBytesNoCopy(), sizeof(word));
        return word == value;
    }
    OSData *data = OSData::withBytes(&value, sizeof(value));
    bool result = data && entry->setProperty(key, data);
    OSSafeReleaseNULL(data);
    added = result;
    return result;
}

extern "C" {
volatile uint32_t native_legacy_stage;
volatile int native_legacy_meta_status = -1;
volatile uint32_t native_legacy_init_result, native_legacy_start_result, native_legacy_probe_match;
volatile uint32_t native_legacy_parent_match, native_legacy_object_size, native_legacy_released;
volatile uint64_t native_legacy_object, native_legacy_nub, native_legacy_workloop;
volatile uint32_t native_legacy_guard_slot;
volatile uint64_t native_legacy_guard_object, native_legacy_guard_caller;
volatile int native_legacy_surface_status = -1;
volatile uint32_t native_legacy_surface_found;
volatile uint64_t native_legacy_video_base, native_legacy_video_bytes;
volatile uint64_t native_legacy_gop_map, native_legacy_gop_virtual, native_legacy_gop_physical, native_legacy_gop_bytes;
volatile uint32_t native_legacy_gop_width, native_legacy_gop_height;
volatile uint32_t native_legacy_registration_calls, native_legacy_properties_length;
volatile uint32_t native_legacy_dt_attached;
volatile int native_legacy_unit_scale_status = -1;
volatile uint32_t native_legacy_unit_scale_calls, native_legacy_chosen_scale, native_legacy_chosen_rotation;
volatile uint32_t native_legacy_pm_stage, native_legacy_pm_calls, native_legacy_pm_callbacks, native_legacy_pm_ordinal;
volatile int native_legacy_pm_status = -1;
volatile uint32_t native_legacy_swap_validation_calls, native_legacy_swap_validation_result, native_legacy_swap_validation_reason;
volatile uint64_t native_legacy_swap_request, native_legacy_swap_surface;
volatile uint32_t native_legacy_transfer_calls;
volatile int native_legacy_transfer_status = -1;
volatile uint64_t native_legacy_transfer_bytes, native_legacy_transfer_request;
volatile uint32_t native_legacy_retirement_calls, native_legacy_retired_generation, native_legacy_presented_generation, native_legacy_retired_frames;
volatile uint64_t native_legacy_retired_current, native_legacy_retired_pending;
char native_legacy_properties[4096];
volatile uint32_t native_legacy_client_calls, native_legacy_client_type;
volatile int native_legacy_client_pid, native_legacy_client_status = -1;
volatile uint64_t native_legacy_client_task, native_legacy_client_object;
char native_legacy_client_class[64];

extern int boot_progress_prepare_scanout(void) __attribute__((weak_import));
extern void boot_progress_cancel_scanout(void) __attribute__((weak_import));
extern void boot_progress_stage(const char *) __attribute__((weak_import));

volatile uint32_t native_legacy_prepare_calls, native_legacy_prepare_failures, native_legacy_prepare_retried, native_legacy_prepare_max_attempts;
volatile uint32_t native_legacy_prepare_reason, native_legacy_prepare_attempts;
volatile uint64_t native_legacy_prepare_wait_us;

static inline uint64_t probeNowUs(void) {
    uint64_t now, freq;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq ? (now * 1000000ULL) / freq : 0;
}

bool native_legacy_prepare_scanout(void) {
    native_legacy_prepare_calls = native_legacy_prepare_calls + 1;
    if (!boot_progress_prepare_scanout) return true;
    const uint64_t began = probeNowUs();
    for (unsigned attempt = 0; attempt < 50; ++attempt) {
        native_legacy_prepare_attempts = attempt + 1;
        if (boot_progress_prepare_scanout()) {
            if (attempt) {
                native_legacy_prepare_retried = native_legacy_prepare_retried + 1;
                native_legacy_prepare_wait_us = native_legacy_prepare_wait_us + (probeNowUs() - began);
                if (attempt + 1 > native_legacy_prepare_max_attempts) native_legacy_prepare_max_attempts = attempt + 1;
            }
            return true;
        }
        IOSleep(1);
    }
    native_legacy_prepare_failures = native_legacy_prepare_failures + 1;
    native_legacy_prepare_wait_us = native_legacy_prepare_wait_us + (probeNowUs() - began);
    native_legacy_prepare_reason = 3;
    if (boot_progress_cancel_scanout) boot_progress_cancel_scanout();
    return false;
}

__attribute__((noinline)) void native_legacy_display_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
__attribute__((noinline)) void native_legacy_gop_mapping_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
__attribute__((noinline)) void native_legacy_client_checkpoint(void) { __asm__ volatile("" ::: "memory"); }

void native_legacy_pm_observed(unsigned stage, IOReturn status, unsigned long ordinal) {
    native_legacy_pm_stage = stage;
    native_legacy_pm_status = status;
    native_legacy_pm_ordinal = (uint32_t)ordinal;
    native_legacy_pm_calls = native_legacy_pm_calls + 1;
    if (stage == 3) native_legacy_pm_callbacks = native_legacy_pm_callbacks + 1;
}

__attribute__((noinline)) void native_legacy_swap_validation_checkpoint(bool, unsigned, const void *, const void *) { __asm__ volatile("" ::: "memory"); }

volatile uint32_t native_legacy_swap_accepted_count, native_legacy_swap_rejected_count, native_legacy_rej_reason;
volatile uint64_t native_legacy_rej_surfaces[4];
volatile uint32_t native_legacy_rej_state[3], native_legacy_rej_options[3], native_legacy_rej_size[2], native_legacy_rej_rects[2][4];
char native_legacy_rej_class[32];
volatile uint64_t native_legacy_rej_surface[7];

void native_legacy_surface_rejected(uint64_t width, uint64_t height, uint32_t format, uint32_t planes, uint64_t stride, uint64_t allocation, uint64_t bytes) {
    native_legacy_rej_surface[0] = width;
    native_legacy_rej_surface[1] = height;
    native_legacy_rej_surface[2] = format;
    native_legacy_rej_surface[3] = planes;
    native_legacy_rej_surface[4] = stride;
    native_legacy_rej_surface[5] = allocation;
    native_legacy_rej_surface[6] = bytes;
}

static void recordRejectedSwap(unsigned reason, const void *request, const void *surface) {
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(request);
    if (!bytes) return;
    native_legacy_rej_reason = reason;
    uint64_t surfaces[4] = {};
    memcpy(surfaces, bytes + 2624, sizeof(surfaces));
    for (unsigned i = 0; i < 4; ++i) native_legacy_rej_surfaces[i] = surfaces[i];
    const unsigned stateOffsets[3] = {792, 796, 800}, optionOffsets[3] = {804, 808, 812};
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t a = 0, b = 0;
        memcpy(&a, bytes + stateOffsets[i], 4);
        memcpy(&b, bytes + optionOffsets[i], 4);
        native_legacy_rej_state[i] = a;
        native_legacy_rej_options[i] = b;
    }
    uint32_t w = 0, h = 0;
    memcpy(&w, bytes + 816, 4);
    memcpy(&h, bytes + 820, 4);
    native_legacy_rej_size[0] = w;
    native_legacy_rej_size[1] = h;
    const unsigned rectOffsets[2] = {17, 81};
    for (unsigned r = 0; r < 2; ++r) {
        uint32_t rect[4] = {};
        memcpy(rect, bytes + rectOffsets[r], sizeof(rect));
        for (unsigned i = 0; i < 4; ++i) native_legacy_rej_rects[r][i] = rect[i];
    }
    char name[32] = {};
    if (surface) {
        const OSObject *object = reinterpret_cast<const OSObject *>(surface);
        const OSMetaClass *meta = object->getMetaClass();
        const char *cls = meta ? meta->getClassName() : nullptr;
        if (cls) strlcpy(name, cls, sizeof(name));
    }
    memcpy(native_legacy_rej_class, name, sizeof(name));
}

#define NATIVE_LEGACY_FRAMES 32
volatile uint32_t native_legacy_frame_count;
volatile uint32_t native_legacy_frame_kind[NATIVE_LEGACY_FRAMES];
volatile uint32_t native_legacy_frame_time[NATIVE_LEGACY_FRAMES];
volatile uint64_t native_legacy_frame_stats[NATIVE_LEGACY_FRAMES][5];
static uint64_t frameEpoch;

void native_legacy_frame_observed(unsigned kind, uint64_t count, uint64_t minx, uint64_t miny, uint64_t maxx, uint64_t maxy) {
    uint32_t n = native_legacy_frame_count;
    uint64_t now, freq;
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (!frameEpoch) frameEpoch = now;
    if (n < NATIVE_LEGACY_FRAMES) {
        native_legacy_frame_kind[n] = kind;
        native_legacy_frame_time[n] = freq ? (uint32_t)((now - frameEpoch) / freq) : 0;
        native_legacy_frame_stats[n][0] = count;
        native_legacy_frame_stats[n][1] = minx;
        native_legacy_frame_stats[n][2] = miny;
        native_legacy_frame_stats[n][3] = maxx;
        native_legacy_frame_stats[n][4] = maxy;
    }
    native_legacy_frame_count = n + 1;
}

volatile uint32_t native_legacy_frame_colors[8][4];
volatile uint64_t native_legacy_frame_color_counts[8][4], native_legacy_frame_color_other[8];

void native_legacy_frame_colors_observed(const uint32_t *colors, const uint64_t *counts, uint64_t other) {
    uint32_t n = native_legacy_frame_count;
    if (!n || n > 8) return;
    n -= 1;
    for (unsigned i = 0; i < 4; ++i) {
        native_legacy_frame_colors[n][i] = colors[i];
        native_legacy_frame_color_counts[n][i] = counts[i];
    }
    native_legacy_frame_color_other[n] = other;
}

struct SwapEvent { uint64_t submitted, offered, done; uint32_t number; };
static SwapEvent swapEvents[64];
static uint32_t swapEventCount;

void native_legacy_swap_stage(unsigned stage) {
    const uint64_t now = probeNowUs();
    uint32_t total = swapEventCount;
    if (stage == 0) {
        SwapEvent &e = swapEvents[total % 64];
        e = {now, 0, 0, total + 1};
        swapEventCount = total + 1;
        return;
    }
    uint32_t first = total > 64 ? total - 64 : 0;
    for (uint32_t i = first; i < total; ++i) {
        SwapEvent &e = swapEvents[i % 64];
        if (stage == 1 && !e.offered && !e.done) { e.offered = now; return; }
        if (stage == 2 && !e.done) { e.done = now; if (!e.offered) e.offered = now; return; }
    }
}

static volatile uint64_t vsyncEvents[6];
static volatile unsigned vsyncHz;
void native_legacy_vsync_observed(unsigned event, unsigned hz) {
    if (event < 6) vsyncEvents[event] = vsyncEvents[event] + 1;
    vsyncHz = hz;
}

void native_legacy_swap_validation_observed(bool accepted, unsigned reason, const void *request, const void *surface) {
    if (accepted) native_legacy_swap_stage(0);
    if (accepted) native_legacy_swap_accepted_count = native_legacy_swap_accepted_count + 1;
    else {
        native_legacy_swap_rejected_count = native_legacy_swap_rejected_count + 1;
        recordRejectedSwap(reason, request, surface);
    }
    native_legacy_swap_validation_calls = native_legacy_swap_validation_calls + 1;
    native_legacy_swap_validation_result = accepted;
    native_legacy_swap_validation_reason = reason;
    native_legacy_swap_request = (uintptr_t)request;
    native_legacy_swap_surface = (uintptr_t)surface;
    native_legacy_swap_validation_checkpoint(accepted, reason, request, surface);
}

volatile uint64_t native_legacy_copy_n, native_legacy_copy_last, native_legacy_copy_max, native_legacy_copy_sum;
volatile uint64_t native_legacy_copy_bytes_last, native_legacy_copy_bytes_sum;

void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes) {
    native_legacy_copy_bytes_last = bytes;
    native_legacy_copy_bytes_sum = native_legacy_copy_bytes_sum + bytes;
    native_legacy_copy_n = native_legacy_copy_n + 1;
    native_legacy_copy_last = ticks;
    native_legacy_copy_sum = native_legacy_copy_sum + ticks;
    if (ticks > native_legacy_copy_max) native_legacy_copy_max = ticks;
}

__attribute__((noinline)) void native_legacy_transfer_observed(IOReturn status, const void *request, uint64_t bytes) {
    native_legacy_transfer_calls = native_legacy_transfer_calls + 1;
    if (status == kIOReturnSuccess) native_legacy_swap_stage(2);
    native_legacy_transfer_status = status;
    native_legacy_transfer_request = (uintptr_t)request;
    native_legacy_transfer_bytes = bytes;
    if (status != kIOReturnSuccess && boot_progress_stage) {
        char message[120];
        snprintf(message, sizeof(message), "DISPLAY COPY FAILED status=%08x prepare=%u tries=%u",
                 (unsigned)status, native_legacy_prepare_reason, native_legacy_prepare_attempts);
        boot_progress_stage(message);
    }
}

bool native_legacy_gpu_offer(void *display, IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height) {
    return gpuHandoffPresenter && native_gop_gpu_offer(gpuHandoffPresenter, display, memory, stride, width, height);
}

void native_legacy_gpu_fallback_observed(int status) {
    native_legacy_gpu_last_status = (uint32_t)status;
    native_legacy_gpu_fallbacks = native_legacy_gpu_fallbacks + 1;
}

__attribute__((noinline)) void native_legacy_retirement_observed(IOService *display) {
    const unsigned char *bytes = reinterpret_cast<const unsigned char *>(display);
    uint64_t current = 0, pending = 0;
    uint32_t completed = 0, presented = 0, frames = 0;
    memcpy(&current, bytes + 5472, 8);
    memcpy(&pending, bytes + 5480, 8);
    memcpy(&completed, bytes + 5820, 4);
    memcpy(&presented, bytes + 5824, 4);
    memcpy(&frames, bytes + 252, 4);
    native_legacy_retired_current = current;
    native_legacy_retired_pending = pending;
    native_legacy_retired_generation = completed;
    native_legacy_presented_generation = presented;
    native_legacy_retired_frames = frames;
    native_legacy_retirement_calls = native_legacy_retirement_calls + 1;
}

void native_legacy_registration_observed(IOService *display) {
    native_legacy_registration_calls = native_legacy_registration_calls + 1;
    OSSerialize *xml = OSSerialize::withCapacity(2048);
    if (xml && display->serializeProperties(xml)) {
        unsigned size = xml->getLength();
        if (size >= sizeof(native_legacy_properties)) size = sizeof(native_legacy_properties) - 1;
        memcpy(native_legacy_properties, xml->text(), size);
        native_legacy_properties[size] = 0;
        native_legacy_properties_length = size;
    }
    OSSafeReleaseNULL(xml);
}

void native_legacy_client_enter(task_t task, uint32_t type) {
    native_legacy_client_calls = native_legacy_client_calls + 1;
    native_legacy_client_task = (uintptr_t)task;
    native_legacy_client_type = type;
    native_legacy_client_pid = task ? proc_pid((proc_t)get_bsdtask_info(task)) : 0;
}

enum { kSeenClients = 8, kSelectors = 96, kRegEvents = 8, kVtableSlots = 256 };
struct ClientStat {
    uintptr_t object;
    int pid;
    uint32_t calls, fails;
    uint32_t selector_calls[kSelectors], selector_fails[kSelectors], selector_status[kSelectors];
    unsigned regs;
    struct { uint32_t type; uint64_t port; uint32_t ref, status; } reg[kRegEvents];
    uintptr_t original_external, original_register;
};
static ClientStat seenClients[kSeenClients];
static unsigned seenClientCount;

typedef IOReturn (*ExternalFn)(IOUserClient *, uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *);
typedef IOReturn (*RegisterFn)(IOUserClient *, mach_port_t, UInt32, UInt32);

static ClientStat *statFor(IOUserClient *client) {
    for (unsigned i = 0; i < seenClientCount; ++i) {
        if (seenClients[i].object == (uintptr_t)client) return &seenClients[i];
    }
    return nullptr;
}

static IOReturn wrappedExternal(IOUserClient *client, uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target, void *ref) {
    ClientStat *stat = statFor(client);
    IOReturn status = stat ? reinterpret_cast<ExternalFn>(stat->original_external)(client, selector, args, dispatch, target, ref) : kIOReturnNotAttached;
    if (stat) {
        ++stat->calls;
        if (status) ++stat->fails;
        if (selector < kSelectors) {
            ++stat->selector_calls[selector];
            if (status) ++stat->selector_fails[selector];
            stat->selector_status[selector] = (uint32_t)status;
        }
    }
    return status;
}

static IOReturn wrappedRegister(IOUserClient *client, mach_port_t port, UInt32 type, UInt32 refCon) {
    ClientStat *stat = statFor(client);
    IOReturn status = stat ? reinterpret_cast<RegisterFn>(stat->original_register)(client, port, type, refCon) : kIOReturnNotAttached;
    if (stat && stat->regs < kRegEvents) {
        auto &e = stat->reg[stat->regs++];
        e.type = type;
        e.port = (uint64_t)(uintptr_t)port;
        e.ref = refCon;
        e.status = (uint32_t)status;
    }
    return status;
}

static void interpose(IOUserClient *client, int pid) {
    if (seenClientCount >= kSeenClients) return;
    ClientStat &stat = seenClients[seenClientCount];
    memset(&stat, 0, sizeof(stat));
    uintptr_t *vtable = *reinterpret_cast<uintptr_t **>(client);
    uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc(kVtableSlots * sizeof(uintptr_t)));
    if (!copy) return;
    memcpy(copy, vtable, kVtableSlots * sizeof(uintptr_t));
    stat.object = (uintptr_t)client;
    stat.pid = pid;
    stat.original_external = copy[168];
    stat.original_register = copy[175];
    copy[168] = (uintptr_t)&wrappedExternal;
    copy[175] = (uintptr_t)&wrappedRegister;
    ++seenClientCount;
    *reinterpret_cast<uintptr_t **>(client) = copy;
}

void native_legacy_client_observed(IOReturn status, IOUserClient *client) {
    if (client && !status) interpose(client, native_legacy_client_pid);
    native_legacy_client_status = status;
    native_legacy_client_object = (uintptr_t)client;
    if (client) strlcpy(native_legacy_client_class, client->getMetaClass()->getClassName(), sizeof(native_legacy_client_class));
    native_legacy_client_checkpoint();
}

__attribute__((noinline)) bool native_legacy_gop_mapping_observed(IOService *object, IOMemoryMap *mapping, uint32_t width, uint32_t height) {
    if ((uintptr_t)object != native_legacy_object) panic("NativeLegacyDisplay: wrong mapping owner");
    native_legacy_gop_map = (uintptr_t)mapping;
    native_legacy_gop_virtual = mapping->getVirtualAddress();
    native_legacy_gop_physical = mapping->getPhysicalAddress();
    native_legacy_gop_bytes = mapping->getLength();
    native_legacy_gop_width = width;
    native_legacy_gop_height = height;
    if (boot_progress_pulse_anchor && !diagnosticAnchorMapping) {
        PE_Video anchorVideo = {};
        if (!PE_current_console(&anchorVideo) && anchorVideo.v_depth == 32 &&
            anchorVideo.v_rowBytes <= 65536 && anchorVideo.v_width == width &&
            anchorVideo.v_height == height && anchorVideo.v_baseAddr == mapping->getPhysicalAddress() &&
            !anchorVideo.v_rotate) {
            mapping->retain();
            if (boot_progress_pulse_anchor(mapping->getVirtualAddress(), mapping->getLength(), width, height,
                                           (unsigned)anchorVideo.v_rowBytes)) {
                diagnosticAnchorMapping = mapping;
            } else {
                mapping->release();
            }
        }
    }
    native_legacy_gop_mapping_checkpoint();
    unsigned enabled = 0;
    PE_parse_boot_argn("offline-legacy-unit-scale", &enabled, sizeof(enabled));
    if (!enabled) return true;
    native_legacy_unit_scale_calls = native_legacy_unit_scale_calls + 1;
    native_legacy_unit_scale_status = 1;
    unsigned phase = 0;
    PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
    PE_Video video = {};
    if (enabled < 1 || enabled > 3 || phase != 4 || unitScaleChosen || PE_current_console(&video) ||
        video.v_width != width || video.v_height != height || video.v_rotate ||
        mapping->getPhysicalAddress() != video.v_baseAddr) return false;

    unitScaleChosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!unitScaleChosen || !publishOrMatchWord(unitScaleChosen, "display-scale", enabled, unitScaleAdded) ||
        !publishOrMatchWord(unitScaleChosen, "display-rotation", video.v_rotate, unitRotationAdded)) {
        finishUnitScalePolicy(false);
        return false;
    }
    OSData *scale = OSDynamicCast(OSData, unitScaleChosen->getProperty("display-scale"));
    OSData *rotation = OSDynamicCast(OSData, unitScaleChosen->getProperty("display-rotation"));
    uint32_t actualScale = 0, actualRotation = 0;
    if (!scale || !rotation || scale->getLength() != 4 || rotation->getLength() != 4) {
        finishUnitScalePolicy(false);
        return false;
    }
    memcpy(&actualScale, scale->getBytesNoCopy(), 4);
    memcpy(&actualRotation, rotation->getBytesNoCopy(), 4);
    native_legacy_chosen_scale = actualScale;
    native_legacy_chosen_rotation = actualRotation;
    native_legacy_unit_scale_status = (actualScale == enabled && actualRotation == video.v_rotate) ? 0 : 2;
    if (native_legacy_unit_scale_status) {
        finishUnitScalePolicy(false);
        return false;
    }
    return true;
}

__attribute__((noreturn, noinline)) void native_legacy_backend_guard(unsigned slot, void *object, void *caller) {
    native_legacy_guard_slot = slot;
    native_legacy_guard_object = (uintptr_t)object;
    native_legacy_guard_caller = (uintptr_t)caller;
    panic("NativeLegacyDisplay: leaf=%u caller=%llx stage=%u PM=%u swap=%u/%u copy=%08x bytes=%llu retire=%u handoff=%u/%u",
          slot, (uint64_t)(uintptr_t)caller, native_legacy_stage, native_legacy_pm_stage,
          native_legacy_swap_validation_result, native_legacy_swap_validation_reason,
          (unsigned)native_legacy_transfer_status, native_legacy_transfer_bytes, native_legacy_retirement_calls,
          native_legacy_prepare_reason, native_legacy_prepare_attempts);
}

void native_legacy_display_probe(void) {
    if (native_legacy_stage) return;
    unsigned reg = 0;
    PE_parse_boot_argn("offline-legacy-register", &reg, sizeof(reg));
    if (reg != 1) return;

    native_legacy_stage = 1;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        native_legacy_display_checkpoint();
        return;
    }
    native_legacy_display_static_init();
    native_gop_presenter_static_init();
    native_legacy_meta_status = OSMetaClass::postModLoad(handle);
    if (native_legacy_meta_status) {
        native_legacy_display_checkpoint();
        return;
    }

    PE_Video video = {};
    native_legacy_stage = 2;
    if (PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height ||
        video.v_depth != 32 || video.v_rotate || video.v_width > 16384 || video.v_height > 16384 ||
        video.v_rowBytes < video.v_width * 4 || video.v_rowBytes > UINT64_MAX / video.v_height) {
        native_legacy_display_checkpoint();
        return;
    }

    IOService *display = native_legacy_display_allocate();
    native_legacy_object = (uintptr_t)display;
    if (!display) {
        native_legacy_display_checkpoint();
        return;
    }

    native_legacy_object_size = display->getMetaClass()->getClassSize();
    native_legacy_parent_match = (display->metaCast("IOMobileFramebufferLegacy") &&
                                  display->metaCast("IOMobileFramebuffer") &&
                                  display->metaCast("IOService"));
    native_legacy_stage = 3;
    native_legacy_init_result = display->init();
    unsigned phase = 1;
    PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
    if (native_legacy_init_result && (phase == 2 || phase == 3 || phase == 4)) {
        pendingLegacyDisplay = display;
        return;
    }
    native_legacy_stage = 7;
    display->release();
    native_legacy_released = 1;
    native_legacy_stage = 8;
    native_legacy_display_checkpoint();
}

void native_legacy_display_late_probe(void) {
    IOService *display = pendingLegacyDisplay;
    if (!display) return;
    pendingLegacyDisplay = nullptr;

    native_legacy_surface_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    native_legacy_surface_found = surface != nullptr;
    OSSafeReleaseNULL(surface);
    OSSafeReleaseNULL(matching);

    PE_Video video = {};
    if (!native_legacy_surface_status && native_legacy_surface_found && !PE_current_console(&video) &&
        video.v_baseAddr && video.v_width && video.v_height && video.v_depth == 32 &&
        !video.v_rotate && video.v_width <= 16384 && video.v_height <= 16384 &&
        video.v_rowBytes >= video.v_width * 4 && video.v_rowBytes <= UINT64_MAX / video.v_height) {
        native_legacy_video_base = video.v_baseAddr;
        native_legacy_video_bytes = video.v_rowBytes * video.v_height;
        native_legacy_stage = 4;
        IOPlatformDevice *nub = new IOPlatformDevice;
        native_legacy_nub = (uintptr_t)nub;
        bool nubAttached = false, nubStarted = false, attached = false, started = false;
        IORegistryEntry *displayTreeParent = nullptr;
        bool dtAttached = false;
        IOService *parent = IOService::getResourceService();
        IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
        OSArray *ranges = OSArray::withCapacity(1);
        if (nub && memory && ranges && parent && nub->init()) {
            ranges->setObject(memory);
            nub->setDeviceMemory(ranges);
            nub->setName("NativeGOPFramebuffer");
            nubAttached = nub->attach(parent);
            if (nubAttached) nubStarted = nub->start(parent);
            if (nubStarted) {
                unsigned phase = 0;
                PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
                if (phase == 4) {
                    displayTreeParent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
                    const char type[] = "display-subsystem";
                    OSData *data = OSData::withBytes(type, sizeof(type));
                    if (displayTreeParent && data && nub->setProperty("device_type", data)) {
                        nub->setName("gop-display", gIODTPlane);
                        dtAttached = nub->attachToParent(displayTreeParent, gIODTPlane);
                    }
                    OSSafeReleaseNULL(data);
                    native_legacy_dt_attached = dtAttached;
                }
                if (phase == 3) {
                    IOService *presenter = native_gop_presenter_create();
                    bool presenterAttached = false;
                    if (presenter && presenter->init() && (presenterAttached = presenter->attach(nub)) && presenter->start(nub)) {
                        activeGOPPresenter = presenter;
                        activeGOPNub = nub;
                        nub = nullptr;
                        nubAttached = false;
                        nubStarted = false;
                    } else {
                        if (presenterAttached) presenter->detach(nub);
                        OSSafeReleaseNULL(presenter);
                    }
                } else {
                    SInt32 score = 0;
                    native_legacy_probe_match = (phase != 4 || dtAttached) && display->probe(nub, &score) == display;
                    if (native_legacy_probe_match) attached = display->attach(nub);
                    if (attached) {
                        native_legacy_stage = 5;
                        started = display->start(nub);
                        native_legacy_start_result = started;
                    }
                    if (started && phase == 4) {
                        activeLegacyDisplay = display;
                        activeGOPNub = nub;
                        native_legacy_stage = 10;
                        IOLog("SIRIUS DISPLAY: GOP display registered (%ux%u at 0x%llx, rowBytes %u)\n",
                              (unsigned)video.v_width, (unsigned)video.v_height,
                              (unsigned long long)video.v_baseAddr, (unsigned)video.v_rowBytes);
                        finishUnitScalePolicy(true);
                        OSSafeReleaseNULL(displayTreeParent);
                        native_legacy_display_checkpoint();
                        return;
                    }
                }
            }
        }
        native_legacy_stage = 6;
        finishUnitScalePolicy(false);
        if (started) display->stop(nub);
        if (attached) display->detach(nub);
        if (dtAttached) nub->detachFromParent(displayTreeParent, gIODTPlane);
        if (nubStarted) nub->stop(parent);
        if (nubAttached) nub->detach(parent);
        OSSafeReleaseNULL(ranges);
        OSSafeReleaseNULL(memory);
        OSSafeReleaseNULL(nub);
        OSSafeReleaseNULL(displayTreeParent);
    }
    native_legacy_stage = 7;
    display->release();
    native_legacy_released = 1;
    native_legacy_stage = 8;
    native_legacy_display_checkpoint();
}
}
