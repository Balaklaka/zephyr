/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <bluetooth/mesh.h>
#include "dfu_slot.h"
#include "dfd.h"
#include "dfu.h"
#include "net.h"
#include "transport.h"

#define BT_DBG_ENABLED IS_ENABLED(CONFIG_BT_MESH_DEBUG_DFU)
#define LOG_MODULE_NAME bt_mesh_dfd_srv
#include "common/log.h"

#define ERASE_BLOCK_SIZE DT_PROP(DT_CHOSEN(zephyr_flash), erase_block_size)

struct slot_search_ctx {
	off_t offset;
	size_t size;
	bool failed;
};

static struct bt_mesh_dfu_target *target_get(struct bt_mesh_dfd_srv *srv,
					     uint16_t addr)
{
	for (int i = 0; i < srv->target_cnt; ++i) {
		if (addr == srv->targets[i].blob.addr) {
			return &srv->targets[i];
		}
	}

	return NULL;
}

static bool is_busy(const struct bt_mesh_dfd_srv *srv)
{
	return srv->phase == BT_MESH_DFD_PHASE_TRANSFER_ACTIVE ||
	       srv->phase == BT_MESH_DFD_PHASE_TRANSFER_SUCCESS ||
	       srv->phase == BT_MESH_DFD_PHASE_APPLYING_UPDATE;
}

static bool upload_is_busy(const struct bt_mesh_dfd_srv *srv)
{
	return bt_mesh_blob_srv_is_busy(&srv->upload.blob) ||
	       srv->upload.phase == BT_MESH_DFD_UPLOAD_PHASE_TRANSFER_ACTIVE;
}

static int slot_del(struct bt_mesh_dfd_srv *srv, const struct bt_mesh_dfu_slot *slot)
{
	if (srv->cb && srv->cb->del) {
		srv->cb->del(srv, slot);
	}

	return bt_mesh_dfu_slot_del(slot);
}

static void receivers_status_rsp(struct bt_mesh_dfd_srv *srv,
				 struct bt_mesh_msg_ctx *ctx,
				 enum bt_mesh_dfd_status status)
{
	BT_MESH_MODEL_BUF_DEFINE(buf, BT_MESH_DFD_OP_RECEIVERS_STATUS, 3);
	bt_mesh_model_msg_init(&buf, BT_MESH_DFD_OP_RECEIVERS_STATUS);

	net_buf_simple_add_u8(&buf, status);
	net_buf_simple_add_le16(&buf, srv->target_cnt);

	bt_mesh_model_send(srv->mod, ctx, &buf, NULL, NULL);
}

static int handle_receivers_add(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
				struct net_buf_simple *buf)
{
	enum bt_mesh_dfd_status status = BT_MESH_DFD_SUCCESS;
	struct bt_mesh_dfd_srv *srv = mod->user_data;

	if (buf->len % 3) {
		return -EINVAL;
	}

	if (bt_mesh_dfu_cli_is_busy(&srv->dfu)) {
		receivers_status_rsp(srv, ctx,
				     BT_MESH_DFD_ERR_BUSY_WITH_DISTRIBUTION);
		return 0;
	}

	while (buf->len >= 3) {
		struct bt_mesh_dfu_target *t;
		uint8_t img_idx;
		uint16_t addr;

		addr = net_buf_simple_pull_le16(buf);
		img_idx = net_buf_simple_pull_u8(buf);
		if (!BT_MESH_ADDR_IS_UNICAST(addr)) {
			continue;
		}

		t = target_get(srv, addr);
		if (t) {
			t->img_idx = img_idx;
			continue;
		}

		/* New target node, add it to the list */

		if (srv->target_cnt == ARRAY_SIZE(srv->targets)) {
			status = BT_MESH_DFD_ERR_INSUFFICIENT_RESOURCES;
			break;
		}

		t = &srv->targets[srv->target_cnt++];
		memset(t, 0, sizeof(*t));
		t->blob.addr = addr;
		t->img_idx = img_idx;

		BT_DBG("Added receiver 0x%04x img: %u", t->blob.addr,
		       t->img_idx);
	}

	receivers_status_rsp(srv, ctx, status);

	return 0;
}

static int handle_receivers_delete_all(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
				       struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;

	if (bt_mesh_dfu_cli_is_busy(&srv->dfu)) {
		receivers_status_rsp(srv, ctx,
				     BT_MESH_DFD_ERR_BUSY_WITH_DISTRIBUTION);
		return 0;
	}

	sys_slist_init(&srv->ctx.targets);
	srv->target_cnt = 0;

	receivers_status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);

	return 0;
}

static int handle_receivers_get(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
				struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;
	uint16_t first, cnt;
	uint8_t progress;
	int i;

	first = net_buf_simple_pull_le16(buf);
	cnt = net_buf_simple_pull_le16(buf);
	if (cnt == 0) {
		return -EINVAL;
	}

	/* Create a buffer that can fit the full target list, maxing out at TX_SDU_MAX: */
	NET_BUF_SIMPLE_DEFINE(
		rsp, MIN(BT_MESH_TX_SDU_MAX,
			 BT_MESH_MODEL_BUF_LEN(
				 BT_MESH_DFD_OP_RECEIVERS_LIST,
				 4 + CONFIG_BT_MESH_DFD_SRV_TARGETS_MAX * 5)));
	bt_mesh_model_msg_init(&rsp, BT_MESH_DFD_OP_RECEIVERS_LIST);

	net_buf_simple_add_le16(&rsp, srv->target_cnt);
	net_buf_simple_add_le16(&rsp, first);

	cnt = MIN(cnt, srv->target_cnt - first);
	progress = bt_mesh_dfu_cli_progress(&srv->dfu) / 2;

	for (i = 0; i < cnt && net_buf_simple_tailroom(&rsp) >= 5 + BT_MESH_MIC_SHORT; i++) {
		const struct bt_mesh_dfu_target *t = &srv->targets[i + first];

		net_buf_simple_add_le32(
			&rsp, ((t->blob.addr & BIT_MASK(15)) |
			       ((t->phase & BIT_MASK(4)) << 15U) |
			       ((t->status & BIT_MASK(3)) << 19U) |
			       ((t->blob.status & BIT_MASK(4)) << 22U) |
			       ((progress & BIT_MASK(6)) << 26U)));
		net_buf_simple_add_u8(&rsp, t->img_idx);
	}

	bt_mesh_model_send(srv->mod, ctx, &rsp, NULL, NULL);

	return 0;
}

static enum bt_mesh_dfu_iter slot_space_cb(const struct bt_mesh_dfu_slot *slot,
					   void *user_data)
{
	size_t *total = user_data;

	*total += slot->size;

	return BT_MESH_DFU_ITER_CONTINUE;
}

static int handle_capabilities_get(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
				   struct net_buf_simple *buf)
{
	size_t size = 0;

	BT_MESH_MODEL_BUF_DEFINE(rsp, BT_MESH_DFD_OP_CAPABILITIES_STATUS, 17);
	bt_mesh_model_msg_init(&rsp, BT_MESH_DFD_OP_CAPABILITIES_STATUS);

	net_buf_simple_add_le16(&rsp, CONFIG_BT_MESH_DFD_SRV_TARGETS_MAX);
	net_buf_simple_add_le16(&rsp, CONFIG_BT_MESH_DFU_SLOT_CNT);
	net_buf_simple_add_le32(&rsp, CONFIG_BT_MESH_DFD_SRV_SLOT_MAX_SIZE);
	net_buf_simple_add_le32(&rsp, CONFIG_BT_MESH_DFD_SRV_SLOT_SPACE);

	/* Remaining size */
	(void)bt_mesh_dfu_slot_foreach(slot_space_cb, &size);
	size = MIN(size, CONFIG_BT_MESH_DFD_SRV_SLOT_SPACE);

	net_buf_simple_add_le32(&rsp, CONFIG_BT_MESH_DFD_SRV_SLOT_SPACE - size);
	net_buf_simple_add_u8(&rsp, 0U); /* OOB retrieval not supported */

	bt_mesh_model_send(mod, ctx, &rsp, NULL, NULL);

	return 0;
}

static void status_rsp(struct bt_mesh_dfd_srv *srv, struct bt_mesh_msg_ctx *ctx,
		       enum bt_mesh_dfd_status status)
{
	BT_MESH_MODEL_BUF_DEFINE(rsp, BT_MESH_DFD_OP_STATUS, 12);
	bt_mesh_model_msg_init(&rsp, BT_MESH_DFD_OP_STATUS);

	net_buf_simple_add_u8(&rsp, status);
	net_buf_simple_add_u8(&rsp, srv->phase);

	if (srv->phase == BT_MESH_DFD_PHASE_IDLE || !srv->dfu.xfer.slot) {
		bt_mesh_model_send(srv->mod, ctx, &rsp, NULL, NULL);
		return;
	}

	net_buf_simple_add_le16(&rsp, srv->ctx.group);
	net_buf_simple_add_le16(&rsp, srv->ctx.app_idx);
	net_buf_simple_add_u8(&rsp, srv->ctx.ttl);
	net_buf_simple_add_le16(&rsp, srv->ctx.timeout_base);
	net_buf_simple_add_u8(&rsp, ((srv->dfu.xfer.blob.mode & BIT_MASK(2)) |
				     ((srv->apply & BIT_MASK(1)) << 2)));
	net_buf_simple_add_le16(&rsp, srv->slot_idx);

	bt_mesh_model_send(srv->mod, ctx, &rsp, NULL, NULL);
}

static int handle_get(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
		      struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;

	status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);

	return 0;
}

static int handle_start(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;
	uint16_t app_idx, timeout_base, slot_idx, group;
	const struct bt_mesh_dfu_slot *slot;
	enum bt_mesh_blob_xfer_mode mode;
	uint8_t byte, ttl;
	bool apply;
	int err, i;

	app_idx = net_buf_simple_pull_le16(buf);
	ttl = net_buf_simple_pull_u8(buf);
	timeout_base = net_buf_simple_pull_le16(buf);
	byte = net_buf_simple_pull_u8(buf);
	mode = byte & BIT_MASK(2);
	apply = (byte >> 2U) & BIT_MASK(1);
	slot_idx = net_buf_simple_pull_le16(buf);

	if (buf->len == 16) {
		/* TODO: Virtual addresses not supported. */
		status_rsp(srv, ctx, BT_MESH_DFD_ERR_INTERNAL);
		return 0;
	}

	if (buf->len != 2) {
		return -EINVAL;
	}

	group = net_buf_simple_pull_le16(buf);

	if (!srv->target_cnt) {
		status_rsp(srv, ctx, BT_MESH_DFD_ERR_RECEIVERS_LIST_EMPTY);
		return 0;
	}

	if (!bt_mesh_app_key_exists(app_idx)) {
		status_rsp(srv, ctx, BT_MESH_DFD_ERR_INVALID_APPKEY_INDEX);
		return 0;
	}

	slot = bt_mesh_dfu_slot_at(slot_idx);
	if (!slot || !bt_mesh_dfu_slot_is_valid(slot)) {
		status_rsp(srv, ctx, BT_MESH_DFD_ERR_FW_NOT_FOUND);
		return 0;
	}

	if (is_busy(srv)) {
		if (srv->ctx.app_idx != app_idx ||
		    srv->ctx.timeout_base != timeout_base ||
		    srv->ctx.group != group || srv->ctx.ttl != ttl ||
		    srv->dfu.xfer.blob.mode != mode || srv->apply != apply ||
		    srv->slot_idx != slot_idx) {
			BT_WARN("Busy distributing another image");
			status_rsp(srv, ctx,
				   BT_MESH_DFD_ERR_BUSY_WITH_DISTRIBUTION);
		} else {
			status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);
		}

		return 0;
	}

	if (srv->phase == BT_MESH_DFD_PHASE_CANCELING_UPDATE) {
		status_rsp(srv, ctx, BT_MESH_DFD_ERR_BUSY_WITH_DISTRIBUTION);
		return 0;
	}

	srv->io = NULL;
	err = srv->cb->send(srv, slot, &srv->io);
	if (err || !srv->io) {
		status_rsp(srv, ctx, BT_MESH_DFD_ERR_INTERNAL);
		return 0;
	}

	sys_slist_init(&srv->ctx.targets);
	for (i = 0; i < srv->target_cnt; i++) {
		sys_slist_append(&srv->ctx.targets, &srv->targets[i].blob.n);
	}

	srv->slot_idx = slot_idx;
	srv->ctx.app_idx = app_idx;
	srv->ctx.timeout_base = timeout_base;
	srv->ctx.group = group;
	srv->ctx.ttl = ttl;
	srv->apply = apply;

	err = bt_mesh_dfu_cli_send(&srv->dfu, slot, &srv->ctx, NULL, srv->io,
				   mode);
	if (err) {
		status_rsp(srv, ctx, BT_MESH_DFD_ERR_INTERNAL);
		return 0;
	}

	srv->phase = BT_MESH_DFD_PHASE_TRANSFER_ACTIVE;

	return 0;
	status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);

	return 0;
}

static int handle_cancel(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			 struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;
	int err;

	if (srv->phase == BT_MESH_DFD_PHASE_CANCELING_UPDATE ||
	    srv->phase == BT_MESH_DFD_PHASE_IDLE) {
		status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);
		return 0;
	}

	if (srv->phase == BT_MESH_DFD_PHASE_COMPLETED ||
	    srv->phase == BT_MESH_DFD_PHASE_FAILED) {
		srv->phase = BT_MESH_DFD_PHASE_IDLE;
		status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);
		return 0;
	}

	/* Phase TRANSFER_ACTIVE, TRANSFER_SUCCESS, APPLYING_UPDATE: */

	srv->phase = BT_MESH_DFD_PHASE_CANCELING_UPDATE;
	err = bt_mesh_dfu_cli_cancel(&srv->dfu, NULL);
	if (err) {
		status_rsp(srv, ctx, BT_MESH_DFD_ERR_INTERNAL);
		return 0;
	}

	status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);

	return 0;
}

static int handle_apply(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;
	int err;

	if (srv->phase == BT_MESH_DFD_PHASE_IDLE ||
	    srv->phase == BT_MESH_DFD_PHASE_CANCELING_UPDATE ||
	    srv->phase == BT_MESH_DFD_PHASE_TRANSFER_ACTIVE ||
	    srv->phase == BT_MESH_DFD_PHASE_FAILED) {
		status_rsp(srv, ctx, BT_MESH_DFD_ERR_WRONG_PHASE);
		return 0;
	}

	if (srv->phase == BT_MESH_DFD_PHASE_APPLYING_UPDATE ||
	    srv->phase == BT_MESH_DFD_PHASE_COMPLETED) {
		status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);
		return 0;
	}

	err = bt_mesh_dfu_cli_apply(&srv->dfu);
	if (err) {
		status_rsp(srv, ctx, BT_MESH_DFD_ERR_INTERNAL);
		return 0;
	}

	status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);

	return 0;
}

static void upload_status_rsp(struct bt_mesh_dfd_srv *srv,
			      struct bt_mesh_msg_ctx *ctx,
			      enum bt_mesh_dfd_status status)
{
	BT_MESH_MODEL_BUF_DEFINE(rsp, BT_MESH_DFD_OP_UPLOAD_STATUS,
				 5 + CONFIG_BT_MESH_DFU_FWID_MAXLEN);
	bt_mesh_model_msg_init(&rsp, BT_MESH_DFD_OP_UPLOAD_STATUS);

	net_buf_simple_add_u8(&rsp, status);
	net_buf_simple_add_u8(&rsp, srv->upload.phase);

	if (srv->upload.phase == BT_MESH_DFD_UPLOAD_PHASE_IDLE ||
	    !srv->upload.slot) {
		bt_mesh_model_send(srv->mod, ctx, &rsp, NULL, NULL);
		return;
	}

	net_buf_simple_add_u8(&rsp,
			      bt_mesh_blob_srv_progress(&srv->upload.blob));
	net_buf_simple_add_mem(&rsp, srv->upload.slot->fwid,
			       srv->upload.slot->fwid_len);

	bt_mesh_model_send(srv->mod, ctx, &rsp, NULL, NULL);
}

static int handle_upload_get(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			     struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;

	upload_status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);

	return 0;
}

static int handle_upload_start(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			       struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;
	const struct bt_mesh_dfu_slot *old_slot = srv->upload.slot;
	size_t meta_len, fwid_len, size;
	const uint8_t *meta, *fwid;
	uint16_t timeout_base;
	uint64_t blob_id;
	int err, idx;
	uint8_t ttl;

	ttl = net_buf_simple_pull_u8(buf);
	timeout_base = net_buf_simple_pull_le16(buf);
	blob_id = net_buf_simple_pull_le64(buf);
	size = net_buf_simple_pull_le32(buf);
	meta_len = net_buf_simple_pull_u8(buf);
	if (buf->len < meta_len) {
		return -EINVAL;
	}

	meta = net_buf_simple_pull_mem(buf, meta_len);
	fwid_len = buf->len;
	fwid = net_buf_simple_pull_mem(buf, fwid_len);

	if (size > CONFIG_BT_MESH_DFD_SRV_SLOT_MAX_SIZE ||
	    fwid_len > CONFIG_BT_MESH_DFU_FWID_MAXLEN ||
	    meta_len > CONFIG_BT_MESH_DFU_METADATA_MAXLEN) {
		upload_status_rsp(srv, ctx,
				  BT_MESH_DFD_ERR_INSUFFICIENT_RESOURCES);
		return 0;
	}

	if (upload_is_busy(srv)) {
		if (!srv->upload.slot) {
			BT_WARN("Busy with no upload slot");
			upload_status_rsp(srv, ctx, BT_MESH_DFD_ERR_INTERNAL);
			return 0;
		}

		if (srv->upload.slot->fwid_len == fwid_len &&
		    !memcmp(srv->upload.slot->fwid, fwid, fwid_len) &&
		    srv->upload.slot->metadata_len == meta_len &&
		    !memcmp(srv->upload.slot->metadata, meta, meta_len) &&
		    srv->upload.blob.state.xfer.id == blob_id &&
		    srv->upload.blob.state.ttl == ttl &&
		    srv->upload.blob.state.timeout_base == timeout_base) {
			BT_DBG("Duplicate upload start");
			upload_status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);
			return 0;
		}

		BT_WARN("Upload already in progress");
		upload_status_rsp(srv, ctx, BT_MESH_DFD_ERR_BUSY_WITH_UPLOAD);
		return 0;
	}

	idx = bt_mesh_dfu_slot_get(fwid, fwid_len, &srv->upload.slot);
	if (idx >= 0 && bt_mesh_dfu_slot_is_valid(srv->upload.slot)) {
		BT_DBG("Already received image");
		srv->upload.phase = BT_MESH_DFD_UPLOAD_PHASE_TRANSFER_SUCCESS;
		upload_status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);
		return 0;
	}

	if (old_slot && !bt_mesh_dfu_slot_is_valid(old_slot)) {
		BT_DBG("Deleting old invalid slot");
		slot_del(srv, old_slot);
	}

	/* TODO Store transfer state before slot is added. */

	srv->upload.slot = bt_mesh_dfu_slot_add(size, fwid, fwid_len, meta,
						meta_len, NULL, 0);
	if (!srv->upload.slot) {
		BT_WARN("No space for slot");
		upload_status_rsp(srv, ctx,
				  BT_MESH_DFD_ERR_INSUFFICIENT_RESOURCES);
		return 0;
	}

	srv->io = NULL;
	err = srv->cb->recv(srv, srv->upload.slot, &srv->io);
	if (err || !srv->io) {
		BT_ERR("App rejected upload. err: %d io: %p", err, srv->io);
		slot_del(srv, srv->upload.slot);
		upload_status_rsp(srv, ctx, BT_MESH_DFD_ERR_INTERNAL);
		return 0;
	}

	err = bt_mesh_blob_srv_recv(&srv->upload.blob, blob_id, srv->io, ttl,
				    timeout_base);
	if (err) {
		BT_ERR("BLOB Server rejected upload (err: %d)", err);
		slot_del(srv, srv->upload.slot);
		upload_status_rsp(srv, ctx, BT_MESH_DFD_ERR_INTERNAL);
		return 0;
	}

	BT_DBG("%s", bt_hex(fwid, fwid_len));

	srv->upload.phase = BT_MESH_DFD_UPLOAD_PHASE_TRANSFER_ACTIVE;
	upload_status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);

	return 0;
}

static int handle_upload_start_oob(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
				   struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;

	BT_DBG("");

	upload_status_rsp(srv, ctx, BT_MESH_DFD_ERR_URI_NOT_SUPPORTED);

	return 0;
}

static int handle_upload_cancel(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
				struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;

	srv->upload.phase = BT_MESH_DFD_UPLOAD_PHASE_IDLE;
	(void)bt_mesh_blob_srv_cancel(&srv->upload.blob);
	upload_status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS);

	return 0;
}

static void fw_status_rsp(struct bt_mesh_dfd_srv *srv,
			  struct bt_mesh_msg_ctx *ctx,
			  enum bt_mesh_dfd_status status, uint16_t idx,
			  const uint8_t *fwid, size_t fwid_len)
{
	BT_MESH_MODEL_BUF_DEFINE(rsp, BT_MESH_DFD_OP_FW_STATUS,
				 7 + CONFIG_BT_MESH_DFU_FWID_MAXLEN);
	bt_mesh_model_msg_init(&rsp, BT_MESH_DFD_OP_FW_STATUS);

	net_buf_simple_add_u8(&rsp, status);
	net_buf_simple_add_le16(&rsp, bt_mesh_dfu_slot_foreach(NULL, NULL));

	net_buf_simple_add_le16(&rsp, idx);
	if (fwid) {
		net_buf_simple_add_mem(&rsp, fwid, fwid_len);
	}

	bt_mesh_model_send(srv->mod, ctx, &rsp, NULL, NULL);
}

static int handle_fw_get(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			 struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;
	const struct bt_mesh_dfu_slot *slot;
	const uint8_t *fwid;
	size_t fwid_len;
	int idx;

	fwid_len = buf->len;
	fwid = net_buf_simple_pull_mem(buf, fwid_len);

	idx = bt_mesh_dfu_slot_get(fwid, fwid_len, &slot);
	if (idx < 0) {
		fw_status_rsp(srv, ctx, BT_MESH_DFD_ERR_FW_NOT_FOUND, 0xffff,
			      fwid, fwid_len);
	} else {
		fw_status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS, idx, fwid,
			      fwid_len);
	}

	return 0;
}

static int handle_fw_get_by_index(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
				  struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;
	const struct bt_mesh_dfu_slot *slot;
	uint16_t idx;

	idx = net_buf_simple_pull_le16(buf);

	slot = bt_mesh_dfu_slot_at(idx);
	if (slot) {
		fw_status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS, idx, slot->fwid,
			      slot->fwid_len);
	} else {
		fw_status_rsp(srv, ctx, BT_MESH_DFD_ERR_FW_NOT_FOUND, idx,
			      NULL, 0);
	}

	return 0;
}

static int handle_fw_delete(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
			    struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;
	const struct bt_mesh_dfu_slot *slot;
	const uint8_t *fwid;
	size_t fwid_len;
	int idx, err;

	fwid_len = buf->len;
	fwid = net_buf_simple_pull_mem(buf, fwid_len);

	if (is_busy(srv) || srv->phase == BT_MESH_DFD_PHASE_CANCELING_UPDATE) {
		fw_status_rsp(srv, ctx, BT_MESH_DFD_ERR_BUSY_WITH_DISTRIBUTION,
			      0xffff, NULL, 0);
		return 0;
	}

	idx = bt_mesh_dfu_slot_get(fwid, fwid_len, &slot);
	if (idx < 0) {
		fw_status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS, 0xffff, fwid,
			      fwid_len);
		return 0;
	}

	err = slot_del(srv, slot);
	if (err) {
		fw_status_rsp(srv, ctx, BT_MESH_DFD_ERR_INTERNAL, 0xffff, NULL, 0);
	} else {
		fw_status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS, 0xffff, fwid, fwid_len);
	}

	return 0;
}

static enum bt_mesh_dfu_iter slot_del_cb(const struct bt_mesh_dfu_slot *slot,
					 void *user_data)
{
	struct bt_mesh_dfd_srv *srv = user_data;
	if (srv->cb && srv->cb->del) {
		srv->cb->del(srv, slot);
	}

	return BT_MESH_DFU_ITER_CONTINUE;
}

static int handle_fw_delete_all(struct bt_mesh_model *mod, struct bt_mesh_msg_ctx *ctx,
				struct net_buf_simple *buf)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;

	if (is_busy(srv) || srv->phase == BT_MESH_DFD_PHASE_CANCELING_UPDATE) {
		fw_status_rsp(srv, ctx, BT_MESH_DFD_ERR_BUSY_WITH_DISTRIBUTION,
			      0xffff, NULL, 0);
		return 0;
	}

	bt_mesh_dfu_slot_foreach(slot_del_cb, srv);

	bt_mesh_dfu_slot_del_all();

	fw_status_rsp(srv, ctx, BT_MESH_DFD_SUCCESS, 0xffff, NULL, 0);

	return 0;
}

const struct bt_mesh_model_op _bt_mesh_dfd_srv_op[] = {
	{ BT_MESH_DFD_OP_RECEIVERS_ADD, BT_MESH_LEN_MIN(3), handle_receivers_add },
	{ BT_MESH_DFD_OP_RECEIVERS_DELETE_ALL, BT_MESH_LEN_EXACT(0), handle_receivers_delete_all },
	{ BT_MESH_DFD_OP_RECEIVERS_GET, BT_MESH_LEN_EXACT(4), handle_receivers_get },
	{ BT_MESH_DFD_OP_CAPABILITIES_GET, BT_MESH_LEN_EXACT(0), handle_capabilities_get },
	{ BT_MESH_DFD_OP_GET, BT_MESH_LEN_EXACT(0), handle_get },
	{ BT_MESH_DFD_OP_START, BT_MESH_LEN_MIN(10), handle_start },
	{ BT_MESH_DFD_OP_CANCEL, BT_MESH_LEN_EXACT(0), handle_cancel },
	{ BT_MESH_DFD_OP_APPLY, BT_MESH_LEN_EXACT(0), handle_apply },
	{ BT_MESH_DFD_OP_UPLOAD_GET, BT_MESH_LEN_EXACT(0), handle_upload_get },
	{ BT_MESH_DFD_OP_UPLOAD_START, BT_MESH_LEN_MIN(16), handle_upload_start },
	{ BT_MESH_DFD_OP_UPLOAD_START_OOB, BT_MESH_LEN_EXACT(2), handle_upload_start_oob },
	{ BT_MESH_DFD_OP_UPLOAD_CANCEL, BT_MESH_LEN_EXACT(0), handle_upload_cancel },
	{ BT_MESH_DFD_OP_FW_GET, BT_MESH_LEN_MIN(0), handle_fw_get },
	{ BT_MESH_DFD_OP_FW_GET_BY_INDEX, BT_MESH_LEN_EXACT(2), handle_fw_get_by_index },
	{ BT_MESH_DFD_OP_FW_DELETE, BT_MESH_LEN_MIN(0), handle_fw_delete },
	{ BT_MESH_DFD_OP_FW_DELETE_ALL, BT_MESH_LEN_EXACT(0), handle_fw_delete_all },

	BT_MESH_MODEL_OP_END
};

static void dfu_ended(struct bt_mesh_dfu_cli *cli,
		      enum bt_mesh_dfu_status reason)
{
	struct bt_mesh_dfd_srv *srv =
		CONTAINER_OF(cli, struct bt_mesh_dfd_srv, dfu);
	int err;

	BT_DBG("%u", reason);

	if (reason != BT_MESH_DFU_SUCCESS) {
		srv->phase = BT_MESH_DFD_PHASE_FAILED;
		return;
	}

	if (!srv->apply) {
		srv->phase = BT_MESH_DFD_PHASE_COMPLETED;
		return;
	}

	srv->phase = BT_MESH_DFD_PHASE_APPLYING_UPDATE;

	err = bt_mesh_dfu_cli_apply(cli);
	if (err) {
		BT_ERR("Apply failed: %d", err);
		srv->phase = BT_MESH_DFD_PHASE_FAILED;
	}
}

static void dfu_applied(struct bt_mesh_dfu_cli *cli)
{
	struct bt_mesh_dfd_srv *srv =
		CONTAINER_OF(cli, struct bt_mesh_dfd_srv, dfu);
	int err;

	if (srv->phase == BT_MESH_DFD_PHASE_CANCELING_UPDATE) {
		srv->phase = BT_MESH_DFD_PHASE_FAILED;
		return;
	}

	if (srv->phase != BT_MESH_DFD_PHASE_APPLYING_UPDATE) {
		return;
	}

	err = bt_mesh_dfu_cli_confirm(cli);
	if (err) {
		BT_ERR("Confirm failed: %d", err);
		srv->phase = BT_MESH_DFD_PHASE_FAILED;
	}
}

static void dfu_confirmed(struct bt_mesh_dfu_cli *cli)
{
	struct bt_mesh_dfd_srv *srv =
		CONTAINER_OF(cli, struct bt_mesh_dfd_srv, dfu);

	if (srv->phase != BT_MESH_DFD_PHASE_APPLYING_UPDATE &&
	    srv->phase != BT_MESH_DFD_PHASE_CANCELING_UPDATE) {
		return;
	}

	srv->phase = BT_MESH_DFD_PHASE_COMPLETED;
}

const struct bt_mesh_dfu_cli_cb _bt_mesh_dfd_srv_dfu_cb = {
	.ended = dfu_ended,
	.applied = dfu_applied,
	.confirmed = dfu_confirmed,
};

static int upload_start(struct bt_mesh_blob_srv *b, struct bt_mesh_msg_ctx *ctx,
			struct bt_mesh_blob_xfer *xfer)
{
	BT_DBG("");
	return 0;
}

static void upload_end(struct bt_mesh_blob_srv *b, uint64_t id, bool success)
{
	struct bt_mesh_dfd_srv *srv =
		CONTAINER_OF(b, struct bt_mesh_dfd_srv, upload.blob);

	if (srv->upload.phase != BT_MESH_DFD_UPLOAD_PHASE_TRANSFER_ACTIVE) {
		return;
	}

	BT_DBG("%u", success);

	if (success) {
		bt_mesh_dfu_slot_valid_set(srv->upload.slot, true);
		srv->upload.phase = BT_MESH_DFD_UPLOAD_PHASE_TRANSFER_SUCCESS;
		return;
	}

	/* Will delete slot when we start a new upload */
	srv->upload.phase = BT_MESH_DFD_UPLOAD_PHASE_TRANSFER_ERROR;
}

static void upload_suspended(struct bt_mesh_blob_srv *b)
{
	BT_DBG("");
}

const struct bt_mesh_blob_srv_cb _bt_mesh_dfd_srv_blob_cb = {
	.start = upload_start,
	.end = upload_end,
	.suspended = upload_suspended,
};

static int dfd_srv_init(struct bt_mesh_model *mod)
{
	struct bt_mesh_dfd_srv *srv = mod->user_data;

	srv->mod = mod;

	if (IS_ENABLED(CONFIG_BT_MESH_MODEL_EXTENSIONS)) {
		bt_mesh_model_extend(mod, srv->upload.blob.mod);
	}

	return 0;
}

const struct bt_mesh_model_cb _bt_mesh_dfd_srv_cb = {
	.init = dfd_srv_init,
};
