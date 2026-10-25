//
//  IOPCIEDeviceWrapper.cpp — versi USB untuk RTL8188EUS (2357:010c).
//
//  Alur:
//    IOUSBHostInterface (match personality)
//        → IOPCIEDeviceWrapper (kelas ini) membuat ItlRtl8188E
//        → registerService() → AirportItlwm match & menjadi IO80211Controller
//
//  pciNub sengaja nullptr. Frontend sudah dipatch agar melewati blok PCI.
//

#include "IOPCIEDeviceWrapper.hpp"
#include "Apple80211.h"

#include "ItlRtl8188E.hpp"

#define super IOService
OSDefineMetaClassAndStructors(IOPCIEDeviceWrapper, IOService);

#define RTL8188EUS_VID 0x2357
#define RTL8188EUS_PID 0x010c

extern IO80211WorkQueue *_fWorkloop;

bool IOPCIEDeviceWrapper::init()
{
    if (!super::init())
        return false;
    fHalService = NULL;
    pciNub = NULL;
    fInterface = NULL;
    return true;
}

void IOPCIEDeviceWrapper::free()
{
    if (fInterface) {
        fInterface->release();
        fInterface = NULL;
    }
    /* fHalService dimiliki (dan di-release) oleh AirportItlwm. */
    super::free();
}

IOWorkLoop *IOPCIEDeviceWrapper::getWorkLoop() const
{
    return _fWorkloop;
}

IOService* IOPCIEDeviceWrapper::
probe(IOService *provider, SInt32 *score)
{
    IOLog("rtl8188e: IOPCIEDeviceWrapper::probe score=%d\n", score ? (int)*score : 0);
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (!super::probe(provider, score))
        return NULL;

    IOUSBHostInterface *interface = OSDynamicCast(IOUSBHostInterface, provider);
    if (!interface) {
        IOLog("rtl8188e: provider bukan IOUSBHostInterface\n");
        XYLog("%s: provider bukan IOUSBHostInterface\n", __FUNCTION__);
        return NULL;
    }

    OSNumber *vid = OSDynamicCast(OSNumber, interface->getProperty("idVendor"));
    OSNumber *pid = OSDynamicCast(OSNumber, interface->getProperty("idProduct"));
    if (!vid || !pid) {
        IOUSBHostDevice *dev = interface->getDevice();
        if (!dev)
            return NULL;
        vid = OSDynamicCast(OSNumber, dev->getProperty("idVendor"));
        pid = OSDynamicCast(OSNumber, dev->getProperty("idProduct"));
    }
    UInt32 vidVal = vid ? vid->unsigned32BitValue() : 0;
    UInt32 pidVal = pid ? pid->unsigned32BitValue() : 0;
    bool match = (vidVal == RTL8188EUS_VID && pidVal == RTL8188EUS_PID) ||
                 (vidVal == 0x0bda && pidVal == 0x8179);
    if (!match) {
        IOLog("rtl8188e: bukan RTL8188EUS vid=0x%04x pid=0x%04x\n", (unsigned)vidVal, (unsigned)pidVal);
        XYLog("%s: bukan RTL8188EUS (vid/pid tidak cocok)\n", __FUNCTION__);
        return NULL;
    }

    fInterface = interface;
    fInterface->retain();
    pciNub = NULL;

    ItlRtl8188E *hal = new ItlRtl8188E;
    if (!hal || !hal->init()) {
        IOLog("rtl8188e: gagal membuat HAL\n");
        XYLog("%s: gagal membuat HAL\n", __FUNCTION__);
        if (hal) hal->release();
        fInterface->release();
        fInterface = NULL;
        return NULL;
    }
    if (!hal->prepare(interface)) {
        IOLog("rtl8188e: HAL prepare gagal\n");
        XYLog("%s: HAL prepare gagal\n", __FUNCTION__);
        hal->release();
        fInterface->release();
        fInterface = NULL;
        return NULL;
    }
    fHalService = hal;
    IOLog("rtl8188e: IOPCIEDeviceWrapper::probe BERHASIL\n");
    return this;
}

bool IOPCIEDeviceWrapper::
start(IOService *provider)
{
    IOLog("rtl8188e: IOPCIEDeviceWrapper::start!\n");
    XYLog("%s\n", __PRETTY_FUNCTION__);
    _fWorkloop = IO80211WorkQueue::workQueue();
    if (!super::start(provider)) {
        IOLog("rtl8188e: IOPCIEDeviceWrapper super::start gagal\n");
        return false;
    }
    if (!_fWorkloop) {
        IOLog("rtl8188e: IOPCIEDeviceWrapper workloop gagal\n");
        XYLog("%s: workloop gagal\n", __FUNCTION__);
        return false;
    }
    UInt8 builtIn = 0;
    OSData *bi = OSData::withBytes(&builtIn, sizeof(builtIn));
    if (bi) {
        setProperty("built-in", bi);
        bi->release();
    }
    static IOPMPowerState powerStateArray[2] = {
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
    };
    PMinit();
    registerPowerDriver(this, powerStateArray, 2);
    provider->joinPMtree(this);
    registerService();
    IOLog("rtl8188e: IOPCIEDeviceWrapper registered service!\n");
    return true;
}

void IOPCIEDeviceWrapper::
stop(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    PMstop();
    super::stop(provider);
}

IOReturn IOPCIEDeviceWrapper::
setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    return IOPMAckImplied;
}
