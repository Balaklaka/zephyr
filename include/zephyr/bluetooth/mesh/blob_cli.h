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

/** Target's Pull mode context used while sending chunks to the target. */
struct bt_mesh_blob_target_pull {
	/** Timestamp when the Block Report Timeout Timer expires for this target. */
	int64_t block_report_timestamp;

	/** Missing chunks reported by this target. */
	uint8_t missing[ceiling_fraction(CONFIG_BT_MESH_BLOB_CHUNK_COUNT_MAX, 8)];
};

/** BLOB Client target node */
struct bt_mesh_blob_target {
	/** Linked list node */
	sys_snode_t n;

	/** Target node address. */
	uint16_t addr;

	/** Target's Pull mode context. Needs to be initialized when sending a BLOB in Pull mode. */
	struct bt_mesh_blob_target_pull *pull;

	/** BLOB transfer status, see @ref bt_mesh_blob_status. */
	uint8_t status;

	uint8_t procedure_complete:1, /* Procedure has been completed. */
		acked:1,              /* Message has been acknowledged. Not used when sending. */
		timedout:1,           /* Target didn't respond after specified timeout. */
		skip:1;               /* Skip target from broadcast. */
};

/** BLOB Client transfer inputs. */
struct bt_mesh_blob_cli_inputs {
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

	/** Time to live value of BLOB transfer messages. */
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

/** Transfer capabilities of a target node. */
struct bt_mesh_blob_cli_caps {
	/** Max BLOB size */
	size_t max_size;

	/** Logarithmic representation of the minimum block size. */
	uint8_t min_block_size_log;

	/** Logarithmic representation of the maximum block size. */
	uint8_t max_block_size_log;

	/** Max number of chunks per block. */
	uint16_t max_chunks;

	/** Max chunk size. */
	uint16_t max_chunk_size;

	/** Max MTU size. */
	uint16_t mtu_size;

	/** Supported transfer modes. */
	enum bt_mesh_blob_xfer_mode modes;
};

/** BLOB Client state. */
enum bt_mesh_blob_cli_state {
	/** No transfer is active. */
	BT_MESH_BLOB_CLI_STATE_NONE,
	/** Retrieving transfer capabilities. */
	BT_MESH_BLOB_CLI_STATE_CAPS_GET,
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
	/** Cancelling transfer. */
	BT_MESH_BLOB_CLI_STATE_CANCEL,
	/** Transfer is suspended. */
	BT_MESH_BLOB_CLI_STATE_SUSPENDED,
};

/** Event handler callbacks for the BLOB Client model.
 *
 *  All handlers are optional.
 */
struct bt_mesh_blob_cli_cb {
	/** @brief Capabilities retrieval completion callback.
	 *
	 *  Called when the capabilities retrieval procedure completes, indicating that
	 *  a common set of acceptable transfer parameters have been established
	 *  for the given list of target nodes. All compatible targets have
	 *  status code @ref BT_MESH_BLOB_SUCCESS.
	 *
	 *  @param cli     BLOB Client instance.
	 *  @param caps    Safe transfer capabilities if the transfer capabilities
	    of at least one target has satisfied the client, or NULL otherwise.
	 */
	void (*caps)(struct bt_mesh_blob_cli *cli,
		     const struct bt_mesh_blob_cli_caps *caps);

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

	/** @brief Transfer is suspended.
	 *
	 * Called when the transfer is suspended due to response timeout from all targets.
	 *
	 * @param cli    BLOB Client instance.
	 */
	void (*suspended)(struct bt_mesh_blob_cli *cli);

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

/** @cond INTERNAL_HIDDEN */
struct blob_cli_broadcast_ctx {
	/** Called for every target in unicast mode, or once in case of multicast mode. */
	void (*send)(struct bt_mesh_blob_cli *cli, uint16_t dst);
	/** Called after every @ref blob_cli_broadcast_ctx::send callback. */
	void (*send_complete)(struct bt_mesh_blob_cli *cli, uint16_t dst);
	/** If @ref blob_cli_broadcast_ctx::acked is true, called after all targets have confirmed
	 * reception by @ref blob_cli_broadcast_rsp. Otherwise, called after transmission has been
	 * completed.
	 */
	void (*next)(struct bt_mesh_blob_cli *cli);
	/** If true, every transmission needs to be confirmed by @ref blob_cli_broadcast_rsp before
	 * @ref blob_cli_broadcast_ctx::next is called.
	 */
	bool acked;
	/** If true, the message is always sent in a unicast way. */
	bool force_unicast;
	/** If true, non-responsive targets won't be dropped after transfer has timed out. */
	bool optional;
	/** Set to true by the BLOB client between blob_cli_broadcast and broadcast_complete calls.
	 */
	bool is_inited;
};
/** INTERNAL_HIDDEN @endcond */

/** BLOB Client model instance. */
struct bt_mesh_blob_cli {
	/** Event handler callbacks */
	const struct bt_mesh_blob_cli_cb *cb;

	/* Runtime state */
	struct bt_mesh_model *mod;

	struct {
		struct bt_mesh_blob_target *target;
		struct blob_cli_broadcast_ctx ctx;
		struct k_work_delayable retry;
		/* Represents Client Timeout timer in a timestamp. Used in Pull mode only. */
		int64_t cli_timestamp;
		struct k_work complete;
		uint16_t pending;
		uint8_t retries;
		uint8_t sending : 1,
			cancelled : 1;
	} tx;

	const struct bt_mesh_blob_io *io;
	const struct bt_mesh_blob_cli_inputs *inputs;
	const struct bt_mesh_blob_xfer *xfer;
	uint16_t block_count;
	uint16_t chunk_idx;
	uint16_t mtu_size;
	enum bt_mesh_blob_cli_state state;
	struct bt_mesh_blob_block block;
	struct bt_mesh_blob_cli_caps caps;
};

/** @brief Retrieve transfer capabilities for a list of targets.
 *
 *  Queries the availability and capabilities of all target nodes, producing a
 *  cumulative set of transfer capabilities for the target nodes, and returning
 *  it through the @ref bt_mesh_blob_cli_cb::caps callback.
 *
 *  Retrieving the capabilities may take several seconds, depending on the
 *  number of targets and mesh network performance. The end of the procedure
 *  is indicated through the @ref bt_mesh_blob_cli_cb::caps callback.
 *
 *  This procedure is not required, but strongly recommended as a
 *  preparation for a transfer to maximize performance and the chances of
 *  success.
 *
 *  @param cli     BLOB Client instance.
 *  @param inputs  Statically allocated BLOB Client transfer inputs.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_blob_cli_caps_get(struct bt_mesh_blob_cli *cli,
			      const struct bt_mesh_blob_cli_inputs *inputs);

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
 *  @param inputs Statically allocated BLOB Client transfer inputs.
 *  @param xfer   Statically allocated transfer parameters.
 *  @param io     BLOB stream to read the transfer from.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_blob_cli_send(struct bt_mesh_blob_cli *cli,
			  const struct bt_mesh_blob_cli_inputs *inputs,
			  const struct bt_mesh_blob_xfer *xfer,
			  const struct bt_mesh_blob_io *io);

/** @brief Suspend the active tranfser.
 *
 *  @param cli BLOB Client instance.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_blob_cli_suspend(struct bt_mesh_blob_cli *cli);

/** @brief Resume the suspended transfer.
 *
 *  @param cli BLOB Client instance.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_blob_cli_resume(struct bt_mesh_blob_cli *cli);

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
 *          retrieving the capabilities and false otherwise.
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
