// SPDX-License-Identifier: BSD-2-Clause-Patent
// The touch screen for iOS: publishes ShengHidInput's touch-only NativeHidInput (a Digitizer/Touch Screen IOHIDDevice)
// on IOHIDResource, fed by the FTS521 driver (package/sirius_touch.c: native_touch_* and native_touch_set_sink).
// Adapted from UniXNU-sheng package/HidInputProbe.cpp (08cde20): the same class registration and the same properties
// backboardd needs on the event service, without the keyboard device or scripted Home presses.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSKext.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>

extern "C" kern_return_t kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *);
extern "C" void thread_deallocate(thread_t);
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" {
extern kern_return_t OSKextLoadKextWithIdentifier(const char *);
// [0] init [1] attach [2] started [3] tick [4] key report [5] key report [6] touch report [7] start failed (NativeHidInput's events)
volatile uint32_t native_hid_input_events[8];
volatile int native_hid_input_results[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
volatile uint32_t sirius_hid_stage;
volatile int sirius_hid_load_status = -1, sirius_hid_meta_status = -1;
volatile uint32_t sirius_hid_device_size;

// Hooks NativeHidInput calls.
void native_hid_input_observed(unsigned event, int result) {
    if (event < 8) {
        native_hid_input_events[event] = native_hid_input_events[event] + 1;
        native_hid_input_results[event] = result;
    }
}
// The keyboard device's Home presses; this board publishes only the touch screen.
bool native_hid_input_requested(void) { return false; }
}

static IORegistryEntry *findEventDriver(IORegistryEntry *parent, unsigned depth) {
    if (!depth) return nullptr;
    OSIterator *it = parent->getChildIterator(gIOServicePlane);
    if (!it) return nullptr;
    IORegistryEntry *found = nullptr;
    while (OSObject *child = it->getNextObject()) {
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, child);
        if (!entry) continue;
        if (entry->metaCast("IOHIDEventDriver")) { found = entry; break; }
        if ((found = findEventDriver(entry, depth - 1))) break;
    }
    it->release();
    return found;
}

// NativeHidInput calls this once a second. backboardd classifies a digitizer by Built-In/DisplayIntegrated on the event service
// it opened and drops digitizer events unless that service has PrimaryUsagePage 0x0D and PrimaryUsage 4 (Touch Screen); publish
// them on the IOHIDEventDriver and its interface once (each setProperty is a registry change notification).
extern "C" __attribute__((noinline)) void native_hid_live_checkpoint(IOService *device) {
    static IORegistryEntry *publishedFor;
    IORegistryEntry *driver = findEventDriver(device, 4);
    if (!driver || publishedFor == driver || !device->getProperty("Built-In")) return;
    publishedFor = driver;
    driver->setProperty("Built-In", kOSBooleanTrue);
    driver->setProperty("DisplayIntegrated", kOSBooleanTrue);
    driver->setProperty("PrimaryUsagePage", (unsigned long long)0x0d, 32);
    driver->setProperty("PrimaryUsage", (unsigned long long)4, 32);
    if (IORegistryEntry *interface = driver->getParentEntry(gIOServicePlane)) {
        interface->setProperty("Built-In", kOSBooleanTrue);
        interface->setProperty("DisplayIntegrated", kOSBooleanTrue);
        interface->setProperty("PrimaryUsagePage", (unsigned long long)0x0d, 32);
        interface->setProperty("PrimaryUsage", (unsigned long long)4, 32);
    }
    IOLog("TOUCH hid event driver %s published as the built-in touch screen\n", driver->getMetaClass()->getClassName());
}

static void hidThread(void *, wait_result_t) {
    sirius_hid_stage = 1;
    sirius_hid_load_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if (sirius_hid_load_status == 0)
        OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver", true, false, kOSKextExcludeNone, kOSKextExcludeNone,
            nullptr);
    const OSSymbol *name = OSSymbol::withCString("IOHIDDevice");
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    sirius_hid_device_size = meta ? meta->getClassSize() : 0;
    OSSafeReleaseNULL(name);
    sirius_hid_stage = 2;
    IOService *resource = nullptr;
    OSDictionary *match = IOService::serviceMatching("IOHIDResource");
    if (sirius_hid_load_status == 0 && match) resource = IOService::waitForMatchingService(match, 10000000000ULL);
    OSSafeReleaseNULL(match);
    // NativeHidInput is compiled against the original 208-byte IOHIDDevice (scripts/data/hid-device-abi.json).
    if (!resource || sirius_hid_device_size != 208) {
        IOLog("TOUCH hid not started: IOHIDFamily load=%d IOHIDResource=%d IOHIDDevice size=%u\n", sirius_hid_load_status,
            resource != nullptr, sirius_hid_device_size);
        OSSafeReleaseNULL(resource);
        return;
    }
    sirius_hid_stage = 3;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (handle) {
        native_hid_static_init();
        sirius_hid_meta_status = OSMetaClass::postModLoad(handle);
    }
    bool started = false;
    if (handle && !sirius_hid_meta_status) {
        IOService *touch = native_hid_touch_allocate();
        started = touch && touch->metaCast("IOHIDDevice") && touch->init() && touch->attach(resource) && touch->start(resource);
        if (!started && touch) {
            touch->detach(resource);
            OSSafeReleaseNULL(touch);
        }
        // The device lives for the boot: its registry attachment and work loop stay owned by IOKit.
    }
    sirius_hid_stage = started ? 4 : 5;
    IOLog("TOUCH hid touch screen %s (metaclass status %d)\n", started ? "started on IOHIDResource" : "FAILED to start",
        sirius_hid_meta_status);
    OSSafeReleaseNULL(resource);
}

// Called from the late display probe (package/LegacyDisplayProbe.cpp); sirius-touch-hid=1.
extern "C" void sirius_hid_touch_start(void) {
    unsigned enabled = 0;
    PE_parse_boot_argn("sirius-touch-hid", &enabled, sizeof(enabled));
    if (enabled != 1 || sirius_hid_stage) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(hidThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
