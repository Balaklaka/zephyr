/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <bluetooth/mesh.h>
#include <bluetooth/mesh/access.h>

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_MODEL)
#define LOG_MODULE_NAME bt_mesh_sar_cfg_cli
#include "common/log.h"

#include "net.h"
#include "access.h"
#include "foundation.h"
#include "mesh.h"
#include "sar_cfg_internal.h"

static int32_t msg_timeout;

static void transmitter_status(struct bt_mesh_model *model,
			       struct bt_mesh_msg_ctx *ctx,
			       struct net_buf_simple *buf)
{
	struct bt_mesh_sar_cfg_cli *cli = model->user_data;
	struct bt_mesh_sar_tx *rsp;

	if (!bt_mesh_msg_ack_ctx_match(&cli->ack_ctx, OP_SAR_CFG_TX_STATUS,
				       ctx->addr, (void **)&rsp)) {
		return;
	}

	bt_mesh_sar_tx_decode(buf, rsp);

	BT_DBG("SAR TX {0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x}",
	       rsp->seg_int_step, rsp->unicast_retrans_count,
	       rsp->unicast_retrans_without_prog_count,
	       rsp->unicast_retrans_int_step, rsp->unicast_retrans_int_inc,
	       rsp->multicast_retrans_count, rsp->multicast_retrans_int);

	bt_mesh_msg_ack_ctx_rx(&cli->ack_ctx);
}

static void receiver_status(struct bt_mesh_model *model,
			    struct bt_mesh_msg_ctx *ctx,
			    struct net_buf_simple *buf)
{
	struct bt_mesh_sar_cfg_cli *cli = model->user_data;
	struct bt_mesh_sar_rx *rsp;

	BT_DBG("net_idx 0x%04x app_idx 0x%04x src 0x%04x len %u: %s",
	       ctx->net_idx, ctx->app_idx, ctx->addr, buf->len,
	       bt_hex(buf->data, buf->len));

	if (!bt_mesh_msg_ack_ctx_match(&cli->ack_ctx, OP_SAR_CFG_RX_STATUS,
				       ctx->addr, (void **)&rsp)) {
		return;
	}

	bt_mesh_sar_rx_decode(buf, rsp);

	BT_DBG("SAR RX {0x%02x 0x%02x 0x%02x 0x%02x 0x%02x}", rsp->seg_thresh,
	       rsp->ack_delay_inc, rsp->discard_timeout, rsp->rx_seg_int_step,
	       rsp->ack_retrans_count);

	bt_mesh_msg_ack_ctx_rx(&cli->ack_ctx);
}

const struct bt_mesh_model_op _bt_mesh_sar_cfg_cli_op[] = {
	{ OP_SAR_CFG_TX_STATUS, BT_MESH_SAR_TX_LEN, transmitter_status },
	{ OP_SAR_CFG_RX_STATUS, BT_MESH_SAR_RX_LEN, receiver_status },
	BT_MESH_MODEL_OP_END,
};

static int cli_prepare(struct bt_mesh_sar_cfg_cli *cli, void *param,
		       uint32_t op, uint16_t addr)
{
	return bt_mesh_msg_ack_ctx_prepare(&cli->ack_ctx, op, addr, param);
}

int32_t bt_mesh_sar_cfg_cli_timeout_get(void)
{
	return msg_timeout;
}

void bt_mesh_sar_cfg_cli_timeout_set(int32_t timeout)
{
	msg_timeout = timeout;
}

static int bt_mesh_sar_cfg_cli_init(struct bt_mesh_model *model)
{
	struct bt_mesh_sar_cfg_cli *cli;

	if (!bt_mesh_model_in_primary(model)) {
		BT_ERR("SAR Configuration Client only allowed in primary element");
		return -EINVAL;
	}

	if (!model->user_data) {
		BT_ERR("No SAR Configuration Client context provided");
		return -EINVAL;
	}

	cli = model->user_data;
	cli->model = model;
	msg_timeout = 2 * MSEC_PER_SEC;

	model->keys[0] = BT_MESH_KEY_DEV_ANY;
	model->flags |= BT_MESH_MOD_DEVKEY_ONLY;

	bt_mesh_msg_ack_ctx_init(&cli->ack_ctx);

	return 0;
}

static void bt_mesh_sar_cfg_cli_reset(struct bt_mesh_model *model)
{
	struct bt_mesh_sar_cfg_cli *cli;

	cli = model->user_data;

	bt_mesh_msg_ack_ctx_clear(&cli->ack_ctx);
}

const struct bt_mesh_model_cb _bt_mesh_sar_cfg_cli_cb = {
	.init = bt_mesh_sar_cfg_cli_init,
	.reset = bt_mesh_sar_cfg_cli_reset,
};

int bt_mesh_sar_cfg_cli_transmitter_get(struct bt_mesh_sar_cfg_cli *cli,
					uint16_t net_idx, uint16_t addr,
					struct bt_mesh_sar_tx *rsp)
{
	BT_MESH_MODEL_BUF_DEFINE(msg, OP_SAR_CFG_TX_GET, 0);
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net_idx,
		.app_idx = BT_MESH_KEY_DEV,
		.addr = addr,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = cli_prepare(cli, rsp, OP_SAR_CFG_TX_STATUS, addr);
	if (err) {
		return err;
	}

	bt_mesh_model_msg_init(&msg, OP_SAR_CFG_TX_GET);

	err = bt_mesh_model_send(cli->model, &ctx, &msg, NULL, NULL);
	if (err) {
		BT_ERR("model_send() failed (err %d)", err);
		bt_mesh_msg_ack_ctx_clear(&cli->ack_ctx);
		return err;
	}

	return bt_mesh_msg_ack_ctx_wait(&cli->ack_ctx, K_MSEC(msg_timeout));
}

int bt_mesh_sar_cfg_cli_transmitter_set(struct bt_mesh_sar_cfg_cli *cli,
					uint16_t net_idx, uint16_t addr,
					const struct bt_mesh_sar_tx *set,
					struct bt_mesh_sar_tx *rsp)
{
	BT_MESH_MODEL_BUF_DEFINE(msg, OP_SAR_CFG_TX_SET, BT_MESH_SAR_TX_LEN);
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net_idx,
		.app_idx = BT_MESH_KEY_DEV,
		.addr = addr,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = cli_prepare(cli, rsp, OP_SAR_CFG_TX_STATUS, addr);
	if (err) {
		return err;
	}

	bt_mesh_model_msg_init(&msg, OP_SAR_CFG_TX_SET);
	bt_mesh_sar_tx_encode(&msg, set);

	err = bt_mesh_model_send(cli->model, &ctx, &msg, NULL, NULL);
	if (err) {
		BT_ERR("model_send() failed (err %d)", err);
		bt_mesh_msg_ack_ctx_clear(&cli->ack_ctx);
		return err;
	}

	return bt_mesh_msg_ack_ctx_wait(&cli->ack_ctx, K_MSEC(msg_timeout));
}

int bt_mesh_sar_cfg_cli_receiver_get(struct bt_mesh_sar_cfg_cli *cli,
				     uint16_t net_idx, uint16_t addr,
				     struct bt_mesh_sar_rx *rsp)
{
	BT_MESH_MODEL_BUF_DEFINE(msg, OP_SAR_CFG_RX_GET, 0);
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net_idx,
		.app_idx = BT_MESH_KEY_DEV,
		.addr = addr,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = cli_prepare(cli, rsp, OP_SAR_CFG_RX_STATUS, addr);
	if (err) {
		return err;
	}

	bt_mesh_model_msg_init(&msg, OP_SAR_CFG_RX_GET);

	err = bt_mesh_model_send(cli->model, &ctx, &msg, NULL, NULL);
	if (err) {
		BT_ERR("model_send() failed (err %d)", err);
		bt_mesh_msg_ack_ctx_clear(&cli->ack_ctx);
		return err;
	}

	return bt_mesh_msg_ack_ctx_wait(&cli->ack_ctx, K_MSEC(msg_timeout));
}

int bt_mesh_sar_cfg_cli_receiver_set(struct bt_mesh_sar_cfg_cli *cli,
				     uint16_t net_idx, uint16_t addr,
				     const struct bt_mesh_sar_rx *set,
				     struct bt_mesh_sar_rx *rsp)
{
	BT_MESH_MODEL_BUF_DEFINE(msg, OP_SAR_CFG_RX_SET, BT_MESH_SAR_RX_LEN);
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = net_idx,
		.app_idx = BT_MESH_KEY_DEV,
		.addr = addr,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};
	int err;

	err = cli_prepare(cli, rsp, OP_SAR_CFG_RX_STATUS, addr);
	if (err) {
		return err;
	}

	bt_mesh_model_msg_init(&msg, OP_SAR_CFG_RX_SET);
	bt_mesh_sar_rx_encode(&msg, set);

	err = bt_mesh_model_send(cli->model, &ctx, &msg, NULL, NULL);
	if (err) {
		BT_ERR("model_send() failed (err %d)", err);
		bt_mesh_msg_ack_ctx_clear(&cli->ack_ctx);
		return err;
	}

	return bt_mesh_msg_ack_ctx_wait(&cli->ack_ctx, K_MSEC(msg_timeout));
}
