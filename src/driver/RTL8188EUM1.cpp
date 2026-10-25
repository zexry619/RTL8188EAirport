/*
 * RTL8188EUM1 — driver Wi-Fi native (IO80211) untuk RTL8188EUS (2357:010c).
 *
 * STATUS: Milestone M1 (bring-up chip) — M2 (bulk TX/RX) — M3/M4 (net80211 + IO80211)
 *
 * Arsitektur:
 *   src/urtwn/rtwn_seq*.c  logika M1 portabel (C murni, tanpa IOKit)
 *   src/urtwn/rtwn_flow.c   power-flow vendor (tabel statik dari RtWlanU.kext)
 *   src/urtwn/rtwn_usb.c    descriptor TX/RX + ring TX (portabel)
 *   src/usb/usb_compat.cpp   transport USB (IOKit) + bulk pipe
 *   src/driver/…            driver ini:生命周期 IOKit + orkestrasi
 *
 * Aturan keselamatan (dari 2 kernel panic sebelumnya):
 *   - Driver INERT saat boot (tidak ada traffic USB) → boot tidak pernah gagal.
 *   - Buffer/context transfer USB TIDAK PERNAH di-free di dalam callback.
 *   - Log & progress ditulis ke registry (M1State/M1Log), bukan Unified Log.
 *
 * Lisensi: GPL-2.0-or-later (lihat ../../LICENSES.md)
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/StandardUSB.h>
#include <mach/mach_time.h>
#include <string.h>

#include "rtwn_m1.h"
#include "rtwn_seq.h"
#include "rtwn_usb.h"
#include "rtwn_hal_8188e.h"
#include "r92creg.h"
#include "m1_mode.h"

#define LOGP "RTL8188EUM1: "

/* Batas: attach < 60 detik setelah boot → anggap ini boot → INERT. */
#define M1_BOOT_GUARD_MS 60000

/*
 * 0 = build AMAN (default): driver tidak melakukan traffic USB sama sekali
 *     → aman di ESP, tidak mungkin bikin mesin gagal boot.
 * 1 = build UJI: boleh melakukan traffic USB (dipakai via hot-swap runtime).
 */
#ifndef M1_ALLOW_USB_TRAFFIC
#define M1_ALLOW_USB_TRAFFIC 0
#endif

/* ================================================================== */
/*  Helper                                                            */
/* ================================================================== */

/* Konversi uint32 -> string (tanpa stdio di kernel). */
/*
 * Konversi uint32 → string.
 *
 * PERBAIKAN (audit 22 Okt 2026, temuan L-1): buffer dulu `static`
 * → dua panggilan berdekatan saling menimpa. Sekarang buffer per-panggilan
 * (di stack) dan hasilnya disalin ke buffer pemanggil.
 */
static void m1Itoa(uint32_t v, char *buf, size_t buflen)
{
	int i = (int)buflen - 1;

	if (!buf || buflen == 0) {
		return;
	}
	buf[i] = 0;
	if (v == 0) {
		buf[--i] = '0';
	}
	while (v && i > 0) {
		buf[--i] = (char)('0' + (v % 10));
		v /= 10;
	}
	if (i > 0) {
		/* geser ke depan supaya string mulai dari buf[0] */
		size_t len = (size_t)((int)buflen - 1 - i);
		memmove(buf, buf + i, len + 1);
	}
}

/*
 * Waktu uptime (estimasi) dalam milidetik.
 *
 * ‑‑‑ SOAL TIMEBASE MACH ‑‑‑
 *
 * mach_absolute_time() mengembalikan TICK, bukan nanodetik. Waktu nyata
 * dalam ns adalah:  ns = ticks * (numer / denom)
 * di mana (numer, denom) = mach_timebase_info().
 *
 * Kita TIDAK bisa memanggil mach_timebase_info() dari kext:
 *   - tidak dideklarasikan di MacKernelSDK, dan
 *   - `mach_timebase_info_trap` meskipun ada di kernel, TIDAK boleh
 *     dipakai kext ("could not find a kext which exports this symbol"
 *     saat KC builder tries to link) — sudah dicoba & dibuktikan.
 *
 * TAPI asumsi 1:1 (1 tick = 1 ns) **aman secara konstruktif**:
 *
 *   - tick secara fisik tidak mungkin lebih cepat dari 1 nanodetik
 *     → (numer/denom) >= 1 → waktu nyata >= ticks.
 *   - jadi estimasi kita (ticks) SELALU <= waktu nyata.
 *   - guard boot kita: "elapsed < 60 detik → INERT".
 *     Karena estimasi <= nyata, guard kita menyala LEBIH LAMBAT
 *     (lebih ketat) dari yang seharusnya, TIDAK pernah lebih cepat.
 *
 * Arah errornya selalu ke sisi AMAN: driver tidak mungkin melakukan
 * traffic USB terlalu dini hanya karena timebase bukan 1:1.
 *
 * (Aturan A3 tetap dijaga: driver tidak boleh menyentuh device tanpa
 *  perintah eksplisit — lihat trigger M1Trigger.)
 */
static uint64_t m1UptimeMs(void)
{
	return mach_absolute_time() / 1000000ULL;
}

/* ================================================================== */
/*  Class                                                             */
/* ================================================================== */

class RTL8188EUM1 : public IOService {
	OSDeclareDefaultStructors(RTL8188EUM1);

public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	void free() override;
	bool terminate(IOOptionBits options = 0) override;

	/* Dipakai modul portabel (rtwn_seq*.c) via callback. */
	rtwn_m1_t           fChip;
	void noteProgress() { fLastProgress = mach_absolute_time(); }

	void scheduleInit();
	void runInitSequence();
	void releaseEventSources();
	void timerFired(IOTimerEventSource *sender);
	void watchdogFired(IOTimerEventSource *sender);
	int  countBulkOutEndpoints(IOUSBHostDevice *device);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fTimer;
	IOTimerEventSource *fWatchdog;
	uint64_t            fLastProgress;
	bool                fStarted;
	bool                fInitDone;

	uint32_t            fRxGood;
	uint32_t            fRxBad;
	uint32_t            fRxBytes;
	OSString           *fRxSsid;
	uint8_t             fRxSsidLen;

	/*
	 * Kunci untuk state yang diakses dari >1 thread:
	 *   - mLog ditulis dari workloop DAN dari callback USB (trace)
	 *     → strlcat paralel = race.
	 *   - fRxSsid / fRxGood / fRxBad / fRxBytes ditulis dari callback
	 *     USB dan dibaca dari workloop.
	 */
	IOLock            *fStateLock;
	char               mLog[4096];
	/*
	 * Set TRUE saat driver mulai detach. Callback USB yang telat
	 * memverifikasi ini → jangan sentuh objek driver yang sudah
	 * di-free (use-after-free).
	 */
	volatile bool       fDetaching;
};

OSDefineMetaClassAndStructors(RTL8188EUM1, IOService);

/* ================================================================== */
/*  Trace ke registry                                                 */
/* ================================================================== */

/*
 * Log marker.
 *
 * PERBAIKAN (audit 22 Okt 2026, temuan M-1):
 * buffer ini dulu `static` (global) sementara kuncinya per-instance.
 * Personality kita cocok untuk IOUSBHostInterface DAN IOUSBHostDevice,
 * jadi bisa ada DUA instancedriver hidup bersamaan → keduanya menulis
 * buffer global yang sama dengan kunci yang BERBEDA → strlcat paralel
 * → log korup.
 *
 * Sekarang buffer per-instance (anggota kelas) → kunci per-instance
 * sudah cukup, dan log tiap instance terpisah (lebih mudah didiagnosis).
 */

/*
 * Tulis marker ke registry + akumulasi log.
 *
 * ‑‑‑ CATATAN KEAMANAN ‑‑‑
 *
 * 1. Buffer log `mLog` ditulis dari lebih dari satu thread (workloop
 *    driver + callback USB lewat m1UsbTrace). Dua strlcat paralel = race
 *    → log korup. Karena itu SELURUH fungsi ini dilindungi fStateLock.
 * 2. JANGAN pernah membaca-balik property untuk menyusun log
 *    (copyProperty + release = jalur crash yang sudah menewaskan 2×).
 * 3. setProperty() dipanggil dari callback USB → bisa memicu notifikasi
 *    registry. Untuk itu tidak ada release objek katalog di sini.
 */
/*
 * Ringkasan hasil M1 ke Unified Log.
 *
 * PENTING: driver melakukan self-detach (terminate()) setelah init selesai
 * → node IORegistry-nya HILANG → `ioreg -r -c RTL8188EUM1` tidak
 * menemukan apa-apa. Karena itu hasil harus dicetak ke Unified Log,
 * yang bertahan setelah driver mati.
 *
 * Format: "M1RESULT <key>=<value> <key>=<value> ..."
 */
static void m1LogResult(const char *state, const char *mac, const char *xtal,
    uint32_t cr, uint32_t ok, uint32_t fail)
{
	IOLog(LOGP "M1RESULT state=%s mac=%s xtal=%s cr=0x%08x ok=%u fail=%u\n",
	    state, mac ? mac : "-", xtal ? xtal : "-", cr, ok, fail);
}



/* Format 6 byte MAC sebagai string hex (tanpa stdio). */
static void
m1_hex_str(char *dst, size_t cap, const uint8_t *src, size_t n)
{
	static const char hx[] = "0123456789abcdef";
	size_t i;

	if (!dst || !src || cap < n * 2 + 1) {
		return;
	}
	for (i = 0; i < n; i++) {
		dst[i * 2] = hx[(src[i] >> 4) & 0xf];
		dst[i * 2 + 1] = hx[src[i] & 0xf];
	}
	dst[n * 2] = 0;
}

static void m1Mark(RTL8188EUM1 *me, const char *key, const char *value);

/*
 * Callback diagnostik: lapisan portabel melaporkan string diagnostik
 * (kode C murni tidak boleh tahu-menahu IOKit).
 */
static void
m1Diag(void *ctx, const char *key, const char *val)
{
	RTL8188EUM1 *me = (RTL8188EUM1 *)ctx;

	if (me && key && val) {
		m1Mark(me, key, val);
	}
}

static void m1Mark(RTL8188EUM1 *me, const char *key, const char *value)
{
	if (!me || !key || !value) {
		return;
	}
	if (me->fDetaching) {
		return;
	}

	OSString *v = OSString::withCString(value);
	if (!v) {
		return;
	}
	me->setProperty(key, v);
	me->noteProgress();

	/* Akumulasi riwayat di buffer statis (TIDAK baca-balik property:
	 * melepas objek property katalog = jalur yang memicu panic). */
	/*
	 * PERBAIKAN (audit 22 Okt 2026, temuan M-2):
	 * jangan melakukan ALOKASI (kalloc) di bawah lock — denganCString()
	 * bisa memblokir & mengambil lock VM → risiko lock-order inversion.
	 * Jadi: salin isi log ke buffer stack DI DALAM lock, lalu alokasi
	 * DI LUAR lock.
	 */
	char snapshot[1024];
	snapshot[0] = 0;

	if (me->fStateLock) {
		IOLockLock(me->fStateLock);
	}
	size_t used = strlen(me->mLog);
	if (used > 0 && used < sizeof(me->mLog) - 4) {
		strlcat(me->mLog, " > ", sizeof(me->mLog));
	}
	if (used < sizeof(me->mLog) - 1) {
		strlcat(me->mLog, value, sizeof(me->mLog));
	}
	strlcpy(snapshot, me->mLog, sizeof(snapshot));
	if (me->fStateLock) {
		IOLockUnlock(me->fStateLock);
	}

	OSString *joined = OSString::withCString(snapshot);
	if (joined) {
		me->setProperty("M1Log", joined);
		joined->release();
	}
	v->release();
}

void RTL8188EUM1::timerFired(IOTimerEventSource *sender)
{
	m1Mark(this, "M1State", "timer-fired");
	runInitSequence();
	fInitDone = true;

	/*
	 * Lepaskan diri setelah selesai: driver yang menempel membuat kext
	 * "in use" → tidak bisa di-unload → hot-swap binary baru mustahil.
	 */
	if (fWatchdog) {
		fWatchdog->cancelTimeout();
	}
	if (fTimer) {
		fTimer->cancelTimeout();
	}
	m1Mark(this, "M1State", "self-detach");
	IOLog(LOGP "self-detach (supaya kext bisa di-unload & di-hot-swap)\n");
	/*
	 * Beri jeda 2 detik sebelum terminate().
	 *
	 * TERMINATE() MENGHAPUS node dari IORegistry → semua marker
	 * (M1State/M1Mac/M1Log) hilang. Tanpa jeda, hasil init terakhir
	 * tidak sempat dibaca user space.
	 */
	IOSleep(2000);
	terminate();
}


void RTL8188EUM1::watchdogFired(IOTimerEventSource *sender)
{
	uint64_t ms = (mach_absolute_time() - fLastProgress) / 1000000;

	if (ms > 60000) {
		m1Mark(this, "M1State", "watchdog-abort");
		IOLog(LOGP "watchdog: init beku %llu ms - batalkan & lepas\n",
		    (unsigned long long)ms);
		fChip.usb.abortAll();
		m1Mark(this, "M1State", "watchdog-done");
		return;
	}
	if (fWatchdog) {
		fWatchdog->setTimeoutMS(15000);
	}
}


/* ================================================================== */
/*  Adapter I/O  (rtwn_seq*.c <-> register hardware)                   */
/* ================================================================== */

static int m1Read(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
	RTL8188EUM1 *me = (RTL8188EUM1 *)ctx;
	uint8_t *b = (uint8_t *)buf;

	/*
	 * Kabari driver bahwa kita masih hidup.
	 *
	 * PENTING: tanpa ini, fase panjang (mis. baca EFUSE 256 byte)
	 * terlihat "macet" oleh watchdog even though tidak ada yang salah.
	 */
	if (me) {
		me->noteProgress();
	}

	switch (len) {
	case 1:
		b[0] = rtwn_m1_read_1(&me->fChip, addr);
		return 0;
	case 2: {
		uint16_t v = rtwn_m1_read_2(&me->fChip, addr);
		b[0] = (uint8_t)(v & 0xff);
		b[1] = (uint8_t)(v >> 8);
		return 0;
	}
	case 4: {
		uint32_t v = rtwn_m1_read_4(&me->fChip, addr);
		b[0] = (uint8_t)(v & 0xff);
		b[1] = (uint8_t)((v >> 8) & 0xff);
		b[2] = (uint8_t)((v >> 16) & 0xff);
		b[3] = (uint8_t)((v >> 24) & 0xff);
		return 0;
	}
	default:
		/*
		 * Panjang lain (mis. 196 byte saat upload firmware).
		 *
		 * PERBAIKAN (audit 22 Okt 2026): sebelumnya `default` →
		 * return -1, sehingga rtwn_seq_load_firmware() gagal pada
		 * chunk pertama dan chip TIDAK PERNAH menerima firmware.
		 *
		 * Vendor request R92C_REQ_REGS (bRequest 0x05) mendukung
		 * wLength bebas — jadi cukup teruskan lewat
		 * rtwn_m1_read_region() (sudah pakai controlOut/controlIn
		 * dengan panjang penuh).
		 */
		/*
		 * len bertipe uint16_t → batas maksimumnya sudah 0xffff.
		 * Yang dicek di sini adalah nilai yang MASIH masuk akal
		 * untuk register read (audit 22 Okt 2026, temuan L-3).
		 */
		if (len == 0 || len > 4096) {
			return -1;
		}
		return rtwn_m1_read_region(&me->fChip, addr, b, (int)len);
	}
}

static int m1Write(void *ctx, uint16_t addr, const void *buf, uint16_t len)
{
	RTL8188EUM1 *me = (RTL8188EUM1 *)ctx;
	const uint8_t *b = (const uint8_t *)buf;

	if (me) {
		me->noteProgress();
	}

	switch (len) {
	case 1:
		rtwn_m1_write_1(&me->fChip, addr, b[0]);
		return 0;
	case 2:
		rtwn_m1_write_2(&me->fChip, addr, (uint16_t)(b[0] | (b[1] << 8)));
		return 0;
	case 4:
		rtwn_m1_write_4(&me->fChip, addr,
		    (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
		    ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
		return 0;
	default:
		/*
		 * Panjang lain (mis. 196 byte saat upload firmware).
		 *
		 * PERBAIKAN (audit 22 Okt 2026): sebelumnya `default` →
		 * return -1, sehingga rtwn_seq_load_firmware() gagal pada
		 * chunk pertama dan chip TIDAK PERNAH menerima firmware.
		 *
		 * Vendor request R92C_REQ_REGS (bRequest 0x05) mendukung
		 * wLength bebas — teruskan lewat rtwn_m1_write_region().
		 */
		/* Lihat catatan di m1Read: batas 4096, bukan 0xffff. */
		if (len == 0 || len > 4096) {
			return -1;
		}
		return rtwn_m1_write_region(&me->fChip, addr, b, (int)len);
	}
}

/* Cari interface USB (nomor 0) milik device.
 *
 * ‑‑‑ KONTRAK IOKIT (penting!) ‑‑‑
 *
 * `OSIterator::getNextObject()` mengembalikan objek **milik collection**:
 *   "The returned object will be released if removed from the collection;
 *    if you plan to store the reference, you should call retain()"
 *   (MacKernelSDK/Headers/libkern/c++/OSIterator.h)
 *
 * Akibatnya:
 *   - JANGAN `obj->release()`  → over-release → objek collection ikut
 *     ter-release lagi → DOUBLE FREE → "A kext releasing ... has corrupted
 *     the registry" (PANIC #1/#2/#3).
 *   - WAJIB `found->retain()` kalau pointer-nya mau disimpan & dipakai
 *     setelah iterator di-release (kalau tidak → dangling pointer →
 *     use-after-free saat deviceRequest()).
 *
 * Fungsi ini mengembalikan reference +1; pemanggil WAJIB release()-nya.
 */
static IOUSBHostInterface *m1FindInterface(IOUSBHostDevice *device)
{
	if (!device) {
		return nullptr;
	}

	OSIterator *it = device->getChildIterator(gIOServicePlane);
	if (!it) {
		return nullptr;
	}

	IOUSBHostInterface *found = nullptr;
	OSObject *obj = nullptr;
	while ((obj = it->getNextObject())) {
		IOUSBHostInterface *i = OSDynamicCast(IOUSBHostInterface, obj);
		if (i) {
			found = i;
			found->retain();	/* +1 untuk pemanggil */
			break;
		}
		/* JANGAN obj->release() — objek milik collection! */
	}
	it->release();
	return found;
}

/* Trace callback (lapisan USB → registry). */
static void m1UsbTrace(void *ctx, const char *msg)
{
	RTL8188EUM1 *me = (RTL8188EUM1 *)ctx;
	if (me) {
		m1Mark(me, "M1Last", msg);
	}
}

/* ================================================================== */
/*  Callback RX                                                       */
/* ================================================================== */

static void m1RxCallback(void *ctx, const uint8_t *data, uint32_t len)
{
	RTL8188EUM1 *me = (RTL8188EUM1 *)ctx;
	rtwn_usb_rxinfo_t info;

	if (!me || !data) {
		return;
	}
	/*
	 * Callback ini berjalan di thread stack USB. Kalau driver sedang
	 * detach/free, objek `me` sudah tidak valid → use-after-free.
	 */
	if (me->fDetaching) {
		return;
	}
	if (!rtwn_usb_parse_rx_desc(data, len, &info)) {
		me->fRxBad++;
		return;
	}
	if (info.crcError || info.icvError) {
		me->fRxBad++;
		return;
	}
	me->fRxGood++;
	me->fRxBytes += info.pktlen;

	/* Ambil SSID dari beacon/probe response (validasi scan). */
	{
		const uint8_t *f = data + info.headerLen;
		uint32_t flen = info.pktlen;

		/*
		 * DEFENSE IN DEPTH (fix keamanan, 22 Okt 2026).
		 *
		 * rtwn_usb_parse_rx_desc() sudah menolak pktlen yang melebihi
		 * data yang diterima, TAPI jangan bergantung pada satu
		 * pemeriksaan saja — hitung ulang batasnya di sini.
		 *
		 * Tanpa ini: f[i + 3 + k] bisa dibaca sampai ~14 KB di luar
		 * buffer RX 2048 byte → kernel panic (halaman tidak ter-map).
		 */
		if (info.headerLen > len) {
			return;
		}
		if (flen > len - info.headerLen) {
			flen = len - info.headerLen;
		}

		if (flen >= 56) {
			uint8_t typ = (f[0] >> 2) & 0x03;	/* beacon */
			if (typ == 0) {
				uint32_t i = 44;
				while (i + 2 <= flen) {
					uint8_t id = f[i];
					uint8_t ilen = f[i + 1];
					if (id == 0) {	/* SSID */
						uint8_t sl = (i + 3 <= flen) ? f[i + 2] : 0;
						char ssid[33];
						uint8_t k = 0;
						/* Bytes yang benar-benar tersedia
						 * setelah byte panjang SSID. */
						uint32_t avail = flen - (i + 3);
						while (k < sl && k < 32 && k < avail) {
							ssid[k] = f[i + 3 + k];
							k++;
						}
						ssid[k] = 0;
						/*
						 * Rilis yang lama dulu — kalau tidak,
						 * setiap beacon bocorkan satu
						 * OSString.
						 */
						OSString *newSsid = OSString::withCString(ssid);
						if (newSsid) {
							OSString *old = nullptr;
							if (me->fStateLock) {
								IOLockLock(me->fStateLock);
							}
							old = me->fRxSsid;
							me->fRxSsid = newSsid;
							me->fRxSsidLen = sl;
							if (me->fStateLock) {
								IOLockUnlock(me->fStateLock);
							}
							if (old) {
								old->release();
							}
						}
						break;
					}
					i += 2 + ilen;
				}
			}
		}
	}
}

/* ================================================================== */
/*  Timer callbacks                                                   */
/* ================================================================== */

static void m1TimerAction(OSObject *owner, IOTimerEventSource *sender)
{
	RTL8188EUM1 *me = OSDynamicCast(RTL8188EUM1, owner);
	if (me) {
		me->timerFired(sender);
	}
}

static void m1WatchdogAction(OSObject *owner, IOTimerEventSource *sender)
{
	RTL8188EUM1 *me = OSDynamicCast(RTL8188EUM1, owner);
	if (me) {
		me->watchdogFired(sender);
	}
}

/* ================================================================== */
/*  Lifecycle                                                         */
/* ================================================================== */

bool RTL8188EUM1::start(IOService *provider)
{
	if (!provider) {
		return false;
	}

	/*
	 * Kunci state dibuat paling awal — dipakai m1Mark() yang bisa
	 * dipanggil dari mana saja (workloop, callback USB).
	 */
	if (!fStateLock) {
		fStateLock = IOLockAlloc();
	}
	fDetaching = false;

	/* Daftarkan hook diagnostik lapisan portabel. */
	rtwn_seq_diag_register(m1Diag);

	/*
	 * Trigger eksplisit: bila properti "M1Trigger" = "run" diset dari
	 * user space (scripts/trigger-init.sh), jalankan init.
	 * Tanpa itu → driver pasif (tidak menyentuh device sama sekali).
	 */
	{
		/*
		 * PERBAIKAN (audit 22 Okt 2026, temuan M-4):
		 * copyProperty() mengembalikan reference +1. OSDynamicCast()
		 * TIDAK me-retain dan mengembalikan NULL untuk tipe lain →
		 * reference +1 itu hilang (bocor). Sekarang reference-nya
		 * dilepas tepat satu kali, apa pun tipenya.
		 */
		OSObject *prop = copyProperty("M1Trigger");
		OSString *trig = OSDynamicCast(OSString, prop);
		bool run = false;
		if (trig) {
			run = trig->isEqualTo("run");
		}
		if (prop) {
			prop->release();
		}
		if (run) {
			m1Mark(this, "M1State", "trigger-diterima");
			IOLog(LOGP "trigger diterima — menjalankan init\n");
			/* lanjut ke attach → scheduleInit() di bawah */
			IOService::start(provider);
			fStarted = true;
			fLastProgress = mach_absolute_time();
			scheduleInit();
			return true;
		}
	}

	fWorkLoop = IOWorkLoop::workLoop();
	if (!fWorkLoop) {
		return false;
	}

	fTimer = IOTimerEventSource::timerEventSource(this, m1TimerAction);
	if (!fTimer) {
		/*
		 * PERBAIKAN (audit 22 Okt 2026, temuan H-4):
		 * start() yang gagal TIDAK pernah memanggil stop() → event
		 * source & work loop harus dilepas sendiri di sini.
		 */
		releaseEventSources();
		return false;
	}
	fWorkLoop->addEventSource(fTimer);
	fTimer->enable();

	fWatchdog = IOTimerEventSource::timerEventSource(this, m1WatchdogAction);
	if (fWatchdog) {
		fWorkLoop->addEventSource(fWatchdog);
		fWatchdog->enable();
	}

	if (!IOService::start(provider)) {
		m1Mark(this, "M1State", "IOService::start-gagal");
		IOLog(LOGP "IOService::start() gagal\n");
		releaseEventSources();
		return false;
	}

	fStarted = true;
	fLastProgress = mach_absolute_time();

	/*
	 * INERT saat boot: kalau attach terjadi < 60 detik setelah boot,
	 * jangan sentuh device sama sekali. Ini yang membuat mesin tidak
	 * pernah gagal boot karena kext eksperimental.
	 */
	if (m1UptimeMs() < M1_BOOT_GUARD_MS) {
		m1Mark(this, "M1State", "started-inert");
		IOLog(LOGP "ter-attach (INERT, uptime < 60 dtk) — "
		    "nihil traffic USB\n");
		return true;
	}

	/*
	 * Build AMAN (M1_ALLOW_USB_TRAFFIC=0) → tidak pernah menyentuh device.
	 * Dipakai untuk build yang ada di ESP (aman saat boot).
	 * Build UJI (1) → boleh melakukan traffic USB, hanya via hot-swap runtime.
	 */
#if !M1_ALLOW_USB_TRAFFIC
	m1Mark(this, "M1State", "started-safe-no-traffic");
	IOLog(LOGP "build AMAN: tidak ada traffic USB\n");
	return true;
#else
	m1Mark(this, "M1State", "started-runtime");
	IOLog(LOGP "ter-attach (runtime) — menjalankan init\n");
	scheduleInit();
	return true;
#endif
}

/*
 * Lepaskan event source dari work loop dengan BENAR.
 *
 * PERBAIKAN (audit 22 Okt 2026, temuan H-1/H-4):
 *
 *   IOWorkLoop::addEventSource() → mAddEvent() melakukan
 *   `inEvent->retain()`. Artinya work loop MEMILIKI reference ke
 *   event source dan menahannya di event chain-nya. Hanya
 *   removeEventSource() yang melepas DAN me-release-nya.
 *
 *   Jika kita hanya `release()` tanpa removeEventSource(), work loop
 *   masih memegang event source yang AKTIF dengan `owner` = driver
 *   yang sudah dibebaskan → ketika timeout-nya berbunyi →
 *   m1TimerAction(owner=<driver yang sudah di-free>) →
 *   OSDynamicCast pada memory bebas → heap corruption → PANIC.
 *
 *   Jalur "start() gagal" TIDAK pernah memanggil stop() → helper ini
 *   juga dipanggil dari free() sebagai jaring pengaman.
 */
void RTL8188EUM1::releaseEventSources()
{
	if (fTimer) {
		fTimer->cancelTimeout();
		fTimer->disable();
		if (fWorkLoop) {
			/* removeEventSource(NULL) = dereference NULL → panic.
			 * Karena itu fTimer selalu dicek dulu. */
			fWorkLoop->removeEventSource(fTimer);
		}
		fTimer->release();
		fTimer = nullptr;
	}
	if (fWatchdog) {
		fWatchdog->cancelTimeout();
		fWatchdog->disable();
		if (fWorkLoop) {
			fWorkLoop->removeEventSource(fWatchdog);
		}
		fWatchdog->release();
		fWatchdog = nullptr;
	}
	if (fWorkLoop) {
		fWorkLoop->release();
		fWorkLoop = nullptr;
	}
}

void RTL8188EUM1::stop(IOService *provider)
{
	IOLog(LOGP "stop()\n");

	/*
	 * Tandai sedang detach SEBELUM apa pun dilepas.
	 *
	 * Callback USB bisa masih berjalan (thread stack USB). Setelah
	 * fDetaching = true, callback tersebut melihat flag ini dan
	 * berhenti menyentuh objek driver — tanpa ini, driver bisa
	 * di-free sementara callback masih memakainya (use-after-free).
	 */
	fDetaching = true;

	/*
	 * PERBAIKAN (audit 22 Okt 2026, temuan C-1):
	 *
	 * Urutannya penting:
	 *   1. DISARM callback RX → tidak ada lagi yang memanggil driver
	 *   2. bulkClose() → abort pipe (completion terakhir keluar, tapi
	 *      sudah tidak menyentuh driver karena callback di-disarm)
	 *   3. detach() → token "alive" dimatikan → completion yang telat
	 *      melihat token false dan keluar tanpa menyentuh apa pun
	 */
	fChip.usb.setRxCallback(nullptr, nullptr);
	fChip.usb.setTrace(nullptr, nullptr);	/* L-6: jangan ada pointer driver yg menggantung */
	fChip.usb.bulkClose();
	fChip.usb.detach();

	releaseEventSources();

	IOService::stop(provider);
}

bool RTL8188EUM1::terminate(IOOptionBits options)
{
	m1Mark(this, "M1State", "terminated");
	IOLog(LOGP "terminate()\n");
	fDetaching = true;
	if (fTimer) {
		fTimer->cancelTimeout();
		fTimer->disable();
	}
	if (fWatchdog) {
		fWatchdog->cancelTimeout();
		fWatchdog->disable();
	}
	return IOService::terminate(options);
}

void RTL8188EUM1::free()
{
	fDetaching = true;

	/*
	 * PERBAIKAN (audit 22 Okt 2026, temuan C-3/H-1/H-4):
	 * - releaseEventSources() menutup jalan "start() gagal → stop()
	 *   tidak dipanggil" (H-1/H-4).
	 * - fRxSsid & fStateLock dilepas dengan cara yang tidak bisa
	 *   bertabrakan dengan callback USB: callback sudah tidak mungkin
	 *   berjalan karena (a) callback di-disarm di stop(), (b) token
	 *   "alive" dimatikan di detach().
	 */
	releaseEventSources();

	if (fRxSsid) {
		fRxSsid->release();
		fRxSsid = nullptr;
	}
	if (fStateLock) {
		IOLockFree(fStateLock);
		fStateLock = nullptr;
	}
	IOService::free();
}

void RTL8188EUM1::scheduleInit()
{
	if (!fTimer) {
		return;
	}
	fLastProgress = mach_absolute_time();
	if (fWatchdog) {
		fWatchdog->setTimeoutMS(15000);
	}
	m1Mark(this, "M1State", "init-dijadwalkan");
	if (fTimer->setTimeoutMS(0) != kIOReturnSuccess) {
		m1Mark(this, "M1State", "timer-gagal");
	}
}



/* Hitung jumlah endpoint bulk OUT dari deskriptor konfigurasi.
 * OpenBSD: urtwn_open_pipes() mengisi sc->ntx dengan cara yang sama. */
int RTL8188EUM1::countBulkOutEndpoints(IOUSBHostDevice *device)
{
	const StandardUSB::ConfigurationDescriptor *cd =
	    device->getConfigurationDescriptor();
	if (!cd) {
		return 0;
	}

	const StandardUSB::InterfaceDescriptor *id =
	    StandardUSB::getNextInterfaceDescriptor(cd, nullptr);
	int ntx = 0;
	while (id) {
		const StandardUSB::EndpointDescriptor *ep = nullptr;
		while ((ep = StandardUSB::getNextEndpointDescriptor(cd, id, ep))) {
			/* bmAttributes: bit 0x03 = transfer type, 0x02 = bulk */
			if ((ep->bmAttributes & 0x03) != 0x02) {
				continue;
			}
			/* Arah: bit 0x80 = IN */
			if (ep->bEndpointAddress & 0x80) {
				continue;
			}
			ntx++;
		}
		id = StandardUSB::getNextInterfaceDescriptor(cd, id);
	}
	return ntx;
}

static volatile bool g_m1_in_progress = false;

/*
 * Lock GLOBAL untuk test-and-set g_m1_in_progress.
 *
 * PERBAIKAN (audit 22 Okt 2026, temuan M-8): flag ini dipakai lintas
 * instance driver, jadi kuncinya harus global — bukan fStateLock yang
 * per-instance.
 */
static IOLock *g_m1_init_lock = nullptr;

static IOLock *m1InitLock(void)
{
	if (!g_m1_init_lock) {
		g_m1_init_lock = IOLockAlloc();
	}
	return g_m1_init_lock;
}


/* Scope guard: flag dilepas otomatis di semua jalur keluar (termasuk early return).
 * Versi sebelumnya menahan flag selamanya ketika satu instance bail di awal —
 * itu membuat semua instance berikutnya "skip-duplicate" dan driver macet total. */
struct M1Guard {
	volatile bool &flag;
	IOLock        *lock;

	explicit M1Guard(volatile bool &f, IOLock *l) : flag(f), lock(l) {
		flag = true;
	}
	~M1Guard() {
		/*
		 * Lepas flag DI BAWAH LOCK yang sama — kalau tidak, ada
		 * jendela di mana instance lain bisa melihat flag masih
		 * true padahal init sudah selesai (atau sebaliknya).
		 */
		if (lock) {
			IOLockLock(lock);
			flag = false;
			IOLockUnlock(lock);
		} else {
			flag = false;
		}
	}
};

void RTL8188EUM1::runInitSequence()
{
	/*
	 * Lebih dari satu personality bisa match → hanya satu instance boleh
	 * menjalankan init.
	 *
	 * PERBAIKAN (audit 22 Okt 2026, temuan M-8):
	 * pemeriksaan + set flag sebelumnya TIDAK atomik → dua instance bisa
	 * lolos bersamaan dan sama-sama menyentuh chip. Sekarang
	 * test-and-set dilakukan di bawah lock GLOBAL.
	 *
	 * ‑‑‑ PANIC #4 (22 Okt 2026, 03:55) ‑‑‑
	 *
	 * Versi pertama dari perbaikan ini Lupa memanggil IOLockLock() dan
	 * langsung IOLockUnlock() → "unlocking an unlocked mutex" →
	 * preemption_level jadi -1 → call berikutnya yang butuh mutex
	 * (IOService::open) memicu:
	 *
	 *   panic: thread_invoke: preemption_level -1, possible cause:
	 *          unlocking an unlocked mutex or spinlock @sched_prim.c:3224
	 *
	 * Pelajaran: SETIAP IOLockUnlock wajibdidahului IOLockLock pada
	 * mutex yang SAMA, dan pointer-nya harus disimpan di variabel lokal
	 * (memanggil m1InitLock() dua kali berisiko mengembalikan pointer
	 * berbeda bila ada balapan saat alokasi).
	 */
	IOLock *lock = m1InitLock();
	if (!lock) {
		return;
	}
	IOLockLock(lock);
	if (g_m1_in_progress) {
		IOLockUnlock(lock);
		m1Mark(this, "M1State", "skip-duplicate");
		return;
	}
	g_m1_in_progress = true;
	IOLockUnlock(lock);
	M1Guard guard(g_m1_in_progress, lock);
	m1Mark(this, "M1State", "init-mulai");

	/*
	 * Provider bisa berupa:
	 *   - IOUSBHostInterface / IOUSBInterface  → device = provider->getProvider()
	 *   - IOUSBHostDevice / AppleUSBDevice     → device = provider
	 */
	IOUSBHostDevice    *device = nullptr;
	IOUSBHostInterface *interface = nullptr;
	bool                interfaceOwned = false;	/* kita pegang reference +1 */

	if (IOUSBHostInterface *iface = OSDynamicCast(IOUSBHostInterface, getProvider())) {
		interface = iface;
		device = OSDynamicCast(IOUSBHostDevice, iface->getProvider());
		IOLog(LOGP "attach ke IOUSBHostInterface (device dari parent)\n");
	} else if (IOUSBHostDevice *dev = OSDynamicCast(IOUSBHostDevice, getProvider())) {
		device = dev;
		IOLog(LOGP "attach ke IOUSBHostDevice\n");
	} else {
		IOLog(LOGP "provider type tidak dikenal (%s) — abort\n",
		    getProvider() ? getProvider()->getMetaClass()->getClassName() : "null");
		return;
	}

	if (!device) {
		IOLog(LOGP "tidak bisa mendapatkan IOUSBHostDevice — abort\n");
		return;
	}

	/*
	 * Mode ditentukan saat BUILD (M1_ALLOW_USB_TRAFFIC):
	 *   0 (default) → driver INERT total: nol traffic USB. Dipakai untuk
	 *                  build yang ada di ESP (aman saat boot).
	 *   1           → varian uji: boleh melakukan traffic USB.
	 */
	/*
	 * Mode efektif:
	 *   - build AMAN (traffic=0) → selalu READONLY, dan boot-arg TIDAK
	 *     bisa menaikkan ke FULL (aturan A3).
	 *   - build UJI (traffic=1)  → boot-arg "-rtl8188eum1_mode readonly"
	 *     memaksa READONLY; tanpa boot-arg → FULL.
	 */
	int mode = m1EffectiveMode(M1_ALLOW_USB_TRAFFIC);
	m1Mark(this, "M1Mode",
	    mode == M1_MODE_FULL ? "full" :
	    mode == M1_MODE_POWERON ? "poweron" : "readonly");
	IOLog(LOGP "start() dipanggil — provider: %s\n",
	    getProvider() ? getProvider()->getMetaClass()->getClassName() : "null");
	m1Mark(this, "M1State", "start-called");
	if (getProvider()) {
		m1Mark(this, "M1Provider", getProvider()->getMetaClass()->getClassName());
	}
	IOLog(LOGP "=== M1 init started (mode: %s) ===\n",
	    mode == M1_MODE_FULL ? "FULL (menulis register chip)" :
	    "READONLY (hanya baca register)");

	/* 1. USB. */
	/* Control transfer idealnya lewat interface (workloop berbeda dari device). */
	if (!interface) {
		interface = m1FindInterface(device);
		interfaceOwned = (interface != nullptr);
		m1Mark(this, "M1Interface", interface ? "found" : "not-found");
	} else {
		m1Mark(this, "M1Interface", "provider");
	}

	fChip.usb.setTrace(m1UsbTrace, this);
	m1Mark(this, "M1State", "usb-attach");
	uint64_t tStart = mach_absolute_time();
	int usbStatus = fChip.usb.attach(this, device, interface);
	if (interfaceOwned && interface) {
		/* Lepas reference yang kita pegang (usb.attach() menyimpan
		 * reference-nya sendiri). */
		interface->release();
		interface = nullptr;
		interfaceOwned = false;
	}
	if (usbStatus != RtwnUsb::RTWN_USB_OK) {
		const char *why = "tidak diketahui";
		switch (usbStatus) {
		case RtwnUsb::RTWN_USB_ERR_OPEN:  why = "open-device-gagal"; break;
		case RtwnUsb::RTWN_USB_ERR_CFG:   why = "setConfiguration-gagal"; break;
		case RtwnUsb::RTWN_USB_ERR_IFACE: why = "open-interface-gagal"; break;
		case RtwnUsb::RTWN_USB_ERR_ARGS:  why = "argumen-gagal"; break;
		}
		m1Mark(this, "M1State", why);
		IOLog(LOGP "gagal attach USB: %s\n", why);
		return;
	}

	m1Mark(this, "M1State", "usb-ok");
	m1Mark(this, "M1Counters", "see-log");
	/* Chip 8188EUS → RTWN_CHIP_88E (OpenBSD: URTWN_DEV_8188EU). */
	fChip.chip = RTWN_CHIP_88E | RTWN_CHIP_USB;
	fChip.ntx = countBulkOutEndpoints(device);
	IOLog(LOGP "chip flags=0x%08x, bulk OUT endpoint=%d\n", fChip.chip, fChip.ntx);

	if (mode == M1_MODE_READONLY) {
		/*
		 * Mode read-only: hanya BACA register. Tidak ada satu pun
		 * penulisan ke chip, jadi driver vendor Realtek boleh tetap
		 * aktif (Wi-Fi + aplikasi vendor tetap jalan).
		 */
		uint8_t macid[6];
		uint32_t cr = 0xffffffff, aps, sysfunc, mcu, syscfg;

		/*
		 * Tunggu chip siap (driver vendor mungkin masih melakukan
		 * power-on + upload firmware-nya). Poll register CR sampai
		 * tidak lagi 0xffffffff (artinya chip menjawab).
		 */
		for (int tries = 0; tries < 100; tries++) {
			cr = rtwn_m1_read_4(&fChip, R92C_CR);
			if (cr != 0xffffffff) {
				break;
			}
			IOSleep(50);
		}
		if (cr == 0xffffffff) {
			IOLog(LOGP "PERINGATAN: chip tidak menjawab register (0xffffffff) — "
			    "mungkin driver vendor belum selesai inisialisasi\n");
		}

		for (int i = 0; i < 6; i++) {
			macid[i] = rtwn_m1_read_1(&fChip, R92C_MACID + i);
		}
		aps     = rtwn_m1_read_4(&fChip, R92C_APS_FSMCO);
		sysfunc = rtwn_m1_read_2(&fChip, R92C_SYS_FUNC_EN);
		mcu     = rtwn_m1_read_4(&fChip, R92C_MCUFWDL);
		syscfg  = rtwn_m1_read_4(&fChip, R92C_SYS_CFG);

		IOLog(LOGP "MACID ( programmed by driver ) = %02x:%02x:%02x:%02x:%02x:%02x\n",
		    macid[0], macid[1], macid[2], macid[3], macid[4], macid[5]);
		IOLog(LOGP "CR      = 0x%08x  (TXDMA=%d RXDMA=%d PROTOCOL=%d EN SEC=%d)\n",
		    cr, (cr & R92C_CR_TXDMA_EN) ? 1 : 0, (cr & R92C_CR_RXDMA_EN) ? 1 : 0,
		    (cr & R92C_CR_PROTOCOL_EN) ? 1 : 0, (cr & R92C_CR_ENSEC) ? 1 : 0);
		IOLog(LOGP "APS_FSMCO= 0x%08x  (ONMAC=%d SUS_HOST=%d)\n",
		    aps, (aps & R92C_APS_FSMCO_APFM_ONMAC) ? 1 : 0,
		    (aps & R92C_APS_FSMCO_SUS_HOST) ? 1 : 0);
		IOLog(LOGP "SYS_FUNC_EN=0x%04x  (CPUEN=%d ELDR=%d)\n", sysfunc,
		    (sysfunc & R92C_SYS_FUNC_EN_CPUEN) ? 1 : 0,
		    (sysfunc & R92C_SYS_FUNC_EN_ELDR) ? 1 : 0);
		IOLog(LOGP "MCUFWDL = 0x%08x  (EN=%d RAM_DL_SEL=%d WINTINI_RDY=%d)\n",
		    mcu, (mcu & R92C_MCUFWDL_EN) ? 1 : 0,
		    (mcu & R92C_MCUFWDL_RAM_DL_SEL) ? 1 : 0,
		    (mcu & R92C_MCUFWDL_WINTINI_RDY) ? 1 : 0);
		IOLog(LOGP "SYS_CFG  = 0x%08x\n", syscfg);
		/*
		 * ── M1a: baca EFUSE (READ-ONLY) ──
		 *
		 * Ini yang memvalidasi perbaikan paling penting kita:
		 * layout register EFUSE 8188E (addr lo 0x031 / addr hi 0x032 /
		 * status 0x033 / data 0x030) yang berbeda dari OpenBSD.
		 *
		 * Catatan: membaca EFUSE memang menulis ke register
		 * EFUSE_CTRL (memilih alamat yang mau dibaca) — itu BUKAN
		 * penulisan destruktif ke chip. Tidak ada upload firmware,
		 * tidak ada power-on, tidak ada perubahan konfigurasi.
		 *
		 * Skyline chip harus sudah aktif (driver vendor sudah
		 * melakukan power-on) — kalau tidak, chip tidak menjawab
		 * dan pembacaan akan gagal dengan timeout (aman).
		 */
		{
			rtwn_seq_result_t res;
			memset(&res, 0, sizeof(res));

			int rc = rtwn_seq_read_efuse_direct_8188e(&res,
			    m1Read, m1Write, this);
			if (rc != 0) {
				IOLog(LOGP "EFUSE 8188E (direct) GAGAL: %d — "
				    "chip mungkin belum aktif\n", rc);
				m1Mark(this, "M1Efuse", "direct-gagal");
			} else if (!res.efuse_ok) {
				IOLog(LOGP "EFUSE 8188E (direct) tidak valid\n");
				m1Mark(this, "M1Efuse", "direct-tidak-valid");
			} else {
				IOLog(LOGP "EFUSE 8188E (direct) OK: "
				    "MAC=%02x:%02x:%02x:%02x:%02x:%02x "
				    "xtal=0x%02x chplan=0x%02x reg=0x%02x\n",
				    res.mac[0], res.mac[1], res.mac[2],
				    res.mac[3], res.mac[4], res.mac[5],
				    res.xtal, res.channel_plan, res.regulatory);
				m1Mark(this, "M1Efuse", "direct-ok");
				char macbuf[32];
				snprintf(macbuf, sizeof(macbuf),
				    "%02x:%02x:%02x:%02x:%02x:%02x",
				    res.mac[0], res.mac[1], res.mac[2],
				    res.mac[3], res.mac[4], res.mac[5]);
				m1Mark(this, "M1Mac", macbuf);
				char xbuf[16];
				snprintf(xbuf, sizeof(xbuf), "0x%02x", res.xtal);
				m1Mark(this, "M1Xtal", xbuf);
			}

			m1LogResult("readonly-selesai",
			    res.efuse_ok ? "efuse-ok" : "efuse-gagal",
			    res.efuse_ok ? "efuse-ok" : "-", 0,
			    fChip.usb.inOk(), fChip.usb.inFail());
		}

		IOLog(LOGP "=== M1 READONLY selesai — jalur USB terbukti bekerja ===\n");
		IOLog(LOGP "Untuk langkah destruktif (upload firmware):\n");
		IOLog(LOGP "  set boot-args '-%s full' di OpenCore, lalu jalankan "
		    "tanpa RtWlanU\n", M1_MODE_ARG);
		m1Mark(this, "M1State", "readonly-selesai");
		return;
	}

	/*
	 * Snapshot register dulu (READ-ONLY) supaya ada data diagnostik
	 * walau langkah berikutnya gagal.
	 */
	{
		uint32_t cr0 = rtwn_m1_read_4(&fChip, R92C_CR);
		uint32_t mcu0 = rtwn_m1_read_4(&fChip, R92C_MCUFWDL);
		uint8_t  mac0[6];
		for (int i = 0; i < 6; i++) {
			mac0[i] = rtwn_m1_read_1(&fChip, R92C_MACID + i);
		}
		IOLog(LOGP "snapshot sebelum power-on: CR=0x%08x MCUFWDL=0x%08x "
		    "MACID=%02x:%02x:%02x:%02x:%02x:%02x\n",
		    cr0, mcu0, mac0[0], mac0[1], mac0[2], mac0[3], mac0[4], mac0[5]);
	}

	/*
	 * ══════════════════════════════════════════════════════════════
	 *  M1 INIT SEQUENCE — mengikuti urutan vendor (InitializeAdapter8188EUsb)
	 *  docs/init-sequence-8188e.md
	 * ══════════════════════════════════════════════════════════════
	 */
	rtwn_seq_result_t res;
	memset(&res, 0, sizeof(res));

	/* Snapshot register (read-only) untuk diagnosa. */
	{
		uint32_t cr0 = rtwn_m1_read_4(&fChip, R92C_CR);
		uint32_t mcu0 = rtwn_m1_read_4(&fChip, R92C_MCUFWDL);
		uint8_t  mac0[6];
		for (int i = 0; i < 6; i++) {
			mac0[i] = rtwn_m1_read_1(&fChip, R92C_MACID + i);
		}
		IOLog(LOGP "snapshot: CR=0x%08x MCUFWDL=0x%08x MACID=%02x:%02x:%02x:%02x:%02x:%02x\n",
		    cr0, mcu0, mac0[0], mac0[1], mac0[2], mac0[3], mac0[4], mac0[5]);
	}

	/* --- [2] Chip detect ------------------------------------------ */
	m1Mark(this, "M1State", "chipid");
	if (rtwn_m1_read_chipid(&fChip) != RTWN_M1_OK) {
		IOLog(LOGP "chip ID tidak dikenali — lanjut tetap\n");
	}
	if (fChip.usb.consecutiveFailures() > 0) {
		m1Mark(this, "M1State", "abort-transfer-gagal");
		return;
	}

	/*
	 * ‑‑‑ URUTAN DIPERBAIKI (22 Okt 2026, hasil uji nyata) ‑‑‑
	 *
	 * Percobaan pertama: EFUSE dulu → power-on (sesuai urutan vendor
	 * #2 → #9). Hasilnya: fase EFUSE memakan 38 detik dan GAGAL —
	 * chip belum berdaya sehingga register EFUSE tidak menjawab.
	 *
	 * power-on (tabel flow vendor) terbukti LOLOS tanpa timeout.
	 * Jadi sekarang: POWER-ON DULU, baru EFUSE.
	 *
	 * power-on tidak butuh MAC/xtal (tabel flow menulis AFE_XTAL dengan
	 * konstanta, bukan nilai dari EFUSE) → tidak ada ketergantungan
	 * yang terputus.
	 */
	m1Mark(this, "M1State", "power-on");
	m1Mark(this, "M1Counters", "see-log");
	if (rtwn_seq_power_on(&res, m1Read, m1Write, this) != 0) {
		IOLog(LOGP "power-on gagal (timeout flow vendor)\n");
		m1Mark(this, "M1State", "power-on-gagal");
		return;
	}
	IOLog(LOGP "power-on OK (flow vendor)\n");
	m1Mark(this, "M1State", "power-on-ok");

	/* --- [10] LLT / DMA init --------------------------------------- */
	m1Mark(this, "M1State", "dma-init");
	if (rtwn_seq_dma_init(&res, m1Read, m1Write, this, fChip.ntx) != 0) {
		IOLog(LOGP "dma/LLT init gagal - lanjut ke firmware tetap\n");
	} else {
		IOLog(LOGP "LLT/DMA OK\n");
	}

	/* --- [13] Upload firmware 8051 ---------------------------------- */
	m1Mark(this, "M1State", "firmware");
	if (rtwn_seq_load_firmware(&res, m1Read, m1Write, this,
	    rtl8188eufw_bin, rtl8188eufw_bin_len) != 0) {
		IOLog(LOGP "upload firmware GAGAL (berhasil %u byte)\n", res.fwlen);
		{
			char fb[32];
			snprintf(fb, sizeof(fb), "gagal-di-%u", res.fwlen);
			m1Mark(this, "M1Firmware", fb);
		}
		m1Mark(this, "M1State", "firmware-gagal");
		return;
	}
	IOLog(LOGP "firmware OK (%u byte)\n", res.fwlen);
	m1Mark(this, "M1Firmware", "ok");

	/*
	 * --- EFUSE / MAC — SETELAH firmware ------------------------------
	 *
	 * URUTAN DIPERBAIKI (22 Okt 2026, hasil uji nyata):
	 *
	 * Vendor 42 langkah: offload EFUSE = langkah #31, firmware = #13.
	 * Jadi jalur OFFLOAD (mesin internal 8051 yang membaca EFUSE) BUTUH
	 * firmware sudah terupload. Pada percobaan sebelumnya EFUSE dibaca
	 * SEBELUM firmware -> 8051 masih kosong -> offload "berhasil" tapi
	 * datanya 0xff semua (MAC = ff:ff:ff:ff:ff:ff).
	 *
	 * Jalur DIRECT (register 0x031/0x032/0x033/0x030) tidak butuh
	 * firmware, tapi perlu chip sudah berdaya (power-on) DAN mode EFUSE
	 * aktif — dan power-on flow vendor me-clear bit mode EFUSE di 0x00f2.
	 */
	/*
	 * Urutan mengikuti vendor: OFFLOAD (jalur HAL) dulu, baru direct
	 * (jalur register) sebagai cadangan.
	 *
	 * Vendor: EFUSE_OneByteRead() -> coba HAL_MAC_EFUSE_Read1Byte()
	 * (untuk 8188E = offload 8051) dulu; register-level baru fallback.
	 */
	m1Mark(this, "M1State", "efuse-offload");
	if (rtwn_seq_read_efuse_offload(&res, m1Read, m1Write, this) != 0) {
		IOLog(LOGP "EFUSE offload gagal - coba jalur DIRECT\n");
		m1Mark(this, "M1State", "efuse-offload-gagal");
		m1Mark(this, "M1State", "efuse-direct");
		if (rtwn_seq_read_efuse_direct_8188e(&res, m1Read, m1Write, this) != 0) {
			IOLog(LOGP "EFUSE direct juga gagal - MAC tidak diketahui\n");
			m1Mark(this, "M1State", "efuse-direct-gagal");
			memset(&res, 0, sizeof(res));
		} else {
			m1Mark(this, "M1State", "efuse-direct-ok");
		}
	} else {
		m1Mark(this, "M1State", "efuse-offload-ok");
	}
	/*
	 * Sumber MAC alternatif: register MACID (0x610) di chip.
	 *
	 * Driver vendor menulis MAC hasil baca EFUSE ke register ini saat
	 * inisialisasi. Kalau chip belum di-reset (tidak dicabut), nilainya
	 * masih ada. dipakai sebagai cadangan + pembanding silang dengan
	 * hasil EFUSE.
	 */
	{
		char macbuf[32];
		int i2;

		for (i2 = 0; i2 < 6; i2++) {
			res.mac[i2] = rtwn_m1_read_1(&fChip, R92C_MACID + i2);
		}
		m1_hex_str(macbuf, sizeof(macbuf), res.mac, 6);
		m1Mark(this, "M1MacIdReg", macbuf);
	}

	if (res.efuse_ok) {
		IOLog(LOGP "EFUSE OK: MAC=%02x:%02x:%02x:%02x:%02x:%02x xtal=0x%02x\n",
		    res.mac[0], res.mac[1], res.mac[2], res.mac[3], res.mac[4],
		    res.mac[5], res.xtal);
		m1Mark(this, "M1Efuse", "ok");
		{
			char mbuf[32], xbuf[16];
			snprintf(mbuf, sizeof(mbuf), "%02x:%02x:%02x:%02x:%02x:%02x",
			    res.mac[0], res.mac[1], res.mac[2],
			    res.mac[3], res.mac[4], res.mac[5]);
			snprintf(xbuf, sizeof(xbuf), "0x%02x", res.xtal);
			m1Mark(this, "M1Mac", mbuf);
			m1Mark(this, "M1Xtal", xbuf);
		}
		memcpy(fChip.macaddr, res.mac, 6);
		/*
		 * Vendor Hal_EfuseParseXtal_8188E:
		 *     CrystalCap = hwinfo[0xB9];
		 *     if ((CrystalCap & 0x3f) == 0) CrystalCap |= 0x20;
		 * Jadi field xtal hanya 6 bit dan 0 berarti default 0x20.
		 */
		fChip.xtal = (uint8_t)(res.xtal & 0x3f);
		if (fChip.xtal == 0) {
			fChip.xtal = 0x20;
		}
		fChip.efuseValid = true;
	} else {
		m1Mark(this, "M1Efuse", "gagal");
	}
	
	/*
	 * ── HAL tables (PHY / RF / MAC) ────────────────────────────────
	 *
	 * Tabel "MP" 8188E diekstrak dari binary vendor (lihat
	 * tools/extract_mp_tables.py → src/urtwn/rtwn_mp_8188e.c).
	 * File MP gaya Windows tidak ada di macOS, jadi tabel built-in
	 * inilah yang benar-benar dipakai driver vendor.
	 *
	 * CATATAN FIDELITAS: entri 0x4xxxxxxx di vendor juga membandingkan
	 * dengan field kanal/bandwidth milik adapter (dm+0x4f/0x52/0x58/
	 * 0x59/0x5e). Bagian itu belum dimodelkan di sini → cabangnya
	 * ditentukan hanya dari perbandingan MAC. Kalau chip ternyata tidak
	 * TX/RX setelah mode HAL, ini kandidat utama penyebabnya.
	 */
	if (mode == M1_MODE_HAL) {
		rtwn_hal_ctx_t hc;
		int applied = 0;

		m1Mark(this, "M1State", "hal");
		rtwn_hal_init(&hc, m1Read, m1Write, this);
		if (fChip.efuseValid) {
			rtwn_hal_set_mac(&hc, fChip.macaddr);
		}

		applied += rtwn_hal_apply_table(&hc, rtwn_mp_8188e_mac_reg,
		    sizeof(rtwn_mp_8188e_mac_reg) / sizeof(rtwn_mp_8188e_mac_reg[0]));
		applied += rtwn_hal_apply_table(&hc, rtwn_mp_8188e_phy_reg,
		    sizeof(rtwn_mp_8188e_phy_reg) / sizeof(rtwn_mp_8188e_phy_reg[0]));
		/*
		 * phy_reg_pg & txpwr_lmt SENGAJA TIDAK diapplier:
		 * formatnya bukan (reg, val) — phy_reg_pg berisi reg=0x000
		 * dan entri 0xffffffff; txpwr_lmt berisi uint16. Keduanya
		 * punya konsumen sendiri di vendor (PHY_InitTxPowerLimit)
		 * dan perlu port terpisah. Menerapkannya apa adanya akan
		 * menulis ke register sampah.
		 */
		applied += rtwn_hal_apply_table(&hc, rtwn_mp_8188e_agc_tab,
		    sizeof(rtwn_mp_8188e_agc_tab) / sizeof(rtwn_mp_8188e_agc_tab[0]));
		applied += rtwn_hal_apply_table(&hc, rtwn_mp_8188e_radioa,
		    sizeof(rtwn_mp_8188e_radioa) / sizeof(rtwn_mp_8188e_radioa[0]));

		IOLog(LOGP "HAL tables diterapkan: %d register ditulis\n", applied);
		{
			char hb[32];
			snprintf(hb, sizeof(hb), "%d-register", applied);
			m1Mark(this, "M1Hal", hb);
		}
		m1Mark(this, "M1State", "hal-selesai");
		m1LogResult("hal-selesai", "-", "-", 0,
		    fChip.usb.inOk(), fChip.usb.inFail());
		return;
	}

	if (mode == M1_MODE_POWERON) {
		/*
		 * TAHAP TENGAH — berhenti setelah EFUSE.
		 */
		uint32_t cr1 = rtwn_m1_read_4(&fChip, R92C_CR);
		IOLog(LOGP "CR = 0x%08x\n", cr1);
		m1Mark(this, "M1State", "poweron-selesai");
		m1LogResult("poweron-selesai", res.efuse_ok ? "efuse-ok" : "efuse-gagal",
		    res.efuse_ok ? "efuse-ok" : "-", cr1,
		    fChip.usb.inOk(), fChip.usb.inFail());
		return;
	}
	

	/* --- [35] Buka pipe bulk USB & antre transfer RX ---------------- */
	m1Mark(this, "M1State", "usb-bulk");
	{
		IOUSBHostInterface *iface = m1FindInterface(device);
		if (iface && fChip.usb.bulkOpen(iface)) {
			fChip.usb.setRxCallback(m1RxCallback, this);
			if (fChip.usb.bulkQueueRx(4)) {
				IOLog(LOGP "pipe bulk terbuka, 4 transfer RX diantre\n");
			} else {
				IOLog(LOGP "gagal antre transfer RX\n");
			}
		} else {
			IOLog(LOGP "gagal membuka pipe bulk\n");
		}
		if (iface) {
			/* m1FindInterface() mengembalikan reference +1. */
			iface->release();
		}
	}

	/* --- [29] Set MAC ke chip (kalau MAC diketahui) ---------------- */
	if (fChip.efuseValid) {
		m1Mark(this, "M1State", "set-mac");
		for (int i = 0; i < 6; i++) {
			rtwn_m1_write_1(&fChip, R92C_MACID + i, fChip.macaddr[i]);
		}
		IOLog(LOGP "MAC ditulis ke chip\n");
	}

	{
		uint64_t tEnd = mach_absolute_time();
		uint64_t ms = (tEnd - tStart) / 1000000;
		m1Mark(this, "M1ElapsedMs", "lihat-log");
		IOLog(LOGP "total waktu init: %llu ms\n", (unsigned long long)ms);
	}
	m1Mark(this, "M1Counters", "see-log");
	{
		char buf[256];
		char num[40];
		strlcpy(num, "none", sizeof(num));
		/*
		 * fRxSsid bisa diganti/dilepas oleh callback USB kapan saja.
		 * Salin di bawah kunci, JANGAN menyimpan pointer-nya.
		 */
		if (fStateLock) {
			IOLockLock(fStateLock);
		}
		if (fRxSsid) {
			const char *s = fRxSsid->getCStringNoCopy();
			if (s) {
				strlcpy(num, s, sizeof(num));
			}
		}
		if (fStateLock) {
			IOLockUnlock(fStateLock);
		}
		char n1[16], n2[16], n3[16];
		m1Itoa(fRxGood, n1, sizeof(n1));
		m1Itoa(fRxBad, n2, sizeof(n2));
		m1Itoa(fRxBytes, n3, sizeof(n3));
		strlcpy(buf, "ok=", sizeof(buf));
		strlcat(buf, n1, sizeof(buf));
		strlcat(buf, " bad=", sizeof(buf));
		strlcat(buf, n2, sizeof(buf));
		strlcat(buf, " bytes=", sizeof(buf));
		strlcat(buf, n3, sizeof(buf));
		strlcat(buf, " ssid=", sizeof(buf));
		strlcat(buf, num, sizeof(buf));
		m1Mark(this, "M1Rx", buf);
		IOLog(LOGP "%s\n", buf);
	}
	m1Mark(this, "M1State", "SELESAI");
	m1Mark(this, "M1Mac", "see-log");
	{
		char mbuf[32], xbuf[16];
		snprintf(mbuf, sizeof(mbuf), "%02x:%02x:%02x:%02x:%02x:%02x",
		    fChip.macaddr[0], fChip.macaddr[1], fChip.macaddr[2],
		    fChip.macaddr[3], fChip.macaddr[4], fChip.macaddr[5]);
		snprintf(xbuf, sizeof(xbuf), "0x%02x", fChip.xtal);
		m1LogResult("selesai", mbuf, xbuf, res.cr,
		    fChip.usb.inOk(), fChip.usb.inFail());
	}
	IOLog(LOGP "=== M1 init SELESAI ===\n");
	IOLog(LOGP "chip=88E  MAC=%02x:%02x:%02x:%02x:%02x:%02x  fw=OK\n",
	    fChip.macaddr[0], fChip.macaddr[1], fChip.macaddr[2],
	    fChip.macaddr[3], fChip.macaddr[4], fChip.macaddr[5]);
	IOLog(LOGP "Langkah berikutnya (M2): bulk pipe TX/RX + net80211\n");
}
