// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off original HID observation; a separate explicit gate starts an
// owned offline input source. No entitlement or original provider is replaced.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSKext.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" {
extern kern_return_t OSKextLoadKextWithIdentifier(const char *);
volatile uint32_t native_hid_stage,native_hid_resource_found,native_hid_provider_used;
volatile int native_hid_load_status=-1;
volatile int native_hid_matching_load_status=-1;
volatile uint32_t native_hid_class_sizes[5];
volatile int native_hid_input_meta_status=-1;
volatile uint64_t native_hid_input_object;
/* The second IOHIDDevice, created only when sheng-touch-split=1. It is kept because its descriptor
 * is the one that carries Digitizer/Touch Screen: native_touch_set_sink() is a single global slot
 * that the last-started service wins (TouchProbe.cpp:432-439), so the touch reports come from this
 * device, and it is this device whose event service has to be published as the digitizer for
 * backboardd to keep the events. It used to live in a local and was therefore never patched. */
volatile uint64_t native_hid_touch_object;
volatile uint32_t native_hid_input_events[8];
volatile int native_hid_input_results[8]={-1,-1,-1,-1,-1,-1,-1,-1};
volatile uint32_t native_hid_input_command;
char native_hid_input_registry[2048];
char native_hid_input_props[1024];
// Either counter means "the UI has presented", and they disagree by host: the copy-to-scanout path the hardware uses ticks
// transfer_calls, while an offline (CPU-rendered) display only ticks frame_count. Requiring only the first made the mailbox
// unconsumable offline, so the Home press - and with it the scripted link test - could never start.
extern volatile uint32_t native_legacy_transfer_calls, native_legacy_frame_count;
// Optional unattended Home presses for hardware, which has no external harness writing the mailbox:
// offline-hid-auto-home=<seconds after the second presented frame>, offline-hid-auto-home-count (default 1)
// and offline-hid-auto-home-interval (default 20 s). Disabled unless the first argument is set.
static unsigned autoConfigured,autoFirst,autoCount,autoInterval,autoSent;
static uint64_t autoNext,autoFrequency;
static uint64_t counterNow(void){uint64_t v;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(v));return v;}
bool native_hid_input_requested(void) {
    if(native_legacy_transfer_calls<2 && native_legacy_frame_count<2)return false;
    if(!autoConfigured) {
        autoConfigured=1;autoFirst=0;autoCount=1;autoInterval=20;
        PE_parse_boot_argn("offline-hid-auto-home",&autoFirst,sizeof(autoFirst));
        PE_parse_boot_argn("offline-hid-auto-home-count",&autoCount,sizeof(autoCount));
        PE_parse_boot_argn("offline-hid-auto-home-interval",&autoInterval,sizeof(autoInterval));
        if(autoCount>8)autoCount=8;if(autoInterval<5)autoInterval=5;
        __asm__ volatile("mrs %0, cntfrq_el0":"=r"(autoFrequency));
        if(autoFirst && autoFrequency)autoNext=counterNow()+(uint64_t)autoFirst*autoFrequency;
    }
    if(autoFirst && autoFrequency && autoSent<autoCount && counterNow()>=autoNext) {
        ++autoSent;autoNext+=(uint64_t)autoInterval*autoFrequency;return true;
    }
    if(native_hid_input_command!=1)return false;
    native_hid_input_command=0;return true;
}
static void describeInput(IORegistryEntry *entry,unsigned depth,size_t &used) {
    if(depth>4 || used>=sizeof(native_hid_input_registry)-128)return;
    int count=snprintf(native_hid_input_registry+used,sizeof(native_hid_input_registry)-used,
        "%u %s [%s]\n",depth,entry->getName(),entry->getMetaClass()->getClassName());
    if(count<=0 || (size_t)count>=sizeof(native_hid_input_registry)-used)return;
    used+=count;OSIterator *it=entry->getChildIterator(gIOServicePlane);
    if(!it)return;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *next=OSDynamicCast(IORegistryEntry,child);
        if(next)describeInput(next,depth+1,used);
    }
    it->release();
}
// Compact one property of the first IOHIDEventDriver below entry: XML tags dropped, key end -> '=', value end -> ' '.
static IORegistryEntry *findEventDriver(IORegistryEntry *parent,unsigned depth) {
    if(!depth)return nullptr;
    OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
    IORegistryEntry *found=nullptr;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
        if(!entry)continue;
        if(entry->metaCast("IOHIDEventDriver")){found=entry;break;}
        if((found=findEventDriver(entry,depth-1)))break;
    }
    it->release();return found;
}
static void appendProperty(IORegistryEntry *driver,const char *key,const char *label,size_t &used) {
    OSObject *value=driver->copyProperty(key);
    /* snprintf answers the length it would have written, not the length it did write, so advancing `used` by
     * its return value after a truncation walked past the array and the terminator at the end of this function
     * then stored past it. Both writes clamp to what fits and leave one byte for the NUL. */
    {const size_t room=sizeof(native_hid_input_props)-used;
     const int n=snprintf(native_hid_input_props+used,room,"%s:",label);
     if(n>0)used+=(size_t)n<room?(size_t)n:room-1;}
    if(value) {
        OSSerialize *text=OSSerialize::withCapacity(4096);
        if(text && value->serialize(text)) {
            const char *in=text->text();bool tag=false;char name[16];unsigned nameLen=0;
            for(unsigned i=0;in && in[i] && used<sizeof(native_hid_input_props)-2;++i) {
                char c=in[i];
                if(c=='<'){tag=true;nameLen=0;continue;}
                if(tag) {
                    if(c=='>') {
                        tag=false;name[nameLen]=0;
                        if(!strcmp(name,"/key"))native_hid_input_props[used++]='=';
                        else if(name[0]=='/')native_hid_input_props[used++]=' ';
                        else if(!strcmp(name,"true/")||!strcmp(name,"false/")){
                            const size_t room=sizeof(native_hid_input_props)-used;
                            const int wrote=snprintf(native_hid_input_props+used,room,"%s ",name[0]=='t'?"T":"F");
                            if(wrote>0)used+=(size_t)wrote<room?(size_t)wrote:room-1;
                        }
                    } else if(nameLen<sizeof(name)-1)name[nameLen++]=c;
                    continue;
                }
                if(c=='\n'||c=='\t')continue;
                native_hid_input_props[used++]=c;
            }
        }
        OSSafeReleaseNULL(text);OSSafeReleaseNULL(value);
    } else if(used<sizeof(native_hid_input_props)-6) {memcpy(native_hid_input_props+used,"none ",5);used+=5;}
    native_hid_input_props[used<sizeof(native_hid_input_props)?used:sizeof(native_hid_input_props)-1]=0;
}
__attribute__((noinline)) bool native_hid_live_checkpoint(IOService *device) {
    size_t used=0;native_hid_input_registry[0]=0;describeInput(device,0,used);
    if(IORegistryEntry *driver=findEventDriver(device,4)) {
        size_t props=0;native_hid_input_props[0]=0;
        // backboardd's digitizer service classification reads Built-In/DisplayIntegrated from the event service it
        // opened, not from the HID device below it; publish them on the service itself (once per second).
        // Re-read the pair and rewrite it while it is not (0x0d,4), then stop once it reads back as
        // Digitizer/Touch Screen. The one competing writer on this board is the in-kernel
        // IOHIDEventDriver::start(), which publishes the device's own usage pair - (1,6), the keyboard
        // collection listed first - and a driver can attach before that returns, so this check may run
        // earlier than the write it has to survive. That is why one pass is not enough here, whereas
        // the reference board gets away with one write: there the publisher calls this on a one-second
        // timer (UniXNU-alioth package/HidInputProbe.cpp:150-152, "written once per driver"), while
        // here the trigger is the match thread's first observation. A sender whose pair stays (1,6)
        // has every digitizer event dropped as coming from an unsupported service
        // ("ignoring digitizer event ..."), which is the failure this prevents.
        OSNumber *page=OSDynamicCast(OSNumber,driver->getProperty("PrimaryUsagePage"));
        OSNumber *usage=OSDynamicCast(OSNumber,driver->getProperty("PrimaryUsage"));
        const bool digitizerPair=page && usage && page->unsigned32BitValue()==0x0d && usage->unsigned32BitValue()==4;
        static IORegistryEntry *publishedFor;
        if(device->getProperty("Built-In") && (publishedFor!=driver || !digitizerPair)) {
            publishedFor=driver;
            driver->setProperty("Built-In",kOSBooleanTrue);driver->setProperty("DisplayIntegrated",kOSBooleanTrue);
            // backboardd (-[BKHIDDirectTouchEventProcessor processEvent:sender:dispatcher:], decoded from the target) drops a
            // digitizer event unless the sending service has PrimaryUsagePage 0x0D and PrimaryUsage 1, 4 or 0x20
            // ("ignoring digitizer event ... from unsupported service"). The combined device lists its keyboard collection
            // first, so the services would publish (1,6); publish Digitizer/Touch Screen on the services after they have
            // matched (setting it on the device would change driver matching, which is observed to depend on the layout).
            driver->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);driver->setProperty("PrimaryUsage",(unsigned long long)4,32);
            // backboardd's digitizer filter is {DeviceUsagePage, DeviceUsage, Built-In}; make the flag visible on the
            // interface (the event service's provider) as well, not only on the device two levels down.
            if(IORegistryEntry *interface=driver->getParentEntry(gIOServicePlane)) {
                interface->setProperty("Built-In",kOSBooleanTrue);interface->setProperty("DisplayIntegrated",kOSBooleanTrue);
                interface->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);interface->setProperty("PrimaryUsage",(unsigned long long)4,32);
            }
        }
        // The Digitizer/DebugState dumps were removed: nothing on a hardware run reads them without a photograph and
        // they walk driver-internal state from a foreign thread. Three cheap reads stay, and they are the ones the
        // boot's property row needs: Built-In, PrimaryUsagePage and PrimaryUsage (0x0d / 4, the digitizer pair).
        appendProperty(driver,"Built-In","BI",props);
        appendProperty(driver,"PrimaryUsagePage","PP",props);appendProperty(driver,"PrimaryUsage","PU",props);
        __asm__ volatile("" ::: "memory");
        return true;
    }
    /* No driver below means the buffer was not refilled, so the caller must not present whatever it
     * still holds as this device's values. Returning the fact is what makes the row attributable. */
    __asm__ volatile("" ::: "memory");
    return false;
}
void native_hid_input_observed(unsigned event,int result) {
    if(event<8){native_hid_input_events[event]=native_hid_input_events[event]+1;native_hid_input_results[event]=result;}
}
/*
 * The offline IOHIDDevice below is created after boot and registered once, and on some boots nothing
 * ever matches it: NativeHidInput refuses to send a touch report while no IOHIDEventDriver sits below
 * it, so touch is dead with the chip still reporting (2026-11-16).
 *
 * Retrying the registration from the touch reader hangs the boot - registerService runs the service
 * match inline on its caller, and that thread is needed for progress - so the retry lives here on a
 * thread of its own, where blocking costs nothing. It asks every two seconds, re-registers when the
 * answer is no, and gives up after ten tries (~20 s) with one row.
 */
static bool native_hid_event_driver_below(IORegistryEntry *parent, unsigned depth) {
    if (!depth) return false;
    OSIterator *it = parent->getChildIterator(gIOServicePlane);
    if (!it) return false;
    bool found = false;
    while (OSObject *child = it->getNextObject()) {
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, child);
        if (!entry) continue;
        if (entry->metaCast("IOHIDEventDriver") || native_hid_event_driver_below(entry, depth - 1)) {
            found = true;
            break;
        }
    }
    it->release();
    return found;
}

/* The device is pinned for the boot: the success path below never releases the reference
 * native_hid_input_allocate() returned, so the pointer this thread reads cannot be freed under it. A
 * future release path would need a retain here. */
/* Publish the digitizer usage pair once the event service exists, on both match paths below.

 * native_hid_live_checkpoint() is the only code that sets PrimaryUsagePage/PrimaryUsage to
 * (0x0d,4), and until this change nothing called it at all: the pair stayed (1,6), backboardd read
 * the service as a keyboard and dropped every digitizer event it sent. The first desktop boot shows
 * exactly that - 378 touch reports submitted, touch on the panel doing nothing.
 *
 * The reference board calls the same function once a second from its publisher and one pass
 * suffices there. This port takes a few passes because the pair is the only state involved and a
 * pass costs microseconds on this thread; it is not a long race window. dk=0 on this board means
 * the in-kernel IOHIDEventDriver owns the service, so the DriverKit re-publish the older comment
 * reasoned about does not happen here.
 *
 * The closing row is the console evidence. Without it a boot that still has no touch cannot be told
 * apart from a boot where this code never ran, and telling those apart is worth more than the line. */
static void native_hid_publish_pair(IOService *device) {
    IOService *touch = (IOService *)(uintptr_t)native_hid_touch_object;
    for (unsigned tick = 0; tick < 5; ++tick) {
        if (tick) IOSleep(200);
        native_hid_live_checkpoint(device);
        /* The split digitizer carries the reports (the sink is single-slot, last start wins) and its
         * descriptor already lists Digitizer/Touch Screen, so its service is the one backboardd has
         * to accept. Patching only the keyboard device, as this did first, publishes the pair on a
         * service whose descriptor still answers (1,6) - the pair row reads back correct and the
         * events are still dropped. Patch both; the closing row then shows the digitizer's values
         * last when it exists, which is the case worth reading. */
        if (touch && touch != device) native_hid_live_checkpoint(touch);
    }
    /* One row per device. A single row cannot attribute the text: the buffer is cleared only when
     * findEventDriver() succeeds, so a miss leaves the other device's string in place, and a suffix
     * chosen from "does touch exist" would then claim the digitizer for the keyboard's values -
     * exactly the misreading this row exists to prevent. Each row carries its own found flag. */
    if (native_hid_live_checkpoint(device))
        kprintf("HidInputProbe: pair kbd %s\n", native_hid_input_props);
    else
        kprintf("HidInputProbe: pair kbd (no event driver)\n");
    if (touch && touch != device) {
        if (native_hid_live_checkpoint(touch))
            kprintf("HidInputProbe: pair digitizer %s\n", native_hid_input_props);
        else
            kprintf("HidInputProbe: pair digitizer (no event driver)\n");
    } else {
        kprintf("HidInputProbe: no split digitizer (object null or same device)\n");
    }
}

static void native_hid_match_thread(void *, wait_result_t) {
    IORegistryEntry *device = (IORegistryEntry *)(uintptr_t)native_hid_input_object;
    if (!device) return;
    for (unsigned attempt = 1; attempt <= 5; ++attempt) {
        IOSleep(2000);
        if (native_hid_event_driver_below(device, 4) || native_hid_input_results[3] > 0) {
            kprintf("HidInputProbe: event driver attached after %u s\n", attempt * 2);
            native_hid_publish_pair(static_cast<IOService *>(device));
            return;
        }
        /* Asynchronous, not kIOServiceSynchronous: the synchronous form runs the match inline and
         * its doServiceMatch/startMatching loops have no deadline, so one stuck match would keep
         * this thread (and its stack) forever - the same shape that took the boot down when the
         * retry ran on the touch thread. The result is read back below instead of waited for. */
        static_cast<IOService *>(device)->registerService(kIOServiceAsynchronous);
        IOSleep(1000);
        if (native_hid_event_driver_below(device, 4) || native_hid_input_results[3] > 0) {
            kprintf("HidInputProbe: the re-registration attached an event driver (try %u)\n", attempt);
            /* Same publish as above. This is the path the retry loop exists for (a first boot whose
             * device matches nothing), so leaving it without the pair would keep touch dead on
             * exactly the boots the retry rescued. */
            native_hid_publish_pair(static_cast<IOService *>(device));
            return;
        }
    }
    /* One closing row, not one per attempt: a burst of kprintf from a background thread is what has
     * stalled this board before. */
    kprintf("HidInputProbe: no event driver after five re-registrations; touch stays off\n");
}

__attribute__((noinline)) void native_hid_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
void native_hid_input_probe(void) {
    unsigned enabled=0;
    if(!PE_parse_boot_argn("offline-hid-input",&enabled,sizeof(enabled)) || enabled!=1) {
        /* The sixth silent path, and the one a completely row-less boot usually is. Native_hid_stage
         * stays 0 here, so the guard keeps it to one row per boot. */
        if(!native_hid_stage) kprintf("HidInputProbe: skipped (offline-hid-input missing or != 1)\n");
        return;
    }
    if(native_hid_stage)return;
    native_hid_stage=1;
    native_hid_load_status=OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if(!native_hid_load_status)
        native_hid_matching_load_status=OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",
            true,false,kOSKextExcludeNone,kOSKextExcludeNone,nullptr);
    native_hid_stage=2;
    /* Five paths below can leave touch dead without printing a character: a failed family load, a
     * closed gate (no provider, offline-hid-home=0, or a class-size mismatch), a null preModLoad
     * handle, a failed postModLoad, and a failed start(). A boot with no touch then looks exactly
     * like a boot where this probe never ran, which costs a flash cycle each time. These four rows
     * are the whole readout, and they run once per boot behind native_hid_stage. */
    kprintf("HidInputProbe: load=%d match=%d\n", native_hid_load_status, native_hid_matching_load_status);
    const char *names[]={"IOHIDUserDevice","IOHIDEventDriver","IOHIDEventService","IOHIDResourceDeviceUserClient","IOHIDDevice"};
    for(unsigned i=0;i<5;++i) {
        const OSSymbol *name=OSSymbol::withCString(names[i]);
        const OSMetaClass *meta=name?OSMetaClass::getMetaClassWithName(name):nullptr;
        if(meta)native_hid_class_sizes[i]=meta->getClassSize();
        OSSafeReleaseNULL(name);
    }
    if(native_hid_load_status==0) {
        OSDictionary *match=IOService::serviceMatching("IOHIDResource");
        IOService *resource=match?IOService::waitForMatchingService(match,5000000000ULL):nullptr;
        native_hid_resource_found=resource!=nullptr;
        // The provider an offline HID device attaches to. Hardware answers this wait, an offline host does not, and the
        // difference is structural: IOHIDResource is created by an IOHIDFamily *personality* under IOResources, and IOKit
        // probes those personalities only in the single pass its last statement startMatching() makes over IOResources at
        // boot (IOService.cpp:4993 registerService -> :1083 startMatching). This probe loads the kext afterwards, so on a
        // host where nothing else re-kicks that pass the service never exists and every later gate stays dark - which is the
        // readout QEMU gave (resource_found=0, then no sink, no report, no touchSend).
        // IOService::getResourceService() is the official provider for an owned offline service and needs no matching: it
        // returns the IOResources instance IOKit creates at init (IOService.cpp:652), and the reference board already
        // attaches its two other offline services to it (package/KeyStoreProbe.cpp:118, package/LegacyDisplayProbe.cpp:544).
        // Keep the real resource wherever it exists - hardware behaviour is unchanged - and only fall back where it does not.
        IOService *provider=resource;
        if(!provider) provider=IOService::getResourceService();
        native_hid_provider_used=resource?1:(provider?2:0);
        unsigned sendHome=0;PE_parse_boot_argn("offline-hid-home",&sendHome,sizeof(sendHome));
        /* 208 is IOHIDDevice's class size in the donor kernel this port was written against: the offline device
         * below fills an object of that layout, so a kernel whose IOHIDDevice answers another size gets no
         * offline device at all rather than one whose ivars sit at the wrong offsets. The gate is the check -
         * native_hid_class_sizes[4] is the value the loaded family actually reports, and the row prints it, so a
         * mismatched kernel shows the number instead of a silent refusal. */
        static constexpr uint32_t kIOHIDDeviceBytes = 208;
        kprintf("HidInputProbe: res=%u used=%u sz=%u home=%u\n",
                native_hid_resource_found, native_hid_provider_used, native_hid_class_sizes[4], sendHome);
        if(provider && sendHome==1 && native_hid_class_sizes[4]==kIOHIDDeviceBytes) {
            void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
            kprintf("HidInputProbe: preModLoad=%s\n", handle ? "ok" : "null");
            if(handle) {
                native_hid_static_init();native_hid_input_meta_status=OSMetaClass::postModLoad(handle);
                /* Its own row: the start row below only runs when this succeeded, so keeping post=
                 * inside it produced a column that could never read anything but 0. */
                kprintf("HidInputProbe: postModLoad=%d\n", native_hid_input_meta_status);
                if(!native_hid_input_meta_status) {
                    IOService *device=native_hid_input_allocate();
                    native_hid_input_object=(uintptr_t)device;
                    bool valid=device && device->metaCast("IOHIDDevice") && device->init();
                    native_hid_input_observed(0,valid?0:kIOReturnError);
                    bool attached=valid && device->attach(provider);
                    native_hid_input_observed(1,attached?0:kIOReturnError);
                    bool started=attached && device->start(provider);
                    kprintf("HidInputProbe: handle=1 valid=%d att=%d start=%d\n", valid, attached, started);
                    if(!started) {
                        native_hid_input_observed(7,kIOReturnError);
                        if(attached)device->detach(provider);
                        OSSafeReleaseNULL(device);native_hid_input_object=0;
                    }
                    // Attaching only links the parent/child edges; IOHIDResource published the device for driver matching
                    // when it was the provider. Under IOResources nothing does, so publish it the way every other offline
                    // service on the reference board does (package/ShengPlatformPower.cpp:125 registerService(kIOServiceSynchronous)).
                    if(started && !resource) device->registerService(kIOServiceSynchronous);
                    if(started) {
                        thread_t matchThread = nullptr;
                        if (kernel_thread_start(native_hid_match_thread, nullptr, &matchThread) != KERN_SUCCESS) {
                            kprintf("HidInputProbe: could not start the match retry thread\n");
                        } else {
                            thread_deallocate(matchThread);
                        }
                    }
                    unsigned touchHid=0;
                    unsigned split=0;
                    PE_parse_boot_argn("sheng-touch-split",&split,sizeof(split));
                    if(started && split==1 && PE_parse_boot_argn("sheng-touch-hid",&touchHid,sizeof(touchHid)) && touchHid==1) {
                        // Separate digitizer service (primary usage Digitizer/Touch Screen, Built-In).
                        IOService *touch=native_hid_touch_allocate();
                        bool touchStarted=touch && touch->metaCast("IOHIDDevice") && touch->init() && touch->attach(provider) && touch->start(provider);
                        if(touchStarted && !resource) touch->registerService(kIOServiceSynchronous);
                        if(!touchStarted && touch){touch->detach(provider);OSSafeReleaseNULL(touch);}
                        /* Remember it: the reports come from this device (the sink is single-slot and
                         * this one starts last), so this is the device whose event service has to be
                         * published as the digitizer. Leaving it in a local, as this did first, made
                         * the pair land on the keyboard device instead, which backboardd ignores. */
                        if(touchStarted) native_hid_touch_object=(uintptr_t)touch;
                        /* Without this row an absent suffix cannot be told apart from a missing
                         * argument, split!=1, touch-hid!=1 or a failed start. */
                        kprintf("HidInputProbe: split=%u hid=%u touchStarted=%d\n",
                                split, touchHid, touchStarted ? 1 : 0);
                    } else {
                        kprintf("HidInputProbe: split=%u hid=%u (digitizer not created)\n", split, touchHid);
                    }
                    // The offline devices live for this bounded boot. Its
                    // service attachment and timer remain genuine live objects.
                }
            }
        }
        OSSafeReleaseNULL(resource);OSSafeReleaseNULL(match);
    }
    native_hid_stage=3;native_hid_checkpoint();
}
}
