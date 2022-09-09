/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "smp_bt.h"

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>

#include <zephyr/mgmt/mcumgr/smp_bt.h>

static struct k_work advertise_work;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL,
		      0x84, 0xaa, 0x60, 0x74, 0x52, 0x8a, 0x8b, 0x86,
		      0xd3, 0x4c, 0xb7, 0x1d, 0x1d, 0xdc, 0x53, 0x8d),
};

static void advertise(struct k_work *work)
{
	int rc;

	rc = bt_le_adv_start(BT_LE_ADV_CONN_NAME, ad, ARRAY_SIZE(ad), NULL, 0);
	if (rc) {
		printk("SMP BT service advertising failed to start (rc %d)", rc);
		return;
	}

	printk("SMP BT service advertising successfully started");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	k_work_submit(&advertise_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.disconnected = disconnected,
};

void smp_bt_start(void)
{
	int err;

	k_work_init(&advertise_work, advertise);

	/* Initialize the Bluetooth mcumgr transport. */
	err = smp_bt_register();
	if (err) {
		printk("SMP BT service registration failed (err %d)", err);
		return;
	}

	k_work_submit(&advertise_work);
}
