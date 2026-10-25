#!/bin/bash
# build-m4.sh — build kext M4: frontend AirportItlwm (ABI Sequoia) + HAL 8188E.
#
# Komponen:
#   - net80211 port itlwm (OpenBSD) + crypto
#   - frontend AirportItlwm V2 (Skywalk, macOS 15.2+)
#   - HAL ItlRtl8188E + init USB 8188E (kode M1/M2 yang sama dengan probe)
#
# Hasil: src/driver/build-m4/RTL8188EAirport.kext
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/src"
UP="$ROOT/upstream/itlwm"
BUILD="$SRC/driver/build-m4"
KEXT_NAME="RTL8188EAirport"
KEXT="$BUILD/$KEXT_NAME.kext"
MKS="$ROOT/vendor/MacKernelSDK"

if [[ ! -d "$MKS/Headers" || ! -d "$UP/itl80211" ]]; then
	echo "[m4] Dependencies not found. Fetching upstream dependencies..."
	"$ROOT/scripts/fetch-upstream.sh"
fi

echo "[m4] generate firmware array"
"$ROOT/scripts/gen-firmware-array.sh"

rm -rf "$BUILD"
mkdir -p "$KEXT/Contents/MacOS"
cp "$SRC/airport/Info.plist" "$KEXT/Contents/Info.plist"

GIT_REV="${GITHUB_SHA:-$(git rev-parse --short HEAD 2>/dev/null || echo local)}"
GIT_REV="${GIT_REV:0:7}"

COMMON=(
	-arch x86_64
	-mkernel
	-fno-common
	-O2
	-Wall
	-Wno-deprecated-declarations
	-Wno-sign-conversion
	-Wno-implicit-int-conversion
	-Wno-unused-function
	-Wno-unused-variable
	-Wno-macro-redefined
	-DKERNEL=1
	-DKERNEL_PRIVATE=1
	-DAIRPORT_WCL
	-D__IO80211_TARGET=__MAC_15_2
	-DIO80211FAMILY_V2
	-D__PRIVATE_SPI__
	-DAIRPORT
	-DIEEE80211_STA_ONLY
	-DITLWM_VERSION='"0.9.1"'
	-DGIT_COMMIT="\"$GIT_REV\""
	-I"$MKS/Headers"
	-I"$SRC/urtwn"
	-I"$SRC/usb"
	-I"$SRC/driver"
	-I"$SRC/airport"
	-I"$UP/include"
	-I"$UP/include/Airport"
	-I"$UP/include/HAL"
	-I"$UP/AirportItlwm"
	-I"$UP/AirportItlwm/WCL"
	-I"$UP/itlwm"
	-I"$UP/itlwm/hal_iwm"
	-I"$UP/itlwm/hal_iwx"
	-I"$UP/itlwm/hal_iwn"
	-I"$UP/itl80211"
	-I"$UP/itl80211/linux"
	-I"$UP/itl80211/openbsd"
	-I"$UP/itl80211/openbsd/net80211"
)

CXXFLAGS=(-std=gnu++14 -fno-rtti -fno-exceptions -nostdinc++ "${COMMON[@]}")
CFLAGS=(-std=gnu11 "${COMMON[@]}")

NET80211=(
	ieee80211.c ieee80211_amrr.c ieee80211_crypto.c
	ieee80211_crypto_bip.c ieee80211_crypto_ccmp.c
	ieee80211_crypto_tkip.c ieee80211_crypto_wep.c
	ieee80211_input.c ieee80211_ioctl.c ieee80211_mira.c
	ieee80211_node.c ieee80211_output.c ieee80211_pae_input.c
	ieee80211_pae_output.c ieee80211_proto.c ieee80211_ra.c
	ieee80211_regdomain.c ieee80211_rssadapt.c
	_string.c timeout.c CTimeout.cpp
	_ctimeout_placeholder
)

CRYPTO=(
	aes.c hmac.c sha2.c rijndael.c ecb3_enc.c set_key.c cast.c
	michael.c sha1.c cmac.c ecb_enc.c chachapoly.c md5.c
	arc4.c blf.c poly1305.c key_wrap.c gmac.c rmd160.c idgen.c
	sha1-pbkdf2.c
)

CRYPTO_EXTRA=(
	"$UP/itl80211/openbsd/sys/arp.c"
)

SUPPORT=(
	"$UP/itl80211/compat.cpp"
	"$UP/itl80211/zutil.c"
	"$UP/itl80211/openbsd/sys/_mbuf.cpp"
	"$UP/itl80211/openbsd/sys/_task.cpp"
	"$UP/itl80211/openbsd/sys/_ifq.cpp"
	"$UP/include/HAL/ItlHalService.cpp"
)

FRONTEND=(
	"$SRC/airport/AirportItlwm.cpp"
	"$SRC/airport/AirportItlwmSkywalkInterface.cpp"
	"$SRC/airport/AirportItlwmEthernetInterface.cpp"
	"$SRC/airport/WCL.cpp"
	"$SRC/airport/IOPCIEDeviceWrapper.cpp"
)

DRIVER=(
	"$SRC/driver/ItlRtl8188E.cpp"
	"$SRC/driver/kmod_info.c"
	"$SRC/urtwn/rtwn_m1.cpp"
	"$SRC/urtwn/rtwn_seq.c"
	"$SRC/urtwn/rtwn_seq_efuse.c"
	"$SRC/urtwn/rtwn_flow.c"
	"$SRC/urtwn/rtwn_efuse_8188e.c"
	"$SRC/urtwn/rtwn_usb.c"
	"$SRC/urtwn/rtwn_80211.c"
	"$SRC/urtwn/rtwn_chan.c"
	"$SRC/urtwn/rtwn_init_8188e.c"
	"$SRC/urtwn/rtwn_platform_kernel.c"
	"$SRC/urtwn/firmware_rtl8188euufw.cpp"
	"$SRC/usb/usb_compat.cpp"
)

# firmware file name typo guard
DRIVER=("${DRIVER[@]/firmware_rtl8188euufw.cpp/firmware_rtl8188eufw.cpp}")

OBJECTS=()
compile_cxx() {
	local src="$1"
	local obj="$BUILD/$(basename "${src%.*}").o"
	if [[ -f "$obj" ]]; then
		echo "[m4] DUPLIKAT objek: $obj ($src)" >&2
		exit 1
	fi
	echo "[m4] c++ $(basename "$src")"
	clang++ "${CXXFLAGS[@]}" -c "$src" -o "$obj"
	OBJECTS+=("$obj")
}
compile_c() {
	local src="$1"
	local obj="$BUILD/$(basename "${src%.*}").o"
	if [[ -f "$obj" ]]; then
		echo "[m4] DUPLIKAT objek: $obj ($src)" >&2
		exit 1
	fi
	echo "[m4] cc  $(basename "$src")"
	clang "${CFLAGS[@]}" -c "$src" -o "$obj"
	OBJECTS+=("$obj")
}

# net80211: semua .c dikompilasi sebagai C++ (Xcode: GCC_INPUT_FILETYPE)
for f in "${NET80211[@]}"; do
	[[ "$f" == *_placeholder ]] && continue
	src="$UP/itl80211/openbsd/net80211/$f"
	[[ -f "$src" ]] || { echo "[m4] hilang: $src" >&2; exit 1; }
	case "$f" in
		*.cpp) compile_cxx "$src" ;;
		*)     compile_cxx "$src" ;;
	esac
done

for f in "${CRYPTO[@]}"; do
	src="$UP/itl80211/openbsd/crypto/$f"
	[[ -f "$src" ]] || { echo "[m4] hilang: $src" >&2; exit 1; }
	compile_cxx "$src"
done

for src in "${CRYPTO_EXTRA[@]}"; do
	compile_cxx "$src"
done

for src in "${SUPPORT[@]}"; do
	case "$src" in
		*.cpp) compile_cxx "$src" ;;
		*)     compile_cxx "$src" ;;
	esac
done

for src in "${FRONTEND[@]}"; do compile_cxx "$src"; done

for src in "${DRIVER[@]}"; do
	case "$src" in
		*.c) compile_c "$src" ;;
		*)   compile_cxx "$src" ;;
	esac
done

echo "[m4] link $KEXT"
ld -kext -arch x86_64 -undefined dynamic_lookup \
   -L "$MKS/Library/x86_64" -lkmod \
   -o "$KEXT/Contents/MacOS/$KEXT_NAME" "${OBJECTS[@]}"

echo "[m4] exported symbols"
nm -g "${OBJECTS[@]}" 2>/dev/null \
	| awk '{ if (NF == 3 && $2 == "T" && $3 ~ /^_/) print $3 }' \
	| sort -u > "$BUILD/$KEXT_NAME.exp"
ld -kext -arch x86_64 -undefined dynamic_lookup \
   -exported_symbols_list "$BUILD/$KEXT_NAME.exp" \
   -L "$MKS/Library/x86_64" -lkmod \
   -o "$KEXT/Contents/MacOS/$KEXT_NAME" "${OBJECTS[@]}"

echo "[m4] validate"
plutil -lint "$KEXT/Contents/Info.plist" >/dev/null
file "$KEXT/Contents/MacOS/$KEXT_NAME" | grep -q 'Mach-O 64-bit kext bundle x86_64'

echo "[m4] sign (ad-hoc)"
codesign -f -s - "$KEXT" >/dev/null 2>&1
codesign --verify --deep --strict "$KEXT"
chown -R root:wheel "$KEXT" 2>/dev/null || true

echo "[m4] OK: $KEXT"
echo "[m4] undefined: $(nm -u "$KEXT/Contents/MacOS/$KEXT_NAME" | wc -l | tr -d ' ')"
