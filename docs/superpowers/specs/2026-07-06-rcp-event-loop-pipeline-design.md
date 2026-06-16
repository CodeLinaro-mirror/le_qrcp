# WiFi 数据面异步发送（rd/dp 并行）设计

日期：2026-07-06
状态：设计已定，分两阶段实现（先快速验证，后视情况通用化）

## 1. 背景与问题

QCC730 通过 QcSPI 连树莓派，树莓派跑 cfg80211 fullmac 驱动（host），QCC730 跑
rcp_app firmware（slave）。UDP TX 数据路径：

```
RPi net stack → proxy handle_data_msg → link_send(WIFI_DATA)
  → QcSPI m2s ring → QCC730 → run_event_loop 消费 → wifi driver → 空口
```

实测吞吐对比：
- QCC730 standalone UDP TX：18.5 Mbps
- 裸 QcSPI + ring bench（不走协议栈）：~21 Mbps
- **RCP 模式：只有 16.5 Mbps** ← 反常，协议栈在上面反而更慢

### 已定位的根因（周期性统计 log 实测）

在 `run_event_loop_once()` 两段各加 µs 级计时，UDP TX 打流稳态数据：

```
evloop/s: spins=1250 tx=0~2 rx=1250 rxempty=0 txbusy=0
  per-frame rd=330 dp=443 us
```

- `spins == rx`：每圈 loop 正好处理一帧，loop 没空转、没假唤醒、没等总线。
- `tx≈0`：RCP ACK 几乎不发（ack_window=7 攒着），**ACK 不是瓶颈**。
- 每帧 `rd(330µs) + dp(443µs) = 773µs` → 1250 帧/s → ~16 Mbps，与实测吻合。

**结论：瓶颈是单线程把 rd 和 dp 串行执行。** 两段性质不同：
- `rd`（`get_rx_data`）：经 `rcp_bus_read` 从 ring 读整帧 —— 占总线，不占 wifi 引擎。
- `dp`（`dispatch_msg` → `handle_data_msg`）：真正大头在 **wifi driver 发包**（RCP decode
  很轻）—— 占 CPU/wifi 引擎，**完全不碰总线**。

串行时 dp 的 443µs 里总线空闲、rd 的 330µs 里 wifi 引擎空闲。二者本可并行。

## 2. 分层分析（关键：确定拆分该在哪一层）

三层职责，`run_event_loop` 是**纯 RCP 协议层**，已与总线/wifi 解耦：

| 层 | 代码 | 职责 |
|---|------|------|
| Bus 层 | `rcp_bus_spi_slave_qcc730.c` | SPI/ring 收发 —— 具体总线 |
| RCP 协议层 | `rcp_interface_impl.c` (`run_event_loop`) + `rcp_mux_dispatcher.c` | 经 `rcp_bus` 抽象读、decode、seq/ACK、按 stream 派发到注册的 handler —— **纯协议，不知道底层是 SPI、也不知道 handler 里在发 wifi** |
| 应用层 | `proxy_wifi_data.c` (`handle_data_msg`) | 收到 payload 后发 wifi —— 具体应用 |

`dispatch_data_msg()` 只是调 `data_dispatch_handlers[stream]` 注册的回调；WIFI_DATA 的回调
是 `handle_data_msg`。dp 的 443µs 大头在这个回调内部的 wifi 发包，**不在 RCP decode**。

**因此流水线不应在 RCP 层做**（那会用 SPI/wifi 的名字侵入协议层、并把 handler 返回值/ACK
时序复杂化）。正确的落点是**应用层：让 wifi handler 自己异步发送**。

## 3. 设计：WiFi handler 内部异步发送

RCP 层**零改动**。改动完全收敛在 `proxy_wifi_data.c`：`handle_data_msg` 收到 payload 后，
把帧拷进自带缓冲并投递到自己的发送队列，**立即返回**，让 RCP `run_event_loop` 马上回去
读下一帧。一个独立的 wifi TX 线程从队列取帧、调 wifi driver 发包。

```
RCP run_event_loop (调用线程, 不变):
    rcp_bus_read + decode + seq/ACK  → dispatch → handle_data_msg
                                                     │ 拷 payload 到自带 buffer,
                                                     │ 入队, 立即 return RCP_RX_NO_ACTION
                                                     ▼
    wifi TX 线程 (新增, 在 proxy_wifi_data.c 内):
        队列取帧 → net_context_sendto / wifi driver 发包 → 释放自带 buffer
```

节拍从 `rd+dp = 773µs` 降到 `max(rd, dp) = 443µs`：
- 理论帧率 1250/s → ~2260/s，吞吐 ~16 Mbps → ~28 Mbps。

### 为什么 Y（应用层异步）而非 X（RCP 层通用异步）

- **RCP 零改动、零耦合**：协议层不需要知道 wifi、不需要管额外线程/队列。
- **ACK 时序不受影响**：WIFI_DATA 的 RCP ACK 由 `manage_data_state` 按 seq/ack_window 在
  handler 返回**之前**决定，与 handler 干什么、多快返回无关。
- **返回值语义不变**：`handle_data_msg` 现在所有路径都返回 `RCP_RX_NO_ACTION`（从不用返回值
  让 RCP 回 ACK/response）。异步化后照样返回 NO_ACTION，RCP 行为完全一致。
- 代价：只有 wifi 受益（但 wifi 正是瓶颈），且需一次 payload 拷贝（见下）。

### 关键决策

| # | 决策 | 理由 |
|---|------|------|
| 落点 Y | 流水线在 wifi 应用层（`proxy_wifi_data.c`）做，RCP 层不动 | 保持分层：RCP 是纯协议，不该耦合 bus/wifi |
| 拷贝 A | handler 内把 payload 拷进 wifi 自带 buffer/net_pkt 再入队 | RCP 一返回就 `release_buffer` 回收 RCP buffer；只入队指针会 use-after-free。拷贝发生在 RCP 线程、~1400B，远小于省下的 443µs |
| 背压 | wifi 自带 buffer 池/队列满 → handler 内等待或丢帧，自成一体 | 不触碰 RCP buffer 生命周期；池满自然回压到 RCP → m2s ring → RPi |
| 返回值 | 异步后仍 `return RCP_RX_NO_ACTION` | 与现状一致，ACK 时序不变 |

### buffer 生命周期（拷贝 A 的核心）

- RCP 传进 handler 的 `payload_ptr` 是 RCP buffer，handler 返回后 `dispatch_data_msg` 立即
  `release_buffer` 回收。
- 所以 handler **必须在返回前把数据拷出**到 wifi 自己掌控的存储（net_pkt 或自管池），入队的是
  这份副本。RCP buffer 生命周期完全不变。
- wifi TX 线程发完后释放这份副本。副本的分配/释放/队列都在 wifi 侧，与 RCP buffer 池独立。

## 4. 实现策略：先快速验证，后视情况通用化

**用户决定：先不考虑通用性/其他平台，用最快方式验证异步是否真的把吞吐拉高，再决定后续。**

### 阶段 1 —— 快速验证（先做）

目标：用最小改动在**当前 730 + RPi 测试环境**证明 rd/dp 并行把吞吐从 16.5M 拉高。

- **只动 `proxy_wifi_data.c` 一个文件**（RCP 层、bus 层、RPi host 都不碰）。
- 用 Zephyr 原生原语：`k_thread`（wifi TX 线程）+ `k_fifo`（发送队列），栈/优先级先硬编码。
- payload 副本用 `net_pkt`（handler 里 `net_pkt_alloc_with_buffer` + write，wifi 线程发完 unref），
  或一个自管定长 buffer 池——实现时选开销小的一种。
- 队列深度/背压：副本池限深，池满时 handler 内短暂等待（回压到 RCP → m2s → RPi），不丢已收帧。
- 保留 730 侧 rd_us/dp_us + evloop/s 统计用于验证。
- 现有 `handle_data_msg` 的同步发送路径可用编译开关保留，方便对照回退。

**验证判据**（同样 UDP TX 打流）：
- `evloop/s` 的 `rx` 帧率从 ~1250 升向 ~2000+；
- 吞吐 16.5M → 目标 20M+（越接近 max(rd,dp) 推算的 ~28M 越好）；
- 帧率趋近 `1/max(rd,dp)` 而非 `1/(rd+dp)`，证明两段真并行；
- 无丢帧异常、无 ring desync、RCP ACK 行为不变。

若阶段 1 有效 → 视情况进阶段 2。若收益远低于预期 → dp 内部大头可能不在“可并行的 wifi 发送”
而在别处，需在 handler 内再插一层计时（RCP→net_context 封装 vs 真实 driver 发包，呼应实验 B）。

### 阶段 2 —— 固化（验证有效后按需）

- 栈/优先级宏化（`RCP_*` config + 平台覆盖）。
- 若其他跑 wifi 数据面的平台（如 740 slave 的等价 proxy）也需要，同样在各自应用层的
  wifi handler 内套用同一异步模式——**仍不改 RCP 层**。
- 注意：本方案是应用层模式，天然不涉及 rcp_protocol 公共 API，因此不影响 STM32 host、demo
  等其他 `run_event_loop` 使用者。

## 5. 风险 / 注意点

- **buffer 副本纪律**：handler 必须在返回前拷完数据；入队后 RCP buffer 归 RCP 回收，wifi 副本
  归 wifi 线程释放，两者独立，不可交叉引用。
- 拷贝开销（~1400B/帧，在 RCP 线程）远小于并行省下的 443µs，但要确认不把 rd 段本身拖慢。
- 副本池/队列满时的等待会回压到 RCP，进而 m2s ring 满 → RPi 重试（与当前背压路径一致），
  需确认等待不过久导致 RCP 线程饿死 ACK。
- wifi TX 线程优先级：应低于/等于 RCP 线程，避免抢占导致 RCP 读帧变慢；实测调。
- dp 内部（RCP→net_context 封装 vs 真实 wifi driver 发包）本次不拆，留作后续。

## 6. 现有代码锚点

- `qrcp/rcp_zephyr/samples/rcp_app/src/proxy_wifi_data.c` — **唯一改动文件**。`handle_data_msg`
  （WIFI_DATA handler，注册于 `:322`，所有路径 `return RCP_RX_NO_ACTION`）；已有 `k_fifo`
  RX 方向模式（`rx_fifo` + `wifi_rx_thread`）可直接参考套用到 TX 方向
- `qrcp/rcp_protocol/src/rcp_mux_dispatcher.c:160` — `dispatch_data_msg`：调 handler 后
  `release_buffer` 回收 RCP buffer（拷贝 A 的原因）
- `qrcp/rcp_protocol/src/rcp_interface_impl.c` — `run_event_loop` / `get_rx_data`（rd 段）；
  已有 evloop_stats 计时（`RCP_EVLOOP_STATS` / rd_us / dp_us），**本方案不改此文件**
- `qrcp/rcp_protocol/src/buffer_mgmt.c` — RCP buffer 池（`buffer_mtx` 已线程安全）
