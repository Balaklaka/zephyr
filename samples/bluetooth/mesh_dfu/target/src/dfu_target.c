/** @file
 *  @brief Bluetooth Mesh DFU Target role handler
 */

/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <sys/reboot.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/mesh.h>

#include <storage/flash_map.h>
#include <dfu/mcuboot.h>

static struct bt_mesh_blob_io_flash flash_stream;
static enum bt_mesh_dfu_effect img_effect = BT_MESH_DFU_EFFECT_NONE;

static struct bt_mesh_dfu_img dfu_imgs[] = { {
	.fwid = &((struct mcuboot_img_sem_ver){}),
	.fwid_len = sizeof(struct mcuboot_img_sem_ver),
} };

static int dfu_meta_check(struct bt_mesh_dfu_srv *srv,
			  const struct bt_mesh_dfu_img *img,
			  const uint8_t *metadata, size_t metadata_len,
			  enum bt_mesh_dfu_effect *effect)
{
	/* TODO: Parse metadata here. */

	*effect = (img_effect = BT_MESH_DFU_EFFECT_UNPROV);
	return 0;
}

static int dfu_start(struct bt_mesh_dfu_srv *srv,
		     const struct bt_mesh_dfu_img *img, const uint8_t *metadata,
		     size_t metadata_len, const struct bt_mesh_blob_io **io)
{
	printk("Firmware upload started\n");

	*io = &flash_stream.io;

	return 0;
}

static void dfu_end(struct bt_mesh_dfu_srv *srv, const struct bt_mesh_dfu_img *img, bool success)
{
	printk("Firmware upload %s\n", success ? "succeeded" : "failed");

	if (!success) {
		return;
	}

	/* TODO: Add verification code here. */

	bt_mesh_dfu_srv_verified(srv);
}

static int dfu_recover(struct bt_mesh_dfu_srv *srv,
		       const struct bt_mesh_dfu_img *img,
		       const struct bt_mesh_blob_io **io)
{
	printk("Recovering the firmware upload\n");

	/* TODO: Need to recover the effect. */

	*io = &flash_stream.io;

	return 0;
}

static void do_reboot(struct k_work *work)
{
	sys_reboot(SYS_REBOOT_WARM);
}

static int dfu_apply(struct bt_mesh_dfu_srv *srv, const struct bt_mesh_dfu_img *img)
{
	static struct k_work pending_reboot;

	printk("Applying the new firmware\n");

	boot_request_upgrade(BOOT_UPGRADE_TEST);

	if (img_effect == BT_MESH_DFU_EFFECT_UNPROV) {
		bt_mesh_reset();

		printk("Pending the mesh settings to cleared before rebooting...");

		/* Let the mesh reset its settings before rebooting the device. */
		k_work_init(&pending_reboot, do_reboot);
		k_work_submit(&pending_reboot);
	} else {
		/* No need to unprovision device. Reboot immediately. */
		sys_reboot(SYS_REBOOT_WARM);
	}

	return 0;
}

static const struct bt_mesh_dfu_srv_cb dfu_handlers = {
	.check = dfu_meta_check,
	.start = dfu_start,
	.end = dfu_end,
	.apply = dfu_apply,
	.recover = dfu_recover,
};

struct bt_mesh_dfu_srv dfu_srv =
	BT_MESH_DFU_SRV_INIT(&dfu_handlers, dfu_imgs, ARRAY_SIZE(dfu_imgs));

static void image_version_load(void)
{
	struct mcuboot_img_sem_ver *img_ver = (struct mcuboot_img_sem_ver *) dfu_imgs[0].fwid;
	struct mcuboot_img_header img_header;
	int err;

	err = boot_read_bank_header(FLASH_AREA_ID(image_0), &img_header, sizeof(img_header));
	if (err) {
		printk("Failed to read image header: %d", err);
		return;
	}

	*img_ver = img_header.h.v1.sem_ver;

	printk("Current image version: %u.%u.%u+%u\n", img_ver->major, img_ver->minor,
	       img_ver->revision, img_ver->build_num);
}

int dfu_target_init(void)
{
	int err;

	image_version_load();

	err = bt_mesh_blob_io_flash_init(&flash_stream, FLASH_AREA_ID(image_1), 0);
	if (err) {
		printk("Failed to init BLOB IO Flash module: %d\n", err);
		return err;
	}

	return 0;
}

void dfu_target_image_confirm(void)
{
	int err;

	err = boot_write_img_confirmed();
	if (err) {
		printk("Failed to confirm image: %d\n", err);
	}

	/* Switch DFU Server state to the Idle state if it was in the Applying state. */
	bt_mesh_dfu_srv_applied(&dfu_srv);
}
