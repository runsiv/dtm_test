#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_radio.h>

LOG_MODULE_REGISTER(dtm_test, LOG_LEVEL_DBG);

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

#define DTM_CHANNEL     CONFIG_DTM_CHANNEL
#define DTM_DATA_LENGTH CONFIG_DTM_DATA_LENGTH
#define DTM_PHY         CONFIG_DTM_PHY
#define DTM_PAYLOAD     CONFIG_DTM_PAYLOAD
#define DTM_CTE_LENGTH  CONFIG_DTM_CTE_LENGTH
#define DTM_CTE_TYPE    CONFIG_DTM_CTE_TYPE

/* Button 1 (sw0): start DTM TX test */
#define BUTTON_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);

/* Button 2 (sw1): increase TX power */
#define BUTTON2_NODE DT_ALIAS(sw1)
static const struct gpio_dt_spec button2 = GPIO_DT_SPEC_GET(BUTTON2_NODE, gpios);
static struct gpio_callback button2_cb_data;

/* Button 3 (sw2): decrease TX power */
#define BUTTON3_NODE DT_ALIAS(sw2)
static const struct gpio_dt_spec button3 = GPIO_DT_SPEC_GET(BUTTON3_NODE, gpios);
static struct gpio_callback button3_cb_data;

/* Button 4 (sw3): stop DTM TX */
#define BUTTON4_NODE DT_ALIAS(sw3)
static const struct gpio_dt_spec button4 = GPIO_DT_SPEC_GET(BUTTON4_NODE, gpios);
static struct gpio_callback button4_cb_data;

/* TX power levels supported by Nordic SoftDevice Controller (dBm) */
static const int8_t tx_power_levels[] = { -8, -4, 0, 3, 4, 8 };
#define TX_POWER_LEVELS_COUNT ARRAY_SIZE(tx_power_levels)
static int tx_power_index = 2; /* default: 0 dBm (index 2) */


/** Convert dBm value to nrf_radio_txpower_t enum.
 *  Mirrors the same lookup used in the direct_test_mode sample (dtm.c).
 *  Returns NRF_RADIO_TXPOWER_0DBM as a safe default for unsupported values.
 */
static nrf_radio_txpower_t dbm_to_nrf_radio_txpower(int8_t dbm)
{
	switch (dbm) {
#if defined(RADIO_TXPOWER_TXPOWER_Pos8dBm)
	case  8: return NRF_RADIO_TXPOWER_POS8DBM;
#endif
#if defined(RADIO_TXPOWER_TXPOWER_Pos4dBm)
	case  4: return NRF_RADIO_TXPOWER_POS4DBM;
#endif
#if defined(RADIO_TXPOWER_TXPOWER_Pos3dBm)
	case  3: return NRF_RADIO_TXPOWER_POS3DBM;
#endif
	case  0: return NRF_RADIO_TXPOWER_0DBM;
	case -4: return NRF_RADIO_TXPOWER_NEG4DBM;
	case -8: return NRF_RADIO_TXPOWER_NEG8DBM;
	case -12: return NRF_RADIO_TXPOWER_NEG12DBM;
	case -16: return NRF_RADIO_TXPOWER_NEG16DBM;
	case -20: return NRF_RADIO_TXPOWER_NEG20DBM;
#if defined(RADIO_TXPOWER_TXPOWER_Neg40dBm)
	case -40: return NRF_RADIO_TXPOWER_NEG40DBM;
#endif
	default:
		LOG_WRN("TX power %d dBm not supported, using 0 dBm", dbm);
		return NRF_RADIO_TXPOWER_0DBM;
	}
}


/*
 * *** DTM over HCI helpers ***
 */

/** Start a DTM TX test using LE Transmitter Test v4 (BT Core 5.4, opcode 0x207b).
 *  This version includes a TX power level field, so the SDC controller applies
 *  it directly — no need to write RADIO->TXPOWER separately.
 *  @param channel   RF channel index 0-39 (f = 2402 + 2*channel MHz)
 *  @param phy       BT_HCI_LE_TX_PHY_1M / _2M / _CODED_S8 / _CODED_S2
 *  @param payload   BT_HCI_TEST_PKT_PAYLOAD_PRBS9, etc.
 *  @param data_len  Test payload length in bytes (0-255, typically 37)
 *  @param tx_power  TX power in dBm, or 0x7F for controller default
 * 
 * Valid payloads
 * BT_HCI_TEST_PKT_PAYLOAD_PRBS9 — pseudo-random 9-bit sequence
 * BT_HCI_TEST_PKT_PAYLOAD_11110000 — alternating nibbles (used here)
 * BT_HCI_TEST_PKT_PAYLOAD_10101010 — alternating bits
 * BT_HCI_TEST_PKT_PAYLOAD_PRBS15 — pseudo-random 15-bit
 * BT_HCI_TEST_PKT_PAYLOAD_11111111 — all ones
 * BT_HCI_TEST_PKT_PAYLOAD_00000000 — all zeros
 * BT_HCI_TEST_PKT_PAYLOAD_00001111 — inverse nibbles
 * BT_HCI_TEST_PKT_PAYLOAD_01010101 — inverse alternating bits
 * 
 * phy — uint8_t
 * Physical layer selection:
 * BT_HCI_LE_TX_PHY_1M — 1 Mbps
 * BT_HCI_LE_TX_PHY_2M — 2 Mbps
 * BT_HCI_LE_TX_PHY_CODED_S8 — Coded PHY, 125 kbps (S=8)
 * BT_HCI_LE_TX_PHY_CODED_S2 — Coded PHY, 500 kbps (S=2)
 * 
 * cte_length — uint8_t
 * Constant Tone Extension length. 0 = no CTE (as used here). Range 2–20 if CTE is used.
 * cte_type — uint8_t
 * CTE type. 0 = AoA, 1 = AoD 1 µs, 2 = AoD 2 µs. Irrelevant when cte_length = 0.
 */
static int dtm_tx_start(uint8_t channel, uint8_t phy, uint8_t payload,
			uint8_t data_len, int8_t tx_power)
{
	struct net_buf *buf;

	/* v4 command has a variable tail: [switching_pattern_length] [antenna_ids...] [tx_power_level]
	 * With CTE disabled (cte_length=0), switching_pattern_length must be 0,
	 * so the tail is just the single tx_power_level byte.
	 */
	struct __packed {
		uint8_t tx_channel;
		uint8_t test_data_length;
		uint8_t packet_payload;
		uint8_t phy;
		uint8_t cte_length;
		uint8_t cte_type;
		uint8_t switching_pattern_length; /* 0 = no antenna switching */
		int8_t  tx_power_level;           /* dBm, or 0x7F = no preference */
	} cp;

	cp.tx_channel              = channel;
	cp.test_data_length        = data_len;
	cp.packet_payload          = payload;
	cp.phy                     = phy;
	cp.cte_length              = DTM_CTE_LENGTH;
	cp.cte_type                = DTM_CTE_TYPE;
	cp.switching_pattern_length = 0x00; /* no antenna switching */
	cp.tx_power_level          = tx_power;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		LOG_ERR("DTM TX: failed to allocate HCI cmd buffer");
		return -ENOMEM;
	}
	net_buf_add_mem(buf, &cp, sizeof(cp));

	/* LE Transmitter Test v4 — opcode 0x207b */
	int err = bt_hci_cmd_send_sync(BT_OP(BT_OGF_LE, 0x007b), buf, NULL);
	if (err) {
		LOG_ERR("DTM TX v4: HCI command failed (err %d)", err);
	} else {
		LOG_INF("DTM TX v4: started ch=%u PHY=%u power=%d dBm",
			channel, phy, tx_power);
	}
	return err;
}

/** Start a DTM RX test.
 *  @param channel  BLE channel index 0–39
 *  @param phy      BT_HCI_LE_RX_PHY_1M / _2M / _CODED
 */
static int dtm_rx_start(uint8_t channel, uint8_t phy)
{
	struct bt_hci_cp_le_enh_rx_test *cp;
	struct net_buf *buf;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (!buf) {
		LOG_ERR("DTM RX: failed to allocate HCI cmd buffer");
		return -ENOMEM;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	cp->rx_ch     = channel;
	cp->phy       = phy;
	cp->mod_index = 0x00; /* Standard modulation */

	int err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_ENH_RX_TEST, buf, NULL);
	if (err) {
		LOG_ERR("DTM RX: HCI command failed (err %d)", err);
	} else {
		LOG_INF("DTM RX: started on channel %u, PHY %u", channel, phy);
	}
	return err;
}

// End an active DTM test and report received package count
static int dtm_end(uint16_t *rx_pkt_count)
{
	struct net_buf *rsp = NULL;
	int err;

	err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_TEST_END, NULL, &rsp);
	if (err) {
		LOG_ERR("DTM End: HCI command failed (err %d)", err);
		return err;
	}

	struct bt_hci_rp_le_test_end *rp = (void *)rsp->data;
	uint16_t count = sys_le16_to_cpu(rp->rx_pkt_count);
	LOG_INF("DTM End: status %u, RX packets %u", rp->status, count);

	if (rx_pkt_count) {
		*rx_pkt_count = count;
	}
	net_buf_unref(rsp);
	return rp->status ? -EIO : 0;
}

static struct gpio_callback button_cb_data;

int dtm_status = 0;
// -1 = decrease, 0 = no change, +1 = increase 
static atomic_t tx_power_change = ATOMIC_INIT(0);
/* Set to 1 by button ISR to request a toggle of the current DTM test */
static atomic_t dtm_toggle = ATOMIC_INIT(0);

// Toggle DTM TX start or stop
void button_pressed_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	atomic_set(&dtm_toggle, 1);
}
// increase tx power
void button2_pressed_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	atomic_set(&tx_power_change, 1);
}
// decrease tx power
void button3_pressed_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	atomic_set(&tx_power_change, -1);
}

// start DTM RX test or stop RX test
void button4_pressed_callback(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	atomic_set(&dtm_toggle, 4);
}

// TX power checker
static void print_tx_power(void)
{
	uint32_t raw = NRF_RADIO->TXPOWER;

	LOG_INF("RADIO->TXPOWER raw = 0x%02x (target: %d dBm)",
		raw, tx_power_levels[tx_power_index]);
}

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0xaa, 0xfe),
	BT_DATA_BYTES(BT_DATA_SVC_DATA16,
		      0xaa, 0xfe, /* Eddystone UUID */
		      0x10, /* Eddystone-URL frame type */
		      0x00, /* Calibrated Tx power at 0m */
		      0x00, /* URL Scheme Prefix http://www. */
		      'z', 'e', 'p', 'h', 'y', 'r',
		      'p', 'r', 'o', 'j', 'e', 'c', 't',
		      0x08) /* .org */
};


static void start_beacon(int err)
{
	char addr_s[BT_ADDR_LE_STR_LEN];
	bt_addr_le_t addr = {0};
	size_t count = 1;

	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return;
	}

	LOG_INF("Bluetooth initialized");

	err = bt_le_adv_start(BT_LE_ADV_NCONN_IDENTITY, ad, ARRAY_SIZE(ad),
			      sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}

	bt_id_get(&addr, &count);
	bt_addr_le_to_str(&addr, addr_s, sizeof(addr_s));

	LOG_INF("Beacon started, advertising as %s", addr_s);
}


int button_setup(void)
{
	int ret;

	// Button 1 ,start/stop DTM TX
	if (!device_is_ready(button.port)) {
		LOG_ERR("Button 1 device is not ready");
		return -1;
	}
	ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("Failed to configure button 1 pin (err %d)", ret);
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		LOG_ERR("Failed to configure button 1 interrupt (err %d)", ret);
		return ret;
	}
	gpio_init_callback(&button_cb_data, button_pressed_callback, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	// increase tx level
	if (!device_is_ready(button2.port)) {
		LOG_ERR("Button 2 device is not ready");
		return -1;
	}
	ret = gpio_pin_configure_dt(&button2, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("Failed to configure button 2 pin (err %d)", ret);
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&button2, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		LOG_ERR("Failed to configure button 2 interrupt (err %d)", ret);
		return ret;
	}
	gpio_init_callback(&button2_cb_data, button2_pressed_callback, BIT(button2.pin));
	gpio_add_callback(button2.port, &button2_cb_data);

	// decrease tx power
	if (!device_is_ready(button3.port)) {
		LOG_ERR("Button 3 device is not ready");
		return -1;
	}
	ret = gpio_pin_configure_dt(&button3, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("Failed to configure button 3 pin (err %d)", ret);
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&button3, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		LOG_ERR("Failed to configure button 3 interrupt (err %d)", ret);
		return ret;
	}
	gpio_init_callback(&button3_cb_data, button3_pressed_callback, BIT(button3.pin));
	gpio_add_callback(button3.port, &button3_cb_data);

	// Button 4, start/stop DTM RX
	if (!device_is_ready(button4.port)) {
		LOG_ERR("Button 4 device is not ready");
		return -1;
	}
	ret = gpio_pin_configure_dt(&button4, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("Failed to configure button 4 pin (err %d)", ret);
		return ret;
	}
	ret = gpio_pin_interrupt_configure_dt(&button4, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		LOG_ERR("Failed to configure button 4 interrupt (err %d)", ret);
		return ret;
	}
	gpio_init_callback(&button4_cb_data, button4_pressed_callback, BIT(button4.pin));
	gpio_add_callback(button4.port, &button4_cb_data);

	return 0;
}

int main(void)
{
	int err;
	err = button_setup();
	if (err) {
		LOG_ERR("Failed to set up buttons (err %d)", err);
		return err;
	}

	LOG_INF("Starting radio");
	LOG_INF("Button 1: start DTM TX | Button 2: TX power UP | Button 3: TX power DOWN | Button 4: stop DTM TX");

	bt_enable(start_beacon);

	while (1) {

		// Handle button 1 / button 4 toggle requests
		atomic_val_t toggle = atomic_set(&dtm_toggle, 0);
		if (toggle != 0) {
			if (dtm_status == 1 || dtm_status == 4) {
				// A test is running — stop it
				LOG_INF("Stopping DTM test (status=%d)", dtm_status);
				dtm_end(NULL);
				dtm_status = 0;
				start_beacon(0);
			} else if (toggle == 1 && dtm_status == 0) {
				// Button 1: request TX start/stop
				dtm_status = 2;
			} else if (toggle == 4 && dtm_status == 0) {
				// Button 4: request RX start/stop
				dtm_status = 3;
			}
		}

		// Handle TX power change requests from button 2/3 ISRs
		atomic_val_t change = atomic_set(&tx_power_change, 0);
		if (change != 0) {
			if (change == 1) {
				if (tx_power_index < TX_POWER_LEVELS_COUNT - 1) {
					tx_power_index++;
				}
				LOG_INF("Button 2: TX power UP -> %d dBm", tx_power_levels[tx_power_index]);
			} else {
				if (tx_power_index > 0) {
					tx_power_index--;
				}
				LOG_INF("Button 3: TX power DOWN -> %d dBm", tx_power_levels[tx_power_index]);
			}

			if (dtm_status == 1) {
				// DTM TX is running - stop it, restart with new power level
				LOG_INF("Stopping DTM TX to apply new TX power");
				dtm_end(NULL);
				LOG_INF("Restarting DTM TX: ch=%d, PHY=%d, payload=0x%02x, %d bytes",
					DTM_CHANNEL, DTM_PHY, DTM_PAYLOAD, DTM_DATA_LENGTH);
				err = dtm_tx_start(DTM_CHANNEL, DTM_PHY,
						   DTM_PAYLOAD, DTM_DATA_LENGTH,
						   tx_power_levels[tx_power_index]);
				if (err) {
					LOG_ERR("Failed to restart DTM TX (err %d)", err);
					dtm_status = 0;
					start_beacon(0);
				}
			}

			print_tx_power();
		}

		/* --- State transitions --- */
		if (dtm_status == 2) {
			// Button 1 start/stop DTM TX
			bt_le_adv_stop();
			LOG_INF("Starting DTM TX: ch=%d, PHY=%d, payload=0x%02x, %d bytes",
				DTM_CHANNEL, DTM_PHY, DTM_PAYLOAD, DTM_DATA_LENGTH);
			err = dtm_tx_start(DTM_CHANNEL, DTM_PHY,
					   DTM_PAYLOAD, DTM_DATA_LENGTH,
					   tx_power_levels[tx_power_index]);
			if (err) {
				LOG_ERR("Failed to start DTM TX (err %d)", err);
				dtm_status = 0;
				start_beacon(0);
			} else {
				dtm_status = 1;
				print_tx_power();
			}
		}

		if (dtm_status == 3) {
			// Button 4 start/stop DTM RX
			bt_le_adv_stop();
			LOG_INF("Starting DTM RX: ch=%d, PHY=%d", DTM_CHANNEL, DTM_PHY);
			err = dtm_rx_start(DTM_CHANNEL, DTM_PHY);
			if (err) {
				LOG_ERR("Failed to start DTM RX (err %d)", err);
				dtm_status = 0;
				start_beacon(0);
			} else {
				dtm_status = 4;
			}
		}

		k_sleep(K_SECONDS(1));
	}

    return 0;
}
