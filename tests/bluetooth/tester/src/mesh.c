/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/bluetooth/bluetooth.h>

#include <assert.h>
#include <errno.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/testing.h>
#include <zephyr/bluetooth/mesh/cfg.h>
#include <zephyr/sys/byteorder.h>
#include <app_keys.h>
#include <sar_cfg_internal.h>
#include <settings/settings.h>
#include <string.h>

#include <zephyr/logging/log.h>
#define LOG_MODULE_NAME bttester_mesh
LOG_MODULE_REGISTER(LOG_MODULE_NAME);

#include "bttester.h"
#include "dfu_slot.h"

#define CONTROLLER_INDEX 0
#define CID_LOCAL 0x05F1
#define COMPANY_ID_LF 0x05F1
#define COMPANY_ID_NORDIC_SEMI 0x05F9

/* Health server data */
#define CUR_FAULTS_MAX 4
#define HEALTH_TEST_ID 0x00

static uint8_t cur_faults[CUR_FAULTS_MAX];
static uint8_t reg_faults[CUR_FAULTS_MAX * 2];

/* Provision node data */
static uint8_t net_key[16];
static uint16_t net_key_idx;
static uint8_t flags;
static uint32_t iv_index;
static uint16_t addr;
static uint8_t dev_key[16];
static uint8_t input_size;
static uint8_t pub_key[64];
static uint8_t priv_key[32];

/* Configured provisioning data */
static uint8_t dev_uuid[16];
static uint8_t static_auth[16];

/* Vendor Model data */
#define VND_MODEL_ID_1 0x1234
static uint8_t vnd_app_key[16];
static uint16_t vnd_app_key_idx = 0x000f;

/* Model send data */
#define MODEL_BOUNDS_MAX 100

/* BLOB Model data*/
static uint8_t blob_rx_sum;
static bool blob_valid;
static const char *blob_data = "11111111111111111111111111111111";

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
	for (int i = 0; i < chunk->size; ++i) {
		blob_rx_sum += chunk->data[i];
		if (chunk->data[i] !=
		    blob_data[(i + chunk->offset) % strlen(blob_data)]) {
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
			blob_data[(i + chunk->offset) % strlen(blob_data)];
	}

	return 0;
}

static const struct bt_mesh_blob_io dummy_blob_io = {
	.open = blob_io_open,
	.rd = blob_chunk_rd,
	.wr = blob_chunk_wr,
};

#if defined(CONFIG_BT_MESH_DFD_SRV)
/* DFD Model data*/
static int dfd_srv_recv(struct bt_mesh_dfd_srv *srv,
			const struct bt_mesh_dfu_slot *slot,
			const struct bt_mesh_blob_io **io)
{
	LOG_DBG("Uploading new firmware image to the distributor.");

	*io = &dummy_blob_io;

	return 0;
}

static void dfd_srv_del(struct bt_mesh_dfd_srv *srv,
			const struct bt_mesh_dfu_slot *slot)
{
	LOG_DBG("Deleting the firmware image from the distributor.");
}

static int dfd_srv_send(struct bt_mesh_dfd_srv *srv,
			const struct bt_mesh_dfu_slot *slot,
			const struct bt_mesh_blob_io **io)
{
	LOG_DBG("Starting the firmware distribution.");

	*io = &dummy_blob_io;

	return 0;
}

static struct bt_mesh_dfd_srv_cb dfd_srv_cb = {
	.recv = dfd_srv_recv,
	.del = dfd_srv_del,
	.send = dfd_srv_send,
};

static struct bt_mesh_dfd_srv dfd_srv = BT_MESH_DFD_SRV_INIT(&dfd_srv_cb);
#endif

#if defined(CONFIG_BT_MESH_BLOB_CLI) && !defined(CONFIG_BT_MESH_DFD_SRV)
static struct {
	struct bt_mesh_blob_cli_inputs inputs;
	struct bt_mesh_blob_target targets[32];
	uint8_t target_count;
	struct bt_mesh_blob_xfer xfer;
} blob_cli_xfer;

static void blob_cli_lost_target(struct bt_mesh_blob_cli *cli,
				 struct bt_mesh_blob_target *target,
				 enum bt_mesh_blob_status reason)
{
	LOG_DBG("Mesh Blob: Lost target 0x%04x (reason: %u)", target->addr,
		reason);
}

static void blob_cli_caps(struct bt_mesh_blob_cli *cli,
			  const struct bt_mesh_blob_cli_caps *caps)
{
	const char *const modes[] = {
		"none",
		"push",
		"pull",
		"all",
	};

	if (!caps) {
		LOG_DBG("None of the targets can be used for BLOB transfer");
		return;
	}

	LOG_DBG("Mesh BLOB: capabilities:");
	LOG_DBG("\tMax BLOB size: %u bytes", caps->max_size);
	LOG_DBG("\tBlock size: %u-%u (%u-%u bytes)", caps->min_block_size_log,
		caps->max_block_size_log, 1 << caps->min_block_size_log,
		1 << caps->max_block_size_log);
	LOG_DBG("\tMax chunks: %u", caps->max_chunks);
	LOG_DBG("\tChunk size: %u", caps->max_chunk_size);
	LOG_DBG("\tMTU size: %u", caps->mtu_size);
	LOG_DBG("\tModes: %s", modes[caps->modes]);
}

static void blob_cli_end(struct bt_mesh_blob_cli *cli,
			 const struct bt_mesh_blob_xfer *xfer, bool success)
{
	if (success) {
		LOG_DBG("Mesh BLOB transfer complete.");
	} else {
		LOG_DBG("Mesh BLOB transfer failed.");
	}
}

static const struct bt_mesh_blob_cli_cb blob_cli_handlers = {
	.lost_target = blob_cli_lost_target,
	.caps = blob_cli_caps,
	.end = blob_cli_end,
};

static struct bt_mesh_blob_cli blob_cli = { .cb = &blob_cli_handlers };
#endif

#if defined(CONFIG_BT_MESH_DFU_SRV)
const char *metadata_data = "1100000000000011";

static uint8_t dfu_fwid[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static struct bt_mesh_dfu_img dfu_imgs[] = { {
	.fwid = &dfu_fwid,
	.fwid_len = sizeof(dfu_fwid),
} };

static int dfu_meta_check(struct bt_mesh_dfu_srv *srv,
			  const struct bt_mesh_dfu_img *img,
			  struct net_buf_simple *metadata,
			  enum bt_mesh_dfu_effect *effect)
{
	char string[2 * CONFIG_BT_MESH_DFU_METADATA_MAXLEN + 1];
	int i;
	size_t len;

	len = bin2hex(metadata->data, metadata->len, string, sizeof(string));
	string[len] = '\0';

	for (i = 0; i <= len; i++) {
		if (string[i] != metadata_data[i]) {
			LOG_ERR("Wrong Firmware Metadata");
			return -EINVAL;
		}
	}

	return 0;
}

static int dfu_start(struct bt_mesh_dfu_srv *srv,
		     const struct bt_mesh_dfu_img *img,
		     struct net_buf_simple *metadata,
		     const struct bt_mesh_blob_io **io)
{
	LOG_DBG("DFU setup");

	*io = &dummy_blob_io;

	return 0;
}

static void dfu_end(struct bt_mesh_dfu_srv *srv,
		    const struct bt_mesh_dfu_img *img, bool success)
{
	if (!success) {
		LOG_ERR("DFU failed");
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

	LOG_DBG("Applying DFU transfer...");

	return 0;
}

static const struct bt_mesh_dfu_srv_cb dfu_handlers = {
	.check = dfu_meta_check,
	.start = dfu_start,
	.end = dfu_end,
	.apply = dfu_apply,
};

static struct bt_mesh_dfu_srv dfu_srv =
	BT_MESH_DFU_SRV_INIT(&dfu_handlers, dfu_imgs, ARRAY_SIZE(dfu_imgs));
#endif /* CONFIG_BT_MESH_DFU_SRV */

/* Model Authentication Method */
#define AUTH_METHOD_STATIC 0x01
#define AUTH_METHOD_OUTPUT 0x02
#define AUTH_METHOD_INPUT 0x03

static struct model_data {
	struct bt_mesh_model *model;
	uint16_t addr;
	uint16_t appkey_idx;
} model_bound[MODEL_BOUNDS_MAX];

static struct {
	uint16_t local;
	uint16_t dst;
	uint16_t net_idx;
} net = {
	.local = BT_MESH_ADDR_UNASSIGNED,
	.dst = BT_MESH_ADDR_UNASSIGNED,
};

static bool default_comp = true;

static void supported_commands(uint8_t *data, uint16_t len)
{
	struct net_buf_simple *buf = NET_BUF_SIMPLE(BTP_DATA_MAX_SIZE);

	net_buf_simple_init(buf, 0);
	net_buf_simple_add_u8(buf, MESH_READ_SUPPORTED_COMMANDS);
	net_buf_simple_add_u8(buf, MESH_CONFIG_PROVISIONING);
	net_buf_simple_add_u8(buf, MESH_PROVISION_NODE);
	net_buf_simple_add_u8(buf, MESH_INIT);
	net_buf_simple_add_u8(buf, MESH_RESET);
	net_buf_simple_add_u8(buf, MESH_INPUT_NUMBER);
	net_buf_simple_add_u8(buf, MESH_INPUT_STRING);
	net_buf_simple_add_u8(buf, MESH_IVU_TEST_MODE);
	net_buf_simple_add_u8(buf, MESH_IVU_TOGGLE_STATE);
	net_buf_simple_add_u8(buf, MESH_NET_SEND);
	net_buf_simple_add_u8(buf, MESH_HEALTH_GENERATE_FAULTS);
	net_buf_simple_add_u8(buf, MESH_HEALTH_CLEAR_FAULTS);
	net_buf_simple_add_u8(buf, MESH_LPN);
	net_buf_simple_add_u8(buf, MESH_LPN_POLL);
	net_buf_simple_add_u8(buf, MESH_MODEL_SEND);
#if defined(CONFIG_BT_TESTING)
	net_buf_simple_add_u8(buf, MESH_LPN_SUBSCRIBE);
	net_buf_simple_add_u8(buf, MESH_LPN_UNSUBSCRIBE);
	net_buf_simple_add_u8(buf, MESH_RPL_CLEAR);
#endif /* CONFIG_BT_TESTING */
	net_buf_simple_add_u8(buf, MESH_PROXY_IDENTITY);
	net_buf_simple_add_u8(buf, MESH_COMP_DATA_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_BEACON_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_BEACON_SET);
	net_buf_simple_add_u8(buf, MESH_CFG_DEFAULT_TTL_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_DEFAULT_TTL_SET);
	net_buf_simple_add_u8(buf, MESH_CFG_GATT_PROXY_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_GATT_PROXY_SET);
	net_buf_simple_add_u8(buf, MESH_CFG_FRIEND_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_FRIEND_SET);
	net_buf_simple_add_u8(buf, MESH_CFG_RELAY_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_RELAY_SET);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_PUB_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_PUB_SET);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_SUB_ADD);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_SUB_DEL);
	net_buf_simple_add_u8(buf, MESH_CFG_NETKEY_ADD);
	net_buf_simple_add_u8(buf, MESH_CFG_NETKEY_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_NETKEY_DEL);
	net_buf_simple_add_u8(buf, MESH_CFG_APPKEY_ADD);
	net_buf_simple_add_u8(buf, MESH_CFG_APPKEY_DEL);
	net_buf_simple_add_u8(buf, MESH_CFG_APPKEY_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_APP_BIND);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_APP_UNBIND);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_APP_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_APP_VND_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_HEARTBEAT_PUB_SET);
	net_buf_simple_add_u8(buf, MESH_CFG_HEARTBEAT_PUB_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_HEARTBEAT_SUB_SET);
	net_buf_simple_add_u8(buf, MESH_CFG_HEARTBEAT_SUB_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_NET_TRANS_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_NET_TRANS_SET);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_SUB_OVW);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_SUB_DEL_ALL);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_SUB_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_SUB_GET_VND);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_SUB_VA_ADD);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_SUB_VA_DEL);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_SUB_VA_OVW);
	net_buf_simple_add_u8(buf, MESH_CFG_NETKEY_UPDATE);
	net_buf_simple_add_u8(buf, MESH_CFG_APPKEY_UPDATE);
	net_buf_simple_add_u8(buf, MESH_CFG_NODE_IDT_SET);
	net_buf_simple_add_u8(buf, MESH_CFG_NODE_IDT_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_NODE_RESET);
	net_buf_simple_add_u8(buf, MESH_CFG_LPN_TIMEOUT_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_MODEL_APP_BIND_VND);
	net_buf_simple_add_u8(buf, MESH_HEALTH_FAULT_GET);
	net_buf_simple_add_u8(buf, MESH_HEALTH_FAULT_CLEAR);
	net_buf_simple_add_u8(buf, MESH_HEALTH_PERIOD_GET);
	net_buf_simple_add_u8(buf, MESH_HEALTH_PERIOD_SET);
	net_buf_simple_add_u8(buf, MESH_HEALTH_ATTENTION_GET);
	net_buf_simple_add_u8(buf, MESH_HEALTH_ATTENTION_SET);
	net_buf_simple_add_u8(buf, MESH_PROVISION_ADV);
	net_buf_simple_add_u8(buf, MESH_CFG_KRP_GET);
	net_buf_simple_add_u8(buf, MESH_CFG_KRP_SET);

	tester_send(BTP_SERVICE_ID_MESH, MESH_READ_SUPPORTED_COMMANDS,
		    CONTROLLER_INDEX, buf->data, buf->len);
}

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
	LOG_DBG("");

	*test_id = HEALTH_TEST_ID;
	*company_id = CID_LOCAL;

	get_faults(cur_faults, sizeof(cur_faults), faults, fault_count);

	return 0;
}

static int fault_get_reg(struct bt_mesh_model *model, uint16_t company_id,
			 uint8_t *test_id, uint8_t *faults, uint8_t *fault_count)
{
	LOG_DBG("company_id 0x%04x", company_id);

	if (company_id != CID_LOCAL) {
		return -EINVAL;
	}

	*test_id = HEALTH_TEST_ID;

	get_faults(reg_faults, sizeof(reg_faults), faults, fault_count);

	return 0;
}

static int fault_clear(struct bt_mesh_model *model, uint16_t company_id)
{
	LOG_DBG("company_id 0x%04x", company_id);

	if (company_id != CID_LOCAL) {
		return -EINVAL;
	}

	(void)memset(reg_faults, 0, sizeof(reg_faults));

	return 0;
}

static int fault_test(struct bt_mesh_model *model, uint8_t test_id,
		      uint16_t company_id)
{
	LOG_DBG("test_id 0x%02x company_id 0x%04x", test_id, company_id);

	if (company_id != CID_LOCAL || test_id != HEALTH_TEST_ID) {
		return -EINVAL;
	}

	return 0;
}

static const struct bt_mesh_health_srv_cb health_srv_cb = {
	.fault_get_cur = fault_get_cur,
	.fault_get_reg = fault_get_reg,
	.fault_clear = fault_clear,
	.fault_test = fault_test,
};

static void show_faults(uint8_t test_id, uint16_t cid, uint8_t *faults, size_t fault_count)
{
	size_t i;

	if (!fault_count) {
		LOG_DBG("Health Test ID 0x%02x Company ID 0x%04x: no faults",
			test_id, cid);
		return;
	}

	LOG_DBG("Health Test ID 0x%02x Company ID 0x%04x Fault Count %zu: ",
		test_id, cid, fault_count);

	for (i = 0; i < fault_count; i++) {
		LOG_DBG("0x%02x", faults[i]);
	}
}

static void health_current_status(struct bt_mesh_health_cli *cli, uint16_t addr,
				  uint8_t test_id, uint16_t cid, uint8_t *faults,
				  size_t fault_count)
{
	LOG_DBG("Health Current Status from 0x%04x", addr);
	show_faults(test_id, cid, faults, fault_count);
}

static struct bt_mesh_health_cli health_cli = {
	.current_status = health_current_status,
};


#ifdef CONFIG_BT_MESH_LARGE_COMP_DATA_SRV
static uint8_t health_tests[] = {
	BT_MESH_HEALTH_TEST_INFO(COMPANY_ID_LF, 6, 0x01, 0x02, 0x03, 0x04, 0x34,
				 0x15),
	BT_MESH_HEALTH_TEST_INFO(COMPANY_ID_NORDIC_SEMI, 3, 0x01, 0x02, 0x03),
};

static struct bt_mesh_models_metadata_entry health_srv_meta[] = {
	BT_MESH_HEALTH_TEST_INFO_METADATA(health_tests),
	BT_MESH_MODELS_METADATA_END,
};
#endif

static struct bt_mesh_health_srv health_srv = {
	.cb = &health_srv_cb,
#ifdef CONFIG_BT_MESH_LARGE_COMP_DATA_SRV
	.metadata = health_srv_meta,
#endif
};

BT_MESH_HEALTH_PUB_DEFINE(health_pub, CUR_FAULTS_MAX);

static struct bt_mesh_cfg_cli cfg_cli = {
};

static struct bt_mesh_sar_cfg_cli sar_cfg_cli;

#if defined(CONFIG_BT_MESH_RPR_CLI)
static void rpr_scan_report(struct bt_mesh_rpr_cli *cli,
			    const struct bt_mesh_rpr_node *srv,
			    struct bt_mesh_rpr_unprov *unprov,
			    struct net_buf_simple *adv_data)
{
	char uuid_hex_str[32 + 1];

	bin2hex(unprov->uuid, 16, uuid_hex_str, sizeof(uuid_hex_str));

	LOG_DBG("Server 0x%04x:\n"
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
			LOG_DBG("\tURI:    \"\\x%02x%s\"",
				data[0], &data[1]);
		} else if (type == BT_DATA_NAME_COMPLETE) {
			LOG_DBG("\tName:   \"%s\"", data);
		} else {
			char string[64 + 1];

			bin2hex(data, len, string, sizeof(string));
			LOG_DBG("\t0x%02x:  %s", type, string);
		}
	}
}

static struct bt_mesh_rpr_cli rpr_cli = {
	.scan_report = rpr_scan_report,
};
#endif

static struct bt_mesh_model root_models[] = {
	BT_MESH_MODEL_CFG_SRV,
	BT_MESH_MODEL_CFG_CLI(&cfg_cli),
	BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
	BT_MESH_MODEL_HEALTH_CLI(&health_cli),
	BT_MESH_MODEL_SAR_CFG_SRV,
	BT_MESH_MODEL_SAR_CFG_CLI(&sar_cfg_cli),
#if defined(CONFIG_BT_MESH_LARGE_COMP_DATA_SRV)
	BT_MESH_MODEL_LARGE_COMP_DATA_SRV,
#endif
	BT_MESH_MODEL_LARGE_COMP_DATA_CLI,
#if defined(CONFIG_BT_MESH_RPR_CLI)
	BT_MESH_MODEL_RPR_CLI(&rpr_cli),
#endif
#if defined(CONFIG_BT_MESH_RPR_SRV)
	BT_MESH_MODEL_RPR_SRV,
#endif
#if defined(CONFIG_BT_MESH_DFD_SRV)
	BT_MESH_MODEL_DFD_SRV(&dfd_srv),
#elif defined(CONFIG_BT_MESH_DFU_SRV)
	BT_MESH_MODEL_DFU_SRV(&dfu_srv),
#endif
#if defined(CONFIG_BT_MESH_BLOB_CLI) && !defined(CONFIG_BT_MESH_DFD_SRV)
	BT_MESH_MODEL_BLOB_CLI(&blob_cli),
#endif
};
struct model_data *lookup_model_bound(uint16_t id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(model_bound); i++) {
		if (model_bound[i].model && model_bound[i].model->id == id) {
			return &model_bound[i];
		}
	}

	return NULL;
}
static struct bt_mesh_model vnd_models[] = {
	BT_MESH_MODEL_VND(CID_LOCAL, VND_MODEL_ID_1, BT_MESH_MODEL_NO_OPS, NULL,
			  NULL),
};

static struct bt_mesh_elem elements[] = {
	BT_MESH_ELEM(0, root_models, vnd_models),
};

static void link_open(bt_mesh_prov_bearer_t bearer)
{
	struct mesh_prov_link_open_ev ev;

	LOG_DBG("bearer 0x%02x", bearer);

	switch (bearer) {
	case BT_MESH_PROV_ADV:
		ev.bearer = MESH_PROV_BEARER_PB_ADV;
		break;
	case BT_MESH_PROV_GATT:
		ev.bearer = MESH_PROV_BEARER_PB_GATT;
		break;
	case BT_MESH_PROV_REMOTE:
		ev.bearer = MESH_PROV_BEARER_REMOTE;
		break;
	default:
		LOG_ERR("Invalid bearer");

		return;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_PROV_LINK_OPEN,
		    CONTROLLER_INDEX, (uint8_t *) &ev, sizeof(ev));
}

static void link_close(bt_mesh_prov_bearer_t bearer)
{
	struct mesh_prov_link_closed_ev ev;

	LOG_DBG("bearer 0x%02x", bearer);

	switch (bearer) {
	case BT_MESH_PROV_ADV:
		ev.bearer = MESH_PROV_BEARER_PB_ADV;
		break;
	case BT_MESH_PROV_GATT:
		ev.bearer = MESH_PROV_BEARER_PB_GATT;
		break;
	case BT_MESH_PROV_REMOTE:
		ev.bearer = MESH_PROV_BEARER_REMOTE;
		break;
	default:
		LOG_ERR("Invalid bearer");

		return;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_PROV_LINK_CLOSED,
		    CONTROLLER_INDEX, (uint8_t *) &ev, sizeof(ev));
}

static int output_number(bt_mesh_output_action_t action, uint32_t number)
{
	struct mesh_out_number_action_ev ev;

	LOG_DBG("action 0x%04x number 0x%08x", action, number);

	ev.action = sys_cpu_to_le16(action);
	ev.number = sys_cpu_to_le32(number);

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_OUT_NUMBER_ACTION,
		    CONTROLLER_INDEX, (uint8_t *) &ev, sizeof(ev));

	return 0;
}

static int output_string(const char *str)
{
	struct mesh_out_string_action_ev *ev;
	struct net_buf_simple *buf = NET_BUF_SIMPLE(BTP_DATA_MAX_SIZE);

	LOG_DBG("str %s", str);

	net_buf_simple_init(buf, 0);

	ev = net_buf_simple_add(buf, sizeof(*ev));
	ev->string_len = strlen(str);

	net_buf_simple_add_mem(buf, str, ev->string_len);

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_OUT_STRING_ACTION,
		    CONTROLLER_INDEX, buf->data, buf->len);

	return 0;
}

static int input(bt_mesh_input_action_t action, uint8_t size)
{
	struct mesh_in_action_ev ev;

	LOG_DBG("action 0x%04x number 0x%02x", action, size);

	input_size = size;

	ev.action = sys_cpu_to_le16(action);
	ev.size = size;

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_IN_ACTION, CONTROLLER_INDEX,
		    (uint8_t *) &ev, sizeof(ev));

	return 0;
}

static void prov_complete(uint16_t net_idx, uint16_t addr)
{
	LOG_DBG("net_idx 0x%04x addr 0x%04x", net_idx, addr);

	net.net_idx = net_idx,
	net.local = addr;
	net.dst = addr;

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_PROVISIONED, CONTROLLER_INDEX,
		    NULL, 0);
}

static void prov_node_added(uint16_t net_idx, uint8_t uuid[16], uint16_t addr,
			    uint8_t num_elem)
{
	struct mesh_prov_node_added_ev ev;

	LOG_DBG("net_idx 0x%04x addr 0x%04x num_elem %d", net_idx, addr,
		num_elem);

	ev.net_idx = net_idx;
	ev.addr = addr;
	ev.num_elems = num_elem;
	memcpy(&ev.uuid, uuid, sizeof(ev.uuid));

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_PROV_NODE_ADDED,
		    CONTROLLER_INDEX, (void *)&ev, sizeof(ev));
}

static void prov_reset(void)
{
	LOG_DBG("");

	bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);

	if (IS_ENABLED(CONFIG_BT_MESH_RPR_SRV)) {
		bt_mesh_prov_enable(BT_MESH_PROV_REMOTE);
	}
}

static const struct bt_mesh_comp comp = {
	.cid = CID_LOCAL,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
	.vid = 1,
};

static const struct bt_mesh_comp comp_alt = {
	.cid = CID_LOCAL,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
	.vid = 2,
};

static struct bt_mesh_prov prov = {
	.uuid = dev_uuid,
	.static_val = static_auth,
	.static_val_len = sizeof(static_auth),
	.output_number = output_number,
	.output_string = output_string,
	.input = input,
	.link_open = link_open,
	.link_close = link_close,
	.complete = prov_complete,
	.node_added = prov_node_added,
	.reset = prov_reset,
	.uri = "Tester",
};

static void config_prov(uint8_t *data, uint16_t len)
{
	const struct mesh_config_provisioning_cmd *cmd = (void *) data;
	int err = 0;

	LOG_DBG("");

	memcpy(dev_uuid, cmd->uuid, sizeof(dev_uuid));
	memcpy(static_auth, cmd->static_auth, sizeof(static_auth));

	prov.output_size = cmd->out_size;
	prov.output_actions = sys_le16_to_cpu(cmd->out_actions);
	prov.input_size = cmd->in_size;
	prov.input_actions = sys_le16_to_cpu(cmd->in_actions);

	if (cmd->auth_method == AUTH_METHOD_OUTPUT) {
		err = bt_mesh_auth_method_set_output(prov.output_actions, prov.output_size);
	} else if (cmd->auth_method == AUTH_METHOD_INPUT) {
		err = bt_mesh_auth_method_set_input(prov.input_actions, prov.input_size);
	} else if (cmd->auth_method == AUTH_METHOD_STATIC) {
		err = bt_mesh_auth_method_set_static(static_auth, sizeof(static_auth));
	}

	if (len > sizeof(*cmd)) {
		memcpy(pub_key, cmd->set_keys->pub_key, sizeof(cmd->set_keys->pub_key));
		memcpy(priv_key, cmd->set_keys->priv_key, sizeof(cmd->set_keys->priv_key));
		prov.public_key_be = pub_key;
		prov.private_key_be = priv_key;
	}

	if (err) {
		LOG_ERR("err %d", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CONFIG_PROVISIONING, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void provision_node(uint8_t *data, uint16_t len)
{
	const struct mesh_provision_node_cmd *cmd = (void *)data;
	int err;

	LOG_DBG("");

	memcpy(dev_key, cmd->dev_key, sizeof(dev_key));
	memcpy(net_key, cmd->net_key, sizeof(net_key));

	addr = sys_le16_to_cpu(cmd->addr);
	flags = cmd->flags;
	iv_index = sys_le32_to_cpu(cmd->iv_index);
	net_key_idx = sys_le16_to_cpu(cmd->net_key_idx);

	if (len > sizeof(*cmd)) {
		memcpy(pub_key, cmd->pub_key, sizeof(pub_key));

		err = bt_mesh_prov_remote_pub_key_set(pub_key);
		if (err) {
			LOG_ERR("err %d", err);
			goto fail;
		}
	}
#if defined(CONFIG_BT_MESH_PROVISIONER)
	err = bt_mesh_cdb_create(net_key);
	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}
#endif
	err = bt_mesh_provision(net_key, net_key_idx, flags, iv_index, addr,
				dev_key);
	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_PROVISION_NODE, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void provision_adv(uint8_t *data, uint16_t len)
{
	const struct mesh_provision_adv_cmd *cmd = (void *)data;
	int err;

	LOG_DBG("");

	err = bt_mesh_provision_adv(cmd->uuid, cmd->net_idx, cmd->address,
				    cmd->attention_duration);
	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_PROVISION_ADV, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void init(uint8_t *data, uint16_t len)
{
	uint8_t status = BTP_STATUS_SUCCESS;
	int err;

	LOG_DBG("");

	if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
		printk("Loading stored settings\n");
		settings_load();
	}

	if (addr) {
		err = bt_mesh_provision(net_key, net_key_idx, flags, iv_index,
					addr, dev_key);
		if (err && err != -EALREADY) {
			status = BTP_STATUS_FAILED;
		}
	} else {
		err = bt_mesh_prov_enable(BT_MESH_PROV_ADV | BT_MESH_PROV_GATT);
		if (err && err != -EALREADY) {
			status = BTP_STATUS_FAILED;
		}
	}

	if (IS_ENABLED(CONFIG_BT_MESH_RPR_SRV)) {
		err = bt_mesh_prov_enable(BT_MESH_PROV_REMOTE);
		if (err) {
			status = BTP_STATUS_FAILED;
		}
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_INIT, CONTROLLER_INDEX,
		   status);
}

static void reset(uint8_t *data, uint16_t len)
{
	LOG_DBG("");

	bt_mesh_reset();

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_RESET, CONTROLLER_INDEX,
		   BTP_STATUS_SUCCESS);
}

static void input_number(uint8_t *data, uint16_t len)
{
	const struct mesh_input_number_cmd *cmd = (void *) data;
	uint8_t status = BTP_STATUS_SUCCESS;
	uint32_t number;
	int err;

	number = sys_le32_to_cpu(cmd->number);

	LOG_DBG("number 0x%04x", number);

	err = bt_mesh_input_number(number);
	if (err) {
		status = BTP_STATUS_FAILED;
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_INPUT_NUMBER, CONTROLLER_INDEX,
		   status);
}

static void input_string(uint8_t *data, uint16_t len)
{
	const struct mesh_input_string_cmd *cmd = (void *) data;
	uint8_t status = BTP_STATUS_SUCCESS;
	uint8_t str_auth[16];
	int err;

	LOG_DBG("");

	if (cmd->string_len > sizeof(str_auth)) {
		LOG_ERR("Too long input (%u chars required)", input_size);
		status = BTP_STATUS_FAILED;
		goto rsp;
	} else if (cmd->string_len < input_size) {
		LOG_ERR("Too short input (%u chars required)", input_size);
		status = BTP_STATUS_FAILED;
		goto rsp;
	}

	strncpy(str_auth, cmd->string, cmd->string_len);

	err = bt_mesh_input_string(str_auth);
	if (err) {
		status = BTP_STATUS_FAILED;
	}

rsp:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_INPUT_STRING, CONTROLLER_INDEX,
		   status);
}

static void ivu_test_mode(uint8_t *data, uint16_t len)
{
	const struct mesh_ivu_test_mode_cmd *cmd = (void *) data;

	LOG_DBG("enable 0x%02x", cmd->enable);

	bt_mesh_iv_update_test(cmd->enable ? true : false);

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_IVU_TEST_MODE, CONTROLLER_INDEX,
		   BTP_STATUS_SUCCESS);
}

static void ivu_toggle_state(uint8_t *data, uint16_t len)
{
	bool result;

	LOG_DBG("");

	result = bt_mesh_iv_update();
	if (!result) {
		LOG_ERR("Failed to toggle the IV Update state");
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_IVU_TOGGLE_STATE, CONTROLLER_INDEX,
		   result ? BTP_STATUS_SUCCESS : BTP_STATUS_FAILED);
}

static void lpn(uint8_t *data, uint16_t len)
{
	struct mesh_lpn_set_cmd *cmd = (void *) data;
	bool enable;
	int err;

	LOG_DBG("enable 0x%02x", cmd->enable);

	enable = cmd->enable ? true : false;
	err = bt_mesh_lpn_set(enable);
	if (err) {
		LOG_ERR("Failed to toggle LPN (err %d)", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_LPN, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void lpn_poll(uint8_t *data, uint16_t len)
{
	int err;

	LOG_DBG("");

	err = bt_mesh_lpn_poll();
	if (err) {
		LOG_ERR("Failed to send poll msg (err %d)", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_LPN_POLL, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void net_send(uint8_t *data, uint16_t len)
{
	struct mesh_net_send_cmd *cmd = (void *) data;
	NET_BUF_SIMPLE_DEFINE(msg, UINT8_MAX);
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.app_idx = vnd_app_key_idx,
		.addr = sys_le16_to_cpu(cmd->dst),
		.send_ttl = cmd->ttl,
	};
	int err;

	LOG_DBG("ttl 0x%02x dst 0x%04x payload_len %d", ctx.send_ttl,
		ctx.addr, cmd->payload_len);

	if (!bt_mesh_app_key_exists(vnd_app_key_idx)) {
		(void)bt_mesh_app_key_add(vnd_app_key_idx, net.net_idx,
					  vnd_app_key);
		vnd_models[0].keys[0] = vnd_app_key_idx;
	}

	net_buf_simple_add_mem(&msg, cmd->payload, cmd->payload_len);

	err = bt_mesh_model_send(&vnd_models[0], &ctx, &msg, NULL, NULL);
	if (err) {
		LOG_ERR("Failed to send (err %d)", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_NET_SEND, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void health_generate_faults(uint8_t *data, uint16_t len)
{
	struct mesh_health_generate_faults_rp *rp;
	NET_BUF_SIMPLE_DEFINE(buf, sizeof(*rp) + sizeof(cur_faults) +
			      sizeof(reg_faults));
	uint8_t some_faults[] = { 0x01, 0x02, 0x03, 0xff, 0x06 };
	uint8_t cur_faults_count, reg_faults_count;

	rp = net_buf_simple_add(&buf, sizeof(*rp));

	cur_faults_count = MIN(sizeof(cur_faults), sizeof(some_faults));
	memcpy(cur_faults, some_faults, cur_faults_count);
	net_buf_simple_add_mem(&buf, cur_faults, cur_faults_count);
	rp->cur_faults_count = cur_faults_count;

	reg_faults_count = MIN(sizeof(reg_faults), sizeof(some_faults));
	memcpy(reg_faults, some_faults, reg_faults_count);
	net_buf_simple_add_mem(&buf, reg_faults, reg_faults_count);
	rp->reg_faults_count = reg_faults_count;

	bt_mesh_health_srv_fault_update(&elements[0]);

	tester_send(BTP_SERVICE_ID_MESH, MESH_HEALTH_GENERATE_FAULTS,
		    CONTROLLER_INDEX, buf.data, buf.len);
}

static void health_clear_faults(uint8_t *data, uint16_t len)
{
	LOG_DBG("");

	(void)memset(cur_faults, 0, sizeof(cur_faults));
	(void)memset(reg_faults, 0, sizeof(reg_faults));

	bt_mesh_health_srv_fault_update(&elements[0]);

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_HEALTH_CLEAR_FAULTS,
		   CONTROLLER_INDEX, BTP_STATUS_SUCCESS);
}

static void model_send(uint8_t *data, uint16_t len)
{
	struct mesh_model_send_cmd *cmd = (void *) data;
	NET_BUF_SIMPLE_DEFINE(msg, UINT8_MAX);
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.app_idx = BT_MESH_KEY_DEV,
		.addr = sys_le16_to_cpu(cmd->dst),
		.send_ttl = cmd->ttl,
	};
	struct bt_mesh_model *model = NULL;
	int err, i;
	uint16_t src = sys_le16_to_cpu(cmd->src);

	/* Lookup source address */
	for (i = 0; i < ARRAY_SIZE(model_bound); i++) {
		if (bt_mesh_model_elem(model_bound[i].model)->addr == src) {
			model = model_bound[i].model;
			ctx.app_idx = model_bound[i].appkey_idx;

			break;
		}
	}

	if (!model) {
		LOG_ERR("Model not found");
		err = -EINVAL;
		goto fail;
	}

	LOG_DBG("src 0x%04x dst 0x%04x model %p payload_len %d", src,
		ctx.addr, model, cmd->payload_len);

	net_buf_simple_add_mem(&msg, cmd->payload, cmd->payload_len);

	err = bt_mesh_model_send(model, &ctx, &msg, NULL, NULL);
	if (err) {
		LOG_ERR("Failed to send (err %d)", err);
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_MODEL_SEND, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

#if defined(CONFIG_BT_TESTING)
static void lpn_subscribe(uint8_t *data, uint16_t len)
{
	struct mesh_lpn_subscribe_cmd *cmd = (void *) data;
	uint16_t address = sys_le16_to_cpu(cmd->address);
	int err;

	LOG_DBG("address 0x%04x", address);

	err = bt_test_mesh_lpn_group_add(address);
	if (err) {
		LOG_ERR("Failed to subscribe (err %d)", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_LPN_SUBSCRIBE, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void lpn_unsubscribe(uint8_t *data, uint16_t len)
{
	struct mesh_lpn_unsubscribe_cmd *cmd = (void *) data;
	uint16_t address = sys_le16_to_cpu(cmd->address);
	int err;

	LOG_DBG("address 0x%04x", address);

	err = bt_test_mesh_lpn_group_remove(&address, 1);
	if (err) {
		LOG_ERR("Failed to unsubscribe (err %d)", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_LPN_UNSUBSCRIBE, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void rpl_clear(uint8_t *data, uint16_t len)
{
	int err;

	LOG_DBG("");

	err = bt_test_mesh_rpl_clear();
	if (err) {
		LOG_ERR("Failed to clear RPL (err %d)", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_RPL_CLEAR, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}
#endif /* CONFIG_BT_TESTING */

static void proxy_identity_enable(uint8_t *data, uint16_t len)
{
	int err;

	LOG_DBG("");

	err = bt_mesh_proxy_identity_enable();
	if (err) {
		LOG_ERR("Failed to enable proxy identity (err %d)", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_PROXY_IDENTITY, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void sar_transmitter_get(uint8_t *data, uint16_t len)
{
	struct mesh_sar_transmitter_get_cmd *cmd = (void *)data;
	struct bt_mesh_sar_tx rsp;
	int err;

	LOG_DBG("");

	bt_mesh_sar_cfg_cli_timeout_set(5000);

	err = bt_mesh_sar_cfg_cli_transmitter_get(
		&sar_cfg_cli, net_key_idx, sys_le16_to_cpu(cmd->dst), &rsp);
	if (err) {
		LOG_ERR("err=%d", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_SAR_TRANSMITTER_GET,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void sar_transmitter_set(uint8_t *data, uint16_t len)
{
	struct mesh_sar_transmitter_set_cmd *cmd = (void *)data;
	struct bt_mesh_sar_tx set, rsp;
	int err;

	LOG_DBG("");

	bt_mesh_sar_cfg_cli_timeout_set(5000);

	set.seg_int_step = cmd->tx.seg_int_step;
	set.unicast_retrans_count = cmd->tx.unicast_retrans_count;
	set.unicast_retrans_int_inc = cmd->tx.unicast_retrans_int_inc;
	set.unicast_retrans_int_step = cmd->tx.unicast_retrans_int_step;
	set.unicast_retrans_without_prog_count =
		cmd->tx.unicast_retrans_without_prog_count;
	set.multicast_retrans_count = cmd->tx.multicast_retrans_count;
	set.multicast_retrans_int = cmd->tx.multicast_retrans_int;

	err = bt_mesh_sar_cfg_cli_transmitter_set(&sar_cfg_cli, net_key_idx,
						  sys_le16_to_cpu(cmd->dst),
						  &set, &rsp);
	if (err) {
		LOG_ERR("err=%d", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_SAR_TRANSMITTER_SET,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void sar_receiver_get(uint8_t *data, uint16_t len)
{
	struct mesh_sar_receiver_get_cmd *cmd = (void *)data;
	struct bt_mesh_sar_rx rsp;
	int err;

	LOG_DBG("");

	err = bt_mesh_sar_cfg_cli_receiver_get(&sar_cfg_cli, net_key_idx,
					       sys_le16_to_cpu(cmd->dst), &rsp);
	if (err) {
		LOG_ERR("err=%d", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_SAR_RECEIVER_GET, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void sar_receiver_set(uint8_t *data, uint16_t len)
{
	struct mesh_sar_receiver_set_cmd *cmd = (void *)data;
	struct bt_mesh_sar_rx set, rsp;
	int err;

	LOG_DBG("");

	set.ack_delay_inc = cmd->rx.ack_delay_inc;
	set.ack_retrans_count = cmd->rx.ack_retrans_count;
	set.discard_timeout = cmd->rx.discard_timeout;
	set.seg_thresh = cmd->rx.seg_thresh;
	set.rx_seg_int_step = cmd->rx.rx_seg_int_step;

	err = bt_mesh_sar_cfg_cli_receiver_set(&sar_cfg_cli, net_key_idx,
					       sys_le16_to_cpu(cmd->dst), &set,
					       &rsp);
	if (err) {
		LOG_ERR("err=%d", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_SAR_RECEIVER_SET, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void large_comp_data_get(uint8_t *data, uint16_t len)
{
	struct mesh_large_comp_data_get_cmd *cmd = (void *) data;
	NET_BUF_SIMPLE_DEFINE(comp, BT_MESH_TX_SDU_MAX);
	int err;

	err = bt_mesh_large_comp_data_get(sys_le16_to_cpu(cmd->net_idx),
				    sys_le16_to_cpu(cmd->addr), cmd->page,
				    sys_le16_to_cpu(cmd->offset), &comp);
	if (err) {
		LOG_ERR("Large Composition Data Get failed (err %d)", err);

		tester_rsp(BTP_SERVICE_ID_MESH, MESH_LARGE_COMP_DATA_GET,
			   CONTROLLER_INDEX, BTP_STATUS_FAILED);
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_LARGE_COMP_DATA_GET,
		    CONTROLLER_INDEX, comp.data, comp.len);
}

static void models_metadata_get(uint8_t *data, uint16_t len)
{
	struct mesh_models_metadata_get_cmd *cmd = (void *) data;
	NET_BUF_SIMPLE_DEFINE(metadata, BT_MESH_TX_SDU_MAX);
	int err;

	err = bt_mesh_models_metadata_get(sys_le16_to_cpu(cmd->net_idx),
					  sys_le16_to_cpu(cmd->addr), cmd->page,
					  sys_le16_to_cpu(cmd->offset), &metadata);

	if (err) {
		LOG_ERR("Models Metadata Get failed (err %d)", err);

		tester_rsp(BTP_SERVICE_ID_MESH, MESH_MODELS_METADATA_GET,
			   CONTROLLER_INDEX, BTP_STATUS_FAILED);
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_MODELS_METADATA_GET,
		    CONTROLLER_INDEX, metadata.data, metadata.len);
}

static void composition_data_get(uint8_t *data, uint16_t len)
{
	struct mesh_comp_data_get_cmd *cmd = (void *)data;
	uint8_t page;
	struct net_buf_simple *comp = NET_BUF_SIMPLE(128);
	int err;

	LOG_DBG("");

	bt_mesh_cfg_cli_timeout_set(10 * MSEC_PER_SEC);

	net_buf_simple_init(comp, 0);

	err = bt_mesh_cfg_cli_comp_data_get(cmd->net_idx, cmd->address, cmd->page,
					&page, comp);
	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_COMP_DATA_GET, CONTROLLER_INDEX,
		    comp->data, comp->len);
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_COMP_DATA_GET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void change_prepare(uint8_t *data, uint16_t len)
{
	int err;
	uint8_t status = BTP_STATUS_SUCCESS;

	LOG_DBG("");

	err = bt_mesh_comp_change_prepare();
	if (err < 0) {
		status = BTP_STATUS_FAILED;
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_COMP_CHANGE_PREPARE, BTP_INDEX_NONE,
		   status);
}

static void set_comp_alt(uint8_t *data, uint16_t len)
{
	default_comp = false;

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_SET_COMP_ALT, BTP_INDEX_NONE,
		   BTP_STATUS_SUCCESS);
}
static void config_krp_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_krp_get_cmd *cmd = (void *)data;
	struct net_buf_simple *buf = NET_BUF_SIMPLE(2);
	uint8_t status;
	uint8_t phase;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_krp_get(cmd->net_idx, cmd->address, cmd->key_net_idx, &status,
				      &phase);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	net_buf_simple_init(buf, 0);
	net_buf_simple_add_u8(buf, status);
	net_buf_simple_add_u8(buf, phase);

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_KRP_GET, CONTROLLER_INDEX, buf->data, buf->len);
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_KRP_GET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_krp_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_krp_set_cmd *cmd = (void *)data;
	struct net_buf_simple *buf = NET_BUF_SIMPLE(2);
	uint8_t status;
	uint8_t phase;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_krp_set(cmd->net_idx, cmd->address, cmd->key_net_idx, cmd->transition,
				  &status, &phase);
	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	net_buf_simple_init(buf, 0);
	net_buf_simple_add_u8(buf, status);
	net_buf_simple_add_u8(buf, phase);

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_KRP_SET, CONTROLLER_INDEX, buf->data, buf->len);
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_KRP_SET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_beacon_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_val_get_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_beacon_get(cmd->net_idx, cmd->address, &status);
	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_BEACON_GET, CONTROLLER_INDEX,
		    &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_BEACON_GET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_beacon_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_beacon_set_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_beacon_set(cmd->net_idx, cmd->address, cmd->val,
				     &status);
	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_BEACON_SET, CONTROLLER_INDEX,
		    &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_BEACON_SET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_default_ttl_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_val_get_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");
	err = bt_mesh_cfg_cli_ttl_get(cmd->net_idx, cmd->address, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_DEFAULT_TTL_GET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_DEFAULT_TTL_GET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_default_ttl_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_default_ttl_set_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");
	err = bt_mesh_cfg_cli_ttl_set(cmd->net_idx, cmd->address, cmd->val,
				  &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_DEFAULT_TTL_SET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_DEFAULT_TTL_SET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_gatt_proxy_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_val_get_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");
	err = bt_mesh_cfg_cli_gatt_proxy_get(cmd->net_idx, cmd->address, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_GATT_PROXY_GET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_GATT_PROXY_GET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_gatt_proxy_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_gatt_proxy_set_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");
	err = bt_mesh_cfg_cli_gatt_proxy_set(cmd->net_idx, cmd->address, cmd->val,
					 &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_GATT_PROXY_SET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_GATT_PROXY_SET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_friend_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_val_get_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");
	err = bt_mesh_cfg_cli_friend_get(cmd->net_idx, cmd->address, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_FRIEND_GET, CONTROLLER_INDEX,
		    &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_FRIEND_GET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_friend_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_friend_set_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");
	err = bt_mesh_cfg_cli_friend_set(cmd->net_idx, cmd->address, cmd->val,
				     &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_FRIEND_SET, CONTROLLER_INDEX,
		    &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_FRIEND_SET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_relay_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_val_get_cmd *cmd = (void *)data;
	uint8_t status;
	uint8_t transmit;
	int err;

	LOG_DBG("");
	err = bt_mesh_cfg_cli_relay_get(cmd->net_idx, cmd->address, &status,
				    &transmit);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_RELAY_GET, CONTROLLER_INDEX,
		    &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_RELAY_GET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_relay_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_relay_set_cmd *cmd = (void *)data;
	uint8_t status;
	uint8_t transmit;
	int err;

	LOG_DBG("");
	err = bt_mesh_cfg_cli_relay_set(cmd->net_idx, cmd->address, cmd->new_relay,
				    cmd->new_transmit, &status, &transmit);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_RELAY_SET, CONTROLLER_INDEX,
		    &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_RELAY_SET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_mod_pub_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_pub_get_cmd *cmd = (void *)data;
	struct bt_mesh_cfg_cli_mod_pub pub;
	uint8_t status;
	int err;

	LOG_DBG("");
	err = bt_mesh_cfg_cli_mod_pub_get(cmd->net_idx, cmd->address,
				      cmd->elem_address, cmd->model_id, &pub,
				      &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_PUB_GET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_PUB_GET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_mod_pub_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_pub_set_cmd *cmd = (void *)data;
	uint8_t status;
	struct bt_mesh_cfg_cli_mod_pub pub;
	int err;

	LOG_DBG("");

	pub.addr = cmd->pub_addr;
	pub.uuid = NULL;
	pub.app_idx = cmd->app_idx;
	pub.cred_flag = cmd->cred_flag;
	pub.ttl = cmd->ttl;
	pub.period = cmd->period;
	pub.transmit = cmd->transmit;

	err = bt_mesh_cfg_cli_mod_pub_set(cmd->net_idx, cmd->address,
				      cmd->elem_address, cmd->model_id, &pub,
				      &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_PUB_SET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_PUB_SET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_mod_pub_va_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_pub_va_set_cmd *cmd = (void *)data;
	uint8_t status;
	struct bt_mesh_cfg_cli_mod_pub pub;
	int err;

	LOG_DBG("");

	pub.uuid = cmd->uuid;
	pub.app_idx = cmd->app_idx;
	pub.cred_flag = cmd->cred_flag;
	pub.ttl = cmd->ttl;
	pub.period = cmd->period;
	pub.transmit = cmd->transmit;

	err = bt_mesh_cfg_cli_mod_pub_set(cmd->net_idx, cmd->address,
				      cmd->elem_address, cmd->model_id,
				      &pub, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_PUB_VA_SET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_PUB_VA_SET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_mod_sub_add(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_sub_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_sub_add(cmd->net_idx, cmd->address,
				      cmd->elem_address, cmd->sub_addr,
				      cmd->model_id, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_ADD,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_ADD, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_mod_sub_ovw(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_sub_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_sub_overwrite(cmd->net_idx, cmd->address,
					    cmd->elem_address, cmd->sub_addr,
					    cmd->model_id, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_OVW,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_OVW, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_mod_sub_del(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_sub_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_sub_del(cmd->net_idx, cmd->address,
				      cmd->elem_address, cmd->sub_addr,
				      cmd->model_id, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_DEL,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_DEL, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_mod_sub_del_all(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_sub_del_all_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_sub_del_all(cmd->net_idx, cmd->address,
					  cmd->elem_address, cmd->model_id,
					  &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_DEL_ALL,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_DEL_ALL, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_mod_sub_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_sub_get_cmd *cmd = (void *)data;
	uint8_t status;
	int16_t subs;
	size_t sub_cn;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_sub_get(cmd->net_idx, cmd->address,
				      cmd->elem_address, cmd->model_id, &status,
				      &subs, &sub_cn);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_GET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_GET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_mod_sub_get_vnd(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_sub_get_vnd_cmd *cmd = (void *)data;
	uint8_t status;
	uint16_t subs;
	size_t sub_cn;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_sub_get_vnd(cmd->net_idx, cmd->address,
					  cmd->elem_address, cmd->model_id,
					  cmd->cid, &status, &subs, &sub_cn);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_GET_VND,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_GET_VND, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_mod_sub_va_add(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_sub_va_cmd *cmd = (void *)data;
	uint8_t status;
	uint16_t virt_addr_rcv;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_sub_va_add(cmd->net_idx, cmd->address,
					 cmd->elem_address, cmd->uuid,
					 cmd->model_id, &virt_addr_rcv,
					 &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_VA_ADD,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_VA_ADD, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_mod_sub_va_del(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_sub_va_cmd *cmd = (void *)data;
	uint8_t status;
	uint16_t virt_addr_rcv;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_sub_va_del(cmd->net_idx, cmd->address,
					 cmd->elem_address, cmd->uuid,
					 cmd->model_id, &virt_addr_rcv,
					 &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_VA_DEL,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_VA_DEL, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_mod_sub_va_ovw(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_sub_va_cmd *cmd = (void *)data;
	uint8_t status;
	uint16_t virt_addr_rcv;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_sub_va_overwrite(cmd->net_idx, cmd->address,
					       cmd->elem_address,
					       cmd->uuid, cmd->model_id,
					       &virt_addr_rcv, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_VA_OVW,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_SUB_VA_OVW, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_netkey_add(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_netkey_add_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_net_key_add(cmd->net_idx, cmd->address,
				      cmd->net_key_idx, cmd->net_key, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_NETKEY_ADD, CONTROLLER_INDEX,
		    &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_NETKEY_ADD, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_netkey_update(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_netkey_add_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_net_key_update(cmd->net_idx, cmd->address,
					 cmd->net_key_idx, cmd->net_key,
					 &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_NETKEY_UPDATE,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_NETKEY_UPDATE, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_netkey_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_val_get_cmd *cmd = (void *)data;
	uint16_t keys;
	size_t key_cnt;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_net_key_get(cmd->net_idx, cmd->address, &keys,
				      &key_cnt);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_NETKEY_GET, CONTROLLER_INDEX,
		    (uint8_t *)&keys, key_cnt);
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_NETKEY_GET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_netkey_del(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_netkey_del_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_net_key_del(cmd->net_idx, cmd->address,
				      cmd->net_key_idx, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_NETKEY_DEL, CONTROLLER_INDEX,
		    &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_NETKEY_DEL, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_appkey_add(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_appkey_add_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_app_key_add(cmd->net_idx, cmd->address,
				      cmd->net_key_idx, cmd->app_key_idx,
				      cmd->app_key, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_APPKEY_ADD, CONTROLLER_INDEX,
		    &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_APPKEY_ADD, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_appkey_update(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_appkey_add_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_app_key_update(cmd->net_idx, cmd->address,
					 cmd->net_key_idx, cmd->app_key_idx,
					 cmd->app_key, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_APPKEY_UPDATE,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_APPKEY_UPDATE, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_appkey_del(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_appkey_del_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_app_key_del(cmd->net_idx, cmd->address,
				      cmd->net_key_idx, cmd->app_key_idx,
				      &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_APPKEY_DEL, CONTROLLER_INDEX,
		    &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_APPKEY_DEL, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_appkey_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_appkey_get_cmd *cmd = (void *)data;
	uint8_t status;
	uint16_t keys;
	size_t key_cnt;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_app_key_get(cmd->net_idx, cmd->address,
				      cmd->net_key_idx, &status, &keys,
				      &key_cnt);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_APPKEY_GET, CONTROLLER_INDEX,
		    &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_APPKEY_GET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_model_app_bind(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_app_bind_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_app_bind(cmd->net_idx, cmd->address,
				       cmd->elem_address, cmd->app_key_idx,
				       cmd->mod_id, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_APP_BIND,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_APP_BIND, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_model_app_bind_vnd(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_app_bind_vnd_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_app_bind_vnd(cmd->net_idx, cmd->address,
					   cmd->elem_address, cmd->app_key_idx,
					   cmd->mod_id, cmd->cid, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_APP_BIND_VND,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_APP_BIND_VND, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_model_app_unbind(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_app_bind_cmd *cmd = (void *)data;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_app_unbind(cmd->net_idx, cmd->address,
					 cmd->elem_address, cmd->app_key_idx,
					 cmd->mod_id, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_APP_UNBIND,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_APP_UNBIND, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_model_app_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_app_get_cmd *cmd = (void *)data;
	uint8_t status;
	uint16_t apps;
	size_t app_cnt;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_app_get(cmd->net_idx, cmd->address,
				      cmd->elem_address, cmd->mod_id, &status,
				      &apps, &app_cnt);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_APP_GET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_APP_GET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_model_app_vnd_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_model_app_get_cmd *cmd = (void *)data;
	uint8_t status;
	uint16_t apps;
	size_t app_cnt;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_mod_app_get_vnd(cmd->net_idx, cmd->address,
					  cmd->elem_address, cmd->mod_id,
					  cmd->cid, &status, &apps, &app_cnt);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_APP_VND_GET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_MODEL_APP_VND_GET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_hb_pub_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_heartbeat_pub_set_cmd *cmd = (void *)data;
	uint8_t status;
	struct bt_mesh_cfg_cli_hb_pub pub;
	int err;

	LOG_DBG("");

	pub.net_idx = cmd->net_key_idx;
	pub.dst = cmd->destination;
	pub.count = cmd->count_log;
	pub.period = cmd->period_log;
	pub.ttl = cmd->ttl;
	pub.feat = cmd->features;

	err = bt_mesh_cfg_cli_hb_pub_set(cmd->net_idx, cmd->address, &pub, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_HEARTBEAT_PUB_SET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_HEARTBEAT_PUB_SET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_hb_pub_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_val_get_cmd *cmd = (void *)data;
	uint8_t status;
	struct bt_mesh_cfg_cli_hb_pub pub;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_hb_pub_get(cmd->net_idx, cmd->address, &pub, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_HEARTBEAT_PUB_GET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_HEARTBEAT_PUB_GET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_hb_sub_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_heartbeat_sub_set_cmd *cmd = (void *)data;
	uint8_t status;
	struct bt_mesh_cfg_cli_hb_sub sub;
	int err;

	LOG_DBG("");

	sub.src = cmd->source;
	sub.dst = cmd->destination;
	sub.period = cmd->period_log;

	err = bt_mesh_cfg_cli_hb_sub_set(cmd->net_idx, cmd->address, &sub, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_HEARTBEAT_SUB_SET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_HEARTBEAT_SUB_SET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_hb_sub_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_val_get_cmd *cmd = (void *)data;
	uint8_t status;
	struct bt_mesh_cfg_cli_hb_sub sub;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_hb_sub_get(cmd->net_idx, cmd->address, &sub, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_HEARTBEAT_SUB_GET,
		    CONTROLLER_INDEX, &status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_HEARTBEAT_SUB_GET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_net_trans_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_val_get_cmd *cmd = (void *)data;
	uint8_t transmit;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_net_transmit_get(cmd->net_idx, cmd->address,
					   &transmit);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_NET_TRANS_GET,
		    CONTROLLER_INDEX, &transmit, sizeof(transmit));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_NET_TRANS_GET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_net_trans_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_net_trans_set_cmd *cmd = (void *)data;
	uint8_t transmit;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_net_transmit_set(cmd->net_idx, cmd->address,
					   cmd->transmit, &transmit);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_NET_TRANS_SET,
		    CONTROLLER_INDEX, &transmit, sizeof(transmit));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_NET_TRANS_SET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void config_node_identity_set(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_node_idt_set_cmd *cmd = (void *)data;
	struct net_buf_simple *buf = NET_BUF_SIMPLE(2);
	uint8_t identity;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_node_identity_set(cmd->net_idx, cmd->address,
					    cmd->net_key_idx, cmd->new_identity,
					    &status, &identity);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	net_buf_simple_init(buf, 0);
	net_buf_simple_add_u8(buf, status);
	net_buf_simple_add_u8(buf, identity);

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_NODE_IDT_SET,
		    CONTROLLER_INDEX, buf->data, buf->len);
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_NODE_IDT_SET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_node_identity_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_node_idt_get_cmd *cmd = (void *)data;
	struct net_buf_simple *buf = NET_BUF_SIMPLE(2);
	uint8_t identity;
	uint8_t status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_node_identity_get(cmd->net_idx, cmd->address,
					    cmd->net_key_idx, &status,
					    &identity);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	net_buf_simple_init(buf, 0);
	net_buf_simple_add_u8(buf, status);
	net_buf_simple_add_u8(buf, identity);

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_NODE_IDT_GET,
		    CONTROLLER_INDEX, buf->data, buf->len);
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_NODE_IDT_GET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_node_reset(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_node_reset_cmd *cmd = (void *)data;
	bool status;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_node_reset(cmd->net_idx, cmd->address, &status);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_NODE_RESET, CONTROLLER_INDEX,
		    (uint8_t *)&status, sizeof(status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_NODE_RESET, CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void config_lpn_timeout_get(uint8_t *data, uint16_t len)
{
	struct mesh_cfg_lpn_timeout_cmd *cmd = (void *)data;
	int32_t polltimeout;
	int err;

	LOG_DBG("");

	err = bt_mesh_cfg_cli_lpn_timeout_get(cmd->net_idx, cmd->address,
					  cmd->unicast_addr, &polltimeout);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MESH, MESH_CFG_LPN_TIMEOUT_GET,
		    CONTROLLER_INDEX, (uint8_t *)&polltimeout,
		    sizeof(polltimeout));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_CFG_LPN_TIMEOUT_GET, CONTROLLER_INDEX,
		   BTP_STATUS_FAILED);
}

static void health_fault_get(uint8_t *data, uint16_t len)
{
	struct mesh_health_fault_get_cmd *cmd = (void *)data;
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.addr = cmd->address,
		.app_idx = cmd->app_idx,
	};
	uint8_t test_id;
	size_t fault_count = 16;
	uint8_t faults[fault_count];
	int err;

	LOG_DBG("");

	err = bt_mesh_health_cli_fault_get(&health_cli, &ctx, cmd->cid, &test_id, faults,
					   &fault_count);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_HEALTH_FAULT_GET, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void health_fault_clear(uint8_t *data, uint16_t len)
{
	struct mesh_health_fault_clear_cmd *cmd = (void *)data;
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.addr = cmd->address,
		.app_idx = cmd->app_idx,
	};
	uint8_t test_id;
	size_t fault_count = 16;
	uint8_t faults[fault_count];
	int err;

	LOG_DBG("");

	if (cmd->ack) {
		err = bt_mesh_health_cli_fault_clear(&health_cli, &ctx, cmd->cid, &test_id, faults,
						     &fault_count);
	} else {
		err = bt_mesh_health_cli_fault_clear_unack(&health_cli, &ctx, cmd->cid);
	}

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	if (cmd->ack) {
		tester_send(BTP_SERVICE_ID_MESH, MESH_HEALTH_FAULT_CLEAR,
			    CONTROLLER_INDEX, &test_id, sizeof(test_id));
		return;
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_HEALTH_FAULT_CLEAR,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void health_fault_test(uint8_t *data, uint16_t len)
{
	struct mesh_health_fault_test_cmd *cmd = (void *)data;
	struct net_buf_simple *buf = NET_BUF_SIMPLE(19);
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.addr = cmd->address,
		.app_idx = cmd->app_idx,
	};
	size_t fault_count = 16;
	uint8_t faults[fault_count];
	uint8_t test_id;
	uint16_t cid;
	int err;

	LOG_DBG("");

	test_id = cmd->test_id;
	cid = cmd->cid;

	if (cmd->ack) {
		err = bt_mesh_health_cli_fault_test(&health_cli, &ctx, cid, test_id, faults,
						    &fault_count);
	} else {
		err = bt_mesh_health_cli_fault_test_unack(&health_cli, &ctx, cid, test_id);
	}

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	if (cmd->ack) {
		net_buf_simple_init(buf, 0);
		net_buf_simple_add_u8(buf, test_id);
		net_buf_simple_add_le16(buf, cid);
		net_buf_simple_add_mem(buf, faults, fault_count);

		tester_send(BTP_SERVICE_ID_MESH, MESH_HEALTH_FAULT_TEST,
			    CONTROLLER_INDEX, buf->data, buf->len);
		return;
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_HEALTH_FAULT_TEST,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void health_period_get(uint8_t *data, uint16_t len)
{
	struct mesh_health_period_get_cmd *cmd = (void *)data;
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.addr = cmd->address,
		.app_idx = cmd->app_idx,
	};
	uint8_t divisor;
	int err;

	LOG_DBG("");

	err = bt_mesh_health_cli_period_get(&health_cli, &ctx, &divisor);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_HEALTH_PERIOD_GET,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void health_period_set(uint8_t *data, uint16_t len)
{
	struct mesh_health_period_set_cmd *cmd = (void *)data;
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.addr = cmd->address,
		.app_idx = cmd->app_idx,
	};
	uint8_t updated_divisor;
	int err;

	LOG_DBG("");

	if (cmd->ack) {
		err = bt_mesh_health_cli_period_set(&health_cli, &ctx, cmd->divisor,
						    &updated_divisor);
	} else {
		err = bt_mesh_health_cli_period_set_unack(&health_cli, &ctx, cmd->divisor);
	}

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	if (cmd->ack) {
		tester_send(BTP_SERVICE_ID_MESH, MESH_HEALTH_PERIOD_SET,
			    CONTROLLER_INDEX, &updated_divisor,
			    sizeof(updated_divisor));
		return;
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_HEALTH_PERIOD_SET,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void health_attention_get(uint8_t *data, uint16_t len)
{
	struct mesh_health_attention_get_cmd *cmd = (void *)data;
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.addr = cmd->address,
		.app_idx = cmd->app_idx,
	};
	uint8_t attention;
	int err;

	LOG_DBG("");

	err = bt_mesh_health_cli_attention_get(&health_cli, &ctx, &attention);

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_HEALTH_ATTENTION_GET,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void health_attention_set(uint8_t *data, uint16_t len)
{
	struct mesh_health_attention_set_cmd *cmd = (void *)data;
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.addr = cmd->address,
		.app_idx = cmd->app_idx,
	};
	uint8_t updated_attention;
	int err;

	LOG_DBG("");

	if (cmd->ack) {
		err = bt_mesh_health_cli_attention_set(&health_cli, &ctx, cmd->attention,
						       &updated_attention);
	} else {
		err = bt_mesh_health_cli_attention_set_unack(&health_cli, &ctx, cmd->attention);
	}

	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	if (cmd->ack) {
		tester_send(BTP_SERVICE_ID_MESH, MESH_HEALTH_ATTENTION_SET,
			    CONTROLLER_INDEX, &updated_attention,
			    sizeof(updated_attention));
		return;
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_HEALTH_ATTENTION_SET,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

#if defined(CONFIG_BT_MESH_RPR_CLI)
static void rpr_scan_start(uint8_t *data, uint16_t len)
{
	struct rpr_scan_start_cmd *cmd = (void *)data;

	struct bt_mesh_rpr_scan_status rsp;
	const struct bt_mesh_rpr_node srv = {
		.addr = cmd->dst,
		.net_idx = net.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	uint8_t uuid[16] = {0};
	int err;

	err = bt_mesh_rpr_scan_start(&rpr_cli, &srv,
				     memcmp(uuid, cmd->uuid, 16) ? cmd->uuid : NULL,
				     cmd->timeout,
				     BT_MESH_RPR_SCAN_MAX_DEVS_ANY, &rsp);

	if (err) {
		LOG_ERR("Scan start failed: %d", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_RPR_SCAN_START,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void rpr_ext_scan_start(uint8_t *data, uint16_t len)
{
	struct rpr_ext_scan_start_cmd *cmd = (void *)data;
	const struct bt_mesh_rpr_node srv = {
		.addr = cmd->dst,
		.net_idx = net.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_rpr_scan_start_ext(&rpr_cli, &srv, cmd->uuid,
					 cmd->timeout, cmd->ad_types,
					 cmd->ad_count);
	if (err) {
		LOG_ERR("Scan start failed: %d", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_RPR_EXT_SCAN_START,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void rpr_scan_caps_get(uint8_t *data, uint16_t len)
{
	struct rpr_scan_caps_get_cmd *cmd = (void *)data;
	struct bt_mesh_rpr_caps caps;
	const struct bt_mesh_rpr_node srv = {
		.addr = cmd->dst,
		.net_idx = net.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_rpr_scan_caps_get(&rpr_cli, &srv, &caps);
	if (err) {
		LOG_ERR("Scan capabilities get failed: %d", err);
		goto fail;
	}

	LOG_DBG("Remote Provisioning scan capabilities of 0x%04x:",
		net.dst);
	LOG_DBG("\tMax devices:     %u", caps.max_devs);
	LOG_DBG("\tActive scanning: %s",
		    caps.active_scan ? "true" : "false");
fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_RPR_SCAN_CAPS_GET,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void rpr_scan_get(uint8_t *data, uint16_t len)
{
	struct rpr_scan_get_cmd *cmd = (void *)data;
	struct bt_mesh_rpr_scan_status rsp;
	const struct bt_mesh_rpr_node srv = {
		.addr = cmd->dst,
		.net_idx = net.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_rpr_scan_get(&rpr_cli, &srv, &rsp);
	if (err) {
		LOG_ERR("Scan get failed: %d", err);
		goto fail;
	}

	LOG_DBG("Remote Provisioning scan on 0x%04x:", cmd->dst);
	LOG_DBG("\tStatus:         %u", rsp.status);
	LOG_DBG("\tScan type:      %u", rsp.scan);
	LOG_DBG("\tMax devices:    %u", rsp.max_devs);
	LOG_DBG("\tRemaining time: %u", rsp.timeout);
fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_RPR_SCAN_GET,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void rpr_scan_stop(uint8_t *data, uint16_t len)
{
	struct rpr_scan_stop_cmd *cmd = (void *)data;
	struct bt_mesh_rpr_scan_status rsp;
	const struct bt_mesh_rpr_node srv = {
		.addr = cmd->dst,
		.net_idx = net.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_rpr_scan_stop(&rpr_cli, &srv, &rsp);
	if (err || rsp.status) {
		LOG_DBG("Scan stop failed: %d %u", err, rsp.status);
		goto fail;
	}

	LOG_DBG("Remote Provisioning scan on 0x%04x stopped.",
		    net.dst);
fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_RPR_SCAN_STOP,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void rpr_link_get(uint8_t *data, uint16_t len)
{
	struct rpr_link_get_cmd *cmd = (void *)data;
	struct bt_mesh_rpr_link rsp;
	const struct bt_mesh_rpr_node srv = {
		.addr = cmd->dst,
		.net_idx = net.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_rpr_link_get(&rpr_cli, &srv, &rsp);
	if (err) {
		LOG_ERR("Link get failed: %d %u", err, rsp.status);
		goto fail;
	}

	LOG_DBG("Remote Provisioning Link on 0x%04x:", cmd->dst);
	LOG_DBG("\tStatus: %u", rsp.status);
	LOG_DBG("\tState:  %u", rsp.state);
fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_RPR_LINK_GET,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void rpr_link_close(uint8_t *data, uint16_t len)
{
	struct rpr_link_close_cmd *cmd = (void *)data;
	struct bt_mesh_rpr_link rsp;
	const struct bt_mesh_rpr_node srv = {
		.addr = cmd->dst,
		.net_idx = net.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_rpr_link_close(&rpr_cli, &srv, &rsp);
	if (err) {
		LOG_ERR("Link close failed: %d %u", err, rsp.status);
		goto fail;
	}

	LOG_DBG("Remote Provisioning Link on 0x%04x:", cmd->dst);
	LOG_DBG("\tStatus: %u", rsp.status);
	LOG_DBG("\tState:  %u", rsp.state);
fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_RPR_LINK_CLOSE,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void rpr_prov_remote(uint8_t *data, uint16_t len)
{
	struct rpr_prov_remote_cmd *cmd = (void *)data;
	struct bt_mesh_rpr_node srv = {
		.addr = cmd->dst,
		.net_idx = net.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = bt_mesh_provision_remote(&rpr_cli, &srv, cmd->uuid,
				       cmd->net_idx, cmd->addr);
	if (err) {
		LOG_ERR("Prov remote start failed: %d", err);
	}

	tester_rsp(BTP_SERVICE_ID_MESH, MESH_RPR_PROV_REMOTE,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void rpr_reprov_remote(uint8_t *data, uint16_t len)
{
	struct rpr_reprov_remote_cmd *cmd = (void *)data;
	struct bt_mesh_rpr_node srv = {
		.addr = cmd->dst,
		.net_idx = net.net_idx,
		.ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	if (!BT_MESH_ADDR_IS_UNICAST(cmd->addr)) {
		LOG_ERR("Must be a valid unicast address");
		err = -EINVAL;
		goto fail;
	}

	err = bt_mesh_reprovision_remote(&rpr_cli, &srv, cmd->addr,
					 cmd->comp_change);
	if (err) {
		LOG_ERR("Reprovisioning failed: %d", err);
	}
fail:
	tester_rsp(BTP_SERVICE_ID_MESH, MESH_RPR_REPROV_REMOTE,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}
#endif

#if defined(CONFIG_BT_MESH_DFD_SRV)
static struct {
	struct bt_mesh_dfu_target targets[32];
	size_t target_cnt;
	struct bt_mesh_blob_cli_inputs inputs;
} dfu_tx;

static void dfu_tx_prepare(void)
{
	sys_slist_init(&dfu_tx.inputs.targets);

	for (int i = 0; i < dfu_tx.target_cnt; i++) {
		/* Reset target context. */
		uint16_t addr = dfu_tx.targets[i].blob.addr;

		memset(&dfu_tx.targets[i].blob, 0,
		       sizeof(struct bt_mesh_blob_target));
		dfu_tx.targets[i].blob.addr = addr;

		sys_slist_append(&dfu_tx.inputs.targets,
				 &dfu_tx.targets[i].blob.n);
	}
}

static void dfu_target(uint8_t img_idx, uint16_t addr)
{
	if (dfu_tx.target_cnt == ARRAY_SIZE(dfu_tx.targets)) {
		LOG_ERR("No room.");
		return;
	}

	for (int i = 0; i < dfu_tx.target_cnt; i++) {
		if (dfu_tx.targets[i].blob.addr == addr) {
			LOG_ERR("Target 0x%04x already exists", addr);
			return;
		}
	}

	dfu_tx.targets[dfu_tx.target_cnt].blob.addr = addr;
	dfu_tx.targets[dfu_tx.target_cnt].img_idx = img_idx;
	sys_slist_append(&dfu_tx.inputs.targets,
			 &dfu_tx.targets[dfu_tx.target_cnt].blob.n);
	dfu_tx.target_cnt++;

	LOG_DBG("Added target 0x%04x", addr);
}
static void dfu_slot_add(size_t size, uint8_t *fwid, size_t fwid_len,
			 uint8_t *metadata, size_t metadata_len,
			 const char *uri)
{
	const struct bt_mesh_dfu_slot *slot;

	slot = bt_mesh_dfu_slot_add(size, fwid, fwid_len, metadata,
				    metadata_len, uri, strlen(uri));
	if (!slot) {
		LOG_ERR("Failed adding slot");
		return;
	}

	bt_mesh_dfu_slot_valid_set(slot, true);

	LOG_DBG("Slot added.");
}
static enum bt_mesh_dfu_iter dfu_img_cb(struct bt_mesh_dfu_cli *cli,
					struct bt_mesh_msg_ctx *ctx,
					uint8_t idx, uint8_t total,
					const struct bt_mesh_dfu_img *img,
					void *cb_data)
{
	char fwid[2 * CONFIG_BT_MESH_DFU_FWID_MAXLEN + 1];
	size_t len;

	idx = 0xff;

	if (img->fwid_len <= sizeof(fwid)) {
		len = bin2hex(img->fwid, img->fwid_len, fwid, sizeof(fwid));
	} else {
		LOG_ERR("FWID is too big");
		return BT_MESH_DFU_ITER_STOP;
	}

	fwid[len] = '\0';

	LOG_DBG("Image %u:", idx);
	LOG_DBG("\tFWID: ");
	if (img->uri) {
		LOG_DBG("\tURI:  ");
	}

	return BT_MESH_DFU_ITER_CONTINUE;
}

static void dfu_info_get(uint8_t *data, uint16_t len)
{
	struct mmdl_dfu_info_get_cmd *cmd = (void *)data;
	struct model_data *model_bound;
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};
	uint8_t max_count;
	int err = 0;

	LOG_DBG("");

	model_bound = lookup_model_bound(BT_MESH_MODEL_ID_DFU_CLI);
	if (!model_bound) {
		LOG_ERR("Model not found");
		err = -EINVAL;
		goto fail;
	}
	ctx.addr = model_bound->addr;
	ctx.app_idx = model_bound->appkey_idx;

	max_count = cmd->limit;

	err = bt_mesh_dfu_cli_imgs_get(&dfd_srv.dfu, &ctx, dfu_img_cb, NULL,
				       max_count);
	if (err) {
		LOG_ERR("ERR %d", err);
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MMDL, MMDL_DFU_INFO_GET, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void dfu_update_metadata_check(uint8_t *data, uint16_t len)
{
	struct mmdl_dfu_metadata_check_cmd *cmd = (void *)data;
	struct net_buf_simple *buf = NET_BUF_SIMPLE(3);
	const struct bt_mesh_dfu_slot *slot;
	struct model_data *model_bound;
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};
	struct bt_mesh_dfu_metadata_status rsp;
	uint8_t img_idx, slot_idx;
	size_t size;
	size_t fwid_len;
	size_t metadata_len;
	uint8_t fwid[CONFIG_BT_MESH_DFU_FWID_MAXLEN];
	uint8_t metadata[CONFIG_BT_MESH_DFU_METADATA_MAXLEN];
	const char *uri = "";
	int err;

	LOG_DBG("");

	model_bound = lookup_model_bound(BT_MESH_MODEL_ID_DFU_CLI);
	if (!model_bound) {
		LOG_ERR("Model not found");
		err = -EINVAL;
		goto fail;
	}

	ctx.addr = model_bound->addr;
	ctx.app_idx = model_bound->appkey_idx;
	img_idx = cmd->index;
	slot_idx = cmd->slot_idx;
	size = cmd->slot_size;
	fwid_len = cmd->fwid_len;
	metadata_len = cmd->metadata_len;

	if (cmd->data) {
		if ((metadata_len > 0) &&
		    (metadata_len < CONFIG_BT_MESH_DFU_METADATA_MAXLEN)) {
			memcpy(&metadata, cmd->data, metadata_len);
		}
	}

	dfu_slot_add(size, fwid, fwid_len, metadata, metadata_len, uri);

	slot = bt_mesh_dfu_slot_at(slot_idx);
	if (!slot) {
		LOG_ERR("No image in slot %u", slot_idx);
		goto fail;
	}

	err = bt_mesh_dfu_cli_metadata_check(&dfd_srv.dfu, &ctx, img_idx, slot,
					     &rsp);

	if (err) {
		LOG_ERR("ERR %d", err);
		goto fail;
	}

	net_buf_simple_init(buf, 0);
	net_buf_simple_add_u8(buf, rsp.idx);
	net_buf_simple_add_u8(buf, rsp.status);
	net_buf_simple_add_u8(buf, rsp.effect);
	tester_send(BTP_SERVICE_ID_MMDL, MMDL_DFU_UPDATE_METADATA_CHECK,
		    CONTROLLER_INDEX, buf->data, buf->len);
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MMDL, MMDL_DFU_UPDATE_METADATA_CHECK,
		   CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void dfu_firmware_update_get(uint8_t *data, uint16_t len)
{
	struct model_data *model_bound;
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};
	struct bt_mesh_dfu_target_status rsp;
	int err;

	LOG_DBG("");

	model_bound = lookup_model_bound(BT_MESH_MODEL_ID_DFU_CLI);
	if (!model_bound) {
		LOG_ERR("Model not found");
		err = -EINVAL;
		goto fail;
	}

	ctx.addr = model_bound->addr;
	ctx.app_idx = model_bound->appkey_idx;

	err = bt_mesh_dfu_cli_status_get(&dfd_srv.dfu, &ctx, &rsp);
	if (err) {
		LOG_ERR("err %d", err);
		goto fail;
	}

	tester_send(BTP_SERVICE_ID_MMDL, MMDL_DFU_FIRMWARE_UPDATE_GET,
		    CONTROLLER_INDEX, &rsp.status, sizeof(rsp.status));
	return;

fail:
	tester_rsp(BTP_SERVICE_ID_MMDL, MMDL_DFU_FIRMWARE_UPDATE_GET,
		   CONTROLLER_INDEX, BTP_STATUS_FAILED);
}

static void dfu_firmware_update_cancel(uint8_t *data, uint16_t len)
{
	struct model_data *model_bound;
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net.net_idx,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	LOG_DBG("");

	model_bound = lookup_model_bound(BT_MESH_MODEL_ID_DFU_CLI);
	if (!model_bound) {
		LOG_ERR("Model not found");
		err = -EINVAL;
		goto fail;
	}

	ctx.addr = model_bound->addr;
	ctx.app_idx = model_bound->appkey_idx;

	err = bt_mesh_dfu_cli_cancel(&dfd_srv.dfu, &ctx);
	if (err) {
		LOG_ERR("err %d", err);
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MMDL, MMDL_DFU_FIRMWARE_UPDATE_CANCEL,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void dfu_firmware_update_start(uint8_t *data, uint16_t len)
{
	struct mmdl_dfu_firmware_update_cmd *cmd = (void *)data;
	struct model_data *model_bound;
	struct bt_mesh_dfu_cli_xfer xfer;
	uint8_t addr_cnt;
	uint16_t addr = BT_MESH_ADDR_UNASSIGNED;
	uint8_t slot_idx;
	size_t size;
	size_t fwid_len;
	size_t metadata_len;
	uint8_t fwid[CONFIG_BT_MESH_DFU_FWID_MAXLEN];
	uint8_t metadata[CONFIG_BT_MESH_DFU_METADATA_MAXLEN];
	const char *uri = "";
	int err = 0;
	int i = 0;

	LOG_DBG("");

	model_bound = lookup_model_bound(BT_MESH_MODEL_ID_DFU_CLI);
	if (!model_bound) {
		LOG_ERR("Model not found");
		err = -EINVAL;
		goto fail;
	}

	struct bt_mesh_dfu_cli_xfer_blob_params blob = {
		.block_size_log = cmd->block_size,
		.chunk_size = cmd->chunk_size,
	};

	addr_cnt = cmd->addr_cnt;
	slot_idx = cmd->slot_idx;
	size = cmd->slot_size;
	fwid_len = cmd->fwid_len;
	metadata_len = cmd->metadata_len;
	xfer.mode = BT_MESH_BLOB_XFER_MODE_PUSH;
	xfer.blob_params = &blob;

	if (cmd->data) {
		if ((metadata_len > 0) &&
		    (metadata_len < CONFIG_BT_MESH_DFU_METADATA_MAXLEN)) {
			memcpy(&metadata, cmd->data, metadata_len);
		}
	}

	err = bt_mesh_dfu_slot_del_all();

	dfu_slot_add(size, fwid, fwid_len, metadata, metadata_len, uri);

	xfer.slot = bt_mesh_dfu_slot_at(slot_idx);
	if (!xfer.slot) {
		LOG_ERR("No image in slot %u", slot_idx);
		goto fail;
	}

	for (i = 0; i < addr_cnt; i++) {
		addr = cmd->data[metadata_len + 1 + i * sizeof(uint16_t)] |
			(cmd->data[metadata_len + i * sizeof(uint16_t)] << 8);
		dfu_target(slot_idx, addr);
	}

	dfu_tx_prepare();

	if (!dfu_tx.target_cnt) {
		LOG_ERR("No targets.");
		goto fail;
	}

	if (addr_cnt > 1) {
		dfu_tx.inputs.group = BT_MESH_ADDR_UNASSIGNED;
	} else {
		dfu_tx.inputs.group = addr;
	}

	dfu_tx.inputs.app_idx = model_bound->appkey_idx;
	dfu_tx.inputs.ttl = BT_MESH_TTL_DEFAULT;

	err = bt_mesh_dfu_cli_send(&dfd_srv.dfu, &dfu_tx.inputs, &dummy_blob_io, &xfer);

	if (err) {
		LOG_ERR("err %d", err);
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MMDL, MMDL_DFU_FIRMWARE_UPDATE_START,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void dfu_firmware_update_apply(uint8_t *data, uint16_t len)
{
	struct model_data *model_bound;
	int err;

	LOG_DBG("");

	model_bound = lookup_model_bound(BT_MESH_MODEL_ID_DFU_CLI);
	if (!model_bound) {
		LOG_ERR("Model not found");
		err = -EINVAL;
		goto fail;
	}

	err = bt_mesh_dfu_cli_apply(&dfd_srv.dfu);
	if (err) {
		LOG_ERR("err %d", err);
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MMDL, MMDL_DFU_FIRMWARE_UPDATE_APPLY,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}
#endif

#if defined(CONFIG_BT_MESH_BLOB_CLI) && !defined(CONFIG_BT_MESH_DFD_SRV)
static void blob_cli_inputs_prepare(uint16_t group, uint16_t app_idx)
{
	int i;

	blob_cli_xfer.inputs.ttl = BT_MESH_TTL_DEFAULT;
	blob_cli_xfer.inputs.group = group;
	blob_cli_xfer.inputs.app_idx = app_idx;
	sys_slist_init(&blob_cli_xfer.inputs.targets);

	for (i = 0; i < blob_cli_xfer.target_count; ++i) {
		/* Reset target context. */
		uint16_t addr = blob_cli_xfer.targets[i].addr;

		memset(&blob_cli_xfer.targets[i], 0,
		       sizeof(struct bt_mesh_blob_target));
		blob_cli_xfer.targets[i].addr = addr;

		sys_slist_append(&blob_cli_xfer.inputs.targets,
				 &blob_cli_xfer.targets[i].n);
	}
}

static int cmd_blob_target(uint16_t addr)
{
	struct bt_mesh_blob_target *t;

	if (blob_cli_xfer.target_count == ARRAY_SIZE(blob_cli_xfer.targets)) {
		LOG_ERR("No more room");
		return 0;
	}

	t = &blob_cli_xfer.targets[blob_cli_xfer.target_count];

	t->addr = addr;

	LOG_DBG("Added target 0x%04x", t->addr);

	blob_cli_xfer.target_count++;
	return 0;
}

static void blob_info_get(uint8_t *data, uint16_t len)
{
	struct mmdl_blob_info_get_cmd *cmd = (void *)data;
	struct model_data *model_bound;
	uint16_t group;
	int err;

	LOG_DBG("");

	model_bound = lookup_model_bound(BT_MESH_MODEL_ID_BLOB_CLI);
	if (!model_bound) {
		LOG_ERR("Model not found");
		err = -EINVAL;
		goto fail;
	}

	group = cmd->addr;

	err = cmd_blob_target(group);
	if (err) {
		LOG_ERR("err target %d", err);
		goto fail;
	}

	if (!blob_cli_xfer.target_count) {
		LOG_ERR("Failed: No targets");
		err = -EINVAL;
		goto fail;
	}

	blob_cli_inputs_prepare(group, model_bound->appkey_idx);

	err = bt_mesh_blob_cli_caps_get(&blob_cli, &blob_cli_xfer.inputs);

fail:
	tester_rsp(BTP_SERVICE_ID_MMDL, MMDL_BLOB_INFO_GET, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void blob_transfer_start(uint8_t *data, uint16_t len)
{
	struct mmdl_blob_transfer_start_cmd *cmd = (void *)data;
	struct model_data *model_bound;
	uint16_t group;
	int err;

	LOG_DBG("");

	model_bound = lookup_model_bound(BT_MESH_MODEL_ID_BLOB_CLI);
	if (!model_bound) {
		LOG_ERR("Model not found");
		err = -EINVAL;
		goto fail;
	}

	group = cmd->addr;

	err = cmd_blob_target(group);
	if (err) {
		LOG_ERR("err target %d", err);
		goto fail;
	}

	if (!blob_cli_xfer.target_count) {
		LOG_ERR("Failed: No targets");
		err = -EINVAL;
		goto fail;
	}
	blob_cli_xfer.xfer.id = cmd->id;
	blob_cli_xfer.xfer.size = cmd->size;
	blob_cli_xfer.xfer.block_size_log = cmd->block_size;
	blob_cli_xfer.xfer.chunk_size = cmd->chunk_size;

	if (blob_cli.caps.modes) {
		blob_cli_xfer.xfer.mode = blob_cli.caps.modes;
	} else {
		blob_cli_xfer.xfer.mode = BT_MESH_BLOB_XFER_MODE_PUSH;
	}
	blob_cli_inputs_prepare(group, model_bound->appkey_idx);

	if (cmd->timeout) {
		blob_cli_xfer.inputs.timeout_base = cmd->timeout;
	}

	err = bt_mesh_blob_cli_send(&blob_cli, &blob_cli_xfer.inputs,
				    &blob_cli_xfer.xfer, &dummy_blob_io);

fail:
	tester_rsp(BTP_SERVICE_ID_MMDL, MMDL_BLOB_TRANSFER_START,
		   CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void blob_transfer_cancel(uint8_t *data, uint16_t len)
{
	LOG_DBG("");

	bt_mesh_blob_cli_cancel(&blob_cli);

	tester_rsp(BTP_SERVICE_ID_MMDL, MMDL_BLOB_TRANSFER_CANCEL,
		   CONTROLLER_INDEX, BTP_STATUS_SUCCESS);
}
#endif /* CONFIG_BT_MESH_BLOB_CLI */

#if defined(CONFIG_BT_MESH_BLOB_SRV)
static void blob_srv_recv(uint8_t *data, uint16_t len)
{
	struct mmdl_blob_srv_recv_cmd *cmd = (void *)data;
	struct model_data *model_bound;
	int err;

#if defined(CONFIG_BT_MESH_DFU_SRV)
	struct bt_mesh_blob_srv *srv = &dfu_srv.blob;
#elif defined(CONFIG_BT_MESH_DFD_SRV)
	struct bt_mesh_blob_srv *srv = &dfd_srv.upload.blob;
#endif

	model_bound = lookup_model_bound(BT_MESH_MODEL_ID_BLOB_SRV);
	if (!model_bound) {
		LOG_ERR("Model not found");
		err = -EINVAL;
		goto fail;
	}

	uint16_t timeout_base;
	uint64_t id;

	LOG_DBG("");

	id = cmd->id;
	timeout_base = cmd->timeout;

	err = bt_mesh_blob_srv_recv(srv, id, &dummy_blob_io, BT_MESH_TTL_MAX,
				    timeout_base);

	if (err) {
		LOG_ERR("ERR %d", err);
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MMDL, MMDL_BLOB_SRV_RECV, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}

static void blob_srv_cancel(uint8_t *data, uint16_t len)
{
	struct model_data *model_bound;
	int err;

#if defined(CONFIG_BT_MESH_DFU_SRV)
	struct bt_mesh_blob_srv *srv = &dfu_srv.blob;
#elif defined(CONFIG_BT_MESH_DFD_SRV)
	struct bt_mesh_blob_srv *srv = &dfd_srv.upload.blob;
#endif

	model_bound = lookup_model_bound(BT_MESH_MODEL_ID_BLOB_SRV);
	if (!model_bound) {
		LOG_ERR("Model not found");
		err = -EINVAL;
		goto fail;
	}

	LOG_DBG("");

	err = bt_mesh_blob_srv_cancel(srv);

	if (err) {
		LOG_ERR("ERR %d", err);
	}

fail:
	tester_rsp(BTP_SERVICE_ID_MMDL, MMDL_BLOB_SRV_CANCEL, CONTROLLER_INDEX,
		   err ? BTP_STATUS_FAILED : BTP_STATUS_SUCCESS);
}
#endif

void tester_handle_mesh(uint8_t opcode, uint8_t index, uint8_t *data, uint16_t len)
{
	switch (opcode) {
	case MESH_READ_SUPPORTED_COMMANDS:
		supported_commands(data, len);
		break;
	case MESH_CONFIG_PROVISIONING:
		config_prov(data, len);
		break;
	case MESH_PROVISION_NODE:
		provision_node(data, len);
		break;
	case MESH_INIT:
		init(data, len);
		break;
	case MESH_RESET:
		reset(data, len);
		break;
	case MESH_INPUT_NUMBER:
		input_number(data, len);
		break;
	case MESH_INPUT_STRING:
		input_string(data, len);
		break;
	case MESH_IVU_TEST_MODE:
		ivu_test_mode(data, len);
		break;
	case MESH_IVU_TOGGLE_STATE:
		ivu_toggle_state(data, len);
		break;
	case MESH_LPN:
		lpn(data, len);
		break;
	case MESH_LPN_POLL:
		lpn_poll(data, len);
		break;
	case MESH_NET_SEND:
		net_send(data, len);
		break;
	case MESH_HEALTH_GENERATE_FAULTS:
		health_generate_faults(data, len);
		break;
	case MESH_HEALTH_CLEAR_FAULTS:
		health_clear_faults(data, len);
		break;
	case MESH_MODEL_SEND:
		model_send(data, len);
		break;
	case MESH_COMP_DATA_GET:
		composition_data_get(data, len);
		break;
	case MESH_CFG_BEACON_GET:
		config_beacon_get(data, len);
		break;
	case MESH_CFG_BEACON_SET:
		config_beacon_set(data, len);
		break;
	case MESH_CFG_DEFAULT_TTL_GET:
		config_default_ttl_get(data, len);
		break;
	case MESH_CFG_DEFAULT_TTL_SET:
		config_default_ttl_set(data, len);
		break;
	case MESH_CFG_GATT_PROXY_GET:
		config_gatt_proxy_get(data, len);
		break;
	case MESH_CFG_GATT_PROXY_SET:
		config_gatt_proxy_set(data, len);
		break;
	case MESH_CFG_FRIEND_GET:
		config_friend_get(data, len);
		break;
	case MESH_CFG_FRIEND_SET:
		config_friend_set(data, len);
		break;
	case MESH_CFG_RELAY_GET:
		config_relay_get(data, len);
		break;
	case MESH_CFG_RELAY_SET:
		config_relay_set(data, len);
		break;
	case MESH_CFG_MODEL_PUB_GET:
		config_mod_pub_get(data, len);
		break;
	case MESH_CFG_MODEL_PUB_SET:
		config_mod_pub_set(data, len);
		break;
	case MESH_CFG_MODEL_SUB_ADD:
		config_mod_sub_add(data, len);
		break;
	case MESH_CFG_MODEL_SUB_DEL:
		config_mod_sub_del(data, len);
		break;
	case MESH_CFG_MODEL_SUB_OVW:
		config_mod_sub_ovw(data, len);
		break;
	case MESH_CFG_MODEL_SUB_DEL_ALL:
		config_mod_sub_del_all(data, len);
		break;
	case MESH_CFG_MODEL_SUB_GET:
		config_mod_sub_get(data, len);
		break;
	case MESH_CFG_MODEL_SUB_GET_VND:
		config_mod_sub_get_vnd(data, len);
		break;
	case MESH_CFG_MODEL_SUB_VA_ADD:
		config_mod_sub_va_add(data, len);
		break;
	case MESH_CFG_MODEL_SUB_VA_DEL:
		config_mod_sub_va_del(data, len);
		break;
	case MESH_CFG_MODEL_SUB_VA_OVW:
		config_mod_sub_va_ovw(data, len);
		break;
	case MESH_CFG_NETKEY_ADD:
		config_netkey_add(data, len);
		break;
	case MESH_CFG_NETKEY_GET:
		config_netkey_get(data, len);
		break;
	case MESH_CFG_NETKEY_DEL:
		config_netkey_del(data, len);
		break;
	case MESH_CFG_NETKEY_UPDATE:
		config_netkey_update(data, len);
		break;
	case MESH_CFG_APPKEY_ADD:
		config_appkey_add(data, len);
		break;
	case MESH_CFG_APPKEY_DEL:
		config_appkey_del(data, len);
		break;
	case MESH_CFG_APPKEY_GET:
		config_appkey_get(data, len);
		break;
	case MESH_CFG_APPKEY_UPDATE:
		config_appkey_update(data, len);
		break;
	case MESH_CFG_MODEL_APP_BIND:
		config_model_app_bind(data, len);
		break;
	case MESH_CFG_MODEL_APP_UNBIND:
		config_model_app_unbind(data, len);
		break;
	case MESH_CFG_MODEL_APP_GET:
		config_model_app_get(data, len);
		break;
	case MESH_CFG_MODEL_APP_VND_GET:
		config_model_app_vnd_get(data, len);
		break;
	case MESH_CFG_HEARTBEAT_PUB_SET:
		config_hb_pub_set(data, len);
		break;
	case MESH_CFG_HEARTBEAT_PUB_GET:
		config_hb_pub_get(data, len);
		break;
	case MESH_CFG_HEARTBEAT_SUB_SET:
		config_hb_sub_set(data, len);
		break;
	case MESH_CFG_HEARTBEAT_SUB_GET:
		config_hb_sub_get(data, len);
		break;
	case MESH_CFG_NET_TRANS_GET:
		config_net_trans_get(data, len);
		break;
	case MESH_CFG_NET_TRANS_SET:
		config_net_trans_set(data, len);
		break;
	case MESH_CFG_NODE_IDT_SET:
		config_node_identity_set(data, len);
		break;
	case MESH_CFG_NODE_IDT_GET:
		config_node_identity_get(data, len);
		break;
	case MESH_CFG_NODE_RESET:
		config_node_reset(data, len);
		break;
	case MESH_CFG_LPN_TIMEOUT_GET:
		config_lpn_timeout_get(data, len);
		break;
	case MESH_CFG_MODEL_PUB_VA_SET:
		config_mod_pub_va_set(data, len);
		break;
	case MESH_CFG_MODEL_APP_BIND_VND:
		config_model_app_bind_vnd(data, len);
		break;
	case MESH_HEALTH_FAULT_GET:
		health_fault_get(data, len);
		break;
	case MESH_HEALTH_FAULT_CLEAR:
		health_fault_clear(data, len);
		break;
	case MESH_HEALTH_FAULT_TEST:
		health_fault_test(data, len);
		break;
	case MESH_HEALTH_PERIOD_GET:
		health_period_get(data, len);
		break;
	case MESH_HEALTH_PERIOD_SET:
		health_period_set(data, len);
		break;
	case MESH_HEALTH_ATTENTION_GET:
		health_attention_get(data, len);
		break;
	case MESH_HEALTH_ATTENTION_SET:
		health_attention_set(data, len);
		break;
	case MESH_PROVISION_ADV:
		provision_adv(data, len);
		break;
	case MESH_CFG_KRP_GET:
		config_krp_get(data, len);
		break;
	case MESH_CFG_KRP_SET:
		config_krp_set(data, len);
		break;
#if defined(CONFIG_BT_TESTING)
	case MESH_LPN_SUBSCRIBE:
		lpn_subscribe(data, len);
		break;
	case MESH_LPN_UNSUBSCRIBE:
		lpn_unsubscribe(data, len);
		break;
	case MESH_RPL_CLEAR:
		rpl_clear(data, len);
		break;
#endif /* CONFIG_BT_TESTING */
	case MESH_PROXY_IDENTITY:
		proxy_identity_enable(data, len);
		break;
	case MESH_SAR_TRANSMITTER_GET:
		sar_transmitter_get(data, len);
		break;
	case MESH_SAR_TRANSMITTER_SET:
		sar_transmitter_set(data, len);
		break;
	case MESH_SAR_RECEIVER_GET:
		sar_receiver_get(data, len);
		break;
	case MESH_SAR_RECEIVER_SET:
		sar_receiver_set(data, len);
		break;
	case MESH_LARGE_COMP_DATA_GET:
		large_comp_data_get(data, len);
		break;
	case MESH_MODELS_METADATA_GET:
		models_metadata_get(data, len);
		break;
	case MESH_COMP_CHANGE_PREPARE:
		change_prepare(data, len);
		break;
	case MESH_SET_COMP_ALT:
		set_comp_alt(data, len);
		break;
#if defined(CONFIG_BT_MESH_RPR_CLI)
	case MESH_RPR_SCAN_START:
		rpr_scan_start(data, len);
		break;
	case MESH_RPR_EXT_SCAN_START:
		rpr_ext_scan_start(data, len);
		break;
	case MESH_RPR_SCAN_CAPS_GET:
		rpr_scan_caps_get(data, len);
		break;
	case MESH_RPR_SCAN_GET:
		rpr_scan_get(data, len);
		break;
	case MESH_RPR_SCAN_STOP:
		rpr_scan_stop(data, len);
		break;
	case MESH_RPR_LINK_GET:
		rpr_link_get(data, len);
		break;
	case MESH_RPR_LINK_CLOSE:
		rpr_link_close(data, len);
		break;
	case MESH_RPR_PROV_REMOTE:
		rpr_prov_remote(data, len);
		break;
	case MESH_RPR_REPROV_REMOTE:
		rpr_reprov_remote(data, len);
		break;
#endif
	default:
		tester_rsp(BTP_SERVICE_ID_MESH, opcode, index,
			   BTP_STATUS_UNKNOWN_CMD);
		break;
	}
}

void net_recv_ev(uint8_t ttl, uint8_t ctl, uint16_t src, uint16_t dst, const void *payload,
		 size_t payload_len)
{
	NET_BUF_SIMPLE_DEFINE(buf, UINT8_MAX);
	struct mesh_net_recv_ev *ev;

	LOG_DBG("ttl 0x%02x ctl 0x%02x src 0x%04x dst 0x%04x payload_len %zu",
		ttl, ctl, src, dst, payload_len);

	if (payload_len > net_buf_simple_tailroom(&buf)) {
		LOG_ERR("Payload size exceeds buffer size");
		return;
	}

	ev = net_buf_simple_add(&buf, sizeof(*ev));
	ev->ttl = ttl;
	ev->ctl = ctl;
	ev->src = sys_cpu_to_le16(src);
	ev->dst = sys_cpu_to_le16(dst);
	ev->payload_len = payload_len;
	net_buf_simple_add_mem(&buf, payload, payload_len);

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_NET_RECV, CONTROLLER_INDEX,
		    buf.data, buf.len);
}

void model_recv_ev(uint16_t src, uint16_t dst, const void *payload,
		   size_t payload_len)
{
	NET_BUF_SIMPLE_DEFINE(buf, UINT8_MAX);
	struct mesh_model_recv_ev *ev;

	LOG_DBG("src 0x%04x dst 0x%04x payload_len %zu", src, dst, payload_len);

	if (payload_len > net_buf_simple_tailroom(&buf)) {
		LOG_ERR("Payload size exceeds buffer size");
		return;
	}

	ev = net_buf_simple_add(&buf, sizeof(*ev));
	ev->src = sys_cpu_to_le16(src);
	ev->dst = sys_cpu_to_le16(dst);
	ev->payload_len = payload_len;
	net_buf_simple_add_mem(&buf, payload, payload_len);

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_MODEL_RECV, CONTROLLER_INDEX,
		    buf.data, buf.len);
}

static void model_bound_cb(uint16_t addr, struct bt_mesh_model *model,
			   uint16_t key_idx)
{
	int i;

	LOG_DBG("remote addr 0x%04x key_idx 0x%04x model %p",
		addr, key_idx, model);

	for (i = 0; i < ARRAY_SIZE(model_bound); i++) {
		if (!model_bound[i].model) {
			model_bound[i].model = model;
			model_bound[i].addr = addr;
			model_bound[i].appkey_idx = key_idx;

			return;
		}
	}

	LOG_ERR("model_bound is full");
}

static void model_unbound_cb(uint16_t addr, struct bt_mesh_model *model,
			     uint16_t key_idx)
{
	int i;

	LOG_DBG("remote addr 0x%04x key_idx 0x%04x model %p",
		addr, key_idx, model);

	for (i = 0; i < ARRAY_SIZE(model_bound); i++) {
		if (model_bound[i].model == model) {
			model_bound[i].model = NULL;
			model_bound[i].addr = 0x0000;
			model_bound[i].appkey_idx = BT_MESH_KEY_UNUSED;

			return;
		}
	}

	LOG_INF("model not found");
}

static void invalid_bearer_cb(uint8_t opcode)
{
	struct mesh_invalid_bearer_ev ev = {
		.opcode = opcode,
	};

	LOG_DBG("opcode 0x%02x", opcode);

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_INVALID_BEARER,
		    CONTROLLER_INDEX, (uint8_t *) &ev, sizeof(ev));
}

static void incomp_timer_exp_cb(void)
{
	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_INCOMP_TIMER_EXP,
		    CONTROLLER_INDEX, NULL, 0);
}

static struct bt_test_cb bt_test_cb = {
	.mesh_net_recv = net_recv_ev,
	.mesh_model_recv = model_recv_ev,
	.mesh_model_bound = model_bound_cb,
	.mesh_model_unbound = model_unbound_cb,
	.mesh_prov_invalid_bearer = invalid_bearer_cb,
	.mesh_trans_incomp_timer_exp = incomp_timer_exp_cb,
};

static void friend_established(uint16_t net_idx, uint16_t lpn_addr,
			       uint8_t recv_delay, uint32_t polltimeout)
{
	struct mesh_frnd_established_ev ev = { net_idx, lpn_addr, recv_delay,
					       polltimeout };

	LOG_DBG("Friendship (as Friend) established with "
			"LPN 0x%04x Receive Delay %u Poll Timeout %u",
			lpn_addr, recv_delay, polltimeout);


	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_FRND_ESTABLISHED,
		    CONTROLLER_INDEX, (uint8_t *) &ev, sizeof(ev));
}

static void friend_terminated(uint16_t net_idx, uint16_t lpn_addr)
{
	struct mesh_frnd_terminated_ev ev = { net_idx, lpn_addr };

	LOG_DBG("Friendship (as Friend) lost with LPN "
			"0x%04x", lpn_addr);

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_FRND_TERMINATED,
		    CONTROLLER_INDEX, (uint8_t *) &ev, sizeof(ev));
}

BT_MESH_FRIEND_CB_DEFINE(friend_cb) = {
	.established = friend_established,
	.terminated = friend_terminated,
};

static void lpn_established(uint16_t net_idx, uint16_t friend_addr,
					uint8_t queue_size, uint8_t recv_win)
{
	struct mesh_lpn_established_ev ev = { net_idx, friend_addr, queue_size,
					      recv_win };

	LOG_DBG("Friendship (as LPN) established with "
			"Friend 0x%04x Queue Size %d Receive Window %d",
			friend_addr, queue_size, recv_win);

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_LPN_ESTABLISHED,
		    CONTROLLER_INDEX, (uint8_t *) &ev, sizeof(ev));
}

static void lpn_terminated(uint16_t net_idx, uint16_t friend_addr)
{
	struct mesh_lpn_polled_ev ev = { net_idx, friend_addr };

	LOG_DBG("Friendship (as LPN) lost with Friend "
			"0x%04x", friend_addr);

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_LPN_TERMINATED,
		    CONTROLLER_INDEX, (uint8_t *) &ev, sizeof(ev));
}

static void lpn_polled(uint16_t net_idx, uint16_t friend_addr, bool retry)
{
	struct mesh_lpn_polled_ev ev = { net_idx, friend_addr, (uint8_t)retry };

	LOG_DBG("LPN polled 0x%04x %s", friend_addr, retry ? "(retry)" : "");

	tester_send(BTP_SERVICE_ID_MESH, MESH_EV_LPN_POLLED,
		    CONTROLLER_INDEX, (uint8_t *) &ev, sizeof(ev));
}

BT_MESH_LPN_CB_DEFINE(lpn_cb) = {
	.established = lpn_established,
	.terminated = lpn_terminated,
	.polled = lpn_polled,
};

uint8_t tester_init_mesh(void)
{
	int err;

	if (IS_ENABLED(CONFIG_BT_TESTING)) {
		bt_test_cb_register(&bt_test_cb);
	}

	if (default_comp) {
		err = bt_mesh_init(&prov, &comp);
	} else {
		err = bt_mesh_init(&prov, &comp_alt);
	}

	if (err) {
		return BTP_STATUS_FAILED;
	}

	return BTP_STATUS_SUCCESS;
}

uint8_t tester_unregister_mesh(void)
{
	return BTP_STATUS_SUCCESS;
}

void tester_handle_mmdl(uint8_t opcode, uint8_t index, uint8_t *data,
			uint16_t len)
{
	switch (opcode) {
#if defined(CONFIG_BT_MESH_DFD_SRV)
	case MMDL_DFU_INFO_GET:
		dfu_info_get(data, len);
		break;
	case MMDL_DFU_UPDATE_METADATA_CHECK:
		dfu_update_metadata_check(data, len);
		break;
	case MMDL_DFU_FIRMWARE_UPDATE_GET:
		dfu_firmware_update_get(data, len);
		break;
	case MMDL_DFU_FIRMWARE_UPDATE_CANCEL:
		dfu_firmware_update_cancel(data, len);
		break;
	case MMDL_DFU_FIRMWARE_UPDATE_START:
		dfu_firmware_update_start(data, len);
		break;
	case MMDL_DFU_FIRMWARE_UPDATE_APPLY:
		dfu_firmware_update_apply(data, len);
		break;
#endif
#if defined(CONFIG_BT_MESH_BLOB_CLI) && !defined(CONFIG_BT_MESH_DFD_SRV)
	case MMDL_BLOB_INFO_GET:
		blob_info_get(data, len);
		break;
	case MMDL_BLOB_TRANSFER_START:
		blob_transfer_start(data, len);
		break;
	case MMDL_BLOB_TRANSFER_CANCEL:
		blob_transfer_cancel(data, len);
		break;
#endif
#if defined(CONFIG_BT_MESH_BLOB_SRV)
	case MMDL_BLOB_SRV_RECV:
		blob_srv_recv(data, len);
		break;
	case MMDL_BLOB_SRV_CANCEL:
		blob_srv_cancel(data, len);
		break;
#endif
	default:
		tester_rsp(BTP_SERVICE_ID_MMDL, opcode, index,
			   BTP_STATUS_UNKNOWN_CMD);
		break;
	}
}

uint8_t tester_init_mmdl(void)
{
	return BTP_STATUS_SUCCESS;
}

uint8_t tester_unregister_mmdl(void)
{
	return BTP_STATUS_SUCCESS;
}
