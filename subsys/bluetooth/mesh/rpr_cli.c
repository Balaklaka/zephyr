/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/bluetooth/conn.h>
#include "host/ecc.h"
#include "prov.h"
#include "rpr.h"

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_MODEL)
#define LOG_MODULE_NAME bt_mesh_rpr_cli
#include "common/log.h"

#define LINK_TIMEOUT_SECONDS_DEFAULT 10

BUILD_ASSERT(BT_MESH_MODEL_OP_LEN(RPR_OP_PDU_SEND) == 2, "Assumes PDU send is a 2 byte opcode");

#define LINK_CTX(_srv, _send_rel)                                              \
	{                                                                      \
		.net_idx = (_srv)->net_idx, .app_idx = BT_MESH_KEY_DEV_REMOTE, \
		.addr = (_srv)->addr, .send_ttl = (_srv)->ttl,                 \
		.send_rel = (_send_rel),                                       \
	}

enum {
	RPR_CLI_LINK_OPEN,
	RPR_CLI_NUM_FLAGS,
};

static struct {
	ATOMIC_DEFINE(flags, RPR_CLI_NUM_FLAGS);
	const struct prov_bearer_cb *cb;
	struct bt_mesh_rpr_cli *cli;
	struct {
		prov_bearer_send_complete_t cb;
	} tx;
} bearer;

static int32_t tx_timeout = (2 * MSEC_PER_SEC);

static void tx_reset(struct bt_mesh_rpr_cli *cli);
static bool rsp_match(const struct bt_mesh_rpr_cli *cli,
		      const struct bt_mesh_rpr_node *srv, uint32_t op);
static void link_reset(struct bt_mesh_rpr_cli *cli);
static void link_closed(struct bt_mesh_rpr_cli *cli,
			enum bt_mesh_rpr_status status);

static void link_report(struct bt_mesh_rpr_cli *cli,
			struct bt_mesh_rpr_node *srv,
			struct bt_mesh_rpr_link *link)
{
	struct pb_remote_ctx ctx = { cli, srv };

	if (link->state == BT_MESH_RPR_LINK_ACTIVE &&
	    !atomic_test_and_set_bit(bearer.flags, RPR_CLI_LINK_OPEN)) {
		BT_DBG("Opened");
		bearer.cb->link_opened(&pb_remote_cli, &ctx);
		return;
	}

	if (link->state != BT_MESH_RPR_LINK_IDLE ||
	    !atomic_test_and_clear_bit(bearer.flags, RPR_CLI_LINK_OPEN)) {
		return;
	}

	BT_DBG("Closed (%u)", link->status);
	bearer.cb->link_closed(&pb_remote_cli, &ctx,
			       ((link->status == BT_MESH_RPR_SUCCESS) ?
					PROV_BEARER_LINK_STATUS_SUCCESS :
					PROV_BEARER_LINK_STATUS_FAIL));
}

static void tx_complete(struct bt_mesh_rpr_cli *cli, int err, void *cb_data)
{
	BT_DBG("%d", err);

	cli->link.tx_pdu++;
	tx_reset(cli);

	if (bearer.tx.cb) {
		bearer.tx.cb(err, cb_data);
	}
}

static int handle_extended_scan_report(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
				       struct net_buf_simple *buf)
{
	struct bt_mesh_rpr_node srv = RPR_NODE(ctx);
	struct bt_mesh_rpr_cli *cli = mod->user_data;
	struct bt_mesh_rpr_unprov dev = { 0 };
	enum bt_mesh_rpr_status status;
	bool found_dev = false;

	status = net_buf_simple_pull_u8(buf);
	if (status != BT_MESH_RPR_SUCCESS) {
		BT_WARN("scan report fail (%u)", status);
		return 0;
	}

	memcpy(dev.uuid, net_buf_simple_pull_mem(buf, 16), 16);
	if (buf->len >= 2) {
		dev.oob = net_buf_simple_pull_le16(buf);
		found_dev = true;
		BT_DBG("0x%04x: %s oob: 0x%04x adv data: %s", srv.addr,
		       bt_hex(dev.uuid, 16), dev.oob,
		       bt_hex(buf->data, buf->len));
	} else {
		BT_DBG("0x%04x: %s not found.", srv.addr, bt_hex(dev.uuid, 16));
	}

	if (cli->scan_report && found_dev) {
		cli->scan_report(cli, &srv, &dev, buf);
	}

	return 0;
}

static int handle_link_report(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			      struct net_buf_simple *buf)
{
	struct bt_mesh_rpr_node srv = RPR_NODE(ctx);
	struct bt_mesh_rpr_cli *cli = mod->user_data;
	struct bt_mesh_rpr_link link;
	uint8_t reason = PROV_ERR_NONE;

	link.status = net_buf_simple_pull_u8(buf);
	link.state = net_buf_simple_pull_u8(buf);
	if (buf->len == 1) {
		reason = net_buf_simple_pull_u8(buf);
	} else if (buf->len) {
		BT_WARN("Invalid link report len");
		return -EINVAL;
	}

	/* The server uses the link report to notify about failed tx */
	if (rsp_match(cli, &srv, RPR_OP_LINK_REPORT) &&
	    link.status != BT_MESH_RPR_SUCCESS) {
		tx_complete(cli, -ECANCELED, cli->tx.ctx);
	}

	k_work_reschedule(&cli->link.timeout, K_SECONDS(cli->link.time));

	if (cli->link.srv.addr != srv.addr) {
		BT_DBG("Link report from unknown server 0x%04x", srv.addr);
		return 0;
	}

	cli->link.state = link.state;

	BT_DBG("0x%04x: status: %u state: %u", srv.addr, link.status,
	       link.state);

	if (link.state == BT_MESH_RPR_LINK_IDLE) {
		link_reset(cli);
	}

	link_report(cli, &srv, &link);

	return 0;
}

static int handle_link_status(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			      struct net_buf_simple *buf)
{
	struct bt_mesh_rpr_cli *cli = mod->user_data;
	struct bt_mesh_rpr_node srv = RPR_NODE(ctx);
	struct bt_mesh_rpr_link *rsp = cli->tx.ctx;
	struct bt_mesh_rpr_link link;

	link.status = net_buf_simple_pull_u8(buf);
	link.state = net_buf_simple_pull_u8(buf);

	BT_DBG("0x%04x: status: %u state: %u", srv.addr, link.status,
	       link.state);

	if (rsp_match(cli, &srv, RPR_OP_LINK_STATUS)) {
		*rsp = link;
		k_sem_give(&cli->tx.sync);
	}

	if (cli->link.srv.addr == srv.addr) {
		k_work_reschedule(&cli->link.timeout, K_SECONDS(cli->link.time));

		cli->link.state = link.state;
		if (link.state == BT_MESH_RPR_LINK_IDLE) {
			cli->link.srv.addr = BT_MESH_ADDR_UNASSIGNED;
		}

		link_report(cli, &cli->link.srv, &link);
	}

	return 0;
}

static int handle_pdu_outbound_report(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
				      struct net_buf_simple *buf)
{
	struct bt_mesh_rpr_cli *cli = mod->user_data;
	struct bt_mesh_rpr_node srv = RPR_NODE(ctx);
	void *cb_data = cli->tx.ctx;
	uint8_t num;

	k_work_reschedule(&cli->link.timeout, K_SECONDS(cli->link.time));

	if (srv.addr != cli->link.srv.addr) {
		BT_WARN("Outbound report from unknown server 0x%04x", srv.addr);
		return 0;
	}

	num = net_buf_simple_pull_u8(buf);

	BT_DBG("0x%04x: %u", srv.addr, num);

	if (!rsp_match(cli, &srv, RPR_OP_PDU_OUTBOUND_REPORT) ||
	    num != cli->link.tx_pdu) {
		BT_WARN("Non-matching PDU report (%u)", num);
		return 0;
	}

	tx_complete(cli, 0, cb_data);

	return 0;
}

static int handle_pdu_report(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			     struct net_buf_simple *buf)
{
	struct bt_mesh_rpr_cli *cli = mod->user_data;
	struct bt_mesh_rpr_node srv = RPR_NODE(ctx);
	struct pb_remote_ctx cb_ctx = {
		cli,
		&cli->link.srv,
	};
	uint8_t pdu;

	if (cli->link.srv.addr != srv.addr) {
		BT_WARN("PDU report from unknown server 0x%04x", srv.addr);
		return 0;
	}

	k_work_reschedule(&cli->link.timeout, K_SECONDS(cli->link.time));

	pdu = net_buf_simple_pull_u8(buf);
	if (pdu <= cli->link.rx_pdu) {
		BT_WARN("Duplicate rx %u", pdu);
		return 0;
	}

	BT_DBG("0x%04x: %u (%u bytes)", srv.addr, pdu, buf->len);

	bearer.cb->recv(&pb_remote_cli, &cb_ctx, buf);

	return 0;
}

static int handle_scan_caps_status(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
				   struct net_buf_simple *buf)
{
	struct bt_mesh_rpr_cli *cli = mod->user_data;
	struct bt_mesh_rpr_node srv = RPR_NODE(ctx);
	struct bt_mesh_rpr_caps *caps = cli->tx.ctx;

	if (!rsp_match(cli, &srv, RPR_OP_SCAN_CAPS_STATUS)) {
		BT_WARN("Unexpected scan caps rsp from 0x%04x", srv.addr);
		return 0;
	}

	caps->max_devs = net_buf_simple_pull_u8(buf);
	caps->active_scan = net_buf_simple_pull_u8(buf);

	BT_DBG("max devs: %u active scan: %u", caps->max_devs,
	       caps->active_scan);

	k_sem_give(&cli->tx.sync);

	return 0;
}

static int handle_scan_report(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			      struct net_buf_simple *buf)
{
	struct bt_mesh_rpr_cli *cli = mod->user_data;
	struct bt_mesh_rpr_node srv = RPR_NODE(ctx);
	struct bt_mesh_rpr_unprov dev = { 0 };

	dev.rssi = net_buf_simple_pull_u8(buf);
	memcpy(dev.uuid, net_buf_simple_pull_mem(buf, 16), 16);
	dev.oob = net_buf_simple_pull_be16(buf);
	if (buf->len == 4) {
		dev.hash = net_buf_simple_pull_be32(buf);
		dev.flags = BT_MESH_RPR_UNPROV_HASH;
	} else if (buf->len) {
		return -EINVAL;
	}

	if (IS_ENABLED(CONFIG_BT_MESH_DEBUG)) {
		struct bt_uuid_128 uuid_repr = { .uuid = { BT_UUID_TYPE_128 } };

		memcpy(uuid_repr.val, dev.uuid, 16);
		BT_DBG("0x%04x: %s oob: 0x%04x %ddBm", srv.addr,
		       bt_uuid_str(&uuid_repr.uuid), dev.oob, dev.rssi);
	}

	if (cli->scan_report) {
		cli->scan_report(cli, &srv, &dev, NULL);
	}

	return 0;
}

static int handle_scan_status(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			      struct net_buf_simple *buf)
{
	struct bt_mesh_rpr_cli *cli = mod->user_data;
	struct bt_mesh_rpr_scan_status *status = cli->tx.ctx;
	struct bt_mesh_rpr_node srv = RPR_NODE(ctx);

	if (!rsp_match(cli, &srv, RPR_OP_SCAN_STATUS)) {
		BT_WARN("Unexpected scan status from 0x%04x", srv.addr);
		return 0;
	}

	status->status = net_buf_simple_pull_u8(buf);
	status->scan = net_buf_simple_pull_u8(buf);
	status->max_devs = net_buf_simple_pull_u8(buf);
	status->timeout = net_buf_simple_pull_u8(buf);

	BT_DBG("status: %u state: %u max devs: %u timeout: %u seconds",
	       status->status, status->scan, status->max_devs, status->timeout);
	k_sem_give(&cli->tx.sync);

	return 0;
}

const struct bt_mesh_model_op _bt_mesh_rpr_cli_op[] = {
	{ RPR_OP_EXTENDED_SCAN_REPORT, BT_MESH_LEN_MIN(17), handle_extended_scan_report },
	{ RPR_OP_LINK_REPORT, BT_MESH_LEN_MIN(2), handle_link_report },
	{ RPR_OP_LINK_STATUS, BT_MESH_LEN_EXACT(2), handle_link_status },
	{ RPR_OP_PDU_OUTBOUND_REPORT, BT_MESH_LEN_EXACT(1), handle_pdu_outbound_report },
	{ RPR_OP_PDU_REPORT, BT_MESH_LEN_MIN(2), handle_pdu_report },
	{ RPR_OP_SCAN_CAPS_STATUS, BT_MESH_LEN_EXACT(2), handle_scan_caps_status },
	{ RPR_OP_SCAN_REPORT, BT_MESH_LEN_MIN(19), handle_scan_report },
	{ RPR_OP_SCAN_STATUS, BT_MESH_LEN_EXACT(4), handle_scan_status },
	BT_MESH_MODEL_OP_END,
};

static void link_timeout(struct k_work *work)
{
	struct bt_mesh_rpr_cli *cli = CONTAINER_OF(k_work_delayable_from_work(work),
						   struct bt_mesh_rpr_cli, link.timeout);

	if (cli->link.state != BT_MESH_RPR_LINK_IDLE) {
		BT_DBG("");
		link_closed(cli, BT_MESH_RPR_ERR_LINK_CLOSED_BY_CLIENT);
	}
}

static int rpr_cli_init(struct bt_mesh_model *mod)
{
	struct bt_mesh_rpr_cli *cli = mod->user_data;

	cli->mod = mod;
	cli->link.time = LINK_TIMEOUT_SECONDS_DEFAULT;

	k_sem_init(&cli->tx.sync, 0, 1);
	k_work_init_delayable(&cli->link.timeout, link_timeout);
	mod->keys[0] = BT_MESH_KEY_DEV_REMOTE;

	return 0;
}

const struct bt_mesh_model_cb _bt_mesh_rpr_cli_cb = {
	.init = rpr_cli_init,
};

static void pdu_send_start(uint16_t duration, int err, void *cb_data)
{
	struct bt_mesh_rpr_cli *cli = cb_data;

	if (err) {
		BT_ERR("PDU Send failed: %d", err);

		link_closed(cli,
			    BT_MESH_RPR_ERR_LINK_CLOSED_AS_CANNOT_SEND_PDU);
	}
}

static void pdu_send_end(int err, void *cb_data)
{
	struct bt_mesh_rpr_cli *cli = cb_data;

	if (err) {
		BT_ERR("PDU Send failed: %d", err);

		link_closed(cli,
			    BT_MESH_RPR_ERR_LINK_CLOSED_AS_CANNOT_SEND_PDU);
	}
}

static const struct bt_mesh_send_cb pdu_send_cb = {
	.start = pdu_send_start,
	.end = pdu_send_end,
};

static void tx_reset(struct bt_mesh_rpr_cli *cli)
{
	cli->tx.rsp = 0;
	cli->tx.ctx = NULL;
	cli->tx.srv = NULL;
}

static int tx_prepare(struct bt_mesh_rpr_cli *cli,
		      const struct bt_mesh_rpr_node *srv, uint32_t op,
		      void *ctx)
{
	if (cli->tx.rsp) {
		BT_WARN("TX Already in progress");
		return -EBUSY;
	}

	cli->tx.rsp = op;
	cli->tx.ctx = ctx;
	cli->tx.srv = srv;

	return 0;
}

static bool rsp_match(const struct bt_mesh_rpr_cli *cli,
		      const struct bt_mesh_rpr_node *srv, uint32_t op)
{
	return cli->tx.rsp == op && cli->tx.srv->addr == srv->addr;
}

static int tx_wait(struct bt_mesh_rpr_cli *cli,
		   const struct bt_mesh_rpr_node *srv,
		   struct net_buf_simple *buf, uint32_t rsp, void *rsp_ctx)
{
	struct bt_mesh_msg_ctx ctx = LINK_CTX(srv, false);
	int err;

	err = tx_prepare(cli, srv, rsp, rsp_ctx);
	if (err) {
		return err;
	}

	err = bt_mesh_model_send(cli->mod, &ctx, buf, NULL, NULL);
	if (err) {
		tx_reset(cli);
		BT_WARN("TX fail");
		return err;
	}

	err = k_sem_take(&cli->tx.sync, K_MSEC(tx_timeout));
	if (err) {
		err = -ETIMEDOUT;
	}

	tx_reset(cli);

	return err;
}

static void link_init(struct bt_mesh_rpr_cli *cli,
		      const struct bt_mesh_rpr_node *srv)
{
	cli->link.srv = *srv;
	cli->link.state = BT_MESH_RPR_LINK_IDLE;
	cli->link.rx_pdu = 0;
	cli->link.tx_pdu = 1;
	k_work_reschedule(&cli->link.timeout, K_SECONDS(cli->link.time));
}

static void link_reset(struct bt_mesh_rpr_cli *cli)
{
	k_work_cancel_delayable(&cli->link.timeout);
	cli->link.srv.addr = BT_MESH_ADDR_UNASSIGNED;
	cli->link.state = BT_MESH_RPR_LINK_IDLE;
	tx_reset(cli);
}

static void link_closed(struct bt_mesh_rpr_cli *cli,
			enum bt_mesh_rpr_status status)
{
	struct bt_mesh_rpr_node srv = cli->link.srv;
	struct bt_mesh_rpr_link link = {
		.status = status,
		.state = BT_MESH_RPR_LINK_IDLE,
	};

	BT_DBG("0x%04x: status: %u state: %u", srv.addr, link.status,
	       link.state);

	link_reset(cli);

	link_report(cli, &srv, &link);
}

int bt_mesh_rpr_scan_caps_get(struct bt_mesh_rpr_cli *cli,
			      const struct bt_mesh_rpr_node *srv,
			      struct bt_mesh_rpr_caps *caps)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, RPR_OP_SCAN_CAPS_GET, 0);
	bt_mesh_model_msg_init(&buf, RPR_OP_SCAN_CAPS_GET);

	return tx_wait(cli, srv, &buf, RPR_OP_SCAN_CAPS_STATUS, caps);
}

int bt_mesh_rpr_scan_get(struct bt_mesh_rpr_cli *cli,
			 const struct bt_mesh_rpr_node *srv,
			 struct bt_mesh_rpr_scan_status *status)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, RPR_OP_SCAN_GET, 0);
	bt_mesh_model_msg_init(&buf, RPR_OP_SCAN_GET);

	return tx_wait(cli, srv, &buf, RPR_OP_SCAN_STATUS, status);
}

int bt_mesh_rpr_scan_start(struct bt_mesh_rpr_cli *cli,
			   const struct bt_mesh_rpr_node *srv,
			   const uint8_t uuid[16], uint8_t timeout,
			   uint8_t max_devs,
			   struct bt_mesh_rpr_scan_status *status)
{
	if (!timeout) {
		return -EINVAL;
	}

	BT_MESH_MODEL_BUF_DEFINE(buf, RPR_OP_SCAN_START, 18);
	bt_mesh_model_msg_init(&buf, RPR_OP_SCAN_START);

	net_buf_simple_add_u8(&buf, max_devs);
	net_buf_simple_add_u8(&buf, timeout);

	if (uuid) {
		net_buf_simple_add_mem(&buf, uuid, 16);
	}

	return tx_wait(cli, srv, &buf, RPR_OP_SCAN_STATUS, status);
}

int bt_mesh_rpr_scan_start_ext(struct bt_mesh_rpr_cli *cli,
			       const struct bt_mesh_rpr_node *srv,
			       const uint8_t uuid[16], uint8_t timeout,
			       const uint8_t *ad_types, size_t ad_count)
{
	struct bt_mesh_msg_ctx ctx = LINK_CTX(srv, false);

	if ((uuid && (timeout < BT_MESH_RPR_EXT_SCAN_TIME_MIN ||
		      timeout > BT_MESH_RPR_EXT_SCAN_TIME_MAX)) ||
	    !ad_count || ad_count > CONFIG_BT_MESH_RPR_AD_TYPES_MAX) {
		return -EINVAL;
	}

	BT_MESH_MODEL_BUF_DEFINE(buf, RPR_OP_EXTENDED_SCAN_START,
				 18 + CONFIG_BT_MESH_RPR_AD_TYPES_MAX);
	bt_mesh_model_msg_init(&buf, RPR_OP_EXTENDED_SCAN_START);

	net_buf_simple_add_u8(&buf, ad_count);
	net_buf_simple_add_mem(&buf, ad_types, ad_count);
	if (uuid) {
		net_buf_simple_add_mem(&buf, uuid, 16);
		net_buf_simple_add_u8(&buf, timeout);
	}

	return bt_mesh_model_send(cli->mod, &ctx, &buf, NULL, NULL);
}

int bt_mesh_rpr_scan_stop(struct bt_mesh_rpr_cli *cli,
			  const struct bt_mesh_rpr_node *srv,
			  struct bt_mesh_rpr_scan_status *status)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, RPR_OP_SCAN_STOP, 0);
	bt_mesh_model_msg_init(&buf, RPR_OP_SCAN_STOP);

	return tx_wait(cli, srv, &buf, RPR_OP_SCAN_STATUS, status);
}

int bt_mesh_rpr_link_get(struct bt_mesh_rpr_cli *cli,
			 const struct bt_mesh_rpr_node *srv,
			 struct bt_mesh_rpr_link *rsp)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, RPR_OP_LINK_GET, 0);
	bt_mesh_model_msg_init(&buf, RPR_OP_LINK_GET);

	return tx_wait(cli, srv, &buf, RPR_OP_LINK_STATUS, rsp);
}

int bt_mesh_rpr_link_close(struct bt_mesh_rpr_cli *cli,
			   const struct bt_mesh_rpr_node *srv,
			   struct bt_mesh_rpr_link *rsp)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, RPR_OP_LINK_CLOSE, 1);
	bt_mesh_model_msg_init(&buf, RPR_OP_LINK_CLOSE);
	net_buf_simple_add_u8(&buf, PROV_BEARER_LINK_STATUS_FAIL);

	return tx_wait(cli, srv, &buf, RPR_OP_LINK_STATUS, rsp);
}

static int link_open_prov(struct bt_mesh_rpr_cli *cli,
			  struct bt_mesh_rpr_node *srv, const uint8_t uuid[16],
			  uint8_t timeout)
{
	struct bt_mesh_msg_ctx ctx = LINK_CTX(srv, false);
	int err;

	if (cli->link.srv.addr != BT_MESH_ADDR_UNASSIGNED) {
		return -EBUSY;
	}

	BT_MESH_MODEL_BUF_DEFINE(buf, RPR_OP_LINK_OPEN, 17);
	bt_mesh_model_msg_init(&buf, RPR_OP_LINK_OPEN);

	net_buf_simple_add_mem(&buf, uuid, 16);

	if (timeout) {
		cli->link.time = timeout;
	}

	if (cli->link.time != LINK_TIMEOUT_SECONDS_DEFAULT) {
		net_buf_simple_add_u8(&buf, cli->link.time);
	}

	link_init(cli, srv);

	BT_DBG("");

	err = bt_mesh_model_send(cli->mod, &ctx, &buf, NULL, NULL);
	if (err) {
		link_reset(cli);
	}

	return err;
}

static int link_open_node(struct bt_mesh_rpr_cli *cli,
			  struct bt_mesh_rpr_node *srv,
			  enum bt_mesh_rpr_node_refresh type)
{
	struct bt_mesh_msg_ctx ctx = LINK_CTX(srv, false);
	int err;

	if (cli->link.srv.addr != BT_MESH_ADDR_UNASSIGNED) {
		return -EBUSY;
	}

	BT_MESH_MODEL_BUF_DEFINE(buf, RPR_OP_LINK_OPEN, 1);
	bt_mesh_model_msg_init(&buf, RPR_OP_LINK_OPEN);

	net_buf_simple_add_u8(&buf, type);

	cli->link.time = LINK_TIMEOUT_SECONDS_DEFAULT;

	link_init(cli, srv);

	BT_DBG("");

	err = bt_mesh_model_send(cli->mod, &ctx, &buf, NULL, NULL);
	if (err) {
		link_reset(cli);
	}

	return err;
}

static int link_close(struct bt_mesh_rpr_cli *cli,
		      enum prov_bearer_link_status status)
{
	struct bt_mesh_msg_ctx ctx = LINK_CTX(&cli->link.srv, false);
	int err;

	if (cli->link.srv.addr == BT_MESH_ADDR_UNASSIGNED) {
		return -EALREADY;
	}

	BT_MESH_MODEL_BUF_DEFINE(buf, RPR_OP_LINK_CLOSE, 1);
	bt_mesh_model_msg_init(&buf, RPR_OP_LINK_CLOSE);

	net_buf_simple_add_u8(&buf, status);

	err = bt_mesh_model_send(cli->mod, &ctx, &buf, NULL, NULL);
	if (err) {
		link_reset(cli);
	}

	return err;
}

static int send(struct bt_mesh_rpr_cli *cli, struct net_buf_simple *buf,
		void *cb_data)
{
	struct bt_mesh_msg_ctx ctx = LINK_CTX(&cli->link.srv, true);
	int err;

	if (cli->link.srv.addr == BT_MESH_ADDR_UNASSIGNED) {
		BT_ERR("No server");
		return -ESHUTDOWN;
	}

	if (net_buf_simple_headroom(buf) < 3) {
		BT_ERR("Invalid buffer");
		return -EINVAL;
	}

	err = tx_prepare(cli, &cli->link.srv, RPR_OP_PDU_OUTBOUND_REPORT,
			 cb_data);
	if (err) {
		BT_ERR("Busy");
		return err;
	}

	BT_DBG("0x%02x", buf->data[0]);

	net_buf_simple_push_u8(buf, cli->link.tx_pdu);
	/* Assumes opcode is 2 bytes. Build assert at top of file ensures this.
	 */
	net_buf_simple_push_be16(buf, RPR_OP_PDU_SEND);

	err = bt_mesh_model_send(cli->mod, &ctx, buf, &pdu_send_cb, cli);
	if (err) {
		link_closed(cli,
			    BT_MESH_RPR_ERR_LINK_CLOSED_AS_CANNOT_SEND_PDU);
	}

	return err;
}

int32_t bt_mesh_rpr_cli_timeout_get(void)
{
	return tx_timeout;
}

void bt_mesh_rpr_cli_timeout_set(int32_t timeout)
{
	tx_timeout = timeout;
}

/*******************************************************************************
 * Prov bearer interface
 ******************************************************************************/

static int pb_send(struct net_buf_simple *buf, prov_bearer_send_complete_t cb,
		   void *cb_data)
{
	bearer.tx.cb = cb;
	return send(bearer.cli, buf, cb_data);
}

static void pb_clear_tx(void)
{
	/* Nothing can be done */
}

static int pb_link_open(const uint8_t uuid[16], uint8_t timeout,
			const struct prov_bearer_cb *cb, void *cb_data)
{
	struct pb_remote_ctx *ctx = cb_data;
	int err;

	bearer.cli = ctx->cli;
	bearer.cb = cb;

	if (uuid) {
		err = link_open_prov(ctx->cli, ctx->srv, uuid, timeout);
	} else {
		err = link_open_node(ctx->cli, ctx->srv, ctx->refresh);
	}

	if (err) {
		return err;
	}

	atomic_clear(bearer.flags);

	return err;
}

static void pb_link_close(enum prov_bearer_link_status status)
{
	int err;

	err = link_close(bearer.cli, status);
	if (err) {
		BT_ERR("Link close failed (%d)", err);
	}
}

const struct prov_bearer pb_remote_cli = {
	.type = BT_MESH_PROV_REMOTE,
	.send = pb_send,
	.clear_tx = pb_clear_tx,
	.link_open = pb_link_open,
	.link_close = pb_link_close,
};
