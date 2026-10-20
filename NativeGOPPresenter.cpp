// SPDX-License-Identifier: BSD-2-Clause-Patent
// Transfer completed GPU pixels to actual firmware scanout. No CPU rendering,
// fabricated refresh event, Metal discovery or original display publication.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include "NativeGOPWire.h"
extern "C" {
volatile uint32_t native_gop_ready,native_gop_failures;
volatile uint64_t native_gop_transfers,native_gop_bytes,native_gop_physical,native_gop_virtual;
IOService *native_gop_presenter_create(void);
}
class NativeGOPClient;
class NativeGOPPresenter : public IOService {
    OSDeclareDefaultStructors(NativeGOPPresenter)
    friend class NativeGOPClient;
    IOMemoryMap *mapping=nullptr;
    IOLock *gate=nullptr;
    IOService *owner=nullptr;
    NGPMode mode={};
    bool online=false;
    IOReturn transfer(task_t,const NGPPresent &,NGPTransfer *);
public:
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    bool handleOpen(IOService *client,IOOptionBits,void *) override {if(owner)return false;owner=client;return true;}
    void handleClose(IOService *client,IOOptionBits) override {if(owner==client)owner=nullptr;}
    bool handleIsOpen(const IOService *client) const override {return client?owner==client:owner!=nullptr;}
    IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
};
class NativeGOPClient : public IOUserClient {
    OSDeclareDefaultStructors(NativeGOPClient)
    NativeGOPPresenter *controller=nullptr;
    task_t task=TASK_NULL;
    bool opened=false;
    static IOReturn modeAction(OSObject *,void *,IOExternalMethodArguments *);
    static IOReturn presentAction(OSObject *,void *,IOExternalMethodArguments *);
public:
    bool initWithTask(task_t,void *,UInt32,OSDictionary *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    IOReturn clientClose() override {terminate();return kIOReturnSuccess;}
    IOReturn externalMethod(uint32_t,IOExternalMethodArguments *,IOExternalMethodDispatch *,OSObject *,void *) override;
    IOReturn clientMemoryForType(UInt32,IOOptionBits *,IOMemoryDescriptor **) override;
};
OSDefineMetaClassAndStructors(NativeGOPPresenter,IOService)
OSDefineMetaClassAndStructors(NativeGOPClient,IOUserClient)
IOService *native_gop_presenter_create(void) {return new NativeGOPPresenter;}
bool NativeGOPPresenter::start(IOService *provider) {
    if(!IOService::start(provider))return false;
    PE_Video video={};
    if(PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height ||
       video.v_width>16384 || video.v_height>16384 || video.v_depth!=32 || video.v_rotate ||
       video.v_rowBytes<video.v_width*4 || video.v_rowBytes>UINT64_MAX/video.v_height)return false;
    mapping=provider->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
    if(!mapping || !mapping->getVirtualAddress() || mapping->getPhysicalAddress()!=video.v_baseAddr ||
       mapping->getLength()!=video.v_rowBytes*video.v_height)return false;
    gate=IOLockAlloc();if(!gate)return false;
    mode={NGP_VERSION,NGP_BGRX8888,video.v_width,video.v_height,video.v_rowBytes,mapping->getLength()};
    native_gop_physical=mapping->getPhysicalAddress();native_gop_virtual=mapping->getVirtualAddress();
    online=true;native_gop_ready=1;registerService();return true;
}
void NativeGOPPresenter::stop(IOService *provider) {
    if(gate)IOLockLock(gate);online=false;native_gop_ready=0;if(gate)IOLockUnlock(gate);
    IOService::stop(provider);
}
void NativeGOPPresenter::free() {
    OSSafeReleaseNULL(mapping);if(gate){IOLockFree(gate);gate=nullptr;}IOService::free();
}
IOReturn NativeGOPPresenter::transfer(task_t task,const NGPPresent &q,NGPTransfer *result) {
    if(!q.address || q.bytes!=mode.bytes || q.width!=mode.width || q.height!=mode.height ||
       q.stride!=mode.stride || q.format!=mode.format || q.address>UINT64_MAX-q.bytes)return kIOReturnBadArgument;
    IOMemoryDescriptor *input=IOMemoryDescriptor::withAddressRange(q.address,q.bytes,kIODirectionIn,task);
    if(!input)return kIOReturnNoMemory;
    IOReturn status=input->prepare(kIODirectionIn);
    if(status==kIOReturnSuccess) {
        IOLockLock(gate);
        if(!online)status=kIOReturnNotReady;
        else if(input->readBytes(0,reinterpret_cast<void *>(mapping->getVirtualAddress()),q.bytes)!=q.bytes)status=kIOReturnUnderrun;
        else {
            __asm__ volatile("dsb oshst" ::: "memory");
            native_gop_transfers=native_gop_transfers+1;result->sequence=native_gop_transfers;
            result->bytes=q.bytes;native_gop_bytes=native_gop_bytes+q.bytes;
        }
        IOLockUnlock(gate);
        IOReturn completion=input->complete(kIODirectionIn);if(status==kIOReturnSuccess)status=completion;
    }
    input->release();if(status!=kIOReturnSuccess)__atomic_fetch_add(&native_gop_failures,1,__ATOMIC_RELAXED);return status;
}
IOReturn NativeGOPPresenter::newUserClient(task_t task,void *security,UInt32 type,OSDictionary *properties,IOUserClient **out) {
    if(!out || type)return kIOReturnBadArgument;
    *out=nullptr;NativeGOPClient *client=new NativeGOPClient;if(!client)return kIOReturnNoMemory;
    bool attached=false;
    if(!client->initWithTask(task,security,type,properties) || !(attached=client->attach(this)) || !client->start(this)) {
        if(attached)client->detach(this);client->release();return kIOReturnNotOpen;
    }
    *out=client;return kIOReturnSuccess;
}
bool NativeGOPClient::initWithTask(task_t ownerTask,void *security,UInt32 type,OSDictionary *properties) {
    if(!ownerTask || type || clientHasPrivilege(security,kIOClientPrivilegeAdministrator)!=kIOReturnSuccess ||
       !IOUserClient::initWithTask(ownerTask,security,type,properties))return false;
    task=ownerTask;task_reference(task);return true;
}
bool NativeGOPClient::start(IOService *provider) {
    controller=OSDynamicCast(NativeGOPPresenter,provider);
    if(!controller || !IOUserClient::start(provider))return false;
    opened=controller->open(this);return opened;
}
void NativeGOPClient::stop(IOService *provider) {
    if(opened){controller->close(this);opened=false;}controller=nullptr;IOUserClient::stop(provider);
}
void NativeGOPClient::free() {if(task){task_deallocate(task);task=TASK_NULL;}IOUserClient::free();}
IOReturn NativeGOPClient::modeAction(OSObject *target,void *,IOExternalMethodArguments *a) {
    NativeGOPClient *client=OSDynamicCast(NativeGOPClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(a->structureOutputDescriptor || !a->structureOutput)return kIOReturnUnsupported;
    memcpy(a->structureOutput,&client->controller->mode,sizeof(NGPMode));return kIOReturnSuccess;
}
IOReturn NativeGOPClient::presentAction(OSObject *target,void *,IOExternalMethodArguments *a) {
    NativeGOPClient *client=OSDynamicCast(NativeGOPClient,target);
    if(!client || !client->opened || !client->controller)return kIOReturnNotOpen;
    if(a->structureInputDescriptor || a->structureOutputDescriptor || !a->structureInput || !a->structureOutput)return kIOReturnUnsupported;
    NGPPresent q={};NGPTransfer result={};memcpy(&q,a->structureInput,sizeof(q));
    IOReturn status=client->controller->transfer(client->task,q,&result);
    if(status==kIOReturnSuccess)memcpy(a->structureOutput,&result,sizeof(result));return status;
}
IOReturn NativeGOPClient::externalMethod(uint32_t selector,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *) {
    static const IOExternalMethodDispatch methods[]={{modeAction,0,0,0,sizeof(NGPMode)},
                                                    {presentAction,0,sizeof(NGPPresent),0,sizeof(NGPTransfer)}};
    if(selector>=2)return kIOReturnUnsupported;
    return IOUserClient::externalMethod(selector,a,const_cast<IOExternalMethodDispatch *>(methods+selector),this,nullptr);
}
IOReturn NativeGOPClient::clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory) {
    if(type || !options || !memory)return kIOReturnBadArgument;
    *memory=nullptr;if(!opened || !controller || !controller->online)return kIOReturnNotReady;
    *memory=controller->mapping->getMemoryDescriptor();if(!*memory)return kIOReturnNoMemory;
    (*memory)->retain();*options|=kIOMapReadOnly;return kIOReturnSuccess;
}
