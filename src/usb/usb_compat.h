/*
 * usb_compat.h — Layer 1: shim transport USB (OpenBSD usbd_* → IOKit).
 *
 * Hanya control transfer (vendor request) yang dibutuhkan untuk Milestone M1:
 * akses register, baca EFUSE, dan upload firmware. Bulk pipe (TX/RX data)
 * menyusul di M2.
 *
 * Lisensi: GPL-2.0-or-later (lihat ../../LICENSES.md)
 */

#ifndef RTL8188EU_USB_COMPAT_H
#define RTL8188EU_USB_COMPAT_H

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLib.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/StandardUSB.h>

/* bmRequestType untuk vendor request ke device (USB 2.0 §9.3). */
#define RTWN_USB_WRITE_VENDOR_DEVICE 0x40 /* host -> device */
#define RTWN_USB_READ_VENDOR_DEVICE  0xc0 /* device -> host */

/* Callback untuk melacak tiap langkah (dipakai driver untuk menulis ke registry). */
typedef void (*RtwnTraceFn)(void *ctx, const char *msg);

/* Callback transport bulk. */
typedef void (*rtwn_rx_cb)(void *ctx, const uint8_t *data, uint32_t len);
typedef void (*rtwn_tx_cb)(void *ctx, int status);
typedef void (*rtwn_tx_done_cb)(void *ctx);

class RtwnUsb;

/*
 * Context satu transfer bulk.
 *
 * ATURAN: objek ini TIDAK PERNAH dibebaskan di dalam completion callback.
 * Completion hanya menaruhnya di antrean; pemilik (context driver) yang
 * membebaskannya. (Aturan A1 — docs/HANDOVER.md)
 */
struct RtwnBulkCtx {
	RtwnUsb       *self;
	IOUSBHostPipe *pipe;
	IOBufferMemoryDescriptor *md;	/* buffer milik transfer */
	uint8_t       *buf;		/* view ke memory descriptor */
	uint32_t       cap;
	bool           isRx;
	rtwn_rx_cb     rxCb;
	void          *rxCtx;
	rtwn_tx_cb     txCb;
	void          *txCtx;
	uint32_t       len;
	RtwnBulkCtx   *next;		/* antrean context TX yang selesai */

	/*
	 * Token "driver masih hidup".
	 *
	 * PENTING (audit 22 Okt 2026, temuan C-1/C-2):
	 * completion callback berjalan di thread stack USB dan bisa
	 * arrive SETELAH driver di-free(). Menaruh penanda "masih hidup"
	 * di dalam objek driver tidak berguna — membacanya sendiri sudah
	 * use-after-free.
	 *
	 * Solusi: token di HEAP, TIDAK PERNAH di-free (aturan A1: leak
	 * jauh lebih murah daripada panic). Jadi callback SELALU bisa
	 * membaca token dengan aman, dan hanya menyentuh driver kalau
	 * token masih true.
	 */
	bool          *alive;
};

class RtwnUsb {
public:
	void setTrace(RtwnTraceFn fn, void *ctx) { fTrace = fn; fTraceCtx = ctx; }

	RtwnUsb() : fPipeIn(nullptr), fPipeOut(nullptr), fNumOut(0),
	    fRxCb(nullptr), fRxCtx(nullptr), fRxCount(0), fTxOk(0), fTxFail(0),
	    fTxInFlight(0), fTxDoneCb(nullptr), fTxDoneCtx(nullptr),
	    fBulkClosing(false), fAttached(false), fTxDone(nullptr),
	    fBulkLock(nullptr), fAlive(nullptr),
	    fCtlLock(nullptr), fCtlMd(nullptr), fCtlBuf(nullptr), fCtlCap(0),
	    fTimeoutMs(250), fMaxFails(12), fConsecutiveFails(0),
	    fInOk(0), fInFail(0), fOutOk(0), fOutFail(0),
	    fTrace(nullptr), fTraceCtx(nullptr),
	    fDevice(nullptr), fInterface(nullptr), fClient(nullptr), fOpen(false) {
		fPipeOutAll[0] = fPipeOutAll[1] = fPipeOutAll[2] = nullptr;
		fPipeStalled[0] = false;
		fPipeStalled[1] = false;
		fPipeStalled[2] = false;
	}

	/* Status attach (untuk diagnostik). */
	enum {
		RTWN_USB_OK        = 0,
		RTWN_USB_ERR_OPEN  = 1,	/* open device gagal */
		RTWN_USB_ERR_CFG   = 2,	/* setConfiguration gagal */
		RTWN_USB_ERR_IFACE = 3,	/* open interface gagal */
		RTWN_USB_ERR_ARGS  = 4
	};

	/* Claim device (+ interface bila ada), pastikan konfigurasi aktif. */
	int attach(IOService *client, IOUSBHostDevice *device,
	    IOUSBHostInterface *interface = nullptr);
	void detach();

	bool isOpen() const { return fOpen; }

	/* Penghitung transfer ( diagnostik). */
	uint32_t inOk() const { return fInOk; }
	uint32_t inFail() const { return fInFail; }
	uint32_t outOk() const { return fOutOk; }
	uint32_t outFail() const { return fOutFail; }

	/* Berapa kali berturut-turut gagal sebelum transfer dianggap buntu. */
	void setMaxConsecutiveFailures(uint32_t n) { fMaxFails = n; }
	uint32_t consecutiveFailures() const { return fConsecutiveFails; }
	void resetFailures() { fConsecutiveFails = 0; }

	/* Timeout control transfer (ms). */
	void setTimeoutMs(uint32_t ms) { fTimeoutMs = ms; }
	uint32_t timeoutMs() const { return fTimeoutMs; }

	/* Batalkan semua request yang sedang menggantung. */
	void abortAll();

	/* ── Transport bulk (M2) ───────────────────────────────────────
	 *
	 * Pipe IN  : buffer RX yang diantre ke device; callback dipanggil
	 *             setiap frame 802.11 masuk.
	 * Pipe OUT : kirim frame 802.11 (descriptor + payload).
	 *
	 * Buffer & transfer TIDAK PERNAH di-free di dalam callback — hanya
	 * setelah completion & tidak ada lagi yang memakainya.
	 */
	bool bulkOpen(IOUSBHostInterface *interface);
	/* Lepaskan ctx+buffer sebuah transfer (setelah completion selesai). */
	static void bulkRelease(void *param);
	void bulkClose();

	/* Queue N transfer RX. */
	bool bulkQueueRx(uint32_t count);
	/*
	 * Kirim satu frame (descriptor sudah disusun oleh pemanggil).
	 *
	 * pipeIdx memilih bulk-out endpoint: 0 = prioritas tertinggi (VO/mgmt),
	 * 1 = VI, 2 = BE/BK. 8188EU memetakan endpoint ke queue TX di hardware;
	 * mengirim frame data lewat endpoint mgmt membuat frame tidak pernah
	 * keluar sebagai data (gejala: DHCP "no server").
	 */
	bool bulkSend(const uint8_t *data, uint32_t len, rtwn_tx_cb cb, void *ctx,
	    int pipeIdx = 0);
	/* Jumlah bulk-out pipe yang berhasil dibuka (1..3). */
	int txPipeCount() const { return fNumOut; }
	/* Jumlah frame RX yang sudah diterima. */
	uint32_t rxCount() const { return fRxCount; }
	uint32_t txOk() const { return fTxOk; }
	uint32_t txFail() const { return fTxFail; }

	void setRxCallback(rtwn_rx_cb cb, void *ctx) { fRxCb = cb; fRxCtx = ctx; }
	void setTxDoneCb(rtwn_tx_done_cb cb, void *ctx) { fTxDoneCb = cb; fTxDoneCtx = ctx; }
	int32_t txInFlight() const { return fTxInFlight; }
	bool txBusy() const { return fTxInFlight >= 32; }

private:
	/* Satu control transfer dengan batas waktu sendiri (buffers di heap). */
	int rtwnCtlTransfer(bool isInput, uint8_t requestType, uint8_t bRequest,
	    uint16_t wValue, void *userBuf, uint32_t len, uint32_t *bytesOut);

public:

	/* Control transfer: device -> host (isi buffer). */
	int controlIn(uint8_t bRequest, uint16_t wValue, void *data, uint32_t len);
	/* Control transfer: host -> device (kirim buffer). */
	int controlOut(uint8_t bRequest, uint16_t wValue, const void *data, uint32_t len);

public:
	IOUSBHostPipe      *fPipeIn;
	IOUSBHostPipe      *fPipeOut;
	/* Semua bulk-out, urutan sesuai descriptor (0 = prioritas tertinggi). */
	IOUSBHostPipe      *fPipeOutAll[3];
	bool                fPipeStalled[3];
	int                 fNumOut;
	rtwn_rx_cb          fRxCb;
	void               *fRxCtx;
	uint32_t            fRxCount;
	uint32_t            fTxOk;
	uint32_t            fTxFail;
	volatile int32_t    fTxInFlight;
	rtwn_tx_done_cb     fTxDoneCb;
	void               *fTxDoneCtx;

	/*
	 * Di Publik karena completion callback (fungsi bebas di
	 * usb_compat.cpp) membacanya. Kebiggranian tetap dijaga lewat
	 * flag fBulkClosing / fAttached.
	 */
	bool                fBulkClosing;
	bool                fAttached;

	/*
	 * Context transfer TX yang sudah selesai.
	 *
	 * TIDAK boleh dibebaskan di dalam completion callback (aturan A1:
	 * stack USB masih bisa menyentuh buffer). Jadi completion hanya
	 * menaruhnya di daftar ini, dan pemanggil bulkSend() (context
	 * driver, aman) yang membebaskannya.
	 */
	RtwnBulkCtx           *fTxDone;
	IOLock                *fBulkLock;

	/*
	 * Token "driver masih hidup" — heap, TIDAK PERNAH di-free.
	 * Lihat komentar di RtwnBulkCtx::alive.
	 */
	bool                  *fAlive;

private:
	void trace(const char *msg) { if (fTrace) fTrace(fTraceCtx, msg); }

	/*
	 * ‑‑‑ Buffer control transfer yang dipakai BERULANG ‑‑‑
	 *
	 * Alasan (panic #1/#2/#3): setiap transfer membuat memory descriptor
	 * baru → kalau tidak dibebaskan, tiap MD-sex.pages membocorkan halaman
	 * kernel (MD dialokasikan page-granular!). Init M1 melakukan ribuan
	 * transfer → puluhan MB terbuang.
	 *
	 * Solusi: satu MD persisten, dipakai ulang, dilindungi IOLock.
	 * MD-nya sendiri TIDAK PERNAH dibebaskan selama kext hidup
	 * (aturan A1: leak lebih murah daripada use-after-free).
	 */
	IOLock                *fCtlLock;
	IOBufferMemoryDescriptor *fCtlMd;
	uint8_t               *fCtlBuf;
	uint32_t               fCtlCap;

	uint32_t            fTimeoutMs;
	uint32_t            fMaxFails;
	uint32_t            fConsecutiveFails;
	uint32_t            fInOk;
	uint32_t            fInFail;
	uint32_t            fOutOk;
	uint32_t            fOutFail;
	RtwnTraceFn         fTrace;
	void               *fTraceCtx;
	IOUSBHostDevice    *fDevice;
	IOUSBHostInterface *fInterface;
	IOService          *fClient;
	bool                fOpen;
};

#endif /* RTL8188EU_USB_COMPAT_H */
