/*
 * usb_compat.cpp — Layer 1: implementasi shim USB.
 *
 * Pemetaan dari OpenBSD (sys/dev/usb):
 *   usbd_do_request()  → IOUSBHostDevice::deviceRequest()
 *   USETW(req.wValue, addr) → wValue ditulis langsung (x86 = little endian)
 *
 * Catatan penting: driver harus "memiliki" device sebelum boleh mengirim
 * control request. Di IOKit modern itu berarti device->open(client).
 */

#include "usb_compat.h"

#include <string.h>

#define LOGP "RTL8188EU: "

int RtwnUsb::attach(IOService *client, IOUSBHostDevice *device,
    IOUSBHostInterface *interface)
{
	if (!client || !device) {
		return RTWN_USB_ERR_ARGS;
	}

	fClient = client;
	fDevice = device;

	/*
	 * Lock untuk control transfer.
	 *
	 * Control transfer memakai SATU memory descriptor persisten (lihat
	 * rtwnCtlTransfer) → harus diserialisasi agar dua transfer tidak
	 * memakai buffer yang sama.
	 */
	if (!fCtlLock) {
		fCtlLock = IOLockAlloc();
	}
	if (!fCtlLock) {
		IOLog(LOGP "usb: gagal membuat lock control transfer\n");
		return RTWN_USB_ERR_ARGS;
	}

	/*
	 * Token "driver masih hidup" (audit 22 Okt 2026, temuan C-1/C-2).
	 *
	 * Alokasi di HEAP dan SENGAJA TIDAK PERNAH di-free — aturan A1.
	 *-token ini yang membuat completion callback bisa tahu dengan aman
	 * apakah driver masih ada, TANPA menyentuh objek driver yang
	 * mungkin sudah di-free.
	 */
	if (!fAlive) {
		fAlive = (bool *)IOMalloc(sizeof(bool));
		if (!fAlive) {
			IOLog(LOGP "usb: gagal alokasi token alive\n");
			return RTWN_USB_ERR_ARGS;
		}
	}
	*fAlive = true;

	/*
	 * Claim device.
	 *
	 * PENTING: open() bisa gagal bila device sudah dibuka driver lain
	 * (mis. legacy shim AppleUSBHostLegacyClient). Itu TIDAK berarti
	 * deviceRequest tidak bisa dipakai — jadi open() gagal hanya
	 * dicatat, tidak digagalkan.
	 */
	if (fDevice->open(fClient)) {
		fOpen = true;
		IOLog(LOGP "usb: device dibuka (open OK)\n");
	} else {
		IOLog(LOGP "usb: open device GAGAL — lanjut tanpa open "
		    "(device mungkin sudah di-claim driver lain)\n");
	}

	/*
	 * Device harus dikonfigurasi sebelum vendor request bisa diproses.
	 *
	 * PERBAIKAN (audit 22 Okt 2026, temuan M-9):
	 * setConfiguration() MENG-RESET device. Kalau device ini sedang
	 * dimiliki driver lain (mis. RtWlanU vendor), merekonfigurasi
	 * device orang lain bisa merusak koneksi Wi-Fi yang sedang jalan
	 * dan membuat driver lain error.
	 *
	 * Jadi: hanya konfigurasi kalau (a) descriptor memang belum ada
	 * DAN (b) kita berhasil memegang device (open OK → kita ownersip).
	 * Kalau open() gagal, kita hanya mencoba vendor request apa adanya
	 * — persis seperti perilaku driver yang sudah berjalan.
	 */
	trace("cek-config");
	if (!fDevice->getConfigurationDescriptor() && fOpen) {
		trace("setConfiguration");
		IOReturn err = fDevice->setConfiguration(1, true);
		if (err == kIOReturnSuccess) {
			trace("setConfiguration-OK");
			IOLog(LOGP "usb: device dikonfigurasi ke configuration #1\n");
		} else {
			trace("setConfiguration-GAGAL");
			IOLog(LOGP "usb: setConfiguration(1) gagal: 0x%08x — "
			    "lanjut tetap\n", err);
		}
	} else {
		const StandardUSB::ConfigurationDescriptor *cd =
		    fDevice->getConfigurationDescriptor();
		trace("sudah-terkonfigurasi");
		IOLog(LOGP "usb: device sudah terkonfigurasi (cfg=%u, iface=%u)\n",
		      cd ? cd->bConfigurationValue : 0,
		      cd ? cd->bNumInterfaces : 0);
	}

	/* Claim interface kalau driver attach ke node interface. */
	if (interface) {
		trace("open-interface");
		/*
		 * Kita pegang reference sendiri terhadap interface.
		 *
		 * PENTING: interface bisa datang dari getNextObject() yang
		 * TIDAK me-retain (kontrak IOKit) — tanpa retain di sini,
		 * pointer bisa dangling setelah iterator di-release
		 * (use-after-free). Route ini harus tetap aman apakah
		 * interface datang dari provider (dipinjam) atau dari
		 * iterator (milik kita).
		 */
		interface->retain();
		fInterface = interface;
		if (interface->open(fClient)) {
			trace("open-interface-OK");
			IOLog(LOGP "usb: interface 0 diklaim (open OK)\n");
		} else {
			trace("open-interface-GAGAL");
			IOLog(LOGP "usb: open interface GAGAL — lanjut tanpa open\n");
		}
	}

	fAttached = true;
	return RTWN_USB_OK;
}

void RtwnUsb::detach()
{
	/*
	 * Urutan penting:
	 *   1. fAttached = false  → tidak ada transfer baru yang dimulai
	 *   2. fBulkClosing = true → completion bulk tidak mengantre ulang
	 *   3. bulkClose()         → abort pipe (completion terakhir keluar)
	 *   4. release interface & device
	 *   5. lepas lock
	 */
	fAttached = false;
	fBulkClosing = true;

	/*
	 * Matikan token "alive" DULUAN — completion yang telat akan
	 * melihat token false dan langsung keluar tanpa menyentuh driver.
	 * Token-nya sendiri TIDAK di-free (aturan A1).
	 */
	if (fAlive) {
		*fAlive = false;
	}

	bulkClose();

	if (fInterface) {
		fInterface->close(fClient);
		fInterface->release();	/* release reference kita sendiri */
		fInterface = nullptr;
	}
	if (fDevice) {
		if (fOpen) {
			fDevice->close(fClient);
		}
		fDevice = nullptr;
		fClient = nullptr;
	}
	fOpen = false;

	if (fCtlLock) {
		IOLockFree(fCtlLock);
		fCtlLock = nullptr;
	}
	if (fBulkLock) {
		IOLockFree(fBulkLock);
		fBulkLock = nullptr;
	}
	fCtlMd = nullptr;
	fCtlBuf = nullptr;
	fCtlCap = 0;
}

/*
 * Control transfer asinkron dengan timeout sendiri.
 *
 * Kenapa tidak pakai varian sinkron (deviceRequest(..., uint32_t& bytesTransferred)):
 * varian itu "acquires the service's workloop lock and calls commandSleep" —
 * kalau perangkat tidak merespons (chip belum di-power-on), thread kita menggantung
 * selamanya. Versi asinkron + timeout sendiri memastikan driver tidak pernah
 * membeku, dan kita tetap tahu persis transfer mana yang gagal.
 */
namespace {
/*
 * Konteks transfer hidup di HEAP dan dimiliki oleh completion callback.
 *
 * PENTING: buffer data TIDAK BOLEH berada di stack, karena transfer bersifat
 * asynchronous — completion bisa dipanggil setelah pemanggil kembali. Versi
 * pertama memakai buffer stack + context stack, dan itu menyebabkan
 * use-after-free → "A kext releasing ... has corrupted the registry" (panic).
 */
struct RtwnCtlCtx {
	volatile bool done;
	IOReturn status;
	uint32_t bytes;
	IOBufferMemoryDescriptor *md;	/* buffer (WAJIB IOMemoryDescriptor) */
	uint8_t *buf;		/* view ke memory descriptor */
	uint32_t len;
};

void rtwnCtlComplete(void *owner, void *param, IOReturn status, uint32_t bytes)
{
	RtwnCtlCtx *ctx = (RtwnCtlCtx *)param;
	if (!ctx) {
		return;
	}
	ctx->status = status;
	ctx->bytes = bytes;
	ctx->done = true;	/* harus aksi terakhir */
}
} // namespace

/*
 * Jalankan satu control transfer dengan batas waktu sendiri.
 *
 * ‑‑‑ Aturan keras (dari 3 kernel panic) ‑‑‑
 *
 *  1. Buffer WAJIB `IOMemoryDescriptor`. Raw pointer → stack USB menimpa
 *     header objek heap → "corrupted the registry" (panic #3).
 *  2. Context transfer TIDAK PERNAH di-free. Stack USB masih bisa menyentuh
 *     buffer setelah completion → free = use-after-free (panic #1 & #2).
 *  3. MD dipakai ULANG (bukan baru tiap transfer). MD dialokasikan
 *     page-granular → ribuan transfer = puluhan MB terbuang.
 *
 * Yang berubah dari versi lama:
 *   - ctx (32 B) tetap bocor → 32 B × ribuan transfer ≈ 100 KB, aman.
 *   - MD-nya persisten & dilindungi IOLock.
 */
int RtwnUsb::rtwnCtlTransfer(bool isInput, uint8_t requestType,
    uint8_t bRequest, uint16_t wValue, void *userBuf, uint32_t len,
    uint32_t *bytesOut)
{
	/* ‑‑‑ Validasi ketat sebelum menyentuh USB ‑‑‑ */
	if (!fDevice || !fOpen || !fCtlLock) {
		return -1;
	}
	if (len == 0 || len > 0xffff) {
		/* wLength adalah uint16_t pada descriptor USB. */
		return -1;
	}
	if (!userBuf) {
		return -1;
	}

	const uint32_t timeoutMs = fTimeoutMs;

	/*
	 * Context transfer: heap, TIDAK PERNAH dibebaskan (aturan #2).
	 * Sengaja bocor — jauh lebih murah daripada kernel panic.
	 */
	RtwnCtlCtx *ctx = (RtwnCtlCtx *)IOMalloc(sizeof(RtwnCtlCtx));
	if (!ctx) {
		return -1;
	}
	ctx->done = false;
	ctx->status = kIOReturnSuccess;
	ctx->bytes = 0;
	ctx->len = len;
	ctx->md = nullptr;
	ctx->buf = nullptr;

	IOUSBHostCompletion completion;
	completion.owner = this;
	completion.action = rtwnCtlComplete;
	completion.parameter = ctx;

	/* Serialisasi: satu transfer pada satu waktu (buffer MD tunggal). */
	IOLockLock(fCtlLock);

	/* ‑‑─ Ambil/persiapkan MD persisten ‑‑─ */
	if (fCtlCap < len) {
		/*
		 * Perbesar. MD lama tidak boleh dipakai transfer apa pun —
		 * lock menjamin tidak ada transfer yang sedang berjalan.
		 */
		if (fCtlMd) {
			fCtlMd->release();
			fCtlMd = nullptr;
			fCtlBuf = nullptr;
			fCtlCap = 0;
		}
		fCtlMd = IOBufferMemoryDescriptor::withCapacity(len,
		    kIODirectionInOut);
		if (fCtlMd) {
			fCtlBuf = (uint8_t *)fCtlMd->getBytesNoCopy();
			fCtlCap = len;
		}
	}
	if (!fCtlMd || !fCtlBuf) {
		IOLockUnlock(fCtlLock);
		return -1;
	}
	ctx->md = fCtlMd;
	ctx->buf = fCtlBuf;

	if (!isInput) {
		memcpy(ctx->buf, userBuf, len);
	} else {
		/* Isi 0xFF agar nilai "tidak terbaca" mudah dikenali. */
		memset(ctx->buf, 0xff, len);
	}

	StandardUSB::DeviceRequest req;
	req.bmRequestType = requestType;
	req.bRequest = bRequest;
	req.wValue = wValue;
	req.wIndex = 0;
	req.wLength = (uint16_t)len;

	IOReturn err;
	if (fInterface) {
		err = fInterface->deviceRequest(req, ctx->md, &completion,
		    timeoutMs);
	} else {
		err = fDevice->deviceRequest(fClient, req, ctx->md,
		    &completion, timeoutMs);
	}
	if (err != kIOReturnSuccess) {
		/*
		 * ctx SENGAJA TIDAK dibebaskan. Stack USB mungkin sudah
		 * memegang ctx->md / ctx->buf.
		 */
		IOLockUnlock(fCtlLock);
		return -1;
	}

	/*
	 * Tunggu completion (stack USB menjamin completion dipanggil saat timeout).
	 * Transfer kontrol USB 2.0 High-Speed selesai dalam ~125–250 µs.
	 * Polling singkat (IODelay 20 µs s/d 1 ms) memangkas waktu per register
	 * dari ~12 ms (IOSleep(10)) menjadi ~0.2 ms (2800 register saat boot:
	 * dari 33.9 detik turun menjadi ~0.6 detik sehingga tidak menahan
	 * IOKitWaitQuiet / WindowServer saat boot).
	 * Bila lewat 1 ms belum selesai, fallback ke IOSleep(1).
	 */
	for (uint32_t us = 0; !ctx->done && us < 1000; us += 20) {
		IODelay(20);
	}
	uint32_t waited = 1;
	while (!ctx->done && waited < timeoutMs + 1500) {
		IOSleep(1);
		waited += 1;
	}
	if (!ctx->done) {
		/* Jangan bebaskan: completion bisa masih datang. Sengaja bocor. */
		IOLockUnlock(fCtlLock);
		return -2;
	}

	IOReturn status = ctx->status;
	uint32_t bytes = ctx->bytes;

	if (isInput && userBuf && status == kIOReturnSuccess && bytes <= len) {
		memcpy(userBuf, ctx->buf, bytes);
	}
	if (bytesOut) {
		*bytesOut = bytes;
	}

	/*
	 * ctx & memory descriptor SENGAJA TIDAK dibebaskan.
	 * Stack USB masih bisa menyentuh buffer setelah completion; membebaskan
	 * = use-after-free = panic. Kebocoran ±32 B per transfer jauh lebih
	 * murah daripada kernel panic. (Aturan A1 — lihat docs/HANDOVER.md)
	 */
	IOLockUnlock(fCtlLock);
	return (status == kIOReturnSuccess) ? 0 : -1;
}

void RtwnUsb::abortAll()
{
	if (fInterface) {
		fInterface->abortDeviceRequests();
	} else if (fDevice) {
		fDevice->abortDeviceRequests(fClient);
	}
}

int RtwnUsb::controlIn(uint8_t bRequest, uint16_t wValue, void *data, uint32_t len)
{
	if (!fDevice || !fOpen) {
		return -1;
	}

	int r = rtwnCtlTransfer(true, RTWN_USB_READ_VENDOR_DEVICE, bRequest,
	    wValue, data, len, nullptr);
	if (r == 0) {
		fInOk++;
		fConsecutiveFails = 0;
		return 0;
	}
	fInFail++;
	fConsecutiveFails++;
	trace(r == -2 ? "ctrl-IN-TIMEOUT" : "ctrl-IN-gagal");
	return r;
}

int RtwnUsb::controlOut(uint8_t bRequest, uint16_t wValue, const void *data,
    uint32_t len)
{
	if (!fDevice || !fOpen) {
		return -1;
	}

	int r = rtwnCtlTransfer(false, RTWN_USB_WRITE_VENDOR_DEVICE, bRequest,
	    wValue, (void *)data, len, nullptr);
	if (r == 0) {
		fOutOk++;
		fConsecutiveFails = 0;
		return 0;
	}
	fOutFail++;
	fConsecutiveFails++;
	trace(r == -2 ? "ctrl-OUT-TIMEOUT" : "ctrl-OUT-gagal");
	return r;
}

/* ══════════════════════════════════════════════════════════════════
 *  Transport bulk (M2)
 * ══════════════════════════════════════════════════════════════════ */

namespace {
/*
 * Context transfer bulk didefinisikan di header (usb_compat.h) supaya tipenya
 * sama dengan anggota RtwnUsb::fTxDone.
 *
 * PENTING: ctx & buffer TIDAK PERNAH di-free di dalam callback. Yang
 * dilakukan hanya adalah menaruh ctx TX ke antrean fTxDone; pembebasan
 * dilakukan oleh pemilik (context driver) — lihat bulkSend().
 */
void rtwnBulkComplete(void *owner, void *param, IOReturn status, uint32_t bytes)
{
	RtwnBulkCtx *ctx = (RtwnBulkCtx *)param;
	if (!ctx) {
		return;
	}

	/*
	 * PEMERIKSAAN PERTAMA — token "alive".
	 *
	 * Token ada di heap & tidak pernah di-free, jadi pembacaan ini
	 * SELALU aman — termasuk setelah driver di-free(). Kalau token
	 * false, driver sudah tidak ada → jangan sentuh apa pun (C-1/C-2).
	 *
	 * PERHATIKAN URUTAN: pemeriksaan ini HARUS sebelum `ctx->self`
	 * dipakai, karena `self` menunjuk ke objek driver yang bisa
	 * sudah dibebaskan.
	 */
	if (!ctx->alive || !*ctx->alive) {
		return;
	}

	RtwnUsb *self = ctx->self;
	if (!self) {
		return;
	}

	/*
	 * PENJAGA RACE (fix baru).
	 *
	 * Completion ini berjalan di thread stack USB. Sementara itu thread
	 * driver bisa memanggil bulkClose() → fPipeIn di-abort & di-release.
	 * Tanpa penjaga, baris di bawah memakai pipe yang sudah dibebaskan
	 * → use-after-free pada objek IOUSBHostPipe.
	 *
	 * fBulkClosing diset TRUE oleh bulkClose() SEBELUM abort, jadi
	 * completion yang telat akan melihatnya dan tidak menyentuh pipe.
	 */
	if (self->fBulkClosing || !self->fAttached) {
		return;
	}

	if (ctx->isRx) {
		if (status == kIOReturnSuccess && bytes > 0) {
			self->fRxCount++;
			if (self->fRxCb) {
				self->fRxCb(self->fRxCtx, ctx->buf, bytes);
			}
		}
		/* Queue ulang transfer RX (memory descriptor tetap dipakai). */
		if (self->fPipeIn && !self->fBulkClosing) {
			IOUSBHostCompletion completion;
			completion.owner = self;
			completion.action = rtwnBulkComplete;
			completion.parameter = ctx;
			self->fPipeIn->io(ctx->md, ctx->cap, &completion, 0);
		}
	} else {
		if (status == kIOReturnSuccess) {
			self->fTxOk++;
		} else {
			self->fTxFail++;
		}
		OSDecrementAtomic(&self->fTxInFlight);
		if (ctx->txCb) {
			ctx->txCb(ctx->txCtx, status == kIOReturnSuccess ? 0 : -1);
		}
		if (self->fTxInFlight <= 24 && self->fTxDoneCb) {
			self->fTxDoneCb(self->fTxDoneCtx);
		}
		/*
		 * ATURAN A1: TIDAK membebaskan ctx/md di sini.
		 * Completion masih berada di dalam stack USB; membebaskan
		 * buffer saat ini = use-after-free = kernel panic.
		 *
		 * Yang dilakukan hanya: taruh di antrean. Daur ulang/pembebasan
		 * dilakukan bulkSend() (context driver, di luar callback).
		 */
		if (self->fBulkLock) {
			IOLockLock(self->fBulkLock);
			ctx->next = self->fTxDone;
			self->fTxDone = ctx;
			IOLockUnlock(self->fBulkLock);
		}
	}
}
} // namespace

/*
 * Lepaskan ctx + buffer milik sebuah transfer. Dipanggil SETELAH completion
 * selesai dan tidak ada lagi yang memakainya.
 */
void RtwnUsb::bulkRelease(void *param)
{
	RtwnBulkCtx *ctx = (RtwnBulkCtx *)param;
	if (!ctx) {
		return;
	}
	if (ctx->md) {
		ctx->md->release();
		ctx->md = nullptr;
	}
	ctx->buf = nullptr;
	IOFree(ctx, sizeof(RtwnBulkCtx));
}

bool RtwnUsb::bulkOpen(IOUSBHostInterface *interface)
{
	if (!interface || !fOpen) {
		return false;
	}

	/* Pipeline aktif kembali. */
	fBulkClosing = false;

	if (!fBulkLock) {
		fBulkLock = IOLockAlloc();
	}

	/* Pipe bulk IN (RX) & OUT (TX): cari dari deskriptor endpoint. */
	const StandardUSB::ConfigurationDescriptor *cd =
	    interface->getConfigurationDescriptor();
	const StandardUSB::InterfaceDescriptor *id =
	    interface->getInterfaceDescriptor();
	if (!cd || !id) {
		IOLog(LOGP "usb: descriptor interface tidak tersedia\n");
		return false;
	}

	const StandardUSB::EndpointDescriptor *ep = nullptr;
	fNumOut = 0;
	while ((ep = StandardUSB::getNextEndpointDescriptor(cd, id, ep)) != nullptr) {
		uint8_t type = ep->bmAttributes & 0x03;
		if (type != 0x02) {		/* bulk */
			continue;
		}
		if (ep->bEndpointAddress & 0x80) {	/* IN */
			fPipeIn = interface->copyPipe(ep->bEndpointAddress);
		} else if (fNumOut < 3) {
			/*
			 * 8188EU punya sampai 3 bulk-out. Urutannya menentukan
			 * queue: 0 = VO/mgmt, 1 = VI, 2 = BE/BK. Frame data
			 * WAJIB lewat pipe yang benar — kalau tidak, chip
			 * memasukkannya ke queue mgmt dan frame tidak pernah
			 * terkirim sebagai data.
			 */
			fPipeOutAll[fNumOut] =
			    interface->copyPipe(ep->bEndpointAddress);
			if (fPipeOutAll[fNumOut]) {
				fNumOut++;
			}
		}
	}
	fPipeOut = fNumOut > 0 ? fPipeOutAll[0] : nullptr;

	if (!fPipeIn || !fPipeOut) {
		IOLog(LOGP "usb: pipe bulk tidak lengkap (in=%p out=%p)\n",
		    (void *)fPipeIn, (void *)fPipeOut);
		if (fPipeIn) { fPipeIn->release(); fPipeIn = nullptr; }
		for (int i = 0; i < fNumOut; i++) {
			if (fPipeOutAll[i]) {
				fPipeOutAll[i]->release();
				fPipeOutAll[i] = nullptr;
			}
		}
		fNumOut = 0;
		fPipeOut = nullptr;
		return false;
	}

	IOLog(LOGP "usb: pipe bulk IN=%p OUT=%d (ep0=%p) siap\n",
	    (void *)fPipeIn, fNumOut, (void *)fPipeOut);
	fTxInFlight = 0;
	for (int i = 0; i < 3; i++) {
		fPipeStalled[i] = false;
	}
	return true;
}

void RtwnUsb::bulkClose()
{
	/*
	 * PENJAGA RACE: set flag SEBELUM abort.
	 * Completion yang sedang/telah dijadwalkan akan melihat flag ini
	 * dan tidak akan memakai pipe yang sebentar lagi dibebaskan.
	 */
	fBulkClosing = true;
	fTxInFlight = 0;

	if (fPipeIn) {
		fPipeIn->abort(0, kIOReturnAborted);
		fPipeIn->release();
		fPipeIn = nullptr;
	}
	for (int i = 0; i < fNumOut; i++) {
		if (fPipeOutAll[i]) {
			fPipeOutAll[i]->abort(0, kIOReturnAborted);
			fPipeOutAll[i]->release();
			fPipeOutAll[i] = nullptr;
		}
	}
	fNumOut = 0;
	fPipeOut = nullptr;

	if (fBulkLock) {
		IOLockLock(fBulkLock);
		RtwnBulkCtx *done = fTxDone;
		fTxDone = nullptr;
		IOLockUnlock(fBulkLock);
		while (done) {
			RtwnBulkCtx *nx = done->next;
			if (done->md) {
				done->md->release();
			}
			IOFree(done, sizeof(RtwnBulkCtx));
			done = nx;
		}
	}
}

bool RtwnUsb::bulkQueueRx(uint32_t count)
{
	if (!fPipeIn || fBulkClosing || !fAttached) {
		return false;
	}

	for (uint32_t i = 0; i < count; i++) {
		RtwnBulkCtx *ctx = (RtwnBulkCtx *)IOMalloc(sizeof(RtwnBulkCtx));
		if (!ctx) {
			return false;
		}
		memset(ctx, 0, sizeof(RtwnBulkCtx));
		const uint32_t cap = 16384;	/* cukup untuk frame besar + agregasi */
		ctx->md = IOBufferMemoryDescriptor::withCapacity(cap, kIODirectionInOut);
		if (!ctx->md) {
			IOFree(ctx, sizeof(RtwnBulkCtx));
			return false;
		}
		ctx->buf = (uint8_t *)ctx->md->getBytesNoCopy();
		ctx->self = this;
		ctx->pipe = fPipeIn;
		ctx->cap = cap;
		ctx->isRx = true;
		ctx->alive = fAlive;

		IOUSBHostCompletion completion;
		completion.owner = this;
		completion.action = rtwnBulkComplete;
		completion.parameter = ctx;

		if (fPipeIn->io(ctx->md, cap, &completion, 0) != kIOReturnSuccess) {
			ctx->md->release();
			IOFree(ctx, sizeof(RtwnBulkCtx));
			return false;
		}
	}
	return true;
}

bool RtwnUsb::bulkSend(const uint8_t *data, uint32_t len, rtwn_tx_cb cb,
    void *ctx, int pipeIdx)
{
	IOUSBHostPipe *pipe;

	if (!data || len == 0 || fBulkClosing || !fAttached || fNumOut <= 0) {
		return false;
	}
	if (fTxInFlight >= 48) {
		return false;
	}
	if (pipeIdx < 0) {
		pipeIdx = 0;
	}
	if (pipeIdx >= fNumOut) {
		pipeIdx = fNumOut - 1;
	}
	pipe = fPipeOutAll[pipeIdx];
	if (!pipe) {
		return false;
	}

	/*
	 * Daur ulang context TX + 4KB IOBufferMemoryDescriptor dari fTxDone
	 * (menghilangkan alokasi halaman VM kernel per paket).
	 *
	 * Aman dilakukan di sini: kita berada di context driver (bukan
	 * completion callback), sepenuhnya mematuhi Aturan A1.
	 */
	RtwnBulkCtx *c = nullptr;
	if (fBulkLock) {
		IOLockLock(fBulkLock);
		if (fTxDone) {
			c = fTxDone;
			fTxDone = c->next;
			c->next = nullptr;
		}
		IOLockUnlock(fBulkLock);
	}
	if (c && (!c->md || !c->buf || c->cap < len)) {
		if (c->md) {
			c->md->release();
		}
		IOFree(c, sizeof(RtwnBulkCtx));
		c = nullptr;
	}
	if (!c) {
		uint32_t allocCap = (len > 4096) ? len : 4096;
		c = (RtwnBulkCtx *)IOMalloc(sizeof(RtwnBulkCtx));
		if (!c) {
			return false;
		}
		memset(c, 0, sizeof(RtwnBulkCtx));
		c->md = IOBufferMemoryDescriptor::withCapacity(allocCap,
		    kIODirectionInOut);
		if (!c->md) {
			IOFree(c, sizeof(RtwnBulkCtx));
			return false;
		}
		c->buf = (uint8_t *)c->md->getBytesNoCopy();
		c->cap = allocCap;
	}
	memcpy(c->buf, data, len);
	c->self = this;
	c->pipe = pipe;
	c->isRx = false;
	c->alive = fAlive;
	c->txCb = cb;
	c->txCtx = ctx;
	c->len = len;
	c->next = nullptr;

	IOUSBHostCompletion completion;
	completion.owner = this;
	completion.action = rtwnBulkComplete;
	completion.parameter = c;

	if (pipe->io(c->md, len, &completion, 0) != kIOReturnSuccess) {
		/* Gagal antre → tidak ada completion yang akan datang,
		 * kembalikan ke pool fTxDone atau bebaskan bila lock tiada. */
		if (fBulkLock) {
			IOLockLock(fBulkLock);
			c->next = fTxDone;
			fTxDone = c;
			IOLockUnlock(fBulkLock);
		} else {
			c->md->release();
			IOFree(c, sizeof(RtwnBulkCtx));
		}
		fTxFail++;
		return false;
	}
	OSIncrementAtomic(&fTxInFlight);
	return true;
}
