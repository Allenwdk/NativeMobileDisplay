#include "NativeLegacyRequest.h"
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the unchanged original generic framebuffer implementation. Unknown
// backend leaves stop; this class is not yet a published system display.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/task.h>
#include <kern/clock.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned,void *,void *);
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *,uint32_t,uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_swap_stage(unsigned);
extern "C" void native_legacy_client_enter(task_t,uint32_t);
extern "C" void native_legacy_client_observed(IOReturn,IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned,IOReturn,unsigned long);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *),native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy);
extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other);
extern "C" void native_legacy_swap_validation_observed(bool,unsigned,const void *,const void *);
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t);
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn,const void *,uint64_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes);
extern "C" void native_legacy_retirement_observed(IOService *);
// GPU hand-off (boot-arg sheng-fb-gpu=1): the frame is offered to the privileged GPU client instead of being copied; the swap retires when the
// client reports the GPU copy done (native_legacy_gpu_complete). A refused offer or a failed copy uses the CPU copy below and is counted.
extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
extern "C" void native_legacy_gpu_fallback_observed(int status);
// Display refresh tick (boot-arg sheng-vsync=<Hz>, 0 or absent: off), the port of the upstream display's tick: the original family
// sends the compositor's vsync message (type 5) through native_legacy_vsync_notify, the unchanged original sender, but only a platform
// subclass calls it from the panel's vblank and that subclass is not part of this port. The GOP scanout has no vblank interrupt, so a
// work-loop timer stands in for it while at least one client has vsync notifications enabled. native_legacy_set_refresh_properties is
// the original setDisplayRefreshProperties: it publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);
extern "C" bool native_legacy_set_refresh_properties(void *display);
// Events: 0 armed, 1 sent, 2 idle (no client), 3 late (deadline passed, cadence restarted), 4 started, 5 refresh properties reported.
extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);
// The GPU probe's answer to "has everything submitted so far retired" (the CP ring drained). Weak: a configuration
// without the probe counts nothing.
extern "C" bool native_gpu_idle(void) __attribute__((weak_import));
// DPU direct scanout (uke-dpu-scanout=3): the display bank stage-1 window from NativeUfs.
extern "C" uint32_t native_ufs_dpu_map(unsigned slot,const uint64_t *pages,unsigned count) __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_dpu_state __attribute__((weak_import));
// Original slot242 and the surface -> Mach-port helper it uses, bound by the
// builder. The helper is called only when the original entitlement gate says
// kIOReturnNotPrivileged.
extern "C" IOReturn uke_original_default_surface_port(void *,task_t,uint32_t *);
extern "C" IOReturn uke_surface_copy_port_name(void *,task_t,void *,uint32_t *);
// The family's IOSurfaceRoot lookup pair (bound by the builder), exactly as swap_submit
// calls them: lookup(root, surfaceId, task) resolves an overlay surface ID the original
// 4-layer loop never reaches.
extern "C" void *uke_surface_root_lookup(void *,uint32_t,void *);
extern "C" void *uke_surface_lookup_task(void);
// 32-bit BGRA, plain ('BGRA') or in Apple's lossless-compressed spelling ('&BGA', one plane): the GPU that renders the surface here writes plain rows
// either way (no compression is implemented), so the bytes are the same.
static inline bool nativeBgra(uint32_t format,uint32_t planes) {
    return (format==0x42475241 && !planes) || (format==0x26424741 && planes<=1);
}
class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay)
    IOMemoryMap *bootMapping=nullptr;
    uint32_t scanBudget=32;
    void *shadow=nullptr;uint64_t shadowBytes=0;bool shadowValid=false;
    bool primary=false;
    bool logicalPM=false;
    bool transferred=false;
    // Every request the validator accepted and the family has not retired yet. A client may submit swap N+1 while swap N is still on the GPU
    // (int-131: swap 3 was validated during swap 2's GPU copy and one remembered pointer lost swap 2, which then never retired and left
    // every wait on it busy), so more than one can be pending.
    const OriginalLegacySwapRequest *validated[8]={};
    bool isValidated(const OriginalLegacySwapRequest *request) const {
        if(!request)return false;
        for(auto entry: validated)if(entry==request)return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request) {for(auto &entry: validated)if(entry==request)entry=nullptr;}
    void addValidated(const OriginalLegacySwapRequest *request) {
        if(isValidated(request))return;
        for(auto &entry: validated)if(!entry){entry=request;return;}
        validated[0]=request;
    }
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
    uint64_t gpuLength=0;
    bool gpuBypass=false;
    // Base-plane presentation (offline-legacy-base-present=1): the compositor renders the base
    // into the default framebuffer surface (the one slot 242 hands the render server) and submits
    // overlay-only transactions whose record layers carry no surfaces, so no swap surface ever
    // validates; presentBase copies that default surface to the GOP instead.
    unsigned baseCopies=0;
    SInt32 baseBusy=0;
    // D1 direct scanout for the compositor's surfaces (uke-dpu-scanout=3): map the IOSurface pages
    // into a slot of the display bank's stage-1 window (native_ufs_dpu_map) and make the live
    // VIG0/VIG1 pipes fetch them. No CPU copy and no GPU copy. Four slots cover the frame being
    // flipped and the ones still on screen; any failure returns to the CPU copy.
    // Two source spellings are scanned here, told apart by the SSPP source format:
    //  - plain 32-bit BGRA, the firmware's own word order (the value read back at start);
    //  - the render server's two-plane 'b3a8' (RGB10A8_2P_XR10) frame, whose colour words are
    //    plane 0 and whose alpha byte plane 1. Plane 0 is exactly a 10-10-10-2 word, which the
    //    VIG pipe unpacks natively as XRGB2101010 (dpu_formats.c: same unpack pattern as ARGB8888,
    //    alpha off and the DX bit on - the SSPP writes bit 14 for DPU_FORMAT_IS_DX and bit 8 only
    //    with alpha_enable). The pipe scans plane 0 alone; the frame's alpha plane is separate and
    //    a composed display frame is opaque.
    IOMemoryMap *mdpMap=nullptr;volatile uint32_t *mdp=nullptr;
    struct DpuSlot {OSObject *surface;IOMemoryDescriptor *memory;uint32_t iova;uint64_t bytes,used;};
    DpuSlot dpuSlots[4]={};uint64_t dpuSlotClock=0;int dpuShownSlot=-1;bool dpuOn=false;
    /* The fence on the direct path. The compositor submits a swap before its GPU work has retired (gpucomp28 flipped the
     * named frame straight away and the panel flickered on every change; gpucomp29 held each frame one swap longer and
     * gpucomp27's counter showed the GPU ring still busy in 98% of flips, so "one frame older" was not one frame
     * complete). A frame is therefore prepared at swap time (slot mapped, format resolved - both may sleep) and flipped
     * from a work-loop timer only once the GPU has retired everything submitted by then (native_gpu_idle, the CP ring
     * drained), so the pipes never scan a surface the GPU is still drawing. A newer frame replaces a frame still waiting;
     * the timer flips anyway after the deadline below, counted, so a ring that never drains cannot freeze the panel. */
    struct DpuFrame {int slot;uint64_t stride;bool tenBit;bool valid;};
    DpuFrame dpuFrame={};
    int dpuPendingSlot=-1;
    IOTimerEventSource *dpuFenceTimer=nullptr;
    IOLock *dpuLock=nullptr;
    uint64_t dpuFenceBegan=0;
    bool dpuGpuPending=false;   // the prepared frame was offered to the scanout client, which reports its GPU work done
    static constexpr uint64_t kDpuFenceDeadlineUs=30000;
    // The scanout client's answer may take longer than a frame when the GPU is loaded (blur passes); waiting is smoother than
    // dropping the frame, which shows up as a skipped image in the middle of an animation.
    static constexpr uint64_t kDpuOfferDeadlineUs=500000;
    uint64_t dpuOfferBegan=0,dpuWaitTotalUs=0,dpuWaitMaxUs=0;
    uint32_t dpuSavedSrc[2]={},dpuSavedStride[2]={},dpuSavedFormat[2]={},dpuSavedUnpack[2]={};
    unsigned tenBitSamples=0;bool tenBitWarned=false;
    // The refresh tick (boot-arg sheng-vsync=<Hz>), the upstream display's tick: a work-loop timer, armed while a client has vsync
    // notifications registered, that sends the family's own vsync message (type 5) through native_legacy_vsync_notify. The timer runs
    // on the family's work loop, the loop of the command gate that registers and removes the clients, so the queue at +5592 never
    // changes while the sender walks it.
    IOTimerEventSource *vsyncTimer=nullptr;
    uint64_t vsyncInterval=0,vsyncNext=0;
    unsigned vsyncHz=0;
    bool vsyncArmed=false;
    // The direct path's counters (P0 measurement): frames flipped by spelling, skips that kept the previous frame on screen, flips that
    // fell back to the CPU decode, and the flush wait's worst and total time.
    uint64_t dpuFlips=0,dpuTenBitFlips=0,dpuSkips=0,dpuFallbacks=0,dpuLatchMaxUs=0,dpuLatchTotalUs=0;
    uint64_t dpuIdleFlips=0,dpuBusyFlips=0;
    uint32_t skipLogged=0;
    // The SSPP source-format word as stock dpu_hw_sspp_setup_format builds it (dpu_formats.c's
    // XRGB2101010: the same unpack pattern as the firmware's ARGB8888, the alpha bit 8 off and
    // the DX bit 14 - the 10-bit unpacker - on).
    uint32_t dpuTenBitFormat(unsigned pipe) const {return (dpuSavedFormat[pipe]&~0x100u)|0x4000u;}
    // XRGB2101010's word order is the firmware pipe's own (B in the low 8 bits, then G, R, A:
    // dpu_formats.c's elements {C1_B_Cb, C0_G_Y, C2_R_Cr, C3_ALPHA} pack to 0x03020001).
    bool dpuTenBitWordOrder() const {return dpuSavedUnpack[0]==0x03020001u && dpuSavedUnpack[1]==0x03020001u;}
    bool dpuStart() {
        if(mdp)return true;
        unsigned mode=0;PE_parse_boot_argn("uke-dpu-scanout",&mode,sizeof(mode));
        if(mode<3)return false;
        IODeviceMemory *memory=IODeviceMemory::withRange(0xae01000,0x8f000);
        mdpMap=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        if(memory)memory->release();
        if(!mdpMap || !mdpMap->getVirtualAddress())return false;
        mdp=reinterpret_cast<volatile uint32_t *>(mdpMap->getVirtualAddress());
        dpuSavedSrc[0]=mdp[(0x4000+0x14)/4];dpuSavedStride[0]=mdp[(0x4000+0x24)/4];
        dpuSavedSrc[1]=mdp[(0x6000+0x14)/4];dpuSavedStride[1]=mdp[(0x6000+0x24)/4];
        dpuSavedFormat[0]=mdp[(0x4000+0x30)/4];dpuSavedUnpack[0]=mdp[(0x4000+0x34)/4];
        dpuSavedFormat[1]=mdp[(0x6000+0x30)/4];dpuSavedUnpack[1]=mdp[(0x6000+0x34)/4];
        dpuLock=IOLockAlloc();
        IOWorkLoop *loop=getWorkLoop();
        dpuFenceTimer=loop?IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::dpuFenceFired):nullptr;
        if(dpuFenceTimer && loop->addEventSource(dpuFenceTimer)!=kIOReturnSuccess) {dpuFenceTimer->release();dpuFenceTimer=nullptr;}
        dpuOn=true;
        IOLog("UKE DPU: direct ready fmt=%08x/%08x unpack=%08x tenbit=%08x fence=%s\n",
            dpuSavedFormat[0],dpuSavedFormat[1],dpuSavedUnpack[0],dpuTenBitFormat(0),dpuFenceTimer?"timer":"none");
        return true;
    }
    void dpuReleaseSlot(unsigned k) {
        DpuSlot &slot=dpuSlots[k];
        if(slot.memory)slot.memory->complete();
        OSSafeReleaseNULL(slot.memory);OSSafeReleaseNULL(slot.surface);
        slot.iova=0;
    }
    void dpuStop() {
        if(!dpuOn)return;
        if(dpuFenceTimer) {dpuFenceTimer->cancelTimeout();if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(dpuFenceTimer);OSSafeReleaseNULL(dpuFenceTimer);}
        dpuFrame.valid=false;dpuPendingSlot=-1;
        if(dpuLock){IOLockFree(dpuLock);dpuLock=nullptr;}
        mdp[(0x4000+0x14)/4]=dpuSavedSrc[0];mdp[(0x6000+0x14)/4]=dpuSavedSrc[1];
        mdp[(0x4000+0x24)/4]=dpuSavedStride[0];mdp[(0x6000+0x24)/4]=dpuSavedStride[1];
        mdp[(0x4000+0x30)/4]=dpuSavedFormat[0];mdp[(0x6000+0x30)/4]=dpuSavedFormat[1];
        mdp[(0x4000+0x34)/4]=dpuSavedUnpack[0];mdp[(0x6000+0x34)/4]=dpuSavedUnpack[1];
        __asm__ volatile("dsb sy":::"memory");mdp[0x15018/4]=0x3;__asm__ volatile("dsb sy":::"memory");
        for(unsigned k=0;k<4;++k)dpuReleaseSlot(k);
        dpuShownSlot=-1;
        dpuOn=false;
    }
    // Maps the pages the pipes fetch, a prefix of the surface for the two-plane spelling (plane 0
    // alone: 27,340,800 bytes of a 32,601,344-byte frame, so the whole descriptor would not fit a
    // 32 MiB slot). Retained while the slot holds the frame.
    int dpuSlot(OSObject *surface,IOMemoryDescriptor *memory,uint64_t needed) {
        ++dpuSlotClock;
        for(unsigned k=0;k<4;++k)
            if(dpuSlots[k].surface==surface && dpuSlots[k].memory==memory && dpuSlots[k].bytes>=needed) {dpuSlots[k].used=dpuSlotClock;return int(k);}
        // Never the slot the pipes are scanning (upstream's directSlot rule): remapping a slot's pages while the pipe
        // fetches from its IOVA makes the panel show a mixture of the old and the new frame for one latch, which is
        // what a burst of new surfaces (an app transition, a blur effect) looked like. The oldest other slot goes.
        int pick=-1;
        for(unsigned k=0;k<4;++k) {
            if(int(k)==dpuShownSlot || int(k)==dpuPendingSlot)continue;
            if(pick<0 || !dpuSlots[k].surface || (dpuSlots[pick].surface && dpuSlots[k].used<dpuSlots[pick].used))pick=int(k);
        }
        if(pick<0)return -1;
        const unsigned k=unsigned(pick);
        dpuReleaseSlot(k);
        if(needed>32ull<<20 || memory->getLength()<needed || memory->prepare()!=kIOReturnSuccess)return -1;
        const unsigned maxPages=static_cast<unsigned>((needed+4095)/4096);
        uint64_t *pages=static_cast<uint64_t *>(IOMallocData(maxPages*sizeof(uint64_t)));
        if(!pages){memory->complete();return -1;}
        unsigned count=0;bool ok=true;
        for(IOByteCount offset=0;ok && offset<needed;) {
            IOByteCount length=0;
            const addr64_t pa=memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            if(!pa || (pa&0xfff) || !length) {ok=false;break;}
            for(IOByteCount at=0;at<length && offset+at<needed;at+=4096)pages[count++]=pa+at;
            offset+=length;
        }
        const uint32_t iova=(ok && count)?native_ufs_dpu_map(k,pages,count):0;
        IOFreeData(pages,maxPages*sizeof(uint64_t));
        if(!iova){memory->complete();return -1;}
        surface->retain();memory->retain();
        dpuSlots[k]={surface,memory,iova,needed,dpuSlotClock};
        IOLog("UKE DPU: direct slot%u iova=%08x pages=%u bytes=%llu\n",k,iova,count,(unsigned long long)needed);
        return int(k);
    }
    // Evidence only (nothing is written): the first two two-plane frames' first colour word, one
    // at the centre, and the alpha plane's byte at the same places, so the layout the pipes fetch
    // is on record.
    void dpuTenBitSample(IOMemoryDescriptor *memory,uint32_t width,uint32_t height) {
        if(tenBitSamples>=2)return;
        const uint64_t row0=(uint64_t)width*4,base1=(uint64_t)width*height*4,center=(height/2ull)*row0+(width/2ull)*4;
        const uint64_t length=memory->getLength();
        if(!width || !height || length<base1+(uint64_t)width*height || length<center+4)return;
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
        if(!source || !source->getVirtualAddress()) {OSSafeReleaseNULL(source);return;}
        const unsigned char *p=reinterpret_cast<const unsigned char *>(source->getVirtualAddress());
        uint32_t first=0,middle=0;unsigned char alpha0=0,alphaMid=0;
        memcpy(&first,p,4);memcpy(&middle,p+center,4);
        alpha0=p[base1];alphaMid=p[base1+(height/2ull)*width+(width/2ull)];
        IOLog("UKE DPU: tenbit sample p0=%08x p0c=%08x alpha=%02x/%02x\n",first,middle,alpha0,alphaMid);
        ++tenBitSamples;
        OSSafeReleaseNULL(source);
    }
    // Prepares a frame for the direct path: maps its slot and resolves the source format (the firmware's own 32-bit
    // word for BGRA, the DX/10-bit unpack of the same word order for the two-plane spelling's colour plane). The flip
    // itself waits for the fence (dpuFenceTick).
    bool dpuPrepareFrame(OSObject *surface,IOMemoryDescriptor *memory,uint64_t sourceStride,
                        uint32_t width,uint32_t height,uint64_t allocation,bool tenBit) {
        if(!dpuOn && !dpuStart())return false;
        if(!&native_ufs_dpu_state || !(native_ufs_dpu_state>>31))return false;
        if(!memory || sourceStride<width*4 || allocation<sourceStride*height)return false;
        if(tenBit && !dpuTenBitWordOrder()) {
            if(!tenBitWarned) {tenBitWarned=true;IOLog("UKE DPU: tenbit refused unpack=%08x/%08x\n",dpuSavedUnpack[0],dpuSavedUnpack[1]);}
            return false;
        }
        const uint64_t needed=sourceStride*height;
        const int k=dpuSlot(surface,memory,needed);
        if(k<0)return false;
        if(!native_legacy_prepare_scanout())return false;
        if(tenBit)dpuTenBitSample(memory,width,height);
        if(dpuLock)IOLockLock(dpuLock);
        dpuFrame={k,sourceStride,tenBit,true};dpuPendingSlot=k;dpuFenceBegan=mach_absolute_time();
        if(dpuLock)IOLockUnlock(dpuLock);
        return true;
    }
    // Flips the prepared frame. Register writes only, so it can run from the work-loop timer.
    void dpuFlipPrepared(int slot,uint64_t stride,bool tenBit) {
        const uint32_t iova=dpuSlots[slot].iova;
        mdp[(0x4000+0x14)/4]=iova;mdp[(0x6000+0x14)/4]=iova;
        mdp[(0x4000+0x24)/4]=uint32_t(stride)&0xffff;mdp[(0x6000+0x24)/4]=uint32_t(stride)&0xffff;
        mdp[(0x4000+0x30)/4]=tenBit?dpuTenBitFormat(0):dpuSavedFormat[0];
        mdp[(0x6000+0x30)/4]=tenBit?dpuTenBitFormat(1):dpuSavedFormat[1];
        mdp[(0x4000+0x34)/4]=dpuSavedUnpack[0];mdp[(0x6000+0x34)/4]=dpuSavedUnpack[1];
        __asm__ volatile("dsb sy":::"memory");
        const uint64_t latchBegan=mach_absolute_time();
        mdp[0x15018/4]=0x3;
        __asm__ volatile("dsb sy":::"memory");
        for(unsigned spin=0;spin<20;++spin){if(!(mdp[0x15018/4]&0x3))break;IODelay(50);}
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-latchBegan,&ns);
        const uint64_t us=ns/1000;
        dpuLatchTotalUs=dpuLatchTotalUs+us;
        if(us>dpuLatchMaxUs)dpuLatchMaxUs=us;
        dpuShownSlot=slot;
        ++dpuFlips;if(tenBit)++dpuTenBitFlips;
        if((dpuFlips%120)==0)
            IOLog("UKE DPU: flip n=%llu tb=%llu skip=%llu fb=%llu latch=%lluus max=%lluus vsync=%u fence=%llu/%llu gpu=%lluus max=%lluus\n",
                (unsigned long long)dpuFlips,(unsigned long long)dpuTenBitFlips,(unsigned long long)dpuSkips,
                (unsigned long long)dpuFallbacks,(unsigned long long)(dpuFlips?dpuLatchTotalUs/dpuFlips:0),
                (unsigned long long)dpuLatchMaxUs,vsyncHz,(unsigned long long)dpuIdleFlips,(unsigned long long)dpuBusyFlips,
                (unsigned long long)(dpuFlips?dpuWaitTotalUs/dpuFlips:0),(unsigned long long)dpuWaitMaxUs);
    }
    static void dpuFenceFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->dpuFenceTick();}
    void dpuFenceTick() {
        if(dpuLock)IOLockLock(dpuLock);
        if(!dpuFrame.valid) {if(dpuLock)IOLockUnlock(dpuLock);return;}
        if(dpuGpuPending) {
            // Offered to the scanout client and still unanswered: drop the frame (the previous one stays up) instead of
            // scanning it unfenced.
            ++dpuBusyFlips;
            dpuGpuPending=false;dpuFrame.valid=false;dpuPendingSlot=-1;
            if(dpuLock)IOLockUnlock(dpuLock);
            return;
        }
        const bool idle=&native_gpu_idle?native_gpu_idle():true;
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-dpuFenceBegan,&ns);
        if(!idle && ns<kDpuFenceDeadlineUs) {
            if(dpuFenceTimer)dpuFenceTimer->setTimeoutUS(500);
            if(dpuLock)IOLockUnlock(dpuLock);
            return;
        }
        if(idle)++dpuIdleFlips; else ++dpuBusyFlips;
        const DpuFrame frame=dpuFrame;
        dpuFrame.valid=false;dpuPendingSlot=-1;
        dpuFlipPrepared(frame.slot,frame.stride,frame.tenBit);
        if(dpuLock)IOLockUnlock(dpuLock);
    }
public:
    // Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop.
    void gpuComplete(int status) {
        struct Args {NativeLegacyDisplay *display;int status;} args={this,status};
        IOWorkLoop *loop=getWorkLoop();
        auto action=[](OSObject *,void *a0,void *,void *,void *)->IOReturn {
            Args *a=static_cast<Args *>(a0);a->display->completeDirect(a->status);return kIOReturnSuccess;
        };
        if(loop)loop->runAction(action,this,&args);else completeDirect(status);
    }
    // The scanout client's answer: the frame's GPU work is done, so the pipes can scan it. A failed or unanswered frame leaves
    // the previous one on screen.
    void completeDirect(int status) {
        if(dpuLock)IOLockLock(dpuLock);
        const bool pending=dpuGpuPending;dpuGpuPending=false;
        if(pending) {
            uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-dpuOfferBegan,&ns);
            const uint64_t us=ns/1000;
            dpuWaitTotalUs=dpuWaitTotalUs+us;
            if(us>dpuWaitMaxUs)dpuWaitMaxUs=us;
        }
        if(pending && dpuFrame.valid) {
            const DpuFrame frame=dpuFrame;
            dpuFrame.valid=false;dpuPendingSlot=-1;
            if(status==0) {
                ++dpuIdleFlips;                 // fenced flips: the client waited for this frame's GPU work
                if(dpuFenceTimer)dpuFenceTimer->cancelTimeout();
                dpuShownSlot=frame.slot;        // protected before the writes, so a prepare cannot evict it
                dpuFlipPrepared(frame.slot,frame.stride,frame.tenBit);
            } else ++dpuBusyFlips;
        }
        if(dpuLock)IOLockUnlock(dpuLock);
        finishGpuSwap(status);
    }
    // A transaction that updates only overlay layers carries no surface the family translates
    // (its per-layer IDs are 0 and the real overlays sit beyond the original 4-layer loop), and
    // the original SwapEnd has already retired the request when this runs; the pixels live in the
    // overlay IOSurface the record names (frameSurfaceId, resolved through the family's own
    // IOSurfaceRoot lookup, which only covers layers 0..3). A transaction with no overlay surface
    // (the compositor's blank/idle updates) presents nothing and keeps the previous frame: the
    // default framebuffer surface is pure black on this compositor and presenting it would blank
    // the panel a few seconds after the first real frame.
    // Called from the framebuffer user-client interpose after the original selector 5 completes,
    // on the compositor's own thread, so the surface cannot be destroyed under the copy.
    void presentBase(unsigned frameSurfaceId) {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-base-present",&enabled,sizeof(enabled));
        if(enabled!=1 || !bootMapping || !primary)return;
        if(!frameSurfaceId)return;
        if(!OSCompareAndSwap(0,1,&baseBusy))return;
        do {
            OSObject *object=nullptr;const char *kind="frame";
            void *root=nullptr;memcpy(&root,reinterpret_cast<unsigned char *>(this)+0x1600,sizeof(root));
            if(root) {
                void *found=uke_surface_root_lookup(root,frameSurfaceId,uke_surface_lookup_task());
                if(found && reinterpret_cast<OSObject *>(found)->metaCast("IOSurface")) {
                    object=reinterpret_cast<OSObject *>(found);
                } else if(baseCopies<2) {
                    IOLog("UKE: frame surface id=%u lookup=%p root=%p\n",(unsigned)frameSurfaceId,found,root);
                }
            }
            // swap_submit's own second rule: a record ID equal to the default
            // surface's ID names fb+0x1618 directly. The adopted surface is not
            // necessarily in the root registry the lookup walks, so keep that
            // rule here too (only for an ID the record named, never a fallback).
            if(!object) {
                OSObject *named=nullptr;memcpy(&named,reinterpret_cast<unsigned char *>(this)+0x1618,sizeof(named));
                uint32_t sid=0;
                if(named)memcpy(&sid,reinterpret_cast<unsigned char *>(named)+0x10,sizeof(sid));
                if(named && sid==frameSurfaceId) {object=named;kind="default";}
                if(baseCopies<2)IOLog("UKE: frame surface id=%u default sid=%u named=%p\n",(unsigned)frameSurfaceId,(unsigned)sid,named);
            }
            if(!object)break;
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            IOMemoryDescriptor *memory=(object && object->metaCast("IOSurface"))?native_legacy_surface_descriptor(object):nullptr;
            uint64_t sourceStride=memory?native_legacy_surface_stride(object):0;
            uint64_t allocation=memory?native_legacy_surface_allocation(object):0;
            uint32_t format=object?native_legacy_surface_format(object):0,planes=object?native_legacy_surface_planes(object):0;
            PE_Video video={};
            // The render server's two-plane 'b3a8' (RGB10A8_2P_XR10) frame keeps its colour words
            // in plane 0, 4 bytes a texel at rows of 4*width bytes, and its alpha byte in plane 1
            // past plane 0's last texel (the layout Metal's GPU reads, metal2vulkan's
            // texture/surface_two_planes.rs); the record's stride field is plane 1's row here
            // (width, 3,200). A composed display frame of that spelling is 5 bytes a texel, which
            // is what the plain-BGRA bounds above cannot describe.
            const bool twoPlane=format==0x62336138 && planes>=2;
            const bool tenBit=twoPlane && memory && (uint64_t)width*height*5<=allocation && allocation<=memory->getLength();
            bool presentable=memory && memory->metaCast("IOMemoryDescriptor") &&
                native_legacy_surface_width(object)>=width && native_legacy_surface_height(object)>=height &&
                (twoPlane ? tenBit : native_legacy_surface_bounds(width,height,sourceStride,allocation,memory->getLength())) &&
                (format==0x42475241 || (format==0x26424741 && planes<=1) || tenBit) &&
                !PE_current_console(&video) && video.v_baseAddr==bootMapping->getPhysicalAddress() &&
                video.v_width==width && video.v_height==height && video.v_depth==32 && !video.v_rotate &&
                native_legacy_surface_bounds(width,height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength());
            if(baseCopies<2)
                IOLog("UKE: %s surface=%p %llux%llu stride=%llu alloc=%llu format=%08x planes=%u mem=%p bytes=%llu presentable=%d\n",
                    kind,object,(unsigned long long)(object?native_legacy_surface_width(object):0),(unsigned long long)(object?native_legacy_surface_height(object):0),
                    (unsigned long long)sourceStride,(unsigned long long)allocation,(unsigned)format,(unsigned)planes,memory,
                    (unsigned long long)(memory?memory->getLength():0),(int)presentable);
            if(!presentable) {
                ++dpuSkips;
                if(skipLogged<8) {
                    ++skipLogged;
                    IOLog("UKE DPU: skip format=%08x planes=%u stride=%llu alloc=%llu bytes=%llu\n",(unsigned)format,(unsigned)planes,
                        (unsigned long long)sourceStride,(unsigned long long)allocation,(unsigned long long)(memory?memory->getLength():0));
                }
                break;
            }
            // The compositor's surfaces are scanned in place: map the pages into the display bank's
            // stage-1 window and flip the live pipes to that IOVA (no CPU copy, no GPU copy). The
            // frame flipped is the previous one: SwapEnd runs before the GPU has necessarily
            // finished writing the current surface, and scanning an in-flight surface is constant
            // flicker. Both spellings travel the one path; the pipes' source format says which.
            if(format==0x42475241 || (format==0x26424741 && planes<=1) || tenBit) {
                const uint64_t stride=tenBit?(uint64_t)width*4:sourceStride;
                if(dpuPrepareFrame(object,memory,stride,width,height,allocation,tenBit)) {
                    ++baseCopies;
                    // The scanout client is the only party that knows when this frame's GPU work finished (the compositor
                    // submits the swap before that), so the frame is offered to it and the flip happens in its completion
                    // (completeDirect). A refused offer keeps the display's own timing; the timer is the watchdog for a
                    // client that goes away mid-frame.
                    bool offered=native_legacy_gpu_offer(this,memory,stride,width,height);
                    if(dpuLock)IOLockLock(dpuLock);
                    dpuGpuPending=offered;dpuOfferBegan=mach_absolute_time();
                    if(dpuLock)IOLockUnlock(dpuLock);
                    if(dpuFenceTimer)dpuFenceTimer->setTimeoutUS(offered?uint32_t(kDpuOfferDeadlineUs):500);
                    if(!offered && !dpuFenceTimer) {
                        if(dpuLock)IOLockLock(dpuLock);
                        const DpuFrame frame=dpuFrame;dpuFrame.valid=false;dpuPendingSlot=-1;
                        dpuFlipPrepared(frame.slot,frame.stride,frame.tenBit);
                        if(dpuLock)IOLockUnlock(dpuLock);
                    }
                    native_legacy_frame_observed(0,0,0,0,0,0);
                    native_legacy_transfer_observed(kIOReturnSuccess,nullptr,(uint64_t)width*height*4);
                    break;
                }
            }
            // Fallback for the two-plane spelling when the direct path is not available (a board
            // without the display bank's window): decode both planes into the scanout on the CPU.
            // The raw pages would be gibberish, and the GPU hand-off and the copy helpers move
            // plain rows.
            if(twoPlane) {
                uint64_t row0=(uint64_t)width*4,row1=width;
                uint64_t base1=(uint64_t)width*height*4;
                if(allocation>=base1+row1*height) {
                    IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
                    uint64_t copyStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyStart));
                    bool ok=source && source->getVirtualAddress() && source->getLength()>=allocation &&
                        native_legacy_prepare_scanout();
                    if(ok) {
                        volatile uint32_t *scan=reinterpret_cast<volatile uint32_t *>(bootMapping->getVirtualAddress());
                        const unsigned char *plane0=reinterpret_cast<const unsigned char *>(source->getVirtualAddress());
                        const unsigned char *plane1=plane0+base1;
                        for(uint32_t y=0;ok && y<height;++y) {
                            const unsigned char *row=plane0+y*row0,*alpha=plane1+y*row1;
                            volatile uint32_t *out=scan+(uint64_t)y*(video.v_rowBytes/4);
                            for(uint32_t x=0;x<width;++x) {
                                uint32_t word;memcpy(&word,row+4*x,4);
                                uint32_t b=(word&0x3ff)>>2,g=((word>>10)&0x3ff)>>2,r=((word>>20)&0x3ff)>>2;
                                out[x]=(uint32_t)alpha[x]<<24|r<<16|g<<8|b;
                            }
                        }
                        __asm__ volatile("dsb sy" ::: "memory");
                    }
                    if(ok) {
                        {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));native_legacy_copy_timing_observed(copyEnd-copyStart,(uint64_t)width*height*4);}
                        ++baseCopies;++dpuFallbacks;
                        native_legacy_frame_observed(0,0,0,0,0,0);
                        native_legacy_transfer_observed(kIOReturnSuccess,nullptr,(uint64_t)width*height*4);
                    } else {
                        native_legacy_transfer_observed(kIOReturnBadArgument,nullptr,0);
                    }
                    OSSafeReleaseNULL(source);
                    break;
                }
            }
            // Upstream's presentation path (sheng-fb-gpu=1): offer the frame to
            // the privileged GPU scanout client, whose GpuScanoutClient blits
            // the surface pages into the scanout on the GPU — no CPU pixel
            // copy. A refused offer (no client, or a frame still in flight)
            // falls back to the CPU copy below, exactly like kickOnce's
            // hand-off; the plugin's completion needs no request to retire.
            if(native_legacy_gpu_offer(this,memory,sourceStride,width,height)) {
                native_legacy_swap_stage(1);
                ++baseCopies;
                native_legacy_frame_observed(0,0,0,0,0,0);
                native_legacy_transfer_observed(kIOReturnSuccess,nullptr,(uint64_t)width*height*4);
                break;
            }
            IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
            if(!source || !source->getVirtualAddress() || source->getLength()<allocation) {
                if(source)native_legacy_transfer_observed(kIOReturnNoMemory,nullptr,0);
                OSSafeReleaseNULL(source);break;
            }
            uint64_t copyStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyStart));
            bool copied=native_legacy_prepare_scanout() &&
                native_legacy_copy_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                    bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                    source->getLength(),sourceStride,width,height);
            if(!copied) {
                native_legacy_transfer_observed(kIOReturnBadArgument,nullptr,0);
                OSSafeReleaseNULL(source);break;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));native_legacy_copy_timing_observed(copyEnd-copyStart,(uint64_t)width*height*4);}
            ++baseCopies;
            if(baseCopies<=2) {
                uint64_t scan[5];
                if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,scan))
                    IOLog("UKE: %s copy %u nonblack=%llu bbox=(%llu,%llu)-(%llu,%llu)\n",kind,baseCopies,
                        (unsigned long long)scan[0],(unsigned long long)scan[1],(unsigned long long)scan[2],
                        (unsigned long long)scan[3],(unsigned long long)scan[4]);
            }
            native_legacy_frame_observed(0,0,0,0,0,0);
            native_legacy_transfer_observed(kIOReturnSuccess,nullptr,(uint64_t)width*height*4);
            OSSafeReleaseNULL(source);
        } while(false);
        baseBusy=0;
    }
    // ---- The refresh tick (ported from the upstream display, GopFramebuffer 33e941a) ----
    bool vsyncWanted() const {
        const void *head=nullptr;memcpy(&head,reinterpret_cast<const unsigned char *>(this)+5592,sizeof(head));return head;
    }
    void armVsync() {
        if(!vsyncTimer || vsyncArmed || !vsyncWanted())return;
        vsyncArmed=true;vsyncNext=mach_absolute_time()+vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);native_legacy_vsync_observed(0,vsyncHz);
    }
    static void vsyncFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();}
    void vsyncTick() {
        vsyncArmed=false;
        if(!vsyncTimer)return;
        if(!vsyncWanted()) {native_legacy_vsync_observed(2,vsyncHz);return;}
        const uint64_t now=mach_absolute_time();
        native_legacy_vsync_notify(this,now,mach_continuous_time(),vsyncInterval);
        native_legacy_vsync_observed(1,vsyncHz);
        vsyncNext+=vsyncInterval;
        if(vsyncNext<=now) {vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
    }
    void startVsync() {
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(!hz || hz>240 || !primary || !bootMapping || vsyncTimer)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::vsyncFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess) {timer->release();return;}
        nanoseconds_to_absolutetime(1000000000ull/hz,&vsyncInterval);
        vsyncHz=hz;vsyncTimer=timer;native_legacy_vsync_observed(4,hz);
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->armVsync();return kIOReturnSuccess;
        },this);
    }
    void stopVsync() {
        if(!vsyncTimer)return;
        vsyncTimer->cancelTimeout();
        if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);vsyncArmed=false;
    }
    // The render server paces its display timer by the minimum refresh interval in IOMFBDisplayRefresh, and the generic slot 301 fixes
    // it at 1/60 s. With sheng-vsync=<Hz> it reports the panel's own rate instead; the maximum interval keeps the original value, or
    // equals the minimum with sheng-vsync-fixed=1 (the panel has no variable refresh). Words in 32.32 s, then their mach times.
    bool getRefreshAttributes(uint64_t *words,bool fixed) override {
        if(!IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed) || !words)return false;
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(hz<=60 || hz>240)return true;
        unsigned locked=0;PE_parse_boot_argn("sheng-vsync-fixed",&locked,sizeof(locked));
        const uint64_t interval=(1ull<<32)/hz;
        uint64_t machTime=0;nanoseconds_to_absolutetime(interval*1000000000ull>>32,&machTime);
        words[0]=interval;words[1]=machTime;
        if(fixed || locked==1){words[2]=interval;words[3]=machTime;}
        native_legacy_vsync_observed(5,hz);
        return true;
    }
    void free() override {stopVsync();if(logicalPM){PMstop();logicalPM=false;}primary=false;OSSafeReleaseNULL(bootMapping);IOMobileFramebufferLegacy::free();}
    void stop(IOService *provider) override {
        stopVsync();
        dpuStop();
        transferred=false;for(auto &entry: validated)entry=nullptr;gpuRequest=nullptr;
        if(logicalPM){PMstop();logicalPM=false;}
        primary=false;IOMobileFramebufferLegacy::stop(provider);OSSafeReleaseNULL(bootMapping);
    }
    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (display+5840,
    // the "powered" half of the state pair next to +5836) is meant to say; the platform subclass that normally writes it is not part of this
    // port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This is the
    // original state setter (virtual 354, the one the original code calls with this same request shape) with only the powered word named.
    void announcePoweredByFirmware() {
        struct StateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];} request={1,0,0,1,1,{}};
        typedef int (*Setter)(void *,StateRequest *,bool);
        Setter setter=reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[354];
        setter(this,&request,false);
    }
    bool start(IOService *provider) override {
        if(!IOMobileFramebufferLegacy::start(provider))return false;
        if(primary && bootMapping)announcePoweredByFirmware();
        if(primary && bootMapping)dpuStart();
        startVsync();
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-pm",&enabled,sizeof(enabled));
        if(!enabled)return true;
        unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
        if(enabled!=1 || phase!=4 || !provider || !bootMapping || !primary) {
            native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;
        }
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
        static IOPMPowerState states[2]={};
        states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        PMinit();logicalPM=initialized;
        if(!logicalPM){native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;}
        provider->joinPMtree(this);
        IOReturn status=registerPowerDriver(this,states,2);native_legacy_pm_observed(1,status,0);
        if(status==kIOReturnSuccess){status=changePowerStateToPriv(1);native_legacy_pm_observed(2,status,1);}
        if(status!=kIOReturnSuccess){PMstop();logicalPM=false;IOMobileFramebufferLegacy::stop(provider);return false;}
        return true;
    }
    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {
        (void)device;
        if(ordinal>1 || !bootMapping){native_legacy_pm_observed(3,kIOReturnUnsupported,ordinal);return kIOReturnUnsupported;}
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        // The panel is already powered by the firmware (GOP) and the original
        // platform subclass/provider is absent.  Do not enter the original
        // power transition: it requests the display rail and resets the tablet.
        native_legacy_pm_observed(4,kIOReturnSuccess,ordinal);
        return kIOReturnSuccess;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options=0) override {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-register",&enabled,sizeof(enabled));
        if(enabled!=1)native_legacy_backend_guard(84,this,__builtin_return_address(0));
        // Nothing in the family publishes IOMFBDisplayRefresh (its platform subclass does), and without it the render server keeps
        // its own 1/60 s heartbeat. Publish it before any client can find the display, only when sheng-vsync asks for more.
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(primary && hz>60 && hz<=240)native_legacy_set_refresh_properties(this);
        IOService::registerService(options);native_legacy_registration_observed(this);
    }
    bool isMainDisplay() override {return primary;}
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking=false,rekick=false;
    void kickSwapQueue() override {
        armVsync();
        if(kicking){rekick=true;return;}
        kicking=true;
        do {
            rekick=false;
            kickOnce();
            gpuBypass=false;
        } while(rekick && !transferred);
        kicking=false;
    }
    // The original SwapEnd gate action walks the active list whose head is at
    // +5496 (requests linked at +2960); +5480 is a different queue and stays
    // empty on this userland. Prefer it, then fall back.
    const OriginalLegacySwapRequest *queuedSwapRequest() const {
        const OriginalLegacySwapRequest *request=nullptr;
        memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5496,sizeof(request));
        if(!request)memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));
        return request;
    }
    void kickOnce() {
        native_legacy_swap_stage(3);
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary)return;
        const OriginalLegacySwapRequest *request=queuedSwapRequest();
        if(!request)return;
        // The family may queue a swap and kick without running slot 296 first;
        // validate the queued request here so the kick does not depend on the
        // absent platform layer's queue thread.
        if(!isValidated(request) && !validateSwap(request))return;
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // The owned GPU instrument completes its work before submission.
        // The separate original software-renderer experiment uses completed
        // synchronous CPU surfaces, with no GPU or display fence fabricated.
        OSObject *surface=nullptr;
        memcpy(&surface,reinterpret_cast<const unsigned char *>(request)+2624,sizeof(surface));
        if(!surface) {
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank={};
            uint32_t bw=0,bh=0;getDisplayDimensions(&bw,&bh);
            uint64_t blankLength=(uint64_t)bw*bh*4;
            if(PE_current_console(&blank) || blank.v_baseAddr!=bootMapping->getPhysicalAddress() ||
               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||
               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            if(!native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),blank.v_rowBytes,bw,bh,0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            shadowValid=false;
            transferred=true;
            native_legacy_frame_observed(1,0,0,0,0,0);
            {const uint32_t colors[4]={0xff000000U,0,0,0};const uint64_t counts[4]={(uint64_t)bw*bh,0,0,0};native_legacy_frame_colors_observed(colors,counts,0);}
            native_legacy_transfer_observed(kIOReturnSuccess,request,blankLength);
            finishTransfer(request);
            return;
        }
        if(!surface->metaCast("IOSurface"))return;
        IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
        PE_Video video={};
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        uint64_t length=(uint64_t)width*height*4;
        uint64_t sourceStride=native_legacy_surface_stride(surface);
        uint64_t allocation=native_legacy_surface_allocation(surface);
        if(!memory || !memory->metaCast("IOMemoryDescriptor") || !native_legacy_surface_bounds(width,height,sourceStride,allocation,memory->getLength()) ||
           native_legacy_surface_width(surface)<width || native_legacy_surface_height(surface)<height ||
           !nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) ||
           PE_current_console(&video) || video.v_baseAddr!=bootMapping->getPhysicalAddress() ||
           video.v_width!=width || video.v_height!=height || video.v_depth!=32 || video.v_rotate ||
           !native_legacy_surface_bounds(width,height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if(!gpuBypass) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
        }
        if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height)) {
            // The GPU writes the scanout memory behind the CPU's back: the differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            shadowValid=false;gpuRequest=request;gpuLength=length;transferred=true;return;
        }
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
        if(!source || !source->getVirtualAddress() || source->getLength()<allocation) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
        }
        if(!native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        // The two content scans read every pixel again; they only describe the first frames (the probe keeps 32).
        if(scanBudget) {
            --scanBudget;
            uint64_t scan[5];
            if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,scan))
                native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
            uint32_t colors[4];uint64_t counts[4],other=0;
            if(native_legacy_frame_colors(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,colors,counts,&other))
                native_legacy_frame_colors_observed(colors,counts,other);
        }
        if(!native_legacy_prepare_scanout()) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        uint64_t copyStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyStart));
        uint64_t stored=UINT64_MAX;
        {
            // Store only what changed since the previous frame (the scanout memory is slow device memory).
            uint64_t need=(uint64_t)width*height*4;
            // Differential copy is opt-in (sheng-fb-diff=1): on hardware it reads the whole source every frame and was not faster
            // than the plain copy (143-285 ms per swap either way), and the first boots with it were followed by an unresponsive
            // lock screen that disappeared with the plain copy (int-005).
            static int diffOff=-1;
            if(diffOff<0){unsigned v=0;PE_parse_boot_argn("sheng-fb-diff",&v,sizeof v);diffOff=(v!=1);}
            if(!diffOff && !shadow && need<=(64ull<<20)){shadow=IOMallocAligned((vm_size_t)need,16384);if(shadow){shadowBytes=need;shadowValid=false;}}
            if(!diffOff && shadow && shadowBytes>=need)
                stored=native_legacy_copy_diff(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                    bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                    source->getLength(),sourceStride,width,height,shadow,shadowValid);
            if(stored!=UINT64_MAX)shadowValid=true;
        }
        if(stored==UINT64_MAX) {
            shadowValid=false;
            if(!native_legacy_copy_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height)) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            stored=length;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));native_legacy_copy_timing_observed(copyEnd-copyStart,stored);}
        OSSafeReleaseNULL(source);transferred=true;
        native_legacy_transfer_observed(kIOReturnSuccess,request,length);
        finishTransfer(request);
    }
    // The GPU client reported its copy (status 0) or lost it. Success retires the swap like a finished copy; anything else uses the CPU copy.
    void finishGpuSwap(int status) {
        const OriginalLegacySwapRequest *request=gpuRequest;
        if(!request)return;
        gpuRequest=nullptr;
        if(status==0) {
            native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            finishTransfer(request,true);return;
        }
        native_legacy_gpu_fallback_observed(status);
        transferred=false;gpuBypass=true;kickSwapQueue();
    }
    // Separately gated retirement after a real scanout store, shared by copied and blank frames.
    void finishTransfer(const OriginalLegacySwapRequest *request,bool force=false) {
        unsigned retire=0,cpuPresent=0;
        PE_parse_boot_argn("offline-legacy-retire",&retire,sizeof(retire));
        PE_parse_boot_argn("offline-legacy-cpu-present",&cpuPresent,sizeof(cpuPresent));
        if(retire==1 || cpuPresent==1 || force) {
            // Only after the real copy and store barrier: select the actual
            // pending head and let the unchanged family release it and wake
            // waiters. This reports memory scanout completion, not a panel
            // vblank or hardware fence. Producer completion must precede
            // submission; this does not implement asynchronous GPU sync.
            if(!activateOnlyPendingSwap(request))return;
            removeValidated(request);
            IOMobileFramebufferLegacy::completeCurrentSwap();
            native_legacy_retirement_observed(this);
            // The unchanged family has retired this generation and released
            // its current reference. Permit the next independently validated
            // single pending frame. Keep the latch set while completion can
            // reenter this method, and whenever retirement remains disabled.
            transferred=false;
            kickSwapQueue();
        }
    }
    IOReturn collectCompletionReports(uint32_t,void *,uint32_t *count,uint32_t) override {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if(count)*count=0;
        return kIOReturnUnsupported;
    }
    bool validateSwap(const OriginalLegacySwapRequest *request) override {
        // Qualified original request fields are packed; use memcpy for each
        // access. Accept only one full-frame, untransformed, nonplanar BGRA
        // surface with real original backing memory. No generic capability
        // or success is supplied for other layer/format/geometry contracts.
        auto bytes=reinterpret_cast<const unsigned char *>(request);
        auto word=[&](unsigned offset){uint32_t value=0;memcpy(&value,bytes+offset,4);return value;};
        OSObject *surface=nullptr;unsigned reason=1;bool accepted=false;
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-validate",&enabled,sizeof(enabled));
        if(enabled==1 && logicalPM && primary && bootMapping && request) {
            reason=2;OSObject *surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));surface=surfaces[0];
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            if(surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
               native_legacy_single_layer_state(word(792),word(796),word(800))) {
                reason=3;bool rectangles=true;
                const unsigned rectOffsets[]={17,81};
                for(unsigned offset: rectOffsets) {
                    uint32_t rect[4]={};memcpy(rect,bytes+offset,sizeof(rect));
                    rectangles=rectangles && rect[0]==0 && rect[1]==0 && rect[2]==width && rect[3]==height;
                }
                bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
                if(width && height && rectangles && options && word(816)==width && word(820)==height) {
                    reason=4;
                    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole display, is copied.
                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       nativeBgra(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(width,height,native_legacy_surface_stride(surface),
                           native_legacy_surface_allocation(surface),memory->getLength())) {
                        reason=0;accepted=true;
                    } else {
                        native_legacy_surface_rejected(native_legacy_surface_width(surface),native_legacy_surface_height(surface),
                            native_legacy_surface_format(surface),native_legacy_surface_planes(surface),native_legacy_surface_stride(surface),
                            native_legacy_surface_allocation(surface),memory?memory->getLength():0);
                    }
                }
            }
        }
        if(!accepted && reason==2 && !surface && enabled==1) {
            // No surface in any slot and every updated layer disabled over black: present a blank screen.
            OSObject *none[4]={};memcpy(none,bytes+2624,sizeof(none));
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
            if(!none[0] && !none[1] && !none[2] && !none[3] && width && height && options &&
               word(816)==width && word(820)==height && native_legacy_blank_state(word(792),word(796),word(800))) {
                reason=5;accepted=true;
            }
        }
        if(accepted)addValidated(request);else removeValidated(request);
        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;
    }
    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch.
    IOReturn getDotPitch(uint32_t *pitch) override {
        if(!pitch)return kIOReturnBadArgument;
        *pitch=0;IOService *provider=getProvider();if(!provider)return kIOReturnNotReady;
        OSObject *property=provider->getProperty("dot-pitch");
        if(!property)return kIOReturnNotFound;
        OSData *data=OSDynamicCast(OSData,property);
        if(!data || data->getLength()!=sizeof(*pitch))return kIOReturnBadArgument;
        memcpy(pitch,data->getBytesNoCopy(),sizeof(*pitch));return kIOReturnSuccess;
    }
    // Selector 74 enumerates optional frame-info trace keys, not display
    // modes. This backend implements no trace-key subscription contract.
    IOReturn supportedFrameInfo(uint32_t,uint32_t *count,char *key,uint32_t *mask) override {
        if(!count || !key || !mask)return kIOReturnBadArgument;
        *count=0;*mask=0;bzero(key,64);return kIOReturnUnsupported;
    }
    IOReturn newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) override {
        native_legacy_client_enter(task,type);
        IOReturn status=IOMobileFramebufferLegacy::newUserClient(task,security,type,client);
        native_legacy_client_observed(status,status==kIOReturnSuccess && client?*client:nullptr);return status;
    }
    // s_default_fb_surface's port step. With PE_i_can_has_debugger()==0 the
    // original slot242 only accepts a task with the explicit-graphics
    // entitlement and otherwise returns kIOReturnNotPrivileged. Use the
    // original surface->port helper directly as the bring-up fallback; it does
    // the same send-right/copyPortNameForObjectInTask translation.
    IOReturn copyDefaultSurfacePortName(task_t task,uint32_t *name) override {
        IOReturn status=uke_original_default_surface_port(this,task,name);
        if(status==kIOReturnNotPrivileged && task && name) {
            void *root=nullptr,*surface=nullptr;
            memcpy(&root,reinterpret_cast<unsigned char *>(this)+0x1600,sizeof(root));
            memcpy(&surface,reinterpret_cast<unsigned char *>(this)+0x1618,sizeof(surface));
            if(root && surface) {
                IOReturn bypass=uke_surface_copy_port_name(root,task,surface,name);
                IOLog("UKE: default surface port fallback root=%p surface=%p status=%08x\n",
                    root,surface,(unsigned)bypass);
                return bypass;
            }
        }
        IOLog("UKE: default surface port status=%08x\n",(unsigned)status);
        return status;
    }
    bool startHardware() override {
        unsigned enabled=0;
        PE_parse_boot_argn("offline-legacy-gop",&enabled,sizeof(enabled));
        if(enabled!=1)return false;
        PE_Video video={};IOService *provider=getProvider();
        if(!provider || PE_current_console(&video) || !video.v_baseAddr ||
           !video.v_width || !video.v_height || video.v_depth!=32 || video.v_rotate ||
           video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4 ||
           video.v_rowBytes>UINT64_MAX/video.v_height)return false;
        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
        if(!mapping)return false;
        bool valid=mapping->getVirtualAddress() && mapping->getPhysicalAddress()==video.v_baseAddr &&
                   mapping->getLength()==video.v_rowBytes*video.v_height;
        if(valid) {
            installBootDimensions((uint32_t)video.v_width,(uint32_t)video.v_height);
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            valid=width==video.v_width && height==video.v_height;
            if(valid) {
                OSSafeReleaseNULL(bootMapping);bootMapping=mapping;mapping=nullptr;primary=video.v_display==1;
                valid=native_legacy_gop_mapping_observed(this,bootMapping,width,height);
            }
        }
        OSSafeReleaseNULL(mapping);return valid;
    }
#define LEAF(n) __attribute__((noreturn,noinline)) void opaque##n() override { native_legacy_backend_guard(n,this,__builtin_return_address(0)); }
    LEAF(211)
    LEAF(245)
    LEAF(246)
    LEAF(306)
    LEAF(307)
    LEAF(338)
    LEAF(345)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)
IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}
extern "C" void native_legacy_present_base(void *display,unsigned frameSurfaceId) {if(display)static_cast<NativeLegacyDisplay *>(display)->presentBase(frameSurfaceId);}
