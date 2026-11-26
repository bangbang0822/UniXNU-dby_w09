/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_STATUS_H
#define NATIVE_ADRENO_STATUS_H
#include <IOKit/IOTypes.h>
#include <stdint.h>
/* Monotonic fault and suspend counters for Turnip's device_check_status and
 * device_get_suspend_count. Turnip compares the fault count against the value
 * it saw last; a change means the context is lost. This object only counts
 * what the driver tells it: the IRQ path records a fatal decode, the reset or
 * power path records a suspend, and submission poisoning must be recorded by
 * the caller that observed it. It performs no detection, reset or recovery and
 * never clears a count, so a client cannot hide a fault by re-reading. */
struct NativeAdrenoStatusCounts {
    uint64_t faults;
    uint64_t suspends;
};
class NativeAdrenoStatus final {
    uint64_t faults_=0,suspends_=0;
public:
    /* The counts are written from the interrupt and recovery threads and read from a client's Query on another
     * one, so every access is atomic: a plain `++faults_` there is a data race, and a lost increment is a fault
     * Turnip never sees -- the one signal this object exists to deliver. The saturation at UINT64_MAX stays,
     * because a wrapped count reads as a decrease and Turnip only looks for a change. */
    void noteFault() {
        uint64_t current=__atomic_load_n(&faults_,__ATOMIC_RELAXED);
        while(current!=UINT64_MAX &&
              !__atomic_compare_exchange_n(&faults_,&current,current+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    }
    void noteSuspend() {
        uint64_t current=__atomic_load_n(&suspends_,__ATOMIC_RELAXED);
        while(current!=UINT64_MAX &&
              !__atomic_compare_exchange_n(&suspends_,&current,current+1,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) {}
    }
    uint64_t faults() const {return __atomic_load_n(&faults_,__ATOMIC_RELAXED);}
    uint64_t suspends() const {return __atomic_load_n(&suspends_,__ATOMIC_RELAXED);}
    NativeAdrenoStatusCounts counts() const {
        /* One read each: a client that compares the fault count must not see it move between the two cells of
         * the same reply. */
        NativeAdrenoStatusCounts out;
        out.faults=__atomic_load_n(&faults_,__ATOMIC_RELAXED);
        out.suspends=__atomic_load_n(&suspends_,__ATOMIC_RELAXED);
        return out;
    }
};
#endif
