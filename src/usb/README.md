# `src/usb/` — Layer 1: shim transport USB

Bagian **yang benar-benar kita tulis sendiri**. Mengganti API USB OpenBSD
(`usbd_*`) dengan API USB modern macOS (`IOUSBHost*`).

## Isi yang harus dibuat

| File | Isi |
|---|---|
| `usb_compat.h/.cpp` | typedef `struct usb_interface` → `IOUSBHostInterface *`, dan fungsi `usbd_*` → IOKit |
| `bsd_compat.h/.cpp` | `timeout_*`, `mtx_*`, `rw_*`, `malloc/free` BSD, `m_*` mbuf, `bcopy` → padanan kernel |

## Peta API (terverifikasi terhadap MacKernelSDK)

| OpenBSD | IOKit modern | Catatan |
|---|---|---|
| `usbd_get_interface_descriptor` | `IOUSBHostInterface::getInterfaceDescriptor()` | |
| `usbd_interface2endpoint_descriptor` | `StandardUSB::getNextEndpointDescriptor()` | |
| `usbd_open_pipe` / `usbd_close_pipe` | `IOUSBHostDevice::createPipe()` / `IOUSBHostPipe::close()` | pipe dibuat dari deskriptor endpoint + interface |
| `usbd_pipe` | `IOUSBHostInterface::copyPipe(addr)` | **harus di-release** |
| `usbd_alloc_xfer` + `usbd_setup_xfer` + `usbd_xfer` | `IOUSBHostPipe::io(IOMemoryDescriptor*, len, IOUSBHostCompletion*, timeout)` | tidak ada objek transfer terpisah anymore |
| `usbd_alloc_buffer` | `IOBufferMemoryDescriptor` (per pipe/transfer) | |
| `usbd_free_xfer` | `release()` buffer + completion | |
| `usbd_do_request` (control) | `IOUSBHostPipe::controlRequest(...)` atau `IOUSBHostDevice::deviceRequest(...)` | |
| `usbd_transfer` | `IOUSBHostPipe::controlRequest(...)` sinkron | |
| `usbd_status` / `usbd_get_xfer_status` | `IOUSBHostCompletion` → callback `(owner, param, status, bytesTransferred)` | |
| `usbd_abort_pipe` | `IOUSBHostPipe::abort(options, error)` | |
| `usbd_clear_endpoint_stall_async` | `IOUSBHostPipe::clearStall(true)` | |
| `usbd_is_dying` | cek `terminate` pada provider | |
| `usbd_ref_incr/decr/wait` | `IORetain` / `IORelease` | |
| `usbd_device` / `usbd_interface` | chain `getProvider()` | |

## Pola dari AirPort-RTW

`upstream/AirPort-RTW/src/kext/RTW88USBDevice.cpp` (GPL-2.0) sudah melakukan hal
yang sama untuk rtw88 USB (8821CU/8822BU): shim `compat_usb_bulk_out()` menjembatani
`IOUSBHostPipe` → API Linux. **Pola inilah yang kita tiru**, hanya dengan target
chip 8188EUS.

## Catatan hierarki kelas USB di IORegistry

Di macOS 15, node USB terlihat sebagai:

```
802.11n NIC@14800000  <class AppleUSBDevice>      ← subclass dari IOUSBHostDevice
  └─ interface          <class AppleUSBInterface>  ← subclass dari IOUSBHostInterface
```

Artinya:
* personality dengan `IOProviderClass = IOUSBHostDevice` tetap cocok
  (IOKit mencocokkan lewat rantai kelas);
* untuk driver sungguhan (M1+) sebaiknya match **`IOUSBHostInterface`** dengan
  `bInterfaceNumber = 0`, karena di sanalah endpoint bulk IN/OUT berada;
* shim wajib menangani kemungkinan node memakai kelas legacy (`AppleUSBDevice`)
  maupun modern (`IOUSBHostDevice`) — jangan asumsikan satu-duanya.

## Catatan performa

`IOUSBHostPipe::io()` bersifat asinkron dengan completion callback.
Untuk throughput tinggi, antrean beberapa `IOMemoryDescriptor` (queue depth 8–16)
supaya pipe tidak pernah kosong — inilah yang membedakan "connected tapi 5 Mbps"
dari "150 Mbps" pada adapter USB 2.0.
