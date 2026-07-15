# QCC730 RCP WiFi — Raspberry Pi cfg80211 Driver

Linux kernel module that makes a QCC730 (running the `rcp_app` firmware) appear
as a normal WiFi interface (`wlan1`) on a Raspberry Pi. Both STA (client) and
SoftAP modes are supported. Standard Linux tools (`iw`, `wpa_supplicant`,
`hostapd`, `ip`) drive it through cfg80211; the driver forwards everything to
the QCC730 over QcSPI using the RCP WIFI_MGMT protobuf protocol.

```
iw / wpa_supplicant            (standard Linux WiFi tools)
        │  cfg80211 / nl80211
   rcp_wifi.ko                 (this driver, on the RPi)
        │  WIFI_MGMT protobuf over QcSPI
   QCC730 rcp_app firmware
        │  qapi
   QCC730 WiFi hardware
```

QCC730 is **802.11n, HT20, 1x1 antenna, MCS 0-3** (no VHT/HE) — this is its real
PHY ceiling, advertised truthfully so upper layers negotiate correctly. For
WPA2/WPA3 the credential comes from `wpa_supplicant`: it hands the firmware a
32-byte PMK (WPA2) or the SAE password (WPA3) at connect time, and the firmware
runs the 4-way / SAE handshake internally. Open networks need nothing.

## Quick start

### Hardware

| Signal   | RPi GPIO    | Direction   | QCC730 GPIO |
|----------|-------------|-------------|-------------|
| SPI MOSI | GPIO10 (19) | RPi→QCC730  | GPIO2       |
| SPI MISO | GPIO9  (21) | QCC730→RPi  | GPIO3       |
| SPI SCLK | GPIO11 (23) | RPi→QCC730  | GPIO0       |
| SPI CS   | GPIO8  (24) | RPi→QCC730  | GPIO1       |
| IRQ      | GPIO21 (29) | QCC730→RPi  | GPIO8       |
| RESET    | GPIO20 (38) | RPi→QCC730  | reset (active low) |
| GND      | pin 6       | -           | GND         |

### Software

Build target: kernel 6.12.75 arm64.

**1. Copy the driver source from the workspace to the RPi.** The generated
protobuf files `wifi_mgmt.pb.{c,h}` are **not** checked into git (they are a
build artifact), so they must be copied separately — the module's Kbuild
compiles `rcp_protocol/src/wifi/wifi_mgmt.pb.c` directly. Build `rcp_app` once on
the build host to produce them under `out/rcp_app/`.

```bash
# same LAN, no jump host needed
# 1a. the whole raspi driver dir + the tracked rcp_protocol sources
scp -r $WORKSPACE/qrcp/host/raspi <rpi-user>@<rpi-host>:~/qrcp/host/
scp -r $WORKSPACE/qrcp/rcp_protocol <rpi-user>@<rpi-host>:~/qrcp/

# 1b. the generated (un-tracked) protobuf files — from the rcp_app build output
scp $WORKSPACE/out/rcp_app/wifi_mgmt.pb.c $WORKSPACE/out/rcp_app/wifi_mgmt.pb.h \
  <rpi-user>@<rpi-host>:~/qrcp/rcp_protocol/src/wifi/
```

**2. Build and load (on the RPi):**

```bash
cd ~/qrcp/host/raspi
make
sudo insmod rcp_wifi.ko
sudo ip link set wlan1 up
iw dev wlan1 scan | grep SSID
```

**3. SPI frequency via DTS overlay.** Install the overlay once, then add
`dtoverlay=qcc730-rcp` to `/boot/firmware/config.txt` and reboot the RPi. The
SPI clock is set by `spi-max-frequency` in `raspi.dts.overlay`; the BCM2835 SPI
controller uses `250 MHz / even_divisor`, so the kernel picks the highest
frequency ≤ the configured value.

| DTS value    | Actual freq | Notes                     |
|--------------|-------------|---------------------------|
| `<1000000>`  | 976 kHz     | original (too slow)       |
| `<4000000>`  | 3.906 MHz   | conservative              |
| `<8000000>`  | 7.813 MHz   | ~920 KB/s measured        |
| `<16000000>` | 15.625 MHz  |                           |
| `<25000000>` | 25 MHz      | used by Fermion project   |
| `<32000000>` | 31.25 MHz   | signal-quality dependent  |

Apply a new value (build the overlay on the build host, then reboot the RPi):

```bash
cd $WORKSPACE/qrcp/host/raspi
dtc -@ -I dts -O dtb -o /tmp/qcc730-rcp.dtbo raspi.dts.overlay 2>/dev/null
scp /tmp/qcc730-rcp.dtbo <rpi-user>@<rpi-host>:/tmp/
ssh <rpi-user>@<rpi-host> \
  "sudo cp /tmp/qcc730-rcp.dtbo /boot/firmware/overlays/qcc730-rcp.dtbo && sudo shutdown -r now"
```

### Automatic QCC730 reset

The RESET GPIO (RPi **GPIO20** → QCC730 reset, active low) lets the host
hard-reset the QCC730. On every `insmod`, `spi_master_open()` pulses reset and
waits for the chip to cold-boot before touching the ring. This is the fix for
the [QcSPI ring desync](#troubleshooting) problem: the host can zero the shared
SRAM, but it cannot reset the QCC730's own software counters and s2m semaphore —
only a chip reboot does. Hard-resetting on attach guarantees both sides start
from a clean, in-sync state, even when the host is reloaded while the QCC730
still has in-flight state (e.g. scan results not yet drained).

The reset sequence (in `qcc730_reset()`, modeled on the FreeRTOS host demo's
`qcc730_reset()` / ESP-Hosted's `esp_reset()`): assert reset (300 ms) → release
(300 ms) → wait for cold boot (2000 ms) → poll NOP/WREN/RDSR until the chip
answers. The QCC730 reset is not always reliable on the first pulse, so this is
retried up to 10 times; each boot is polled up to 5× at 100 ms. On success:

```
rcp: qcc730 alive after 1 reset(s), 1 poll(s)
```

If all retries are exhausted (chip absent / unpowered / reset not wired),
`insmod` fails with `-ENODEV` rather than hanging. The `reset-gpios` property is
declared in `raspi.dts.overlay`; the polarity flag `1` is `GPIO_ACTIVE_LOW`
(written as a literal so the overlay builds with a plain `dtc`, no dt-bindings
include needed).

## Link throughput benchmark

Measures end-to-end RCP link throughput without WiFi, enabled at **insmod
time** with `bench_mode=1` (no recompile). `bench_mode=0` (default) is the
normal WiFi path.

The benchmark runs a **stream loopback** through the full RCP protocol stack:
the host sends on an RCP stream and the QCC730 firmware echoes every message
back (built-in loopback control message). So it needs **no QCC730 shell command
and no firmware change** — the firmware's loopback handler is always present.
Because traffic rides the whole stack (seq/ACK/buffers/dispatch), the reported
rate is the real link throughput, not just the raw ring ceiling.

```bash
sudo insmod rcp_wifi.ko bench_mode=1 bench_count=1000 bench_slot_size=1596
dmesg | grep bench_loopback   # reports one-way KB/s and bus KB/s (payload crosses twice)
sudo rmmod rcp_wifi
```

Module params (all `int`, read-only): `bench_mode` (0=WiFi, 1=loopback),
`bench_count` (round-trips, default 1000), `bench_slot_size` (bytes/message,
default 1596; clamped to bus MTU − RCP header), `bench_stream` (stream to loop
back, default 6 = TEST_ONLY), `bench_inflight` (outstanding messages 1..7,
default 4). Results print via `dmesg`.

The output line reports two figures: **one-way KB/s** (application-visible rate
for one direction) and **bus KB/s (x2)** (payload crosses the bus twice per
round-trip, so this is the total on-wire throughput — compare against SPI clock
limits). Reference: at 25 MHz SPI, ≈ 2788 KB/s bus / 1394 KB/s one-way, which
sits right at the raw QcSPI ceiling — i.e. the physical link, not the RCP
stack, is the bottleneck. 25 MHz is the value Fermion uses; 31.25 MHz is
signal-quality dependent and not guaranteed stable.

## STA mode

### iw

Everything below works with **standard `iw`** — no vendor-specific syntax.
Captured on a clean bring-up (STA associated to a 2.4 GHz open AP `Openwrt_2g`,
MAC `c4:cc:37:a0:f1:a9`).

测试环境固定值：STA MAC `c4:cc:37:a0:f1:a9`；开放 AP `Openwrt_2g` BSSID `00:03:7f:95:27:55`（2.4G）。
`<phy>` 不固定，运行时取：`phy$(sudo iw dev wlan1 info | grep wiphy | tr -dc 0-9)`。

**表列说明**：`命令`=可直接下发的完整命令；`实际输出`=真实回显（摘关键行）；`断言 token`=可作 `expect` 的无空格/无单引号子串（命令经 `ssh '<cmd>'` 下发，pattern 内不能含单引号）；`前置`=运行该命令前必须满足的状态。

| 命令 | 实际输出 | 断言 token | 前置 |
|------|----------|-----------|------|
| `sudo iw dev wlan1 scan \| grep SSID` | `SSID: Openwrt_2g`（多行 SSID 列表） | `SSID` | 模块已加载、wlan1 up |
| `sudo iw dev wlan1 scan ssid Openwrt_2g \| grep Openwrt_2g` | `SSID: Openwrt_2g` | `Openwrt_2g` | 目标 AP 在射程内 |
| `sudo iw dev wlan1 scan dump \| grep SSID` | `SSID: Openwrt_2g` … | `SSID` | 先做过一次 scan |
| `sudo iw dev wlan1 connect Openwrt_2g; echo done` | `done`（connect 本身无回显） | `done` | 未关联 |
| `sudo iw dev wlan1 link` | `Connected to 00:03:7f:95:27:55` `SSID: Openwrt_2g` `freq: 2462` | `Connected to` | 已 connect（轮询等待关联完成） |
| `sudo iw dev wlan1 disconnect; echo done` | `done` | `done` | 已关联 |
| `sudo iw dev wlan1 link`（断开后） | `Not connected.` | `Not connected` | 已 disconnect |
| `sudo iw dev wlan1 station dump \| grep -i 00:03:7f:95:27:55` | `Station 00:03:7f:95:27:55 (on wlan1)` | `00:03:7f:95:27:55` | 已关联（表项就绪需轮询） |
| `sudo iw dev wlan1 station dump \| grep -i signal` | `signal:-24 dBm` | `signal` | 已关联 |
| `sudo iw dev wlan1 station get 00:03:7f:95:27:55 \| grep -i 00:03:7f:95:27:55` | `Station 00:03:7f:95:27:55 (on wlan1)` | `00:03:7f:95:27:55` | 已关联 |
| `sudo iw dev wlan1 info \| grep -i addr` | `addr c4:cc:37:a0:f1:a9` | `c4:cc:37:a0:f1:a9` | — |
| `sudo iw dev wlan1 info \| grep -i type` | `type managed` | `managed` | — |
| `sudo iw dev wlan1 info \| grep -i channel` | `channel 11 (2462 MHz), width: 20 MHz` | `channel` | 已关联（channel 仅关联后可见） |
| `sudo iw dev wlan1 info \| grep -i txpower` | `txpower 6.00 dBm` | `txpower` | 已关联 |
| `sudo iw dev wlan1 set txpower fixed 800; echo rc=$?` | `rc=0` | `rc=0` | 已关联 |
| `sudo iw dev wlan1 info \| grep -i txpower`（set 后） | `txpower 8.00 dBm`（800 mBm=8dBm） | `8.00 dBm` | 已执行 set txpower fixed 800 |
| `sudo iw phy phy$(sudo iw dev wlan1 info \| grep wiphy \| tr -dc 0-9) set rts 500; echo rc=$?` | `rc=0`（无回显；QCC730 退化为 RTS on） | `rc=0` | 已关联 |
| `sudo iw phy phy$(sudo iw dev wlan1 info \| grep wiphy \| tr -dc 0-9) set rts off; echo rc=$?` | `rc=0` | `rc=0` | 已关联 |
| `sudo iw reg set CN; echo done` | `done` | `done` | — |
| `sudo iw reg get \| grep -i country` | `country CN: DFS-FCC` | `country CN` | 已 reg set CN |
| `sudo iw phy phy$(sudo iw dev wlan1 info \| grep wiphy \| tr -dc 0-9) info \| grep -i Antennas` | `Available Antennas: TX 0x1 RX 0x1` | `Available Antennas` | — |
| `sudo iw phy phy$(sudo iw dev wlan1 info \| grep wiphy \| tr -dc 0-9) info \| grep -i managed` | ` * managed` | `managed` | — |
| `sudo iw phy phy$(sudo iw dev wlan1 info \| grep wiphy \| tr -dc 0-9) info \| grep -i MCS` | `HT TX/RX MCS rate indexes supported: 0-3` | `MCS` | — |

### wpa_supplicant / wpa_cli

WPA2/WPA3 require **wpa_supplicant 2.11**. Stock Raspberry Pi OS ships an older
build;

**Build 2.11 from source (on the RPi):**

```bash
sudo apt install build-essential pkg-config libnl-3-dev libnl-genl-3-dev libssl-dev
wget https://w1.fi/releases/wpa_supplicant-2.11.tar.gz
tar xzf wpa_supplicant-2.11.tar.gz
cd wpa_supplicant-2.11/wpa_supplicant
# the shipped defconfig pulls D-Bus/MACsec dev packages; use this minimal .config instead
cat > .config <<EOF
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL32=y
CONFIG_CTRL_IFACE=y
CONFIG_BACKEND=file
CONFIG_IEEE80211W=y
CONFIG_SAE=y
CONFIG_SAE_PK=y
CONFIG_TLS=openssl
CONFIG_DEBUG_FILE=y
EOF
make -j4          # binaries: ./wpa_supplicant  ./wpa_cli
```

**Start wpa_supplicant.** Pick the conf for the target security type, then:

```bash
sudo wpa_supplicant -B -i wlan1 -c /etc/wpa_supplicant_wpa2.conf   # WPA2 (or _wpa3 / plain open conf)
wpa_cli -i wlan1 status                                            # expect wpa_state=COMPLETED
```

Open (`/etc/wpa_supplicant.conf`):
```
ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev
update_config=1
country=CN

network={
    ssid="Openwrt_2g"
    key_mgmt=NONE
}
```

WPA2-PSK (`/etc/wpa_supplicant_wpa2.conf`):
```
ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev
update_config=1
country=CN

network={
    ssid="55"
    key_mgmt=WPA-PSK
    psk="12345678"
}
```

WPA3-SAE (`/etc/wpa_supplicant_wpa3.conf`):
```
ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev
update_config=1
country=CN

network={
    ssid="Openwrt_5g"
    key_mgmt=SAE
    sae_password="12345678"
    ieee80211w=2
}
```

**wpa_cli verified output** — 命令前缀省略为 `wpa_cli`，实际路径
`~/wpa_supplicant-2.11/wpa_supplicant/wpa_cli`（测试脚本用 `$HOME/wpa_supplicant-2.11/wpa_supplicant/wpa_cli`）。
**全局前置**：`wpa_supplicant -B -i wlan1 -c <conf>` 已运行；除注明外均需 `wpa_state=COMPLETED`。
连接 conf：开放 `/etc/wpa_supplicant.conf`（Openwrt_2g）、WPA2 `/etc/wpa_supplicant_wpa2.conf`（SSID 55）、
WPA3-SAE `/etc/wpa_supplicant_wpa3.conf`（Openwrt_5g）。

| 命令 | 实际输出 | 断言 token | 前置 |
|------|----------|-----------|------|
| `sudo wpa_supplicant -B -i wlan1 -c /etc/wpa_supplicant_wpa3.conf` | `Successfully initialized wpa_supplicant` | `Successfully initialized` | 未运行 supplicant |
| `wpa_cli -i wlan1 status \| grep wpa_state` | `wpa_state=COMPLETED` | `wpa_state=COMPLETED` | supplicant 启动后轮询 |
| `wpa_cli -i wlan1 status \| grep key_mgmt` | `key_mgmt=SAE`（WPA2 时 `WPA2-PSK`） | `key_mgmt=SAE` | 已 COMPLETED |
| `wpa_cli -i wlan1 status \| grep ssid` | `ssid=Openwrt_5g` | `Openwrt_5g` | 已 COMPLETED |
| `wpa_cli -i wlan1 scan_results \| grep -i Openwrt_5g` | `00:03:7f:08:51:42 5180 -19 [WPA2-SAE-CCMP][ESS] Openwrt_5g` | `Openwrt_5g` | 已关联（表项就绪需轮询） |
| `wpa_cli -i wlan1 signal_poll \| grep RSSI` | `RSSI=-20` | `RSSI` | 已关联 |
| `wpa_cli -i wlan1 list_networks \| grep -i Openwrt_5g` | `0 Openwrt_5g any [CURRENT]` | `Openwrt_5g` | 已关联 |
| `wpa_cli -i wlan1 bss 0 \| grep -i ssid` | `ssid=Openwrt_5g` | `Openwrt_5g` | 已关联 |
| `wpa_cli -i wlan1 pktcnt_poll \| grep TXGOOD` | `TXGOOD=28`（TXBAD 恒 0） | `TXGOOD` | 已关联 |
| `wpa_cli -i wlan1 get_capability key_mgmt` | `NONE IEEE8021X WPA-PSK SAE` | `SAE` | — |
| `wpa_cli -i wlan1 get_capability pairwise` | `CCMP TKIP`（TKIP 内核默认列出，firmware 仅 CCMP） | `CCMP` | — |
| `wpa_cli -i wlan1 get_capability modes` | `AP` | `AP` | — |
| `wpa_cli -i wlan1 get_capability channels \| grep Mode` | `Mode[G] Channels:\n1\n2\n…`（含 `Mode[A]`） | `Mode` | — |
| `wpa_cli -i wlan1 mib \| grep dot11RSNAEnabled` | `dot11RSNAEnabled=TRUE` | `dot11RSNAEnabled=TRUE` | 已关联 |
| `wpa_cli -i wlan1 pmksa` | `FAIL`（offload 架构 firmware 持有 PMKSA，符合预期） | `FAIL` | 已关联 |
| `wpa_cli -i wlan1 disconnect` | `OK` | `OK` | 已关联 |
| `wpa_cli -i wlan1 reconnect` | `OK`（status 回到 COMPLETED） | `OK` | 已 disconnect |

## AP mode (SoftAP)

SoftAP runs via standard `hostapd`. The QCC730 firmware is the AP SME and
authenticator — it runs the 4-way handshake, owns the keys, and builds the RSN
beacon; the host driver advertises full key-management offload
(`WIPHY_FLAG_HAVE_AP_SME` + `NL80211_EXT_FEATURE_4WAY_HANDSHAKE_AP_PSK`) so
`hostapd` hands the 32-byte PMK down at AP-start time. Open and WPA2-PSK are
supported; WPA3-SAE AP is **not** supported yet (`start_ap` returns
`-EOPNOTSUPP` for SAE).

**WPA2 requires hostapd 2.11.** Stock Debian `hostapd v2.10` lacks AP-PSK
4-way-handshake offload — with 2.10 the beacon comes up with no RSN IE and
clients connect with no password (effectively open). A working 2.11 build is on
the testbed at `~/hostapd-2.11/hostapd/hostapd`. Open AP works with stock
hostapd; only WPA2 needs 2.11.

**Build 2.11 from source (on the RPi):** same dependencies as wpa_supplicant
(`build-essential pkg-config libnl-3-dev libnl-genl-3-dev libssl-dev`). hostapd
builds straight from its shipped defconfig (already enables
`CONFIG_DRIVER_NL80211` + `CONFIG_LIBNL32`):

```bash
wget https://w1.fi/releases/hostapd-2.11.tar.gz
tar xzf hostapd-2.11.tar.gz
cd hostapd-2.11/hostapd
cp defconfig .config
make -j4          # binaries: ./hostapd  ./hostapd_cli
```

**Start an Open AP:**

```bash
cat > /tmp/open.conf <<EOF
interface=wlan1
driver=nl80211
ssid=rcp_open
hw_mode=g
channel=6
EOF

sudo insmod ~/qrcp/host/raspi/rcp_wifi.ko
sudo ip link set wlan1 up
sudo hostapd -B /tmp/open.conf            # stock hostapd is fine for open
```

**Start a WPA2-PSK AP:**

```bash
cat > /tmp/wpa2.conf <<EOF
interface=wlan1
driver=nl80211
ssid=rcp_wpa2
hw_mode=g
channel=6
wpa=2
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
wpa_passphrase=test1234
EOF

sudo ip link set wlan1 up
# MUST be hostapd 2.11 (stock 2.10 lacks AP-PSK offload -> beacon has no RSN)
sudo ~/hostapd-2.11/hostapd/hostapd -B /tmp/wpa2.conf
```

**Assign the AP IP and start DHCP (dnsmasq on wlan1):**

```bash
sudo ip addr add 192.168.50.1/24 dev wlan1
sudo dnsmasq --interface=wlan1 --bind-interfaces \
  --dhcp-range=192.168.50.10,192.168.50.100,12h --dhcp-option=3,192.168.50.1
```

> `start_ap.sh [open|wpa2]` on the testbed does steps above end-to-end (stop old
> AP/DHCP, reload module, start hostapd, assign IP, start dnsmasq).

**hostapd_cli verified output** — 命令前缀省略为 `hostapd_cli`，实际路径
`~/hostapd-2.11/hostapd/hostapd_cli`（脚本用 `$HOME/hostapd-2.11/hostapd/hostapd_cli`）。
**全局前置**：SoftAP 已起（`hostapd -B ~/open.conf` 或 `~/wpa2.conf`，ch6，SSID `rcp_open`/`rcp_wpa2`）。
需 STA 的行：一个 QCC730 STA（MAC `c4:cc:37:a0:8e:2a`）已关联。`<sta>` = 该 STA MAC，脚本里
动态取：`$(hostapd_cli -i wlan1 list_sta | grep c4:cc:37 | head -1)`。
`[AUTHORIZED]` 依赖驱动在 AP_STA_CONNECTED 时调 `cfg80211_port_authorized`（offload 授权由 firmware 完成）。

| 命令 | 实际输出 | 断言 token | 前置 |
|------|----------|-----------|------|
| `sudo hostapd -B ~/open.conf` | `wlan1: AP-ENABLED` | `AP-ENABLED` | 模块已加载、wlan1 up、无其它 hostapd |
| `hostapd_cli -i wlan1 ping` | `PONG` | `PONG` | AP 已起（ctrl socket 就绪需轮询） |
| `hostapd_cli -i wlan1 status \| grep state` | `state=ENABLED` | `state=ENABLED` | AP 已起 |
| `hostapd_cli -i wlan1 status \| grep channel` | `channel=6` | `channel=6` | AP 已起 |
| `hostapd_cli -i wlan1 status \| grep ssid` | `ssid[0]=rcp_open` | `rcp_open` | open AP |
| `hostapd_cli -i wlan1 status \| grep num_sta` | `num_sta[0]=0` | `num_sta[0]=0` | 无 STA 关联 |
| `hostapd_cli -i wlan1 get_config \| grep ssid` | `ssid=rcp_open` | `rcp_open` | open AP |
| `hostapd_cli -i wlan1 get_config \| grep key_mgmt` | `key_mgmt=WPA-PSK` | `WPA-PSK` | WPA2 AP（`~/wpa2.conf`） |
| `hostapd_cli -i wlan1 vendor 0x001374 3` | `000000000000`（AP 模式 STA-iface GET_MAC 返回全 0） | `000000000000` | AP 已起 |
| `hostapd_cli -i wlan1 list_sta \| grep c4:cc:37` | `c4:cc:37:a0:8e:2a` | `c4:cc:37` | STA 已关联（轮询等待） |
| `hostapd_cli -i wlan1 all_sta \| grep -i AUTHORIZED` | `flags=[AUTH][ASSOC][AUTHORIZED]` | `AUTHORIZED` | STA 已关联 |
| `hostapd_cli -i wlan1 sta <sta> \| grep -i flags` | `flags=[AUTH][ASSOC][AUTHORIZED]` | `flags` | STA 已关联 |
| `hostapd_cli -i wlan1 poll_sta <sta>` | `OK`（QoS-null 探测） | `OK` | STA 已关联 |
| `hostapd_cli -i wlan1 deauthenticate <sta>` | `OK`（STA 会很快重连，勿断言下线） | `OK` | STA 已关联 |
| `hostapd_cli -i wlan1 disassociate <sta>` | `OK`（STA 会很快重连，勿断言下线） | `OK` | STA 已关联 |

## Vendor commands (chip-private only)

Vendor commands are reserved for QCC730-specific operations that have **no**
standard `iw` equivalent: PHY mode, MAC readback, low-level PHY/MAC tuning knobs
(RTS/response rate, EDCA, BA window, slot time, BMISS, CTS-to-self) and the
firmware unit-test dispatch.

All ride one generic protobuf carrier, `VendorCmd{subcmd_id, payload,
response_payload}` (proto tag 50). The host picks a `subcmd_id`, packs a raw
little-endian struct into `payload`, and the firmware `memcpy`s it into the
matching driver struct; GET commands return the raw struct in `response_payload`.

- OUI is always `0x001374`.
- All multi-byte fields are **little-endian**.
- Max payload / response is **128 bytes** (`RCP_WIFI_VENDOR_CMD_MAX_PAYLOAD`).

`iw` and `wpa_cli` are interchangeable — both issue the same
`NL80211_CMD_VENDOR` netlink command routed to the same handler. Differences:

| | `iw` | `wpa_cli` |
|-|------|-----------|
| vendor_id | `0x001374` | `0x001374` (decimal `5004` → FAIL) |
| subcmd id | hex, `0x` prefix (`0x3`) | plain decimal (`3`, `14`) |
| SET payload | space-separated `0xNN` bytes | one continuous hex string, no `0x`/spaces (`09000000`) |
| GET (no payload) | pass a dummy `0x00` (empty `""` not allowed) | just the subcmd |
| GET output | 4-byte NL header + payload; **strip** `LL 00 01 00` (LL = 4+payload len), rest is LE struct | raw `response_payload` hex only, **no NL header** |
| verbs | `send`=SET, `recv`=GET (hex), `recvbin`=raw | `vendor <id> <cmd> [<hex>]` |

> **SET commands and firmware validation:** whether a SET succeeds depends on the
> firmware's check of the *value* and the *current connection state* — not on the
> tool. A rejected SET surfaces identically as `iw` → `command failed: Input/output
> error (-5)` and `wpa_cli` → `FAIL` (dmesg: `vendor_cmd: rejected by QCC730`). E.g.
> `SET_SLOT_TIME=9` is accepted while `=2` (illegal) is rejected;
> `SET_RTS_RATE`/`RSP_RATE`/`EDCA`/`BMISS`/`CTS_TO_SELF` are accepted only in the
> right band/associated state. `SET_BA_WIN_SIZE` must be issued while **disconnected**.

### Supported commands

`SET` sends the struct via `vendor send`; `GET` reads it back via `vendor recv`.
"needs conn" = the firmware only accepts it while associated to an AP.

| subcmd | Name | Dir | Payload struct (LE) | Notes |
|-------:|------|-----|---------------------|-------|
| `0x1` | SET_PHY_MODE | SET | `phy_mode:u32` | `qapi_WLAN_Phy_Mode_e` (e.g. 3 = 11n) |
| `0x2` | GET_PHY_MODE | GET | `phy_mode:u32` | |
| `0x3` | GET_MAC_ADDRESS | GET | `mac:u8[6]` | device MAC |
| `0x4` | GET_BOOT_REASON | GET | `boot_reason:u32` | platform bitfield; 0 when idle/normal |
| `0x5` | SET_RTS_RATE | SET | `rts_rate:u32` | 2.4G RTS rate **index**: 0=1M, 1=6M, 2=12M; needs conn |
| `0x6` | GET_RTS_RATE | GET | `rts_rate:u32` | returns the rate in **Mbps** (1/6/12), not the index |
| `0x7` | SET_RSP_RATE | SET | `rate_idx:u8` | response-frame rate index (8 = 6 Mbps); needs conn |
| `0x8` | SET_EDCA_PARAM_CFG | SET | `qid:u8, aifsn:u8, cw_min:u16, cw_max:u16, txop_limit:u16` | see EDCA note; needs conn |
| `0x9` | GET_EDCA_PARAM_CFG | GET | same struct; **`qid` is an input selector** (first byte) | fills the rest for that queue |
| `0xa` | SET_BA_WIN_TIMING | SET | `ack_timeout:u16, delay:u16` | ack_timeout µs (<4096), delay in SM cycles; needs conn |
| `0xb` | GET_BA_WIN_TIMING | GET | `ack_timeout:u16, delay:u16` | |
| `0xc` | SET_BA_WIN_SIZE | SET | `tx_size:u16, rx_size:u16` | **must be set while disconnected** (qapi rejects when associated) |
| `0xd` | SET_SLOT_TIME | SET | `slot_time:u32` | µs (9 = short, 20 = long); needs conn |
| `0xe` | GET_SLOT_TIME | GET | `slot_time:u32` | |
| `0xf` | SET_BMISS_THRESHOLD | SET | `threshold:u32` | missed beacons before action; needs conn |
| `0x10` | GET_BMISS_THRESHOLD | GET | `threshold:u32` | |
| `0x11` | SET_CTS_TO_SELF | SET | `enable:u32` | 1 = enable, 0 = disable; needs conn |
| `0x12` | UNIT_TEST | SET | `vdev_id:u8, module_id:u8, num_args:u16, args:u32[16]` | firmware unit-test dispatch; results print on the **QCC730 serial console**, not returned |
| `0x13` | SET_IGNORE_BC_MC_IN_BMPS | SET | `enable:u8` | in BMPS, ignore broadcast/multicast to stay asleep; needs conn + BMPS |
| `0x14` | SET_POWER_OPT_IN_BMPS | SET | `enable:u8` | in BMPS, enable extra power optimization; needs conn + BMPS |
| `0x15` | SET_COMPRESS_QOS_NULL_IN_BMPS | SET | `enable:u8` | in BMPS, send compressed QoS-Null frames; needs conn + BMPS |
| `0x16` | SET_RX_FILTER_IN_BMPS | SET | `enable:u8` | in BMPS, enable RX filtering (firmware-default rule set, cb stays firmware-side); needs conn + BMPS |
| `0x17` | SET_LISTEN_INTERVAL | SET | `listen_interval:u16` | STA listen interval in **beacon intervals** (0..65535); `WIFI_PS_PARAM_LISTEN_INTERVAL` → qapi; needs conn |

> **EDCA value semantics (qapi, not a bug):** `cw_min`/`cw_max` are **exponents** —
> the firmware stores `2^e − 1` (so writing `cw_min=5` reads back `31`). `txop_limit`
> is in **32 µs units** (writing `47` reads back `1504`). `qid`: 0..7 select a queue,
> `0xFF` = all.

### iw vendor — verified output

**全局前置**：STA 已关联（`iw dev wlan1 connect Openwrt_2g` → link COMPLETED）——
GET_MAC/RTS_RATE 及所有 SET 都需已关联。`recv` 打印 4 字节 NL 头 `LL 00 01 00`
（LL=4+payload 长度）后跟 LE 载荷；断言 token 取载荷部分即可。
`send` 成功无回显（用 `; echo rc=$?` → `rc=0` 判定），非法值回 `command failed: Input/output error`。
**改状态的 SET 用完须写回默认值**（硬复位实测默认：SLOT=`0x14`、BMISS=`0x1e`、BA_WIN=`28 00 0f 00`；
EDCA qid0 默认 `00 25 0f 00 ff 03 00 00`）。EDCA：cw_min/cw_max 为指数（存 `2^e-1`），txop 单位 32µs，aifsn 读回恒 `0x25`。

| 命令 | 实际输出 | 断言 token | 前置 |
|------|----------|-----------|------|
| `sudo iw dev wlan1 vendor recv 0x001374 0x3 0x00 \| grep vendor` | `vendor response: 0a 00 01 00 c4 cc 37 a0 f1 a9` | `c4 cc 37 a0 f1 a9` | 已关联（轮询） |
| `sudo iw dev wlan1 vendor recv 0x001374 0x2 0x00 \| grep vendor` | `vendor response: 08 00 01 00 05 00 00 00` | `05 00 00 00` | 已关联 |
| `sudo iw dev wlan1 vendor recv 0x001374 0x4 0x00 \| grep vendor` | `vendor response: 08 00 01 00 00 00 00 00` | `00 00 00 00` | 已关联 |
| `sudo iw dev wlan1 vendor recv 0x001374 0x6 0x00 \| grep vendor` | `vendor response: 08 00 01 00 06 00 00 00`（RTS=6 Mbps） | `06 00 00 00` | 已关联 |
| `sudo iw dev wlan1 vendor recv 0x001374 0x9 0x00 \| grep vendor` | `vendor response: 0c 00 01 00 00 25 0f 00 ff 03 00 00`（EDCA qid0 默认） | `00 25 0f 00 ff 03 00 00` | 已关联 |
| `sudo iw dev wlan1 vendor send 0x001374 0x8 0x00 0x03 0x05 0x00 0x0a 0x00 0x2f 0x00; echo rc=$?` | `rc=0`（SET_EDCA cw_min_exp=5 cw_max_exp=10 txop=47） | `rc=0` | 已关联 |
| `sudo iw dev wlan1 vendor recv 0x001374 0x9 0x00 \| grep vendor` | `vendor response: 0c 00 01 00 00 25 1f 00 ff 03 e0 05`（cw_min=31 txop=1504） | `00 25 1f 00 ff 03 e0 05` | 已 SET_EDCA |
| `sudo iw dev wlan1 vendor send 0x001374 0x8 0x00 0x03 0x04 0x00 0x0a 0x00 0x00 0x00; echo rc=$?` | `rc=0`（恢复 EDCA 默认） | `rc=0` | 恢复步 |
| `sudo iw dev wlan1 vendor send 0x001374 0x1 0x05 0x00 0x00 0x00; echo rc=$?` | `rc=0`（SET_PHY_MODE=11n） | `rc=0` | 已关联 |
| `sudo iw dev wlan1 vendor send 0x001374 0x5 0x01 0x00 0x00 0x00; echo rc=$?` | `rc=0`（SET_RTS_RATE idx1） | `rc=0` | 已关联 |
| `sudo iw dev wlan1 vendor send 0x001374 0x7 0x08; echo rc=$?` | `rc=0`（SET_RSP_RATE=8） | `rc=0` | 已关联 |
| `sudo iw dev wlan1 vendor send 0x001374 0x11 0x01 0x00 0x00 0x00; echo rc=$?` | `rc=0`（SET_CTS_TO_SELF on） | `rc=0` | 已关联 |
| `sudo iw dev wlan1 vendor send 0x001374 0x11 0x00 0x00 0x00 0x00; echo rc=$?` | `rc=0`（CTS off 恢复） | `rc=0` | 已关联 |
| `sudo iw dev wlan1 vendor send 0x001374 0xd 0x09 0x00 0x00 0x00; echo rc=$?` | `rc=0`（SET_SLOT_TIME=9） | `rc=0` | 已关联 |
| `sudo iw dev wlan1 vendor recv 0x001374 0xe 0x00 \| grep vendor` | `vendor response: 08 00 01 00 09 00 00 00` | `09 00 00 00` | 已 SET_SLOT=9 |
| `sudo iw dev wlan1 vendor send 0x001374 0xd 0x14 0x00 0x00 0x00; echo rc=$?` | `rc=0`（恢复 SLOT=20 默认） | `rc=0` | 恢复步 |
| `sudo iw dev wlan1 vendor send 0x001374 0xf 0x0a 0x00 0x00 0x00; echo rc=$?` | `rc=0`（SET_BMISS=10） | `rc=0` | 已关联 |
| `sudo iw dev wlan1 vendor recv 0x001374 0x10 0x00 \| grep vendor` | `vendor response: 08 00 01 00 0a 00 00 00` | `0a 00 00 00` | 已 SET_BMISS=10 |
| `sudo iw dev wlan1 vendor send 0x001374 0xf 0x1e 0x00 0x00 0x00; echo rc=$?` | `rc=0`（恢复 BMISS=30 默认） | `rc=0` | 恢复步 |
| `sudo iw dev wlan1 vendor send 0x001374 0xa 0x32 0x00 0x14 0x00; echo rc=$?` | `rc=0`（SET_BA_WIN_TIMING=50/20） | `rc=0` | 已关联 |
| `sudo iw dev wlan1 vendor recv 0x001374 0xb 0x00 \| grep vendor` | `vendor response: 08 00 01 00 32 00 14 00` | `32 00 14 00` | 已 SET_BA_WIN |
| `sudo iw dev wlan1 vendor send 0x001374 0xa 0x28 0x00 0x0f 0x00; echo rc=$?` | `rc=0`（恢复 BA_WIN=40/15 默认） | `rc=0` | 恢复步 |
| `sudo iw dev wlan1 vendor send 0x001374 0xc 0x40 0x00 0x40 0x00; echo rc=$?` | `rc=0`（SET_BA_WIN_SIZE tx/rx=64） | `rc=0` | **未关联**（此 SET 需断开态） |
| `sudo iw dev wlan1 vendor send 0x001374 0xd 0x02 0x00 0x00 0x00 2>&1` | `command failed: Input/output error (-5)`（SLOT=2 非法拒绝） | `Input/output error` | 已关联 |
| `sudo iw dev wlan1 vendor send 0x001374 0x13 0x01; echo rc=$?` | `rc=0`（IGNORE_BC_MC_IN_BMPS on） | `rc=0` | 已关联 + BMPS 未开 |
| `sudo iw dev wlan1 vendor send 0x001374 0x14 0x01; echo rc=$?` | `rc=0`（POWER_OPT_IN_BMPS on） | `rc=0` | 已关联 + BMPS 未开 |
| `sudo iw dev wlan1 vendor send 0x001374 0x15 0x01; echo rc=$?` | `rc=0`（COMPRESS_QOS_NULL_IN_BMPS on） | `rc=0` | 已关联 + BMPS 未开 |
| `sudo iw dev wlan1 vendor send 0x001374 0x16 0x01; echo rc=$?` | `rc=0`（RX_FILTER_IN_BMPS on） | `rc=0` | 已关联 + BMPS 未开 |
| `sudo iw dev wlan1 vendor send 0x001374 0x17 0x0a 0x00; echo rc=$?` | `rc=0`（SET_LISTEN_INTERVAL=10 beacons，**待实测**） | `rc=0` | 已关联 |

### wpa_cli vendor — verified output

`wpa_cli` 输出为纯 `response_payload` hex（无 NL 头）；GET 直接回 hex，SET 成功回**空**、失败回 `FAIL`
（exit code 恒 0，**不能**用 `rc=$?` 判 SET 成败——SET-only 用 `out=$(...); echo ${out:-OK}` 断言 `OK`，
有 GET 的以 readback 断言）。**全局前置**：supplicant 已运行且 `wpa_state=COMPLETED`。
**关键**：wpa_supplicant 对 OUI `0x001374` 非白名单 subcmd 默认加 `NLA_F_NESTED`，
把裸结构体 payload 当嵌套属性下发会被内核拒（回 `FAIL`，命令到不了 firmware）——
**所有 SET 必须显式追加 `nested=0`**；GET 无 payload 不受影响。改状态的 SET 用完写回默认值。

| 命令 | 实际输出 | 断言 token | 前置 |
|------|----------|-----------|------|
| `wpa_cli -i wlan1 vendor 0x001374 3` | `c4cc37a0f1a9`（GET_MAC） | `c4cc37a0f1a9` | 已关联（轮询） |
| `wpa_cli -i wlan1 vendor 0x001374 4` | `00000000`（GET_BOOT_REASON） | `00000000` | 已关联 |
| `wpa_cli -i wlan1 vendor 0x001374 6` | `06000000`（GET_RTS_RATE=6 Mbps） | `06000000` | 已关联 |
| `wpa_cli -i wlan1 vendor 0x001374 9` | `00250f00ff030000`（GET_EDCA qid0 默认） | `00250f00ff030000` | 已关联 |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 8 000305000a002f00 nested=0); echo ${out:-OK}` | `OK`（SET_EDCA） | `OK` | 已关联、`nested=0` |
| `wpa_cli -i wlan1 vendor 0x001374 9` | `00251f00ff03e005`（cw_min=31 txop=1504） | `00251f00ff03e005` | 已 SET_EDCA |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 8 000304000a00000000 nested=0); echo ${out:-OK}` | `OK`（恢复 EDCA 默认） | `OK` | 恢复步 |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 1 05000000 nested=0); echo ${out:-OK}` | `OK`（SET_PHY_MODE） | `OK` | 已关联、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 5 01000000 nested=0); echo ${out:-OK}` | `OK`（SET_RTS_RATE） | `OK` | 已关联、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 7 08 nested=0); echo ${out:-OK}` | `OK`（SET_RSP_RATE） | `OK` | 已关联、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 17 01000000 nested=0); echo ${out:-OK}` | `OK`（SET_CTS_TO_SELF on） | `OK` | 已关联、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 17 00000000 nested=0); echo ${out:-OK}` | `OK`（CTS off 恢复） | `OK` | 已关联、`nested=0` |
| `wpa_cli -i wlan1 vendor 0x001374 13 09000000 nested=0; wpa_cli -i wlan1 vendor 0x001374 14` | `09000000`（SET_SLOT=9 后回读） | `09000000` | 已关联、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 13 14000000 nested=0); echo ${out:-OK}` | `OK`（恢复 SLOT=20） | `OK` | 恢复步 |
| `wpa_cli -i wlan1 vendor 0x001374 15 0a000000 nested=0; wpa_cli -i wlan1 vendor 0x001374 16` | `0a000000`（SET_BMISS=10 后回读） | `0a000000` | 已关联、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 15 1e000000 nested=0); echo ${out:-OK}` | `OK`（恢复 BMISS=30） | `OK` | 恢复步 |
| `wpa_cli -i wlan1 vendor 0x001374 10 32001400 nested=0; wpa_cli -i wlan1 vendor 0x001374 11` | `32001400`（SET_BA_WIN=50/20 后回读） | `32001400` | 已关联、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 10 28000f00 nested=0); echo ${out:-OK}` | `OK`（恢复 BA_WIN=40/15） | `OK` | 恢复步 |
| `wpa_cli -i wlan1 vendor 0x001374 13 02000000 nested=0` | `FAIL`（SLOT=2 非法，firmware 拒绝） | `FAIL` | 已关联、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 19 01 nested=0); echo ${out:-OK}` | `OK`（IGNORE_BC_MC_IN_BMPS on） | `OK` | 已关联 + BMPS 未开、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 20 01 nested=0); echo ${out:-OK}` | `OK`（POWER_OPT_IN_BMPS on） | `OK` | 已关联 + BMPS 未开、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 21 01 nested=0); echo ${out:-OK}` | `OK`（COMPRESS_QOS_NULL_IN_BMPS on） | `OK` | 已关联 + BMPS 未开、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 22 01 nested=0); echo ${out:-OK}` | `OK`（RX_FILTER_IN_BMPS on） | `OK` | 已关联 + BMPS 未开、`nested=0` |
| `out=$(wpa_cli -i wlan1 vendor 0x001374 23 0a00 nested=0); echo ${out:-OK}` | `OK`（SET_LISTEN_INTERVAL=10 beacons，**待实测**） | `OK` | 已关联、`nested=0` |

## Changing the protobuf protocol

`wifi_mgmt.pb.{c,h}` are **generated** from `rcp_protocol/proto/wifi_mgmt.proto`,
**not** hand-edited and **not** checked into git. The generator (protoc + nanopb)
runs only as part of the `rcp_app` firmware build on the build host — the RPi has
no protoc/nanopb toolchain and its Kbuild compiles the generated `.pb.c`
directly. Generating in one place keeps the host driver and the QCC730 firmware
byte-for-byte identical; if they disagree on the proto, decode fails on-air
(`Invalid RX message` / `Rx Decode failed`). So after editing the `.proto`:

1. Build `rcp_app` on the build host — this regenerates
   `out/rcp_app/wifi_mgmt.pb.{c,h}` and links them into the firmware.
2. Flash the firmware to the QCC730 (both sides must move together).
3. Copy the regenerated files to the RPi and rebuild the module:
   ```bash
   scp $WORKSPACE/out/rcp_app/wifi_mgmt.pb.c $WORKSPACE/out/rcp_app/wifi_mgmt.pb.h \
     <rpi-user>@<rpi-host>:~/qrcp/rcp_protocol/src/wifi/
   ssh <rpi-user>@<rpi-host> "cd ~/qrcp/host/raspi && make"
   ```
4. Reload in sync (see QcSPI ring desync below): `platform reboot` the QCC730,
   then `rmmod`/`insmod rcp_wifi` on the RPi.

## Troubleshooting

**QcSPI ring desync** — the ring state lives on both sides (SRAM indices plus
the QCC730's own software counters/semaphore, which the host cannot touch). If
only one side restarts while the other keeps running — e.g. `rmmod`/`insmod`
the host while the QCC730 still has in-flight scan results it wrote to the s2m
ring — the two sides disagree and you see `Invalid RX message` / `Rx Decode
failed`, with `wlan1` never syncing (MAC stays random instead of the QCC730's
`c4:cc:37:…`).

This is now handled automatically: `insmod` hard-resets the QCC730 via the
RESET GPIO (see [Automatic QCC730 reset](#automatic-qcc730-reset)) before
touching the ring, so the chip always cold-boots into a clean state and the two
sides start in sync. No manual `platform reboot` is needed.

If the RESET GPIO is **not** wired / not in the DTS, `insmod` fails early with
`rcp: no reset GPIO … check DTS reset-gpios`. In that case fall back to the
manual recovery: `platform reboot` the QCC730 (wait for the RCP-ready log), then
`rmmod`/`insmod rcp_wifi`.

**Switch AP back to STA** — stopping hostapd is enough to return the interface
to STA mode (`stop_ap` restores the netdev MAC from the SAP MAC back to the STA
MAC and sets iftype back to `managed`); no module reload needed. Remove the AP
static IP and stop the DHCP server first, or STA DHCP will be disrupted.

```bash
sudo pkill hostapd                   # 1. stop AP (restores STA MAC)
sudo systemctl stop dnsmasq          # 2. stop DHCP (or: sudo pkill dnsmasq)
sudo ip addr flush dev wlan1         #    drop the AP static IP
sudo ip link set wlan1 up            # 3. back to STA
sudo iw dev wlan1 scan | grep SSID
sudo iw dev wlan1 info               # verify: type managed, addr …:f1:a9 (STA MAC)
```

If `dnsmasq` was started via systemd (with `/etc/dnsmasq.d/rcp-wlan1.conf`), a
plain `pkill` may get respawned — use `systemctl stop dnsmasq` or remove that
conf file. If after switching you see `Invalid RX message` / `wlan1` desync,
reset both sides (see QcSPI ring desync above); a normal `pkill hostapd` does
not cause this.

**AP limitations:**
- The host netdev (`wlan1`) uses the QCC730 STA MAC by default and switches to
  the SAP MAC while the AP is up, so downlink frames' source MAC matches the
  over-the-air BSSID (else clients drop the DHCP/ARP replies).
- Client-to-client (intra-BSS) traffic is **NOT** forwarded: the firmware
  uplinks all frames to the host and the host has no bridge/hairpin, so two
  associated clients cannot reach each other (each can reach the AP).
- STA and AP do not run at the same time in this setup (STA+AP concurrency is
  untested).
