/*
 * UsbBackend — pembungkus USB modern (IOUSBHostInterface / IOUSBHostPipe)
 *              untuk driver 8188E.
 *
 * Menggantikan `IOPCIEDeviceWrapper` dari AirportItlwm (yang PCI-specific).
 * Antarmuka sengaja dibuat mirip supaya adaptasi frontend AirportItlwm
 * (AirportItlwm.cpp) tetap kecil.
 *
 * Yang ditangani:
 *   - claim interface USB (open)
 *   - akses pipe bulk IN/OUT
 *   - transfer asinkron dengan completion
 *   - buffer TX (descriptor + frame 802.11) & RX
 *
 * Aturan keselamatan (dari 2 kernel panic sebelumnya):
 *   - buffer/context transfer di-heap, TIDAK PERNAH di-free di dalam callback
 *   - context di-release SETELAH completion selesai & tidak ada yang memakainya
 *
 * Lisensi: GPL-2.0-or-later
 */

#include "UsbBackend.hpp"
#include <string.h>

/* ---- callback transfer ---- */
static void usbBackendComplete(void *owner, void *param, IOReturn status,
    uint32_t bytes)
{
	UsbBackend *self = (UsbBackend *)owner;
	UsbBackend::Xfer *x = (UsbBackend::Xfer *)param;

	if (!self || !x) {
		return;
	}

	if (x->isRx) {
		if (status == kIOReturnSuccess && bytes > 0 && self->fRxCb) {
			self->fRxCb(self->fRxCtx, x->buf, bytes);
		}
		/* Antre ulang transfer RX. */
		if (self->fPipeIn) {
			IOUSBHostCompletion completion;
			completion.owner = self;
			completion.action = usbBackendComplete;
			completion.parameter = x;
			self->fPipeIn->io(x->md, x->cap, &completion, 0);
		}
	} else {
		if (status == kIOReturnSuccess) {
			self->fTxOk++;
		} else {
			self->fTxFail++;
		}
		if (x->cb) {
			x->cb(x->ctx, status == kIOReturnSuccess ? 0 : -1);
		}
		/* Xfer TX di-release oleh pemilik (setelah callback selesai). */
	}
}

bool UsbBackend::attach(IOUSBHostInterface *interface)
{
	if (!interface) {
		return false;
	}
	fInterface = interface;

	if (!interface->open(nullptr)) {
		fInterface = nullptr;
		return false;
	}
	fOpened = true;

	/* Cari pipe bulk IN & OUT dari deskriptor endpoint. */
	const StandardUSB::ConfigurationDescriptor *cd =
	    interface->getConfigurationDescriptor();
	const StandardUSB::InterfaceDescriptor *id =
	    interface->getInterfaceDescriptor();
	if (!cd || !id) {
		detach(nullptr);
		return false;
	}

	const StandardUSB::EndpointDescriptor *ep = nullptr;
	while ((ep = StandardUSB::getNextEndpointDescriptor(cd, id, ep)) != nullptr) {
		if ((ep->bmAttributes & 0x03) != 0x02) {	/* bulk */
			continue;
		}
		if (ep->bEndpointAddress & 0x80) {
			fPipeIn = interface->copyPipe(ep->bEndpointAddress);
		} else {
			fPipeOut = interface->copyPipe(ep->bEndpointAddress);
		}
	}

	if (!fPipeIn || !fPipeOut) {
		detach(nullptr);
		return false;
	}
	return true;
}

void UsbBackend::detach(IOService *)
{
	stopRx();
	if (fPipeIn) {
		fPipeIn->abort(0, kIOReturnAborted);
		fPipeIn->release();
		fPipeIn = nullptr;
	}
	if (fPipeOut) {
		fPipeOut->abort(0, kIOReturnAborted);
		fPipeOut->release();
		fPipeOut = nullptr;
	}
	if (fOpened && fInterface) {
		fInterface->close(nullptr);
	}
	fOpened = false;
	fInterface = nullptr;
}

bool UsbBackend::queueRx(uint32_t count, RxCb cb, void *ctx)
{
	if (!fPipeIn) {
		return false;
	}
	fRxCb = cb;
	fRxCtx = ctx;

	for (uint32_t i = 0; i < count; i++) {
		Xfer *x = new Xfer();
		if (!x) {
			return false;
		}
		x->isRx = true;
		x->cap = 2048;
		x->md = IOBufferMemoryDescriptor::withCapacity(x->cap,
		    kIODirectionInOut);
		if (!x->md) {
			delete x;
			return false;
		}
		x->buf = (uint8_t *)x->md->getBytesNoCopy();

		IOUSBHostCompletion completion;
		completion.owner = this;
		completion.action = usbBackendComplete;
		completion.parameter = x;

		if (fPipeIn->io(x->md, x->cap, &completion, 0) != kIOReturnSuccess) {
			x->md->release();
			delete x;
			return false;
		}
		if (fRxXfers.count < 8) {
			fRxXfers.xfer[fRxXfers.count++] = x;
		}
	}
	return true;
}

void UsbBackend::stopRx(void)
{
	/* Transfer RX tidak dibatalkan di sini; pipe->abort() yang dipakai.
	 * (kctx & buffer TIDAK di-free di sini — lihat catatan(use-after-free).) */
	fRxCb = nullptr;
	fRxCtx = nullptr;
}

bool UsbBackend::send(const uint8_t *data, uint32_t len, TxCb cb, void *ctx)
{
	if (!fPipeOut || !data || len == 0) {
		return false;
	}

	Xfer *x = new Xfer();
	if (!x) {
		return false;
	}
	x->isRx = false;
	x->cap = len;
	x->md = IOBufferMemoryDescriptor::withCapacity(len, kIODirectionInOut);
	if (!x->md) {
		delete x;
		return false;
	}
	x->buf = (uint8_t *)x->md->getBytesNoCopy();
	memcpy(x->buf, data, len);
	x->cb = cb;
	x->ctx = ctx;

	IOUSBHostCompletion completion;
	completion.owner = this;
	completion.action = usbBackendComplete;
	completion.parameter = x;

	if (fPipeOut->io(x->md, len, &completion, 0) != kIOReturnSuccess) {
		x->md->release();
		delete x;
		fTxFail++;
		return false;
	}
	return true;
}

void UsbBackend::releaseXfer(void *param)
{
	UsbBackend::Xfer *x = (UsbBackend::Xfer *)param;
	if (!x) {
		return;
	}
	if (x->md) {
		x->md->release();
		x->md = nullptr;
	}
	delete x;
}

uint32_t UsbBackend::rxCount() const
{
	return fRxCount;
}

uint32_t UsbBackend::txOk() const
{
	return fTxOk;
}

uint32_t UsbBackend::txFail() const
{
	return fTxFail;
}
