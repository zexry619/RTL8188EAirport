# RTL8188EAirport

[![Build Kext](https://github.com/zexry619/RTL8188EAirport/actions/workflows/build.yml/badge.svg)](https://github.com/zexry619/RTL8188EAirport/actions/workflows/build.yml)
[![macOS Sequoia](https://img.shields.io/badge/macOS-15.x%20Sequoia-blue.svg)](https://apple.com)
[![Kext Version](https://img.shields.io/badge/version-0.9.1-success.svg)](https://github.com/zexry619/RTL8188EAirport)
[![Architecture](https://img.shields.io/badge/arch-x86__64-orange.svg)](#)
[![License](https://img.shields.io/badge/license-GPL--2.0%20%2F%20ISC-green.svg)](#license)

**RTL8188EAirport** is a native macOS Apple 802.11 (`IO80211Family`) kernel extension for **Realtek RTL8188EU / RTL8188EUS** USB Wi-Fi dongles (such as the **TP-Link TL-WN725N v2/v3**).

Unlike legacy vendor drivers (`RtWlanU.kext` / Wireless Network Utility) that require third-party status bar apps and fail on modern macOS releases, **RTL8188EAirport** directly attaches to the native macOS network stack. Wi-Fi networks can be scanned, joined, and managed seamlessly via the native **macOS Wi-Fi Menu Bar, Control Center, and Network Settings**.

---

## ✨ Features

- **Native macOS Wi-Fi Integration:** Works natively with Apple's `airportd`, `CoreWLAN`, and Control Center. No external status bar app or utility required.
- **Hardware A-MPDU TX/RX Aggregation:**
  - Full hardware TX A-MPDU aggregation (`R88E_TXDW2_AGG_EN`) supporting up to 10 subframes per burst (~15 KB bursts) tailored to the 20 KB TX packet FIFO.
  - Full RX A-MPDU reordering support with unblocked 20-byte `BlockAckReq` (`BAR`) control frame handling, eliminating multi-second TCP window stalls under load.
- **Full 802.11n Support (HT20 & HT40):**
  - MCS 0–7 data rates with Short Guard Interval (SGI).
  - Up to **72.2 Mbps** link speed on 20 MHz channels.
  - Up to **150 Mbps** link speed on 40 MHz channels (`HT40+` and `HT40-` dynamic carrier frequency tuning).
- **Fast Roaming & Clean Association:**
  - Robust WPA2-PSK 4-way handshake completed in under **30 ms**.
  - Seamless SSID switching and reconnects without needing to toggle Wi-Fi off and on.
- **WMM QoS & EDCA Turbo Acceleration:**
  - Access Categories (Voice, Video, Best Effort, Background) with Realtek EDCA Turbo parameters for optimal upload bursts.
- **Ultra-Low Latency & High-Speed Data Path:**
  - Zero-page-allocation kernel VM recycling for USB TX descriptors.
  - Local gateway ping response of **~1.3–2.0 ms**.
  - Fast boot-time initialization (< 0.7s) to prevent `IOKitWaitQuiet` boot delays.

---

## 📊 Benchmark Results

Verified on **TP-Link TL-WN725N** (1x1 2.4 GHz 802.11n, RTL8188EUS) on **macOS 15.8.1 Sequoia**:

| Test Metric | Performance | Remarks |
| :--- | :--- | :--- |
| **Sequential Download (`networkQuality -s`)** | **37.12 Mbps** *(peaked at **37.64 Mbps**)* | Standard Speedtest.net download |
| **Sequential Upload (`networkQuality -s`)** | **39.24 Mbps** *(peaked at **44.43 Mbps**)* | Enabled by Hardware TX A-MPDU |
| **Simultaneous Bi-Directional (DL + UL)** | **52.39 Mbps** *(peaked at **58.86 Mbps**)* | **~81.5%** of theoretical 72.2 Mbps HT20 airtime capacity |
| **Local Gateway Latency (`ping 192.168.1.1`)** | **1.37 ms min / 2.04 ms avg** | Zero packet loss |
| **WPA2 4-Way Handshake Time** | **29 ms** | Instant connection |

---

## 🔌 Supported Hardware

Any USB adapter powered by the Realtek **RTL8188EU** or **RTL8188EUS** chipset:

| Vendor | Model | USB ID |
| :--- | :--- | :--- |
| **TP-Link** | TL-WN725N (v2 / v3) | `2357:010c` |
| **Realtek** | RTL8188ETV / RTL8188EUS Reference | `0bda:8179` |
| **D-Link** | DWA-123 / DWA-125 (rev D1) | `2001:3310`, `2001:330f` |

*(Additional USB IDs can be added easily in `src/airport/Info.plist`)*.

---

## 💻 Compatibility

- **macOS Sequoia** (15.x)
- **macOS Sonoma** (14.x)
- **macOS Ventura** (13.x)
- **Bootloader:** OpenCore / Clover, or installed directly into the Auxiliary Kernel Collection (AuxKC).

---

## 🚀 Installation

### Prerequisites
1. **Disable / Remove Old Realtek Drivers:**  
   If you previously installed the vendor package or Wireless Network Utility, remove `RtWlanU.kext` or `RtWlanU1827.kext` from `/Library/Extensions/` or OpenCore `config.plist`. Having both loaded simultaneously will cause device access conflicts.

### Method A: OpenCore (Recommended for Hackintosh)
1. Download or compile `RTL8188EAirport.kext`.
2. Copy `RTL8188EAirport.kext` to `EFI/OC/Kexts/`.
3. Add the entry to your `EFI/OC/config.plist` under `Kernel` -> `Add`:
   ```xml
   <dict>
       <key>Arch</key>
       <string>x86_64</string>
       <key>BundlePath</key>
       <string>RTL8188EAirport.kext</string>
       <key>Comment</key>
       <string>Native RTL8188EU Wi-Fi</string>
       <key>Enabled</key>
       <true/>
       <key>ExecutablePath</key>
       <string>Contents/MacOS/RTL8188EAirport</string>
       <key>MaxKernel</key>
       <string></string>
       <key>MinKernel</key>
       <string>22.0.0</string>
       <key>PlistPath</key>
       <string>Contents/Info.plist</string>
   </dict>
   ```
4. Save and reboot.

---

## 🛠️ Building from Source

### Requirements
- macOS with Xcode Command Line Tools (`xcode-select --install`).
- `clang` with C++17 support.

### Build Steps
```bash
# Clone the repository with submodules
git clone --recursive https://github.com/zexry619/RTL8188EAirport.git
cd RTL8188EAirport

# Fetch dependencies (MacKernelSDK, itlwm upstream)
./scripts/fetch-upstream.sh

# Build the kext
./scripts/build-m4.sh
```

The compiled `RTL8188EAirport.kext` will be generated in `src/driver/build-m4/RTL8188EAirport.kext`.

---

## 🏗️ Architecture

```text
       CoreWLAN / Airport Menu Bar / Control Center
                           │
                           │  IO80211 Private KPI
                           ▼
┌───────────────────────────────────────────────────────────┐
│ src/airport/  — IO80211 Airport Frontend                  │
│   Bridge between Apple IO80211 and the driver core.       │
├───────────────────────────────────────────────────────────┤
│ upstream/itlwm/ — OpenBSD net80211 Port                   │
│   802.11 State Machine: Auth, Assoc, WPA2, Rate Control.  │
├───────────────────────────────────────────────────────────┤
│ src/urtwn/    — Portable Realtek RTL8188E Hardware HAL    │
│   RF6052 & Baseband programming, EFUSE parser, A-MPDU.    │
├───────────────────────────────────────────────────────────┤
│ src/usb/      — Low-Latency USB Transport Shim            │
│   IOUSBHostPipe mapping with zero-page-allocation pool.   │
└───────────────────────────────────────────────────────────┘
```

---

## 📄 License

- Hardware HAL & Portions from OpenBSD `urtwn`: **ISC License**.
- Frontend & Submodule integration adapted from `itlwm`: **GPL-2.0 License**.
- MacKernelSDK: **Apple Public Source License (APSL)**.

---

## 🙏 Acknowledgements

- **OpenBSD Project** for the clean and robust `urtwn` and `net80211` implementations.
- **zxystd / OpenIntelWireless team (`itlwm`)** for the pioneering IO80211 shim architecture on modern macOS.
- **Acidanthera** for `MacKernelSDK` and the OpenCore ecosystem.
