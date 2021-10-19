/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** @file
 *  @brief Bluetooth Mesh DFU Target role sample
 */
#include <zephyr.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <drivers/hwinfo.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/mesh.h>
#include "dfu_target.h"

#if DT_NODE_EXISTS(DT_ALIAS(led0))
#define LED0 DT_ALIAS(led0)
#elif DT_NODE_EXISTS(DT_NODELABEL(led0))
#define LED0 DT_NODELABEL(led0)
#else
#define LED0 DT_INVALID_NODE
#endif

#if DT_NODE_EXISTS(LED0)
#define LED0_DEV DT_PHANDLE(LED0, gpios)
#define LED0_PIN DT_PHA(LED0, gpios, pin)
#define LED0_FLAGS DT_PHA(LED0, gpios, flags)

static const struct device *led_dev = DEVICE_DT_GET(LED0_DEV);
#endif /* LED0 */

static void led_init(void)
{
#if DT_NODE_EXISTS(LED0)
	int err;

	if (!device_is_ready(led_dev)) {
		return;
	}

	err = gpio_pin_configure(led_dev, LED0_PIN, LED0_FLAGS | GPIO_OUTPUT_INACTIVE);
	if (err) {
		printk("Failed to initialize LEDs: %d\n", err);
	}
#else
	printk("WARNING: LEDs not supported on this board.\n");
#endif
}

static void led_set(bool val)
{
#if DT_NODE_EXISTS(LED0)
	gpio_pin_set(led_dev, LED0_PIN, val);
#endif
}

static void attention_on(struct bt_mesh_model *mod)
{
	led_set(true);
}

static void attention_off(struct bt_mesh_model *mod)
{
	led_set(false);
}

static const struct bt_mesh_health_srv_cb health_srv_cb = {
	.attn_on = attention_on,
	.attn_off = attention_off,
};

static struct bt_mesh_health_srv health_srv = {
	.cb = &health_srv_cb,
};

BT_MESH_HEALTH_PUB_DEFINE(health_pub, 0);

static struct bt_mesh_model models[] = {
	BT_MESH_MODEL_CFG_SRV,
	BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
	BT_MESH_MODEL_DFU_SRV(&dfu_srv)
};

static struct bt_mesh_elem elements[] = {
	BT_MESH_ELEM(1, models, BT_MESH_MODEL_NONE),
};

static const struct bt_mesh_comp comp = {
	.cid = CONFIG_BT_COMPANY_ID,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
};

static void prov_complete(uint16_t net_idx, uint16_t addr)
{
	printk("Node provisioned, addr: 0x%04x\n", addr);
}

static void prov_reset(void)
{
	bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
}

static uint8_t dev_uuid[16] = { 0xdd, 0xdd };

static const struct bt_mesh_prov prov = {
	.uuid = dev_uuid,
	.complete = prov_complete,
	.reset = prov_reset,
};

static void bt_ready(int err)
{
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
		return;
	}

	printk("Bluetooth initialized\n");

	err = bt_mesh_init(&prov, &comp);
	if (err) {
		printk("Initializing mesh failed (err %d)\n", err);
		return;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);

	printk("Mesh initialized\n");

	/* Confirm the image and mark it as applied after the mesh started. */
	dfu_target_image_confirm();
}

void main(void)
{
	int err;

	printk("Initializing...\n");

	if (IS_ENABLED(CONFIG_HWINFO)) {
		hwinfo_get_device_id(dev_uuid, sizeof(dev_uuid));
	}

	led_init();
	dfu_target_init();

	err = bt_enable(bt_ready);
	if (err) {
		printk("Bluetooth init failed (err %d)\n", err);
	}
}
