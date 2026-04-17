# DTM Test — nRF54LM20 Direct Test Mode Application

A Zephyr/NCS application for running Bluetooth Low Energy **Direct Test Mode (DTM)** TX and RX tests on the **nRF54LM20 DK**, with interactive TX power control via the DK buttons.

Built with **nRF Connect SDK v3.2.99**.

---

## Overview

This application uses the Nordic SoftDevice Controller (SDC) HCI interface to start and stop BLE DTM tests. When no test is running, the device advertises as an Eddystone-URL beacon so it remains visible to BLE scanners.

### Key features

- **DTM TX** using *LE Transmitter Test v4* (Bluetooth Core 5.4, HCI opcode `0x207b`) — includes a TX power level parameter so the SDC sets the output power directly
- **DTM RX** using the standard *LE Enhanced Receiver Test* HCI command
- **Live TX power adjustment** during an active TX test — the test is restarted automatically with the new power level
- **Toggle start/stop** on a single button press — no need to stop before starting
- Zephyr structured logging (`LOG_INF` / `LOG_ERR`) over UART

---

## Hardware

| Target board | `nrf54lm20dk/nrf54lm20a/cpuapp` |
|---|---|
| SoC | nRF54LM20A (Cortex-M33, single core) |
| Interface | nRF Connect SDK + SoftDevice Controller |

---

## Button mapping

| Button | Function |
|--------|----------|
| **Button 1** (SW0) | **Toggle DTM TX** — press once to start, press again to stop |
| **Button 2** (SW1) | **TX power UP** — step to next higher power level |
| **Button 3** (SW2) | **TX power DOWN** — step to next lower power level |
| **Button 4** (SW3) | **Toggle DTM RX** — press once to start, press again to stop |

> Buttons 2 and 3 are ISR-safe: power changes are queued via an atomic flag and applied in the main thread. If a TX test is running when the power is changed, the test is briefly stopped and restarted at the new level automatically.

---

## TX power levels

The following output power levels are available (stepped through with Buttons 2/3):

| Index | Power |
|-------|-------|
| 0 | −8 dBm |
| 1 | −4 dBm |
| **2** | **0 dBm** ← default |
| 3 | +3 dBm |
| 4 | +4 dBm |
| 5 | +8 dBm |

---

## Application state machine

| `dtm_status` | Meaning |
|---|---|
| `0` | Idle — Eddystone beacon advertising |
| `1` | DTM TX running |
| `2` | DTM TX start requested (resolved within one main loop tick) |
| `3` | DTM RX start requested (resolved within one main loop tick) |
| `4` | DTM RX running |

---

## Configuration (Kconfig)

All test parameters are set in `prj.conf`. Comments are supported on their own line (prefix `#`) but **not** inline after a value.

### Channel and packet length

| Symbol | Default | Range | Description |
|--------|---------|-------|-------------|
| `CONFIG_DTM_CHANNEL` | `22` | 0–39 | BLE RF channel index. Frequency = 2402 + 2 × channel MHz. Channel 22 = 2446 MHz. |
| `CONFIG_DTM_DATA_LENGTH` | `37` | 0–255 | DTM test packet payload length in bytes. |

### Physical layer — `CONFIG_DTM_PHY`

| Value | Constant | PHY |
|-------|----------|-----|
| **1** | `BT_HCI_LE_TX_PHY_1M` | 1 Mbps ← default |
| 2 | `BT_HCI_LE_TX_PHY_2M` | 2 Mbps |
| 3 | `BT_HCI_LE_TX_PHY_CODED_S8` | Coded S=8, 125 kbps (long range) |
| 4 | `BT_HCI_LE_TX_PHY_CODED_S2` | Coded S=2, 500 kbps |

### Payload pattern — `CONFIG_DTM_PAYLOAD`

| Value | Constant | Pattern |
|-------|----------|---------|
| 0 | `BT_HCI_TEST_PKT_PAYLOAD_PRBS9` | Pseudo-random 9-bit sequence |
| **1** | `BT_HCI_TEST_PKT_PAYLOAD_11110000` | Alternating nibbles 0xF0 ← default |
| 2 | `BT_HCI_TEST_PKT_PAYLOAD_10101010` | Alternating bits 0xAA |
| 3 | `BT_HCI_TEST_PKT_PAYLOAD_PRBS15` | Pseudo-random 15-bit sequence |
| 4 | `BT_HCI_TEST_PKT_PAYLOAD_11111111` | All ones 0xFF |
| 5 | `BT_HCI_TEST_PKT_PAYLOAD_00000000` | All zeros 0x00 |
| 6 | `BT_HCI_TEST_PKT_PAYLOAD_00001111` | Inverse nibbles 0x0F |
| 7 | `BT_HCI_TEST_PKT_PAYLOAD_01010101` | Inverse alternating bits 0x55 |

### Constant Tone Extension (CTE)

| Symbol | Default | Range | Description |
|--------|---------|-------|-------------|
| `CONFIG_DTM_CTE_LENGTH` | `0` | 0–20 | CTE duration in 8 µs units. `0` disables CTE entirely. Valid range when enabled: 2–20. |
| `CONFIG_DTM_CTE_TYPE` | `0` | 0–2 | CTE type. Ignored when `DTM_CTE_LENGTH = 0`. `0` = AoA, `1` = AoD 1 µs slot, `2` = AoD 2 µs slot. |

### Example `prj.conf` snippet

```
# 1=1M  2=2M  3=CodedS8  4=CodedS2
CONFIG_DTM_PHY=1

# 0=PRBS9  1=0xF0  2=0xAA  3=PRBS15  4=0xFF  5=0x00  6=0x0F  7=0x55
CONFIG_DTM_PAYLOAD=1

# 0=disabled, 2-20=enabled
CONFIG_DTM_CTE_LENGTH=0

# 0=AoA  1=AoD1us  2=AoD2us
CONFIG_DTM_CTE_TYPE=0
```

> **Note:** `prj.conf` does not support inline comments (e.g. `CONFIG_DTM_PHY=1 # comment` is invalid). Place comments on a separate line above the assignment.
