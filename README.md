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

| Symbol | Default | Description |
|--------|---------|-------------|
| `CONFIG_DTM_CHANNEL` | `22` | BLE RF channel index (0–39). Frequency = 2402 + 2 × channel MHz. Channel 22 = 2446 MHz. |
| `CONFIG_DTM_DATA_LENGTH` | `37` | DTM test packet payload length in bytes (0–255). |

Override on the command line without editing files:

```sh
west build -- -DCONFIG_DTM_CHANNEL=10 -DCONFIG_DTM_DATA_LENGTH=255
```

Or edit `prj.conf`:

```
CONFIG_DTM_CHANNEL=22
CONFIG_DTM_DATA_LENGTH=37
```

---

## Building and flashing

```sh
# Build (pristine)
west build -p always -b nrf54lm20dk/nrf54lm20a/cpuapp /Users/rusi/Support/dtm_test

# Flash
west flash --recover
```

---

## Log output (UART)

With `CONFIG_LOG=y` and `CONFIG_LOG_BACKEND_UART=y` the application prints structured log messages. Example output:

```
[00:00:00.312 INF] dtm_test: Bluetooth initialized
[00:00:00.315 INF] dtm_test: Beacon started, advertising as XX:XX:XX:XX:XX:XX (random)
[00:00:05.001 INF] dtm_test: Starting DTM TX: ch=22, 1M PHY, 0xF0 pattern, 37 bytes
[00:00:05.018 INF] dtm_test: DTM TX v4: started ch=22 PHY=1 power=0 dBm
[00:00:05.019 INF] dtm_test: RADIO->TXPOWER raw = 0x00 (target: 0 dBm)
[00:00:07.001 INF] dtm_test: Button 2: TX power UP -> 3 dBm
[00:00:07.002 INF] dtm_test: Stopping DTM TX to apply new TX power
[00:00:07.019 INF] dtm_test: Restarting DTM TX: ch=22, 1M PHY, 0xF0 pattern, 37 bytes
[00:00:07.035 INF] dtm_test: DTM TX v4: started ch=22 PHY=1 power=3 dBm
```

---

## Notes

- **TX power via v4 HCI command**: Earlier versions used `BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL` (VS HCI) which only affects BLE advertising/connections and has no effect on DTM. The current implementation uses *LE Transmitter Test v4* which carries the TX power level as part of the command itself, so the SDC applies it when starting the test.
- **Soft reset preserves debug state**: On the nRF54LM20, `sys_reboot()` (`SYSRESETREQ`) does not reset the debug components. Hardware breakpoints (FPB) and watchpoints (DWT) survive the reset, which is useful when debugging boot sequences.
