/*
 * rtwn-probe — lab RTL8188EUS (2357:010c) dari USER SPACE.
 *
 * Tanpa kernel, tanpa reboot, tanpa risiko panic. Memakai modul portabel
 * src/urtwn/rtwn_seq.c — kode yang PERSIS sama dengan driver kernel.
 *
 * Keuntungan: setiap eksperimen (EFUSE, power-on, firmware, HAL, kanal,
 * scan) bisa diulang dalam hitungan DETIK, bukan menit/reboot.
 *
 * Build:
 *   make probe          (atau lihat scripts/build-probe.sh)
 *
 * Pakai:
 *   sudo ./tools/rtwn_probe                     # info + baca register
 *   sudo ./tools/rtwn_probe --efuse-all         # semua jalur EFUSE + diag
 *   sudo ./tools/rtwn_probe --full              # power-on + LLT + firmware
 *   sudo ./tools/rtwn_probe --scan 1 13         # scan 2,4 GHz
 *   sudo ./tools/rtwn_probe --join ZEKRI.NET --txnull
 *                                               # auth+assoc+uji TX data
 *   sudo ./tools/rtwn_probe --peek 0x610 6      # baca mentah
 *   sudo ./tools/rtwn_probe --poke 0x100 0xdeadbeef
 *   sudo ./tools/rtwn_probe --dump 0x000 0x40
 *
 * CATATAN: device harus TIDAK dipegang driver kernel. Kext kita self-detach
 * setelah init, jadi tunggu beberapa detik setelah dongle dicabut-colok.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/usb/USB.h>
#include <CoreFoundation/CoreFoundation.h>

#include "rtwn_seq.h"
#include "rtwn_init_8188e.h"
#include "rtwn_chan.h"
#include "rtwn_80211.h"
#include "rtwn_usb.h"

/* Firmware 8188E (dari linux-firmware), disematkan. */
extern const unsigned char rtl8188eufw_bin[];
extern const unsigned int rtl8188eufw_bin_len;

#define VID 0x2357
#define PID 0x010C
#define REQ_REGS 0x05

static IOUSBDeviceInterface **gDev = NULL;

static void diag_cb(void *ctx, const char *key, const char *val)
{
	(void)ctx;
	printf("      [diag] %-22s %s\n", key, val);
}

/* --- callback I/O untuk rtwn_seq --- */
static int probe_read(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
	IOUSBDevRequest req;

	(void)ctx;
	if (gDev == NULL) {
		return -1;
	}
	memset(&req, 0, sizeof(req));
	req.bmRequestType = 0xC0;	/* IN | VENDOR | DEVICE */
	req.bRequest = REQ_REGS;
	req.wValue = addr;
	req.wIndex = 0;
	req.wLength = len;
	req.pData = buf;
	req.wLenDone = 0;

	if ((*gDev)->DeviceRequest(gDev, &req) != kIOReturnSuccess) {
		return -1;
	}
	if (req.wLenDone != len) {
		return -1;
	}
	return 0;
}

static int probe_write(void *ctx, uint16_t addr, const void *buf, uint16_t len)
{
	IOUSBDevRequest req;

	(void)ctx;
	if (gDev == NULL) {
		return -1;
	}
	memset(&req, 0, sizeof(req));
	req.bmRequestType = 0x40;	/* OUT | VENDOR | DEVICE */
	req.bRequest = REQ_REGS;
	req.wValue = addr;
	req.wIndex = 0;
	req.wLength = len;
	req.pData = (void *)buf;
	req.wLenDone = 0;

	if ((*gDev)->DeviceRequest(gDev, &req) != kIOReturnSuccess) {
		return -1;
	}
	return 0;
}

static void print_result(const rtwn_seq_result_t *r)
{
	printf("  step=%d chip=%d efuse=%d power=%d dma=%d fw=%d\n",
	    r->step, r->chip_ok, r->efuse_ok, r->power_ok, r->dma_ok, r->fw_ok);
	printf("  MAC      = %02x:%02x:%02x:%02x:%02x:%02x\n",
	    r->mac[0], r->mac[1], r->mac[2], r->mac[3], r->mac[4], r->mac[5]);
	printf("  xtal=0x%02x regulatory=%u channel_plan=%u\n",
	    r->xtal, r->regulatory, r->channel_plan);
	printf("  CR=0x%08x MCUFWDL=0x%08x SYS_CFG=0x%08x fwlen=%u\n",
	    r->cr, r->mcuwdl, r->syscfg, r->fwlen);
}

/* ── buka device dengan API yang BENAR ─────────────────────────────────
 *
 * Kode lama memakai IOServiceOpen() lalu me-cast koneksi menjadi
 * IOUSBDeviceInterface** — itu salah; hasilnya kIOReturnNotOpen (0xe00002c7).
 *
 * Yang benar: IOCreatePlugInInterfaceForService(kIOUSBDeviceUserClientTypeID)
 *   → QueryInterface(kIOUSBDeviceInterfaceID)
 *   → USBDeviceOpen()
 */
static int open_device(void)
{
	CFMutableDictionaryRef match;
	io_iterator_t iter = 0;
	io_service_t svc = 0;
	IOCFPlugInInterface **plug = NULL;
	SInt32 score = 0;
	IOReturn kr;
	int vidVal = VID, pidVal = PID;
	CFNumberRef vid, pid;
	UInt8 cfg = 0;

	match = IOServiceMatching("IOUSBDevice");
	vid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vidVal);
	pid = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pidVal);
	CFDictionarySetValue(match, CFSTR("idVendor"), vid);
	CFDictionarySetValue(match, CFSTR("idProduct"), pid);
	CFRelease(vid);
	CFRelease(pid);

	if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) !=
	    KERN_SUCCESS) {
		printf("!! IOServiceGetMatchingServices gagal\n");
		return -1;
	}
	svc = IOIteratorNext(iter);
	IOObjectRelease(iter);
	if (!svc) {
		printf("!! Device 2357:010c tidak ditemukan (dongle tercabut?)\n");
		return -2;
	}

	kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
	    kIOCFPlugInInterfaceID, &plug, &score);
	if (kr != KERN_SUCCESS || plug == NULL) {
		printf("!! IOCreatePlugInInterfaceForService gagal: 0x%08x\n", kr);
		IOObjectRelease(svc);
		return -3;
	}

	kr = (*plug)->QueryInterface(plug,
	    CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&gDev);
	(*plug)->Release(plug);
	IOObjectRelease(svc);
	if (kr != KERN_SUCCESS || gDev == NULL) {
		printf("!! QueryInterface(kIOUSBDeviceInterfaceID) gagal: 0x%08x\n",
		    kr);
		return -4;
	}

	kr = (*gDev)->USBDeviceOpen(gDev);
	if (kr != kIOReturnSuccess) {
		printf("  USBDeviceOpen: 0x%08x → coba USBDeviceOpenSeize...\n", kr);
		kr = (*gDev)->USBDeviceOpenSeize(gDev);
	}
	if (kr != kIOReturnSuccess) {
		printf("!! tidak bisa membuka device: 0x%08x\n", kr);
		if (kr == kIOReturnExclusiveAccess) {
			printf("   → masih dipegang driver kernel.\n");
			printf("     Kext kita self-detach; tunggu ~10 dtk lalu ulangi.\n");
			printf("     Jika tetap: sudo kextunload -b "
			    "io.github.zekri.RTL8188EUM1\n");
		}
		(*gDev)->Release(gDev);
		gDev = NULL;
		return -5;
	}
	(*gDev)->GetConfiguration(gDev, &cfg);
	printf("  device dibuka ✓ (config=%u)\n", cfg);
	if (cfg == 0) {
		kr = (*gDev)->SetConfiguration(gDev, 1);
		printf("  SetConfiguration(1) → 0x%08x\n", kr);
	}
	return 0;
}

static void close_device(void)
{
	if (gDev) {
		(*gDev)->USBDeviceClose(gDev);
		(*gDev)->Release(gDev);
		gDev = NULL;
	}
}

/* ── bulk pipe (interface 0) ─────────────────────────────────────────── */

static IOUSBInterfaceInterface **gIf = NULL;
static UInt8 gPipeIn = 0;	/* pipeRef untuk ReadPipe  */
static UInt8 gPipeOut = 0;	/* pipeRef untuk WritePipe (prioritas tertinggi) */
static UInt8 gPipeOutAll[3];	/* semua bulk OUT, urutan descriptor */
static int gNumOut = 0;
static UInt8 gEpIn = 0, gEpOut = 0;
static UInt16 gMpsIn = 0, gMpsOut = 0;

static int
get_int_prop(io_service_t svc, CFStringRef key, int *out)
{
	CFTypeRef ref;
	CFNumberRef num;
	int v = 0;
	int ok = 0;

	ref = IORegistryEntryCreateCFProperty(svc, key, kCFAllocatorDefault, 0);
	if (ref == NULL) {
		return -1;
	}
	if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
		num = (CFNumberRef)ref;
		if (CFNumberGetValue(num, kCFNumberIntType, &v)) {
			*out = v;
			ok = 1;
		}
	}
	CFRelease(ref);
	return ok ? 0 : -1;
}

/*
 * Buka interface 0 dan cari pipe bulk IN/OUT.
 *
 * Harus dipanggil SETELAH open_device() (yang melakukan SetConfiguration).
 * Memakai API legacy IOUSBInterfaceInterface — API yang sama dipakai
 * IOUSBDeviceInterface di atas, dan masih berfungsi di macOS 15.
 */
static int
open_interface(void)
{
	CFMutableDictionaryRef match;
	io_iterator_t iter = 0;
	io_service_t svc = 0;
	IOCFPlugInInterface **plug = NULL;
	SInt32 score = 0;
	IOReturn kr;
	int found = 0, vid = 0, pid = 0;
	UInt8 numEp = 0, i;

	match = IOServiceMatching("IOUSBInterface");
	if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) !=
	    KERN_SUCCESS) {
		printf("!! IOServiceGetMatchingServices(IOUSBInterface) gagal\n");
		return -1;
	}

	while ((svc = IOIteratorNext(iter)) != 0) {
		if (get_int_prop(svc, CFSTR("idVendor"), &vid) != 0 ||
		    get_int_prop(svc, CFSTR("idProduct"), &pid) != 0 ||
		    vid != VID || pid != PID) {
			IOObjectRelease(svc);
			continue;
		}
		/* interface 0 = vendor specific (bInterfaceNumber 0) */
		{
			int ifno = -1;
			if (get_int_prop(svc, CFSTR("bInterfaceNumber"),
			    &ifno) == 0 && ifno != 0) {
				IOObjectRelease(svc);
				continue;
			}
		}

		kr = IOCreatePlugInInterfaceForService(svc,
		    kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID,
		    &plug, &score);
		if (kr != KERN_SUCCESS || plug == NULL) {
			IOObjectRelease(svc);
			continue;
		}
		kr = (*plug)->QueryInterface(plug,
		    CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),
		    (LPVOID *)&gIf);
		(*plug)->Release(plug);
		plug = NULL;
		if (kr != KERN_SUCCESS || gIf == NULL) {
			IOObjectRelease(svc);
			continue;
		}

		kr = (*gIf)->USBInterfaceOpen(gIf);
		if (kr == kIOReturnExclusiveAccess) {
			kr = (*gIf)->USBInterfaceOpenSeize(gIf);
		}
		if (kr != kIOReturnSuccess) {
			printf("  USBInterfaceOpen: 0x%08x\n", kr);
			(*gIf)->Release(gIf);
			gIf = NULL;
			IOObjectRelease(svc);
			continue;
		}
		found = 1;
		IOObjectRelease(svc);
		break;
	}
	IOObjectRelease(iter);

	if (!found || gIf == NULL) {
		printf("!! interface 0 tidak bisa dibuka\n");
		return -2;
	}

	kr = (*gIf)->GetNumEndpoints(gIf, &numEp);
	if (kr != kIOReturnSuccess) {
		printf("!! GetNumEndpoints: 0x%08x\n", kr);
		return -3;
	}
	printf("  interface 0 dibuka ✓ (%u endpoint)\n", numEp);

	for (i = 1; i <= numEp; i++) {
		UInt8 dir = 0, num = 0, type = 0, interval = 0;
		UInt16 mps = 0;

		kr = (*gIf)->GetPipeProperties(gIf, i, &dir, &num, &type,
		    &mps, &interval);
		if (kr != kIOReturnSuccess) {
			continue;
		}
		printf("    pipe %u: ep=0x%02x dir=%s type=%u mps=%u\n", i, num,
		    dir == kUSBIn ? "IN" : "OUT", type, mps);
		if (type != kUSBBulk) {
			continue;
		}
		if (dir == kUSBIn && gPipeIn == 0) {
			gPipeIn = i;
			gEpIn = num;
			gMpsIn = mps;
		} else if (dir == kUSBOut && gNumOut < 3) {
			/* Urutan endpoint = queue: 0 VO/mgmt, 1 VI, 2 BE/BK. */
			gPipeOutAll[gNumOut] = i;
			if (gNumOut == 0) {
				gPipeOut = i;
				gEpOut = num;
				gMpsOut = mps;
			}
			gNumOut++;
		}
	}

	if (gPipeIn == 0 || gPipeOut == 0) {
		printf("!! pipe bulk tidak lengkap (in=%u out=%u)\n", gPipeIn,
		    gPipeOut);
		return -4;
	}
	printf("  bulk: IN ep=0x%02x (pipe %u, mps %u), OUT %d pipe "
	    "(ep0=0x%02x, pipe %u, mps %u)\n", gEpIn, gPipeIn, gMpsIn,
	    gNumOut, gEpOut, gPipeOut, gMpsOut);
	return 0;
}

static void
close_interface(void)
{
	if (gIf) {
		(*gIf)->USBInterfaceClose(gIf);
		(*gIf)->Release(gIf);
		gIf = NULL;
	}
}

static int
bulk_write_pipe(int idx, const void *buf, uint32_t len, uint32_t timeout_ms)
{
	IOReturn kr;
	UInt8 pipe;

	if (gIf == NULL || gNumOut <= 0) {
		return -1;
	}
	if (idx < 0) {
		idx = 0;
	}
	if (idx >= gNumOut) {
		idx = gNumOut - 1;
	}
	pipe = gPipeOutAll[idx];

	kr = (*gIf)->WritePipeTO(gIf, pipe, (void *)buf, len, 0, timeout_ms);
	if (kr != kIOReturnSuccess) {
		printf("    bulk OUT gagal (pipe %u): 0x%08x\n", pipe, kr);
		return -1;
	}
	return 0;
}

static int
bulk_write(const void *buf, uint32_t len, uint32_t timeout_ms)
{
	return bulk_write_pipe(0, buf, len, timeout_ms);
}

static int
bulk_read(void *buf, uint32_t *len, uint32_t timeout_ms)
{
	IOReturn kr;

	if (gIf == NULL || gPipeIn == 0) {
		return -1;
	}
	kr = (*gIf)->ReadPipeTO(gIf, gPipeIn, buf, len, 0, timeout_ms);
	if (kr == kIOReturnTimeout || kr == kIOUSBTransactionTimeout) {
		return 1;	/* timeout, bukan error */
	}
	if (kr != kIOReturnSuccess) {
		return -1;
	}
	return 0;
}

/* ── scan M2 ─────────────────────────────────────────────────────────── */

static void
hexdump(const char *tag, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	printf("      %s (%u):", tag, n);
	for (i = 0; i < n && i < 48; i++) {
		printf(" %02x", b[i]);
	}
	if (n > 48) {
		printf(" ...");
	}
	printf("\n");
}

/* Apakah frame ini beacon / probe response yang masuk akal? */
static bool
looks_like_beacon(const uint8_t *f, uint32_t len)
{
	uint32_t n, end;
	int subtypes;

	if (len < 36) {
		return false;
	}
	if ((f[0] & 0x03) != 0) {	/* version 0 */
		return false;
	}
	if (((f[0] >> 2) & 0x03) != 0) {	/* management */
		return false;
	}
	subtypes = f[0] & 0xf0;
	if (subtypes != 0x80 && subtypes != 0x50) {
		return false;
	}

	/* Body beacon = 24 header + 12 fixed. Elemen mulai di 36. */
	n = 36;
	end = len;
	while (n + 2 <= end) {
		uint8_t id = f[n];
		uint8_t elen = f[n + 1];
		if (n + 2 + elen > end) {
			return false;
		}
		if (id == 0x00 && elen <= 32) {
			return true;
		}
		n += 2 + elen;
	}
	return false;
}

/*
 * Coba beberapa offset frame di dalam buffer RX.
 *
 * Descriptor 24 byte, lalu PHY status (infosz), lalu frame. Vendor
 * menambahkan 2 byte lain (rfd[0x2F2] + rfd[0x2F3]); kita coba semua
 * kandidat dan pilih yang benar-benar berisi beacon.
 */
static const uint8_t *
find_frame(const uint8_t *buf, uint32_t len, const rtwn_usb_rxinfo_t *info,
    uint32_t *foff_out)
{
	static const uint32_t extra[] = { 0, 2 };
	uint32_t base, k;
	size_t i;

	for (k = 0; k < 2; k++) {
		base = RTWN_USB_RX_DESC_SIZE + info->infosz + extra[k];
		if (base > len) {
			continue;
		}
		if (looks_like_beacon(buf + base, len - base)) {
			*foff_out = base;
			return buf + base;
		}
		/* Kalau descriptor tidak melaporkan infosz, coba tanpa itu. */
		base = RTWN_USB_RX_DESC_SIZE + extra[k];
		if (base <= len && looks_like_beacon(buf + base, len - base)) {
			*foff_out = base;
			return buf + base;
		}
	}
	(void)i;
	return NULL;
}

static void
print_ssid(const char *ssid, uint8_t len)
{
	uint8_t i;

	if (len == 0) {
		printf("<hidden>");
		return;
	}
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)ssid[i];
		if (c >= 0x20 && c < 0x7f) {
			putchar(c);
		} else {
			printf("\\x%02x", c);
		}
	}
}

static int
run_scan(int ch_start, int ch_end)
{
	rtwn_seq_result_t res, ef;
	uint8_t txdesc[RTWN_USB_TX_DESC_SIZE];
	uint8_t txbuf[RTWN_USB_TX_DESC_SIZE + 128];
	uint8_t rbuf[16384];
	char ssid[33];
	uint8_t ssid_len, bssid[6];
	int chan, i, seen = 0, frames = 0, probes = 0;
	int verbose = 0;

	if (ch_start < 1) ch_start = 1;
	if (ch_end > 14) ch_end = 14;
	if (ch_end < ch_start) ch_end = ch_start;

	memset(&res, 0, sizeof(res));

	printf("── Init untuk scan ──\n");
	if (rtwn_seq_power_on(&res, probe_read, probe_write, NULL) != 0) {
		printf("  ✗ power-on gagal\n");
		return -1;
	}
	printf("  power-on ✓\n");
	if (rtwn_seq_dma_init(&res, probe_read, probe_write, NULL, 3) != 0) {
		printf("  ✗ DMA/LLT gagal\n");
		return -1;
	}
	printf("  DMA/LLT ✓\n");
	if (rtwn_seq_load_firmware(&res, probe_read, probe_write, NULL,
	    rtl8188eufw_bin, rtl8188eufw_bin_len) != 0) {
		printf("  ✗ firmware gagal\n");
		return -1;
	}
	printf("  firmware ✓ (%u byte)\n", res.fwlen);

	/* MAC + xtal dari EFUSE (butuh firmware). */
	memset(&ef, 0, sizeof(ef));
	if (rtwn_seq_read_efuse_offload(&ef, probe_read, probe_write, NULL) == 0 &&
	    ef.efuse_ok) {
		res.mac[0] = ef.mac[0]; res.mac[1] = ef.mac[1];
		res.mac[2] = ef.mac[2]; res.mac[3] = ef.mac[3];
		res.mac[4] = ef.mac[4]; res.mac[5] = ef.mac[5];
		res.xtal = ef.xtal;
		printf("  EFUSE ✓ MAC=%02x:%02x:%02x:%02x:%02x:%02x xtal=0x%02x\n",
		    res.mac[0], res.mac[1], res.mac[2], res.mac[3], res.mac[4],
		    res.mac[5], res.xtal);
	} else {
		printf("  ! EFUSE gagal — pakai MAC default 02:00:00:00:00:01\n");
		res.mac[0] = 0x02; res.mac[5] = 0x01;
		res.xtal = 0x20;
	}

	printf("── MAC/BB/RF init (tabel OpenBSD) ──\n");
	printf("  MAC: %d register\n",
	    rtwn_init_mac_8188e(probe_read, probe_write, NULL));
	printf("  BB : %d register\n",
	    rtwn_init_bb_8188e(probe_read, probe_write, NULL, res.xtal));
	printf("  RF : %d register\n",
	    rtwn_init_rf_8188e(probe_read, probe_write, NULL));
	printf("  core config: %d\n",
	    rtwn_core_config_8188e(probe_read, probe_write, NULL, res.mac));

	usleep(20 * 1000);

	printf("── Scan kanal %d..%d ──\n", ch_start, ch_end);
	for (chan = ch_start; chan <= ch_end; chan++) {
		size_t flen = 0;
		uint32_t want;

		if (rtwn_set_chan_8188e(probe_read, probe_write, NULL,
		    (uint8_t)chan) != 0) {
			printf("  ch %2d: set kanal gagal\n", chan);
			continue;
		}
		{
			uint32_t rfv = rtwn_rf_read_8188e(probe_read,
			    probe_write, NULL, 0, 0x18);
			printf("  ch %2d  RF18=0x%06x\n", chan, rfv);
		}
		usleep(15 * 1000);

		/* Buang paket sisa kanal sebelumnya (RX aggregation). */
		for (i = 0; i < 4; i++) {
			uint32_t dl = sizeof(rbuf);
			if (bulk_read(rbuf, &dl, 20) != 0 || dl == 0) {
				break;
			}
		}

		/* Probe request wildcard (scan semua SSID). */
		if (!rtwn_build_probe_request(
		    (const uint8_t *)"\xff\xff\xff\xff\xff\xff", res.mac,
		    "", 0, (uint8_t)chan, txbuf + RTWN_USB_TX_DESC_SIZE, 128,
		    &flen)) {
			printf("  ch %2d: gagal bangun frame\n", chan);
			continue;
		}
		if (!rtwn_usb_build_mgmt_tx_desc(txdesc, sizeof(txdesc),
		    (uint16_t)flen, true)) {
			continue;
		}
		memcpy(txbuf, txdesc, RTWN_USB_TX_DESC_SIZE);

		if (bulk_write(txbuf, RTWN_USB_TX_DESC_SIZE + (uint32_t)flen,
		    500) != 0) {
			printf("  ch %2d: TX gagal\n", chan);
			continue;
		}
		probes++;

		/* Baca hasil selama ±300 ms. */
		for (i = 0; i < 6; i++) {
			uint8_t *b = rbuf;
			uint32_t len = sizeof(rbuf);
			int rc;

			rc = bulk_read(b, &len, 60);
			if (rc == 1 || len == 0) {
				continue;
			}
			if (rc < 0) {
				printf("  ch %2d: RX error\n", chan);
				break;
			}

			/* Satu transfer bisa berisi beberapa paket. */
			want = 0;
			while (want + RTWN_USB_RX_DESC_SIZE <= len) {
				rtwn_usb_rxinfo_t info;
				const uint8_t *f;
				uint32_t foff = 0, plen;

				if (!rtwn_usb_parse_rx_desc(b + want,
				    len - want, &info)) {
					break;
				}
				plen = info.pktlen;
				if (plen == 0 || plen > 3000) {
					break;
				}
				if (verbose < 4) {
					printf("    rx len=%u pktlen=%u "
					    "infosz=%u rate=%u crc=%d\n",
					    len, info.pktlen, info.infosz,
					    info.rate, info.crcError);
					hexdump("desc+", b + want,
					    len - want);
					verbose++;
				}
				f = find_frame(b + want, len - want, &info,
				    &foff);
				if (f != NULL) {
					uint32_t flen2 = len - want - foff;
					if (verbose < 6) {
						printf("      frame @%u: "
						    "%02x %02x %02x %02x | "
						    "%02x %02x %02x %02x "
						    "%02x %02x %02x %02x\n",
						    foff, f[0], f[1], f[2], f[3],
						    f[4], f[5], f[6], f[7],
						    f[8], f[9], f[10], f[11]);
						if (getenv("RTWN_DUMPFRAME") != NULL) {
							uint32_t z;
							printf("      RAWFRAME:");
							for (z = 0; z < flen2 && z < 400; z++)
								printf(" %02x", f[z]);
							printf("\n");
						}
						verbose++;
					}
					frames++;
					if (rtwn_parse_ssid(f, flen2, ssid,
					    &ssid_len) &&
					    rtwn_parse_bssid(f, flen2,
					    bssid)) {
						printf("  ch %2d  %02x:%02x:%02x:"
						    "%02x:%02x:%02x  ", chan,
						    bssid[0], bssid[1],
						    bssid[2], bssid[3],
						    bssid[4], bssid[5]);
						print_ssid(ssid, ssid_len);
						printf("\n");
						seen++;
					}
				}
				want += RTWN_USB_RX_DESC_SIZE + info.infosz +
				    plen;
				if (want & 3) {
					want = (want + 3) & ~3u;
				}
			}
		}
		usleep(40 * 1000);
	}

	printf("── Hasil: %d probe TX, %d frame, %d SSID ──\n", probes,
	    frames, seen);
	return seen > 0 ? 0 : 1;
}

static uint32_t hexarg(const char *s)
{
	return (uint32_t)strtoul(s, NULL, 0);
}

/* ── M3 userspace lab: join + uji TX data ───────────────────────────── */

struct rxframe {
	const uint8_t *p;
	uint32_t len;
	uint32_t dw3;
	bool txrep;
};

struct rxstat {
	int transfers;
	int frames;
	int txrep;
	int txok;
	int crc;
};

/*
 * Ambil frame dari satu transfer RX.
 *
 * Offset frame = 24 (descriptor) + infosz (PHY status) + shift.
 * Vendor kadang menambah 2 byte; kalau frame di offset utama tidak masuk
 * akal, coba kandidat lain.
 */
static const uint8_t *
frame_in_slot(const uint8_t *b, uint32_t len, const rtwn_usb_rxinfo_t *info,
    uint32_t *foff)
{
	static const int32_t adj[] = { 0, 2, -2 };
	size_t k;

	for (k = 0; k < sizeof(adj) / sizeof(adj[0]); k++) {
		int32_t off = (int32_t)(RTWN_USB_RX_DESC_SIZE + info->infosz) +
		    adj[k];
		if (off < 0 || (uint32_t)off + 24 > len) {
			continue;
		}
		if ((b[off] & 0x03) == 0 &&
		    ((b[off] >> 2) & 0x03) <= 2) {
			*foff = (uint32_t)off;
			return b + off;
		}
	}
	/* fallback: tanpa infosz */
	if (RTWN_USB_RX_DESC_SIZE + 24 <= len) {
		*foff = RTWN_USB_RX_DESC_SIZE;
		return b + RTWN_USB_RX_DESC_SIZE;
	}
	return NULL;
}

/*
 * Baca satu transfer, kembalikan frame yang ditemukan.
 * Return jumlah frame (0 kalau timeout/kosong), -1 kalau error.
 */
static int
rx_collect(uint8_t *buf, uint32_t bufsz, uint32_t timeout_ms,
    struct rxframe *out, int max, struct rxstat *st)
{
	uint32_t len = bufsz, want = 0;
	int rc, n = 0;

	rc = bulk_read(buf, &len, timeout_ms);
	if (rc == 1 || len == 0) {
		return 0;
	}
	if (rc < 0) {
		return -1;
	}
	if (st) {
		st->transfers++;
	}

	while (want + RTWN_USB_RX_DESC_SIZE <= len && n < max) {
		rtwn_usb_rxinfo_t info;
		const uint8_t *f;
		uint32_t foff = 0, stride, dw3;
		bool isrep;

		if (!rtwn_usb_parse_rx_desc(buf + want, len - want, &info)) {
			break;
		}
		if (info.pktlen == 0 || info.pktlen > 3000) {
			break;
		}

		if (st && (info.crcError || info.icvError)) {
			st->crc++;
		}

		dw3 = (uint32_t)buf[want + 12] |
		    ((uint32_t)buf[want + 13] << 8) |
		    ((uint32_t)buf[want + 14] << 16) |
		    ((uint32_t)buf[want + 15] << 24);
		isrep = ((dw3 >> 14) & 0x3) != 0;

		f = frame_in_slot(buf + want, len - want, &info, &foff);
		/*
		 * TX report BUKAN frame 802.11: payload-nya struct
		 * r88e_tx_rpt_ccx. Jangan disaring oleh frame_in_slot —
		 * kalau disaring, report yang ada pun tidak terhitung.
		 */
		if (f != NULL || isrep) {
			out[n].p = f ? f : (buf + want + foff);
			out[n].len = info.pktlen;
			out[n].dw3 = dw3;
			out[n].txrep = isrep;
			if (st) {
				st->frames++;
				if (isrep) {
					st->txrep++;
				}
			}
			n++;
		}

		stride = RTWN_USB_RX_DESC_SIZE + info.infosz + info.shift +
		    info.pktlen;
		stride = (stride + 3) & ~3u;
		if (stride == 0 || want + stride > len) {
			break;
		}
		want += stride;
	}
	return n;
}

/* Init chip lengkap (sama dengan jalur kernel, kode portabel). */
static int
chip_bringup(rtwn_seq_result_t *res)
{
	rtwn_seq_result_t ef;

	memset(res, 0, sizeof(*res));

	if (rtwn_seq_power_on(res, probe_read, probe_write, NULL) != 0) {
		printf("  ✗ power-on gagal\n");
		return -1;
	}
	if (rtwn_seq_dma_init(res, probe_read, probe_write, NULL, 3) != 0) {
		printf("  ✗ DMA/LLT gagal\n");
		return -1;
	}
	if (rtwn_seq_load_firmware(res, probe_read, probe_write, NULL,
	    rtl8188eufw_bin, rtl8188eufw_bin_len) != 0) {
		printf("  ✗ firmware gagal\n");
		return -1;
	}

	memset(&ef, 0, sizeof(ef));
	if (rtwn_seq_read_efuse_offload(&ef, probe_read, probe_write, NULL) == 0 &&
	    ef.efuse_ok) {
		memcpy(res->mac, ef.mac, 6);
		res->xtal = ef.xtal;
	} else if (rtwn_seq_read_efuse_direct_8188e(&ef, probe_read,
	    probe_write, NULL) == 0 && ef.efuse_ok) {
		memcpy(res->mac, ef.mac, 6);
		res->xtal = ef.xtal;
	} else {
		res->mac[0] = 0x02;
		res->mac[5] = 0x01;
		res->xtal = 0x20;
	}

	rtwn_init_mac_8188e(probe_read, probe_write, NULL);
	rtwn_init_bb_8188e(probe_read, probe_write, NULL, res->xtal);
	rtwn_init_rf_8188e(probe_read, probe_write, NULL);
	rtwn_core_config_8188e(probe_read, probe_write, NULL, res->mac);

	usleep(20 * 1000);
	printf("  chip siap: MAC %02x:%02x:%02x:%02x:%02x:%02x xtal=0x%02x\n",
	    res->mac[0], res->mac[1], res->mac[2], res->mac[3], res->mac[4],
	    res->mac[5], res->xtal);
	return 0;
}

static int
parse_mac(const char *s, uint8_t mac[6])
{
	unsigned v[6];
	int i;

	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4],
	    &v[5]) != 6) {
		return -1;
	}
	for (i = 0; i < 6; i++) {
		mac[i] = (uint8_t)v[i];
	}
	return 0;
}

/* Cari BSSID + kanal untuk SSID tertentu lewat directed probe. */
static int
find_target(const char *ssid, const uint8_t sta[6], uint8_t bssid[6],
    int *chan_out)
{
	uint8_t txbuf[RTWN_USB_TX_DESC_SIZE + 128];
	uint8_t rbuf[16384];
	uint8_t target_len = (uint8_t)strlen(ssid);
	int chan;

	for (chan = 1; chan <= 13; chan++) {
		struct rxframe fr[16];
		struct rxstat st;
		size_t flen = 0;
		int k, i;

		if (rtwn_set_chan_8188e(probe_read, probe_write, NULL,
		    (uint8_t)chan) != 0) {
			continue;
		}
		usleep(12 * 1000);

		for (i = 0; i < 3; i++) {
			uint32_t dl = sizeof(rbuf);
			if (bulk_read(rbuf, &dl, 5) != 0 || dl == 0) {
				break;
			}
		}

		if (!rtwn_build_probe_request(
		    (const uint8_t *)"\xff\xff\xff\xff\xff\xff", sta, ssid,
		    target_len, (uint8_t)chan,
		    txbuf + RTWN_USB_TX_DESC_SIZE, 128, &flen)) {
			continue;
		}
		if (!rtwn_usb_build_mgmt_tx_desc(txbuf, RTWN_USB_TX_DESC_SIZE,
		    (uint16_t)flen, true)) {
			continue;
		}
		if (bulk_write(txbuf, RTWN_USB_TX_DESC_SIZE + (uint32_t)flen,
		    300) != 0) {
			continue;
		}

		memset(&st, 0, sizeof(st));
		for (i = 0; i < 4; i++) {
			int n = rx_collect(rbuf, sizeof(rbuf), 40, fr, 16, &st);

			if (n <= 0) {
				continue;
			}
			for (k = 0; k < n; k++) {
				char got[33];
				uint8_t got_len = 0;
				uint8_t b[6];

				if (fr[k].txrep) {
					continue;
				}
				if (!rtwn_parse_ssid(fr[k].p, fr[k].len, got,
				    &got_len) || !rtwn_parse_bssid(fr[k].p,
				    fr[k].len, b)) {
					continue;
				}
				if (got_len == target_len &&
				    memcmp(got, ssid, got_len) == 0) {
					memcpy(bssid, b, 6);
					*chan_out = chan;
					return 0;
				}
			}
		}
	}
	return -1;
}

/* Kirim Deauth supaya AP tidak menyimpan asosiasi lama (tes bisa diulang). */
static void
send_deauth(const uint8_t bssid[6], const uint8_t sta[6], uint16_t seq)
{
	uint8_t buf[RTWN_USB_TX_DESC_SIZE + 32];
	uint8_t *f = buf + RTWN_USB_TX_DESC_SIZE;
	size_t flen = 26;

	memset(buf, 0, sizeof(buf));
	f[0] = 0xc0;			/* mgmt / deauth */
	f[1] = 0x00;
	memcpy(f + 4, bssid, 6);
	memcpy(f + 10, sta, 6);
	memcpy(f + 16, bssid, 6);
	f[22] = (uint8_t)((seq << 4) & 0xff);
	f[23] = (uint8_t)(((seq << 4) >> 8) & 0xff);
	f[24] = 0x03;			/* reason: STA leaving */
	f[25] = 0x00;

	if (!rtwn_usb_build_mgmt_tx_desc(buf, RTWN_USB_TX_DESC_SIZE,
	    (uint16_t)flen, false)) {
		return;
	}
	bulk_write(buf, RTWN_USB_TX_DESC_SIZE + (uint32_t)flen, 300);
	usleep(20 * 1000);
}

/*
 * Join (open auth + assoc) lalu — kalau diminta — kirim QoS-Null untuk
 * membuktikan jalur TX data. Tidak ada kripto di sini: tujuan tes ini
 * memisahkan "TX data rusak" dari "WPA2 gagal".
 */
static int
run_join(const char *ssid, const char *bssid_opt, int force_chan, int doTxNull,
    const char *mac_opt)
{
	rtwn_seq_result_t res;
	uint8_t bssid[6] = { 0 };
	uint8_t txbuf[RTWN_USB_TX_DESC_SIZE + 256];
	uint8_t rbuf[16384];
	int chan = force_chan, attempt, ret = 1;
	uint16_t seq = 0x10;

	printf("── Bring-up chip ──\n");
	if (chip_bringup(&res) != 0) {
		return -1;
	}

	/*
	 * MAC alternatif: AP sering menyimpan state "already associated" untuk
	 * MAC yang sama, sehingga assoc berikutnya diabaikan. Untuk pengujian
	 * berulang, pakai MAC baru tiap run.
	 */
	if (mac_opt != NULL) {
		uint8_t m[6];

		if (strcmp(mac_opt, "rand") == 0) {
			uint32_t r = (uint32_t)time(NULL) ^ (uint32_t)getpid();

			m[0] = 0x02;			/* locally administered */
			m[1] = (uint8_t)(r & 0xff);
			m[2] = (uint8_t)((r >> 8) & 0xff);
			m[3] = (uint8_t)((r >> 16) & 0xff);
			m[4] = (uint8_t)((r >> 24) & 0xff);
			m[5] = (uint8_t)((r ^ 0x5a) & 0xff);
		} else if (parse_mac(mac_opt, m) != 0) {
			printf("!! --mac tidak valid\n");
			return -1;
		}
		memcpy(res.mac, m, 6);
		for (int i = 0; i < 6; i++) {
			probe_write(NULL, (uint16_t)(0x610 + i), &m[i], 1);
		}
		printf("  MAC uji: %02x:%02x:%02x:%02x:%02x:%02x\n",
		    m[0], m[1], m[2], m[3], m[4], m[5]);
	}

	if (bssid_opt != NULL) {
		if (parse_mac(bssid_opt, bssid) != 0) {
			printf("!! --bssid tidak valid\n");
			return -1;
		}
		printf("── BSSID diberikan: %s\n", bssid_opt);
	} else {
		printf("── Cari \"%s\" di kanal 1..13 ──\n", ssid);
		if (find_target(ssid, res.mac, bssid, &chan) != 0) {
			printf("  ✗ SSID tidak ditemukan (AP mati / di luar jangkauan)\n");
			return -1;
		}
	}

	if (chan == 0) {
		chan = 1;
		printf("  ! kanal tidak diketahui, pakai 1 (sebut --chan untuk memaksa)\n");
	}
	printf("  target %02x:%02x:%02x:%02x:%02x:%02x kanal %d\n",
	    bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], chan);

	if (rtwn_set_chan_8188e(probe_read, probe_write, NULL,
	    (uint8_t)chan) != 0) {
		printf("  ✗ set kanal gagal\n");
		return -1;
	}
	usleep(15 * 1000);

	/* Bersihkan asosiasi lama dari run sebelumnya (kalau ada). */
	send_deauth(bssid, res.mac, seq++);

	/* ── auth open ── */
	printf("── Auth (open) ──\n");
	for (attempt = 0; attempt < 3; attempt++) {
		size_t flen = 0;
		int i, got = 0;

		if (!rtwn_build_auth_req(bssid, res.mac, seq++, 0, 1,
		    txbuf + RTWN_USB_TX_DESC_SIZE, 128, &flen)) {
			break;
		}
		if (!rtwn_usb_build_mgmt_tx_desc(txbuf, RTWN_USB_TX_DESC_SIZE,
		    (uint16_t)flen, false)) {
			break;
		}
		if (bulk_write(txbuf, RTWN_USB_TX_DESC_SIZE + (uint32_t)flen,
		    300) != 0) {
			continue;
		}

		for (i = 0; i < 8 && !got; i++) {
			struct rxframe fr[16];
			int n = rx_collect(rbuf, sizeof(rbuf), 40, fr, 16, NULL);
			int k;

			if (n <= 0) {
				continue;
			}
			for (k = 0; k < n; k++) {
				uint16_t status = 0;

				if (fr[k].txrep ||
				    !rtwn_frame_for(fr[k].p, fr[k].len, res.mac)) {
					continue;
				}
				if (rtwn_parse_auth_resp(fr[k].p, fr[k].len,
				    &status)) {
					printf("  ← auth resp status=%u\n",
					    status);
					got = 1;
					break;
				}
			}
		}
		if (got) {
			break;
		}
		printf("  (percobaan %d: tidak ada jawaban)\n", attempt + 1);
	}
	if (attempt == 3) {
		printf("  ✗ auth gagal — AP tidak menjawab\n");
		return -1;
	}

	/* ── assoc ── */
	printf("── Assoc ──\n");
	for (attempt = 0; attempt < 3; attempt++) {
		size_t flen = 0;
		bool privacy = attempt > 0;	/* coba tanpa lalu dengan Privacy */
		int i, got = 0;

		if (!rtwn_build_assoc_req(bssid, res.mac, ssid,
		    (uint8_t)strlen(ssid), (uint8_t)chan, privacy, seq++,
		    txbuf + RTWN_USB_TX_DESC_SIZE, 256, &flen)) {
			break;
		}
		if (!rtwn_usb_build_mgmt_tx_desc(txbuf, RTWN_USB_TX_DESC_SIZE,
		    (uint16_t)flen, false)) {
			break;
		}
		if (bulk_write(txbuf, RTWN_USB_TX_DESC_SIZE + (uint32_t)flen,
		    300) != 0) {
			continue;
		}

		for (i = 0; i < 10 && !got; i++) {
			struct rxframe fr[16];
			int n = rx_collect(rbuf, sizeof(rbuf), 50, fr, 16, NULL);
			int k;

			if (n <= 0) {
				continue;
			}
			for (k = 0; k < n; k++) {
				uint16_t status = 0;

				if (fr[k].txrep) {
					continue;
				}
				if (!rtwn_frame_for(fr[k].p, fr[k].len, res.mac)) {
					/* Bukan untuk kita — catat kalau mgmt ke
					 * alamat lain (mis. deauth broadcast). */
					if ((fr[k].p[0] & 0x0c) == 0 &&
					    (fr[k].p[0] & 0xf0) == 0xc0) {
						printf("    (deauth dari %02x:%02x:"
						    "%02x:%02x:%02x:%02x)\n",
						    fr[k].p[10], fr[k].p[11],
						    fr[k].p[12], fr[k].p[13],
						    fr[k].p[14], fr[k].p[15]);
					}
					continue;
				}
				if (rtwn_parse_assoc_resp(fr[k].p, fr[k].len,
				    &status)) {
					printf("  ← assoc resp status=%u%s\n",
					    status, status == 0 ? " (OK)" : "");
					got = 1;
					ret = (status == 0) ? 0 : 1;
					break;
				}
				printf("    (frame untuk kita: fc=%02x %02x len=%u)\n",
				    fr[k].p[0], fr[k].p[1], fr[k].len);
			}
		}
		if (got) {
			break;
		}
		printf("  (percobaan %d: tidak ada jawaban)\n", attempt + 1);
	}
	if (ret != 0) {
		/*
		 * Assoc response sering tidak tertangkap (agregasi RX), padahal
		 * AP sudah menganggap kita terasosiasi — terlihat dari deauth
		 * yang dikirim AP setelah EAPOL timeout. Untuk uji TX data,
		 * jendela itu cukup: lanjutkan saja.
		 */
		if (!doTxNull) {
			printf("  ✗ assoc gagal\n");
			return ret;
		}
		printf("  ! assoc resp tidak tertangkap — lanjut uji TX "
		    "(AP tetap menganggap kita asosiasi sesaat)\n");
	}

	/* ── uji TX data ── */
	if (doTxNull) {
		struct rxstat st;
		int round, k;

		memset(&st, 0, sizeof(st));
		printf("── Uji TX data (QoS-Null × 5) ──\n");
		for (round = 0; round < 5; round++) {
			size_t flen = 0;

			if (!rtwn_build_null_data(bssid, res.mac, true, seq++,
			    txbuf + RTWN_USB_TX_DESC_SIZE, 128, &flen)) {
				break;
			}
			if (!rtwn_usb_build_tx_desc(txbuf,
			    RTWN_USB_TX_DESC_SIZE, (uint16_t)flen,
			    RTWN_USB_TX_DESC_SIZE, false, true, 6, false,
			    false, 0)) {
				break;
			}
			/* samakan dengan jalur kernel: QoS + HWSEQ off + retry */
			txbuf[5] = (uint8_t)(txbuf[5] & 0xe0);	/* QSEL = BE */
			txbuf[16] = 0x40;
			txbuf[17] = 0x01;
			txbuf[20] = 0x00;
			txbuf[21] = 0xff;
			txbuf[22] = 0x01;
			txbuf[23] = 0x00;
			rtwn_usb_tx_checksum(txbuf, RTWN_USB_TX_DESC_SIZE);

			/* Data WAJIB lewat pipe BE (index 2), bukan pipe mgmt. */
			if (bulk_write_pipe(2, txbuf,
			    RTWN_USB_TX_DESC_SIZE + (uint32_t)flen, 300) != 0) {
				printf("  round %d: bulk OUT gagal (pipe BE)\n",
				    round);
				continue;
			}
			usleep(20 * 1000);

			{
				struct rxframe fr[16];
				int n = rx_collect(rbuf, sizeof(rbuf), 30, fr,
				    16, &st);

				for (k = 0; k < n; k++) {
					if (fr[k].txrep) {
						/* r88e_tx_rpt_ccx: rptb1 bit6 =
						 * PKT_OK (di-ACK AP), rptb2 =
						 * jumlah retry. */
						bool ok = (fr[k].p[1] & 0x40) != 0;
						unsigned rty =
						    fr[k].p[2] & 0x3f;

						printf("  round %d: TX report "
						    "dw3=0x%08x %s retry=%u "
						    "[%02x %02x %02x %02x %02x "
						    "%02x %02x %02x]\n",
						    round, fr[k].dw3,
						    ok ? "ACK" : "no-ack", rty,
						    fr[k].p[0], fr[k].p[1],
						    fr[k].p[2], fr[k].p[3],
						    fr[k].p[4], fr[k].p[5],
						    fr[k].p[6], fr[k].p[7]);
						if (ok) {
							st.txok++;
						}
					}
				}
			}
		}
		printf("── Hasil ──\n");
		printf("  transfer=%d frame=%d txreport=%d ack=%d crc=%d\n",
		    st.transfers, st.frames, st.txrep, st.txok, st.crc);
		if (st.txok > 0) {
			printf("  ✓ frame data TERKIRIM dan di-ACK AP\n");
		} else if (st.txrep > 0) {
			printf("  ! frame terkirim tapi TIDAK di-ACK "
			    "(AP belum menganggap kita terasosiasi)\n");
		} else {
			printf("  ✗ tidak ada TX report: frame data tidak "
			    "dikirim\n");
		}
	}

	/* Tinggalkan AP dengan rapi supaya run berikutnya tidak bentrok. */
	send_deauth(bssid, res.mac, seq++);

	/*
	 * Matikan radio sebelum keluar. Tanpa ini chip tertinggal dalam
	 * keadaan sudah di-init (firmware jalan) dan driver vendor tidak
	 * bisa mengambil alih tanpa cabut-colok dongle.
	 */
	rtwn_flow_radio_off_8188e(probe_read, probe_write, NULL);
	printf("  radio dimatikan (chip ditinggal dalam keadaan aman)\n");

	return ret;
}

int main(int argc, char **argv)

{
	rtwn_seq_result_t res;
	int doEfuseAll = 0, doFull = 0, doWrite = 0;
	int doPeek = 0, doPoke = 0, doDump = 0, doScan = 0;
	int scanCh1 = 1, scanCh2 = 13;
	const char *joinSsid = NULL, *joinBssid = NULL;
	int doTxNull = 0, joinChan = 0;
	const char *joinMac = NULL;
	uint32_t a1 = 0, a2 = 0;
	int i, ret = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--efuse") == 0 || strcmp(argv[i], "--efuse-all") == 0)
			doEfuseAll = 1;
		else if (strcmp(argv[i], "--full") == 0) { doEfuseAll = 1; doFull = 1; }
		else if (strcmp(argv[i], "--write-test") == 0) doWrite = 1;
		else if (strcmp(argv[i], "--txnull") == 0) doTxNull = 1;
		else if (strcmp(argv[i], "--join") == 0 && i + 1 < argc)
			joinSsid = argv[++i];
		else if (strcmp(argv[i], "--bssid") == 0 && i + 1 < argc)
			joinBssid = argv[++i];
		else if (strcmp(argv[i], "--mac") == 0 && i + 1 < argc)
			joinMac = argv[++i];
		else if (strcmp(argv[i], "--chan") == 0 && i + 1 < argc)
			joinChan = (int)hexarg(argv[++i]);
		else if (strcmp(argv[i], "--scan") == 0) {
			doScan = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				scanCh1 = (int)hexarg(argv[++i]);
				scanCh2 = scanCh1;
				if (i + 1 < argc && argv[i + 1][0] != '-') {
					scanCh2 = (int)hexarg(argv[++i]);
				}
			}
		}
		else if (strcmp(argv[i], "--peek") == 0 && i + 1 < argc) {
			doPeek = 1; a1 = hexarg(argv[++i]);
			if (i + 1 < argc && argv[i + 1][0] != '-') a2 = hexarg(argv[++i]);
			else a2 = 1;
		} else if (strcmp(argv[i], "--poke") == 0 && i + 2 < argc) {
			doPoke = 1; a1 = hexarg(argv[++i]); a2 = hexarg(argv[++i]);
		} else if (strcmp(argv[i], "--dump") == 0 && i + 2 < argc) {
			doDump = 1; a1 = hexarg(argv[++i]); a2 = hexarg(argv[++i]);
		}
	}

	printf("=== rtwn-probe: RTL8188EUS (2357:010c) dari user space ===\n\n");
	rtwn_seq_diag_register(diag_cb);

	if (open_device() != 0) {
		return 2;
	}

	memset(&res, 0, sizeof(res));

	/* ── join + uji TX data (M3 userspace lab) ──────────────────── */
	if (joinSsid != NULL) {
		if (open_interface() != 0) {
			ret = 1;
			goto done;
		}
		ret = run_join(joinSsid, joinBssid, joinChan, doTxNull, joinMac);
		close_interface();
		goto done;
	}

	/* ── scan M2 ────────────────────────────────────────────────── */
	if (doScan) {
		if (open_interface() != 0) {
			ret = 1;
			goto done;
		}
		ret = run_scan(scanCh1, scanCh2);
		close_interface();
		goto done;
	}

	/* ── mode mentah ────────────────────────────────────────────── */
	if (doPeek) {
		uint8_t buf[256];
		uint32_t n = a2 ? a2 : 1;
		if (n > sizeof(buf)) n = sizeof(buf);
		printf("── peek 0x%03x (%u byte) ──\n", a1, n);
		if (probe_read(NULL, (uint16_t)a1, buf, (uint16_t)n) == 0) {
			for (uint32_t k = 0; k < n; k++) {
				if (k % 16 == 0) printf("  %03x:", a1 + k);
				printf(" %02x", buf[k]);
				if (k % 16 == 15) printf("\n");
			}
			if (n % 16) printf("\n");
		} else {
			printf("  GAGAL\n");
			ret = 1;
		}
		goto done;
	}

	if (doPoke) {
		uint32_t v = a2;
		printf("── poke 0x%03x <= 0x%08x ──\n", a1, v);
		if (probe_write(NULL, (uint16_t)a1, &v, 4) == 0) {
			uint32_t rb = 0;
			if (probe_read(NULL, (uint16_t)a1, &rb, 4) == 0)
				printf("  baca balik = 0x%08x %s\n", rb,
				    rb == v ? "✓" : "✗");
		} else {
			printf("  GAGAL\n");
			ret = 1;
		}
		goto done;
	}

	if (doDump) {
		uint32_t n = a2, off;
		printf("── dump 0x%03x..0x%03x ──\n", a1, a1 + n - 1);
		for (off = 0; off < n; off += 16) {
			uint32_t v[4] = {0, 0, 0, 0};
			uint32_t chunk = (n - off < 16) ? (n - off) : 16;
			if (probe_read(NULL, (uint16_t)(a1 + off), v, (uint16_t)chunk) != 0) {
				printf("  %03x: <gagal>\n", a1 + off);
				continue;
			}
			printf("  %03x:", a1 + off);
			for (uint32_t k = 0; k < chunk; k++)
				printf(" %02x", ((uint8_t *)v)[k]);
			printf("\n");
		}
		goto done;
	}

	/* ── info dasar ─────────────────────────────────────────────── */
	printf("── Tahap 1: baca register ──\n");
	if (rtwn_seq_read_regs(&res, probe_read, probe_write, NULL) != 0) {
		printf("  GAGAL: chip tidak menjawab vendor request.\n");
		ret = 1;
		goto done;
	}
	printf("  CR      = 0x%08x\n", res.cr);
	printf("  MCUFWDL = 0x%08x\n", res.mcuwdl);
	printf("  SYS_CFG = 0x%08x\n", res.syscfg);
	printf("  chip menjawab ✓\n\n");

	printf("── Register kunci ──\n");
	{
		static const struct { uint16_t a; const char *n; } regs[] = {
			{ 0x000, "SYS_FUNC_EN" }, { 0x002, "SYS_FUNC_EN+2" },
			{ 0x004, "APS_FSMCO" },   { 0x005, "APS_FSMCO+1" },
			{ 0x008, "RSV_CTRL" },    { 0x01c, "AFE_XTAL_CTRL" },
			{ 0x01d, "AFE_XTAL_CTRL2" }, { 0x003, "CR2" },
			{ 0x030, "EFUSE_CTRL/data" }, { 0x031, "EFUSE_ADDR_LO" },
			{ 0x032, "EFUSE_ADDR_HI" },   { 0x033, "EFUSE_STATUS" },
			{ 0x080, "MCUFWDL" },     { 0x088, "FWDL/trigger" },
			{ 0x093, "MCUFWDL_93" },  { 0x0cf, "EFUSE_ACCESS" },
			{ 0x0f0, "SYS_CFG" },     { 0x100, "CR" },
			{ 0x106, "EFUSE_106" },   { 0x140, "EFUSE_WIN" },
			{ 0x143, "EFUSE_STAT" },  { 0x144, "EFUSE_HDR" },
			{ 0x148, "EFUSE_DATA" },  { 0x209, "EFUSE_MODE" },
			{ 0x610, "MACID" },
		};
		for (size_t k = 0; k < sizeof(regs) / sizeof(regs[0]); k++) {
			uint8_t v = 0;
			if (probe_read(NULL, regs[k].a, &v, 1) == 0)
				printf("  0x%03x %-16s = 0x%02x\n", regs[k].a,
				    regs[k].n, v);
			else
				printf("  0x%03x %-16s = <gagal>\n", regs[k].a,
				    regs[k].n);
		}
	}
	printf("\n");

	if (doWrite) {
		uint32_t before = 0, after = 0;
		printf("── Uji tulis ──\n");
		if (probe_read(NULL, RTWN_REG_CR, &before, 4) == 0) {
			uint32_t want = before ^ 0x5a5a5a5a;
			printf("  CR 0x%08x → tulis 0x%08x ... ", before, want);
			if (probe_write(NULL, RTWN_REG_CR, &want, 4) == 0 &&
			    probe_read(NULL, RTWN_REG_CR, &after, 4) == 0) {
				printf("baca 0x%08x %s\n", after,
				    (after == want) ? "✓ cocok" : "✗ tidak cocok");
			} else {
				printf("GAGAL\n");
			}
		} else {
			printf("  GAGAL baca\n");
		}
		printf("\n");
	}

	if (doFull) {
		printf("── Tahap 3: power-on ──\n");
		if (rtwn_seq_power_on(&res, probe_read, probe_write, NULL) != 0)
			printf("  GAGAL: timeout menunggu chip power up\n");
		else
			printf("  power on OK\n");

		printf("── Tahap 4: LLT/DMA init ──\n");
		if (rtwn_seq_dma_init(&res, probe_read, probe_write, NULL, 3) != 0)
			printf("  GAGAL: LLT init\n");
		else
			printf("  LLT/DMA OK\n");

		printf("── Tahap 5: upload firmware 8051 ──\n");
		if (rtwn_seq_load_firmware(&res, probe_read, probe_write, NULL,
		    rtl8188eufw_bin, rtl8188eufw_bin_len) != 0)
			printf("  GAGAL: upload firmware (timeout checksum?)\n");
		else
			printf("  firmware %u byte terupload ✓\n", res.fwlen);
		printf("\n");
	}

	/* ── EFUSE: SEMUA jalur + diagnostik ─────────────────────────── */
	if (doEfuseAll) {
		rtwn_seq_result_t r2;

		printf("── Tahap 2b: EFUSE OFFLOAD (jalur HAL vendor) ──\n");
		memset(&r2, 0, sizeof(r2));
		if (rtwn_seq_read_efuse_offload(&r2, probe_read, probe_write, NULL) != 0)
			printf("  ✗ GAGAL\n");
		else {
			printf("  ✓ MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
			    r2.mac[0], r2.mac[1], r2.mac[2], r2.mac[3], r2.mac[4],
			    r2.mac[5]);
			res = r2;
		}
		printf("\n");

		printf("── Tahap 2a: EFUSE DIRECT (register 0x031/32/33/30) ──\n");
		memset(&r2, 0, sizeof(r2));
		if (rtwn_seq_read_efuse_direct_8188e(&r2, probe_read, probe_write,
		    NULL) != 0)
			printf("  ✗ GAGAL\n");
		else {
			printf("  ✓ MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
			    r2.mac[0], r2.mac[1], r2.mac[2], r2.mac[3], r2.mac[4],
			    r2.mac[5]);
			if (!res.efuse_ok) res = r2;
		}
		printf("\n");

		printf("── Tahap 2c: MACID register 0x610 ──\n");
		{
			uint8_t m[6];
			int ok = 1;
			for (int k = 0; k < 6; k++)
				if (probe_read(NULL, (uint16_t)(0x610 + k), &m[k], 1) != 0)
					ok = 0;
			if (ok)
				printf("  MACID = %02x:%02x:%02x:%02x:%02x:%02x%s\n",
				    m[0], m[1], m[2], m[3], m[4], m[5],
				    (m[0] == 0xff && m[1] == 0xff) ? "  (kosong)" : "");
			else
				printf("  GAGAL baca\n");
		}
		printf("\n");
	}

	printf("=== RINGKASAN ===\n");
	print_result(&res);
	printf("\n");

done:
	close_device();
	return ret;
}
