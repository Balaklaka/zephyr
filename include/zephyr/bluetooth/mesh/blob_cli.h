/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @defgroup bt_mesh_blob_cli BLOB Client model API
 * @{
 * @brief API for the Binary Large Object transfer client model.
 */

#ifndef ZEPHYR_INCLUDE_BLUETOOTH_MESH_BLOB_CLI_H_
#define ZEPHYR_INCLUDE_BLUETOOTH_MESH_BLOB_CLI_H_

#include <bluetooth/mesh/access.h>
#include <bluetooth/mesh/blob.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct bt_mesh_blob_cli;

/** @def BT_MESH_MODEL_BLOB_CLI
 *
 * @brief BLOB Client model composition data entry.
 *
 * @param _cli Pointer to a @ref bt_mesh_blob_cli instance.
 */
#define BT_MESH_MODEL_BLOB_CLI(_cli)                                           \
	BT_MESH_MODEL_CB(BT_MESH_MODEL_ID_BLOB_CLI, _bt_mesh_blob_cli_op,      \
			 NULL, _cli, &_bt_mesh_blob_cli_cb)

/** BLOB Client target node */
struct bt_mesh_blob_target {
	/** Linked list node */
	sys_snode_t n;

	/** Target node address. */
	uint16_t addr;

	/** BLOB transfer status, see @ref bt_mesh_blob_status. */
	uint8_t status;

	uint8_t procedure_complete:1, /* Procedure has been completed. */
		acked:1;              /* Message has been acknowledged. */
};

/** BLOB Client transfer context. */
struct bt_mesh_blob_cli_ctx {
	/** Linked list of targets. Each node should point to @ref
	 *  bt_mesh_blob_target::n.
	 */
	sys_slist_t targets;

	/** AppKey index to send with. */
	uint16_t app_idx;

	/** Group address destination for the BLOB transfer, or @ref
	 *  BT_MESH_ADDR_UNASSIGNED to send every message to each target
	 *  individually.
	 */
	uint16_t group;

	/** Time to live value for the bounds check. */
	uint8_t ttl;

	/** Additional response time for the targets, in 10 second increments.
	 *
	 *  The extra time can be used to give the targets more time to respond
	 *  to messages from the client. The actual timeout will be calculated
	 *  according to the following formula:
	 *
	 *  @verbatim
	 *  timeout = 20 seconds + (10 seconds * timeout_base) + (100 ms * TTL)
	 *  @endverbatim
	 *
	 *  If a target fails to respond to a message from the client within the
	 *  configured transfer timeout, the target is dropped.
	 */
	uint16_t timeout_base;
};

/** Transfer parameter boundaries */
struct bt_mesh_blob_cli_bounds {
	/** Max BLOB size */
	size_t max_size;

	/** Logarithmic representation of the minimum block size. */
	uint8_t min_block_size_log;

	/** Logarithmic representation of the maximum block size. */
	uint8_t max_block_size_log;

	/** Max number of chunks per block. */
	uint16_t max_chunks;

	/** Max chunk size. */
	uint16_t chunk_size;

	/** Max MTU size. */
	uint16_t mtu_size;

	/** Supported transfer modes. */
	enum bt_mesh_blob_xfer_mode modes;
};

/** BLOB Client state. */
enum bt_mesh_blob_cli_state {
	/** No transfer is active. */
	BT_MESH_BLOB_CLI_STATE_NONE,
	/** Checking transfer parameter boundaries. */
	BT_MESH_BLOB_CLI_STATE_BOUNDS_CHECK,
	/** Sending transfer start. */
	BT_MESH_BLOB_CLI_STATE_START,
	/** Sending block start. */
	BT_MESH_BLOB_CLI_STATE_BLOCK_START,
	/** Sending block chunks. */
	BT_MESH_BLOB_CLI_STATE_BLOCK_SEND,
	/** Checking block status. */
	BT_MESH_BLOB_CLI_STATE_BLOCK_CHECK,
	/** Checking transfer status. */
	BT_MESH_BLOB_CLI_STATE_XFER_CHECK,
	/** Checking transfer status. */
	BT_MESH_BLOB_CLI_STATE_CANCEL,
};

/** Event handler callbacks for the BLOB Client model.
 *
 *  All handlers are optional.
 */
struct bt_mesh_blob_cli_cb {
	/** @brief Boundary check completion callback.
	 *
	 *  Called when the boundary check procedure completes, indicating that
	 *  a common set of acceptable transfer parameters have been established
	 *  for the given list of target nodes. All compatible targets have
	 *  status code @ref BT_MESH_BLOB_SUCCESS.
	 *
	 *  @param cli     BLOB Client instance.
	 *  @param bounds  Safe transfer parameter boundaries.
	 */
	void (*bounds)(struct bt_mesh_blob_cli *cli,
		       const struct bt_mesh_blob_cli_bounds *bounds);

	/** @brief Target loss callback.
	 *
	 *  Called whenever a target has been lost due to some error in the
	 *  transfer. Losing a target node is not considered a fatal error for
	 *  the client until all targets have been lost.
	 *
	 *  @param cli    BLOB Client instance.
	 *  @param target Target that was lost.
	 *  @param reason Reason for the target loss.
	 */
	void (*lost_target)(struct bt_mesh_blob_cli *cli,
			    struct bt_mesh_blob_target *target,
			    enum bt_mesh_blob_status reason);

	/** @brief Transfer end callback.
	 *
	 *  Called when the transfer ends.
	 *
	 *  @param cli     BLOB Client instance.
	 *  @param xfer    Completed transfer.
	 *  @param success Status of the transfer. Is @c true if at least one
	 *                 target received the whole transfer.
	 */
	void (*end)(struct bt_mesh_blob_cli *cli,
		    const struct bt_mesh_blob_xfer *xfer, bool success);
};

/** BLOB Client model instance. */
struct bt_mesh_blob_cli {
	/** Event handler callbacks */
	const struct bt_mesh_blob_cli_cb *cb;

	/* Runtime state */
	struct bt_mesh_model *mod;
	const struct bt_mesh_blob_xfer *xfer;

	struct {
		struct bt_mesh_blob_target *target;
		const struct blob_cli_broadcast_ctx *ctx;
		struct k_delayed_work retry;
		struct k_work complete;
		uint16_t pending;
		uint8_t retries;
		uint8_t polls;
		uint8_t sending : 1,
			cancelled : 1;
	} tx;

	const struct bt_mesh_blob_io *io;
	const struct bt_mesh_blob_cli_ctx *ctx;
	uint16_t block_count;
	uint16_t chunk_idx;
	uint16_t mtu_size;
	uint8_t block_size_log;
	enum bt_mesh_blob_cli_state state;
	struct bt_mesh_blob_block block;
	struct bt_mesh_blob_cli_bounds *bounds;
};

/** The BLOB Client's own transfer parameter boundaries. */
extern const struct bt_mesh_blob_cli_bounds bt_mesh_blob_cli_boundaries;

/** @brief Check transfer parameter boundaries for a list of targets.
 *
 *  Queries the availability and capabilities of all target nodes, producing a
 *  cumulative set of parameter boundaries for the target nodes, and returning
 *  it through the @ref bt_mesh_blob_cli_cb::bounds callback.
 *
 *  The boundary check may take several seconds, depending on the number of
 *  targets and mesh network performance. The end of the boundary check is
 *  indicated through the @ref bt_mesh_blob_cli_cb::bounds callback.
 *
 *  The boundary check is not required, but strongly recommended as a
 *  preparation for a transfer to maximize performance and the chances of
 *  success.
 *
 *  @param cli     BLOB Client instance.
 *  @param ctx     Statically allocated BLOB context.
 *  @param bounds  Initial boundary parameters. The memory must be available
 *                 until the end of the procedure.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_blob_cli_bounds_check(struct bt_mesh_blob_cli *cli,
				  const struct bt_mesh_blob_cli_ctx *ctx,
				  struct bt_mesh_blob_cli_bounds *bounds);

/** @brief Perform a BLOB transfer.
 *
 *  Starts sending the transfer to the target nodes. Only target nodes with a
 *  @c status of @ref BT_MESH_BLOB_SUCCESS will be considered.
 *
 *  The transfer will keep going either until all targets have been dropped, or
 *  the full BLOB has been sent.
 *
 *  The BLOB transfer may take several minutes, depending on the number of
 *  targets, size of the BLOB and mesh network performance. The end of the
 *  transfer is indicated through the @ref bt_mesh_blob_cli_cb::end callback.
 *
 *  A client only supports one transfer at the time.
 *
 *  @param cli    BLOB Client instance.
 *  @param ctx    Statically allocated BLOB context.
 *  @param xfer   Statically allocated transfer parameters.
 *  @param bounds Transfer boundaries, or NULL to use the highest boundaries
 *                supported by the client.
 *  @param io     BLOB stream to read the transfer from.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_blob_cli_send(struct bt_mesh_blob_cli *cli,
			  const struct bt_mesh_blob_cli_ctx *ctx,
			  const struct bt_mesh_blob_xfer *xfer,
			  const struct bt_mesh_blob_cli_bounds *bounds,
			  const struct bt_mesh_blob_io *io);

/** @brief Cancel an ongoing transfer.
 *
 *  @param cli BLOB Client instance.
 */
void bt_mesh_blob_cli_cancel(struct bt_mesh_blob_cli *cli);

/** @brief Get the current progress of the active transfer in percent.
 *
 *  @param cli BLOB Client instance.
 *
 *  @return The current transfer progress, or 0 if no transfer is active.
 */
uint8_t bt_mesh_blob_cli_progress(struct bt_mesh_blob_cli *cli);

/** @brief Get the current state of the BLOB Client.
 *
 *  @param cli BLOB Client instance.
 *
 *  @return true if the BLOB Client is currently participating in a transfer or
 *          bounds check and false otherwise.
 */
bool bt_mesh_blob_cli_is_busy(struct bt_mesh_blob_cli *cli);

/** @cond INTERNAL_HIDDEN */
extern const struct bt_mesh_model_op _bt_mesh_blob_cli_op[];
extern const struct bt_mesh_model_cb _bt_mesh_blob_cli_cb;
/** @endcond */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_BLUETOOTH_MESH_BLOB_CLI_H_ */

/** @} */

