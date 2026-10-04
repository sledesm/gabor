/*
 * The setup GATT service, as the Muse app expects it from the SDK's gadgets:
 * service 7fdd3d1c-..., RX (write) and TX (read/notify) characteristics, and
 * an advertisement with the service UUID, manufacturer data 0xFFFF + a paired
 * flag, and the name MuseGadgetXXXXXX in the scan response.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include "ble_framing.h"
#include "board.h"
#include "setup.h"
#include "setup_transport.h"
#include "store.h"

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

#define SVC_UUID_VAL BT_UUID_128_ENCODE(0x7fdd3d1c, 0x38ea, 0x46cf, 0x8b46, 0x314ecf5f240c)
static struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(SVC_UUID_VAL);
static struct bt_uuid_128 rx_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x4d593029, 0x28a2, 0x4a6e, 0xa1f0, 0x3c2d5e8f9b01));
static struct bt_uuid_128 tx_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xd75dc4ca, 0x7b2b, 0x4e9c, 0x8f0a, 0x1d2e3f4a5b6c));

static struct bt_conn *s_conn;
static bool s_advertising_enabled;
static struct k_work s_adv_work;
static struct k_work_delayable s_disconnect_work;
static struct k_work_delayable s_stop_work;
static K_MUTEX_DEFINE(s_conn_lock);

static ssize_t on_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	setup_on_write(buf, len);
	return len;
}

static ssize_t on_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
		       uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, "", 0);
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	LOG_INF("client %s notifications", value ? "subscribed to" : "unsubscribed from");
}

BT_GATT_SERVICE_DEFINE(setup_svc,
	BT_GATT_PRIMARY_SERVICE(&svc_uuid),
	BT_GATT_CHARACTERISTIC(&rx_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, on_write, NULL),
	BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, on_read, NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static int ble_send(const uint8_t *pkt, size_t len)
{
	k_mutex_lock(&s_conn_lock, K_FOREVER);
	struct bt_conn *conn = s_conn ? bt_conn_ref(s_conn) : NULL;
	k_mutex_unlock(&s_conn_lock);
	if (!conn) {
		return -ENOTCONN;
	}
	int rc;
	for (int tries = 0; tries < 100; tries++) {
		rc = bt_gatt_notify(conn, &setup_svc.attrs[4], pkt, len);
		if (rc != -ENOMEM && rc != -ENOBUFS) {
			break;
		}
		k_sleep(K_MSEC(10));
	}
	bt_conn_unref(conn);
	return rc;
}

static uint16_t ble_mtu(void)
{
	k_mutex_lock(&s_conn_lock, K_FOREVER);
	uint16_t mtu = s_conn ? bt_gatt_get_mtu(s_conn) : FRAMING_DEFAULT_MTU;
	k_mutex_unlock(&s_conn_lock);
	return mtu;
}

static void disconnect_work(struct k_work *work)
{
	k_mutex_lock(&s_conn_lock, K_FOREVER);
	if (s_conn) {
		bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
	k_mutex_unlock(&s_conn_lock);
}

static void ble_disconnect(int delay_ms)
{
	k_work_reschedule(&s_disconnect_work, K_MSEC(delay_ms));
}

static const struct setup_transport TRANSPORT = {
	.send = ble_send,
	.mtu = ble_mtu,
	.disconnect = ble_disconnect,
};

static void adv_work(struct k_work *work)
{
	if (!s_advertising_enabled || s_conn) {
		return;
	}
	static uint8_t mfg[3] = {0xFF, 0xFF, 0x00};
	mfg[2] = store_is_paired() ? 0x01 : 0x00;
	static const uint8_t svc[] = {SVC_UUID_VAL};
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
		BT_DATA(BT_DATA_UUID128_ALL, svc, sizeof(svc)),
		BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg, sizeof(mfg)),
	};
	const char *name = bt_get_name();
	const struct bt_data sd[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name)),
	};
	int rc = bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
						 BT_GAP_ADV_FAST_INT_MAX_2, NULL),
				 ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (rc && rc != -EALREADY) {
		LOG_ERR("advertising failed: %d", rc);
	} else {
		LOG_INF("advertising as %s", name);
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		k_work_submit(&s_adv_work);
		return;
	}
	k_mutex_lock(&s_conn_lock, K_FOREVER);
	if (s_conn) {
		k_mutex_unlock(&s_conn_lock);
		bt_conn_disconnect(conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
		return;
	}
	s_conn = bt_conn_ref(conn);
	k_mutex_unlock(&s_conn_lock);
	LOG_INF("setup client connected");
	board_set_status(BOARD_STATUS_PAIRING);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	k_mutex_lock(&s_conn_lock, K_FOREVER);
	if (conn != s_conn) {
		k_mutex_unlock(&s_conn_lock);
		return;
	}
	bt_conn_unref(s_conn);
	s_conn = NULL;
	k_mutex_unlock(&s_conn_lock);
	LOG_INF("setup client disconnected (0x%02x)", reason);
	setup_on_disconnect();
	if (!store_is_paired()) {
		board_set_status(BOARD_STATUS_SETUP);
	}
}

static void recycled(void)
{
	k_work_submit(&s_adv_work);
}

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	LOG_INF("ATT MTU %u", bt_gatt_get_mtu(conn));
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

static struct bt_gatt_cb gatt_cb = {
	.att_mtu_updated = mtu_updated,
};

static void stop_work(struct k_work *work)
{
	s_advertising_enabled = false;
	bt_le_adv_stop();
	disconnect_work(NULL);
	LOG_INF("setup closed");
}

int setup_transport_start(const char *name)
{
	static bool initialised;
	if (!initialised) {
		k_work_init(&s_adv_work, adv_work);
		k_work_init_delayable(&s_disconnect_work, disconnect_work);
		k_work_init_delayable(&s_stop_work, stop_work);
		int rc = bt_enable(NULL);
		if (rc) {
			LOG_ERR("Bluetooth init failed: %d", rc);
			return rc;
		}
		bt_gatt_cb_register(&gatt_cb);
		initialised = true;
	}
	setup_init(&TRANSPORT, NULL);
	bt_set_name(name);
	s_advertising_enabled = true;
	k_work_submit(&s_adv_work);
	return 0;
}

void setup_transport_stop(int delay_ms)
{
	k_work_reschedule(&s_stop_work, K_MSEC(delay_ms));
}
