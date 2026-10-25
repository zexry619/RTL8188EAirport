/*
 * ItlRtl8188E.hpp — HAL service untuk chip RTL8188E USB.
 *
 * Menghubungkan net80211 (port itlwm/OpenBSD) dengan chip RTL8188EUS:
 *   - init chip: power-on → DMA/LLT → firmware → EFUSE → MAC/BB/RF
 *   - ic_newstate: scan (ganti kanal + dwell) dan transisi state
 *   - TX: mgmt queue + data queue → descriptor USB 32 byte → bulk OUT
 *   - RX: bulk IN → ieee80211_inputm()
 *
 * Lisensi: GPL-2.0-or-later
 */

#ifndef RTL8188EU_ITL_RTL8188E_HPP
#define RTL8188EU_ITL_RTL8188E_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IONetworkInterface.h>
#include <IOKit/network/IONetworkController.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/usb/IOUSBHostInterface.h>
#include <IOKit/usb/IOUSBHostDevice.h>
#include <IOKit/IOTimerEventSource.h>

#include "HAL/ItlHalService.hpp"
#include "HAL/ItlDriverInfo.hpp"
#include "HAL/ItlDriverController.hpp"

#include "rtwn_m1.h"

class ItlRtl8188E : public ItlHalService, public ItlDriverInfo,
    public ItlDriverController {
public:
	OSDeclareDefaultStructors(ItlRtl8188E)

public:
	bool init() override;
	void free() override;

	/* Dipanggil wrapper sebelum attach(); menyimpan interface USB. */
	bool prepare(IOUSBHostInterface *interface);

	/* --- ItlHalService --- */
	bool attach(IOPCIDevice *device) override;
	void detach(IOPCIDevice *device) override;
	IOReturn enable(IONetworkInterface *interface) override;
	IOReturn disable(IONetworkInterface *interface) override;
	struct ieee80211com *get80211Controller() override;
	void updateMACAddress(const uint8_t *mac) override;
	ItlDriverInfo *getDriverInfo() override { return this; }
	ItlDriverController *getDriverController() override { return this; }

	/* --- ItlDriverInfo --- */
	const char *getFirmwareVersion() override;
	int16_t getBSSNoise() override;
	bool is5GBandSupport() override;
	int getTxNSS() override;
	const char *getFirmwareName() override;
	UInt32 supportedFeatures() override;
	const char *getFirmwareCountryCode() override;
	uint32_t getTxQueueSize() override;

	/* --- ItlDriverController --- */
	void clearScanningFlags() override;
	IOReturn setMulticastList(IOEthernetAddress *addr, int count) override;

	/* --- internal --- */
	void setMacAddress(const uint8_t *mac);
	void setFirmwareVersion(const char *ver);
	void setCountryCode(const char *cc);

	rtwn_m1_t   fChip;
	IOUSBHostInterface *fInterface;
	IOUSBHostDevice    *fDevice;

	bool         fAttached;
	bool         fEnabled;
	bool         fScanning;
	bool         fScanSeen;
	bool         fChipReady;
	bool         fBulkReady;
	bool         fIfAttached;
	bool         fSawRx;
	uint32_t     fTxQueueSize;
	uint8_t      fMac[6];
	char         fFirmwareVersion[32];
	char         fCountryCode[8];

	/*
	 * Penghitung diagnostik. Dipublikasikan sebagai properti ioreg
	 * (RTL8188E-stat) supaya satu reboot langsung memberi jawaban:
	 * frame data benar-benar dikirim? EAPOL masuk/keluar? enkripsi gagal?
	 */
	uint32_t     fTxMgmt;
	uint32_t     fTxData;
	uint32_t     fTxEapol;
	uint32_t     fTxFail;
	uint32_t     fRxMgmt;
	uint32_t     fRxData;
	uint32_t     fRxEapol;
	uint32_t     fEncryptFail;
	uint32_t     fTxReport;		/* laporan TX diterima */
	uint32_t     fTxOk;		/* frame di-ACK AP */
	uint32_t     fTxRetry;		/* frame yang butuh retry */
	/*
	 * Per-subtype management: satu-satunya cara membedakan
	 * "auth tidak pernah dikirim" dari "auth dikirim tapi AP
	 * tidak membalas" — dua masalah dengan perbaikan berbeda.
	 */
	uint32_t     fTxAuthReq;	/* auth request dikirim */
	uint32_t     fTxAssocReq;	/* assoc request dikirim */
	uint32_t     fRxAuth;		/* auth response diterima */
	uint32_t     fRxAssocResp;	/* assoc response diterima */
	/*
	 * Percobaan join ke-N. Setiap siklus join memakai varian RSN
	 * berbeda (lihat applyRsnVariant) supaya satu boot bisa
	 * menemukan kombinasi yang diterima AP tanpa reboot berulang.
	 */
	uint32_t     fJoinTry;
	uint8_t      fLastJoinBssid[6];
	uint8_t      fRsnVariant;
	/*
	 * RX: berapa frame yang benar-benar lolos ke network stack
	 * (ml dari ieee80211_inputm). Selisih dengan fRxData menunjukkan
	 * net80211 membuang frame — mis. dekripsi gagal atau node salah.
	 */
	uint32_t     fRxDelivered;
	uint32_t     fRxDataLogged;
	uint32_t     fRxBar;		/* BlockAckReq diterima */
	uint32_t     fTxAgg;		/* frame QoS dikirim dengan AGG_EN */
	bool         fBw40;		/* radio aktif di 40 MHz (HT40) */
	bool         fPriChLo;		/* primary channel di bawah secondary */
	uint8_t      fTxScratch[4096];	/* scratch TX descriptor + MPDU (workloop-serialized) */
	void         applyRsnVariant(uint32_t attempt);
	/*
	 * Setara rtwn_updateslot() + INIRTS_RATE_SEL di OpenBSD.
	 * Slot time menentukan DIFS/backoff; kalau tidak cocok dengan AP,
	 * frame bisa dikirim pada jendela yang salah dan bertabrakan.
	 */
	void         updateSlot(struct ieee80211com *ic);
	static void  updateSlotCb(struct ieee80211com *ic);
	void publishStats(void);

	/* net80211 */
	struct ieee80211com fIc;
	CTimeout           *fScanTo;
	int (*fOrigNewstate)(struct ieee80211com *, enum ieee80211_state, int);

	/*
	 * RX diserialisasi ke workloop IO80211 lewat timer 0 ms.
	 * Callback USB hanya menyalin buffer (tidak boleh memblokir
	 * workloop USB — bisa deadlock dengan control transfer).
	 */
	static const uint32_t kRxQueueDepth = 512;
	IOTimerEventSource *fRxTimer;
	IOTimerEventSource *fTxTimer;
	IOLock             *fRxLock;
	uint8_t            *fRxBuf[kRxQueueDepth];
	uint32_t            fRxLen[kRxQueueDepth];
	uint32_t            fRxHead;
	uint32_t            fRxCount;
	volatile bool       fRxRunning;

	/* ------------------------------------------------------------------ */
	/* Callback statis (dipanggil net80211 / USB)                          */
	/* ------------------------------------------------------------------ */
	static int  halRead(void *ctx, uint16_t addr, void *buf, uint16_t len);
	static int  halWrite(void *ctx, uint16_t addr, const void *buf, uint16_t len);
	static void rxCb(void *ctx, const uint8_t *data, uint32_t len);
	static void txStartCb(struct _ifnet *ifp);
	static int  newstateCb(struct ieee80211com *ic,
	    enum ieee80211_state nstate, int arg);
	static void scanTimeoutCb(void *arg);
	static void watchdogCb(struct _ifnet *ifp);
	static int  setKeyCb(struct ieee80211com *ic,
	    struct ieee80211_node *ni, struct ieee80211_key *k);
	static void deleteKeyCb(struct ieee80211com *ic,
	    struct ieee80211_node *ni, struct ieee80211_key *k);
	static void txDoneHook(void *ctx);
	void rxTimerAction(IOTimerEventSource *timer);
	void txTimerAction(IOTimerEventSource *timer);
	void rxQueueFree(void);

	/* Hardware CAM Security Engine (registers 0x670/0x674/0x678) */
	void camWrite(uint32_t addr, uint32_t data);
	uint32_t camRead(uint32_t addr);
	int  setCamKey(int entry, uint8_t key_id, uint8_t algo,
	    const uint8_t *macaddr, const uint8_t *key, int keylen);
	void deleteCamKey(int entry);

	void handleRx(const uint8_t *buf, uint32_t len);
	void txStart(struct _ifnet *ifp);
	void sendFrame(mbuf_t m, struct ieee80211_node *ni, bool isMgmt);
	void setChannel(struct ieee80211_channel *chan);
	void setDiag(const char *key, const char *value);
	int  initChip(void);
	void stopChip(void);
};

#endif /* RTL8188EU_ITL_RTL8188E_HPP */
