/*
 * UsbBackend.hpp — pembungkus USB modern untuk driver 8188E.
 *
 * Lisensi: GPL-2.0-or-later
 */

#ifndef RTL8188EU_USB_BACKEND_HPP
#define RTL8188EU_USB_BACKEND_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostPipe.h>
#include <IOKit/usb/StandardUSB.h>
#include <stdint.h>

class UsbBackend {
public:
	typedef void (*RxCb)(void *ctx, const uint8_t *data, uint32_t len);
	typedef void (*TxCb)(void *ctx, int status);

	UsbBackend() : fInterface(nullptr), fPipeIn(nullptr), fPipeOut(nullptr),
	    fOpened(false), fRxCb(nullptr), fRxCtx(nullptr), fRxCount(0),
	    fTxOk(0), fTxFail(0) {}

	~UsbBackend() { detach(nullptr); }

	/* Claim interface & buka pipe bulk IN/OUT. */
	bool attach(IOUSBHostInterface *interface);
	void detach(IOService *forClient);

	/* Antre N transfer RX. */
	bool queueRx(uint32_t count, RxCb cb, void *ctx);
	void stopRx();

	/* Kirim satu frame (descriptor + payload). */
	bool send(const uint8_t *data, uint32_t len, TxCb cb, void *ctx);

	/* Lepaskan Xfer setelah completion selesai. */
	static void releaseXfer(void *param);

	uint32_t rxCount() const;
	uint32_t txOk() const;
	uint32_t txFail() const;

	IOUSBHostPipe *pipeIn() const { return fPipeIn; }
	IOUSBHostPipe *pipeOut() const { return fPipeOut; }

	struct Xfer {
		IOBufferMemoryDescriptor *md = nullptr;
		uint8_t *buf = nullptr;
		uint32_t cap = 0;
		bool isRx = false;
		TxCb cb = nullptr;
		void *ctx = nullptr;
	};

public:
	IOUSBHostInterface *fInterface;
	IOUSBHostPipe     *fPipeIn;
	IOUSBHostPipe     *fPipeOut;
	bool               fOpened;

	RxCb     fRxCb;
	void    *fRxCtx;
	uint32_t fRxCount;
	uint32_t fTxOk;
	uint32_t fTxFail;

	/* Kernel tidak punya <vector>; daftar transfer RX dibatasi. */
	struct {
		Xfer   *xfer[8];
		uint32_t count;
	} fRxXfers;
};

#endif /* RTL8188EU_USB_BACKEND_HPP */
