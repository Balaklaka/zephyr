/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <bluetooth/mesh.h>
#include "mesh.h"
#include "blob.h"
#include "net.h"
#include "transport.h"

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_MODEL)
#define LOG_MODULE_NAME bt_mesh_blob_cli
#include "common/log.h"

#define TARGETS_FOR_EACH(cli, target)                                          \
	SYS_SLIST_FOR_EACH_CONTAINER((sys_slist_t *)&(cli)->ctx->targets,      \
				     target, n)

#define CHUNK_SIZE_MAX BLOB_CHUNK_SIZE_MAX(BT_MESH_TX_SDU_MAX)

#define RETRY_TIME_PULL K_SECONDS(BLOB_POLL_TIME_MAX_SECS * 2 + 7)

#define UNICAST_MODE(cli)                                                      \
	((cli)->ctx->group == BT_MESH_ADDR_UNASSIGNED ||                       \
	 (cli)->tx.ctx->force_unicast)

BUILD_ASSERT(BLOB_BLOCK_SIZE_LOG_MIN <= BLOB_BLOCK_SIZE_LOG_MAX,
	     "The must be at least one number between the min and "
	     "max block size that is the power of two.");

struct xfer_info {
	enum bt_mesh_blob_status status;
	enum bt_mesh_blob_xfer_mode mode;
	enum bt_mesh_blob_xfer_phase phase;
	uint64_t id;
	uint32_t size;
	uint8_t block_size_log;
	uint16_t mtu_size;
	const uint8_t *missing_blocks;
};

struct block_status {
	enum bt_mesh_blob_status status;
	enum bt_mesh_blob_chunks_missing missing;
	struct bt_mesh_blob_block block;
};

const struct bt_mesh_blob_cli_bounds bt_mesh_blob_cli_boundaries = {
	.max_block_size_log = BLOB_BLOCK_SIZE_LOG_MAX,
	.min_block_size_log = BLOB_BLOCK_SIZE_LOG_MIN,
	.max_chunks = CONFIG_BT_MESH_BLOB_CHUNK_COUNT_MAX,
	.chunk_size = CHUNK_SIZE_MAX,
	.max_size = CONFIG_BT_MESH_BLOB_SIZE_MAX,
	.mtu_size = BT_MESH_TX_SDU_MAX,
	.modes = BT_MESH_BLOB_XFER_MODE_ALL,
};

static struct bt_mesh_blob_target *next_target(struct bt_mesh_blob_cli *cli);
static void transfer_cancel(struct bt_mesh_blob_cli *cli);

static void start_retry_timer(struct bt_mesh_blob_cli *cli)
{
	k_timeout_t time;

	if (cli->xfer && cli->xfer->mode == BT_MESH_BLOB_XFER_MODE_PULL) {
		time = RETRY_TIME_PULL;
	} else {
		time = K_MSEC((10 * MSEC_PER_SEC * (cli->ctx->timeout_base + 2) +
			       100 * cli->ctx->ttl) /
			      CONFIG_BT_MESH_BLOB_CLI_BLOCK_RETRIES);
	}

	k_delayed_work_submit(&cli->tx.retry, time);
}

static void blob_cli_reset(struct bt_mesh_blob_cli *cli)
{
	k_delayed_work_cancel(&cli->tx.retry);
	cli->xfer = NULL;
	cli->state = BT_MESH_BLOB_CLI_STATE_NONE;
	cli->tx.ctx = NULL;
	cli->tx.sending = 0;
}

static struct bt_mesh_blob_target *target_get(struct bt_mesh_blob_cli *cli,
					      uint16_t addr)
{
	struct bt_mesh_blob_target *target;

	TARGETS_FOR_EACH(cli, target) {
		if (target->addr == addr) {
			return target;
		}
	}

	BT_ERR("Unknown target 0x%04x", addr);
	return NULL;
}

static void target_drop(struct bt_mesh_blob_cli *cli,
			struct bt_mesh_blob_target *target,
			enum bt_mesh_blob_status reason)
{
	BT_WARN("Dropping 0x%04x: %u", target->addr, reason);

	target->status = reason;
	if (cli->cb && cli->cb->lost_target) {
		cli->cb->lost_target(cli, target, reason);
	}
}

static uint32_t targets_reset(struct bt_mesh_blob_cli *cli)
{
	struct bt_mesh_blob_target *target;
	uint32_t count = 0;

	TARGETS_FOR_EACH(cli, target) {
		if (target->status == BT_MESH_BLOB_SUCCESS) {
			target->acked = 0U;
			count++;
		}
	}

	return count;
}

static bool targets_active(struct bt_mesh_blob_cli *cli)
{
	struct bt_mesh_blob_target *target;

	TARGETS_FOR_EACH(cli, target) {
		if (target->status == BT_MESH_BLOB_SUCCESS) {
			return true;
		}
	}

	return false;
}

static int io_open(struct bt_mesh_blob_cli *cli)
{
	if (!cli->io->open) {
		return 0;
	}

	return cli->io->open(cli->io, cli->xfer, BT_MESH_BLOB_READ);
}

static void io_close(struct bt_mesh_blob_cli *cli)
{
	if (!cli->io->close) {
		return;
	}

	cli->io->close(cli->io, cli->xfer);
}

static uint16_t next_missing_chunk(struct bt_mesh_blob_cli *cli, uint16_t idx)
{
	while (!blob_chunk_missing_get(&cli->block, idx)) {
		if (++idx >= cli->block.chunk_count) {
			break;
		}
	}

	return idx;
}

static inline size_t chunk_size(const struct bt_mesh_blob_block *block,
				uint16_t chunk_idx)
{
	if ((chunk_idx == block->chunk_count - 1) &&
	    (block->size % block->chunk_size)) {
		return block->size % block->chunk_size;
	}

	return block->chunk_size;
}

static int chunk_idx_decode(struct net_buf_simple *buf)
{
	uint16_t data;
	uint8_t byte;

	if (buf->len == 0) {
		return -EINVAL;
	}

	byte = net_buf_simple_pull_u8(buf);

	/* utf-8 decoding */
	if ((byte & 0xf0) == 0xe0) { /* 0x800 - 0xffff */
		if (buf->len < 2) {
			return -EINVAL;
		}

		data = (byte & 0x0f) << 12;
		data |= (net_buf_simple_pull_u8(buf) & 0x3f) << 6;
		data |= (net_buf_simple_pull_u8(buf) & 0x3f);
	} else if ((byte & 0xe0) == 0xc0) { /* 0x80 - 0x7ff */
		if (buf->len < 1) {
			return -EINVAL;
		}

		data = (byte & 0x1f) << 6;
		data |= (net_buf_simple_pull_u8(buf) & 0x3f);
	} else { /* 0x00 - 0x7f */
		data = byte & 0x7f;
	}

	return data;
}

static void block_set(struct bt_mesh_blob_cli *cli, uint16_t block_idx)
{
	cli->block.number = block_idx;
	cli->block.offset = block_idx * (1UL << cli->block_size_log);
	cli->block.size = blob_block_size(cli->xfer->size, cli->block_size_log,
					  block_idx);
	cli->block.chunk_count =
		ceiling_fraction(cli->block.size, cli->block.chunk_size);

	if (cli->xfer->mode == BT_MESH_BLOB_XFER_MODE_PUSH) {
		blob_chunk_missing_set_all(&cli->block);
	} else {
		/* In pull mode, the server will tell us which blocks are
		 * missing.
		 */
		memset(cli->block.missing, 0, sizeof(cli->block.missing));
	}

	BT_DBG("%u size: %u chunks: %u", block_idx, cli->block.size,
	       cli->block.chunk_count);
}

static void end(struct bt_mesh_blob_cli *cli, bool success)
{
	const struct bt_mesh_blob_xfer *xfer = cli->xfer;

	BT_DBG("%u", success);

	io_close(cli);
	blob_cli_reset(cli);
	if (cli->cb && cli->cb->end) {
		cli->cb->end(cli, xfer, success);
	}
}

static enum bt_mesh_blob_status
bounds_apply(struct bt_mesh_blob_cli_bounds *b,
	     const struct bt_mesh_blob_cli_bounds *in)
{
	if (!(in->modes & b->modes)) {
		return BT_MESH_BLOB_ERR_UNSUPPORTED_MODE;
	}

	if ((in->min_block_size_log > b->max_block_size_log) ||
	    (in->max_block_size_log < b->min_block_size_log)) {
		return BT_MESH_BLOB_ERR_INVALID_BLOCK_SIZE;
	}

	b->min_block_size_log =
		MAX(b->min_block_size_log, in->min_block_size_log);
	b->max_block_size_log =
		MIN(b->max_block_size_log, in->max_block_size_log);
	b->max_chunks = MIN(b->max_chunks, in->max_chunks);
	b->mtu_size = MIN(b->mtu_size, in->mtu_size);
	b->chunk_size = MIN(b->chunk_size, in->chunk_size);
	b->modes &= in->modes;
	b->max_size = MIN(b->max_size, in->max_size);

	return BT_MESH_BLOB_SUCCESS;
}
/*******************************************************************************
 * TX State machine
 *
 * All messages in the transfer are going out to all the targets, either through
 * group messaging or directly to each. The TX state machine implements this
 * pattern for the transfer state machine to use. It will send the messages to
 * all devices (through the group or directly), repeating until it receives a
 * response from each device, or the attempts run out. Messages may also be
 * marked as unacked if they require no response.
 ******************************************************************************/

static struct bt_mesh_blob_target *next_target(struct bt_mesh_blob_cli *cli)
{
	if (cli->tx.target) {
		cli->tx.target = SYS_SLIST_PEEK_NEXT_CONTAINER(cli->tx.target, n);
	} else {
		cli->tx.target = SYS_SLIST_PEEK_HEAD_CONTAINER(
			(sys_slist_t *)&cli->ctx->targets, cli->tx.target, n);
	}

	while (cli->tx.target &&
	       (cli->tx.target->acked || cli->tx.target->procedure_complete ||
		cli->tx.target->status != BT_MESH_BLOB_SUCCESS)) {
		cli->tx.target = SYS_SLIST_PEEK_NEXT_CONTAINER(cli->tx.target, n);
	}

	return cli->tx.target;
}

static void send(struct bt_mesh_blob_cli *cli)
{
	cli->tx.sending = 1U;
	if (UNICAST_MODE(cli)) {
		cli->tx.ctx->send(cli, cli->tx.target->addr);
	} else {
		cli->tx.ctx->send(cli, cli->ctx->group);
	}
}

static void broadcast_complete(struct bt_mesh_blob_cli *cli)
{
	const struct blob_cli_broadcast_ctx *ctx = cli->tx.ctx;

	BT_DBG("%s", cli->tx.cancelled ? "cancelling" : "continuing");

	cli->tx.ctx = NULL;
	k_delayed_work_cancel(&cli->tx.retry);
	if (cli->tx.cancelled) {
		transfer_cancel(cli);
	} else {
		__ASSERT(ctx && ctx->next, "NULL ctx");
		ctx->next(cli);
	}
}

static void tx_complete(struct k_work *work)
{
	struct bt_mesh_blob_cli *cli =
		CONTAINER_OF(work, struct bt_mesh_blob_cli, tx.complete);

	if (!cli->tx.ctx || !cli->tx.sending) {
		return;
	}

	cli->tx.sending = 0U;

	if (cli->tx.cancelled) {
		broadcast_complete(cli);
		return;
	}

	if (UNICAST_MODE(cli) && next_target(cli)) {
		send(cli);
		return;
	}

	if (cli->tx.ctx->acked && cli->tx.pending) {
		start_retry_timer(cli);
		return;
	}

	broadcast_complete(cli);
}

static void drop_remaining_targets(struct bt_mesh_blob_cli *cli)
{
	struct bt_mesh_blob_target *target;

	BT_DBG("");

	cli->tx.pending = 0;

	TARGETS_FOR_EACH(cli, target) {
		if (!target->acked) {
			target_drop(cli, target, BT_MESH_BLOB_ERR_INTERNAL);
		}
	}
}

static void retry_timeout(struct k_work *work)
{
	struct bt_mesh_blob_cli *cli =
		CONTAINER_OF(work, struct bt_mesh_blob_cli, tx.retry.work);

	BT_DBG("%u", cli->tx.retries);

	cli->tx.retries--;
	cli->tx.target = NULL;

	__ASSERT(!cli->tx.sending, "still sending");
	__ASSERT(cli->tx.ctx, "has ctx");

	if (!cli->tx.retries) {
		if (!cli->tx.ctx->optional) {
			drop_remaining_targets(cli);
		}

		broadcast_complete(cli);
		return;
	}

	if (!cli->tx.ctx->acked || !next_target(cli) || cli->tx.cancelled) {
		broadcast_complete(cli);
		return;
	}

	send(cli);
}

void blob_cli_broadcast(struct bt_mesh_blob_cli *cli,
			const struct blob_cli_broadcast_ctx *ctx)
{
	if (cli->tx.ctx || cli->tx.sending) {
		BT_ERR("BLOB cli busy");
		return;
	}

	cli->tx.cancelled = 0U;
	cli->tx.retries = CONFIG_BT_MESH_BLOB_CLI_BLOCK_RETRIES;
	cli->tx.ctx = ctx;

	cli->tx.pending = targets_reset(cli);

	BT_DBG("%u targets", cli->tx.pending);

	cli->tx.target = NULL;
	if (!next_target(cli)) {
		BT_ERR("No active targets");
		broadcast_complete(cli);
		return;
	}

	send(cli);
}

void blob_cli_broadcast_tx_complete(struct bt_mesh_blob_cli *cli)
{
	k_work_submit(&cli->tx.complete);
}

void blob_cli_broadcast_rsp(struct bt_mesh_blob_cli *cli,
			    struct bt_mesh_blob_target *target)
{
	if (target->acked) {
		return;
	}

	BT_DBG("0x%04x", target->addr);

	target->acked = 1U;

	if (!--cli->tx.pending && !cli->tx.sending) {
		broadcast_complete(cli);
	}
}

void blob_cli_broadcast_abort(struct bt_mesh_blob_cli *cli)
{
	if (!cli->tx.ctx) {
		return;
	}

	if ((cli)->state >= BT_MESH_BLOB_CLI_STATE_START) {
		io_close(cli);
	}

	blob_cli_reset(cli);
}

static void send_start(uint16_t duration, int err, void *cb_data);
static void send_end(int err, void *user_data);

static int tx(struct bt_mesh_blob_cli *cli, uint16_t addr,
	      struct net_buf_simple *buf)
{
	static const struct bt_mesh_send_cb end_cb = {
		.start = send_start,
		.end = send_end,
	};
	struct bt_mesh_msg_ctx ctx = {
		.app_idx = cli->ctx->app_idx,
		.addr = addr,
		.send_ttl = cli->ctx->ttl,
	};
	int err;

	err = bt_mesh_model_send(cli->mod, &ctx, buf, &end_cb, cli);
	if (err) {
		BT_ERR("Send err: %d", err);
		send_end(err, cli);
		return err;
	}

	return 0;
}

static void send_start(uint16_t duration, int err, void *cb_data)
{
	if (err) {
		BT_ERR("TX Start failed: %d", err);
		send_end(err, cb_data);
	}
}

static void send_end(int err, void *user_data)
{
	struct bt_mesh_blob_cli *cli = user_data;

	if (!cli->tx.ctx) {
		return;
	}

	blob_cli_broadcast_tx_complete(cli);
}

/*******************************************************************************
 * TX
 ******************************************************************************/

static void info_get_tx(struct bt_mesh_blob_cli *cli, uint16_t dst)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, BT_MESH_BLOB_OP_INFO_GET, 0);
	bt_mesh_model_msg_init(&buf, BT_MESH_BLOB_OP_INFO_GET);

	tx(cli, dst, &buf);
}

static void xfer_start_tx(struct bt_mesh_blob_cli *cli, uint16_t dst)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, BT_MESH_BLOB_OP_XFER_START, 16);
	bt_mesh_model_msg_init(&buf, BT_MESH_BLOB_OP_XFER_START);
	net_buf_simple_add_u8(&buf, cli->xfer->mode << 6);
	net_buf_simple_add_le64(&buf, cli->xfer->id);
	net_buf_simple_add_le32(&buf, cli->xfer->size);
	net_buf_simple_add_u8(&buf, cli->block_size_log);
	net_buf_simple_add_le16(&buf, BT_MESH_TX_SDU_MAX);

	tx(cli, dst, &buf);
}

static void xfer_get_tx(struct bt_mesh_blob_cli *cli, uint16_t dst)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, BT_MESH_BLOB_OP_XFER_GET, 0);
	bt_mesh_model_msg_init(&buf, BT_MESH_BLOB_OP_XFER_GET);

	tx(cli, dst, &buf);
}

static void xfer_cancel_tx(struct bt_mesh_blob_cli *cli, uint16_t dst)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, BT_MESH_BLOB_OP_XFER_CANCEL, 8);
	bt_mesh_model_msg_init(&buf, BT_MESH_BLOB_OP_XFER_CANCEL);
	net_buf_simple_add_le64(&buf, cli->xfer->id);

	tx(cli, dst, &buf);
}

static void block_start_tx(struct bt_mesh_blob_cli *cli, uint16_t dst)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, BT_MESH_BLOB_OP_BLOCK_START, 4);
	bt_mesh_model_msg_init(&buf, BT_MESH_BLOB_OP_BLOCK_START);
	net_buf_simple_add_le16(&buf, cli->block.number);
	net_buf_simple_add_le16(&buf, cli->block.chunk_size);

	tx(cli, dst, &buf);
}

static void chunk_tx(struct bt_mesh_blob_cli *cli, uint16_t dst)
{
	NET_BUF_SIMPLE_DEFINE(buf, BT_MESH_TX_SDU_MAX);
	struct bt_mesh_blob_chunk chunk;
	int err;

	bt_mesh_model_msg_init(&buf, BT_MESH_BLOB_OP_CHUNK);
	net_buf_simple_add_le16(&buf, cli->chunk_idx);

	chunk.size = chunk_size(&cli->block, cli->chunk_idx);
	chunk.offset = cli->block.chunk_size * cli->chunk_idx;
	chunk.data = net_buf_simple_add(&buf, chunk.size);

	err = cli->io->rd(cli->io, cli->xfer, &cli->block, &chunk);
	if (err || cli->state == BT_MESH_BLOB_CLI_STATE_NONE) {
		bt_mesh_blob_cli_cancel(cli);
		return;
	}

	tx(cli, dst, &buf);
}

static void block_get_tx(struct bt_mesh_blob_cli *cli, uint16_t dst)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, BT_MESH_BLOB_OP_BLOCK_GET, 0);
	bt_mesh_model_msg_init(&buf, BT_MESH_BLOB_OP_BLOCK_GET);

	tx(cli, dst, &buf);
}

/*******************************************************************************
 * State machine
 *
 * The BLOB Client state machine walks through the steps in the BLOB transfer in
 * the following fashion:
 *
 *                                                 .----[No]-----.
 *                                                 V             |
 * xfer_start -> block_start -> block_send -> chunk_send ->[block complete?]
 *                                  A                            |
 *                                  |                          [Yes]
 *                                  |                            |
 *                                  |                            V
 *                                  '----[No]------------[Transfer complete?]
 *                                                               |
 *                                                             [Yes]
 *                                                               V
 *                                                        transfer_complete
 *
 * In each state, the Client transmits a message to all target nodes, and once
 * all nodes have received the message, it moves on to the next state.
 *
 ******************************************************************************/
static void bounds_collected(struct bt_mesh_blob_cli *cli);
static void block_start(struct bt_mesh_blob_cli *cli);
static void chunk_send(struct bt_mesh_blob_cli *cli);
static void block_check(struct bt_mesh_blob_cli *cli);
static void block_check_end(struct bt_mesh_blob_cli *cli);
static void chunk_send_end(struct bt_mesh_blob_cli *cli);
static void confirm_transfer(struct bt_mesh_blob_cli *cli);
static void transfer_complete(struct bt_mesh_blob_cli *cli);

static void bounds_check(struct bt_mesh_blob_cli *cli)
{
	static const struct blob_cli_broadcast_ctx ctx = {
		.send = info_get_tx,
		.next = bounds_collected,
		.acked = true,
	};

	cli->state = BT_MESH_BLOB_CLI_STATE_BOUNDS_CHECK;
	blob_cli_broadcast(cli, &ctx);
}

static void bounds_collected(struct bt_mesh_blob_cli *cli)
{
	cli->state = BT_MESH_BLOB_CLI_STATE_NONE;

	blob_cli_reset(cli);

	while ((1UL << cli->bounds->max_block_size_log) >
	       (cli->bounds->chunk_size * cli->bounds->max_chunks)) {
		cli->bounds->max_block_size_log--;
	}

	if (cli->cb && cli->cb->bounds) {
		cli->cb->bounds(cli, cli->bounds);
	}
}

static int xfer_start(struct bt_mesh_blob_cli *cli)
{
	static const struct blob_cli_broadcast_ctx ctx = {
		.send = xfer_start_tx,
		.next = block_start,
		.acked = true,
	};
	int err;

	err = io_open(cli);
	if (err) {
		return -EIO;
	}

	cli->state = BT_MESH_BLOB_CLI_STATE_START;

	blob_cli_broadcast(cli, &ctx);
	return 0;
}

static void block_start(struct bt_mesh_blob_cli *cli)
{
	static const struct blob_cli_broadcast_ctx ctx = {
		.send = block_start_tx,
		.next = chunk_send,
		.acked = true,
	};
	struct bt_mesh_blob_target *target;

	if (!targets_active(cli)) {
		end(cli, false);
		return;
	}

	BT_DBG("%u (%u chunks, %u/%u)", cli->block.number,
	       cli->block.chunk_count, cli->block.number + 1, cli->block_count);

	cli->chunk_idx = 0;
	cli->tx.polls = CONFIG_BT_MESH_BLOB_CLI_BLOCK_RETRIES;
	cli->state = BT_MESH_BLOB_CLI_STATE_BLOCK_START;

	TARGETS_FOR_EACH(cli, target) {
		target->procedure_complete = 0U;
	}

	if (cli->io->block_start) {
		cli->io->block_start(cli->io, cli->xfer, &cli->block);
		if (cli->state == BT_MESH_BLOB_CLI_STATE_NONE) {
			return;
		}
	}

	blob_cli_broadcast(cli, &ctx);
}

static void chunk_send(struct bt_mesh_blob_cli *cli)
{
	static const struct blob_cli_broadcast_ctx ctx = {
		.send = chunk_tx,
		.next = chunk_send_end,
		.acked = false,
	};

	if (!targets_active(cli)) {
		end(cli, false);
		return;
	}

	BT_DBG("%u / %u size: %u", cli->chunk_idx + 1, cli->block.chunk_count,
	       chunk_size(&cli->block, cli->chunk_idx));

	cli->state = BT_MESH_BLOB_CLI_STATE_BLOCK_SEND;
	blob_cli_broadcast(cli, &ctx);
}

static void chunk_send_end(struct bt_mesh_blob_cli *cli)
{
	static const struct blob_cli_broadcast_ctx ctx = {
		.next = chunk_send,
		.acked = false,
	};

	/* In pull mode, the partial block reports are used to confirm which
	 * chunks have been received, while in push mode, we just assume that a
	 * sent chunk has been received.
	 */
	if (cli->xfer->mode == BT_MESH_BLOB_XFER_MODE_PUSH) {
		blob_chunk_missing_set(&cli->block, cli->chunk_idx, false);
	}

	cli->chunk_idx = next_missing_chunk(cli, cli->chunk_idx + 1);
	if (cli->chunk_idx < cli->block.chunk_count) {
		chunk_send(cli);
		return;
	}

	if (cli->xfer->mode == BT_MESH_BLOB_XFER_MODE_PUSH) {
		block_check(cli);
		return;
	}

	cli->tx.polls--;
	if (!cli->tx.polls) {
		drop_remaining_targets(cli);
		end(cli, false);
		return;
	}

	BT_DBG("Waiting for partial block report... (%u)", cli->tx.polls);
	cli->chunk_idx = next_missing_chunk(cli, 0);

	cli->tx.ctx = &ctx;
	start_retry_timer(cli);
}

static void block_check(struct bt_mesh_blob_cli *cli)
{
	static const struct blob_cli_broadcast_ctx ctx = {
		.send = block_get_tx,
		.next = block_check_end,
		.acked = true,
	};

	cli->state = BT_MESH_BLOB_CLI_STATE_BLOCK_CHECK;

	BT_DBG("");

	/* In pull mode, the block check procedure doesn't require any status
	 * request, the server will send a block report unprompted. For this
	 * case, we'll just run the retry timer without starting a TX, allowing
	 * it to time out and stop the transfer if no block report came.
	 */
	if (cli->xfer->mode == BT_MESH_BLOB_XFER_MODE_PUSH) {
		blob_cli_broadcast(cli, &ctx);
	} else {
		cli->tx.retries = 0;
		cli->tx.ctx = &ctx;
		start_retry_timer(cli);
	}
}

static void block_check_end(struct bt_mesh_blob_cli *cli)
{
	BT_DBG("");

	if (!targets_active(cli)) {
		end(cli, false);
		return;
	}

	cli->chunk_idx = next_missing_chunk(cli, 0);
	if (cli->chunk_idx < cli->block.chunk_count) {
		chunk_send(cli);
		return;
	}

	if (cli->block.number == cli->block_count - 1) {
		struct bt_mesh_blob_target *target;

		TARGETS_FOR_EACH(cli, target) {
			target->procedure_complete = 0U;
		}

		confirm_transfer(cli);
		return;
	}

	if (cli->io->block_end) {
		cli->io->block_end(cli->io, cli->xfer, &cli->block);
		if (cli->state == BT_MESH_BLOB_CLI_STATE_NONE) {
			return;
		}
	}

	block_set(cli, cli->block.number + 1);
	block_start(cli);
}

static void confirm_transfer(struct bt_mesh_blob_cli *cli)
{
	static const struct blob_cli_broadcast_ctx ctx = {
		.send = xfer_get_tx,
		.next = transfer_complete,
		.acked = true,
	};

	BT_DBG("");

	cli->state = BT_MESH_BLOB_CLI_STATE_XFER_CHECK;

	blob_cli_broadcast(cli, &ctx);
}

static void transfer_cancel(struct bt_mesh_blob_cli *cli)
{
	static const struct blob_cli_broadcast_ctx ctx = {
		.send = xfer_cancel_tx,
		.next = transfer_complete,
		.acked = true,
	};

	BT_DBG("");

	cli->state = BT_MESH_BLOB_CLI_STATE_CANCEL;

	blob_cli_broadcast(cli, &ctx);
}

static void transfer_complete(struct bt_mesh_blob_cli *cli)
{
	bool success = targets_active(cli) &&
		       cli->state == BT_MESH_BLOB_CLI_STATE_XFER_CHECK;

	end(cli, success);
}

/*******************************************************************************
 * RX
 ******************************************************************************/

static void rx_block_status(struct bt_mesh_blob_cli *cli,
			    struct bt_mesh_msg_ctx *ctx,
			    struct block_status *block)
{
	struct bt_mesh_blob_target *target;

	if (cli->state != BT_MESH_BLOB_CLI_STATE_BLOCK_START &&
	    cli->state != BT_MESH_BLOB_CLI_STATE_BLOCK_SEND &&
	    cli->state != BT_MESH_BLOB_CLI_STATE_BLOCK_CHECK) {
		BT_WARN("Invalid state %u", cli->state);
		return;
	}

	target = target_get(cli, ctx->addr);
	if (!target) {
		return;
	}

	BT_DBG("#%u status: %u", block->block.number, block->status);

	if (block->status != BT_MESH_BLOB_SUCCESS) {
		target_drop(cli, target, block->status);
		blob_cli_broadcast_rsp(cli, target);
		return;
	}

	if (block->block.number != cli->block.number) {
		BT_DBG("Invalid block num (expected %u)", cli->block.number);
		return;
	}

	if (block->missing == BT_MESH_BLOB_CHUNKS_MISSING_NONE) {
		target->procedure_complete = 1U;
	} else if (block->missing == BT_MESH_BLOB_CHUNKS_MISSING_ALL) {
		blob_chunk_missing_set_all(&cli->block);
	} else if (cli->xfer->mode == BT_MESH_BLOB_XFER_MODE_PULL) {
		memcpy(cli->block.missing, block->block.missing,
		       sizeof(cli->block.missing));
	} else {
		for (int i = 0; i < ARRAY_SIZE(block->block.missing); ++i) {
			cli->block.missing[i] |= block->block.missing[i];
		}
	}

	blob_cli_broadcast_rsp(cli, target);
}

static int handle_xfer_status(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			      struct net_buf_simple *buf)
{
	struct bt_mesh_blob_cli *cli = mod->user_data;
	enum bt_mesh_blob_xfer_phase expected_phase;
	struct bt_mesh_blob_target *target;
	struct xfer_info info = { 0 };
	uint8_t status_and_mode;

	status_and_mode = net_buf_simple_pull_u8(buf);
	info.status = status_and_mode & BIT_MASK(4);
	info.mode = status_and_mode >> 6;
	info.phase = net_buf_simple_pull_u8(buf);

	if (buf->len) {
		info.id = net_buf_simple_pull_le64(buf);
	}

	if (buf->len >= 7) {
		info.size = net_buf_simple_pull_le32(buf);
		info.block_size_log = net_buf_simple_pull_u8(buf);
		info.mtu_size = net_buf_simple_pull_le16(buf);
		info.missing_blocks = net_buf_simple_pull(buf, buf->len);
	}

	BT_DBG("status: %u %s phase: %u %s", info.status,
	       info.mode == BT_MESH_BLOB_XFER_MODE_PUSH ? "push" : "pull",
	       info.phase, bt_hex(&info.id, 8));


	if (cli->state != BT_MESH_BLOB_CLI_STATE_START &&
	    cli->state != BT_MESH_BLOB_CLI_STATE_XFER_CHECK &&
	    cli->state != BT_MESH_BLOB_CLI_STATE_CANCEL) {
		BT_WARN("Wrong state: %d", cli->state);
		return -EBUSY;
	}

	target = target_get(cli, ctx->addr);
	if (!target) {
		return -ENOENT;
	}

	if (cli->state == BT_MESH_BLOB_CLI_STATE_START) {
		expected_phase = BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_BLOCK;
	} else if (cli->state == BT_MESH_BLOB_CLI_STATE_XFER_CHECK) {
		expected_phase = BT_MESH_BLOB_XFER_PHASE_COMPLETE;
	} else {
		expected_phase = BT_MESH_BLOB_XFER_PHASE_INACTIVE;
	}

	if (info.status != BT_MESH_BLOB_SUCCESS) {
		target_drop(cli, target, info.status);
	} else if (info.phase != expected_phase) {
		BT_WARN("Wrong phase: %u != %u", expected_phase, info.phase);
		return -EINVAL;
	} else if (info.phase != BT_MESH_BLOB_XFER_PHASE_INACTIVE &&
		   info.id != cli->xfer->id) {
		target_drop(cli, target, BT_MESH_BLOB_ERR_WRONG_BLOB_ID);
	}

	blob_cli_broadcast_rsp(cli, target);

	return 0;
}

static int handle_block_report(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			       struct net_buf_simple *buf)
{
	struct bt_mesh_blob_cli *cli = mod->user_data;
	struct block_status status = {
		.status = BT_MESH_BLOB_SUCCESS,
		.block.number = cli->block.number,
		.missing = (buf->len ? BT_MESH_BLOB_CHUNKS_MISSING_ENCODED :
				       BT_MESH_BLOB_CHUNKS_MISSING_NONE),
	};

	if (cli->xfer->mode == BT_MESH_BLOB_XFER_MODE_PUSH) {
		BT_WARN("Unexpected encoded block report in push mode");
		return -EINVAL;
	}

	BT_DBG("");

	while (buf->len) {
		int idx;

		idx = chunk_idx_decode(buf);
		if (idx < 0) {
			return idx;
		}

		blob_chunk_missing_set(&status.block, idx, true);
	}

	rx_block_status(cli, ctx, &status);

	return 0;
}

static int handle_block_status(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			       struct net_buf_simple *buf)
{
	struct bt_mesh_blob_cli *cli = mod->user_data;
	struct block_status status = { 0 };
	uint8_t status_and_format;
	uint16_t chunk_size;
	size_t len;
	int idx;

	status_and_format = net_buf_simple_pull_u8(buf);
	status.status = status_and_format & BIT_MASK(4);
	status.missing = status_and_format >> 6;
	status.block.number = net_buf_simple_pull_le16(buf);
	chunk_size = net_buf_simple_pull_le16(buf);
	status.block.chunk_count =
		ceiling_fraction(cli->block.size, chunk_size);

	BT_DBG("status: %u block: %u encoding: %u", status.status,
	       status.block.number, status.missing);

	switch (status.missing) {
	case BT_MESH_BLOB_CHUNKS_MISSING_ALL:
		blob_chunk_missing_set_all(&status.block);
		break;
	case BT_MESH_BLOB_CHUNKS_MISSING_NONE:
		break;
	case BT_MESH_BLOB_CHUNKS_MISSING_SOME:
		if (buf->len > sizeof(status.block.missing)) {
			return -EINVAL;
		}

		len = buf->len;
		memcpy(status.block.missing, net_buf_simple_pull_mem(buf, len),
		       len);

		BT_DBG("Missing: %s", bt_hex(status.block.missing, len));
		break;
	case BT_MESH_BLOB_CHUNKS_MISSING_ENCODED:
		while (buf->len) {
			idx = chunk_idx_decode(buf);
			if (idx < 0 || idx >= status.block.chunk_count) {
				BT_ERR("Invalid encoding");
				return -EINVAL;
			}

			BT_DBG("Missing %d", idx);

			blob_chunk_missing_set(&status.block, idx, true);
		}
		break;
	}

	rx_block_status(cli, ctx, &status);

	return 0;
}

static int handle_info_status(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			      struct net_buf_simple *buf)
{
	struct bt_mesh_blob_cli *cli = mod->user_data;
	struct bt_mesh_blob_cli_bounds bounds;
	enum bt_mesh_blob_status status;
	struct bt_mesh_blob_target *target;

	if (cli->state != BT_MESH_BLOB_CLI_STATE_BOUNDS_CHECK) {
		return -EBUSY;
	}

	bounds.min_block_size_log = net_buf_simple_pull_u8(buf);
	bounds.max_block_size_log = net_buf_simple_pull_u8(buf);
	bounds.max_chunks = net_buf_simple_pull_le16(buf);
	bounds.chunk_size = net_buf_simple_pull_le16(buf);
	bounds.max_size = net_buf_simple_pull_le32(buf);
	bounds.mtu_size = net_buf_simple_pull_le16(buf);
	bounds.modes = net_buf_simple_pull_u8(buf);

	if (bounds.min_block_size_log < 0x06 ||
	    bounds.max_block_size_log > 0x20 ||
	    bounds.max_block_size_log < bounds.min_block_size_log ||
	    bounds.max_chunks == 0 || bounds.chunk_size < 8 ||
	    bounds.max_size == 0 || bounds.mtu_size < 0x14) {
		return -EINVAL;
	}

	BT_DBG("0x%04x\n\tblock size: %u - %u\n\tchunks: %u\n\tchunk size: %u\n"
	       "\tblob size: %u\n\tmtu size: %u\n\tmodes: %x",
	       ctx->addr, bounds.min_block_size_log, bounds.max_block_size_log,
	       bounds.max_chunks, bounds.chunk_size, bounds.max_size,
	       bounds.mtu_size, bounds.modes);


	target = target_get(cli, ctx->addr);
	if (!target) {
		return -ENOENT;
	}

	status = bounds_apply(cli->bounds, &bounds);
	if (status != BT_MESH_BLOB_SUCCESS) {
		target_drop(cli, target, status);
	}

	blob_cli_broadcast_rsp(cli, target);

	return 0;
}

const struct bt_mesh_model_op _bt_mesh_blob_cli_op[] = {
	{ BT_MESH_BLOB_OP_XFER_STATUS, BT_MESH_LEN_MIN(2), handle_xfer_status },
	{ BT_MESH_BLOB_OP_BLOCK_REPORT, BT_MESH_LEN_MIN(0), handle_block_report },
	{ BT_MESH_BLOB_OP_BLOCK_STATUS, BT_MESH_LEN_MIN(5), handle_block_status },
	{ BT_MESH_BLOB_OP_INFO_STATUS, BT_MESH_LEN_EXACT(13), handle_info_status },
	BT_MESH_MODEL_OP_END,
};

static int blob_cli_init(struct bt_mesh_model *mod)
{
	struct bt_mesh_blob_cli *cli = mod->user_data;

	cli->mod = mod;

	k_delayed_work_init(&cli->tx.retry, retry_timeout);
	k_work_init(&cli->tx.complete, tx_complete);

	return 0;
}

const struct bt_mesh_model_cb _bt_mesh_blob_cli_cb = {
	.init = blob_cli_init,
};

int bt_mesh_blob_cli_bounds_check(struct bt_mesh_blob_cli *cli,
				  const struct bt_mesh_blob_cli_ctx *ctx,
				  struct bt_mesh_blob_cli_bounds *bounds)
{
	if (bt_mesh_blob_cli_is_busy(cli)) {
		return -EBUSY;
	}

	if (bounds_apply(bounds, &bt_mesh_blob_cli_boundaries) !=
	    BT_MESH_BLOB_SUCCESS) {
		BT_ERR("Bounds incompatible with client capabilities");
		return -EINVAL;
	}

	cli->ctx = ctx;
	cli->bounds = bounds;

	if (!targets_reset(cli)) {
		BT_ERR("No valid targets");
		return -ENODEV;
	}

	bounds_check(cli);

	return 0;
}

int bt_mesh_blob_cli_send(struct bt_mesh_blob_cli *cli,
			  const struct bt_mesh_blob_cli_ctx *ctx,
			  const struct bt_mesh_blob_xfer *xfer,
			  const struct bt_mesh_blob_cli_bounds *bounds,
			  const struct bt_mesh_blob_io *io)
{
	if (bt_mesh_blob_cli_is_busy(cli)) {
		BT_ERR("BLOB Client is busy");
		return -EBUSY;
	}

	if (!cli || !ctx || !xfer || !io) {
		return -EINVAL;
	}

	if (bounds) {
		cli->block_size_log = bounds->max_block_size_log;
		cli->block.chunk_size = bounds->chunk_size;
	} else {
		cli->block_size_log = BLOB_BLOCK_SIZE_LOG_MAX;
		cli->block.chunk_size = CHUNK_SIZE_MAX;
	}

	cli->io = io;
	cli->ctx = ctx;
	cli->xfer = xfer;
	cli->block_count = ceiling_fraction(cli->xfer->size,
					    (1U << cli->block_size_log));
	if (!targets_reset(cli)) {
		BT_ERR("No valid targets");
		return -ENODEV;
	}

	BT_DBG("\n\tblock size: %u\n\tchunk size: %u\n"
	       "\tblob size: %u\n\tmode: %x",
	       (1 << cli->block_size_log), cli->block.chunk_size,
	       cli->xfer->size, cli->xfer->mode);

	block_set(cli, 0);

	return xfer_start(cli);
}

void bt_mesh_blob_cli_cancel(struct bt_mesh_blob_cli *cli)
{
	if (!bt_mesh_blob_cli_is_busy(cli)) {
		BT_WARN("BLOB xfer already cancelled");
		return;
	}

	BT_DBG("");

	if (cli->state == BT_MESH_BLOB_CLI_STATE_BOUNDS_CHECK) {
		blob_cli_reset(cli);
		return;
	}

	cli->tx.cancelled = 1U;
	cli->state = BT_MESH_BLOB_CLI_STATE_CANCEL;
}

uint8_t bt_mesh_blob_cli_progress(struct bt_mesh_blob_cli *cli)
{
	if (cli->state < BT_MESH_BLOB_CLI_STATE_START) {
		return 0;
	}

	return (100U * cli->block.number) / cli->block_count;
}

bool bt_mesh_blob_cli_is_busy(struct bt_mesh_blob_cli *cli)
{
	return cli->state != BT_MESH_BLOB_CLI_STATE_NONE;
}
