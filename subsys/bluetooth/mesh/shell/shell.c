/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <ctype.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <zephyr/shell/shell.h>
#include <zephyr/settings/settings.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/mesh/shell.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

/* Private includes for raw Network & Transport layer access */
#include "mesh/mesh.h"
#include "mesh/net.h"
#include "mesh/rpl.h"
#include "mesh/transport.h"
#include "mesh/foundation.h"
#include "mesh/settings.h"
#include "mesh/access.h"
#include "mesh/shell_dfd.h"
#include "mesh/dfu_slot.h"
#include "mesh/sar_cfg_internal.h"
#include "utils.h"

#define CID_NVAL   0xffff
#define COMPANY_ID_LF 0x05F1
#define COMPANY_ID_NORDIC_SEMI 0x05F9

const struct shell *bt_mesh_shell_ctx_shell;

struct bt_mesh_shell_target bt_mesh_shell_target_ctx = {
	.dst = BT_MESH_ADDR_UNASSIGNED,
};

#define shell_print_ctx(_ft, ...)                                                            \
		do {                                                                         \
			if (bt_mesh_shell_ctx_shell != NULL) {                               \
				shell_print(bt_mesh_shell_ctx_shell, _ft, ##__VA_ARGS__);    \
			}                                                                    \
		} while (0)

/* Default net, app & dev key values, unless otherwise specified */
const uint8_t bt_mesh_shell_default_key[16] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
};

#if defined(CONFIG_BT_MESH_SHELL_HEALTH_SRV_INSTANCE)
static uint8_t cur_faults[BT_MESH_SHELL_CUR_FAULTS_MAX];
static uint8_t reg_faults[BT_MESH_SHELL_CUR_FAULTS_MAX * 2];

static void get_faults(uint8_t *faults, uint8_t faults_size, uint8_t *dst, uint8_t *count)
{
	uint8_t i, limit = *count;

	for (i = 0U, *count = 0U; i < faults_size && *count < limit; i++) {
		if (faults[i]) {
			*dst++ = faults[i];
			(*count)++;
		}
	}
}

static int fault_get_cur(struct bt_mesh_model *model, uint8_t *test_id,
			 uint16_t *company_id, uint8_t *faults, uint8_t *fault_count)
{
	shell_print_ctx("Sending current faults");

	*test_id = 0x00;
	*company_id = BT_COMP_ID_LF;

	get_faults(cur_faults, sizeof(cur_faults), faults, fault_count);

	return 0;
}

static int fault_get_reg(struct bt_mesh_model *model, uint16_t cid,
			 uint8_t *test_id, uint8_t *faults, uint8_t *fault_count)
{
	if (cid != CONFIG_BT_COMPANY_ID) {
		shell_print_ctx("Faults requested for unknown Company ID"
				" 0x%04x", cid);
		return -EINVAL;
	}

	shell_print_ctx("Sending registered faults");

	*test_id = 0x00;

	get_faults(reg_faults, sizeof(reg_faults), faults, fault_count);

	return 0;
}

static int fault_clear(struct bt_mesh_model *model, uint16_t cid)
{
	if (cid != CONFIG_BT_COMPANY_ID) {
		return -EINVAL;
	}

	(void)memset(reg_faults, 0, sizeof(reg_faults));

	return 0;
}

static int fault_test(struct bt_mesh_model *model, uint8_t test_id,
		      uint16_t cid)
{
	if (cid != CONFIG_BT_COMPANY_ID) {
		return -EINVAL;
	}

	if (test_id != 0x00) {
		return -EINVAL;
	}

	return 0;
}

static void attention_on(struct bt_mesh_model *model)
{
	shell_print_ctx("Attention On");
}

static void attention_off(struct bt_mesh_model *model)
{
	shell_print_ctx("Attention Off");
}

static const struct bt_mesh_health_srv_cb health_srv_cb = {
	.fault_get_cur = fault_get_cur,
	.fault_get_reg = fault_get_reg,
	.fault_clear = fault_clear,
	.fault_test = fault_test,
	.attn_on = attention_on,
	.attn_off = attention_off,
};
#endif /* CONFIG_BT_MESH_SHELL_HEALTH_SRV_INSTANCE */

#ifdef CONFIG_BT_MESH_LARGE_COMP_DATA_SRV
static uint8_t health_tests[] = {
	BT_MESH_HEALTH_TEST_INFO(COMPANY_ID_LF, 6, 0x01, 0x02, 0x03, 0x04, 0x34, 0x15),
	BT_MESH_HEALTH_TEST_INFO(COMPANY_ID_NORDIC_SEMI, 3, 0x01, 0x02, 0x03),
};

static struct bt_mesh_models_metadata_entry health_srv_meta[] = {
	BT_MESH_HEALTH_TEST_INFO_METADATA(health_tests),
	BT_MESH_MODELS_METADATA_END,
};
#endif

struct bt_mesh_health_srv bt_mesh_shell_health_srv = {
#if defined(CONFIG_BT_MESH_SHELL_HEALTH_SRV_INSTANCE)
	.cb = &health_srv_cb,
#endif
#ifdef CONFIG_BT_MESH_LARGE_COMP_DATA_SRV
	.metadata = health_srv_meta,
#endif
};

#if defined(CONFIG_BT_MESH_SHELL_HEALTH_CLI)
static void show_faults(uint8_t test_id, uint16_t cid, uint8_t *faults, size_t fault_count)
{
	size_t i;

	if (!fault_count) {
		shell_print_ctx("Health Test ID 0x%02x Company ID "
				"0x%04x: no faults", test_id, cid);
		return;
	}

	shell_print_ctx("Health Test ID 0x%02x Company ID 0x%04x Fault "
			"Count %zu:", test_id, cid, fault_count);

	for (i = 0; i < fault_count; i++) {
		shell_print_ctx("\t0x%02x", faults[i]);
	}
}

static void health_current_status(struct bt_mesh_health_cli *cli, uint16_t addr,
				  uint8_t test_id, uint16_t cid, uint8_t *faults,
				  size_t fault_count)
{
	shell_print_ctx("Health Current Status from 0x%04x", addr);
	show_faults(test_id, cid, faults, fault_count);
}

static void health_fault_status(struct bt_mesh_health_cli *cli, uint16_t addr,
				uint8_t test_id, uint16_t cid, uint8_t *faults,
				size_t fault_count)
{
	shell_print_ctx("Health Fault Status from 0x%04x", addr);
	show_faults(test_id, cid, faults, fault_count);
}

static void health_attention_status(struct bt_mesh_health_cli *cli,
				    uint16_t addr, uint8_t attention)
{
	shell_print_ctx("Health Attention Status from 0x%04x: %u", addr, attention);
}

static void health_period_status(struct bt_mesh_health_cli *cli, uint16_t addr,
				 uint8_t period)
{
	shell_print_ctx("Health Fast Period Divisor Status from 0x%04x: %u", addr, period);
}

struct bt_mesh_health_cli bt_mesh_shell_health_cli = {
	.current_status = health_current_status,
	.fault_status = health_fault_status,
	.attention_status = health_attention_status,
	.period_status = health_period_status,
};
#endif /* CONFIG_BT_MESH_SHELL_HEALTH_CLI */

#if defined(CONFIG_BT_MESH_BLOB_CLI) || defined(CONFIG_BT_MESH_BLOB_SRV)

static uint8_t blob_rx_sum;
static bool blob_valid;
static const char *blob_data = "blob";

static int blob_io_open(const struct bt_mesh_blob_io *io,
		    const struct bt_mesh_blob_xfer *xfer,
		    enum bt_mesh_blob_io_mode mode)
{
	blob_rx_sum = 0;
	blob_valid = true;
	return 0;
}

static int blob_chunk_wr(const struct bt_mesh_blob_io *io,
			 const struct bt_mesh_blob_xfer *xfer,
			 const struct bt_mesh_blob_block *block,
			 const struct bt_mesh_blob_chunk *chunk)
{
	int i;

	for (i = 0; i < chunk->size; ++i) {
		blob_rx_sum += chunk->data[i];
		if (chunk->data[i] !=
		    blob_data[(i + chunk->offset) % sizeof(blob_data)]) {
			blob_valid = false;
		}
	}

	return 0;
}

static int blob_chunk_rd(const struct bt_mesh_blob_io *io,
			 const struct bt_mesh_blob_xfer *xfer,
			 const struct bt_mesh_blob_block *block,
			 const struct bt_mesh_blob_chunk *chunk)
{
	for (int i = 0; i < chunk->size; ++i) {
		chunk->data[i] =
			blob_data[(i + chunk->offset) % sizeof(blob_data)];
	}

	return 0;
}

static const struct bt_mesh_blob_io dummy_blob_io = {
	.open = blob_io_open,
	.rd = blob_chunk_rd,
	.wr = blob_chunk_wr,
};

static const struct bt_mesh_blob_io *blob_io;

#endif

#if defined(CONFIG_BT_MESH_BLOB_IO_FLASH)

static struct bt_mesh_blob_io_flash blob_flash_stream;

#endif

#if defined(CONFIG_BT_MESH_DFD_SRV) || defined(CONFIG_BT_MESH_DFU_CLI)

static void slot_info_print(const struct shell *shell, const struct bt_mesh_dfu_slot *slot,
			    const uint8_t *idx)
{
	char fwid[2 * CONFIG_BT_MESH_DFU_FWID_MAXLEN + 1];
	char metadata[2 * CONFIG_BT_MESH_DFU_METADATA_MAXLEN + 1];
	char uri[CONFIG_BT_MESH_DFU_URI_MAXLEN + 1];
	size_t len;

	len = bin2hex(slot->fwid, slot->fwid_len, fwid, sizeof(fwid));
	fwid[len] = '\0';
	len = bin2hex(slot->metadata, slot->metadata_len, metadata,
		      sizeof(metadata));
	metadata[len] = '\0';
	memcpy(uri, slot->uri, slot->uri_len);
	uri[slot->uri_len] = '\0';

	if (idx != NULL) {
		shell_print(shell, "Slot %u:", *idx);
	} else {
		shell_print(shell, "Slot:");
	}
	shell_print(shell, "\tSize:     %u bytes", slot->size);
	shell_print(shell, "\tFWID:     %s", fwid);
	shell_print(shell, "\tMetadata: %s", metadata);
	shell_print(shell, "\tURI:      %s", uri);
}

#endif /* defined(CONFIG_BT_MESH_DFD_SRV) || defined(CONFIG_BT_MESH_DFU_CLI) */

#if !defined(CONFIG_BT_MESH_DFD_SRV)
#if defined(CONFIG_BT_MESH_DFU_CLI)

static void dfu_cli_ended(struct bt_mesh_dfu_cli *cli,
			  enum bt_mesh_dfu_status reason)
{
	shell_print(bt_mesh_shell_ctx_shell, "DFU ended: %u", reason);
}

static void dfu_cli_applied(struct bt_mesh_dfu_cli *cli)
{
	shell_print(bt_mesh_shell_ctx_shell, "DFU applied.");
}

static void dfu_cli_lost_target(struct bt_mesh_dfu_cli *cli,
				struct bt_mesh_dfu_target *target)
{
	shell_print(bt_mesh_shell_ctx_shell, "DFU target lost: 0x%04x", target->blob.addr);
}

static void dfu_cli_confirmed(struct bt_mesh_dfu_cli *cli)
{
	shell_print(bt_mesh_shell_ctx_shell, "DFU confirmed");
}

const struct bt_mesh_dfu_cli_cb dfu_cli_cb = {
	.ended = dfu_cli_ended,
	.applied = dfu_cli_applied,
	.lost_target = dfu_cli_lost_target,
	.confirmed = dfu_cli_confirmed,
};

struct bt_mesh_dfu_cli bt_mesh_shell_dfu_cli = BT_MESH_DFU_CLI_INIT(&dfu_cli_cb);

#elif defined(CONFIG_BT_MESH_BLOB_CLI)

static struct {
	struct bt_mesh_blob_cli_inputs inputs;
	struct bt_mesh_blob_target targets[32];
	struct bt_mesh_blob_target_pull pull[32];
	uint8_t target_count;
	struct bt_mesh_blob_xfer xfer;
} blob_cli_xfer;

static void blob_cli_lost_target(struct bt_mesh_blob_cli *cli,
				 struct bt_mesh_blob_target *target,
				 enum bt_mesh_blob_status reason)
{
	shell_print(bt_mesh_shell_ctx_shell, "Mesh Blob: Lost target 0x%04x (reason: %u)",
		    target->addr, reason);
}

static void blob_cli_caps(struct bt_mesh_blob_cli *cli,
			  const struct bt_mesh_blob_cli_caps *caps)
{
	static const char * const modes[] = {
		"none",
		"push",
		"pull",
		"all",
	};

	if (!caps) {
		shell_print(bt_mesh_shell_ctx_shell,
			    "None of the targets can be used for BLOB transfer");
		return;
	}

	shell_print(bt_mesh_shell_ctx_shell, "Mesh BLOB: capabilities:");
	shell_print(bt_mesh_shell_ctx_shell, "\tMax BLOB size: %u bytes", caps->max_size);
	shell_print(bt_mesh_shell_ctx_shell, "\tBlock size: %u-%u (%u-%u bytes)",
		    caps->min_block_size_log, caps->max_block_size_log,
		    1 << caps->min_block_size_log,
		    1 << caps->max_block_size_log);
	shell_print(bt_mesh_shell_ctx_shell, "\tMax chunks: %u", caps->max_chunks);
	shell_print(bt_mesh_shell_ctx_shell, "\tChunk size: %u", caps->max_chunk_size);
	shell_print(bt_mesh_shell_ctx_shell, "\tMTU size: %u", caps->mtu_size);
	shell_print(bt_mesh_shell_ctx_shell, "\tModes: %s", modes[caps->modes]);
}

static void blob_cli_end(struct bt_mesh_blob_cli *cli,
			 const struct bt_mesh_blob_xfer *xfer, bool success)
{
	if (success) {
		shell_print(bt_mesh_shell_ctx_shell, "Mesh BLOB transfer complete.");
	} else {
		shell_print(bt_mesh_shell_ctx_shell, "Mesh BLOB transfer failed.");
	}
}

static const struct bt_mesh_blob_cli_cb blob_cli_handlers = {
	.lost_target = blob_cli_lost_target,
	.caps = blob_cli_caps,
	.end = blob_cli_end,
};

struct bt_mesh_blob_cli bt_mesh_shell_blob_cli = {
	.cb = &blob_cli_handlers
};

#endif /* CONFIG_BT_MESH_BLOB_CLI */

#endif /* !CONFIG_BT_MESH_DFD_SRV */

#if defined(CONFIG_BT_MESH_DFU_SRV)

struct shell_dfu_fwid {
	uint8_t type;
	struct mcuboot_img_sem_ver ver;
};

static struct bt_mesh_dfu_img dfu_imgs[] = { {
	.fwid = &((struct shell_dfu_fwid){ 0x01, { 1, 0, 0, 0 } }),
	.fwid_len = sizeof(struct shell_dfu_fwid),
} };

static int dfu_meta_check(struct bt_mesh_dfu_srv *srv,
			      const struct bt_mesh_dfu_img *img,
			      struct net_buf_simple *metadata,
			      enum bt_mesh_dfu_effect *effect)
{
	return 0;
}

static int dfu_start(struct bt_mesh_dfu_srv *srv,
		     const struct bt_mesh_dfu_img *img,
		     struct net_buf_simple *metadata,
		     const struct bt_mesh_blob_io **io)
{
	shell_print(bt_mesh_shell_ctx_shell, "DFU setup");

	*io = blob_io;

	return 0;
}

static void dfu_end(struct bt_mesh_dfu_srv *srv,
		    const struct bt_mesh_dfu_img *img, bool success)
{
	if (!success) {
		shell_print(bt_mesh_shell_ctx_shell, "DFU failed");
		return;
	}

	if (!blob_valid) {
		bt_mesh_dfu_srv_rejected(srv);
		return;
	}

	bt_mesh_dfu_srv_verified(srv);
}

static int dfu_apply(struct bt_mesh_dfu_srv *srv,
		     const struct bt_mesh_dfu_img *img)
{
	if (!blob_valid) {
		return -EINVAL;
	}

	shell_print(bt_mesh_shell_ctx_shell, "Applying DFU transfer...");

	return 0;
}

static const struct bt_mesh_dfu_srv_cb dfu_handlers = {
	.check = dfu_meta_check,
	.start = dfu_start,
	.end = dfu_end,
	.apply = dfu_apply,
};

struct bt_mesh_dfu_srv bt_mesh_shell_dfu_srv =
	BT_MESH_DFU_SRV_INIT(&dfu_handlers, dfu_imgs, ARRAY_SIZE(dfu_imgs));

#elif defined(CONFIG_BT_MESH_BLOB_SRV)
static int64_t blob_time;

static int blob_srv_start(struct bt_mesh_blob_srv *srv,
			  struct bt_mesh_msg_ctx *ctx,
			  struct bt_mesh_blob_xfer *xfer)
{
	shell_print(bt_mesh_shell_ctx_shell, "BLOB start");
	blob_time = k_uptime_get();
	return 0;
}

static void blob_srv_end(struct bt_mesh_blob_srv *srv, uint64_t id,
			 bool success)
{
	if (success) {
		int64_t duration = k_uptime_delta(&blob_time);

		shell_print(bt_mesh_shell_ctx_shell, "BLOB completed in %u.%03u s",
			(uint32_t)(duration / MSEC_PER_SEC),
			(uint32_t)(duration % MSEC_PER_SEC));
	} else {
		shell_print(bt_mesh_shell_ctx_shell, "BLOB cancelled");
	}
}

static const struct bt_mesh_blob_srv_cb blob_srv_cb = {
	.start = blob_srv_start,
	.end = blob_srv_end,
};

struct bt_mesh_blob_srv bt_mesh_shell_blob_srv = {
	.cb = &blob_srv_cb
};
#endif

#if defined(CONFIG_BT_MESH_RPR_CLI)
static void rpr_scan_report(struct bt_mesh_rpr_cli *cli,
			    const struct bt_mesh_rpr_node *srv,
			    struct bt_mesh_rpr_unprov *unprov,
			    struct net_buf_simple *adv_data)
{
	char uuid_hex_str[32 + 1];

	bin2hex(unprov->uuid, 16, uuid_hex_str, sizeof(uuid_hex_str));

	shell_print(bt_mesh_shell_ctx_shell,
		    "Server 0x%04x:\n"
		    "\tuuid:   %s\n"
		    "\tOOB:    0x%04x",
		    srv->addr, uuid_hex_str, unprov->oob);

	while (adv_data && adv_data->len > 2) {
		uint8_t len, type;
		uint8_t data[31];

		len = net_buf_simple_pull_u8(adv_data) - 1;
		type = net_buf_simple_pull_u8(adv_data);
		memcpy(data, net_buf_simple_pull_mem(adv_data, len), len);
		data[len] = '\0';

		if (type == BT_DATA_URI) {
			shell_print(bt_mesh_shell_ctx_shell, "\tURI:    \"\\x%02x%s\"",
				    data[0], &data[1]);
		} else if (type == BT_DATA_NAME_COMPLETE) {
			shell_print(bt_mesh_shell_ctx_shell, "\tName:   \"%s\"", data);
		} else {
			char string[64 + 1];

			bin2hex(data, len, string, sizeof(string));
			shell_print(bt_mesh_shell_ctx_shell, "\t0x%02x:  %s", type, string);
		}
	}
}

struct bt_mesh_rpr_cli bt_mesh_shell_rpr_cli = {
	.scan_report = rpr_scan_report,
};

#endif /* CONFIG_BT_MESH_RPR_CLI */

static int cmd_init(const struct shell *sh, size_t argc, char *argv[])
{

	bt_mesh_shell_ctx_shell = sh;
	shell_print(sh, "Mesh shell initialized");

#if defined(CONFIG_BT_MESH_DFU_SRV) && defined(CONFIG_BOOTLOADER_MCUBOOT)
	struct mcuboot_img_header img_header;

	int err = boot_read_bank_header(FLASH_AREA_ID(image_0), &img_header,
					sizeof(img_header));
	if (!err) {
		struct shell_dfu_fwid *fwid =
			(struct shell_dfu_fwid *)dfu_imgs[0].fwid;

		fwid->ver = img_header.h.v1.sem_ver;

		boot_write_img_confirmed();
	}
#endif

	blob_io = &dummy_blob_io;

	if (IS_ENABLED(CONFIG_BT_MESH_RPR_SRV)) {
		bt_mesh_prov_enable(BT_MESH_PROV_REMOTE);
	}

	return 0;
}

static int cmd_reset(const struct shell *sh, size_t argc, char *argv[])
{
#if defined(CONFIG_BT_MESH_CDB)
	bt_mesh_cdb_clear();
# endif
	bt_mesh_reset();
	shell_print(sh, "Local node reset complete");

	return 0;
}

#if defined(CONFIG_BT_MESH_SHELL_LOW_POWER)
static int cmd_lpn(const struct shell *sh, size_t argc, char *argv[])
{
	static bool enabled;
	bool onoff;
	int err = 0;

	if (argc < 2) {
		shell_print(sh, "%s", enabled ? "enabled" : "disabled");
		return 0;
	}

	onoff = shell_strtobool(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	if (onoff) {
		if (enabled) {
			shell_print(sh, "LPN already enabled");
			return 0;
		}

		err = bt_mesh_lpn_set(true);
		if (err) {
			shell_error(sh, "Enabling LPN failed (err %d)", err);
		} else {
			enabled = true;
		}
	} else {
		if (!enabled) {
			shell_print(sh, "LPN already disabled");
			return 0;
		}

		err = bt_mesh_lpn_set(false);
		if (err) {
			shell_error(sh, "Enabling LPN failed (err %d)", err);
		} else {
			enabled = false;
		}
	}

	return 0;
}

static int cmd_poll(const struct shell *sh, size_t argc, char *argv[])
{
	int err;

	err = bt_mesh_lpn_poll();
	if (err) {
		shell_error(sh, "Friend Poll failed (err %d)", err);
	}

	return 0;
}

static void lpn_established(uint16_t net_idx, uint16_t friend_addr,
					uint8_t queue_size, uint8_t recv_win)
{
	shell_print_ctx("Friendship (as LPN) established to "
			"Friend 0x%04x Queue Size %d Receive Window %d",
			friend_addr, queue_size, recv_win);
}

static void lpn_terminated(uint16_t net_idx, uint16_t friend_addr)
{
	shell_print_ctx("Friendship (as LPN) lost with Friend "
			"0x%04x", friend_addr);
}

BT_MESH_LPN_CB_DEFINE(lpn_cb) = {
	.established = lpn_established,
	.terminated = lpn_terminated,
};
#endif /* CONFIG_BT_MESH_SHELL_LOW_POWER */

#if defined(CONFIG_BT_MESH_SHELL_GATT_PROXY)
#if defined(CONFIG_BT_MESH_GATT_PROXY)
static int cmd_ident(const struct shell *sh, size_t argc, char *argv[])
{
	int err;

	err = bt_mesh_proxy_identity_enable();
	if (err) {
		shell_error(sh, "Failed advertise using Node Identity (err "
			    "%d)", err);
	}

	return 0;
}
#endif /* CONFIG_BT_MESH_GATT_PROXY */

#if defined(CONFIG_BT_MESH_PROXY_CLIENT)
static int cmd_proxy_connect(const struct shell *sh, size_t argc,
			     char *argv[])
{
	uint16_t net_idx;
	int err = 0;

	net_idx = shell_strtoul(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	err = bt_mesh_proxy_connect(net_idx);
	if (err) {
		shell_error(sh, "Proxy connect failed (err %d)", err);
	}

	return 0;
}

static int cmd_proxy_disconnect(const struct shell *sh, size_t argc,
				char *argv[])
{
	uint16_t net_idx;
	int err = 0;

	net_idx = shell_strtoul(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	err = bt_mesh_proxy_disconnect(net_idx);
	if (err) {
		shell_error(sh, "Proxy disconnect failed (err %d)", err);
	}

	return 0;
}
#endif /* CONFIG_BT_MESH_PROXY_CLIENT */
#endif /* CONFIG_BT_MESH_SHELL_GATT_PROXY */

#if defined(CONFIG_BT_MESH_SHELL_PROV)
static int cmd_input_num(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	uint32_t val;

	val = shell_strtoul(argv[1], 10, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	err = bt_mesh_input_number(val);
	if (err) {
		shell_error(sh, "Numeric input failed (err %d)", err);
	}

	return 0;
}

static int cmd_input_str(const struct shell *sh, size_t argc, char *argv[])
{
	int err = bt_mesh_input_string(argv[1]);

	if (err) {
		shell_error(sh, "String input failed (err %d)", err);
	}

	return 0;
}

static const char *bearer2str(bt_mesh_prov_bearer_t bearer)
{
	switch (bearer) {
	case BT_MESH_PROV_ADV:
		return "PB-ADV";
	case BT_MESH_PROV_GATT:
		return "PB-GATT";
	case BT_MESH_PROV_REMOTE:
		return "PB-REMOTE";
	default:
		return "unknown";
	}
}

#if defined(CONFIG_BT_MESH_SHELL_PROV_CTX_INSTANCE)
static uint8_t dev_uuid[16] = { 0xdd, 0xdd };

static void prov_complete(uint16_t net_idx, uint16_t addr)
{

	shell_print_ctx("Local node provisioned, net_idx 0x%04x address "
			"0x%04x", net_idx, addr);

	bt_mesh_shell_target_ctx.net_idx = net_idx,
	bt_mesh_shell_target_ctx.dst = addr;
}

static void reprovisioned(uint16_t addr)
{
	shell_print(bt_mesh_shell_ctx_shell, "Local node re-provisioned, new address 0x%04x",
		    addr);

	if (bt_mesh_shell_target_ctx.dst == bt_mesh_primary_addr()) {
		bt_mesh_shell_target_ctx.dst = addr;
	}
}

static void prov_node_added(uint16_t net_idx, uint8_t uuid[16], uint16_t addr,
			    uint8_t num_elem)
{
	shell_print_ctx("Node provisioned, net_idx 0x%04x address "
			"0x%04x elements %d", net_idx, addr, num_elem);

	bt_mesh_shell_target_ctx.net_idx = net_idx,
	bt_mesh_shell_target_ctx.dst = addr;
}

#if defined(CONFIG_BT_MESH_PROVISIONER)
static enum {
	AUTH_NO_OOB,
	AUTH_STATIC_OOB,
	AUTH_OUTPUT_OOB,
	AUTH_INPUT_OOB
} auth_type;

static void capabilities(const struct bt_mesh_dev_capabilities *cap)
{
	if (cap->oob_type && auth_type == AUTH_STATIC_OOB) {
		bt_mesh_auth_method_set_static(bt_mesh_shell_prov.static_val,
					       bt_mesh_shell_prov.static_val_len);
		return;
	}

	if (cap->output_actions && auth_type == AUTH_OUTPUT_OOB) {
		bt_mesh_auth_method_set_output(BT_MESH_DISPLAY_NUMBER, 6);
		return;
	}

	if (cap->input_actions && auth_type == AUTH_INPUT_OOB) {
		bt_mesh_auth_method_set_input(BT_MESH_ENTER_NUMBER, 6);
		return;
	}

	bt_mesh_auth_method_set_none();
}
#endif

static void prov_input_complete(void)
{
	shell_print_ctx("Input complete");
}

static void prov_reset(void)
{
	shell_print_ctx("The local node has been reset and needs "
			"reprovisioning");
}

static int output_number(bt_mesh_output_action_t action, uint32_t number)
{
	switch (action) {
	case BT_MESH_BLINK:
		shell_print_ctx("OOB blink Number: %u", number);
		break;
	case BT_MESH_BEEP:
		shell_print_ctx("OOB beep Number: %u", number);
		break;
	case BT_MESH_VIBRATE:
		shell_print_ctx("OOB vibrate Number: %u", number);
		break;
	case BT_MESH_DISPLAY_NUMBER:
		shell_print_ctx("OOB display Number: %u", number);
		break;
	default:
		if (bt_mesh_shell_ctx_shell != NULL) {
			shell_error(bt_mesh_shell_ctx_shell,
				    "Unknown Output action %u (number %u) requested!",
				    action, number);
		}
		return -EINVAL;
	}

	return 0;
}

static int output_string(const char *str)
{
	shell_print_ctx("OOB String: %s", str);
	return 0;
}

static int input(bt_mesh_input_action_t act, uint8_t size)
{

	switch (act) {
	case BT_MESH_ENTER_NUMBER:
		shell_print_ctx("Enter a number (max %u digits) with: Input-num <num>", size);
		break;
	case BT_MESH_ENTER_STRING:
		shell_print_ctx("Enter a string (max %u chars) with: Input-str <str>", size);
		break;
	case BT_MESH_TWIST:
		shell_print_ctx("\"Twist\" a number (max %u digits) with: Input-num <num>", size);
		break;
	case BT_MESH_PUSH:
		shell_print_ctx("\"Push\" a number (max %u digits) with: Input-num <num>", size);
		break;
	default:
		if (bt_mesh_shell_ctx_shell != NULL) {
			shell_error(bt_mesh_shell_ctx_shell,
				    "Unknown Input action %u (size %u) requested!", act, size);
		}
		return -EINVAL;
	}

	return 0;
}

static void link_open(bt_mesh_prov_bearer_t bearer)
{
	shell_print_ctx("Provisioning link opened on %s", bearer2str(bearer));
}

static void link_close(bt_mesh_prov_bearer_t bearer)
{
	shell_print_ctx("Provisioning link closed on %s", bearer2str(bearer));
}

static uint8_t static_val[16];

struct bt_mesh_prov bt_mesh_shell_prov = {
	.uuid = dev_uuid,
	.link_open = link_open,
	.link_close = link_close,
	.complete = prov_complete,
	.reprovisioned = reprovisioned,
	.node_added = prov_node_added,
	.reset = prov_reset,
	.static_val = NULL,
	.static_val_len = 0,
	.output_size = 6,
	.output_actions = (BT_MESH_BLINK | BT_MESH_BEEP | BT_MESH_VIBRATE | BT_MESH_DISPLAY_NUMBER |
			   BT_MESH_DISPLAY_STRING),
	.output_number = output_number,
	.output_string = output_string,
	.input_size = 6,
	.input_actions =
		(BT_MESH_ENTER_NUMBER | BT_MESH_ENTER_STRING | BT_MESH_TWIST | BT_MESH_PUSH),
	.input = input,
	.input_complete = prov_input_complete,
#if defined(CONFIG_BT_MESH_PROVISIONER)
	.capabilities = capabilities
#endif
};

static int cmd_static_oob(const struct shell *sh, size_t argc, char *argv[])
{
	if (argc < 2) {
		bt_mesh_shell_prov.static_val = NULL;
		bt_mesh_shell_prov.static_val_len = 0U;
	} else {
		bt_mesh_shell_prov.static_val_len = hex2bin(argv[1], strlen(argv[1]),
					      static_val, 16);
		if (bt_mesh_shell_prov.static_val_len) {
			bt_mesh_shell_prov.static_val = static_val;
		} else {
			bt_mesh_shell_prov.static_val = NULL;
		}
	}

	if (bt_mesh_shell_prov.static_val) {
		shell_print(sh, "Static OOB value set (length %u)",
			    bt_mesh_shell_prov.static_val_len);
	} else {
		shell_print(sh, "Static OOB value cleared");
	}

	return 0;
}

static int cmd_uuid(const struct shell *sh, size_t argc, char *argv[])
{
	uint8_t uuid[16];
	size_t len;

	if (argc < 2) {
		char uuid_hex_str[32 + 1];

		bin2hex(dev_uuid, 16, uuid_hex_str, sizeof(uuid_hex_str));

		shell_print_ctx("Device UUID: %s", uuid_hex_str);
		return 0;
	}

	len = hex2bin(argv[1], strlen(argv[1]), uuid, sizeof(uuid));
	if (len < 1) {
		return -EINVAL;
	}

	memcpy(dev_uuid, uuid, len);
	(void)memset(dev_uuid + len, 0, sizeof(dev_uuid) - len);

	shell_print(sh, "Device UUID set");

	return 0;
}

static void print_unprovisioned_beacon(uint8_t uuid[16],
				       bt_mesh_prov_oob_info_t oob_info,
				       uint32_t *uri_hash)
{
	char uuid_hex_str[32 + 1];

	bin2hex(uuid, 16, uuid_hex_str, sizeof(uuid_hex_str));

	shell_print_ctx("PB-ADV UUID %s, OOB Info 0x%04x, URI Hash 0x%x",
			uuid_hex_str, oob_info,
			(uri_hash == NULL ? 0 : *uri_hash));
}

#if defined(CONFIG_BT_MESH_PB_GATT_CLIENT)
static void pb_gatt_unprovisioned(uint8_t uuid[16],
				  bt_mesh_prov_oob_info_t oob_info)
{
	char uuid_hex_str[32 + 1];

	bin2hex(uuid, 16, uuid_hex_str, sizeof(uuid_hex_str));

	shell_print_ctx("PB-GATT UUID %s, OOB Info 0x%04x", uuid_hex_str, oob_info);
}
#endif

static int cmd_beacon_listen(const struct shell *sh, size_t argc,
			     char *argv[])
{
	int err = 0;
	bool val = shell_strtobool(argv[1], 0, &err);

	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	if (val) {
		bt_mesh_shell_prov.unprovisioned_beacon = print_unprovisioned_beacon;
#if defined(CONFIG_BT_MESH_PB_GATT_CLIENT)
		bt_mesh_shell_prov.unprovisioned_beacon_gatt = pb_gatt_unprovisioned;
#endif
	} else {
		bt_mesh_shell_prov.unprovisioned_beacon = NULL;
		bt_mesh_shell_prov.unprovisioned_beacon_gatt = NULL;
	}

	return 0;
}
#endif /* CONFIG_BT_MESH_SHELL_PROV_CTX_INSTANCE */

#if defined(CONFIG_BT_MESH_PB_GATT_CLIENT)
static int cmd_provision_gatt(const struct shell *sh, size_t argc,
			      char *argv[])
{
	static uint8_t uuid[16];
	uint8_t attention_duration;
	uint16_t net_idx;
	uint16_t addr;
	size_t len;
	int err = 0;

	len = hex2bin(argv[1], strlen(argv[1]), uuid, sizeof(uuid));
	(void)memset(uuid + len, 0, sizeof(uuid) - len);

	net_idx = shell_strtoul(argv[2], 0, &err);
	addr = shell_strtoul(argv[3], 0, &err);
	attention_duration = shell_strtoul(argv[4], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	err = bt_mesh_provision_gatt(uuid, net_idx, addr, attention_duration);
	if (err) {
		shell_error(sh, "Provisioning failed (err %d)", err);
	}

	return 0;
}
#endif /* CONFIG_BT_MESH_PB_GATT_CLIENT */

#if defined(CONFIG_BT_MESH_PROV_DEVICE)
static int cmd_pb(bt_mesh_prov_bearer_t bearer, const struct shell *sh,
		  size_t argc, char *argv[])
{
	int err = 0;
	bool onoff;

	if (argc < 2) {
		return -EINVAL;
	}

	onoff = shell_strtobool(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	if (onoff) {
		err = bt_mesh_prov_enable(bearer);
		if (err) {
			shell_error(sh, "Failed to enable %s (err %d)",
				    bearer2str(bearer), err);
		} else {
			shell_print(sh, "%s enabled", bearer2str(bearer));
		}
	} else {
		err = bt_mesh_prov_disable(bearer);
		if (err) {
			shell_error(sh, "Failed to disable %s (err %d)",
				    bearer2str(bearer), err);
		} else {
			shell_print(sh, "%s disabled", bearer2str(bearer));
		}
	}

	return 0;
}

#if defined(CONFIG_BT_MESH_PB_ADV)
static int cmd_pb_adv(const struct shell *sh, size_t argc, char *argv[])
{
	return cmd_pb(BT_MESH_PROV_ADV, sh, argc, argv);
}
#endif /* CONFIG_BT_MESH_PB_ADV */

#if defined(CONFIG_BT_MESH_PB_GATT)
static int cmd_pb_gatt(const struct shell *sh, size_t argc, char *argv[])
{
	return cmd_pb(BT_MESH_PROV_GATT, sh, argc, argv);
}
#endif /* CONFIG_BT_MESH_PB_GATT */
#endif /* CONFIG_BT_MESH_PROV_DEVICE */

#if defined(CONFIG_BT_MESH_PROVISIONER)
static int cmd_remote_pub_key_set(const struct shell *sh, size_t argc, char *argv[])
{
	size_t len;
	uint8_t pub_key[64];
	int err = 0;

	len = hex2bin(argv[1], strlen(argv[1]), pub_key, sizeof(pub_key));
	if (len < 1) {
		shell_warn(sh, "Unable to parse input string argument");
		return -EINVAL;
	}

	err = bt_mesh_prov_remote_pub_key_set(pub_key);

	if (err) {
		shell_error(sh, "Setting remote pub key failed (err %d)", err);
	}

	return 0;
}

static int cmd_auth_method_set_input(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	bt_mesh_input_action_t action = shell_strtoul(argv[1], 10, &err);
	uint8_t size = shell_strtoul(argv[2], 10, &err);

	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	err = bt_mesh_auth_method_set_input(action, size);
	if (err) {
		shell_error(sh, "Setting input OOB authentication action failed (err %d)", err);
	}

	return 0;
}

static int cmd_auth_method_set_output(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;
	bt_mesh_output_action_t action = shell_strtoul(argv[1], 10, &err);
	uint8_t size = shell_strtoul(argv[2], 10, &err);

	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	err = bt_mesh_auth_method_set_output(action, size);
	if (err) {
		shell_error(sh, "Setting output OOB authentication action failed (err %d)", err);
	}
	return 0;
}

static int cmd_auth_method_set_static(const struct shell *sh, size_t argc, char *argv[])
{
	size_t len;
	uint8_t static_val[16];
	int err = 0;

	len = hex2bin(argv[1], strlen(argv[1]), static_val, sizeof(static_val));
	if (len < 1) {
		shell_warn(sh, "Unable to parse input string argument");
		return -EINVAL;
	}

	err = bt_mesh_auth_method_set_static(static_val, len);
	if (err) {
		shell_error(sh, "Setting static OOB authentication failed (err %d)", err);
	}
	return 0;
}

static int cmd_auth_method_set_none(const struct shell *sh, size_t argc, char *argv[])
{
	int err = bt_mesh_auth_method_set_none();

	if (err) {
		shell_error(sh, "Disabling authentication failed (err %d)", err);
	}
	return 0;
}

static int cmd_provision_adv(const struct shell *sh, size_t argc,
			     char *argv[])
{
	uint8_t uuid[16];
	uint8_t attention_duration;
	uint16_t net_idx;
	uint16_t addr;
	size_t len;
	int err = 0;

	len = hex2bin(argv[1], strlen(argv[1]), uuid, sizeof(uuid));
	(void)memset(uuid + len, 0, sizeof(uuid) - len);

	net_idx = shell_strtoul(argv[2], 0, &err);
	addr = shell_strtoul(argv[3], 0, &err);
	attention_duration = shell_strtoul(argv[4], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	err = bt_mesh_provision_adv(uuid, net_idx, addr, attention_duration);
	if (err) {
		shell_error(sh, "Provisioning failed (err %d)", err);
	}

	return 0;
}
#endif /* CONFIG_BT_MESH_PROVISIONER */

static int cmd_provision_local(const struct shell *sh, size_t argc, char *argv[])
{
	const uint8_t *net_key = bt_mesh_shell_default_key;
	uint16_t net_idx, addr;
	uint32_t iv_index;
	int err = 0;

	net_idx = shell_strtoul(argv[1], 0, &err);
	addr = shell_strtoul(argv[2], 0, &err);

	if (argc > 3) {
		iv_index = shell_strtoul(argv[3], 0, &err);
	} else {
		iv_index = 0U;
	}

	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	if (IS_ENABLED(CONFIG_BT_MESH_CDB)) {
		const struct bt_mesh_cdb_subnet *sub;

		sub = bt_mesh_cdb_subnet_get(net_idx);
		if (!sub) {
			shell_error(sh, "No cdb entry for subnet 0x%03x",
				    net_idx);
			return 0;
		}

		net_key = sub->keys[SUBNET_KEY_TX_IDX(sub)].net_key;
	}

	err = bt_mesh_provision(net_key, net_idx, 0, iv_index, addr,
				bt_mesh_shell_default_key);
	if (err) {
		shell_error(sh, "Provisioning failed (err %d)", err);
	}

	return 0;
}
#endif /* CONFIG_BT_MESH_SHELL_PROV */

#if defined(CONFIG_BT_MESH_SHELL_TEST)
static int cmd_net_send(const struct shell *sh, size_t argc, char *argv[])
{
	NET_BUF_SIMPLE_DEFINE(msg, 32);

	struct bt_mesh_msg_ctx ctx = {
		.send_ttl = BT_MESH_TTL_DEFAULT,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.addr = bt_mesh_shell_target_ctx.dst,
		.app_idx = bt_mesh_shell_target_ctx.app_idx,

	};
	struct bt_mesh_net_tx tx = {
		.ctx = &ctx,
		.src = bt_mesh_primary_addr(),
	};

	size_t len;
	int err;

	len = hex2bin(argv[1], strlen(argv[1]),
		      msg.data, net_buf_simple_tailroom(&msg) - 4);
	net_buf_simple_add(&msg, len);

	err = bt_mesh_trans_send(&tx, &msg, NULL, NULL);
	if (err) {
		shell_error(sh, "Failed to send (err %d)", err);
	}

	return 0;
}

#if defined(CONFIG_BT_MESH_IV_UPDATE_TEST)
static int cmd_iv_update(const struct shell *sh, size_t argc, char *argv[])
{
	if (bt_mesh_iv_update()) {
		shell_print(sh, "Transitioned to IV Update In Progress "
			    "state");
	} else {
		shell_print(sh, "Transitioned to IV Update Normal state");
	}

	shell_print(sh, "IV Index is 0x%08x", bt_mesh.iv_index);

	return 0;
}

static int cmd_iv_update_test(const struct shell *sh, size_t argc,
			      char *argv[])
{
	int err = 0;
	bool enable;

	enable = shell_strtobool(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	if (enable) {
		shell_print(sh, "Enabling IV Update test mode");
	} else {
		shell_print(sh, "Disabling IV Update test mode");
	}

	bt_mesh_iv_update_test(enable);

	return 0;
}
#endif /* CONFIG_BT_MESH_IV_UPDATE_TEST */

static int cmd_rpl_clear(const struct shell *sh, size_t argc, char *argv[])
{
	bt_mesh_rpl_clear();
	return 0;
}

#if defined(CONFIG_BT_MESH_SHELL_HEALTH_SRV_INSTANCE)
static struct bt_mesh_elem *primary_element(void)
{
	const struct bt_mesh_comp *comp = bt_mesh_comp_get();

	if (comp) {
		return &comp->elem[0];
	}

	return NULL;
}

static int cmd_add_fault(const struct shell *sh, size_t argc, char *argv[])
{
	uint8_t fault_id;
	uint8_t i;
	struct bt_mesh_elem *elem;
	int err = 0;

	elem = primary_element();
	if (elem == NULL) {
		shell_print(sh, "Element not found!");
		return -EINVAL;
	}

	fault_id = shell_strtoul(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	if (!fault_id) {
		shell_print(sh, "The Fault ID must be non-zero!");
		return -EINVAL;
	}

	for (i = 0U; i < sizeof(cur_faults); i++) {
		if (!cur_faults[i]) {
			cur_faults[i] = fault_id;
			break;
		}
	}

	if (i == sizeof(cur_faults)) {
		shell_print(sh, "Fault array is full. Use \"del-fault\" to "
			    "clear it");
		return 0;
	}

	for (i = 0U; i < sizeof(reg_faults); i++) {
		if (!reg_faults[i]) {
			reg_faults[i] = fault_id;
			break;
		}
	}

	if (i == sizeof(reg_faults)) {
		shell_print(sh, "No space to store more registered faults");
	}

	bt_mesh_health_srv_fault_update(elem);

	return 0;
}

static int cmd_del_fault(const struct shell *sh, size_t argc, char *argv[])
{
	uint8_t fault_id;
	uint8_t i;
	struct bt_mesh_elem *elem;
	int err = 0;

	elem = primary_element();
	if (elem == NULL) {
		shell_print(sh, "Element not found!");
		return -EINVAL;
	}

	if (argc < 2) {
		(void)memset(cur_faults, 0, sizeof(cur_faults));
		shell_print(sh, "All current faults cleared");
		bt_mesh_health_srv_fault_update(elem);
		return 0;
	}

	fault_id = shell_strtoul(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	if (!fault_id) {
		shell_print(sh, "The Fault ID must be non-zero!");
		return -EINVAL;
	}

	for (i = 0U; i < sizeof(cur_faults); i++) {
		if (cur_faults[i] == fault_id) {
			cur_faults[i] = 0U;
			shell_print(sh, "Fault cleared");
		}
	}

	bt_mesh_health_srv_fault_update(elem);

	return 0;
}
#endif /* CONFIG_BT_MESH_SHELL_HEALTH_SRV_INSTANCE */
#endif /* CONFIG_BT_MESH_SHELL_TEST */

#if defined(CONFIG_BT_MESH_SHELL_CDB)
static int cmd_cdb_create(const struct shell *sh, size_t argc,
			  char *argv[])
{
	uint8_t net_key[16];
	size_t len;
	int err;

	if (argc < 2) {
		bt_rand(net_key, 16);
	} else {
		len = hex2bin(argv[1], strlen(argv[1]), net_key,
			      sizeof(net_key));
		memset(net_key + len, 0, sizeof(net_key) - len);
	}

	err = bt_mesh_cdb_create(net_key);
	if (err < 0) {
		shell_print(sh, "Failed to create CDB (err %d)", err);
	}

	return 0;
}

static int cmd_cdb_clear(const struct shell *sh, size_t argc,
			 char *argv[])
{
	bt_mesh_cdb_clear();

	shell_print(sh, "Cleared CDB");

	return 0;
}

static void cdb_print_nodes(const struct shell *sh)
{
	char key_hex_str[32 + 1], uuid_hex_str[32 + 1];
	struct bt_mesh_cdb_node *node;
	int i, total = 0;
	bool configured;

	shell_print(sh, "Address  Elements  Flags  %-32s  DevKey", "UUID");

	for (i = 0; i < ARRAY_SIZE(bt_mesh_cdb.nodes); ++i) {
		node = &bt_mesh_cdb.nodes[i];
		if (node->addr == BT_MESH_ADDR_UNASSIGNED) {
			continue;
		}

		configured = atomic_test_bit(node->flags,
					     BT_MESH_CDB_NODE_CONFIGURED);

		total++;
		bin2hex(node->uuid, 16, uuid_hex_str, sizeof(uuid_hex_str));
		bin2hex(node->dev_key, 16, key_hex_str, sizeof(key_hex_str));
		shell_print(sh, "0x%04x   %-8d  %-5s  %s  %s", node->addr,
			    node->num_elem, configured ? "C" : "-",
			    uuid_hex_str, key_hex_str);
	}

	shell_print(sh, "> Total nodes: %d", total);
}

static void cdb_print_subnets(const struct shell *sh)
{
	struct bt_mesh_cdb_subnet *subnet;
	char key_hex_str[32 + 1];
	int i, total = 0;

	shell_print(sh, "NetIdx  NetKey");

	for (i = 0; i < ARRAY_SIZE(bt_mesh_cdb.subnets); ++i) {
		subnet = &bt_mesh_cdb.subnets[i];
		if (subnet->net_idx == BT_MESH_KEY_UNUSED) {
			continue;
		}

		total++;
		bin2hex(subnet->keys[0].net_key, 16, key_hex_str,
			sizeof(key_hex_str));
		shell_print(sh, "0x%03x   %s", subnet->net_idx,
			    key_hex_str);
	}

	shell_print(sh, "> Total subnets: %d", total);
}

static void cdb_print_app_keys(const struct shell *sh)
{
	struct bt_mesh_cdb_app_key *app_key;
	char key_hex_str[32 + 1];
	int i, total = 0;

	shell_print(sh, "NetIdx  AppIdx  AppKey");

	for (i = 0; i < ARRAY_SIZE(bt_mesh_cdb.app_keys); ++i) {
		app_key = &bt_mesh_cdb.app_keys[i];
		if (app_key->net_idx == BT_MESH_KEY_UNUSED) {
			continue;
		}

		total++;
		bin2hex(app_key->keys[0].app_key, 16, key_hex_str,
			sizeof(key_hex_str));
		shell_print(sh, "0x%03x   0x%03x   %s",
			    app_key->net_idx, app_key->app_idx, key_hex_str);
	}

	shell_print(sh, "> Total app-keys: %d", total);
}

static int cmd_cdb_show(const struct shell *sh, size_t argc,
			char *argv[])
{
	if (!atomic_test_bit(bt_mesh_cdb.flags, BT_MESH_CDB_VALID)) {
		shell_print(sh, "No valid networks");
		return 0;
	}

	shell_print(sh, "Mesh Network Information");
	shell_print(sh, "========================");

	cdb_print_nodes(sh);
	shell_print(sh, "---");
	cdb_print_subnets(sh);
	shell_print(sh, "---");
	cdb_print_app_keys(sh);

	return 0;
}

static int cmd_cdb_node_add(const struct shell *sh, size_t argc,
			    char *argv[])
{
	struct bt_mesh_cdb_node *node;
	uint8_t uuid[16], dev_key[16];
	uint16_t addr, net_idx;
	uint8_t num_elem;
	size_t len;
	int err = 0;

	len = hex2bin(argv[1], strlen(argv[1]), uuid, sizeof(uuid));
	memset(uuid + len, 0, sizeof(uuid) - len);

	addr = shell_strtoul(argv[2], 0, &err);
	num_elem = shell_strtoul(argv[3], 0, &err);
	net_idx = shell_strtoul(argv[4], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	if (argc < 6) {
		bt_rand(dev_key, 16);
	} else {
		len = hex2bin(argv[5], strlen(argv[5]), dev_key,
			      sizeof(dev_key));
		memset(dev_key + len, 0, sizeof(dev_key) - len);
	}

	node = bt_mesh_cdb_node_alloc(uuid, addr, num_elem, net_idx);
	if (node == NULL) {
		shell_print(sh, "Failed to allocate node");
		return 0;
	}

	memcpy(node->dev_key, dev_key, 16);

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		bt_mesh_cdb_node_store(node);
	}

	shell_print(sh, "Added node 0x%04x", node->addr);

	return 0;
}

static int cmd_cdb_node_del(const struct shell *sh, size_t argc,
			    char *argv[])
{
	struct bt_mesh_cdb_node *node;
	uint16_t addr;
	int err = 0;

	addr = shell_strtoul(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	node = bt_mesh_cdb_node_get(addr);
	if (node == NULL) {
		shell_print(sh, "No node with address 0x%04x", addr);
		return 0;
	}

	bt_mesh_cdb_node_del(node, true);

	shell_print(sh, "Deleted node 0x%04x", addr);

	return 0;
}

static int cmd_cdb_subnet_add(const struct shell *sh, size_t argc,
			     char *argv[])
{
	struct bt_mesh_cdb_subnet *sub;
	uint8_t net_key[16];
	uint16_t net_idx;
	size_t len;
	int err = 0;

	net_idx = shell_strtoul(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	if (argc < 3) {
		bt_rand(net_key, 16);
	} else {
		len = hex2bin(argv[2], strlen(argv[2]), net_key,
			      sizeof(net_key));
		memset(net_key + len, 0, sizeof(net_key) - len);
	}

	sub = bt_mesh_cdb_subnet_alloc(net_idx);
	if (sub == NULL) {
		shell_print(sh, "Could not add subnet");
		return 0;
	}

	memcpy(sub->keys[0].net_key, net_key, 16);

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		bt_mesh_cdb_subnet_store(sub);
	}

	shell_print(sh, "Added Subnet 0x%03x", net_idx);

	return 0;
}

static int cmd_cdb_subnet_del(const struct shell *sh, size_t argc,
			     char *argv[])
{
	struct bt_mesh_cdb_subnet *sub;
	uint16_t net_idx;
	int err = 0;

	net_idx = shell_strtoul(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	sub = bt_mesh_cdb_subnet_get(net_idx);
	if (sub == NULL) {
		shell_print(sh, "No subnet with NetIdx 0x%03x", net_idx);
		return 0;
	}

	bt_mesh_cdb_subnet_del(sub, true);

	shell_print(sh, "Deleted subnet 0x%03x", net_idx);

	return 0;
}

static int cmd_cdb_app_key_add(const struct shell *sh, size_t argc,
			      char *argv[])
{
	struct bt_mesh_cdb_app_key *key;
	uint16_t net_idx, app_idx;
	uint8_t app_key[16];
	size_t len;
	int err = 0;

	net_idx = shell_strtoul(argv[1], 0, &err);
	app_idx = shell_strtoul(argv[2], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	if (argc < 4) {
		bt_rand(app_key, 16);
	} else {
		len = hex2bin(argv[3], strlen(argv[3]), app_key,
			      sizeof(app_key));
		memset(app_key + len, 0, sizeof(app_key) - len);
	}

	key = bt_mesh_cdb_app_key_alloc(net_idx, app_idx);
	if (key == NULL) {
		shell_print(sh, "Could not add AppKey");
		return 0;
	}

	memcpy(key->keys[0].app_key, app_key, 16);

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		bt_mesh_cdb_app_key_store(key);
	}

	shell_print(sh, "Added AppKey 0x%03x", app_idx);

	return 0;
}

static int cmd_cdb_app_key_del(const struct shell *sh, size_t argc,
			      char *argv[])
{
	struct bt_mesh_cdb_app_key *key;
	uint16_t app_idx;
	int err = 0;

	app_idx = shell_strtoul(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	key = bt_mesh_cdb_app_key_get(app_idx);
	if (key == NULL) {
		shell_print(sh, "No AppKey 0x%03x", app_idx);
		return 0;
	}

	bt_mesh_cdb_app_key_del(key, true);

	shell_print(sh, "Deleted AppKey 0x%03x", app_idx);

	return 0;
}
#endif /* CONFIG_BT_MESH_SHELL_CDB */

static int cmd_dst(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	if (argc < 2) {
		shell_print(sh, "Destination address: 0x%04x%s", bt_mesh_shell_target_ctx.dst,
			    bt_mesh_shell_target_ctx.dst == bt_mesh_primary_addr()
				    ? " (local)"
				    : "");
		return 0;
	}

	if (!strcmp(argv[1], "local")) {
		bt_mesh_shell_target_ctx.dst = bt_mesh_primary_addr();
	} else {
		bt_mesh_shell_target_ctx.dst = shell_strtoul(argv[1], 0, &err);
		if (err) {
			shell_warn(sh, "Unable to parse input string argument");
			return err;
		}
	}

	shell_print(sh, "Destination address set to 0x%04x%s", bt_mesh_shell_target_ctx.dst,
		    bt_mesh_shell_target_ctx.dst == bt_mesh_primary_addr() ? " (local)"
										   : "");
	return 0;
}

static int cmd_netidx(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	if (argc < 2) {
		shell_print(sh, "NetIdx: 0x%04x", bt_mesh_shell_target_ctx.net_idx);
		return 0;
	}

	bt_mesh_shell_target_ctx.net_idx = shell_strtoul(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	shell_print(sh, "NetIdx set to 0x%04x", bt_mesh_shell_target_ctx.net_idx);
	return 0;
}

static int cmd_appidx(const struct shell *sh, size_t argc, char *argv[])
{
	int err = 0;

	if (argc < 2) {
		shell_print(sh, "AppIdx: 0x%04x", bt_mesh_shell_target_ctx.app_idx);
		return 0;
	}

	bt_mesh_shell_target_ctx.app_idx = shell_strtoul(argv[1], 0, &err);
	if (err) {
		shell_warn(sh, "Unable to parse input string argument");
		return err;
	}

	shell_print(sh, "AppIdx set to 0x%04x", bt_mesh_shell_target_ctx.app_idx);
	return 0;
}

#if defined(CONFIG_BT_MESH_RPR_CLI) || defined(CONFIG_BT_MESH_PRIV_BEACON_CLI)

static uint8_t str2u8(const char *str)
{
	if (isdigit((unsigned char)str[0])) {
		return strtoul(str, NULL, 0);
	}

	return (!strcmp(str, "on") || !strcmp(str, "enable") ||
		!strcmp(str, "true"));
}

static bool str2bool(const char *str)
{
	return str2u8(str);
}

#endif

static int cmd_comp_change(const struct shell *shell, size_t argc, char *argv[])
{
	bt_mesh_comp_change_prepare();
	return 0;
}

#ifdef CONFIG_BT_MESH_OP_AGG_CLI
static int cmd_op_agg_seq_start(const struct shell *sh, size_t argc, char *argv[])
{
	uint16_t elem_addr;
	int err;

	elem_addr = strtoul(argv[1], NULL, 0);
	bt_mesh_shell_target_ctx.dst = elem_addr;
	shell_print(sh, "mesh dst set to 0x%04x", elem_addr);

	err = bt_mesh_op_agg_cli_seq_start(bt_mesh_shell_target_ctx.net_idx,
					   bt_mesh_shell_target_ctx.app_idx,
					   bt_mesh_shell_target_ctx.dst, elem_addr);
	if (err) {
		shell_error(sh, "Failed to configure Opcodes Aggregator Context (err %d)", err);
	}

	return 0;
}

static int cmd_op_agg_seq_send(const struct shell *sh, size_t argc, char *argv[])
{
	int err;

	err = bt_mesh_op_agg_cli_seq_send();
	if (err) {
		shell_error(sh, "Failed to send Opcodes Aggregator Sequence message (err %d)", err);
	}

	return 0;
}

static int cmd_op_agg_seq_abort(const struct shell *sh, size_t argc, char *argv[])
{
	bt_mesh_op_agg_cli_seq_abort();

	return 0;
}
#endif

#ifdef CONFIG_BT_MESH_LARGE_COMP_DATA_CLI
static int cmd_large_comp_data_get(const struct shell *shell, size_t argc,
				   char *argv[])
{
	NET_BUF_SIMPLE_DEFINE(comp, 64);
	uint8_t page;
	uint16_t offset;
	int err;

	net_buf_simple_init(&comp, 0);

	page = strtoul(argv[1], NULL, 0);
	offset = strtoul(argv[2], NULL, 0);

	err = bt_mesh_large_comp_data_get(bt_mesh_shell_target_ctx.net_idx,
					  bt_mesh_shell_target_ctx.dst, page, offset,
					  &comp);
	if (err) {
		shell_print(
			shell,
			"Failed to send Large Composition Data Get (err=%d)",
			err);
		return err;
	}

	shell_print(shell, "Large Composition Data Get len=%d", comp.len);

	return 0;
}

static int cmd_models_metadata_get(const struct shell *shell, size_t argc,
				   char *argv[])
{
	NET_BUF_SIMPLE_DEFINE(metadata, 64);
	uint8_t page;
	uint16_t offset;
	int err;

	net_buf_simple_init(&metadata, 0);

	page = strtoul(argv[1], NULL, 0);
	offset = strtoul(argv[2], NULL, 0);

	err = bt_mesh_models_metadata_get(bt_mesh_shell_target_ctx.net_idx,
					  bt_mesh_shell_target_ctx.dst, page, offset,
					  &metadata);
	if (err) {
		shell_print(
			shell,
			"Failed to send Models Metadata Get (err=%d)",
			err);
		return err;
	}

	shell_print(shell, "Models Metadata Get len=%d", metadata.len);

	return 0;
}
#endif /* CONFIG_BT_MESH_LARGE_COMP_DATA_CLI */

#if defined(CONFIG_BT_MESH_BLOB_IO_FLASH)
static int cmd_dfu_blob_flash_stream_set(const struct shell *shell, size_t argc, char *argv[])
{
	uint8_t area_id;
	uint32_t offset = 0;
	int err;

	if (argc < 2) {
		return -EINVAL;
	}

	area_id = strtoul(argv[1], NULL, 0);

	if (argc >= 3) {
		offset = strtoul(argv[2], NULL, 0);
	}

	err = bt_mesh_blob_io_flash_init(&blob_flash_stream, area_id, offset);
	if (err) {
		shell_error(shell, "Failed to init BLOB IO Flash module: %d\n", err);
	}

	blob_io = &blob_flash_stream.io;

	shell_print(shell, "Flash stream is initialized with area %u, offset: %u", area_id, offset);

	return 0;
}

static int cmd_dfu_blob_flash_stream_unset(const struct shell *shell, size_t argc, char *argv[])
{
	blob_io = &dummy_blob_io;
	return 0;
}

#endif /* CONFIG_BT_MESH_BLOB_IO_FLASH */

#if defined(CONFIG_BT_MESH_DFU_METADATA)

NET_BUF_SIMPLE_DEFINE_STATIC(dfu_comp_data, BT_MESH_TX_SDU_MAX);

static int cmd_dfu_comp_clear(const struct shell *shell, size_t argc, char *argv[])
{
	net_buf_simple_reset(&dfu_comp_data);
	return 0;
}

static int cmd_dfu_comp_add(const struct shell *shell, size_t argc, char *argv[])
{
	if (argc < 6) {
		return -EINVAL;
	}

	if (net_buf_simple_tailroom(&dfu_comp_data) < 10) {
		shell_print(shell, "Buffer is too small: %u",
			    net_buf_simple_tailroom(&dfu_comp_data));
		return -EMSGSIZE;
	}

	for (size_t i = 1; i <= 5; i++) {
		net_buf_simple_add_le16(&dfu_comp_data, strtoul(argv[i], NULL, 0));
	}

	return 0;
}

static int cmd_dfu_comp_elem_add(const struct shell *shell, size_t argc, char *argv[])
{
	uint8_t sig_model_count;
	uint8_t vnd_model_count;

	if (argc < 5) {
		return -EINVAL;
	}

	sig_model_count = strtoul(argv[2], NULL, 0);
	vnd_model_count = strtoul(argv[3], NULL, 0);

	if (argc < 4 + sig_model_count + vnd_model_count * 2) {
		return -EINVAL;
	}

	if (net_buf_simple_tailroom(&dfu_comp_data) < 4 + sig_model_count * 2 +
	    vnd_model_count * 4) {
		shell_print(shell, "Buffer is too small: %u",
			    net_buf_simple_tailroom(&dfu_comp_data));
		return -EMSGSIZE;
	}

	net_buf_simple_add_le16(&dfu_comp_data, strtoul(argv[1], NULL, 0));
	net_buf_simple_add_u8(&dfu_comp_data, sig_model_count);
	net_buf_simple_add_u8(&dfu_comp_data, vnd_model_count);

	for (size_t i = 0; i < sig_model_count; i++) {
		net_buf_simple_add_le16(&dfu_comp_data, strtoul(argv[4 + i], NULL, 0));
	}

	for (size_t i = 0; i < vnd_model_count; i++) {
		size_t arg_i = 4 + sig_model_count + i * 2;
		net_buf_simple_add_le16(&dfu_comp_data, strtoul(argv[arg_i], NULL, 0));
		net_buf_simple_add_le16(&dfu_comp_data, strtoul(argv[arg_i + 1], NULL, 0));
	}

	return 0;
}

static int cmd_dfu_comp_hash_get(const struct shell *shell, size_t argc, char *argv[])
{
	uint8_t key[16] = {};
	uint32_t hash;
	int err;

	if (dfu_comp_data.len < 14) {
		shell_print(shell, "Composition data is not set");
		return -EINVAL;
	}

	if (argc > 1) {
		hex2bin(argv[1], strlen(argv[1]), key, sizeof(key));
	}

	shell_print(shell, "Composition data to be hashed:");
	shell_print(shell, "\tCID: 0x%04x", sys_get_le16(&dfu_comp_data.data[0]));
	shell_print(shell, "\tPID: 0x%04x", sys_get_le16(&dfu_comp_data.data[2]));
	shell_print(shell, "\tVID: 0x%04x", sys_get_le16(&dfu_comp_data.data[4]));
	shell_print(shell, "\tCPRL: %u", sys_get_le16(&dfu_comp_data.data[6]));
	shell_print(shell, "\tFeatures: 0x%x", sys_get_le16(&dfu_comp_data.data[8]));

	for (size_t i = 10; i < dfu_comp_data.len - 4;) {
		uint8_t sig_model_count = dfu_comp_data.data[i + 2];
		uint8_t vnd_model_count = dfu_comp_data.data[i + 3];

		shell_print(shell, "\tElem: %u", sys_get_le16(&dfu_comp_data.data[i]));
		shell_print(shell, "\t\tNumS: %u", sig_model_count);
		shell_print(shell, "\t\tNumV: %u", vnd_model_count);

		for (size_t j = 0; j < sig_model_count; j++) {
			shell_print(shell, "\t\tSIG Model ID: 0x%04x",
				    sys_get_le16(&dfu_comp_data.data[i + 4 + j * 2]));
		}

		for (size_t j = 0; j < vnd_model_count; j++) {
			size_t arg_i = i + 4 + sig_model_count * 2 + j * 4;
			shell_print(shell, "\t\tVnd Company ID: 0x%04x, Model ID: 0x%04x",
				    sys_get_le16(&dfu_comp_data.data[arg_i]),
				    sys_get_le16(&dfu_comp_data.data[arg_i + 2]));
		}

		i += 4 + sig_model_count * 2 + vnd_model_count * 4;
	}

	err = bt_mesh_dfu_metadata_comp_hash_get(&dfu_comp_data, key, &hash);
	if (err) {
		shell_print(shell, "Failed to compute composition data hash: %d\n", err);
		return err;
	}

	shell_print(shell, "Composition data hash: 0x%04x", hash);

	return 0;
}

static int cmd_dfu_metadata_encode(const struct shell *shell, size_t argc, char *argv[])
{
	char md_str[2 * CONFIG_BT_MESH_DFU_METADATA_MAXLEN + 1];
	NET_BUF_SIMPLE_DEFINE(buf, CONFIG_BT_MESH_DFU_METADATA_MAXLEN);
	uint8_t user_data[CONFIG_BT_MESH_DFU_METADATA_MAXLEN - 18];
	struct bt_mesh_dfu_metadata md;
	size_t len;
	int err;

	if (argc < 9) {
		return -EINVAL;
	}

	md.fw_ver.major = strtoul(argv[1], NULL, 0);
	md.fw_ver.minor = strtoul(argv[2], NULL, 0);
	md.fw_ver.revision = strtoul(argv[3], NULL, 0);
	md.fw_ver.build_num = strtoul(argv[4], NULL, 0);
	md.fw_size = strtoul(argv[5], NULL, 0);
	md.fw_core_type = strtoul(argv[6], NULL, 0);
	md.comp_hash = strtoul(argv[7], NULL, 0);
	md.elems = strtoul(argv[8], NULL, 0);

	if (argc > 9) {
		if (sizeof(user_data) < strlen(argv[9]) / 2) {
			shell_print(shell, "User data is too big.");
			return -EINVAL;
		}

		md.user_data_len = hex2bin(argv[9], strlen(argv[9]), user_data, sizeof(user_data));
		md.user_data = user_data;
	} else {
		md.user_data_len = 0;
	}

	shell_print(shell, "Metadata to be encoded:");
	shell_print(shell, "\tVersion: %u.%u.%u+%u", md.fw_ver.major, md.fw_ver.minor,
		    md.fw_ver.revision, md.fw_ver.build_num);
	shell_print(shell, "\tSize: %u", md.fw_size);
	shell_print(shell, "\tCore Type: 0x%x", md.fw_core_type);
	shell_print(shell, "\tComposition data hash: 0x%x", md.comp_hash);
	shell_print(shell, "\tElements: %u", md.elems);

	if (argc > 9) {
		shell_print(shell, "\tUser data: %s", argv[10]);
	}

	shell_print(shell, "\tUser data length: %u", md.user_data_len);

	err = bt_mesh_dfu_metadata_encode(&md, &buf);
	if (err) {
		shell_print(shell, "Failed to encode metadata: %d", err);
		return err;
	}

	len = bin2hex(buf.data, buf.len, md_str, sizeof(md_str));
	md_str[len] = '\0';
	shell_print(shell, "Encoded metadata: %s", md_str);

	return 0;
}

#endif /* CONFIG_BT_MESH_DFU_METADATA */

#if defined(CONFIG_BT_MESH_DFD_SRV) || defined(CONFIG_BT_MESH_DFU_CLI)

static int cmd_dfu_slot_add(const struct shell *shell, size_t argc,
			    char *argv[])
{
	const struct bt_mesh_dfu_slot *slot;
	size_t size;
	uint8_t fwid[CONFIG_BT_MESH_DFU_FWID_MAXLEN];
	size_t fwid_len = 0;
	uint8_t metadata[CONFIG_BT_MESH_DFU_METADATA_MAXLEN];
	size_t metadata_len = 0;
	const char *uri = "";

	size = strtoul(argv[1], NULL, 0);

	if (argc > 2) {
		fwid_len = hex2bin(argv[2], strlen(argv[2]), fwid,
				   sizeof(fwid));
	}

	if (argc > 3) {
		metadata_len = hex2bin(argv[3], strlen(argv[3]), metadata,
				       sizeof(metadata));
	}

	if (argc > 4) {
		uri = argv[4];
	}

	shell_print(shell, "Adding slot (size: %u)", size);

	slot = bt_mesh_dfu_slot_add(size, fwid, fwid_len, metadata,
				    metadata_len, uri, strlen(uri));
	if (!slot) {
		shell_print(shell, "Failed.");
		return 0;
	}

	bt_mesh_dfu_slot_valid_set(slot, true);

	shell_print(shell, "Slot added. ID: %u", bt_mesh_dfu_slot_idx_get(slot));

	return 0;
}

static int cmd_dfu_slot_del(const struct shell *shell, size_t argc,
			    char *argv[])
{
	const struct bt_mesh_dfu_slot *slot;
	uint8_t idx;
	int err;

	idx = strtoul(argv[1], NULL, 0);
	slot = bt_mesh_dfu_slot_at(idx);
	if (!slot) {
		shell_print(shell, "No slot at %u", idx);
		return 0;
	}

	err = bt_mesh_dfu_slot_del(slot);
	if (err) {
		shell_print(shell, "Failed deleting slot %u (err: %d)", idx,
			    err);
		return 0;
	}

	shell_print(shell, "Slot %u deleted.", idx);
	return 0;
}

static int cmd_dfu_slot_del_all(const struct shell *shell, size_t argc,
			        char *argv[])
{
	int err;

	err = bt_mesh_dfu_slot_del_all();
	if (err) {
		shell_print(shell, "Failed deleting all slots (err: %d)", err);
		return 0;
	}

	shell_print(shell, "All slots deleted.");
	return 0;
}


static int cmd_dfu_slot_get(const struct shell *shell, size_t argc,
			    char *argv[])
{
	const struct bt_mesh_dfu_slot *slot;
	uint8_t idx;

	idx = strtoul(argv[1], NULL, 0);
	slot = bt_mesh_dfu_slot_at(idx);
	if (!slot) {
		shell_print(shell, "No slot at %u", idx);
		return 0;
	}

	slot_info_print(shell, slot, &idx);
	return 0;
}

#endif /* defined(CONFIG_BT_MESH_DFD_SRV) || defined(CONFIG_BT_MESH_DFU_CLI) */

#if !defined(CONFIG_BT_MESH_DFD_SRV)
#if defined(CONFIG_BT_MESH_DFU_CLI)

static struct {
	struct bt_mesh_dfu_target targets[32];
	struct bt_mesh_blob_target_pull pull[32];
	size_t target_cnt;
	struct bt_mesh_blob_cli_inputs inputs;
} dfu_tx;

static void dfu_tx_prepare(void)
{
	sys_slist_init(&dfu_tx.inputs.targets);

	for (size_t i = 0; i < dfu_tx.target_cnt; i++) {
		/* Reset target context. */
		uint16_t addr = dfu_tx.targets[i].blob.addr;
		memset(&dfu_tx.targets[i].blob, 0, sizeof(struct bt_mesh_blob_target));
		memset(&dfu_tx.pull[i], 0, sizeof(struct bt_mesh_blob_target_pull));
		dfu_tx.targets[i].blob.addr = addr;
		dfu_tx.targets[i].blob.pull = &dfu_tx.pull[i];

		sys_slist_append(&dfu_tx.inputs.targets, &dfu_tx.targets[i].blob.n);
	}
}

static int cmd_dfu_target(const struct shell *shell, size_t argc, char *argv[])
{
	uint8_t img_idx;
	uint16_t addr;

	addr = strtoul(argv[1], NULL, 0);
	img_idx = strtoul(argv[2], NULL, 0);

	if (dfu_tx.target_cnt == ARRAY_SIZE(dfu_tx.targets)) {
		shell_print(shell, "No room.");
		return 0;
	}

	for (size_t i = 0; i < dfu_tx.target_cnt; i++) {
		if (dfu_tx.targets[i].blob.addr == addr) {
			shell_print(shell, "Target 0x%04x already exists", addr);
			return 0;
		}
	}

	dfu_tx.targets[dfu_tx.target_cnt].blob.addr = addr;
	dfu_tx.targets[dfu_tx.target_cnt].img_idx = img_idx;
	sys_slist_append(&dfu_tx.inputs.targets, &dfu_tx.targets[dfu_tx.target_cnt].blob.n);
	dfu_tx.target_cnt++;

	shell_print(shell, "Added target 0x%04x", addr);
	return 0;
}

static int cmd_dfu_targets_reset(const struct shell *shell, size_t argc, char *argv[])
{
	dfu_tx_prepare();
	return 0;
}

static int cmd_dfu_target_state(const struct shell *shell, size_t argc,
				char *argv[])
{
	struct bt_mesh_dfu_target_status rsp;
	struct bt_mesh_msg_ctx ctx = {
		.send_ttl = BT_MESH_TTL_DEFAULT,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.addr = bt_mesh_shell_target_ctx.dst,
		.app_idx = bt_mesh_shell_target_ctx.app_idx,
	};
	int err;

	err = bt_mesh_dfu_cli_status_get(&bt_mesh_shell_dfu_cli, &ctx, &rsp);
	if (err) {
		shell_print(shell, "Failed getting target status (err: %d)",
			    err);
		return 0;
	}

	shell_print(shell, "Target 0x%04x:", bt_mesh_shell_target_ctx.dst);
	shell_print(shell, "\tStatus:     %u", rsp.status);
	shell_print(shell, "\tPhase:      %u", rsp.phase);
	if (rsp.phase != BT_MESH_DFU_PHASE_IDLE) {
		shell_print(shell, "\tEffect:       %u", rsp.effect);
		shell_print(shell, "\tImg Idx:      %u", rsp.img_idx);
		shell_print(shell, "\tTTL:          %u", rsp.ttl);
		shell_print(shell, "\tTimeout base: %u", rsp.timeout_base);
	}

	return 0;
}

static enum bt_mesh_dfu_iter dfu_img_cb(struct bt_mesh_dfu_cli *cli,
					struct bt_mesh_msg_ctx *ctx,
					uint8_t idx, uint8_t total,
					const struct bt_mesh_dfu_img *img,
					void *cb_data)
{
	char fwid[2 * CONFIG_BT_MESH_DFU_FWID_MAXLEN + 1];
	size_t len;

	len = bin2hex(img->fwid, img->fwid_len, fwid, sizeof(fwid));
	fwid[len] = '\0';

	shell_print(bt_mesh_shell_ctx_shell, "Image %u:", idx);
	shell_print(bt_mesh_shell_ctx_shell, "\tFWID: %s", fwid);
	if (img->uri) {
		shell_print(bt_mesh_shell_ctx_shell, "\tURI:  %s", img->uri);
	}

	return BT_MESH_DFU_ITER_CONTINUE;
}

static int cmd_dfu_target_imgs(const struct shell *shell, size_t argc,
			       char *argv[])
{
	struct bt_mesh_msg_ctx ctx = {
		.send_ttl = BT_MESH_TTL_DEFAULT,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.addr = bt_mesh_shell_target_ctx.dst,
		.app_idx = bt_mesh_shell_target_ctx.app_idx,
	};
	uint8_t img_cnt = 0xff;
	int err;

	if (argc == 2) {
		img_cnt = strtoul(argv[1], NULL, 0);
	}

	shell_print(shell, "Requesting DFU images in 0x%04x", bt_mesh_shell_target_ctx.dst);

	err = bt_mesh_dfu_cli_imgs_get(&bt_mesh_shell_dfu_cli, &ctx, dfu_img_cb, NULL,
				       img_cnt);
	if (err) {
		shell_print(shell, "Request failed (err: %d)", err);
	}

	return 0;
}

static int cmd_dfu_target_check(const struct shell *shell, size_t argc,
			       char *argv[])
{
	struct bt_mesh_dfu_metadata_status rsp;
	const struct bt_mesh_dfu_slot *slot;
	struct bt_mesh_msg_ctx ctx = {
		.send_ttl = BT_MESH_TTL_DEFAULT,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.addr = bt_mesh_shell_target_ctx.dst,
		.app_idx = bt_mesh_shell_target_ctx.app_idx,
	};
	uint8_t slot_idx, img_idx;
	int err;

	slot_idx = strtoul(argv[1], NULL, 0);

	slot = bt_mesh_dfu_slot_at(slot_idx);
	if (!slot) {
		shell_print(shell, "No image in slot %u", slot_idx);
		return 0;
	}

	img_idx = strtoul(argv[2], NULL, 0);

	err = bt_mesh_dfu_cli_metadata_check(&bt_mesh_shell_dfu_cli, &ctx, img_idx, slot,
					     &rsp);
	if (err) {
		shell_print(shell, "Metadata check failed. err: %d", err);
		return 0;
	}

	shell_print(shell, "Slot %u check for 0x%04x image %u:", slot_idx,
		    bt_mesh_shell_target_ctx.dst, img_idx);
	shell_print(shell, "\tStatus: %u", rsp.status);
	shell_print(shell, "\tEffect: 0x%x", rsp.effect);

	return 0;
}

static int cmd_dfu_send(const struct shell *shell, size_t argc, char *argv[])
{
	struct bt_mesh_dfu_cli_xfer_blob_params blob_params;
	struct bt_mesh_dfu_cli_xfer xfer;
	uint8_t slot_idx;
	uint16_t group;
	int err;

	slot_idx = strtoul(argv[1], NULL, 0);
	if (argc > 2) {
		group = strtoul(argv[2], NULL, 0);
	} else {
		group = BT_MESH_ADDR_UNASSIGNED;
	}

	if (argc > 3) {
		xfer.mode = strtoul(argv[3], NULL, 0);
	} else {
		xfer.mode = BT_MESH_BLOB_XFER_MODE_PUSH;
	}

	if (argc > 5) {
		blob_params.block_size_log = strtoul(argv[4], NULL, 0);
		blob_params.chunk_size = strtoul(argv[5], NULL, 0);
		xfer.blob_params = &blob_params;
	} else {
		xfer.blob_params = NULL;
	}

	if (!dfu_tx.target_cnt) {
		shell_print(shell, "No targets.");
		return 0;
	}

	xfer.slot = bt_mesh_dfu_slot_at(slot_idx);
	if (!xfer.slot) {
		shell_print(shell, "No image in slot %u", slot_idx);
		return 0;
	}

	shell_print(shell, "Starting DFU from slot %u (%u targets)", slot_idx,
		    dfu_tx.target_cnt);

	dfu_tx.inputs.group = group;
	dfu_tx.inputs.app_idx = bt_mesh_shell_target_ctx.app_idx;
	dfu_tx.inputs.ttl = BT_MESH_TTL_DEFAULT;

	err = bt_mesh_dfu_cli_send(&bt_mesh_shell_dfu_cli, &dfu_tx.inputs, blob_io, &xfer);
	if (err) {
		shell_print(shell, "Failed (err: %d)", err);
		return 0;
	}
	return 0;
}

static int cmd_dfu_cancel(const struct shell *shell, size_t argc, char *argv[])
{
	struct bt_mesh_msg_ctx ctx = {
		.send_ttl = BT_MESH_TTL_DEFAULT,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.addr = bt_mesh_shell_target_ctx.dst,
		.app_idx = bt_mesh_shell_target_ctx.app_idx,
	};
	int err;

	if (argc == 2) {
		ctx.addr = strtoul(argv[1], NULL, 0);
		shell_print(shell, "Cancelling DFU for 0x%04x", ctx.addr);
	} else {
		shell_print(shell, "Cancelling DFU");
	}

#if defined(CONFIG_BT_MESH_DFU_SRV)
	bt_mesh_dfu_srv_cancel(&bt_mesh_shell_dfu_srv);
#endif

	err = bt_mesh_dfu_cli_cancel(&bt_mesh_shell_dfu_cli, (argc == 2) ? &ctx : NULL);
	if (err) {
		shell_print(shell, "Failed (err: %d)", err);
	}

	return 0;
}

static int cmd_dfu_apply(const struct shell *shell, size_t argc, char *argv[])
{
	int err;

	shell_print(shell, "Applying DFU");

	err = bt_mesh_dfu_cli_apply(&bt_mesh_shell_dfu_cli);
	if (err) {
		shell_print(shell, "Failed (err: %d)", err);
	}

	return 0;
}

static int cmd_dfu_confirm(const struct shell *shell, size_t argc, char *argv[])
{
	int err;

	shell_print(shell, "Confirming DFU");

	err = bt_mesh_dfu_cli_confirm(&bt_mesh_shell_dfu_cli);
	if (err) {
		shell_print(shell, "Failed (err: %d)", err);
	}

	return 0;
}

static int cmd_dfu_suspend(const struct shell *sh, size_t argc, char *argv[])
{
	int err;

	shell_print(sh, "Suspending DFU");

	err = bt_mesh_dfu_cli_suspend(&bt_mesh_shell_dfu_cli);
	if (err) {
		shell_print(sh, "Failed (err: %d)", err);
	}

	return 0;
}

static int cmd_dfu_resume(const struct shell *sh, size_t argc, char *argv[])
{
	int err;

	shell_print(sh, "Resuming DFU");

	err = bt_mesh_dfu_cli_resume(&bt_mesh_shell_dfu_cli);
	if (err) {
		shell_print(sh, "Failed (err: %d)", err);
	}

	return 0;
}

#elif defined(CONFIG_BT_MESH_BLOB_CLI)

static void blob_cli_inputs_prepare(uint16_t group)
{
	int i;

	blob_cli_xfer.inputs.ttl = BT_MESH_TTL_DEFAULT;
	blob_cli_xfer.inputs.group = group;
	blob_cli_xfer.inputs.app_idx = bt_mesh_shell_target_ctx.app_idx;
	sys_slist_init(&blob_cli_xfer.inputs.targets);

	for (i = 0; i < blob_cli_xfer.target_count; ++i) {
		/* Reset target context. */
		uint16_t addr = blob_cli_xfer.targets[i].addr;
		memset(&blob_cli_xfer.targets[i], 0, sizeof(struct bt_mesh_blob_target));
		memset(&blob_cli_xfer.pull[i], 0, sizeof(struct bt_mesh_blob_target_pull));
		blob_cli_xfer.targets[i].addr = addr;
		blob_cli_xfer.targets[i].pull = &blob_cli_xfer.pull[i];

		sys_slist_append(&blob_cli_xfer.inputs.targets,
				 &blob_cli_xfer.targets[i].n);
	}
}

static int cmd_blob_tx(const struct shell *shell, size_t argc, char *argv[])
{
	uint16_t group;
	int err;

	blob_cli_xfer.xfer.id = strtoul(argv[1], NULL, 0);
	blob_cli_xfer.xfer.size = strtoul(argv[2], NULL, 0);
	blob_cli_xfer.xfer.block_size_log = strtoul(argv[3], NULL, 0);
	blob_cli_xfer.xfer.chunk_size = strtoul(argv[4], NULL, 0);

	if (argc >= 6) {
		group = strtoul(argv[5], NULL, 0);
	} else {
		group = BT_MESH_ADDR_UNASSIGNED;
	}

	if (argc < 7 || !strcmp(argv[6], "push")) {
		blob_cli_xfer.xfer.mode = BT_MESH_BLOB_XFER_MODE_PUSH;
	} else if (!strcmp(argv[6], "pull")) {
		blob_cli_xfer.xfer.mode = BT_MESH_BLOB_XFER_MODE_PULL;
	} else {
		shell_print(shell, "Mode must be either push or pull");
		return -EINVAL;
	}

	if (argc >= 8) {
		blob_cli_xfer.inputs.timeout_base = strtoul(argv[7], NULL, 0);
	} else {
		blob_cli_xfer.inputs.timeout_base = 0;
	}

	if (!blob_cli_xfer.target_count) {
		shell_print(shell, "Failed: No targets");
		return 0;
	}

	blob_cli_inputs_prepare(group);

	shell_print(shell,
		    "Sending transfer 0x%x (mode: %s, %u bytes) to 0x%04x",
		    (uint32_t)blob_cli_xfer.xfer.id,
		    blob_cli_xfer.xfer.mode == BT_MESH_BLOB_XFER_MODE_PUSH ?
			    "push" :
			    "pull",
		    blob_cli_xfer.xfer.size, group);

	err = bt_mesh_blob_cli_send(&bt_mesh_shell_blob_cli, &blob_cli_xfer.inputs,
				    &blob_cli_xfer.xfer, blob_io);
	if (err) {
		shell_print(shell, "BLOB transfer TX failed (err: %d)", err);
	}

	return 0;
}

static int cmd_blob_target(const struct shell *shell, size_t argc, char *argv[])
{
	struct bt_mesh_blob_target *t;

	if (blob_cli_xfer.target_count ==
	    ARRAY_SIZE(blob_cli_xfer.targets)) {
		shell_print(shell, "No more room");
		return 0;
	}

	t = &blob_cli_xfer.targets[blob_cli_xfer.target_count];

	t->addr = strtoul(argv[1], NULL, 0);

	shell_print(shell, "Added target 0x%04x", t->addr);

	blob_cli_xfer.target_count++;
	return 0;
}

static int cmd_blob_caps(const struct shell *shell, size_t argc, char *argv[])
{
	uint16_t group;
	int err;

	shell_print(shell, "Retrieving transfer capabilities...");

	if (argc > 1) {
		group = strtoul(argv[1], NULL, 0);
	} else {
		group = BT_MESH_ADDR_UNASSIGNED;
	}

	if (argc > 2) {
		blob_cli_xfer.inputs.timeout_base = strtoul(argv[2], NULL, 0);
	} else {
		blob_cli_xfer.inputs.timeout_base = 0;
	}

	if (!blob_cli_xfer.target_count) {
		shell_print(shell, "Failed: No targets");
		return 0;
	}

	blob_cli_inputs_prepare(group);

	err = bt_mesh_blob_cli_caps_get(&bt_mesh_shell_blob_cli, &blob_cli_xfer.inputs);
	if (err) {
		shell_print(shell, "Boundary check start failed (err: %d)",
			    err);
	}

	return 0;
}

static int cmd_blob_tx_cancel(const struct shell *shell, size_t argc,
			      char *argv[])
{
	shell_print(shell, "Cancelling transfer");
	bt_mesh_blob_cli_cancel(&bt_mesh_shell_blob_cli);
	return 0;
}

static int cmd_blob_tx_suspend(const struct shell *shell, size_t argc,
			       char *argv[])
{
	shell_print(shell, "Suspending transfer");
	bt_mesh_blob_cli_suspend(&bt_mesh_shell_blob_cli);
	return 0;
}

static int cmd_blob_tx_resume(const struct shell *shell, size_t argc,
			      char *argv[])
{
	shell_print(shell, "Resuming transfer");
	bt_mesh_blob_cli_resume(&bt_mesh_shell_blob_cli);
	return 0;
}

#endif /* CONFIG_BT_MESH_BLOB_CLI */

#if defined(CONFIG_BT_MESH_BLOB_SRV)

static int cmd_blob_rx(const struct shell *shell, size_t argc, char *argv[])
{
#if defined(CONFIG_BT_MESH_DFU_SRV)
	struct bt_mesh_blob_srv *srv = &bt_mesh_shell_dfu_srv.blob;
#else
	struct bt_mesh_blob_srv *srv = &bt_mesh_shell_blob_srv;
#endif
	uint16_t timeout_base;
	uint32_t id;
	int err;

	id = strtoul(argv[1], NULL, 0);
	blob_rx_sum = 0;

	if (argc > 2) {
		timeout_base = strtoul(argv[2], NULL, 0);
	} else {
		timeout_base = 0U;
	}

	shell_print(shell, "Receive BLOB 0x%x", id);
	err = bt_mesh_blob_srv_recv(srv, id, blob_io, BT_MESH_TTL_MAX,
				    timeout_base);
	if (err) {
		shell_print(shell, "BLOB RX setup failed (%d)", err);
	}

	return 0;
}

static int cmd_blob_rx_cancel(const struct shell *shell, size_t argc,
			      char *argv[])
{
#if defined(CONFIG_BT_MESH_DFU_SRV)
	struct bt_mesh_blob_srv *srv = &bt_mesh_shell_dfu_srv.blob;
#else
	struct bt_mesh_blob_srv *srv = &bt_mesh_shell_blob_srv;
#endif
	int err;

	shell_print(shell, "Cancelling BLOB rx");
	err = bt_mesh_blob_srv_cancel(srv);
	if (err) {
		shell_print(shell, "BLOB cancel failed (%d)", err);
	}

	return 0;
}
#endif /* CONFIG_BT_MESH_BLOB_SRV */

#if defined(CONFIG_BT_MESH_DFU_SRV)
static int cmd_dfu_applied(const struct shell *shell, size_t argc, char *argv[])
{
	bt_mesh_dfu_srv_applied(&bt_mesh_shell_dfu_srv);
	return 0;
}

static int cmd_dfu_rx_cancel(const struct shell *shell, size_t argc, char *argv[])
{
	bt_mesh_dfu_srv_cancel(&bt_mesh_shell_dfu_srv);
	return 0;
}
#endif

#if defined(CONFIG_BT_MESH_DFU_CLI) || defined(CONFIG_BT_MESH_DFU_SRV)
static int cmd_dfu_progress(const struct shell *shell, size_t argc,
			    char *argv[])
{
	shell_print(shell, "DFU progress:");

#if defined(CONFIG_BT_MESH_DFU_SRV)
	shell_print(shell, "\tServer: %u %%",
		    bt_mesh_dfu_srv_progress(&bt_mesh_shell_dfu_srv));
#endif
#if defined(CONFIG_BT_MESH_DFD_SRV)
#elif defined(CONFIG_BT_MESH_DFU_CLI)
	shell_print(shell, "\tClient: %u %%",
		    bt_mesh_dfu_cli_progress(&bt_mesh_shell_dfu_cli));
#endif

	return 0;
}
#endif
#endif /* !defined(CONFIG_BT_MESH_DFD_SRV) */


#if defined(CONFIG_BT_MESH_RPR_CLI)
static int cmd_rpr_scan(const struct shell *shell, size_t argc, char *argv[])
{
	struct bt_mesh_rpr_scan_status rsp;
	const struct bt_mesh_rpr_node srv = {
		.addr = bt_mesh_shell_target_ctx.dst,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	uint8_t uuid[16] = {0};
	int err;

	if (argc > 2) {
		hex2bin(argv[2], strlen(argv[2]), uuid, 16);
	}

	err = bt_mesh_rpr_scan_start(&bt_mesh_shell_rpr_cli, &srv, argc > 2 ? uuid : NULL,
				     strtoul(argv[1], NULL, 0),
				     BT_MESH_RPR_SCAN_MAX_DEVS_ANY, &rsp);
	if (err) {
		shell_print(shell, "Scan start failed: %d", err);
		return err;
	}

	if (rsp.status == BT_MESH_RPR_SUCCESS) {
		shell_print(shell, "Scan started.");
	} else {
		shell_print(shell, "Scan start response: %d", rsp.status);
	}

	return 0;
}

static int cmd_rpr_scan_ext(const struct shell *shell, size_t argc,
			    char *argv[])
{
	const struct bt_mesh_rpr_node srv = {
		.addr = bt_mesh_shell_target_ctx.dst,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	uint8_t ad_types[CONFIG_BT_MESH_RPR_AD_TYPES_MAX];
	uint8_t uuid[16] = {0};
	int i, err;

	hex2bin(argv[2], strlen(argv[2]), uuid, 16);

	for (i = 0; i < argc - 3; i++) {
		ad_types[i] = strtoul(argv[3 + i], NULL, 0);
	}

	err = bt_mesh_rpr_scan_start_ext(&bt_mesh_shell_rpr_cli, &srv, uuid,
					 strtoul(argv[1], NULL, 0), ad_types,
					 (argc - 3));
	if (err) {
		shell_print(shell, "Scan start failed: %d", err);
		return err;
	}

	shell_print(shell, "Extended scan started.");

	return 0;
}

static int cmd_rpr_scan_srv(const struct shell *shell, size_t argc,
			    char *argv[])
{
	const struct bt_mesh_rpr_node srv = {
		.addr = bt_mesh_shell_target_ctx.dst,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	uint8_t ad_types[CONFIG_BT_MESH_RPR_AD_TYPES_MAX];
	int i, err;

	for (i = 0; i < argc - 1; i++) {
		ad_types[i] = strtoul(argv[1 + i], NULL, 0);
	}

	err = bt_mesh_rpr_scan_start_ext(&bt_mesh_shell_rpr_cli, &srv, NULL, 0, ad_types,
					 (argc - 1));
	if (err) {
		shell_print(shell, "Scan start failed: %d", err);
		return err;
	}

	return 0;
}

static int cmd_rpr_scan_caps(const struct shell *shell, size_t argc,
			    char *argv[])
{
	struct bt_mesh_rpr_caps caps;
	const struct bt_mesh_rpr_node srv = {
		.addr = bt_mesh_shell_target_ctx.dst,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_rpr_scan_caps_get(&bt_mesh_shell_rpr_cli, &srv, &caps);
	if (err) {
		shell_print(shell, "Scan capabilities get failed: %d", err);
		return err;
	}

	shell_print(shell, "Remote Provisioning scan capabilities of 0x%04x:",
		    bt_mesh_shell_target_ctx.dst);
	shell_print(shell, "\tMax devices:     %u", caps.max_devs);
	shell_print(shell, "\tActive scanning: %s",
		    caps.active_scan ? "true" : "false");
	return 0;
}

static int cmd_rpr_scan_get(const struct shell *shell, size_t argc,
			    char *argv[])
{
	struct bt_mesh_rpr_scan_status rsp;
	const struct bt_mesh_rpr_node srv = {
		.addr = bt_mesh_shell_target_ctx.dst,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_rpr_scan_get(&bt_mesh_shell_rpr_cli, &srv, &rsp);
	if (err) {
		shell_print(shell, "Scan get failed: %d", err);
		return err;
	}

	shell_print(shell, "Remote Provisioning scan on 0x%04x:", bt_mesh_shell_target_ctx.dst);
	shell_print(shell, "\tStatus:         %u", rsp.status);
	shell_print(shell, "\tScan type:      %u", rsp.scan);
	shell_print(shell, "\tMax devices:    %u", rsp.max_devs);
	shell_print(shell, "\tRemaining time: %u", rsp.timeout);
	return 0;
}

static int cmd_rpr_scan_stop(const struct shell *shell, size_t argc,
			    char *argv[])
{
	struct bt_mesh_rpr_scan_status rsp;
	const struct bt_mesh_rpr_node srv = {
		.addr = bt_mesh_shell_target_ctx.dst,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_rpr_scan_stop(&bt_mesh_shell_rpr_cli, &srv, &rsp);
	if (err || rsp.status) {
		shell_print(shell, "Scan stop failed: %d %u", err, rsp.status);
		return err;
	}

	shell_print(shell, "Remote Provisioning scan on 0x%04x stopped.",
		    bt_mesh_shell_target_ctx.dst);
	return 0;
}

static int cmd_rpr_link_get(const struct shell *shell, size_t argc,
			    char *argv[])
{
	struct bt_mesh_rpr_link rsp;
	const struct bt_mesh_rpr_node srv = {
		.addr = bt_mesh_shell_target_ctx.dst,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_rpr_link_get(&bt_mesh_shell_rpr_cli, &srv, &rsp);
	if (err) {
		shell_print(shell, "Link get failed: %d %u", err, rsp.status);
		return err;
	}

	shell_print(shell, "Remote Provisioning Link on 0x%04x:", bt_mesh_shell_target_ctx.dst);
	shell_print(shell, "\tStatus: %u", rsp.status);
	shell_print(shell, "\tState:  %u", rsp.state);
	return 0;
}

static int cmd_rpr_link_close(const struct shell *shell, size_t argc,
			    char *argv[])
{
	struct bt_mesh_rpr_link rsp;
	const struct bt_mesh_rpr_node srv = {
		.addr = bt_mesh_shell_target_ctx.dst,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_rpr_link_close(&bt_mesh_shell_rpr_cli, &srv, &rsp);
	if (err) {
		shell_print(shell, "Link close failed: %d %u", err, rsp.status);
		return err;
	}

	shell_print(shell, "Remote Provisioning Link on 0x%04x:", bt_mesh_shell_target_ctx.dst);
	shell_print(shell, "\tStatus: %u", rsp.status);
	shell_print(shell, "\tState:  %u", rsp.state);
	return 0;
}

static int cmd_provision_remote(const struct shell *shell, size_t argc,
				char *argv[])
{
	struct bt_mesh_rpr_node srv = {
		.addr = bt_mesh_shell_target_ctx.dst,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	uint8_t uuid[16];
	size_t len;
	int err;

	len = hex2bin(argv[1], strlen(argv[1]), uuid, sizeof(uuid));
	(void)memset(uuid + len, 0, sizeof(uuid) - len);

	err = bt_mesh_provision_remote(&bt_mesh_shell_rpr_cli, &srv, uuid,
				       strtoul(argv[2], NULL, 0),
				       strtoul(argv[3], NULL, 0));
	if (err) {
		shell_print(shell, "Prov remote start failed: %d", err);
	}

	return err;
}

static int cmd_reprovision_remote(const struct shell *shell, size_t argc,
				  char *argv[])
{
	struct bt_mesh_rpr_node srv = {
		.addr = bt_mesh_shell_target_ctx.dst,
		.net_idx = bt_mesh_shell_target_ctx.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	bool composition_changed;
	uint16_t addr;
	int err;

	addr = strtoul(argv[1], NULL, 0);
	if (!BT_MESH_ADDR_IS_UNICAST(addr)) {
		shell_print(shell, "Must be a valid unicast address");
		return -EINVAL;
	}

	composition_changed = (argc > 2 && str2bool(argv[2]));

	err = bt_mesh_reprovision_remote(&bt_mesh_shell_rpr_cli, &srv, addr,
					 composition_changed);
	if (err) {
		shell_print(shell, "Reprovisioning failed: %d", err);
	}

	return 0;
}

#endif

#if defined(CONFIG_BT_MESH_SAR_CFG_CLI)

struct bt_mesh_sar_cfg_cli bt_mesh_shell_sar_cfg_cli;

static int cmd_sar_tx_get(const struct shell *shell, size_t argc, char *argv[])
{
	struct bt_mesh_sar_tx rsp;
	int err;

	err = bt_mesh_sar_cfg_cli_transmitter_get(&bt_mesh_shell_sar_cfg_cli, bt_mesh_shell_target_ctx.net_idx,
						  bt_mesh_shell_target_ctx.dst, &rsp);
	if (err) {
		shell_error(shell,
			    "Failed to send SAR Transmitter Get (err %d)", err);
		return 0;
	}

	shell_print(shell, "Transmitter Get: %u %u %u %u %u %u %u",
		    rsp.seg_int_step, rsp.unicast_retrans_count,
		    rsp.unicast_retrans_without_prog_count,
		    rsp.unicast_retrans_int_step, rsp.unicast_retrans_int_inc,
		    rsp.multicast_retrans_count, rsp.multicast_retrans_int);

	return 0;
}

static int cmd_sar_tx_set(const struct shell *shell, size_t argc, char *argv[])
{
	struct bt_mesh_sar_tx set, rsp;
	int err;

	set.seg_int_step = strtoul(argv[1], NULL, 0);
	set.unicast_retrans_count = strtoul(argv[2], NULL, 0);
	set.unicast_retrans_without_prog_count = strtoul(argv[3], NULL, 0);
	set.unicast_retrans_int_step = strtoul(argv[4], NULL, 0);
	set.unicast_retrans_int_inc = strtoul(argv[5], NULL, 0);
	set.multicast_retrans_count = strtoul(argv[6], NULL, 0);
	set.multicast_retrans_int = strtoul(argv[7], NULL, 0);

	err = bt_mesh_sar_cfg_cli_transmitter_set(&bt_mesh_shell_sar_cfg_cli,
						  bt_mesh_shell_target_ctx.net_idx,
						  bt_mesh_shell_target_ctx.dst, &set, &rsp);
	if (err) {
		shell_error(shell,
			    "Failed to send SAR Transmitter Set (err %d)", err);
		return 0;
	}

	shell_print(shell, "Transmitter Set: %u %u %u %u %u %u %u",
		    rsp.seg_int_step, rsp.unicast_retrans_count,
		    rsp.unicast_retrans_without_prog_count,
		    rsp.unicast_retrans_int_step, rsp.unicast_retrans_int_inc,
		    rsp.multicast_retrans_count, rsp.multicast_retrans_int);

	return 0;
}

static int cmd_sar_rx_get(const struct shell *shell, size_t argc, char *argv[])
{
	struct bt_mesh_sar_rx rsp;
	int err;

	err = bt_mesh_sar_cfg_cli_receiver_get(&bt_mesh_shell_sar_cfg_cli,
					       bt_mesh_shell_target_ctx.net_idx,
					       bt_mesh_shell_target_ctx.dst, &rsp);
	if (err) {
		shell_error(shell, "Failed to send SAR Receiver Get (err %d)",
			    err);
		return 0;
	}

	shell_print(shell, "Receiver Get: %u %u %u %u %u", rsp.seg_thresh,
		    rsp.ack_delay_inc, rsp.ack_retrans_count,
		    rsp.discard_timeout, rsp.rx_seg_int_step);

	return 0;
}

static int cmd_sar_rx_set(const struct shell *shell, size_t argc, char *argv[])
{
	struct bt_mesh_sar_rx set, rsp;
	int err;

	set.seg_thresh = strtoul(argv[1], NULL, 0);
	set.ack_delay_inc = strtoul(argv[2], NULL, 0);
	set.ack_retrans_count = strtoul(argv[3], NULL, 0);
	set.discard_timeout = strtoul(argv[4], NULL, 0);
	set.rx_seg_int_step = strtoul(argv[5], NULL, 0);

	err = bt_mesh_sar_cfg_cli_receiver_set(&bt_mesh_shell_sar_cfg_cli,
					       bt_mesh_shell_target_ctx.net_idx,
					       bt_mesh_shell_target_ctx.dst, &set, &rsp);
	if (err) {
		shell_error(shell, "Failed to send SAR Receiver Set (err %d)",
			    err);
		return 0;
	}

	shell_print(shell, "Receiver Set: %u %u %u %u %u", rsp.seg_thresh,
		    rsp.ack_delay_inc, rsp.ack_retrans_count,
		    rsp.discard_timeout, rsp.rx_seg_int_step);

	return 0;
}
#endif

#ifdef CONFIG_BT_MESH_PRIV_BEACON_CLI
struct bt_mesh_priv_beacon_cli bt_mesh_shell_priv_beacon_cli;

static int cmd_priv_beacon_get(const struct shell *sh, size_t argc, char *argv[])
{
	struct bt_mesh_priv_beacon val;
	int err;

	err = bt_mesh_priv_beacon_cli_get(&bt_mesh_shell_priv_beacon_cli,
					  bt_mesh_shell_target_ctx.net_idx,
					  bt_mesh_shell_target_ctx.dst,
					  &val);
	if (err) {
		shell_error(sh, "Failed to send Private Beacon Get (err %d)", err);
		return 0;
	}

	shell_print(sh, "Private Beacon state: %u, %u", val.enabled, val.rand_interval);

	return 0;
}

static int cmd_priv_beacon_set(const struct shell *sh, size_t argc, char *argv[])
{
	struct bt_mesh_priv_beacon val;
	int err;

	val.enabled = str2bool(argv[1]);
	val.rand_interval = strtoul(argv[2], NULL, 0);

	err = bt_mesh_priv_beacon_cli_set(&bt_mesh_shell_priv_beacon_cli,
					  bt_mesh_shell_target_ctx.net_idx,
					  bt_mesh_shell_target_ctx.dst,
					  &val);
	if (err) {
		shell_error(sh, "Failed to send Private Beacon Set (err %d)", err);
		return 0;
	}

	return 0;
}

static int cmd_priv_gatt_proxy_get(const struct shell *sh, size_t argc, char *argv[])
{
	uint8_t state;
	int err;

	err = bt_mesh_priv_beacon_cli_gatt_proxy_get(&bt_mesh_shell_priv_beacon_cli,
						     bt_mesh_shell_target_ctx.net_idx,
						     bt_mesh_shell_target_ctx.dst, &state);
	if (err) {
		shell_error(sh, "Failed to send Private GATT Proxy Get (err %d)", err);
		return 0;
	}

	shell_print(sh, "Private GATT Proxy state: %u", state);

	return 0;
}

static int cmd_priv_gatt_proxy_set(const struct shell *sh, size_t argc, char *argv[])
{
	uint8_t state;
	int err;

	state = str2u8(argv[1]);

	err = bt_mesh_priv_beacon_cli_gatt_proxy_set(&bt_mesh_shell_priv_beacon_cli,
						     bt_mesh_shell_target_ctx.net_idx,
						     bt_mesh_shell_target_ctx.dst, &state);
	if (err) {
		shell_error(sh, "Failed to send Private GATT Proxy Set (err %d)", err);
		return 0;
	}

	return 0;
}

static int cmd_priv_node_id_get(const struct shell *sh, size_t argc, char *argv[])
{
	struct bt_mesh_priv_node_id val;
	uint16_t key_net_idx;
	int err;

	key_net_idx = strtoul(argv[1], NULL, 0);

	err = bt_mesh_priv_beacon_cli_node_id_get(&bt_mesh_shell_priv_beacon_cli,
						  bt_mesh_shell_target_ctx.net_idx,
						  bt_mesh_shell_target_ctx.dst, key_net_idx, &val);
	if (err) {
		shell_error(sh, "Failed to send Private Node Identity Get (err %d)", err);
		return 0;
	}

	shell_print(sh, "Private Node Identity state: %u %u %u", val.net_idx, val.state,
		    val.status);

	return 0;
}

static int cmd_priv_node_id_set(const struct shell *sh, size_t argc, char *argv[])
{
	struct bt_mesh_priv_node_id val;
	int err;

	val.net_idx = strtoul(argv[1], NULL, 0);
	val.state = strtoul(argv[2], NULL, 0);

	err = bt_mesh_priv_beacon_cli_node_id_set(&bt_mesh_shell_priv_beacon_cli,
						  bt_mesh_shell_target_ctx.net_idx,
						  bt_mesh_shell_target_ctx.dst, &val);
	if (err) {
		shell_error(sh, "Failed to send Private Node Identity Set (err %d)", err);
		return 0;
	}

	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(mesh_11_cmds,
#ifdef CONFIG_BT_MESH_LARGE_COMP_DATA_CLI
	SHELL_CMD_ARG(large-comp-data-get, NULL, NULL, cmd_large_comp_data_get,
		      3, 0),
	SHELL_CMD_ARG(models-metadata-get, NULL, NULL, cmd_models_metadata_get,
		      3, 0),
#endif

#if defined(CONFIG_BT_MESH_BLOB_IO_FLASH)
	SHELL_CMD_ARG(blob-flash-stream-set, NULL, "<area id> [<offset>]",
		      cmd_dfu_blob_flash_stream_set, 2, 1),
	SHELL_CMD_ARG(blob-flash-stream-unset, NULL, NULL, cmd_dfu_blob_flash_stream_unset, 1, 0),
#endif

#if defined(CONFIG_BT_MESH_DFU_METADATA)
	SHELL_CMD_ARG(dfu-comp-clear, NULL, NULL, cmd_dfu_comp_clear, 1, 0),
	SHELL_CMD_ARG(dfu-comp-add, NULL, "<cid> <pid> <vid> <crpl> <features>",
		      cmd_dfu_comp_add, 6, 0),
	SHELL_CMD_ARG(dfu-comp-elem-add, NULL, "<loc> <nums> <numv> "
		      "{<sig model id>|<vnd company id> <vnd model id>}...",
		      cmd_dfu_comp_elem_add, 5, 10),
	SHELL_CMD_ARG(dfu-comp-hash-get, NULL, "[<128-bit key>]", cmd_dfu_comp_hash_get, 1, 1),
	SHELL_CMD_ARG(dfu-metadata-encode, NULL, "<major> <minor> <rev> <build_num> <size> "
		      "<core type> <hash> <elems> [<user data>]",
		      cmd_dfu_metadata_encode, 9, 1),
#endif

#if defined(CONFIG_BT_MESH_DFD_SRV) || defined(CONFIG_BT_MESH_DFU_CLI)
	SHELL_CMD_ARG(dfu-slot-add, NULL,
		      "<size> [<fwid> [<metadata> [<uri>]]]",
		      cmd_dfu_slot_add, 2, 3),
	SHELL_CMD_ARG(dfu-slot-del, NULL, "<slot idx>", cmd_dfu_slot_del, 2, 0),
	SHELL_CMD_ARG(dfu-slot-del-all, NULL, NULL, cmd_dfu_slot_del_all, 1, 0),
	SHELL_CMD_ARG(dfu-slot-get, NULL, "<slot idx>", cmd_dfu_slot_get, 2, 0),
#endif

#if defined(CONFIG_BT_MESH_DFD_SRV)
	SHELL_CMD_ARG(dfd-receivers-add, NULL, "<addr>,<fw_idx>[;<addr>,<fw_idx>]...",
		      cmd_dfd_receivers_add, 2, 0),
	SHELL_CMD_ARG(dfd-receivers-delete-all, NULL, NULL, cmd_dfd_receivers_delete_all, 1, 0),
	SHELL_CMD_ARG(dfd-receivers-get, NULL, "<first> <count>", cmd_dfd_receivers_get, 3, 0),
	SHELL_CMD_ARG(dfd-capabilities-get, NULL, NULL, cmd_dfd_capabilities_get, 1, 0),
	SHELL_CMD_ARG(dfd-get, NULL, NULL, cmd_dfd_get, 1, 0),
	SHELL_CMD_ARG(dfd-start, NULL,
		      "<app_idx> <slot_idx> [<group> [<policy_apply> [<ttl> "
		      "[<timeout_base> [<xfer_mode>]]]]]",
		      cmd_dfd_start, 3, 5),
	SHELL_CMD_ARG(dfd-suspend, NULL, NULL, cmd_dfd_suspend, 1, 0),
	SHELL_CMD_ARG(dfd-cancel, NULL, NULL, cmd_dfd_cancel, 1, 0),
	SHELL_CMD_ARG(dfd-apply, NULL, NULL, cmd_dfd_apply, 1, 0),
	SHELL_CMD_ARG(dfd-fw-get, NULL, "<fwid>", cmd_dfd_fw_get, 2, 0),
	SHELL_CMD_ARG(dfd-fw-get-by-idx, NULL, "<idx>", cmd_dfd_fw_get_by_idx, 2, 0),
	SHELL_CMD_ARG(dfd-fw-delete, NULL, "<fwid>", cmd_dfd_fw_delete, 2, 0),
	SHELL_CMD_ARG(dfd-fw-delete-all, NULL, NULL, cmd_dfd_fw_delete_all, 1, 0),
	SHELL_CMD_ARG(dfd-instance-set, NULL, "<elem_idx>", cmd_dfd_instance_set, 2, 0),
	SHELL_CMD_ARG(dfd-instance-get-all, NULL, NULL, cmd_dfd_instance_get_all, 1, 0),
#else
#if defined(CONFIG_BT_MESH_DFU_CLI)
	/* DFU Client Model Operations */
	SHELL_CMD_ARG(dfu-target, NULL, "<addr> <img idx>", cmd_dfu_target, 3,
		      0),
	SHELL_CMD_ARG(dfu-targets-reset, NULL, NULL, cmd_dfu_targets_reset, 1, 0),
	SHELL_CMD_ARG(dfu-target-state, NULL, NULL, cmd_dfu_target_state, 1, 0),
	SHELL_CMD_ARG(dfu-target-imgs, NULL, "[<max count>]",
		      cmd_dfu_target_imgs, 1, 1),
	SHELL_CMD_ARG(dfu-target-check, NULL, "<slot idx> <target img idx>",
		      cmd_dfu_target_check, 3, 0),
	SHELL_CMD_ARG(dfu-send, NULL, "<slot idx>  [<group> "
		      "[<mode: push, pull> [<block size log> <chunk size>]]]", cmd_dfu_send, 2, 4),
	SHELL_CMD_ARG(dfu-cancel, NULL, "[<addr>]", cmd_dfu_cancel, 1, 1),
	SHELL_CMD_ARG(dfu-apply, NULL, NULL, cmd_dfu_apply, 0, 0),
	SHELL_CMD_ARG(dfu-confirm, NULL, NULL, cmd_dfu_confirm, 0, 0),
	SHELL_CMD_ARG(dfu-suspend, NULL, NULL, cmd_dfu_suspend, 0, 0),
	SHELL_CMD_ARG(dfu-resume, NULL, NULL, cmd_dfu_resume, 0, 0),
#elif defined(CONFIG_BT_MESH_BLOB_CLI)
	/* BLOB Client Model Operations */
	SHELL_CMD_ARG(blob-target, NULL, "<addr>", cmd_blob_target, 2, 0),
	SHELL_CMD_ARG(blob-caps, NULL, "[<group> [<timeout base>]]", cmd_blob_caps, 1, 2),
	SHELL_CMD_ARG(blob-tx, NULL, "<id> <size> <block size log> "
		      "<chunk size> [<group> [<mode: push, pull> "
		      "[<timeout base>]]]", cmd_blob_tx, 5, 3),
	SHELL_CMD_ARG(blob-tx-cancel, NULL, NULL, cmd_blob_tx_cancel, 1, 0),
	SHELL_CMD_ARG(blob-tx-suspend, NULL, NULL, cmd_blob_tx_suspend, 1, 0),
	SHELL_CMD_ARG(blob-tx-resume, NULL, NULL, cmd_blob_tx_resume, 1, 0),
#endif

#if defined(CONFIG_BT_MESH_DFU_SRV)
	SHELL_CMD_ARG(dfu-applied, NULL, NULL, cmd_dfu_applied, 1, 0),
	SHELL_CMD_ARG(dfu-rx-cancel, NULL, NULL, cmd_dfu_rx_cancel, 1, 0),
#endif

#if defined(CONFIG_BT_MESH_BLOB_SRV)
	/* BLOB Server Model Operations */
	SHELL_CMD_ARG(blob-rx, NULL, "<id> [<timeout base>]", cmd_blob_rx, 2, 1),
	SHELL_CMD_ARG(blob-rx-cancel, NULL, NULL, cmd_blob_rx_cancel, 1, 0),
#endif
#if defined(CONFIG_BT_MESH_DFU_CLI) || defined(CONFIG_BT_MESH_DFU_SRV)
	SHELL_CMD_ARG(dfu-progress, NULL, NULL, cmd_dfu_progress, 1, 0),
#endif
#endif /* !defined(CONFIG_BT_MESH_DFD_SRV) */

#if defined(CONFIG_BT_MESH_RPR_CLI)
	SHELL_CMD_ARG(rpr-scan, NULL, "<timeout in seconds> [<UUID>]",
		      cmd_rpr_scan, 2, 1),
	SHELL_CMD_ARG(rpr-scan-ext, NULL,
		      "<timeout in seconds> <UUID> [<AD-type> ... ]",
		      cmd_rpr_scan_ext, 3, CONFIG_BT_MESH_RPR_AD_TYPES_MAX),
	SHELL_CMD_ARG(rpr-scan-srv, NULL, "[<AD-type> ... ]",
		      cmd_rpr_scan_srv, 1, CONFIG_BT_MESH_RPR_AD_TYPES_MAX),
	SHELL_CMD_ARG(rpr-scan-caps, NULL, NULL, cmd_rpr_scan_caps, 1, 0),
	SHELL_CMD_ARG(rpr-scan-get, NULL, NULL, cmd_rpr_scan_get, 1, 0),
	SHELL_CMD_ARG(rpr-scan-stop, NULL, NULL, cmd_rpr_scan_stop, 1, 0),
	SHELL_CMD_ARG(rpr-link-get, NULL, NULL, cmd_rpr_link_get, 1, 0),
	SHELL_CMD_ARG(rpr-link-close, NULL, NULL, cmd_rpr_link_close, 1, 0),
	SHELL_CMD_ARG(provision-remote, NULL, "<UUID> <NetKeyIndex> <addr>",
		      cmd_provision_remote, 4, 0),
	SHELL_CMD_ARG(reprovision-remote, NULL,
		      "<addr> [<comp changed: false, true>]",
		      cmd_reprovision_remote, 2, 1),
#endif
#if defined(CONFIG_BT_MESH_SAR_CFG_CLI)
	SHELL_CMD_ARG(sar-tx-get, NULL, NULL, cmd_sar_tx_get, 1, 0),
	SHELL_CMD_ARG(sar-tx-set, NULL, "<7 transmitter state values>", cmd_sar_tx_set, 8, 0),
	SHELL_CMD_ARG(sar-rx-get, NULL, NULL, cmd_sar_rx_get, 1, 0),
	SHELL_CMD_ARG(sar-rx-set, NULL, "<5 receiver state values>", cmd_sar_rx_set, 6, 0),
#endif

#if defined(CONFIG_BT_MESH_PRIV_BEACON_CLI)
	SHELL_CMD_ARG(priv-beacon-get, NULL, NULL, cmd_priv_beacon_get, 1, 0),
	SHELL_CMD_ARG(priv-beacon-set, NULL, "<enable> <rand_interval>", cmd_priv_beacon_set, 3, 0),
	SHELL_CMD_ARG(priv-gatt-proxy-get, NULL, NULL, cmd_priv_gatt_proxy_get, 1, 0),
	SHELL_CMD_ARG(priv-gatt-proxy-set, NULL, "<state>", cmd_priv_gatt_proxy_set, 2, 0),
	SHELL_CMD_ARG(priv-node-id-get, NULL, "<net_idx>", cmd_priv_node_id_get, 2, 0),
	SHELL_CMD_ARG(priv-node-id-set, NULL, "<net_idx> <state>", cmd_priv_node_id_set, 3, 0),
#endif

#if defined(CONFIG_BT_MESH_OP_AGG_CLI)
	SHELL_CMD_ARG(op-agg-seq-start, NULL, "<elem_addr>", cmd_op_agg_seq_start, 2, 0),
	SHELL_CMD_ARG(op-agg-seq-send, NULL, NULL, cmd_op_agg_seq_send, 1, 0),
	SHELL_CMD_ARG(op-agg-seq-abort, NULL, NULL, cmd_op_agg_seq_abort, 1, 0),
#endif
	SHELL_SUBCMD_SET_END);
#if defined(CONFIG_BT_MESH_SHELL_CDB)
SHELL_STATIC_SUBCMD_SET_CREATE(cdb_cmds,
	/* Mesh Configuration Database Operations */
	SHELL_CMD_ARG(create, NULL, "[NetKey]", cmd_cdb_create, 1, 1),
	SHELL_CMD_ARG(clear, NULL, NULL, cmd_cdb_clear, 1, 0),
	SHELL_CMD_ARG(show, NULL, NULL, cmd_cdb_show, 1, 0),
	SHELL_CMD_ARG(node-add, NULL, "<UUID> <Addr> <Num-elem> "
		      "<NetKeyIdx> [DevKey]", cmd_cdb_node_add, 5, 1),
	SHELL_CMD_ARG(node-del, NULL, "<Addr>", cmd_cdb_node_del, 2, 0),
	SHELL_CMD_ARG(subnet-add, NULL, "<NeyKeyIdx> [<NetKey>]",
		      cmd_cdb_subnet_add, 2, 1),
	SHELL_CMD_ARG(subnet-del, NULL, "<NetKeyIdx>", cmd_cdb_subnet_del,
		      2, 0),
	SHELL_CMD_ARG(app-key-add, NULL, "<NetKeyIdx> <AppKeyIdx> "
		      "[<AppKey>]", cmd_cdb_app_key_add, 3, 1),
	SHELL_CMD_ARG(app-key-del, NULL, "<AppKeyIdx>", cmd_cdb_app_key_del,
		      2, 0),
	SHELL_SUBCMD_SET_END);
#endif

#if defined(CONFIG_BT_MESH_SHELL_PROV)
#if defined(CONFIG_BT_MESH_PROVISIONER)
SHELL_STATIC_SUBCMD_SET_CREATE(auth_cmds,
	SHELL_CMD_ARG(input, NULL, "<Action> <Size>",
		      cmd_auth_method_set_input, 3, 0),
	SHELL_CMD_ARG(output, NULL, "<Action> <Size>",
		      cmd_auth_method_set_output, 3, 0),
	SHELL_CMD_ARG(static, NULL, "<Value>", cmd_auth_method_set_static, 2,
		      0),
	SHELL_CMD_ARG(none, NULL, NULL, cmd_auth_method_set_none, 2, 0),
	SHELL_SUBCMD_SET_END);
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	prov_cmds, SHELL_CMD_ARG(input-num, NULL, "<Number>", cmd_input_num, 2, 0),
	SHELL_CMD_ARG(input-str, NULL, "<String>", cmd_input_str, 2, 0),
	SHELL_CMD_ARG(local, NULL, "<NetKeyIndex> <Addr> [IVIndex]", cmd_provision_local, 3, 1),
#if defined(CONFIG_BT_MESH_SHELL_PROV_CTX_INSTANCE)
	SHELL_CMD_ARG(static-oob, NULL, "[Val: 1-16 hex values]", cmd_static_oob, 2, 1),
	SHELL_CMD_ARG(uuid, NULL, "[UUID: 1-16 hex values]", cmd_uuid, 1, 1),
	SHELL_CMD_ARG(beacon-listen, NULL, "<Val: off, on>", cmd_beacon_listen, 2, 0),
#endif

	SHELL_CMD_ARG(comp-change, NULL, NULL, cmd_comp_change, 1, 0),

/* Provisioning operations */
#if defined(CONFIG_BT_MESH_PROV_DEVICE)
#if defined(CONFIG_BT_MESH_PB_GATT)
	SHELL_CMD_ARG(pb-gatt, NULL, "<Val: off, on>", cmd_pb_gatt, 2, 0),
#endif
#if defined(CONFIG_BT_MESH_PB_ADV)
	SHELL_CMD_ARG(pb-adv, NULL, "<Val: off, on>", cmd_pb_adv, 2, 0),
#endif
#endif /* CONFIG_BT_MESH_PROV_DEVICE */

#if defined(CONFIG_BT_MESH_PROVISIONER)
	SHELL_CMD(auth-method, &auth_cmds, "Authentication methods", bt_mesh_shell_mdl_cmds_help),
	SHELL_CMD_ARG(remote-pub-key, NULL, "<PubKey>", cmd_remote_pub_key_set, 2, 0),
	SHELL_CMD_ARG(remote-adv, NULL,
		      "<UUID> <NetKeyIndex> <Addr> "
		      "<AttentionDuration>",
		      cmd_provision_adv, 5, 0),
#endif

#if defined(CONFIG_BT_MESH_PB_GATT_CLIENT)
	SHELL_CMD_ARG(remote-gatt, NULL,
		      "<UUID> <NetKeyIndex> <Addr> "
		      "<AttentionDuration>",
		      cmd_provision_gatt, 5, 0),
#endif
	SHELL_SUBCMD_SET_END);
#endif /* CONFIG_BT_MESH_SHELL_PROV */

#if defined(CONFIG_BT_MESH_SHELL_TEST)
#if defined(CONFIG_BT_MESH_SHELL_HEALTH_SRV_INSTANCE)
SHELL_STATIC_SUBCMD_SET_CREATE(health_srv_cmds,
	/* Health Server Model Operations */
	SHELL_CMD_ARG(add-fault, NULL, "<Fault ID>", cmd_add_fault, 2, 0),
	SHELL_CMD_ARG(del-fault, NULL, "[Fault ID]", cmd_del_fault, 1, 1),
	SHELL_SUBCMD_SET_END);
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(test_cmds,
	/* Commands which access internal APIs, for testing only */
	SHELL_CMD_ARG(net-send, NULL, "<Hex string>", cmd_net_send,
		      2, 0),
#if defined(CONFIG_BT_MESH_IV_UPDATE_TEST)
	SHELL_CMD_ARG(iv-update, NULL, NULL, cmd_iv_update, 1, 0),
	SHELL_CMD_ARG(iv-update-test, NULL, "<Val: off, on>", cmd_iv_update_test, 2, 0),
#endif
	SHELL_CMD_ARG(rpl-clear, NULL, NULL, cmd_rpl_clear, 1, 0),
#if defined(CONFIG_BT_MESH_SHELL_HEALTH_SRV_INSTANCE)
	SHELL_CMD(health-srv, &health_srv_cmds, "Health Server test", bt_mesh_shell_mdl_cmds_help),
#endif
	SHELL_SUBCMD_SET_END);
#endif /* CONFIG_BT_MESH_SHELL_TEST */

#if defined(CONFIG_BT_MESH_SHELL_GATT_PROXY)
SHELL_STATIC_SUBCMD_SET_CREATE(proxy_cmds,
#if defined(CONFIG_BT_MESH_GATT_PROXY)
	SHELL_CMD_ARG(identity-enable, NULL, NULL, cmd_ident, 1, 0),
#endif

#if defined(CONFIG_BT_MESH_PROXY_CLIENT)
	SHELL_CMD_ARG(connect, NULL, "<NetKeyIndex>", cmd_proxy_connect, 2, 0),
	SHELL_CMD_ARG(disconnect, NULL, "<NetKeyIndex>", cmd_proxy_disconnect, 2, 0),
#endif
	SHELL_SUBCMD_SET_END);
#endif /* CONFIG_BT_MESH_SHELL_GATT_PROXY */

#if defined(CONFIG_BT_MESH_SHELL_LOW_POWER)
SHELL_STATIC_SUBCMD_SET_CREATE(low_pwr_cmds,
	SHELL_CMD_ARG(set, NULL, "<Val: off, on>", cmd_lpn, 2, 0),
	SHELL_CMD_ARG(poll, NULL, NULL, cmd_poll, 1, 0),
	SHELL_SUBCMD_SET_END);
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(target_cmds,
	SHELL_CMD_ARG(dst, NULL, "[dst_addr]", cmd_dst, 1, 1),
	SHELL_CMD_ARG(net, NULL, "[net_idx]", cmd_netidx, 1, 1),
	SHELL_CMD_ARG(app, NULL, "[app_idx]", cmd_appidx, 1, 1),
	SHELL_SUBCMD_SET_END);

/* Placeholder for model shell modules that is configured in the application */
SHELL_SUBCMD_SET_CREATE(model_cmds, (mesh, models));

/* List of Mesh subcommands.
 *
 * Each command is documented in doc/reference/bluetooth/mesh/shell.rst.
 *
 * Please keep the documentation up to date by adding any new commands to the
 * list.
 */
SHELL_STATIC_SUBCMD_SET_CREATE(mesh_cmds,
	SHELL_CMD_ARG(init, NULL, NULL, cmd_init, 1, 0),
	SHELL_CMD_ARG(reset-local, NULL, NULL, cmd_reset, 1, 0),

	SHELL_CMD(models, &model_cmds, "Model commands", bt_mesh_shell_mdl_cmds_help),

#if defined(CONFIG_BT_MESH_SHELL_LOW_POWER)
	SHELL_CMD(lpn, &low_pwr_cmds, "Low Power commands", bt_mesh_shell_mdl_cmds_help),
#endif

#if defined(CONFIG_BT_MESH_SHELL_CDB)
	SHELL_CMD(cdb, &cdb_cmds, "Configuration Database", bt_mesh_shell_mdl_cmds_help),
#endif

#if defined(CONFIG_BT_MESH_SHELL_GATT_PROXY)
	SHELL_CMD(proxy, &proxy_cmds, "Proxy commands", bt_mesh_shell_mdl_cmds_help),
#endif

#if defined(CONFIG_BT_MESH_SHELL_PROV)
	SHELL_CMD(prov, &prov_cmds, "Provisioning commands", bt_mesh_shell_mdl_cmds_help),
#endif

#if defined(CONFIG_BT_MESH_SHELL_TEST)
	SHELL_CMD(test, &test_cmds, "Test commands", bt_mesh_shell_mdl_cmds_help),
#endif
	SHELL_CMD(target, &target_cmds, "Target commands", bt_mesh_shell_mdl_cmds_help),

	SHELL_CMD(new, &mesh_11_cmds, "Mesh 1.1 commands", bt_mesh_shell_mdl_cmds_help),

	SHELL_SUBCMD_SET_END
);

SHELL_CMD_ARG_REGISTER(mesh, &mesh_cmds, "Bluetooth mesh shell commands",
			bt_mesh_shell_mdl_cmds_help, 1, 1);
