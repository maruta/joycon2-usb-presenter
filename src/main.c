/*
 * Joy-Con 2 USB Presenter — BLE central + composite USB HID device.
 *
 * Copyright (c) 2026 Ichiro Maruta
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Bluetooth-host bring-up plumbing was originally inspired by the
 * Nordic Semiconductor `samples/bluetooth/central_hids` example, but the
 * BLE/GATT, USB, HID, and input-mapping logic in this file has been
 * reimplemented from scratch against the public Zephyr Bluetooth and USB
 * APIs.
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/scan.h>

#include <zephyr/settings/settings.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/drivers/usb/udc.h>

/* Nintendo (Joy-Con 2) custom 128-bit BLE service:
 *   service:      ab7de9be-89fe-49ad-828f-118f09df7fd0
 *   input notify: ab7de9be-89fe-49ad-828f-118f09df7fd2
 * Write characteristic UUID is not fixed — discovered at runtime.
 */
#define BT_UUID_NINTENDO_SVC_VAL \
	BT_UUID_128_ENCODE(0xab7de9be, 0x89fe, 0x49ad, 0x828f, 0x118f09df7fd0)
#define BT_UUID_NINTENDO_INPUT_VAL \
	BT_UUID_128_ENCODE(0xab7de9be, 0x89fe, 0x49ad, 0x828f, 0x118f09df7fd2)
/* Dedicated write characteristic in a separate service. Sourced from
 * Misaka10571/joycon2-connector. Writing the IMU enable bytes here (and not
 * to the wwr-capable char inside the Nintendo input service) is what
 * actually turns IMU notifications on. */
#define BT_UUID_NINTENDO_WRITE_VAL \
	BT_UUID_128_ENCODE(0x649d4ac9, 0x8eb7, 0x4e6c, 0xaf44, 0x1ea54fe5f005)

static struct bt_uuid_128 nintendo_svc_uuid =
	BT_UUID_INIT_128(BT_UUID_NINTENDO_SVC_VAL);
static struct bt_uuid_128 nintendo_input_uuid =
	BT_UUID_INIT_128(BT_UUID_NINTENDO_INPUT_VAL);
static struct bt_uuid_128 nintendo_write_uuid =
	BT_UUID_INIT_128(BT_UUID_NINTENDO_WRITE_VAL);

static struct bt_conn *default_conn;

static struct bt_gatt_subscribe_params input_subscribe_params;
static uint16_t write_handle;

/* Discovery is kicked off from a workqueue, never directly from a BT RX
 * callback, so we have a real stack and a clean context. */
static void discover_work_handler(struct k_work *work);
static K_WORK_DEFINE(discover_work, discover_work_handler);
static struct bt_conn *discover_conn;

/* Misaka10571/joycon2-connector enables IMU by sending two writes 300 ms
 * apart, after the CCC subscription has settled. Schedule those via delayed
 * work so we don't block BT context. */
static void enable_send_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(enable_work, enable_send_handler);
static int enable_step;

/* Joy-Con 2 needs these two writes after subscribing before it starts
 * sending input notifications. Sourced from seitanmen/Joycon2forMac.
 */
static const uint8_t enable_std[] = {
	0x0c, 0x91, 0x01, 0x02, 0x00, 0x04, 0x00, 0x00,
	0xff, 0x00, 0x00, 0x00,
};
static const uint8_t enable_ext[] = {
	0x0c, 0x91, 0x01, 0x04, 0x00, 0x04, 0x00, 0x00,
	0xff, 0x00, 0x00, 0x00,
};

/* Player-LED command, format from Misaka10571/joycon2-connector
 * (BLECommands.h SetPlayerLEDs). Same envelope as the input-enable writes
 * but with command byte 0x09 and subcommand 0x07. */
static void joycon_set_leds(uint8_t mask)
{
	if (!default_conn || !write_handle) {
		return;
	}
	const uint8_t cmd[12] = {
		0x09, 0x91, 0x01, 0x07, 0x00, 0x04, 0x00, 0x00,
		mask, 0x00, 0x00, 0x00,
	};
	(void)bt_gatt_write_without_response(default_conn, write_handle,
					     cmd, sizeof(cmd), false);
}

/* Bit 3 = LED4, used as a "connected" indicator. */
#define LED_CONNECTED 0x08
#define LED_CTRL      0x01

static uint32_t prev_buttons;

/* ---- USB HID: composite Keyboard + Mouse, two report IDs ---------------- */
#define HID_RID_KBD   1
#define HID_RID_MOUSE 2

static const uint8_t hid_report_desc[] = {
	/* Keyboard collection */
	0x05, 0x01,       /* Usage Page: Generic Desktop */
	0x09, 0x06,       /* Usage: Keyboard */
	0xA1, 0x01,       /* Collection: Application */
	0x85, HID_RID_KBD,
	0x05, 0x07,       /*   Usage Page: Keyboard/Keypad */
	0x19, 0xE0,       /*   Usage Min: LCtrl */
	0x29, 0xE7,       /*   Usage Max: RGui */
	0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08,
	0x81, 0x02,       /*   8 modifier bits */
	0x95, 0x01, 0x75, 0x08,
	0x81, 0x01,       /*   reserved byte */
	0x95, 0x06, 0x75, 0x08,
	0x15, 0x00, 0x25, 0x65,
	0x05, 0x07,
	0x19, 0x00, 0x29, 0x65,
	0x81, 0x00,       /*   6 key codes */
	0xC0,
	/* Mouse collection */
	0x05, 0x01,       /* Usage Page: Generic Desktop */
	0x09, 0x02,       /* Usage: Mouse */
	0xA1, 0x01,       /* Collection: Application */
	0x85, HID_RID_MOUSE,
	0x09, 0x01,       /*   Usage: Pointer */
	0xA1, 0x00,       /*   Collection: Physical */
	0x05, 0x09,       /*     Usage Page: Button */
	0x19, 0x01, 0x29, 0x03,
	0x15, 0x00, 0x25, 0x01,
	0x95, 0x03, 0x75, 0x01,
	0x81, 0x02,       /*     3 buttons */
	0x95, 0x01, 0x75, 0x05,
	0x81, 0x03,       /*     5-bit padding */
	0x05, 0x01,
	0x09, 0x30, 0x09, 0x31,
	0x15, 0x81, 0x25, 0x7F,
	0x75, 0x08, 0x95, 0x02,
	0x81, 0x06,       /*     dx, dy */
	0x09, 0x38,       /*     Wheel */
	0x15, 0x81, 0x25, 0x7F,
	0x75, 0x08, 0x95, 0x01,
	0x81, 0x06,
	0x05, 0x0C,       /*     Usage Page: Consumer */
	0x0A, 0x38, 0x02, /*     AC Pan (horizontal wheel) */
	0x15, 0x81, 0x25, 0x7F,
	0x75, 0x08, 0x95, 0x01,
	0x81, 0x06,
	0xC0,
	0xC0,
};

struct __packed kbd_report {
	uint8_t report_id;
	uint8_t modifiers;
	uint8_t reserved;
	uint8_t keys[6];
};

struct __packed mouse_report {
	uint8_t report_id;
	uint8_t buttons;
	int8_t  dx;
	int8_t  dy;
	int8_t  wheel_v;
	int8_t  wheel_h;
};

static const struct device *hid_dev;
static atomic_t hid_iface_ready_flag;

static void hid_iface_ready(const struct device *dev, bool ready)
{
	atomic_set(&hid_iface_ready_flag, ready ? 1 : 0);
	printk("HID iface %s\n", ready ? "ready" : "not-ready");
}

static int hid_get_report_stub(const struct device *dev,
			       const uint8_t type, const uint8_t id,
			       const uint16_t len, uint8_t *const buf)
{
	return 0;
}

static int hid_set_report_stub(const struct device *dev,
			       const uint8_t type, const uint8_t id,
			       const uint16_t len, const uint8_t *const buf)
{
	return 0;
}

static struct hid_device_ops joycon_hid_ops = {
	.iface_ready = hid_iface_ready,
	.get_report  = hid_get_report_stub,
	.set_report  = hid_set_report_stub,
};

static int hid_send(const void *report, size_t len)
{
	if (!hid_dev || !atomic_get(&hid_iface_ready_flag)) {
		return -ENODEV;
	}
	return hid_device_submit_report(hid_dev, len, report);
}

/* HID Usage IDs (Keyboard/Keypad page 0x07) */
#define KEY_E         0x08
#define KEY_P         0x13
#define KEY_RIGHT     0x4F
#define KEY_LEFT      0x50
#define KEY_DOWN      0x51
#define KEY_UP        0x52
#define MOD_LCTRL     0x01
#define MOD_LSHIFT    0x02

/* Joy-Con 2 button bit positions (Nohzockt mapping) */
#define JC_Y     0
#define JC_X     1
#define JC_B     2
#define JC_A     3
#define JC_R_SR  4
#define JC_R_SL  5
#define JC_R     6
#define JC_ZR    7
#define JC_MINUS 8
#define JC_PLUS  9
#define JC_RJ    10
#define JC_LJ    11
#define JC_HOME  12
#define JC_CAPT  13
#define JC_C     14
#define JC_DOWN  16
#define JC_UP    17
#define JC_RIGHT 18
#define JC_LEFT  19
#define JC_L_SR  20
#define JC_L_SL  21
#define JC_L     22
#define JC_ZL    23

#define BTN(b, bit) (((b) >> (bit)) & 1u)

static void build_kbd_report(uint32_t btn, bool ctrl_active,
			     struct kbd_report *r)
{
	memset(r, 0, sizeof(*r));
	r->report_id = HID_RID_KBD;

	if (ctrl_active) {
		r->modifiers |= MOD_LCTRL;
	}

	int slot = 0;
#define ADD_KEY(code)                                                          \
	do {                                                                   \
		if (slot < 6) {                                                \
			r->keys[slot++] = (code);                              \
		}                                                              \
	} while (0)

	if (BTN(btn, JC_RIGHT) || BTN(btn, JC_A)) ADD_KEY(KEY_RIGHT);
	if (BTN(btn, JC_LEFT)  || BTN(btn, JC_Y)) ADD_KEY(KEY_LEFT);
	if (BTN(btn, JC_UP)    || BTN(btn, JC_X)) ADD_KEY(KEY_UP);
	if (BTN(btn, JC_DOWN)  || BTN(btn, JC_B)) ADD_KEY(KEY_DOWN);
	if (BTN(btn, JC_MINUS) || BTN(btn, JC_PLUS)) ADD_KEY(KEY_P);
	if (BTN(btn, JC_CAPT)  || BTN(btn, JC_C)) ADD_KEY(KEY_E);
#undef ADD_KEY
}

#define GYRO_DEADZONE 50    /* raw units */
#define GYRO_DIVISOR  32    /* tune mouse speed */
/* Joy-Con 2 optical mouse sensor scaling. Raw values are an int16 absolute
 * counter at bytes 16-19; we diff successive samples. Increase to slow the
 * cursor down, decrease to speed it up. */
#define OPTICAL_DIVISOR 2
#define STICK_CENTER  2048
#define STICK_WHEEL_THRESH 150
/* Lower = faster max scroll. With ~1700 max deflection past deadzone and a
 * 60 Hz BLE notification rate, accum gains ~100k/sec at full tilt; 8000
 * gives ≈12 clicks/sec at full deflection, ≈3 clicks/sec at quarter tilt. */
#define WHEEL_ACCUM_THRESH 2000

static int32_t wheel_h_accum;
static int32_t wheel_v_accum;

static bool build_mouse_report(uint32_t btn,
			       int16_t gx, int16_t gz,
			       int16_t opt_dx, int16_t opt_dy,
			       uint16_t lx, uint16_t ly,
			       uint16_t rx, uint16_t ry,
			       struct mouse_report *r)
{
	memset(r, 0, sizeof(*r));
	r->report_id = HID_RID_MOUSE;

	if (BTN(btn, JC_ZR) || BTN(btn, JC_ZL)) r->buttons |= 0x01;
	if (BTN(btn, JC_R)  || BTN(btn, JC_L))  r->buttons |= 0x02;
	if (BTN(btn, JC_RJ) || BTN(btn, JC_LJ)) r->buttons |= 0x04;

	/* Gyro to mouse delta — gated by any side-rail button (L_SL, L_SR,
	 * R_SL, R_SR). Holding one of these is the user's "I am pointing"
	 * gesture; otherwise the cursor sits still. Axis 1 (X gyro) drives
	 * vertical, axis 3 (Z gyro) drives horizontal, both inverted so the
	 * cursor follows the user's pointing direction intuitively. */
	bool gyro_active = BTN(btn, JC_L_SL) || BTN(btn, JC_L_SR) ||
			   BTN(btn, JC_R_SL) || BTN(btn, JC_R_SR) ||
			   BTN(btn, JC_ZR)   || BTN(btn, JC_ZL) ||
			   BTN(btn, JC_R)    || BTN(btn, JC_L);

	int32_t dx_total = 0;
	int32_t dy_total = 0;

	if (gyro_active) {
		int32_t dx_raw =
			(gz > GYRO_DEADZONE || gz < -GYRO_DEADZONE) ? -gz : 0;
		int32_t dy_raw =
			(gx > GYRO_DEADZONE || gx < -GYRO_DEADZONE) ? -gx : 0;

		dx_total += dx_raw / GYRO_DIVISOR;
		dy_total += dy_raw / GYRO_DIVISOR;
	}

	/* Optical mouse delta is always summed in. When the controller is in
	 * the air the sensor reports ~zero, so this adds nothing. On a desk
	 * surface it dominates. */
	dx_total += opt_dx / OPTICAL_DIVISOR;
	dy_total += opt_dy / OPTICAL_DIVISOR;

	r->dx = (int8_t)CLAMP(dx_total, -127, 127);
	r->dy = (int8_t)CLAMP(dy_total, -127, 127);

	/* Sticks → wheel via per-frame accumulation. Bigger tilt = faster
	 * accumulation = more frequent ±1 clicks. */
#define DEFLECT_DZ(raw)                                                        \
	((raw) >  STICK_WHEEL_THRESH ? (raw) - STICK_WHEEL_THRESH :            \
	 (raw) < -STICK_WHEEL_THRESH ? (raw) + STICK_WHEEL_THRESH : 0)

	int32_t lx_d = (int32_t)lx - STICK_CENTER;
	int32_t ly_d = (int32_t)ly - STICK_CENTER;
	int32_t rx_d = (int32_t)rx - STICK_CENTER;
	int32_t ry_d = (int32_t)ry - STICK_CENTER;
	int32_t h_input = DEFLECT_DZ(lx_d) + DEFLECT_DZ(rx_d);
	int32_t v_input = DEFLECT_DZ(ly_d) + DEFLECT_DZ(ry_d);
#undef DEFLECT_DZ

	/* Reset accumulator when stick recenters so the next push starts fresh
	 * (avoids one immediate click from leftover accumulation). */
	if (h_input == 0) wheel_h_accum = 0;
	else wheel_h_accum += h_input;
	if (v_input == 0) wheel_v_accum = 0;
	else wheel_v_accum += v_input;

	int8_t hw = 0, vw = 0;
	if (wheel_h_accum >=  WHEEL_ACCUM_THRESH) { hw =  1; wheel_h_accum -= WHEEL_ACCUM_THRESH; }
	else if (wheel_h_accum <= -WHEEL_ACCUM_THRESH) { hw = -1; wheel_h_accum += WHEEL_ACCUM_THRESH; }
	if (wheel_v_accum >=  WHEEL_ACCUM_THRESH) { vw =  1; wheel_v_accum -= WHEEL_ACCUM_THRESH; }
	else if (wheel_v_accum <= -WHEEL_ACCUM_THRESH) { vw = -1; wheel_v_accum += WHEEL_ACCUM_THRESH; }

	r->wheel_v = vw;
	r->wheel_h = hw;

	bool any = (r->buttons || r->dx || r->dy || r->wheel_v || r->wheel_h);
	return any;
}

/* device_next: build a single usbd context that contains both CDC ACM
 * (registered automatically because the board defines its DT node) and our
 * HID device. */
USBD_DEVICE_DEFINE(joycon_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   0x2FE3, 0x0007);

USBD_DESC_LANG_DEFINE(joycon_lang);
USBD_DESC_MANUFACTURER_DEFINE(joycon_mfr, "Joy-Con 2 Presenter");
USBD_DESC_PRODUCT_DEFINE(joycon_product, "Joy-Con 2 USB Presenter");

USBD_DESC_CONFIG_DEFINE(joycon_fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(joycon_fs_cfg, USB_SCD_SELF_POWERED, 250,
			  &joycon_fs_cfg_desc);

static int usb_setup(void)
{
	int err;

	hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
	if (!device_is_ready(hid_dev)) {
		printk("HID device is not ready\n");
		return -ENODEV;
	}

	err = hid_device_register(hid_dev, hid_report_desc,
				  sizeof(hid_report_desc), &joycon_hid_ops);
	if (err) {
		printk("hid_device_register failed: %d\n", err);
		return err;
	}

	usbd_add_descriptor(&joycon_usbd, &joycon_lang);
	usbd_add_descriptor(&joycon_usbd, &joycon_mfr);
	usbd_add_descriptor(&joycon_usbd, &joycon_product);

	err = usbd_add_configuration(&joycon_usbd, USBD_SPEED_FS, &joycon_fs_cfg);
	if (err) {
		printk("usbd_add_configuration failed: %d\n", err);
		return err;
	}

	err = usbd_register_all_classes(&joycon_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		printk("usbd_register_all_classes failed: %d\n", err);
		return err;
	}

	/* CDC + HID -> use IAD-friendly class triple. */
	usbd_device_set_code_triple(&joycon_usbd, USBD_SPEED_FS,
				    USB_BCC_MISCELLANEOUS, 0x02, 0x01);

	err = usbd_init(&joycon_usbd);
	if (err) {
		printk("usbd_init failed: %d\n", err);
		return err;
	}

	err = usbd_enable(&joycon_usbd);
	if (err) {
		printk("usbd_enable failed: %d\n", err);
		return err;
	}

	printk("USB HID + CDC ACM ready\n");
	return 0;
}
/* ----------------------------------------------------------------------- */

static uint8_t input_notify_cb(struct bt_conn *conn,
			       struct bt_gatt_subscribe_params *params,
			       const void *data, uint16_t length)
{
	if (!data) {
		return BT_GATT_ITER_STOP;
	}
	if (length < 60) {
		return BT_GATT_ITER_CONTINUE;
	}

	const uint8_t *p = data;
	/* Buttons: 24 valid bits in bytes 4..6; byte 7 is status flags. */
	uint32_t btn = sys_get_le32(&p[4]) & 0x00ffffffu;
	uint16_t lx = p[10] | ((p[11] & 0x0f) << 8);
	uint16_t ly = (p[11] >> 4) | (p[12] << 4);
	uint16_t rx = p[13] | ((p[14] & 0x0f) << 8);
	uint16_t ry = (p[14] >> 4) | (p[15] << 4);

	/* Optical mouse: int16 absolute counters → diff for delta. */
	int16_t opt_x = (int16_t)sys_get_le16(&p[16]);
	int16_t opt_y = (int16_t)sys_get_le16(&p[18]);
	static int16_t prev_opt_x, prev_opt_y;
	static bool opt_seeded;
	int16_t opt_dx = opt_seeded ? (int16_t)(opt_x - prev_opt_x) : 0;
	int16_t opt_dy = opt_seeded ? (int16_t)(opt_y - prev_opt_y) : 0;

	prev_opt_x = opt_x;
	prev_opt_y = opt_y;
	opt_seeded = true;

	/* Gyro X/Z drive vertical/horizontal mouse motion (other IMU axes unused). */
	int16_t gx = (int16_t)sys_get_le16(&p[54]);
	int16_t gz = (int16_t)sys_get_le16(&p[58]);

	/* Laser-pointer toggle: L_SL / R_SR edge-press. While ON, Ctrl is
	 * implicitly held during left-click or scroll. */
	static uint32_t edge_prev_btn;
	static bool laser_mode, prev_laser_mode, prev_ctrl_active;
	uint32_t pressed_now = btn & ~edge_prev_btn;

	edge_prev_btn = btn;
	if (pressed_now & ((1u << JC_L_SL) | (1u << JC_R_SR))) {
		laser_mode = !laser_mode;
	}

	bool left_click = BTN(btn, JC_ZR) || BTN(btn, JC_ZL);
	bool scroll_active =
		(abs((int)lx - STICK_CENTER) > STICK_WHEEL_THRESH) ||
		(abs((int)ly - STICK_CENTER) > STICK_WHEEL_THRESH) ||
		(abs((int)rx - STICK_CENTER) > STICK_WHEEL_THRESH) ||
		(abs((int)ry - STICK_CENTER) > STICK_WHEEL_THRESH);
	bool ctrl_active  = laser_mode && (left_click || scroll_active);
	bool ctrl_changed = (ctrl_active != prev_ctrl_active);

	prev_ctrl_active = ctrl_active;
	if (laser_mode != prev_laser_mode) {
		prev_laser_mode = laser_mode;
		joycon_set_leds(LED_CONNECTED | (laser_mode ? LED_CTRL : 0));
	}

	if ((btn != prev_buttons) || ctrl_changed) {
		struct kbd_report kbr;

		build_kbd_report(btn, ctrl_active, &kbr);
		hid_send(&kbr, sizeof(kbr));
	}
	prev_buttons = btn;

	struct mouse_report mr;
	bool send_mouse = build_mouse_report(btn, gx, gz, opt_dx, opt_dy,
					     lx, ly, rx, ry, &mr);
	/* Always send when mouse-button bits change so releases reach the host. */
	static uint8_t prev_mouse_buttons;

	if (mr.buttons != prev_mouse_buttons) {
		prev_mouse_buttons = mr.buttons;
		send_mouse = true;
	}
	if (send_mouse) {
		hid_send(&mr, sizeof(mr));
	}
	return BT_GATT_ITER_CONTINUE;
}

/* Send std/ext enables in alternation, twice each. Joy-Con 2 (R)
 * sometimes ignores the first pair if it arrives too soon after CCC
 * subscription, so we always send four. After that, light LED4 to
 * indicate the controller is fully online. */
static void enable_send_handler(struct k_work *work)
{
	if (!default_conn || !write_handle) {
		return;
	}
	if (enable_step >= 4) {
		joycon_set_leds(LED_CONNECTED);
		printk("Initial LED set\n");
		return;
	}

	const uint8_t *cmd = (enable_step & 1) ? enable_ext : enable_std;

	(void)bt_gatt_write_without_response(default_conn, write_handle,
					     cmd, sizeof(enable_std), false);
	printk("enable step %d sent\n", enable_step);

	enable_step++;
	k_work_schedule(&enable_work, K_MSEC(300));
}

static void schedule_enables(void)
{
	enable_step = 0;
	/* Give the CCC write a moment to land at the peer first. */
	k_work_schedule(&enable_work, K_MSEC(150));
}

static void on_scan_match(struct bt_scan_device_info *info,
			  struct bt_scan_filter_match *match,
			  bool connectable)
{
	ARG_UNUSED(match);

	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(info->recv_info->addr, addr, sizeof(addr));
	printk("match: %s rssi=%d %sconn\n",
	       addr, info->recv_info->rssi, connectable ? "" : "non-");
}

static void on_scan_create_failed(struct bt_scan_device_info *info)
{
	ARG_UNUSED(info);
	printk("scan: connect attempt failed\n");
}

static void on_scan_connecting(struct bt_scan_device_info *info,
			       struct bt_conn *conn)
{
	ARG_UNUSED(info);
	/* Hold a reference to the conn until disconnect releases it. */
	default_conn = bt_conn_ref(conn);
}

BT_SCAN_CB_INIT(jcp_scan_cbs,
		on_scan_match, NULL,
		on_scan_create_failed, on_scan_connecting);

/* Discovery state machine using core bt_gatt_discover.
 * Step 1: find Nintendo primary service (handle range)
 * Step 2: enumerate characteristics in that range
 * Step 3: locate the CCC descriptor of the input notify char
 * Step 4: subscribe + send enable commands
 */
static struct bt_gatt_discover_params discover_params;
static struct bt_uuid_16 ccc_uuid_storage = BT_UUID_INIT_16(BT_UUID_GATT_CCC_VAL);

static struct {
	uint16_t svc_start;
	uint16_t svc_end;
	uint16_t input_value_handle;
	uint16_t input_ccc_handle;
} disc_state;

static uint8_t write_char_discover_cb(struct bt_conn *conn,
				      const struct bt_gatt_attr *attr,
				      struct bt_gatt_discover_params *params)
{
	if (!attr) {
		printk("Dedicated write characteristic not found\n");
		return BT_GATT_ITER_STOP;
	}

	const struct bt_gatt_chrc *chrc = attr->user_data;

	write_handle = chrc->value_handle;
	printk("Write char (dedicated) value_handle=%u props=0x%02x\n",
	       chrc->value_handle, chrc->properties);

	schedule_enables();
	return BT_GATT_ITER_STOP;
}

static uint8_t ccc_discover_cb(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       struct bt_gatt_discover_params *params)
{
	if (!attr) {
		printk("CCC descriptor not found\n");
		return BT_GATT_ITER_STOP;
	}

	disc_state.input_ccc_handle = attr->handle;
	printk("CCC handle=%u\n", attr->handle);

	input_subscribe_params.notify       = input_notify_cb;
	input_subscribe_params.value        = BT_GATT_CCC_NOTIFY;
	input_subscribe_params.value_handle = disc_state.input_value_handle;
	input_subscribe_params.ccc_handle   = disc_state.input_ccc_handle;

	int err = bt_gatt_subscribe(conn, &input_subscribe_params);

	if (err && err != -EALREADY) {
		printk("Subscribe failed (err %d)\n", err);
		return BT_GATT_ITER_STOP;
	}
	printk("Subscribed\n");

	/* Now hunt for the dedicated write characteristic across the whole
	 * GATT db; it lives in a separate service from the input one. */
	discover_params.uuid         = &nintendo_write_uuid.uuid;
	discover_params.start_handle = 0x0001;
	discover_params.end_handle   = 0xffff;
	discover_params.type         = BT_GATT_DISCOVER_CHARACTERISTIC;
	discover_params.func         = write_char_discover_cb;

	err = bt_gatt_discover(conn, &discover_params);
	if (err) {
		printk("Write char discover failed (err %d)\n", err);
	}

	return BT_GATT_ITER_STOP;
}

static uint8_t char_discover_cb(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				struct bt_gatt_discover_params *params)
{
	if (!attr) {
		if (!disc_state.input_value_handle) {
			printk("Input characteristic not found\n");
			return BT_GATT_ITER_STOP;
		}

		discover_params.uuid         = &ccc_uuid_storage.uuid;
		discover_params.start_handle = disc_state.input_value_handle + 1;
		discover_params.end_handle   = disc_state.svc_end;
		discover_params.type         = BT_GATT_DISCOVER_DESCRIPTOR;
		discover_params.func         = ccc_discover_cb;

		int err = bt_gatt_discover(conn, &discover_params);

		if (err) {
			printk("CCC discover failed (err %d)\n", err);
		}
		return BT_GATT_ITER_STOP;
	}

	const struct bt_gatt_chrc *chrc = attr->user_data;

	if (!bt_uuid_cmp(chrc->uuid, &nintendo_input_uuid.uuid)) {
		disc_state.input_value_handle = chrc->value_handle;
		printk("Input char value_handle=%u props=0x%02x\n",
		       chrc->value_handle, chrc->properties);
	}
	return BT_GATT_ITER_CONTINUE;
}

static uint8_t primary_svc_cb(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr,
			      struct bt_gatt_discover_params *params)
{
	if (!attr) {
		printk("Nintendo Service not found\n");
		return BT_GATT_ITER_STOP;
	}

	const struct bt_gatt_service_val *svc = attr->user_data;

	disc_state.svc_start = attr->handle;
	disc_state.svc_end   = svc->end_handle;
	printk("Nintendo Service handles=%u..%u\n",
	       disc_state.svc_start, disc_state.svc_end);

	discover_params.uuid         = NULL;
	discover_params.start_handle = disc_state.svc_start + 1;
	discover_params.end_handle   = disc_state.svc_end;
	discover_params.type         = BT_GATT_DISCOVER_CHARACTERISTIC;
	discover_params.func         = char_discover_cb;

	int err = bt_gatt_discover(conn, &discover_params);

	if (err) {
		printk("Char discover failed (err %d)\n", err);
	}
	return BT_GATT_ITER_STOP;
}

static void discover_work_handler(struct k_work *work)
{
	if (!discover_conn) {
		return;
	}

	memset(&disc_state, 0, sizeof(disc_state));
	write_handle = 0;

	printk("Starting GATT discovery\n");
	discover_params.uuid         = &nintendo_svc_uuid.uuid;
	discover_params.start_handle = 0x0001;
	discover_params.end_handle   = 0xffff;
	discover_params.type         = BT_GATT_DISCOVER_PRIMARY;
	discover_params.func         = primary_svc_cb;

	int err = bt_gatt_discover(discover_conn, &discover_params);

	if (err) {
		printk("Primary discover failed (err %d)\n", err);
	}
}

static void gatt_discover(struct bt_conn *conn)
{
	if (conn != default_conn) {
		return;
	}

	discover_conn = conn;
	k_work_submit(&discover_work);
}

/* Drop the cached connection ref if it matches the one being torn down. */
static void release_default_conn(struct bt_conn *expected)
{
	if (default_conn != expected) {
		return;
	}
	bt_conn_unref(default_conn);
	default_conn = NULL;
}

/* Resume scanning; logs but does not propagate the error since there's
 * not much the caller can do about it. */
static void resume_scan(void)
{
	int rc = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);

	if (rc) {
		printk("scan restart failed (%d)\n", rc);
	}
}

static void on_connected(struct bt_conn *conn, uint8_t hci_err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (hci_err == 0U) {
		printk("connected: %s\n", addr);
		/* Discovery runs on the system workqueue to keep the BT RX
		 * thread's stack lean. */
		gatt_discover(conn);
		return;
	}

	printk("connect failed: %s err=0x%02x (%s)\n",
	       addr, hci_err, bt_hci_err_to_str(hci_err));
	release_default_conn(conn);
	resume_scan();
}

static void on_disconnected(struct bt_conn *conn, uint8_t hci_reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("disconnected: %s reason=0x%02x (%s)\n",
	       addr, hci_reason, bt_hci_err_to_str(hci_reason));

	write_handle = 0;
	release_default_conn(conn);
	resume_scan();
}

static void on_security_changed(struct bt_conn *conn, bt_security_t lvl,
				enum bt_security_err sec_err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (sec_err == BT_SECURITY_ERR_SUCCESS) {
		printk("security: %s level=%u\n", addr, lvl);
	} else {
		printk("security FAIL: %s level=%u err=%d (%s)\n",
		       addr, lvl, sec_err, bt_security_err_to_str(sec_err));
	}
	gatt_discover(conn);
}

BT_CONN_CB_DEFINE(jcp_conn_cbs) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.security_changed = on_security_changed,
};

/* Joy-Con 2 manufacturer-data signature observed by sniffing: starts with
 * Nintendo company ID 0x0553 LE, then a fixed prefix 01 00 03, then 7e 05.
 * Matching these 7 bytes is enough to disambiguate from random nearby
 * BLE devices and works for both L and R Joy-Con 2 units. */
static const uint8_t jcp_mfg_signature[] = {
	0x53, 0x05, 0x01, 0x00, 0x03, 0x7e, 0x05,
};

/* Faster connection events than the BT-spec default (30-50 ms). 15-30 ms
 * keeps Joy-Con 2 happy while still leaving 4 s supervision timeout. */
static const struct bt_le_conn_param jcp_conn_param =
	BT_LE_CONN_PARAM_INIT(0x000c, 0x0018, 0, 400);

static int jcp_scan_setup(void)
{
	const struct bt_scan_init_param init = {
		.connect_if_match = 1,
		.scan_param       = NULL,
		.conn_param       = &jcp_conn_param,
	};
	struct bt_scan_manufacturer_data mfg_filter = {
		.data     = (uint8_t *)jcp_mfg_signature,
		.data_len = sizeof(jcp_mfg_signature),
	};
	int rc;

	bt_scan_init(&init);
	bt_scan_cb_register(&jcp_scan_cbs);

	rc = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_MANUFACTURER_DATA,
				&mfg_filter);
	if (rc) {
		printk("filter add failed (%d)\n", rc);
		return rc;
	}

	rc = bt_scan_filter_enable(BT_SCAN_MANUFACTURER_DATA_FILTER, false);
	if (rc) {
		printk("filter enable failed (%d)\n", rc);
		return rc;
	}
	return 0;
}

/* Compose "<tag>: <addr>" without repeating bt_addr_le_to_str everywhere. */
static void log_with_addr(const char *tag, struct bt_conn *conn)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("%s: %s\n", tag, addr);
}

static void on_pair_cancel(struct bt_conn *conn)
{
	log_with_addr("pair-cancel", conn);
}

static void on_pair_done(struct bt_conn *conn, bool bonded)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("paired: %s (bonded=%d)\n", addr, bonded);
}

static void on_pair_fail(struct bt_conn *conn, enum bt_security_err why)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	printk("pair fail: %s why=%d (%s)\n",
	       addr, why, bt_security_err_to_str(why));
}

/* Populate only .cancel so Zephyr derives our IO capability as
 * NoInputNoOutput; SMP then negotiates Just Works against the IO-less
 * Joy-Con 2. */
static struct bt_conn_auth_cb auth_basic_cb = {
	.cancel = on_pair_cancel,
};

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = on_pair_done,
	.pairing_failed   = on_pair_fail,
};

/* Bring the Bluetooth host stack up: register pairing callbacks first so
 * they're in place before any incoming connection event, then enable the
 * controller, then load persisted settings. */
static int bt_bringup(void)
{
	int rc;

	rc = bt_conn_auth_cb_register(&auth_basic_cb);
	if (rc) {
		printk("auth cb register: %d\n", rc);
		return rc;
	}

	rc = bt_conn_auth_info_cb_register(&auth_info_cb);
	if (rc) {
		printk("auth info cb register: %d\n", rc);
		return rc;
	}

	rc = bt_enable(NULL);
	if (rc) {
		printk("bt_enable: %d\n", rc);
		return rc;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	/* Drop persisted bonds — re-pair fresh on every boot. Simplifies
	 * recovery when a stale LTK lingers from an earlier session. */
	(void)bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	return 0;
}

int main(void)
{
	/* USB first: CDC ACM is the console, HID is the presenter output. */
	usb_setup();
	k_sleep(K_SECONDS(3));   /* let the host open the virtual COM port */
	printk("=== Joy-Con 2 USB Presenter ===\n");

	if (bt_bringup() != 0) {
		return 0;
	}
	printk("BT initialized\n");

	if (jcp_scan_setup() != 0) {
		return 0;
	}
	if (bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE) != 0) {
		printk("scan start failed\n");
		return 0;
	}
	printk("scanning... hold SYNC on Joy-Con 2\n");

	/* Low-frequency liveness ping so the operator can tell the firmware
	 * is still alive when nothing else is happening. */
	for (uint32_t t = 0; ; t++) {
		k_sleep(K_SECONDS(10));
		printk(". %u\n", t);
	}
	return 0;
}
