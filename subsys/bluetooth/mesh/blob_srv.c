/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <bluetooth/mesh.h>
#include "net.h"
#include "access.h"
#include "transport.h"
#include "lpn.h"
#include "blob.h"

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_MODEL)
#define LOG_MODULE_NAME bt_mesh_blob_srv
#include "common/log.h"

#define CHUNK_SIZE_MAX BLOB_CHUNK_SIZE_MAX(BT_MESH_RX_SDU_MAX)
#define MTU_SIZE_MAX (BT_MESH_RX_SDU_MAX - BT_MESH_MIC_SHORT)

#define PULL_BLOB_REQ_COUNT 16
#define PULL_ATTEMPTS 6
#define REPORT_TIMER_TIMEOUT K_SECONDS(BLOB_POLL_TIME_MAX_SECS + 1)

BUILD_ASSERT(BLOB_BLOCK_SIZE_LOG_MIN <= BLOB_BLOCK_SIZE_LOG_MAX,
	     "The must be at least one number between the min and "
	     "max block size that is the power of two.");

static void cancel(struct bt_mesh_blob_srv *srv);
static void suspend(struct bt_mesh_blob_srv *srv);

static inline uint32_t block_count_get(const struct bt_mesh_blob_srv *srv)
{
	return ceiling_fraction(srv->state.xfer.size,
				(1U << srv->state.block_size_log));
}

static inline uint32_t max_chunk_size(const struct bt_mesh_blob_srv *srv)
{
	return MIN((srv->state.mtu_size - 2 -
		    BT_MESH_MODEL_OP_LEN(BT_MESH_BLOB_OP_CHUNK)),
		   CHUNK_SIZE_MAX);
}

static inline uint32_t max_chunk_count(const struct bt_mesh_blob_srv *srv)
{
	return MIN(8 * (srv->state.mtu_size - 6),
		   CONFIG_BT_MESH_BLOB_CHUNK_COUNT_MAX);
}

static inline uint32_t missing_chunks(const struct bt_mesh_blob_block *block)
{
	int i;
	uint32_t count = 0;

	for (i = 0; i < ARRAY_SIZE(block->missing); ++i) {
		count += popcount(block->missing[i]);
	}

	return count;
}

static void store_state(const struct bt_mesh_blob_srv *srv)
{
	if (!IS_ENABLED(CONFIG_BT_SETTINGS)) {
		return;
	}

	/* Convert bit count to byte count: */
	uint32_t block_len = ceiling_fraction(block_count_get(srv), 8);

	bt_mesh_model_data_store(
		srv->mod, false, NULL, &srv->state,
		offsetof(struct bt_mesh_blob_srv_state, blocks) + block_len);
}

static void erase_state(struct bt_mesh_blob_srv *srv)
{
	if (!IS_ENABLED(CONFIG_BT_SETTINGS)) {
		return;
	}

	bt_mesh_model_data_store(srv->mod, false, NULL, NULL, 0);
}

static int io_open(struct bt_mesh_blob_srv *srv)
{
	if (!srv->io->open) {
		return 0;
	}

	return srv->io->open(srv->io, &srv->state.xfer, BT_MESH_BLOB_WRITE);
}

static void io_close(struct bt_mesh_blob_srv *srv)
{
	if (!srv->io->close) {
		return;
	}

	srv->io->close(srv->io, &srv->state.xfer);
}

static void reset_timer(struct bt_mesh_blob_srv *srv)
{
	if (srv->state.xfer.mode != BT_MESH_BLOB_XFER_MODE_PULL) {
		k_delayed_work_submit(
			&srv->rx_timeout,
			K_SECONDS(10 * (1 + srv->state.timeout_base)));
	}
}

static void buf_chunk_index_add(struct net_buf_simple *buf, uint16_t chunk)
{
	/* utf-8 encoded: */
	if (chunk < 0x80) {
		net_buf_simple_add_u8(buf, chunk);
	} else if (chunk < 0x8000) {
		net_buf_simple_add_u8(buf, 0xc0 | chunk >> 6);
		net_buf_simple_add_u8(buf, 0x80 | (chunk & BIT_MASK(6)));
	} else {
		net_buf_simple_add_u8(buf, 0xe0 | chunk >> 12);
		net_buf_simple_add_u8(buf, 0x80 | ((chunk >> 6) & BIT_MASK(6)));
		net_buf_simple_add_u8(buf, 0x80 | (chunk & BIT_MASK(6)));
	}
}

static int pull_req_max(const struct bt_mesh_blob_srv *srv)
{
	int count = PULL_BLOB_REQ_COUNT;

#if defined(CONFIG_BT_MESH_LOW_POWER)
	/* No point in requesting more than the friend node can hold: */
	if (bt_mesh_lpn_established()) {
		uint32_t segments_per_chunk = ceiling_fraction(
			BLOB_CHUNK_SDU_LEN(srv->block.chunk_size),
			BT_MESH_APP_SEG_SDU_MAX);

		count = MIN(PULL_BLOB_REQ_COUNT,
			    bt_mesh.lpn.queue_size / segments_per_chunk);
	}
#endif

	return MIN(count, missing_chunks(&srv->block));
}

static void report_sent(int err, void *cb_data)
{
	struct bt_mesh_blob_srv *srv = cb_data;

	BT_DBG("");

	if (IS_ENABLED(CONFIG_BT_MESH_LOW_POWER) && bt_mesh_lpn_established()) {
		bt_mesh_lpn_poll();
	}

	k_delayed_work_submit(&srv->pull.report, REPORT_TIMER_TIMEOUT);
}

static void block_report(struct bt_mesh_blob_srv *srv)
{
	static const struct bt_mesh_send_cb report_cb = { .end = report_sent };
	struct bt_mesh_msg_ctx ctx = {
		.app_idx = srv->state.app_idx,
		.send_ttl = srv->state.ttl,
		.addr = srv->state.cli,
	};
	int count;
	int i;

	if (!srv->pull.counter--) {
		srv->pull.counter = 0;
		suspend(srv);
		return;
	}

	BT_DBG("remaining: %u", srv->pull.counter);

	BT_MESH_MODEL_BUF_DEFINE(buf, BT_MESH_BLOB_OP_BLOCK_REPORT,
				 PULL_BLOB_REQ_COUNT * 3);
	bt_mesh_model_msg_init(&buf, BT_MESH_BLOB_OP_BLOCK_REPORT);

	count = pull_req_max(srv);

	for (i = 0; i < srv->block.chunk_count && count; ++i) {
		if (blob_chunk_missing_get(&srv->block, i)) {
			buf_chunk_index_add(&buf, i);
			count--;
		}
	}

	bt_mesh_model_send(srv->mod, &ctx, &buf, &report_cb, srv);
}

static void phase_set(struct bt_mesh_blob_srv *srv,
		      enum bt_mesh_blob_xfer_phase phase)
{
	srv->phase = phase;
	BT_DBG("Phase: %u", phase);
}

static void cancel(struct bt_mesh_blob_srv *srv)
{
	/* TODO: Could this state be preserved instead somehow? Wiping the
	 * entire transfer state is a bit overkill
	 */
	phase_set(srv, BT_MESH_BLOB_XFER_PHASE_INACTIVE);
	srv->state.xfer.mode = BT_MESH_BLOB_XFER_MODE_NONE;
	srv->state.ttl = BT_MESH_TTL_DEFAULT;
	srv->block.number = 0xffff;
	srv->block.chunk_size = 0xffff;
	k_delayed_work_cancel(&srv->rx_timeout);
	k_delayed_work_cancel(&srv->pull.report);
	io_close(srv);
	erase_state(srv);

	if (srv->cb && srv->cb->end) {
		srv->cb->end(srv, srv->state.xfer.id, false);
	}
}

static void suspend(struct bt_mesh_blob_srv *srv)
{
	BT_DBG("");
	k_delayed_work_cancel(&srv->rx_timeout);
	phase_set(srv, BT_MESH_BLOB_XFER_PHASE_SUSPENDED);
	if (srv->cb && srv->cb->suspended) {
		srv->cb->suspended(srv);
	}
}

static void resume(struct bt_mesh_blob_srv *srv)
{
	if (srv->phase != BT_MESH_BLOB_XFER_PHASE_SUSPENDED) {
		return;
	}

	phase_set(srv, BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_BLOCK);
	reset_timer(srv);
}

static void timeout(struct k_work *work)
{
	struct bt_mesh_blob_srv *srv =
		CONTAINER_OF(work, struct bt_mesh_blob_srv, rx_timeout.work);

	BT_DBG("");

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_START) {
		cancel(srv);
	} else {
		suspend(srv);
	}
}

static void report_timeout(struct k_work *work)
{
	struct bt_mesh_blob_srv *srv =
		CONTAINER_OF(work, struct bt_mesh_blob_srv, pull.report.work);

	BT_DBG("");

	block_report(srv);
}

static void lpn_poll_visit(struct bt_mesh_model *mod, struct bt_mesh_elem *elem,
			   bool vnd, bool primary, void *user_data)
{
	struct bt_mesh_blob_srv *srv;

	if (vnd || mod->id != BT_MESH_MODEL_ID_BLOB_SRV) {
		return;
	}

	srv = mod->user_data;

	if (srv->state.xfer.mode != BT_MESH_BLOB_XFER_MODE_PULL) {
		return;
	}

	srv->pull.counter = PULL_ATTEMPTS;
	block_report(srv);
}

static void lpn_polled(uint16_t net_idx, uint16_t friend_addr, bool retry)
{
	bt_mesh_model_foreach(lpn_poll_visit, NULL);
}

BT_MESH_LPN_CB_DEFINE(blob_srv_lpn_cb) = {
	.polled = lpn_polled,
};

/*******************************************************************************
 * Message handling
 ******************************************************************************/

static void xfer_status_rsp(struct bt_mesh_blob_srv *srv,
			    struct bt_mesh_msg_ctx *ctx,
			    enum bt_mesh_blob_status status)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, BT_MESH_BLOB_OP_XFER_STATUS,
				 17 + sizeof(srv->state.blocks));
	bt_mesh_model_msg_init(&buf, BT_MESH_BLOB_OP_XFER_STATUS);

	net_buf_simple_add_u8(&buf, ((status & BIT_MASK(4)) |
				     (srv->state.xfer.mode << 6)));
	net_buf_simple_add_u8(&buf, srv->phase);

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_INACTIVE) {
		goto send;
	}

	net_buf_simple_add_le64(&buf, srv->state.xfer.id);

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_START) {
		goto send;
	}

	net_buf_simple_add_le32(&buf, srv->state.xfer.size);
	net_buf_simple_add_u8(&buf, srv->state.block_size_log);
	net_buf_simple_add_le16(&buf, srv->state.mtu_size);
	net_buf_simple_add_mem(&buf, srv->state.blocks,
			       ceiling_fraction(block_count_get(srv), 8));

send:
	ctx->send_ttl = srv->state.ttl;
	bt_mesh_model_send(srv->mod, ctx, &buf, NULL, NULL);
}

static void block_status_rsp(struct bt_mesh_blob_srv *srv,
			     struct bt_mesh_msg_ctx *ctx,
			     enum bt_mesh_blob_status status)
{
	enum bt_mesh_blob_chunks_missing format;
	uint32_t missing;
	int i;

	BT_MESH_MODEL_BUF_DEFINE(buf, BT_MESH_BLOB_OP_BLOCK_STATUS,
				 5 + MAX(sizeof(srv->block.missing),
					 PULL_BLOB_REQ_COUNT * 3));
	bt_mesh_model_msg_init(&buf, BT_MESH_BLOB_OP_BLOCK_STATUS);

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_INACTIVE ||
	    srv->phase == BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_START ||
	    srv->phase == BT_MESH_BLOB_XFER_PHASE_SUSPENDED) {
		missing = srv->block.chunk_count;
	} else if (srv->phase == BT_MESH_BLOB_XFER_PHASE_COMPLETE) {
		missing = 0U;
	} else {
		missing = missing_chunks(&srv->block);
	}

	if (srv->state.xfer.mode == BT_MESH_BLOB_XFER_MODE_PULL) {
		format = BT_MESH_BLOB_CHUNKS_MISSING_ENCODED;
	} else if (missing == srv->block.chunk_count) {
		format = BT_MESH_BLOB_CHUNKS_MISSING_ALL;
	} else if (missing == 0) {
		format = BT_MESH_BLOB_CHUNKS_MISSING_NONE;
	} else {
		format = BT_MESH_BLOB_CHUNKS_MISSING_SOME;
	}

	BT_DBG("Missing: %u/%u", missing, srv->block.chunk_count);

	net_buf_simple_add_u8(&buf, (status & BIT_MASK(4)) | (format << 6));
	net_buf_simple_add_le16(&buf, srv->block.number);
	net_buf_simple_add_le16(&buf, srv->block.chunk_size);

	if (format == BT_MESH_BLOB_CHUNKS_MISSING_SOME) {
		net_buf_simple_add_mem(&buf, srv->block.missing,
				       ceiling_fraction(srv->block.chunk_count,
							8));

		BT_DBG("Bits: %s",
		       bt_hex(srv->block.missing,
			      ceiling_fraction(srv->block.chunk_count, 8)));

	} else if (format == BT_MESH_BLOB_CHUNKS_MISSING_ENCODED) {
		int count = pull_req_max(srv);

		for (i = 0; (i < srv->block.chunk_count) && count; ++i) {
			if (blob_chunk_missing_get(&srv->block, i)) {
				BT_DBG("Missing %u", i);
				buf_chunk_index_add(&buf, i);
				count--;
			}
		}
	}

	ctx->send_ttl = srv->state.ttl;
	bt_mesh_model_send(srv->mod, ctx, &buf, NULL, NULL);
}

static int handle_xfer_get(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			   struct net_buf_simple *buf)
{
	struct bt_mesh_blob_srv *srv = mod->user_data;

	BT_DBG("");
	xfer_status_rsp(srv, ctx, BT_MESH_BLOB_SUCCESS);

	return 0;
}

static int handle_xfer_start(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			     struct net_buf_simple *buf)
{
	struct bt_mesh_blob_srv *srv = mod->user_data;
	enum bt_mesh_blob_status status;
	struct bt_mesh_blob_xfer xfer;
	uint8_t block_size_log;
	uint32_t block_count;
	uint16_t mtu_size;
	int err;

	xfer.mode = (net_buf_simple_pull_u8(buf) >> 6);
	xfer.id = net_buf_simple_pull_le64(buf);
	xfer.size = net_buf_simple_pull_le32(buf);
	block_size_log = net_buf_simple_pull_u8(buf);
	mtu_size = net_buf_simple_pull_le16(buf);

	BT_DBG("\n\tsize: %u block size: %u\n\tmtu_size: %u\n\tmode: %s",
	       xfer.size, (1U << block_size_log), mtu_size,
	       xfer.mode == BT_MESH_BLOB_XFER_MODE_PUSH ? "push" : "pull");

	if (xfer.mode != BT_MESH_BLOB_XFER_MODE_PULL &&
	    xfer.mode != BT_MESH_BLOB_XFER_MODE_PUSH) {
		BT_WARN("Invalid mode 0x%x", xfer.mode);
		return -EINVAL;
	}

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_INACTIVE) {
		status = BT_MESH_BLOB_ERR_WRONG_PHASE;
		BT_WARN("Uninitialized");
		goto rsp;
	}

	if (srv->state.xfer.id != xfer.id) {
		status = BT_MESH_BLOB_ERR_WRONG_BLOB_ID;
		BT_WARN("Invalid ID (was %s, expected %s)",
			bt_hex(&xfer.id, sizeof(xfer.id)),
			bt_hex(&srv->state.xfer.id,
			       sizeof(srv->state.xfer.id)));
		goto rsp;
	}

	if (srv->phase != BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_START) {
		if (srv->state.xfer.mode != xfer.mode ||
		    srv->state.xfer.size != xfer.size ||
		    srv->state.block_size_log != block_size_log ||
		    srv->state.mtu_size > mtu_size) {
			status = BT_MESH_BLOB_ERR_WRONG_PHASE;
			BT_WARN("Busy");
			goto rsp;
		}

		if (srv->phase == BT_MESH_BLOB_XFER_PHASE_SUSPENDED) {
			resume(srv);
		}

		status = BT_MESH_BLOB_SUCCESS;
		BT_DBG("Duplicate");
		goto rsp;
	}

	if (xfer.size > CONFIG_BT_MESH_BLOB_SIZE_MAX) {
		BT_WARN("Too large");
		status = BT_MESH_BLOB_ERR_BLOB_TOO_LARGE;
		cancel(srv);
		goto rsp;
	}

	if (((1U << block_size_log) < CONFIG_BT_MESH_BLOB_BLOCK_SIZE_MIN) ||
	    ((1U << block_size_log) > CONFIG_BT_MESH_BLOB_BLOCK_SIZE_MAX)) {
		BT_WARN("Invalid block size");
		status = BT_MESH_BLOB_ERR_INVALID_BLOCK_SIZE;
		cancel(srv);
		goto rsp;
	}

	srv->state.cli = ctx->addr;
	srv->state.app_idx = ctx->app_idx;
	srv->state.block_size_log = block_size_log;
	srv->state.mtu_size = MIN(mtu_size, MTU_SIZE_MAX);
	srv->state.xfer = xfer;
	srv->block.number = 0xffff;
	srv->block.chunk_size = 0xffff;

	block_count = block_count_get(srv);
	if (block_count > BT_MESH_BLOB_BLOCKS_MAX) {
		BT_WARN("Invalid block count (%u)", block_count);
		status = BT_MESH_BLOB_ERR_INVALID_PARAM;
		cancel(srv);
		goto rsp;
	}

	memset(srv->state.blocks, 0, sizeof(srv->state.blocks));
	for (int i = 0; i < block_count; i++) {
		atomic_set_bit(srv->state.blocks, i);
	}

	err = io_open(srv);
	if (err) {
		BT_ERR("Couldn't open stream (err: %d)", err);
		status = BT_MESH_BLOB_ERR_INTERNAL;
		cancel(srv);
		goto rsp;
	}

	if (srv->cb && srv->cb->start) {
		err = srv->cb->start(srv, ctx, &srv->state.xfer);
		if (err) {
			BT_ERR("Couldn't start transfer (err: %d)", err);
			status = BT_MESH_BLOB_ERR_INTERNAL;
			cancel(srv);
			goto rsp;
		}
	}

	reset_timer(srv);
	phase_set(srv, BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_BLOCK);
	store_state(srv);
	status = BT_MESH_BLOB_SUCCESS;

	/* TODO: Set the LPN poll interval in a permanent manner.
	 * Currently, it'll move towards CONFIG_POLL_MAX exponentially, even
	 * if we set it manually.
	 */
rsp:
	xfer_status_rsp(srv, ctx, status);

	return 0;
}

static int handle_xfer_cancel(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			      struct net_buf_simple *buf)
{
	enum bt_mesh_blob_status status = BT_MESH_BLOB_SUCCESS;
	struct bt_mesh_blob_srv *srv = mod->user_data;
	uint64_t id;

	id = net_buf_simple_pull_le64(buf);

	BT_DBG("%u", (uint32_t)id);

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_INACTIVE) {
		goto rsp;
	}

	if (srv->state.xfer.id != id) {
		status = BT_MESH_BLOB_ERR_WRONG_BLOB_ID;
		goto rsp;
	}

	cancel(srv);

rsp:
	xfer_status_rsp(srv, ctx, status);

	return 0;
}

static int handle_block_get(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			    struct net_buf_simple *buf)
{
	enum bt_mesh_blob_status status;
	struct bt_mesh_blob_srv *srv = mod->user_data;

	switch (srv->phase) {
	case BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_BLOCK:
	case BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_CHUNK:
	case BT_MESH_BLOB_XFER_PHASE_COMPLETE:
		status = BT_MESH_BLOB_SUCCESS;
		break;
	case BT_MESH_BLOB_XFER_PHASE_SUSPENDED:
		status = BT_MESH_BLOB_ERR_INFO_UNAVAILABLE;
		break;
	case BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_START:
	case BT_MESH_BLOB_XFER_PHASE_INACTIVE:
		status = BT_MESH_BLOB_ERR_WRONG_PHASE;
		break;
	default:
		status = BT_MESH_BLOB_ERR_INTERNAL;
		break;
	}

	BT_DBG("");

	block_status_rsp(srv, ctx, status);

	return 0;
}

static int handle_block_start(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			      struct net_buf_simple *buf)
{
	struct bt_mesh_blob_srv *srv = mod->user_data;
	enum bt_mesh_blob_status status;
	uint16_t block_number, chunk_size;
	int err;

	block_number = net_buf_simple_pull_le16(buf);
	chunk_size = net_buf_simple_pull_le16(buf);

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_START ||
	    srv->phase == BT_MESH_BLOB_XFER_PHASE_INACTIVE) {
		status = BT_MESH_BLOB_ERR_WRONG_PHASE;
		goto rsp;
	}

	reset_timer(srv);

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_CHUNK) {
		if (block_number != srv->block.number ||
		    chunk_size != srv->block.chunk_size) {
			status = BT_MESH_BLOB_ERR_WRONG_PHASE;
		} else {
			status = BT_MESH_BLOB_SUCCESS;
		}

		goto rsp;
	}

	if (block_number >= block_count_get(srv)) {
		status = BT_MESH_BLOB_ERR_INVALID_BLOCK_NUM;
		goto rsp;
	}

	if (!chunk_size || chunk_size > max_chunk_size(srv) ||
	    (ceiling_fraction((1 << srv->state.block_size_log), chunk_size) >
	     max_chunk_count(srv))) {
		status = BT_MESH_BLOB_ERR_INVALID_CHUNK_SIZE;
		goto rsp;
	}

	srv->block.size = blob_block_size(
		srv->state.xfer.size, srv->state.block_size_log, block_number);
	srv->block.number = block_number;
	srv->block.chunk_count = ceiling_fraction(srv->block.size, chunk_size);
	srv->block.chunk_size = chunk_size;
	srv->block.offset = block_number * (1UL << srv->state.block_size_log);

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_COMPLETE ||
	    !atomic_test_bit(srv->state.blocks, block_number)) {
		memset(srv->block.missing, 0, sizeof(srv->block.missing));
		status = BT_MESH_BLOB_SUCCESS;
		goto rsp;
	}

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_SUSPENDED && srv->cb &&
	    srv->cb->resume) {
		srv->cb->resume(srv);
	}

	phase_set(srv, BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_CHUNK);
	blob_chunk_missing_set_all(&srv->block);

	BT_DBG("%u: (%u/%u)\n\tsize: %u\n\tchunk size: %u\n\tchunk count: %u",
	       srv->block.number, srv->block.number + 1, block_count_get(srv),
	       srv->block.size, chunk_size, srv->block.chunk_count);

	if (srv->io->block_start) {
		err = srv->io->block_start(srv->io, &srv->state.xfer,
					   &srv->block);
		if (err) {
			cancel(srv);
			status = BT_MESH_BLOB_ERR_INTERNAL;
			goto rsp;
		}
	}

	if (srv->state.xfer.mode == BT_MESH_BLOB_XFER_MODE_PULL) {
		/* Wait for the client to send the first chunk */
		srv->pull.counter = PULL_ATTEMPTS;
		k_delayed_work_submit(&srv->pull.report, REPORT_TIMER_TIMEOUT);
	}

	status = BT_MESH_BLOB_SUCCESS;

rsp:
	block_status_rsp(srv, ctx, status);

	return 0;
}

static int handle_chunk(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			struct net_buf_simple *buf)
{
	struct bt_mesh_blob_srv *srv = mod->user_data;
	struct bt_mesh_blob_chunk chunk;
	size_t expected_size = 0;
	uint16_t idx;
	int i, err;

	idx = net_buf_simple_pull_le16(buf);
	chunk.size = buf->len;
	chunk.data = net_buf_simple_pull_mem(buf, chunk.size);
	chunk.offset = idx * srv->block.chunk_size;

	if (srv->phase != BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_CHUNK ||
	    idx >= srv->block.chunk_count) {
		BT_ERR("Invalid phase or index (%u %u)", srv->phase,
		       idx);
		return -EINVAL;
	}

	if (idx == srv->block.chunk_count - 1) {
		expected_size = srv->block.size % srv->block.chunk_size;
	}

	if (expected_size == 0) {
		expected_size = srv->block.chunk_size;
	}

	if (chunk.size != expected_size) {
		BT_ERR("Unexpected size: expected %u", expected_size);
		return -EINVAL;
	}

	BT_DBG("%u/%u (%u bytes)", idx + 1, srv->block.chunk_count,
	       chunk.size);


	reset_timer(srv);
	if (srv->state.xfer.mode == BT_MESH_BLOB_XFER_MODE_PULL) {
		srv->pull.counter = PULL_ATTEMPTS;
		k_delayed_work_submit(&srv->pull.report, REPORT_TIMER_TIMEOUT);
	}

	if (!blob_chunk_missing_get(&srv->block, idx)) {
		BT_DBG("Duplicate chunk %u", idx);
		return -EALREADY;
	}

	err = srv->io->wr(srv->io, &srv->state.xfer, &srv->block, &chunk);
	if (err) {
		return err;
	}

	blob_chunk_missing_set(&srv->block, idx, false);
	if (missing_chunks(&srv->block)) {
		return 0;
	}

	if (srv->io->block_end) {
		srv->io->block_end(srv->io, &srv->state.xfer, &srv->block);
	}

	atomic_clear_bit(srv->state.blocks, srv->block.number);

	for (i = 0; i < ARRAY_SIZE(srv->state.blocks); ++i) {
		if (!srv->state.blocks[i]) {
			continue;
		}

		phase_set(srv, BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_BLOCK);
		store_state(srv);
		return 0;
	}

	phase_set(srv, BT_MESH_BLOB_XFER_PHASE_COMPLETE);
	k_delayed_work_cancel(&srv->rx_timeout);
	io_close(srv);
	erase_state(srv);

	if (srv->cb && srv->cb->end) {
		srv->cb->end(srv, srv->state.xfer.id, true);
	}

	return 0;
}

static int handle_info_get(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			   struct net_buf_simple *buf)
{
	struct bt_mesh_blob_srv *srv = mod->user_data;

	BT_DBG("");

	BT_MESH_MODEL_BUF_DEFINE(rsp, BT_MESH_BLOB_OP_INFO_STATUS, 15);
	bt_mesh_model_msg_init(&rsp, BT_MESH_BLOB_OP_INFO_STATUS);
	net_buf_simple_add_u8(&rsp, BLOB_BLOCK_SIZE_LOG_MIN);
	net_buf_simple_add_u8(&rsp, BLOB_BLOCK_SIZE_LOG_MAX);
	net_buf_simple_add_le16(&rsp, CONFIG_BT_MESH_BLOB_CHUNK_COUNT_MAX);
	net_buf_simple_add_le16(&rsp, CHUNK_SIZE_MAX);
	net_buf_simple_add_le32(&rsp, CONFIG_BT_MESH_BLOB_SIZE_MAX);
	net_buf_simple_add_le16(&rsp, MTU_SIZE_MAX);
	net_buf_simple_add_u8(&rsp, BT_MESH_BLOB_XFER_MODE_ALL);

	bt_mesh_model_send(srv->mod, ctx, &rsp, NULL, NULL);

	return 0;
}

const struct bt_mesh_model_op _bt_mesh_blob_srv_op[] = {
	{ BT_MESH_BLOB_OP_XFER_GET, BT_MESH_LEN_EXACT(0), handle_xfer_get },
	{ BT_MESH_BLOB_OP_XFER_START, BT_MESH_LEN_EXACT(12), handle_xfer_start },
	{ BT_MESH_BLOB_OP_XFER_CANCEL, BT_MESH_LEN_EXACT(8), handle_xfer_cancel },
	{ BT_MESH_BLOB_OP_BLOCK_GET, BT_MESH_LEN_EXACT(0), handle_block_get },
	{ BT_MESH_BLOB_OP_BLOCK_START, BT_MESH_LEN_EXACT(4), handle_block_start },
	{ BT_MESH_BLOB_OP_CHUNK, BT_MESH_LEN_MIN(2), handle_chunk },
	{ BT_MESH_BLOB_OP_INFO_GET, BT_MESH_LEN_EXACT(0), handle_info_get },
	BT_MESH_MODEL_OP_END,
};

static int blob_srv_init(struct bt_mesh_model *mod)
{
	struct bt_mesh_blob_srv *srv = mod->user_data;

	srv->mod = mod;
	srv->state.ttl = BT_MESH_TTL_DEFAULT;
	srv->block.number = 0xffff;
	srv->block.chunk_size = 0xffff;
	k_delayed_work_init(&srv->rx_timeout, timeout);
	k_delayed_work_init(&srv->pull.report, report_timeout);

	return 0;
}

static int blob_srv_settings_set(struct bt_mesh_model *mod, const char *name,
				 size_t len_rd, settings_read_cb read_cb,
				 void *cb_arg)
{
	struct bt_mesh_blob_srv *srv = mod->user_data;
	ssize_t len;

	if (len_rd < offsetof(struct bt_mesh_blob_srv_state, blocks)) {
		return -EINVAL;
	}

	len = read_cb(cb_arg, &srv->state, sizeof(srv->state));
	if (len < 0) {
		return len;
	}

	srv->block.number = 0xffff;
	srv->block.chunk_size = 0xffff;

	if (block_count_get(srv) > BT_MESH_BLOB_BLOCKS_MAX) {
		BT_WARN("Loaded block count too high (%u, max: %u)",
			block_count_get(srv), BT_MESH_BLOB_BLOCKS_MAX);
		return 0;
	}

	phase_set(srv, BT_MESH_BLOB_XFER_PHASE_SUSPENDED);

	BT_DBG("Recovered transfer from 0x%04x (%llu)", srv->state.cli,
	       srv->state.xfer.id);

	return 0;
}

static int blob_srv_start(struct bt_mesh_model *mod)
{
	struct bt_mesh_blob_srv *srv = mod->user_data;
	int err = -ENOTSUP;

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_INACTIVE) {
		return 0;
	}

	if (srv->cb && srv->cb->recover) {
		srv->io = NULL;
		err = srv->cb->recover(srv, &srv->state.xfer, &srv->io);
		if (!err && srv->io) {
			err = io_open(srv);
		}
	}

	if (err || !srv->io) {
		BT_WARN("Abandoning transfer.");
		phase_set(srv, BT_MESH_BLOB_XFER_PHASE_INACTIVE);
		srv->state.xfer.mode = BT_MESH_BLOB_XFER_MODE_NONE;
		srv->state.ttl = BT_MESH_TTL_DEFAULT;
		erase_state(srv);
	}

	return 0;
}

const struct bt_mesh_model_cb _bt_mesh_blob_srv_cb = {
	.init = blob_srv_init,
	.settings_set = blob_srv_settings_set,
	.start = blob_srv_start,
};

int bt_mesh_blob_srv_recv(struct bt_mesh_blob_srv *srv, uint64_t id,
			  const struct bt_mesh_blob_io *io, uint8_t ttl,
			  uint16_t timeout_base)
{
	if (bt_mesh_blob_srv_is_busy(srv)) {
		return -EBUSY;
	}

	if (!io || !io->wr) {
		return -EINVAL;
	}

	srv->state.xfer.id = id;
	srv->state.ttl = ttl;
	srv->state.timeout_base = timeout_base;
	srv->io = io;
	srv->block.number = 0xffff;
	srv->block.chunk_size = 0xffff;
	phase_set(srv, BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_START);
	store_state(srv);

	return 0;
}

int bt_mesh_blob_srv_cancel(struct bt_mesh_blob_srv *srv)
{
	if (!bt_mesh_blob_srv_is_busy(srv)) {
		return -EALREADY;
	}

	cancel(srv);

	return 0;
}

bool bt_mesh_blob_srv_is_busy(const struct bt_mesh_blob_srv *srv)
{
	return srv->phase != BT_MESH_BLOB_XFER_PHASE_INACTIVE &&
	       srv->phase != BT_MESH_BLOB_XFER_PHASE_COMPLETE;
}

uint8_t bt_mesh_blob_srv_progress(const struct bt_mesh_blob_srv *srv)
{
	uint32_t total;
	uint32_t received;

	if (srv->phase == BT_MESH_BLOB_XFER_PHASE_INACTIVE ||
	    srv->phase == BT_MESH_BLOB_XFER_PHASE_WAITING_FOR_START) {
		return 0;
	}

	total = block_count_get(srv);

	received = 0;
	for (int i = 0; i < total; ++i) {
		if (!atomic_test_bit(srv->state.blocks, i)) {
			received++;
		}
	}

	return (100U * received) / total;
}
