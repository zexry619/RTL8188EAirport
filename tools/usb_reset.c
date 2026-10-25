#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>

int main(void)
{
	CFMutableDictionaryRef match;
	io_iterator_t iter;
	io_service_t svc;
	IOCFPlugInInterface **plug = NULL;
	IOUSBDeviceInterface **dev = NULL;
	SInt32 score;
	kern_return_t kr;
	int vid = 0x2357, pid = 0x010c;

	match = IOServiceMatching(kIOUSBDeviceClassName);
	CFDictionarySetValue(match, CFSTR(kUSBVendorID),
	    CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vid));
	CFDictionarySetValue(match, CFSTR(kUSBProductID),
	    CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid));

	if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS) {
		printf("IOServiceGetMatchingServices fail\n");
		return 1;
	}
	svc = IOIteratorNext(iter);
	IOObjectRelease(iter);
	if (!svc) {
		printf("Device 2357:010c not found\n");
		return 2;
	}

	kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
	    kIOCFPlugInInterfaceID, &plug, &score);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS || !plug) {
		printf("IOCreatePlugInInterfaceForService fail: 0x%x\n", kr);
		return 3;
	}

	kr = (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev);
	(*plug)->Release(plug);
	if (kr != KERN_SUCCESS || !dev) {
		printf("QueryInterface fail: 0x%x\n", kr);
		return 4;
	}

	kr = (*dev)->USBDeviceOpenSeize(dev);
	if (kr != kIOReturnSuccess) {
		kr = (*dev)->USBDeviceOpen(dev);
	}
	if (kr != kIOReturnSuccess) {
		printf("USBDeviceOpen fail: 0x%x\n", kr);
		(*dev)->Release(dev);
		return 5;
	}

	printf("Resetting USB device 2357:010c...\n");
	kr = (*dev)->ResetDevice(dev);
	printf("ResetDevice returned: 0x%x\n", kr);

	/*
	 * Re-enumerate = "cabut-colok" secara software. Diperlukan setelah
	 * driver userspace meninggalkan chip dalam keadaan sudah di-init
	 * (firmware jalan) — driver vendor tidak bisa mengambil alih tanpa
	 * enumerasi ulang.
	 */
	kr = (*dev)->USBDeviceReEnumerate(dev, 0);
	printf("ReEnumerate returned: 0x%x\n", kr);

	(*dev)->USBDeviceClose(dev);
	(*dev)->Release(dev);
	return 0;
}
