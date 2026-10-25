/*
 * ItlRtl8188E.cpp — HAL service RTL8188EUS untuk frontend AirportItlwm (M4).
 *
 * Jalur data:
 *   net80211 (port itlwm)  ──►  ic_newstate / if_start  ──►  bulk OUT
 *   bulk IN                ──►  ieee80211_inputm()      ──►  scan result / data
 *
 * Semua inisialisasi chip memakai modul portabel yang sama dengan
 * tools/rtwn_probe (sudah terbukti di hardware pada M1/M2).
 *
 * Lisensi: GPL-2.0-or-later
 */

#include "ItlRtl8188E.hpp"

#include <string.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_proto.h>
#include <net80211/ieee80211_node.h>

#include <sys/timeout.h>

extern "C" {
#include "r92creg.h"
#include "rtwn_seq.h"
#include "rtwn_init_8188e.h"
#include "rtwn_chan.h"
#include "rtwn_usb.h"
}

#define super ItlHalService
OSDefineMetaClassAndStructors(ItlRtl8188E, ItlHalService);

#define LOGP "rtl8188e: "

/* ------------------------------------------------------------------ */
/*  register access                                                    */
/* ------------------------------------------------------------------ */

int ItlRtl8188E::halRead(void *ctx, uint16_t addr, void *buf, uint16_t len)
{
	ItlRtl8188E *me = (ItlRtl8188E *)ctx;
	if (!me)
		return -1;
	return rtwn_m1_read_region(&me->fChip, addr, (uint8_t *)buf, len);
}

int ItlRtl8188E::halWrite(void *ctx, uint16_t addr, const void *buf,
    uint16_t len)
{
	ItlRtl8188E *me = (ItlRtl8188E *)ctx;
	if (!me)
		return -1;
	return rtwn_m1_write_region(&me->fChip, addr, (const uint8_t *)buf, len);
}

/* ------------------------------------------------------------------ */
/*  hidup/mati                                                         */
/* ------------------------------------------------------------------ */

bool ItlRtl8188E::init()
{
	if (!super::init())
		return false;
	memset(&fChip, 0, sizeof(fChip));
	memset(&fIc, 0, sizeof(fIc));
	fInterface = nullptr;
	fDevice = nullptr;
	fScanTo = nullptr;
	fOrigNewstate = nullptr;
	fAttached = false;
	fEnabled = false;
	fScanning = false;
	fScanSeen = false;
	fChipReady = false;
	fBulkReady = false;
	fIfAttached = false;
	fSawRx = false;
	fTxQueueSize = 256;
	fRxTimer = nullptr;
	fTxTimer = nullptr;
	fRxLock = nullptr;
	fRxHead = 0;
	fRxCount = 0;
	fRxRunning = false;
	memset(fRxBuf, 0, sizeof(fRxBuf));
	memset(fRxLen, 0, sizeof(fRxLen));
	fFirmwareVersion[0] = 0;
	fCountryCode[0] = 0;
	memset(fMac, 0, sizeof(fMac));
	fTxMgmt = fTxData = fTxEapol = fTxFail = 0;
	fRxMgmt = fRxData = fRxEapol = fEncryptFail = 0;
	fTxReport = fTxOk = fTxRetry = 0;
	fTxAuthReq = fTxAssocReq = fRxAuth = fRxAssocResp = 0;
	fJoinTry = 0;
	memset(fLastJoinBssid, 0, sizeof(fLastJoinBssid));
	fRsnVariant = 0;
	fRxDelivered = 0;
	fRxDataLogged = 0;
	fRxBar = 0;
	fTxAgg = 0;
	fBw40 = false;
	fPriChLo = false;
	memset(fTxScratch, 0, sizeof(fTxScratch));
	return true;
}

void ItlRtl8188E::free()
{
	/*
	 * Jaring pengaman: kalau start() gagal setelah attach net80211,
	 * frontend memanggil releaseAll() TANPA detach(). Timer net80211
	 * (slowtimo) akan menembak ifp yang sudah dibebaskan kalau ifdetach
	 * tidak dipanggil.
	 */
	fRxRunning = false;
	if (fAttached)
		fChip.usb.setRxCallback(nullptr, nullptr);
	if (fBulkReady) {
		fChip.usb.bulkClose();
		fBulkReady = false;
	}
	if (fScanTo) {
		timeout_del(&fScanTo);
		timeout_free(&fScanTo);
		fScanTo = nullptr;
	}
	if (fRxTimer) {
		fRxTimer->cancelTimeout();
		if (getMainWorkLoop())
			getMainWorkLoop()->removeEventSource(fRxTimer);
		fRxTimer->release();
		fRxTimer = nullptr;
	}
	if (fTxTimer) {
		fTxTimer->cancelTimeout();
		if (getMainWorkLoop())
			getMainWorkLoop()->removeEventSource(fTxTimer);
		fTxTimer->release();
		fTxTimer = nullptr;
	}
	rxQueueFree();
	if (fRxLock) {
		IOLockFree(fRxLock);
		fRxLock = nullptr;
	}
	if (fIfAttached && fIc.ic_if.if_snd.queue) {
		fIc.ic_newstate = fOrigNewstate;
		fOrigNewstate = nullptr;
		ieee80211_ifdetach(&fIc.ic_if);
		fIfAttached = false;
	}
	stopChip();
	if (fInterface) {
		fInterface->release();
		fInterface = nullptr;
	}
	if (fDevice) {
		fDevice->release();
		fDevice = nullptr;
	}
	super::free();
}

bool ItlRtl8188E::prepare(IOUSBHostInterface *interface)
{
	if (!interface)
		return false;
	fInterface = interface;
	fInterface->retain();
	fDevice = interface->getDevice();
	if (fDevice)
		fDevice->retain();
	return fDevice != nullptr;
}

/* ------------------------------------------------------------------ */
/*  init chip (urutan M1/M2 yang sudah terbukti)                       */
/* ------------------------------------------------------------------ */

int ItlRtl8188E::initChip(void)
{
	rtwn_seq_result_t res;
	rtwn_seq_result_t ef;
	int rc = 0;

	memset(&res, 0, sizeof(res));
	memset(&ef, 0, sizeof(ef));

	/* TP-Link TL-WN725N (RTL8188EUS) memiliki 2 endpoint Bulk-OUT:
	 *   EP 0x02 (pipe 0) = High Queue (HQ: VO, VI, MGNT, HIGH)
	 *   EP 0x03 (pipe 1) = Normal Queue (NQ: BE, BK)
	 * ntx=2 memprogram R92C_TRXDMA_CTRL (0x10c) = 0xfaf0 (QMAP_HQ_NQ)
	 * dan R92C_RQPN (0x200) = 0x8090000c, R92C_RQPN_NPQ (0x214) = 0x0d,
	 * persis seperti RtWlanU.kext (live_regs_dump.txt).
	 */
	fChip.ntx = 2;

	if (rtwn_seq_power_on(&res, halRead, halWrite, this) != 0) {
		IOLog(LOGP "power-on gagal\n");
		return -1;
	}

	/*
	 * Baca EFUSE langsung SEBELUM rtwn_seq_dma_init & rtwn_seq_load_firmware!
	 * rtwn_seq_read_efuse_offload menulis 0xf4 ke register 0x209 (BCN_HEAD)
	 * dan mereset MCU 8051 8 kali bila gagal.
	 */
	if (rtwn_seq_read_efuse_direct_8188e(&ef, halRead, halWrite,
	    this) == 0 && ef.efuse_ok) {
		memcpy(res.mac, ef.mac, 6);
		res.xtal = ef.xtal;
	} else if (rtwn_seq_read_efuse_offload(&ef, halRead, halWrite, this) == 0 &&
	    ef.efuse_ok) {
		memcpy(res.mac, ef.mac, 6);
		res.xtal = ef.xtal;
	} else {
		IOLog(LOGP "EFUSE gagal, pakai MAC default\n");
		res.mac[0] = 0x02;
		res.mac[5] = 0x01;
		res.xtal = 0x20;
	}

	if (rtwn_seq_dma_init(&res, halRead, halWrite, this, 2) != 0) {
		IOLog(LOGP "DMA/LLT gagal\n");
		return -1;
	}
	if (rtwn_seq_load_firmware(&res, halRead, halWrite, this,
	    rtl8188eufw_bin, rtl8188eufw_bin_len) != 0) {
		IOLog(LOGP "firmware gagal (%u byte)\n", res.fwlen);
		return -1;
	}
	IOLog(LOGP "firmware OK (%u byte)\n", res.fwlen);

	memcpy(fMac, res.mac, 6);
	memcpy(fChip.macaddr, res.mac, 6);
	fChip.xtal = res.xtal ? res.xtal : 0x20;

	IOLog(LOGP "MAC %02x:%02x:%02x:%02x:%02x:%02x xtal=0x%02x\n",
	    fMac[0], fMac[1], fMac[2], fMac[3], fMac[4], fMac[5], fChip.xtal);

	rc += rtwn_init_mac_8188e(halRead, halWrite, this);
	rc += rtwn_init_bb_8188e(halRead, halWrite, this, fChip.xtal);
	rc += rtwn_init_rf_8188e(halRead, halWrite, this);
	rc += rtwn_core_config_8188e(halRead, halWrite, this, fMac);

	IOSleep(2);
	fChipReady = true;
	IOLog(LOGP "init selesai (%d register)\n", rc);
	return 0;
}

void ItlRtl8188E::stopChip(void)
{
	if (fBulkReady) {
		fChip.usb.setTxDoneCb(nullptr, nullptr);
		fChip.usb.setRxCallback(nullptr, nullptr);
		fChip.usb.bulkClose();
		fBulkReady = false;
	}
	if (fChipReady) {
		/* Radio off supaya chip tidak memancar saat unload. */
		rtwn_flow_radio_off_8188e(halRead, halWrite, this);
		fChipReady = false;
	}
	if (fAttached) {
		fChip.usb.detach();
		fAttached = false;
	}
}

/* ------------------------------------------------------------------ */
/*  ItlHalService                                                      */
/* ------------------------------------------------------------------ */

bool ItlRtl8188E::attach(IOPCIDevice *device)
{
	(void)device;

	if (fAttached)
		return true;
	if (!fInterface)
		return false;

	setDiag("RTL8188E-stage", "usb-attach");
	if (fChip.usb.attach(getController(), fDevice, fInterface) != 0) {
		IOLog(LOGP "usb attach gagal\n");
		setDiag("RTL8188E-stage", "fail-usb-attach");
		return false;
	}
	fAttached = true;

	setDiag("RTL8188E-stage", "chip-init");
	if (initChip() != 0) {
		IOLog(LOGP "init chip gagal\n");
		setDiag("RTL8188E-stage", "fail-chip-init");
		stopChip();
		return false;
	}

	if (!fChip.usb.bulkOpen(fInterface)) {
		IOLog(LOGP "bulkOpen gagal\n");
		setDiag("RTL8188E-stage", "fail-bulk-open");
		stopChip();
		return false;
	}
	setDiag("RTL8188E-stage", "net80211-attach");

	/* ---------------- net80211 ---------------- */
	struct _ifnet *ifp = &fIc.ic_if;

	IEEE80211_ADDR_COPY(fIc.ic_myaddr, fMac);
	fIc.ic_phytype = IEEE80211_T_OFDM;
	fIc.ic_opmode = IEEE80211_M_STA;
	fIc.ic_state = IEEE80211_S_INIT;
	fIc.ic_curmode = IEEE80211_MODE_11N;
	fIc.ic_caps = IEEE80211_C_WEP | IEEE80211_C_RSN |
	    IEEE80211_C_SHSLOT | IEEE80211_C_SHPREAMBLE |
	    IEEE80211_C_QOS | IEEE80211_C_TX_AMPDU |
	    IEEE80211_C_AMSDU_IN_AMPDU;
	/*
	 * 802.11n (HT): aktifkan kapabilitas HT, MCS 0..7, dan A-MPDU aggregation.
	 */
	fIc.ic_htcaps = IEEE80211_HTCAP_CBW20_40 |
	    IEEE80211_HTCAP_SGI20 | IEEE80211_HTCAP_SGI40 |
	    IEEE80211_HTCAP_DSSSCCK40;
	fIc.ic_htcaps |= (IEEE80211_HTCAP_SMPS_DIS << IEEE80211_HTCAP_SMPS_SHIFT);
	fIc.ic_sup_rates[IEEE80211_MODE_11B] = ieee80211_std_rateset_11b;
	fIc.ic_sup_rates[IEEE80211_MODE_11G] = ieee80211_std_rateset_11g;
	memset(fIc.ic_sup_mcs, 0, sizeof(fIc.ic_sup_mcs));
	fIc.ic_sup_mcs[0] = 0xff;	/* MCS 0..7 (hingga 72.2M di 20MHz / 150M di 40MHz) */
	fIc.ic_htxcaps = 0;
	fIc.ic_txbfcaps = 0;
	fIc.ic_aselcaps = 0;
	fIc.ic_ampdu_params = (IEEE80211_AMPDU_PARAM_SS_4 | 0x3 /* 64k */);
	fIc.ic_max_rssi = 100;

	/*
	 * Tabel kanal 2.4 GHz WAJIB diisi SEBELUM ieee80211_ifattach.
	 * ieee80211_channel_init() hanya memvalidasi kanal yang sudah
	 * bertanda; tanpa ini ic_chan_avail kosong dan scan tidak pernah
	 * mengunjungi satu kanal pun.
	 */
	for (uint32_t ch = 1; ch <= 14; ch++) {
		struct ieee80211_channel *c = &fIc.ic_channels[ch];
		c->ic_freq = ieee80211_ieee2mhz(ch, IEEE80211_CHAN_2GHZ);
		c->ic_flags = IEEE80211_CHAN_2GHZ |
		    IEEE80211_CHAN_CCK | IEEE80211_CHAN_OFDM |
		    IEEE80211_CHAN_DYN | IEEE80211_CHAN_HT20;
		if (ch <= 7)
			c->ic_flags |= IEEE80211_CHAN_HT40U;
		if (ch >= 5)
			c->ic_flags |= IEEE80211_CHAN_HT40D;
	}
	fIc.ic_ibss_chan = &fIc.ic_channels[1];

	ifp->if_softc = this;
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_start = txStartCb;
	ifp->if_ioctl = ieee80211_ioctl;
	ifp->if_watchdog = watchdogCb;
	strlcpy(ifp->if_xname, "wlan0", IFNAMSIZ);

	if_attach(ifp);
	ieee80211_ifattach(ifp, getController());
	/*
	 * Antrean TX (if_snd) 256 paket (~384 KB): mencegah tail-drop pada
	 * 16 paralel TCP flow (networkQuality) saat A-MPDU burst dibentuk,
	 * jauh di bawah default 2048 yang memicu bufferbloat.
	 */
	ifq_set_maxlen(&ifp->if_snd, 256);
	ieee80211_media_init(ifp);

	fOrigNewstate = fIc.ic_newstate;
	fIc.ic_newstate = newstateCb;
	fIc.ic_updateslot = updateSlotCb;
	fIc.ic_softc = this;
	fIc.ic_set_key = setKeyCb;
	fIc.ic_delete_key = deleteKeyCb;

	timeout_set(&fScanTo, scanTimeoutCb, this);
	fIfAttached = true;

	/*
	 * RX baru dimulai SETELAH net80211 siap. Kalau tidak, frame yang
	 * datang lebih awal akan memanggil ieee80211_inputm pada ic_bss
	 * yang masih NULL → panic.
	 */
	fRxLock = IOLockAlloc();
	fRxTimer = IOTimerEventSource::timerEventSource(this,
	    OSMemberFunctionCast(IOTimerEventSource::Action, this,
	    &ItlRtl8188E::rxTimerAction));
	fTxTimer = IOTimerEventSource::timerEventSource(this,
	    OSMemberFunctionCast(IOTimerEventSource::Action, this,
	    &ItlRtl8188E::txTimerAction));
	if (!fRxLock || !fRxTimer || !fTxTimer || !getMainWorkLoop() ||
	    getMainWorkLoop()->addEventSource(fRxTimer) != kIOReturnSuccess ||
	    getMainWorkLoop()->addEventSource(fTxTimer) != kIOReturnSuccess) {
		IOLog(LOGP "gagal membuat RX/TX event source\n");
		if (fRxTimer) {
			getMainWorkLoop()->removeEventSource(fRxTimer);
			fRxTimer->release();
			fRxTimer = nullptr;
		}
		if (fTxTimer) {
			getMainWorkLoop()->removeEventSource(fTxTimer);
			fTxTimer->release();
			fTxTimer = nullptr;
		}
		if (fRxLock) {
			IOLockFree(fRxLock);
			fRxLock = nullptr;
		}
		ieee80211_ifdetach(&fIc.ic_if);
		fIfAttached = false;
		stopChip();
		return false;
	}
	fRxTimer->enable();
	fTxTimer->enable();
	fRxRunning = true;
	fChip.usb.setTxDoneCb(txDoneHook, this);
	fChip.usb.setRxCallback(rxCb, this);
	if (!fChip.usb.bulkQueueRx(32)) {
		IOLog(LOGP "gagal antre RX\n");
		fRxRunning = false;
		fChip.usb.setTxDoneCb(nullptr, nullptr);
		fChip.usb.setRxCallback(nullptr, nullptr);
		getMainWorkLoop()->removeEventSource(fRxTimer);
		fRxTimer->release();
		fRxTimer = nullptr;
		getMainWorkLoop()->removeEventSource(fTxTimer);
		fTxTimer->release();
		fTxTimer = nullptr;
		ieee80211_ifdetach(&fIc.ic_if);
		fIfAttached = false;
		stopChip();
		return false;
	}
	fBulkReady = true;
	setDiag("RTL8188E-stage", "net80211-ready");
	if (getController()) {
		OSData *mac = OSData::withBytes(fMac, IEEE80211_ADDR_LEN);

		if (mac) {
			getController()->setProperty("RTL8188E-mac", mac);
			mac->release();
		}
	}
	IOLog(LOGP "net80211 attach OK (state INIT)\n");
	return true;
}

void ItlRtl8188E::detach(IOPCIDevice *device)
{
	(void)device;

	/* 1. Hentikan sumber RX — jangan ada frame baru saat ifdetach. */
	fRxRunning = false;
	if (fAttached)
		fChip.usb.setRxCallback(nullptr, nullptr);
	/*
	 * Tutup pipe RX lebih dulu: bulkClose() melakukan abort sinkron,
	 * jadi setelah ini tidak ada completion yang boleh menyentuh
	 * fRxLock/fRxTimer yang akan dibebaskan di bawah.
	 */
	if (fBulkReady) {
		fChip.usb.bulkClose();
		fBulkReady = false;
	}
	if (fRxTimer) {
		fRxTimer->cancelTimeout();
		if (getMainWorkLoop())
			getMainWorkLoop()->removeEventSource(fRxTimer);
		fRxTimer->release();
		fRxTimer = nullptr;
	}
	if (fTxTimer) {
		fTxTimer->cancelTimeout();
		if (getMainWorkLoop())
			getMainWorkLoop()->removeEventSource(fTxTimer);
		fTxTimer->release();
		fTxTimer = nullptr;
	}
	rxQueueFree();
	if (fRxLock) {
		IOLockFree(fRxLock);
		fRxLock = nullptr;
	}

	/* 2. Timer scan + state machine. */
	if (fScanTo) {
		timeout_del(&fScanTo);
		timeout_free(&fScanTo);
		fScanTo = nullptr;
	}
	if (fOrigNewstate) {
		fIc.ic_newstate = fOrigNewstate;
		fOrigNewstate = nullptr;
	}
	if (fIfAttached && fIc.ic_if.if_snd.queue) {
		ieee80211_ifdetach(&fIc.ic_if);
		fIfAttached = false;
	}
	fBulkReady = false;
	stopChip();
	if (fIc.ic_event_handler)
		fIc.ic_event_handler(&fIc, IEEE80211_EVT_DRIVER_STOPPED, NULL);
	fEnabled = false;
}

void ItlRtl8188E::updateMACAddress(const uint8_t *mac)
{
	if (!mac)
		return;
	for (int i = 0; i < IEEE80211_ADDR_LEN; i++) {
		fMac[i] = mac[i];
		fChip.macaddr[i] = mac[i];
		halWrite(this, R92C_MACID + i, &mac[i], 1);
	}
	IOLog(LOGP "MAC address hardware disinkronkan ke %02x:%02x:%02x:%02x:%02x:%02x\n",
	    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

IOReturn ItlRtl8188E::enable(IONetworkInterface *interface)
{
	(void)interface;
	struct _ifnet *ifp = &fIc.ic_if;

	/* Pastikan register MAC chip cocok dengan ic_myaddr */
	for (int i = 0; i < IEEE80211_ADDR_LEN; i++) {
		uint8_t b = fIc.ic_myaddr[i];
		halWrite(this, R92C_MACID + i, &b, 1);
		fMac[i] = b;
		fChip.macaddr[i] = b;
	}

	if (ifp->if_flags & IFF_UP)
		return kIOReturnSuccess;

	ifp->if_flags |= IFF_UP | IFF_RUNNING;
	fEnabled = true;
	fRxRunning = true;
	setDiag("RTL8188E-stage", "enabled");

	if (fIc.ic_event_handler)
		fIc.ic_event_handler(&fIc, IEEE80211_EVT_DRIVER_RESET_BEGIN, NULL);

	if (fIc.ic_state == IEEE80211_S_INIT)
		ieee80211_new_state(&fIc, IEEE80211_S_SCAN, -1);

	if (fIc.ic_event_handler)
		fIc.ic_event_handler(&fIc, IEEE80211_EVT_DRIVER_READY, NULL);

	return kIOReturnSuccess;
}

IOReturn ItlRtl8188E::disable(IONetworkInterface *interface)
{
	(void)interface;
	struct _ifnet *ifp = &fIc.ic_if;

	if (!(ifp->if_flags & IFF_UP))
		return kIOReturnSuccess;

	ifp->if_flags &= ~(IFF_UP | IFF_RUNNING);
	fEnabled = false;
	fRxRunning = false;
	setDiag("RTL8188E-stage", "disabled");
	if (fScanTo)
		timeout_del(&fScanTo);
	if (fIc.ic_state != IEEE80211_S_INIT)
		ieee80211_new_state(&fIc, IEEE80211_S_INIT, -1);

	if (fIc.ic_event_handler)
		fIc.ic_event_handler(&fIc, IEEE80211_EVT_DRIVER_STOPPED, NULL);

	return kIOReturnSuccess;
}

struct ieee80211com *ItlRtl8188E::get80211Controller()
{
	return &fIc;
}

/* ------------------------------------------------------------------ */
/*  net80211 state machine                                             */
/* ------------------------------------------------------------------ */

/*
 * Diagnostik lewat ioreg (log show rusak di mesin ini):
 *   ioreg -r -c AirportItlwm -l | grep RTL8188E
 *   ioreg -r -c IOPCIEDeviceWrapper -l | grep RTL8188E
 * Nilai dipasang pada controller DAN provider (wrapper) supaya tetap
 * terlihat walau start() frontend gagal.
 */
void ItlRtl8188E::setDiag(const char *key, const char *value)
{
	IOService *svc = getController();

	if (!svc || !key || !value)
		return;
	svc->setProperty(key, value);
	IOService *provider = svc->getProvider();

	if (provider)
		provider->setProperty(key, value);
}

void ItlRtl8188E::publishStats(void)
{
	char s[224];

	snprintf(s, sizeof(s),
	    "txm=%u txd=%u txe=%u txf=%u agg=%u rpt=%u txok=%u rty=%u "
	    "rxm=%u rxd=%u rxe=%u bar=%u encf=%u pipe=%d inf=%d "
	    "auth=%u/%u assoc=%u/%u try=%u rsnv=%u dlv=%u "
	    "bw=%u st=%d xf=%08x sndq=%d",
	    (unsigned)fTxMgmt, (unsigned)fTxData, (unsigned)fTxEapol,
	    (unsigned)fTxFail, (unsigned)fTxAgg, (unsigned)fTxReport,
	    (unsigned)fTxOk, (unsigned)fTxRetry, (unsigned)fRxMgmt,
	    (unsigned)fRxData, (unsigned)fRxEapol, (unsigned)fRxBar,
	    (unsigned)fEncryptFail,
	    fChip.usb.txPipeCount(), fChip.usb.txInFlight(),
	    (unsigned)fTxAuthReq, (unsigned)fRxAuth,
	    (unsigned)fTxAssocReq, (unsigned)fRxAssocResp,
	    (unsigned)fJoinTry, (unsigned)fRsnVariant,
	    (unsigned)fRxDelivered,
	    fBw40 ? 40u : 20u,
	    (int)fIc.ic_state, (unsigned)fIc.ic_xflags,
	    fIc.ic_if.if_snd.queue ? 1 : 0);
	setDiag("RTL8188E-stat", s);
}

void ItlRtl8188E::setChannel(struct ieee80211_channel *chan)
{
	if (!chan)
		return;
	uint8_t ch = (uint8_t)ieee80211_chan2ieee(&fIc, chan);
	if (ch >= 1 && ch <= 14) {
		char stage[24];
		int bw40 = 0;
		int prichlo = 0;

		if (fIc.ic_state == IEEE80211_S_RUN && fIc.ic_bss &&
		    fIc.ic_bss->ni_chan == chan &&
		    fIc.ic_bss->ni_chw == IEEE80211_CHAN_WIDTH_40) {
			uint8_t sco = fIc.ic_bss->ni_htop0 &
			    IEEE80211_HTOP0_SCO_MASK;
			if (sco == IEEE80211_HTOP0_SCO_SCA ||
			    sco == IEEE80211_HTOP0_SCO_SCB) {
				bw40 = 1;
				prichlo = (sco == IEEE80211_HTOP0_SCO_SCA) ? 1 : 0;
			}
		}

		fBw40 = (bw40 != 0);
		fPriChLo = (prichlo != 0);
		snprintf(stage, sizeof(stage), "ch-%u%s", ch,
		    bw40 ? (prichlo ? "+40" : "-40") : "");
		setDiag("RTL8188E-scan", stage);
		if (rtwn_set_chan_bw_8188e(halRead, halWrite, this, ch,
		    bw40, prichlo) != 0)
			IOLog(LOGP "set kanal %u (bw40=%d) gagal\n", ch, bw40);
	}
}

/*
 * Varian RSN per percobaan join.
 *
 * AP ZEKRI.NET mengiklankan WPA2-PSK-CCMP tetapi juga WPA3-transition
 * (RSN caps MFPC+MFPR). Driver tidak membalas assoc request apa pun
 * varian RSN yang dipakai net80211 dari macOS — AP diam, bukan menolak.
 *
 * Karena kext IO80211 hanya bisa dimuat saat boot, mencoba satu varian
 * per reboot terlalu mahal. Fungsi ini mengganti parameter RSN pada
 * setiap siklus join, jadi SATU boot mencoba beberapa kombinasi sampai
 * ada yang diterima AP.
 *
 *   1  apa adanya dari macOS (baseline)
 *   2  CCMP saja (buang TKIP)
 *   3  WPA1+WPA2, CCMP+TKIP (kompatibilitas maksimum)
 *   4  WPA1 saja, TKIP+CCMP (AP lama)
 *   5  tanpa RSN (open assoc) — diagnostik: membuktikan apakah RSN IE
 *      yang membuat AP diam
 */
void ItlRtl8188E::applyRsnVariant(uint32_t attempt)
{
	/*
	 * Selalu gunakan varian 1 (baseline macOS RSN IE).
	 * Varian 2..5 (eksperimen lama) mengubah cipher/proto menjadi
	 * TKIP/WPA1/SHA256 yang ditolak AP ZEKRI.NET dan memicu deauth loop.
	 */
	(void)attempt;
	fRsnVariant = 1;
	IOLog(LOGP "rsn-varian 1: baseline macOS\n");
}

/*
 * Setara rtwn_updateslot() + bagian INIRTS_RATE_SEL di rtwn_newstate()
 * (OpenBSD sys/dev/ic/rtwn.c).
 *
 * R92C_SLOT tidak pernah ditulis di port ini. Nilai bawaan chip belum
 * tentu cocok dengan AP: AP yang mengiklankan short slot (capability
 * bit 10, seperti ZEKRI.NET dengan cap=0x0431) memakai 9 us, sedangkan
 * long slot 20 us. Slot time menentukan DIFS dan backoff — kalau client
 * dan AP berbeda, frame dikirim pada jendela yang salah dan bertabrakan,
 * sehingga assoc request bisa hilang tanpa pernah sampai ke AP.
 */
void ItlRtl8188E::updateSlot(struct ieee80211com *ic)
{
	uint8_t slot = (ic->ic_flags & IEEE80211_F_SHSLOT) ?
	    IEEE80211_DUR_DS_SHSLOT : IEEE80211_DUR_DS_SLOT;
	uint8_t irts = (ic->ic_curmode == IEEE80211_MODE_11B) ? 0 : 3;
	uint32_t trx = 0;

	halWrite(this, R92C_SLOT, &slot, 1);
	halWrite(this, R92C_INIRTS_RATE_SEL, &irts, 1);
	if (halRead(this, R92C_WMAC_TRXPTCL_CTL, &trx, 4) == 0) {
		if (ic->ic_flags & IEEE80211_F_SHPREAMBLE)
			trx |= R92C_WMAC_TRXPTCL_CTL_SHORT;
		else
			trx &= ~(uint32_t)R92C_WMAC_TRXPTCL_CTL_SHORT;
		halWrite(this, R92C_WMAC_TRXPTCL_CTL, &trx, 4);
	}
	IOLog(LOGP "slot=%u us shslot=%d shpre=%d irts=%u\n", slot,
	    (ic->ic_flags & IEEE80211_F_SHSLOT) ? 1 : 0,
	    (ic->ic_flags & IEEE80211_F_SHPREAMBLE) ? 1 : 0, irts);
}

void ItlRtl8188E::updateSlotCb(struct ieee80211com *ic)
{
	ItlRtl8188E *me = ic ? (ItlRtl8188E *)ic->ic_softc : nullptr;
	if (me)
		me->updateSlot(ic);
}

void ItlRtl8188E::camWrite(uint32_t addr, uint32_t data)
{
	rtwn_m1_write_4(&fChip, R92C_CAMWRITE, data);
	rtwn_m1_write_4(&fChip, R92C_CAMCMD,
	    R92C_CAMCMD_POLLING | R92C_CAMCMD_WRITE |
	    ((addr & R92C_CAMCMD_ADDR_M) << R92C_CAMCMD_ADDR_S));
}

uint32_t ItlRtl8188E::camRead(uint32_t addr)
{
	rtwn_m1_write_4(&fChip, R92C_CAMCMD,
	    R92C_CAMCMD_POLLING |
	    ((addr & R92C_CAMCMD_ADDR_M) << R92C_CAMCMD_ADDR_S));
	return rtwn_m1_read_4(&fChip, R92C_CAMREAD);
}

int ItlRtl8188E::setCamKey(int entry, uint8_t key_id, uint8_t algo,
    const uint8_t *macaddr, const uint8_t *key, int keylen)
{
	uint8_t keybuf[16];
	memset(keybuf, 0, sizeof(keybuf));
	if (key && keylen > 0) {
		size_t copylen = (size_t)keylen;
		if (copylen > sizeof(keybuf))
			copylen = sizeof(keybuf);
		memcpy(keybuf, key, copylen);
	}

	/* 
	 * Tulis 6 DWORD (offsets +2..+7) persis seperti CAM_program_entry vendor driver:
	 * DWORD 0..3: AES-128 key material (16 byte).
	 * DWORD 4..5: zero padding untuk MIC/reserved di silikon RTL8188E.
	 */
	for (int i = 0; i < 4; i++) {
		uint32_t k = (uint32_t)keybuf[i * 4] |
		    ((uint32_t)keybuf[i * 4 + 1] << 8) |
		    ((uint32_t)keybuf[i * 4 + 2] << 16) |
		    ((uint32_t)keybuf[i * 4 + 3] << 24);
		camWrite((uint32_t)R92C_CAM_KEY(entry, i), k);
	}
	camWrite((uint32_t)R92C_CAM_KEY(entry, 4), 0);
	camWrite((uint32_t)R92C_CAM_KEY(entry, 5), 0);

	/*
	 * KRUSIAL: Tulis CTL1 (offset +1: MAC[2..5]) TERLEBIH DAHULU, lalu
	 * CTL0 (offset +0: MAC[0..1] + config + R92C_CAM_VALID) TERAKHIR!
	 * Menurut dokumentasi resmi Realtek dan OpenBSD:
	 * "Write CTL0 last since that will validate the CAM entry."
	 * Jika CTL0 ditulis mendahului CTL1, hardware silikon langsung
	 * memvalidasi entri sebelum MAC[2..5] terisi (macaddr menjadi 00:00:00:00),
	 * sehingga pencarian CAM di jalur TX selalu miss!
	 */
	uint32_t ctl1 = (uint32_t)macaddr[2] |
	    ((uint32_t)macaddr[3] << 8) |
	    ((uint32_t)macaddr[4] << 16) |
	    ((uint32_t)macaddr[5] << 24);
	camWrite((uint32_t)R92C_CAM_CTL1(entry), ctl1);

	uint32_t ctl0 = ((uint32_t)macaddr[0] << 16) |
	    ((uint32_t)macaddr[1] << 24) |
	    (((uint32_t)algo & 0x07) << 2) |
	    ((entry >= 4) ? 0 : ((uint32_t)key_id & 0x03)) |
	    R92C_CAM_VALID;
	camWrite((uint32_t)R92C_CAM_CTL0(entry), ctl0);

	/* Verifikasi baca kembali dari register hardware CAM */
	uint32_t r_ctl0 = camRead((uint32_t)R92C_CAM_CTL0(entry));
	uint32_t r_ctl1 = camRead((uint32_t)R92C_CAM_CTL1(entry));
	uint32_t r_k0   = camRead((uint32_t)R92C_CAM_KEY(entry, 0));
	IOLog(LOGP "CAM key %d VERIFY: CTL0=%08x (exp %08x) CTL1=%08x (exp %08x) K0=%08x\n",
	    entry, r_ctl0, ctl0, r_ctl1, ctl1, r_k0);

	return 0;
}

void ItlRtl8188E::deleteCamKey(int entry)
{
	camWrite((uint32_t)R92C_CAM_CTL0(entry), 0);
	camWrite((uint32_t)R92C_CAM_CTL1(entry), 0);
	for (int i = 0; i < 4; i++)
		camWrite((uint32_t)R92C_CAM_KEY(entry, i), 0);
}

int ItlRtl8188E::setKeyCb(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	/* Software crypto penuh di net80211 (SECCFG=0x0000). */
	return ieee80211_set_key(ic, ni, k);
}

void ItlRtl8188E::deleteKeyCb(struct ieee80211com *ic, struct ieee80211_node *ni,
    struct ieee80211_key *k)
{
	ieee80211_delete_key(ic, ni, k);
}

int ItlRtl8188E::newstateCb(struct ieee80211com *ic,
    enum ieee80211_state nstate, int arg)
{
	ItlRtl8188E *me = (ItlRtl8188E *)ic->ic_softc;
	int error;

	if (!me)
		return EINVAL;

	switch (nstate) {
	case IEEE80211_S_INIT:
		me->setDiag("RTL8188E-stage", "state-init");
		break;
	case IEEE80211_S_AUTH:
		me->setDiag("RTL8188E-stage", "state-auth");
		break;
	case IEEE80211_S_ASSOC:
		me->setDiag("RTL8188E-stage", "state-assoc");
		break;
	case IEEE80211_S_RUN:
		me->setDiag("RTL8188E-stage", "state-run");
		break;
	default:
		break;
	}

	switch (nstate) {
	case IEEE80211_S_SCAN: {
		int dwell = 30;

		/*
		 * Turunkan link state internal sebelum scan.
		 *
		 * ieee80211_set_link_state() hanya memanggil
		 * setLinkStatus(kIONetworkLinkActive) kalau if_link_state
		 * BERUBAH. Kalau nilainya masih UP dari sesi sebelumnya
		 * (reconnect tanpa reboot), transisi UP berikutnya tidak
		 * memicu apa pun: IOLinkActiveCount tetap 0, Skywalk tidak
		 * menyerahkan paket, dan TX data mati sampai interface
		 * di-down/up manual.
		 */
		ieee80211_set_link_state(ic, LINK_STATE_DOWN);

		uint32_t rcr = R92C_RCR_APM | R92C_RCR_AM | R92C_RCR_AB |
		    R92C_RCR_ADF | R92C_RCR_AMF |
		    R92C_RCR_APP_ICV | R92C_RCR_HTC_LOC_CTRL |
		    R92C_RCR_APP_MIC | R92C_RCR_APP_PHYSTS;
		halWrite(me, R92C_RCR, &rcr, 4);
		if (!me->fScanning)
			rtwn_set_rx_gain_8188e(halRead, halWrite, me, 0x20);

		me->fScanning = true;
		if (ic->ic_bss && ic->ic_bss->ni_chan)
			me->setChannel(ic->ic_bss->ni_chan);
		if (ic->ic_flags & IEEE80211_F_ASCAN) {
			dwell = 30;
		} else {
			dwell = 50;
		}
		timeout_add_msec(&me->fScanTo, dwell);
		break;
	}
	case IEEE80211_S_AUTH:
	case IEEE80211_S_ASSOC:
	case IEEE80211_S_RUN: {
		me->fScanning = false;
		me->fScanSeen = false;
		if (me->fScanTo)
			timeout_del(&me->fScanTo);
		/*
		 * Setiap kali masuk AUTH adalah percobaan join baru
		 * (net80211 kembali ke SCAN dulu setelah gagal). Pakai
		 * kesempatan itu untuk mengganti varian RSN — satu boot
		 * mencoba beberapa kombinasi tanpa reboot.
		 */
		if (nstate == IEEE80211_S_AUTH) {
			bool isSameBssid = (ic->ic_bss &&
			    ic->ic_bss->ni_bssid[0] != 0 &&
			    IEEE80211_ADDR_EQ(ic->ic_bss->ni_bssid, me->fLastJoinBssid));
			if (!isSameBssid) {
				me->fJoinTry = 0;
				if (ic->ic_bss)
					memcpy(me->fLastJoinBssid, ic->ic_bss->ni_bssid, 6);
			}
			/*
			 * Percobaan ulang BSSID yang sama: beritahu AP dulu bahwa sesi
			 * lama berakhir. Tanpa deauth, AP menyimpan
			 * state "authenticated" untuk MAC kita dan
			 * mengabaikan assoc request berikutnya (AP hanya
			 * membalas deauth reason 2, assoc response tidak
			 * pernah datang).
			 */
			if (me->fJoinTry > 0 && isSameBssid && ic->ic_bss &&
			    ic->ic_bss->ni_bssid[0] != 0) {
				IEEE80211_SEND_MGMT(ic, ic->ic_bss,
				    IEEE80211_FC0_SUBTYPE_DEAUTH,
				    IEEE80211_REASON_AUTH_LEAVE);
				IOLog(LOGP "deauth sebelum retry #%u\n",
				    (unsigned)me->fJoinTry);
			}
			me->fJoinTry++;
			rtwn_set_rx_gain_8188e(halRead, halWrite, me, 0x32);
			me->updateSlot(ic);
			me->applyRsnVariant(me->fJoinTry);
			me->publishStats();
		}
		if (ic->ic_bss) {
			struct ieee80211_node *ni = ic->ic_bss;
			if (ni->ni_chan)
				me->setChannel(ni->ni_chan);
			uint32_t b0 = (uint32_t)ni->ni_bssid[0] |
			    ((uint32_t)ni->ni_bssid[1] << 8) |
			    ((uint32_t)ni->ni_bssid[2] << 16) |
			    ((uint32_t)ni->ni_bssid[3] << 24);
			uint16_t b4 = (uint16_t)ni->ni_bssid[4] |
			    ((uint16_t)ni->ni_bssid[5] << 8);
			halWrite(me, R92C_BSSID + 0, &b0, 4);
			halWrite(me, R92C_BSSID + 4, &b4, 2);
		}
		for (int i = 0; i < IEEE80211_ADDR_LEN; i++) {
			uint8_t b = ic->ic_myaddr[i];
			halWrite(me, R92C_MACID + i, &b, 1);
		}
		if (nstate == IEEE80211_S_RUN) {
			me->fJoinTry = 0;
			rtwn_set_rx_gain_8188e(halRead, halWrite, me, 0x32);
			uint8_t msr = R92C_MSR_NETTYPE_INFRA;
			me->updateSlot(ic);
			halWrite(me, R92C_MSR, &msr, 1);
			/*
			 * Aktifkan R92C_RCR_ACF bersama RXFLTMAP1 = 0x0100 (BAR saja)
			 * agar BlockAckReq (0x84) dari AP masuk ke ieee80211_recv_bar()
			 * dan melepaskan reorder buffer A-MPDU seketika tanpa menunggu
			 * gap timeout 300 ms!
			 */
			uint16_t flt1 = 0x0100;
			halWrite(me, R92C_RXFLTMAP1, &flt1, 2);
			uint16_t flt2 = 0xffff;
			halWrite(me, R92C_RXFLTMAP2, &flt2, 2);
			uint32_t rcr = R92C_RCR_APM | R92C_RCR_AM | R92C_RCR_AB |
			    R92C_RCR_CBSSID_DATA |
			    R92C_RCR_ADF | R92C_RCR_ACF | R92C_RCR_AMF |
			    R92C_RCR_APP_ICV | R92C_RCR_HTC_LOC_CTRL |
			    R92C_RCR_APP_MIC | R92C_RCR_APP_PHYSTS;
			halWrite(me, R92C_RCR, &rcr, 4);
			uint8_t txpause = 0x00;
			halWrite(me, R92C_TXPAUSE, &txpause, 1);
			if (ic->ic_bss) {
				uint16_t bcn_int = ic->ic_bss->ni_intval ? ic->ic_bss->ni_intval : 100;
				halWrite(me, R92C_BCN_INTERVAL, &bcn_int, 2);
				ic->ic_bss->ni_txmcs = 7;
			}
		}
		break;
	}
	case IEEE80211_S_INIT:
		me->fScanning = false;
		me->fScanSeen = false;
		me->fJoinTry = 0;
		memset(me->fLastJoinBssid, 0, sizeof(me->fLastJoinBssid));
		if (me->fScanTo)
			timeout_del(&me->fScanTo);
		/*
		 * Tinggalkan AP dengan rapi. Tanpa ini AP tetap
		 * menganggap kita terasosiasi; join berikutnya (bahkan
		 * setelah reboot) bisa diabaikan AP.
		 */
		if (ic->ic_bss && ic->ic_bss->ni_bssid[0] != 0)
			IEEE80211_SEND_MGMT(ic, ic->ic_bss,
			    IEEE80211_FC0_SUBTYPE_DEAUTH,
			    IEEE80211_REASON_AUTH_LEAVE);
		break;
	default:
		break;
	}

	error = me->fOrigNewstate ? me->fOrigNewstate(ic, nstate, arg) : 0;
	return error;
}

void ItlRtl8188E::scanTimeoutCb(void *arg)
{
	ItlRtl8188E *me = (ItlRtl8188E *)arg;

	if (me->fIc.ic_state == IEEE80211_S_SCAN)
		ieee80211_next_scan(&me->fIc.ic_if);
}

/*
 * Frontend memanggil (*ifp->if_watchdog)(ifp) tiap 1 detik. Tanpa
 * pointer ini, watchdogAction() langsung memanggil NULL → panic.
 * TX kita tidak punya antrean yang bisa macet, jadi cukup bersihkan
 * if_timer.
 */
void ItlRtl8188E::watchdogCb(struct _ifnet *ifp)
{
	ItlRtl8188E *me = ifp ? (ItlRtl8188E *)ifp->if_softc : nullptr;

	/*
	 * Bukan sekadar membersihkan if_timer: net80211 memakai watchdog
	 * ini untuk retry auth/assoc dan memaksa scan ulang kalau AP diam.
	 * Tanpa ini state machine bisa tertahan di AUTH/ASSOC selamanya.
	 */
	if (ifp)
		ieee80211_watchdog(ifp);

	/* Satu kali per detik: publikasikan penghitung ke ioreg. */
	if (me)
		me->publishStats();
}

/* ------------------------------------------------------------------ */
/*  TX                                                                 */
/* ------------------------------------------------------------------ */

void ItlRtl8188E::txStartCb(struct _ifnet *ifp)
{
	ItlRtl8188E *me = (ItlRtl8188E *)ifp->if_softc;
	if (me)
		me->txStart(ifp);
}

void ItlRtl8188E::sendFrame(mbuf_t m, struct ieee80211_node *ni, bool isMgmt)
{
	uint8_t *wh;
	uint32_t len, pktlen;
	bool bcast;
	uint8_t raid;
	uint8_t *buf;
	size_t total;

	if (!m)
		return;

	/*
	 * Semua frame (mgmt maupun data) datang dengan referensi node
	 * yang harus dilepas driver. Jangan bocorkan saat bulk belum siap.
	 */
	if (!fBulkReady) {
		mbuf_freem(m);
		if (ni)
			ieee80211_release_node(&fIc, ni);
		return;
	}

	wh = (uint8_t *)mbuf_data(m);
	if (!wh) {
		mbuf_freem(m);
		if (ni)
			ieee80211_release_node(&fIc, ni);
		return;
	}

	/*
	 * Pastikan header 802.11 + LLC (maks 40 byte) kontigu di mbuf pertama.
	 * JANGAN pernah pullup seluruh frame ('len'): di Darwin 64-bit, MHLEN
	 * hanya ~88 byte. Pullup > 88 byte (mis. DHCP 360B atau TCP 1500B)
	 * memicu "m_pullup: failed len ... too big" di XNU kernel, membebaskan
	 * mbuf, dan membuang semua frame data keluar!
	 * mbuf_copydata() di bawah sudah menyalin seluruh chain mbuf ke buffer DMA.
	 */
	len = mbuf_pkthdr_len(m);
	{
		size_t hdrPull = (len < 40) ? len : 40;
		if (mbuf_len(m) < hdrPull) {
			if (mbuf_pullup(&m, hdrPull) != 0) {
				if (ni)
					ieee80211_release_node(&fIc, ni);
				return;
			}
			wh = (uint8_t *)mbuf_data(m);
		}
	}

	if (len < sizeof(struct ieee80211_frame)) {
		mbuf_freem(m);
		if (ni)
			ieee80211_release_node(&fIc, ni);
		return;
	}

	/*
	 * Cek jenis frame (EAPOL, ARP, ICMP ping, DNS) SEBELUM enkripsi CCMP!
	 * Setelah ieee80211_encrypt(), payload 802.11 sudah menjadi ciphertext AES,
	 * sehingga pengecekan ether_type / IP proto di bawah tidak akan pernah cocok
	 * jika dilakukan setelah enkripsi.
	 */
	bool isEapol = false;
	bool isControl = isMgmt;
	if (!isMgmt) {
		size_t llcOff = (wh[0] & IEEE80211_FC0_SUBTYPE_QOS) ? 26 : 24;
		if (len >= llcOff + 8) {
			uint16_t et = ((uint16_t)wh[llcOff + 6] << 8) | wh[llcOff + 7];
			if (et == 0x888e) {
				isEapol = true;
				isControl = true;
				fTxEapol++;
				IOLog(LOGP "TX EAPOL handshake frame (len %u)\n", (unsigned)len);
			} else if (et == 0x0806) {
				/* ARP: latency-sensitive */
				isControl = true;
			} else if (et == 0x0800 && len >= llcOff + 8 + 20) {
				/* IPv4: periksa protokol di byte 9 dari header IP (1 = ICMP / ping, 2 = IGMP) */
				uint8_t proto = wh[llcOff + 8 + 9];
				if (proto == 1 /* ICMP */ || proto == 2 /* IGMP */) {
					isControl = true;
				} else if (proto == 17 && len >= llcOff + 8 + 28) {
					/* UDP: port 53 (DNS) */
					uint16_t dport = ((uint16_t)wh[llcOff + 8 + 22] << 8) | wh[llcOff + 8 + 23];
					if (dport == 53) {
						isControl = true;
					}
				}
			} else if (et == 0x86dd && len >= llcOff + 8 + 40) {
				/* IPv6: periksa next header (byte 6) (58 = ICMPv6) */
				uint8_t nexthdr = wh[llcOff + 8 + 6];
				if (nexthdr == 58 /* ICMPv6 */) {
					isControl = true;
				} else if (nexthdr == 17 && len >= llcOff + 8 + 48) {
					/* UDP: port 53 (DNS) */
					uint16_t dport = ((uint16_t)wh[llcOff + 8 + 42] << 8) | wh[llcOff + 8 + 43];
					if (dport == 53) {
						isControl = true;
					}
				}
			}
		}
	}

	/*
	 * Kripto software: 8188E memakai software CCMP di net80211 (SECCFG=0x0000)
	 * yang sudah terbukti sukses dua arah pada v0.7.5 (ping) & v0.7.6/v0.7.7 (DHCP),
	 * ditambah perbaikan alokasi cluster mbuf_get_mhlen() (6e43ea0) untuk frame > 88B.
	 */
	if (!isMgmt && (wh[1] & IEEE80211_FC1_PROTECTED)) {
		struct ieee80211_key *k = ieee80211_get_txkey(&fIc,
		    (struct ieee80211_frame *)wh, ni);
		if (!k) {
			/* Frame terproteksi tanpa key jangan dikirim polos. */
			mbuf_freem(m);
			if (ni)
				ieee80211_release_node(&fIc, ni);
			return;
		}
		mbuf_t enc = ieee80211_encrypt(&fIc, m, k);
		if (!enc) {
			/*
			 * ieee80211_encrypt() sudah membebaskan m
			 * (baik sukses maupun gagal). Jangan free lagi.
			 */
			fEncryptFail++;
			if (ni)
				ieee80211_release_node(&fIc, ni);
			return;
		}
		m = enc;
		wh = (uint8_t *)mbuf_data(m);
		len = mbuf_pkthdr_len(m);
	}

	bcast = IEEE80211_IS_MULTICAST(wh + 4);
	/*
	 * RAID (Rate Adaptive ID):
	 *   - Data unicast: 0 = RATR_INX_WIRELESS_NGB (tabel 11n MCS 0..7 + 11g + 11b)
	 *   - Bcast / Mgmt / EAPOL: 6 = RAID 11B (keandalan maksimal pada basic rates)
	 */
	raid = (bcast || isMgmt || isEapol) ? 6 : 0;

	/*
	 * Hitung per-subtype. Tanpa ini "auth tidak dikirim" dan
	 * "auth dikirim tapi AP diam" terlihat sama di statistik.
	 */
	if (isMgmt) {
		uint8_t sub = wh[0] & IEEE80211_FC0_SUBTYPE_MASK;

		if (sub == IEEE80211_FC0_SUBTYPE_AUTH) {
			fTxAuthReq++;
		} else if (sub == IEEE80211_FC0_SUBTYPE_ASSOC_REQ ||
		    sub == IEEE80211_FC0_SUBTYPE_REASSOC_REQ) {
			fTxAssocReq++;
			/*
			 * Ringkas: header + capability + awal SSID/RSN.
			 * snprintf per-byte di kernel tidak andal untuk
			 * buffer panjang, jadi pakai dword.
			 */
			if (len >= 24) {
				const uint32_t *dw = (const uint32_t *)wh;

				IOLog(LOGP "TX assoc len=%u fc=%02x%02x "
				    "dw1=%08x dw2=%08x dw3=%08x "
				    "dw4=%08x dw5=%08x\n",
				    (unsigned)len, wh[0], wh[1],
				    dw[1], dw[2], dw[3], dw[4], dw[5]);
			}
			/*
			 * Body: dw6 = capability+listen, dw7.. = IE.
			 * AP diam tanpa assoc response berarti body yang
			 * dibuang — capability atau RSN IE salah.
			 */
			if (len >= 56) {
				const uint32_t *dw = (const uint32_t *)wh;

				IOLog(LOGP "TX assoc body cap=%04x li=%u "
				    "dw7=%08x dw8=%08x dw9=%08x "
				    "dw10=%08x dw11=%08x dw12=%08x\n",
				    (unsigned)(wh[24] | (wh[25] << 8)),
				    (unsigned)(wh[26] | (wh[27] << 8)),
				    dw[7], dw[8], dw[9], dw[10], dw[11],
				    dw[12]);
			}
		}
	}

	/*
	 * Pilih queue TX (dan bulk-out pipe) sesuai access category.
	 *
	 * Pada 2 bulk-out pipe (TRXDMA_CTRL = 0xfaf0 / QMAP_HQ_NQ):
	 *   pipe 0 = VO/VI/MGNT/HIGH (EP 0x02, High Queue FIFO)
	 *   pipe 1 = BE/BK data      (EP 0x03, Normal Queue FIFO)
	 */
	{
		int ntx = fChip.usb.txPipeCount();
		uint8_t qsel = 0x00;	/* BE */
		int pipe = (ntx >= 3) ? 2 : ((ntx == 2) ? 1 : 0);
		bool qos = !isMgmt && ((wh[0] & IEEE80211_FC0_SUBTYPE_QOS) != 0);
		uint8_t tid = qos ? (wh[24] & 0x0f) : 0;

		if (isMgmt) {
			qsel = 0x12;	/* MGNT */
			pipe = 0;
		} else if (qos) {
			/*
			 * KRUSIAL untuk BlockAck (A-MPDU):
			 * Semua frame QoS dengan TID yang sama WAJIB melewati queue
			 * hardware (qsel & pipe) yang sama agar nomor urut ni_qos_txseqs[tid]
			 * diterima berurutan oleh reorder buffer AP (tidak memicu gap stall!).
			 */
			if (tid >= 6) {
				qsel = 0x07;	/* VO */
				pipe = 0;
			} else if (tid >= 4) {
				qsel = 0x05;	/* VI */
				pipe = (ntx >= 3) ? 1 : 0;
			} else if (tid >= 1 && tid <= 2) {
				qsel = 0x02;	/* BK */
				pipe = (ntx >= 3) ? 2 : ((ntx == 2) ? 1 : 0);
			} else {
				qsel = 0x00;	/* BE */
				pipe = (ntx >= 3) ? 2 : ((ntx == 2) ? 1 : 0);
			}
		} else if (isControl) {
			/*
			 * Frame non-QoS sensitif latensi (EAPOL handshake, ARP/ICMP/DNS
			 * sebelum BlockAck aktif) dikirim lewat High Queue FIFO (pipe 0).
			 */
			qsel = 0x07;	/* VO / High Priority */
			pipe = 0;
		}

		total = RTWN_USB_TX_DESC_SIZE + len;
		bool useScratch = (total <= sizeof(fTxScratch));
		buf = useScratch ? fTxScratch : (uint8_t *)IOMalloc(total);
		if (!buf) {
			mbuf_freem(m);
			if (ni)
				ieee80211_release_node(&fIc, ni);
			return;
		}

		if (isMgmt) {
			rtwn_usb_build_mgmt_tx_desc(buf, RTWN_USB_TX_DESC_SIZE,
			    (uint16_t)len, bcast);
			if (fBw40) {
				buf[18] |= (uint8_t)((fPriChLo ? 1 : 2) << 4);
				rtwn_usb_tx_checksum(buf, RTWN_USB_TX_DESC_SIZE);
			}
		} else {
			uint16_t seq = ((uint16_t)wh[22] |
			    ((uint16_t)wh[23] << 8)) >> IEEE80211_SEQ_SEQ_SHIFT;
			bool canAgg = qos && !bcast && !isEapol && ni &&
			    (ni->ni_tx_ba[tid].ba_state == IEEE80211_BA_AGREED);

			rtwn_usb_build_tx_desc(buf, RTWN_USB_TX_DESC_SIZE,
			    (uint16_t)len, RTWN_USB_TX_DESC_SIZE, bcast, true,
			    raid, false, false, 0);
			/* QSEL (txdw1 bit 8-12) harus sama dengan queue pipe. */
			buf[5] = (uint8_t)((buf[5] & 0xe0) | (qsel & 0x1f));

			/*
			 * A-MPDU TX Aggregation (sesuai TxFillDescriptor8188EUsb
			 * @0x14d5c4..0x14d5d7 di RtWlanU.kext):
			 *   - Bila BA_AGREED aktif pada TID ini: aktifkan R88E_TXDW2_AGG_EN
			 *     (buf[9] |= 0x10), matikan AGGBK (buf[10] &= ~0x01), dan set
			 *     txdw6 = 0x6666f800 (batas agregasi & densitas MPDU).
			 *   - Bila belum BA_AGREED / EAPOL / bcast: pasang AGGBK (buf[10] |= 0x01).
			 */
			if (canAgg) {
				buf[9] |= 0x10;		/* R88E_TXDW2_AGG_EN */
				buf[10] &= ~0x01;	/* clear R88E_TXDW2_AGGBK */
				buf[25] = 0xf8;
				buf[26] = 0x66;
				buf[27] = 0x66;
				fTxAgg++;
			} else {
				buf[10] |= 0x01;	/* R88E_TXDW2_AGGBK */
			}

			/*
			 * Sesuai TxFillDescriptor8188EUsb (@0x14d66d-0x14d694) di RtWlanU.kext:
			 *   - seq & 0x0fff selalu ditulis ke buf[14..15]
			 *   - Non-QoS (!qos): txdw3 |= 0x80000000 (buf[15] |= 0x80) dan
			 *     pada RTL8188E (cardType == 0x0f), txdw4 |= 0x40 (buf[16] = 0x40 = R88E_HWSEQ_EN).
			 *   - QoS Data (qos): JANGAN set 0x80 di buf[15] dan JANGAN set 0x40 di buf[16].
			 */
			buf[14] = (uint8_t)(seq & 0xff);
			buf[15] = (uint8_t)((seq >> 8) & 0x0f);
			if (qos) {
				buf[16] = 0x00;
				buf[17] = 0x01;	/* DRVRATE */
			} else {
				buf[15] |= 0x80;	/* txdseq: HWSEQEN */
				buf[16] = 0x40;		/* R88E_TXDW4_HWSEQ_EN */
				buf[17] = 0x01;		/* DRVRATE */
			}
			buf[18] = 0x00;
			buf[19] = 0x00;
			/*
			 * txdw4 & txdw5:
			 *   - Unicast data: 0x13 (MCS 7 = 65/72.2 Mb/s di 20 MHz, 135/150 Mb/s di 40 MHz).
			 *   - Mgmt / Bcast / EAPOL: 0x00 (CCK 1 Mb/s).
			 *   - 40 MHz (HT40): pasang R92C_TXDW4_40MHZ (buf[19] |= 0x02) + SGI40.
			 *   - Frame 20 MHz pada BSS 40 MHz: pasang DATA_SC di buf[18] bit 4..5.
			 */
			uint8_t rate = (!bcast && !isEapol && !isMgmt) ? 0x13 : 0x00;
			bool use40 = (rate == 0x13) && fBw40 && ni &&
			    (ni->ni_chw == IEEE80211_CHAN_WIDTH_40);
			if (use40) {
				buf[19] |= 0x02;	/* R92C_TXDW4_40MHZ */
				if ((ni->ni_flags & IEEE80211_NODE_HT_SGI40) ||
				    (ni->ni_htcaps & IEEE80211_HTCAP_SGI40)) {
					rate |= 0x40;
				}
			} else {
				if (fBw40) {
					buf[18] |= (uint8_t)((fPriChLo ? 1 : 2) << 4);
				}
				if (rate == 0x13 && fIc.ic_bss &&
				    ((fIc.ic_bss->ni_flags & IEEE80211_NODE_HT_SGI20) ||
				     (fIc.ic_bss->ni_htcaps & IEEE80211_HTCAP_SGI20))) {
					rate |= 0x40;
				}
			}
			buf[10] &= ~0x08;	/* pastikan CCX_RPT mati agar tidak membanjiri Bulk-IN */
			buf[20] = rate;
			buf[21] = 0x1f;		/* batas fallback rate (RtWlanU @0x14d22f) */
			buf[22] = 0x01;
			buf[23] = 0x00;
			rtwn_usb_tx_checksum(buf, RTWN_USB_TX_DESC_SIZE);
		}

		pktlen = len;
		if (mbuf_copydata(m, 0, pktlen, buf + RTWN_USB_TX_DESC_SIZE) != 0) {
			if (!useScratch)
				IOFree(buf, total);
			mbuf_freem(m);
			if (ni)
				ieee80211_release_node(&fIc, ni);
			return;
		}

		if (!fChip.usb.bulkSend(buf, (uint32_t)total, nullptr, nullptr,
		    pipe)) {
			static int failLog = 0;
			if (failLog++ < 10 || (failLog % 100) == 0) {
				IOLog(LOGP "bulkSend gagal (%u byte, pipe %d, count %d)\n",
				    (unsigned)total, pipe, failLog);
			}
			fTxFail++;
		} else if (isMgmt) {
			fTxMgmt++;
		} else {
			fTxData++;
			if (fIc.ic_if.netStat)
				fIc.ic_if.netStat->outputPackets++;
		}

		if (!useScratch)
			IOFree(buf, total);
		mbuf_freem(m);

		/* net80211 menyerahkan referensi node ke driver. */
		if (ni)
			ieee80211_release_node(&fIc, ni);
		return;
	}
}

void ItlRtl8188E::txDoneHook(void *ctx)
{
	ItlRtl8188E *me = (ItlRtl8188E *)ctx;
	if (me && me->fTxTimer) {
		me->fTxTimer->setTimeoutMS(0);
	}
}

void ItlRtl8188E::txTimerAction(IOTimerEventSource *timer)
{
	(void)timer;
	if (!fIfAttached)
		return;
	struct _ifnet *ifp = &fIc.ic_if;
	if (ifp->if_snd.queue) {
		ifq_clr_oactive(&ifp->if_snd);
		if (!ifq_empty(&ifp->if_snd)) {
			txStart(ifp);
		}
	}
}

void ItlRtl8188E::txStart(struct _ifnet *ifp)
{
	struct ieee80211com *ic = &fIc;
	struct ieee80211_node *ni;
	mbuf_t m;

	/*
	 * Diagnostik: apakah if_start benar-benar dipanggil, dan kalau ya
	 * apakah loop data di bawah terlewati karena state/xflags.
	 */
	{
		static int txsLog = 0;
		if (txsLog < 8) {
			txsLog++;
			IOLog(LOGP "txStart #%d state=%d xf=%08x sndq=%d empty=%d\n",
			    txsLog, (int)ic->ic_state,
			    (unsigned)ic->ic_xflags,
			    ifp->if_snd.queue ? 1 : 0,
			    ifp->if_snd.queue ?
			    (ifq_empty(&ifp->if_snd) ? 1 : 0) : -1);
		}
	}

	while ((m = mq_dequeue(&ic->ic_mgtq)) != nullptr) {
		ni = (struct ieee80211_node *)mbuf_pkthdr_rcvif(m);
		sendFrame(m, ni, true);
	}

	if ((ic->ic_state != IEEE80211_S_RUN && ic->ic_state != IEEE80211_S_ASSOC) ||
	    (ic->ic_xflags & IEEE80211_F_TX_MGMT_ONLY))
		return;

	/*
	 * Pacing / Flow Control:
	 * Hanya kirim frame data jika antrean USB belum sibuk (!txBusy).
	 * Jika antrean USB penuh, tandai ifq_set_oactive dan hentikan
	 * mengambil paket dari if_snd. Paket tetap aman di antrean.
	 * Saat completion USB mengurangi fTxInFlight, txDoneHook akan
	 * membangunkan fTxTimer untuk membersihkan oactive dan memanggil
	 * txStart kembali.
	 */
	while (!fChip.usb.txBusy()) {
		m = ifq_dequeue(&ifp->if_snd);
		if (!m) break;
		ni = nullptr;
		m = ieee80211_encap(ifp, m, &ni);
		if (!m) {
			if (ifp->netStat)
				ifp->netStat->outputErrors++;
			continue;
		}
		sendFrame(m, ni, false);
	}

	if (fChip.usb.txBusy()) {
		ifq_set_oactive(&ifp->if_snd);
	} else {
		ifq_clr_oactive(&ifp->if_snd);
	}
}

/* ------------------------------------------------------------------ */
/*  RX                                                                 */
/* ------------------------------------------------------------------ */

void ItlRtl8188E::rxCb(void *ctx, const uint8_t *data, uint32_t len)
{
	ItlRtl8188E *me = (ItlRtl8188E *)ctx;
	uint8_t *copy;
	uint32_t idx;
	bool wasEmpty = false;

	if (!me || !me->fRxRunning || !me->fRxTimer || !me->fRxLock ||
	    !data || len == 0)
		return;

	/*
	 * Salin sekarang: buffer transfer akan segera di-queue ulang oleh
	 * usb_compat. Jangan memproses net80211 di thread completion USB —
	 * workloop USB bisa deadlock dengan control transfer di workloop
	 * IO80211. Cukup titip ke antrean lalu bangunkan timer 0 ms.
	 */
	copy = (uint8_t *)IOMalloc(len);
	if (!copy)
		return;
	memcpy(copy, data, len);

	IOLockLock(me->fRxLock);
	if (me->fRxCount >= kRxQueueDepth) {
		IOLockUnlock(me->fRxLock);
		IOFree(copy, len);
		return;
	}
	wasEmpty = (me->fRxCount == 0);
	idx = (me->fRxHead + me->fRxCount) % kRxQueueDepth;
	me->fRxBuf[idx] = copy;
	me->fRxLen[idx] = len;
	me->fRxCount++;
	IOLockUnlock(me->fRxLock);

	if (wasEmpty)
		me->fRxTimer->setTimeoutMS(0);
}

void ItlRtl8188E::rxTimerAction(IOTimerEventSource *timer)
{
	uint8_t *bufs[32];
	uint32_t lens[32];
	int maxLoops = 8;

	(void)timer;

	while (maxLoops-- > 0) {
		uint32_t n = 0, i;

		IOLockLock(fRxLock);
		while (fRxCount > 0 && n < 32) {
			bufs[n] = fRxBuf[fRxHead];
			lens[n] = fRxLen[fRxHead];
			fRxBuf[fRxHead] = nullptr;
			fRxLen[fRxHead] = 0;
			fRxHead = (fRxHead + 1) % kRxQueueDepth;
			fRxCount--;
			n++;
		}
		IOLockUnlock(fRxLock);

		if (n == 0)
			break;

		for (i = 0; i < n; i++) {
			if (fRxRunning)
				handleRx(bufs[i], lens[i]);
			IOFree(bufs[i], lens[i]);
		}
	}

	/*
	 * Jika masih ada paket tersisa dalam antrean saat burst besar, jadwal ulang
	 * segera. Ini menyerahkan thread workloop sejenak agar txStart (ping dan TCP ACK)
	 * dapat berjalan tanpa tertahan antrean RX (mencegah latency spike saat download).
	 */
	IOLockLock(fRxLock);
	bool hasMore = (fRxCount > 0);
	IOLockUnlock(fRxLock);
	if (hasMore && fRxRunning) {
		fRxTimer->setTimeoutMS(0);
	}
}

void ItlRtl8188E::rxQueueFree(void)
{
	uint8_t *bufs[32];
	uint32_t lens[32];

	if (!fRxLock)
		return;

	for (;;) {
		uint32_t n = 0, i;

		IOLockLock(fRxLock);
		while (fRxCount > 0 && n < 32) {
			bufs[n] = fRxBuf[fRxHead];
			lens[n] = fRxLen[fRxHead];
			fRxBuf[fRxHead] = nullptr;
			fRxLen[fRxHead] = 0;
			fRxHead = (fRxHead + 1) % kRxQueueDepth;
			fRxCount--;
			n++;
		}
		IOLockUnlock(fRxLock);

		if (n == 0)
			break;
		for (i = 0; i < n; i++)
			IOFree(bufs[i], lens[i]);
	}
}

static inline uint32_t
rxDescDw(const uint8_t *buf, uint32_t off, int dw)
{
	const uint8_t *p = buf + off + dw * 4;
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Validasi satu slot RX: descriptor masuk akal + frame 802.11 di posisi
 * canonical (24 + infosz). Dipakai untuk memilih stride 128 vs 4 byte.
 */
static bool
rxSlotOk(const uint8_t *buf, uint32_t len, uint32_t off)
{
	rtwn_usb_rxinfo_t info;
	const struct ieee80211_frame *wh;
	uint32_t foff, dw3;
	uint8_t type, rpt_sel;

	if (off + RTWN_USB_RX_DESC_SIZE > len)
		return false;
	if (!rtwn_usb_parse_rx_desc(buf + off, len - off, &info))
		return false;
	dw3 = rxDescDw(buf, off, 3);
	rpt_sel = (uint8_t)((dw3 >> 14) & 0x03);
	if (rpt_sel != 0)
		return info.pktlen > 0 && info.pktlen <= 512;
	if (info.pktlen == 0 || info.pktlen > 4096)
		return false;
	foff = off + RTWN_USB_RX_DESC_SIZE + info.infosz + info.shift;
	if (foff + info.pktlen > len ||
	    info.pktlen < sizeof(struct ieee80211_frame_min))
		return false;
	wh = (const struct ieee80211_frame *)(buf + foff);
	if ((wh->i_fc[0] & IEEE80211_FC0_VERSION_MASK) !=
	    IEEE80211_FC0_VERSION_0)
		return false;
	type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
	return type == IEEE80211_FC0_TYPE_MGT ||
	    type == IEEE80211_FC0_TYPE_DATA ||
	    type == IEEE80211_FC0_TYPE_CTL;
}

void ItlRtl8188E::handleRx(const uint8_t *buf, uint32_t len)
{
	struct _ifnet *ifp = &fIc.ic_if;
	struct mbuf_list ml = MBUF_LIST_INITIALIZER();
	uint32_t off = 0;

	while (off + RTWN_USB_RX_DESC_SIZE <= len) {
		rtwn_usb_rxinfo_t info;
		uint32_t flen, foff, dw3, rawLen, s4, s8, s128, next;
		struct ieee80211_frame *wh;
		struct ieee80211_node *ni;
		struct ieee80211_rxinfo rxi;
		mbuf_t m;
		int8_t rssi = -95;
		uint8_t rpt_sel;
		bool txReport;

		if (!rtwn_usb_parse_rx_desc(buf + off, len - off, &info))
			break;

		flen = info.pktlen;
		if (flen == 0 || flen > 4096)
			break;

		dw3 = rxDescDw(buf, off, 3);
		foff = off + RTWN_USB_RX_DESC_SIZE + info.infosz + info.shift;
		if (foff + flen > len)
			break;

		/*
		 * RTL8188E memakai RXDW3 bit 14-15 untuk jenis report:
		 *   0 = frame 802.11 normal
		 *   1 = CCX TX report (8 byte)
		 *   2 = ODM RA TX report 2
		 *   3 = USB HISR report
		 */
		rpt_sel = (uint8_t)((dw3 >> 14) & 0x3);
		txReport = (rpt_sel != 0);

		if (!txReport && (info.crcError || info.icvError)) {
			if (foff + 2 <= len) {
				IOLog(LOGP "RX crc-err fc=%02x%02x len=%u\n",
				    buf[foff], buf[foff + 1],
				    (unsigned)flen);
			}
			txReport = true;
		}

		if (rpt_sel == 1 && foff + 4 <= len) {
			const uint8_t *rpt = buf + foff;

			fTxReport++;
			if (rpt[1] & 0x40) {
				fTxOk++;
			}
			if (rpt[2] & 0x3f) {
				fTxRetry++;
			}
		}

		/*
		 * _SegregateDmaAggPackets_8188E (@0x14c7fa) memakai alignment 8-byte,
		 * sedangkan _SegregateUsbAggPackets_8188E (@0x14c984) memakai 128-byte.
		 * Jangan batasi loop dengan npkts ((dw2 >> 16) & 0xff) karena pada
		 * agregasi DMA atau setelah slot TX report nilainya 0!
		 */
		rawLen = RTWN_USB_RX_DESC_SIZE + info.infosz + info.shift + flen;
		s4 = (rawLen + 3) & ~3u;
		s8 = (rawLen + 7) & ~7u;
		s128 = (rawLen + 127) & ~127u;
		if (rxSlotOk(buf, len, off + s8))
			next = off + s8;
		else if (rxSlotOk(buf, len, off + s4))
			next = off + s4;
		else if (rxSlotOk(buf, len, off + s128))
			next = off + s128;
		else
			next = 0;

		if (!txReport) {
			uint8_t type, sub;

			/*
			 * BlockAckReq (BAR, fc=0x84) adalah frame CTL 20 byte
			 * (sizeof(ieee80211_frame_min) = 16B + 4B BAR control/SSN).
			 * Jangan buang frame CTL 20 byte dengan batas 24 byte!
			 */
			if (flen < sizeof(struct ieee80211_frame_min))
				goto advance;
			wh = (struct ieee80211_frame *)(buf + foff);
			if ((wh->i_fc[0] & IEEE80211_FC0_VERSION_MASK) !=
			    IEEE80211_FC0_VERSION_0)
				goto advance;

			type = wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK;
			sub = wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK;
			if (type != IEEE80211_FC0_TYPE_CTL &&
			    flen < sizeof(struct ieee80211_frame))
				goto advance;

			/*
			 * Dalam STA mode:
			 * 1. Jangan pernah memproses Probe Request (sub=0x40).
			 * 2. Jangan pernah memproses frame unicast yang bukan untuk ic_myaddr.
			 * 3. Frame CTL hanya boleh diproses saat RUN dan hanya berupa
			 *    BlockAckReq (BAR >= 20B) dari BSSID kita (tanpa menyentuh i_addr3!).
			 * 4. Saat state RUN (dan tidak sedang scan/bgscan), buang frame
			 *    data/mgmt dari BSSID asing agar tidak menimpa ni_rxseq / ni_rssi
			 *    pada fIc.ic_bss atau memicu error dekripsi CCMP.
			 */
			if (type == IEEE80211_FC0_TYPE_MGT &&
			    sub == IEEE80211_FC0_SUBTYPE_PROBE_REQ)
				goto advance;

			if (!IEEE80211_IS_MULTICAST(wh->i_addr1) &&
			    !IEEE80211_ADDR_EQ(wh->i_addr1, fIc.ic_myaddr))
				goto advance;

			if (type == IEEE80211_FC0_TYPE_CTL) {
				if (fIc.ic_state != IEEE80211_S_RUN || !fIc.ic_bss ||
				    sub != IEEE80211_FC0_SUBTYPE_BAR || flen < 20 ||
				    !IEEE80211_ADDR_EQ(wh->i_addr2, fIc.ic_bss->ni_bssid))
					goto advance;
				fRxBar++;
			} else if (fIc.ic_state == IEEE80211_S_RUN && fIc.ic_bss) {
				if (type == IEEE80211_FC0_TYPE_DATA &&
				    !IEEE80211_ADDR_EQ(wh->i_addr2, fIc.ic_bss->ni_bssid))
					goto advance;
				if (type == IEEE80211_FC0_TYPE_MGT &&
				    sub != IEEE80211_FC0_SUBTYPE_BEACON &&
				    sub != IEEE80211_FC0_SUBTYPE_PROBE_RESP &&
				    !(fIc.ic_flags & IEEE80211_F_BGSCAN) &&
				    !fIc.ic_wcl_scan_requested &&
				    !IEEE80211_ADDR_EQ(wh->i_addr2, fIc.ic_bss->ni_bssid) &&
				    !IEEE80211_ADDR_EQ(wh->i_addr3, fIc.ic_bss->ni_bssid))
					goto advance;
			}

			if (!fSawRx) {
				const char *kind = "data";

				fSawRx = true;
				switch (type) {
				case IEEE80211_FC0_TYPE_MGT:
					kind = (sub ==
					    IEEE80211_FC0_SUBTYPE_BEACON) ?
					    "beacon" : "mgmt";
					break;
				case IEEE80211_FC0_TYPE_CTL:
					kind = "ctrl";
					break;
				default:
					break;
				}
				setDiag("RTL8188E-first-rx", kind);
			}

			if (type == IEEE80211_FC0_TYPE_DATA) {
				size_t llcOff = (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_QOS) ? 26 : 24;

				fRxData++;
				if (fRxDataLogged < 3) {
					fRxDataLogged++;
					IOLog(LOGP "RX data fc=%02x%02x len=%u "
					    "prot=%d qos=%d\n",
					    wh->i_fc[0], wh->i_fc[1],
					    (unsigned)flen,
					    (wh->i_fc[1] &
					    IEEE80211_FC1_PROTECTED) ? 1 : 0,
					    (wh->i_fc[0] &
					    IEEE80211_FC0_SUBTYPE_QOS) ? 1 : 0);
				}
				if (flen >= llcOff + 8) {
					const uint8_t *p = buf + foff + llcOff;
					uint16_t et = ((uint16_t)p[6] << 8) | p[7];
					if (et == 0x888e) {
						fRxEapol++;
						IOLog(LOGP "RX EAPOL handshake frame (len %u)\n", (unsigned)flen);
					}
				}
			} else if (type == IEEE80211_FC0_TYPE_MGT) {
				fRxMgmt++;
				if (sub == IEEE80211_FC0_SUBTYPE_AUTH) {
					fRxAuth++;
					IOLog(LOGP "RX auth resp (len %u, seq %u)\n",
					    (unsigned)flen,
					    (unsigned)(((uint16_t)wh->i_seq[0] |
					    ((uint16_t)wh->i_seq[1] << 8)) >>
					    IEEE80211_SEQ_SEQ_SHIFT));
				} else if (sub ==
				    IEEE80211_FC0_SUBTYPE_ASSOC_RESP) {
					fRxAssocResp++;
					IOLog(LOGP "RX assoc resp (len %u)\n",
					    (unsigned)flen);
				} else if (sub != IEEE80211_FC0_SUBTYPE_BEACON &&
				    sub != IEEE80211_FC0_SUBTYPE_PROBE_RESP) {
					IOLog(LOGP "RX mgmt sub=%02x len=%u\n",
					    sub, (unsigned)flen);
				}
			}

			/* RSSI dari PHY status (rtwn_r88e_get_rssi). */
			if (info.infosz > 0 && info.hasPhyStatus) {
				const uint8_t *phy = buf + foff - info.infosz;

				if (info.rate <= 3) {
					static const int8_t cckoff[] =
					    { 20, 14, 10, -4, -16, -22, -38, -40 };
					uint8_t agc = phy[5];
					uint8_t rpt = (agc >> 5) & 0x7;

					rssi = ((agc & 0x1f) > 27) ? -94 :
					    (int8_t)(cckoff[rpt] -
					    ((agc & 0x1f) << 1));
				} else {
					uint32_t sq = (uint32_t)phy[4] |
					    ((uint32_t)phy[5] << 8) |
					    ((uint32_t)phy[6] << 16) |
					    ((uint32_t)phy[7] << 24);

					rssi = (int8_t)(((sq >> 1) & 0x7f) - 110);
				}
			}

			m = getController() ?
			    getController()->allocatePacket(flen) : nullptr;
			if (m) {
				if (mbuf_copyback(m, 0, flen, buf + foff,
				    MBUF_DONTWAIT) == 0) {
					if (fIc.ic_state ==
					    IEEE80211_S_RUN && fIc.ic_bss &&
					    (type == IEEE80211_FC0_TYPE_DATA ||
					     IEEE80211_ADDR_EQ(wh->i_addr2, fIc.ic_bss->ni_bssid))) {
						ni = ieee80211_ref_node(
						    fIc.ic_bss);
					} else {
						ni = ieee80211_find_rxnode(
						    &fIc, wh);
					}
					memset(&rxi, 0, sizeof(rxi));
					/*
					 * Port net80211 ini memakai RSSI ternormalisasi
					 * 0..ic_max_rssi (bukan dBm), sama seperti iwm:
					 * frontend menghitung asr_rssi = rssi - 100.
					 * rxi/ni_rssi bertipe u8; dBm negatif akan wrap.
					 */
					{
						int norm = (int)rssi + 100;

						if (norm < 0)
							norm = 0;
						if (norm > fIc.ic_max_rssi)
							norm = fIc.ic_max_rssi;
						rxi.rxi_rssi = norm;
					}
					ieee80211_inputm(ifp, m, ni, &rxi,
					    &ml);
					ieee80211_release_node(&fIc, ni);
				} else {
					mbuf_freem(m);
				}
			}
		}

advance:
		if (next == 0 || next <= off)
			break;
		off = next;
	}

	if (ml.ml_len > 0)
		fRxDelivered += ml.ml_len;
	if_input(ifp, &ml);
}

/* ------------------------------------------------------------------ */
/*  ItlDriverInfo                                                      */
/* ------------------------------------------------------------------ */

const char *ItlRtl8188E::getFirmwareVersion(void)
{
	return fFirmwareVersion[0] ? fFirmwareVersion : "rtl8188eufw (1.28)";
}

int16_t ItlRtl8188E::getBSSNoise(void)
{
	return -95;
}

bool ItlRtl8188E::is5GBandSupport(void)
{
	return false;
}

int ItlRtl8188E::getTxNSS(void)
{
	return 1;
}

const char *ItlRtl8188E::getFirmwareName(void)
{
	return "rtl8188eufw.bin";
}

UInt32 ItlRtl8188E::supportedFeatures(void)
{
	return 0;
}

const char *ItlRtl8188E::getFirmwareCountryCode(void)
{
	return fCountryCode[0] ? fCountryCode : "ID";
}

uint32_t ItlRtl8188E::getTxQueueSize(void)
{
	return fTxQueueSize;
}

/* ------------------------------------------------------------------ */
/*  ItlDriverController                                                */
/* ------------------------------------------------------------------ */

void ItlRtl8188E::clearScanningFlags(void)
{
	fScanning = false;
}

IOReturn ItlRtl8188E::setMulticastList(IOEthernetAddress *addr, int count)
{
	(void)addr;
	(void)count;
	return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/*  internal                                                           */
/* ------------------------------------------------------------------ */

void ItlRtl8188E::setMacAddress(const uint8_t *mac)
{
	if (mac)
		memcpy(fChip.macaddr, mac, 6);
}

void ItlRtl8188E::setFirmwareVersion(const char *ver)
{
	if (ver)
		strlcpy(fFirmwareVersion, ver, sizeof(fFirmwareVersion));
}

void ItlRtl8188E::setCountryCode(const char *cc)
{
	if (cc)
		strlcpy(fCountryCode, cc, sizeof(fCountryCode));
}
