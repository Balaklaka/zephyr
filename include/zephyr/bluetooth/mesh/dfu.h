/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @defgroup bt_mesh_dfu Bluetooth Mesh Device Firmware Update
 * @{
 * @brief Common types and functions for the Bluetooth Mesh Device Firmware
 *        Update procedure.
 */

#ifndef ZEPHYR_INCLUDE_BLUETOOTH_MESH_DFU_H__
#define ZEPHYR_INCLUDE_BLUETOOTH_MESH_DFU_H__

#include <zephyr.h>
#include <sys/types.h>
#include <bluetooth/mesh/blob.h>

#ifdef __cplusplus
extern "C" {
#endif

/** DFU transfer phase */
enum bt_mesh_dfu_phase {
	/** Ready to start a Receive Firmware procedure. */
	BT_MESH_DFU_PHASE_IDLE,

	/** The Transfer BLOB procedure failed. */
	BT_MESH_DFU_PHASE_TRANSFER_ERR,

	/** The Receive Firmware procedure is being executed  */
	BT_MESH_DFU_PHASE_TRANSFER_ACTIVE,

	/** The Verify Firmware procedure is being executed. */
	BT_MESH_DFU_PHASE_VERIFY,

	/** The Verify Firmware procedure completed successfully. */
	BT_MESH_DFU_PHASE_VERIFY_OK,

	/** The Verify Firmware procedure failed. */
	BT_MESH_DFU_PHASE_VERIFY_FAIL,

	/** The Apply New Firmware procedure is being executed. */
	BT_MESH_DFU_PHASE_APPLYING,

	/** Firmware transfer has been canceled */
	BT_MESH_DFU_PHASE_TRANSFER_CANCELED,

	/** Firmware applying succeeded */
	BT_MESH_DFU_PHASE_APPLY_SUCCESS,

	/** Firmware applying failed */
	BT_MESH_DFU_PHASE_APPLY_FAIL,

	/** The current phase is unknown.
	 *
	 *  This is a metaphase, used by the DFU Client to keep track of
	 *  the target state, and is not defined by the spec.
	 */
	BT_MESH_DFU_PHASE_UNKNOWN,
};


/** DFU status */
enum bt_mesh_dfu_status {
	/** The message was processed successfully. */
	BT_MESH_DFU_SUCCESS,

	/** Insufficient resources on the node */
	BT_MESH_DFU_ERR_RESOURCES,

	/** The operation cannot be performed while the server is in the current
	 *  phase.
	 */
	BT_MESH_DFU_ERR_WRONG_PHASE,

	/** An internal error occurred on the node. */
	BT_MESH_DFU_ERR_INTERNAL,

	/** The message contains a firmware index value that is not expected. */
	BT_MESH_DFU_ERR_FW_IDX,

	/** The metadata check failed. */
	BT_MESH_DFU_ERR_METADATA,

	/** The server cannot start a firmware update. */
	BT_MESH_DFU_ERR_TEMPORARILY_UNAVAILABLE,

	/** Another BLOB transfer is in progress. */
	BT_MESH_DFU_ERR_BLOB_XFER_BUSY,
};

/** Expected effect of a DFU transfer */
enum bt_mesh_dfu_effect {
	/** No changes to node composition data. */
	BT_MESH_DFU_EFFECT_NONE,

	/** Node composition data changed and the node does not support remote
	 *  provisioning.
	 */
	BT_MESH_DFU_EFFECT_COMP_CHANGE_NO_RPR,

	/** Node composition data changed, and remote provisioning is supported.
	 *  The node supports remote provisioning and composition data page
	 *  0x80. Page 0x80 contains different composition data than page 0x0.
	 */
	BT_MESH_DFU_EFFECT_COMP_CHANGE,

	/** Node will be unprovisioned after the update. */
	BT_MESH_DFU_EFFECT_UNPROV,
};

/** Action for DFU iteration callbacks */
enum bt_mesh_dfu_iter {
	/** Stop iterating. */
	BT_MESH_DFU_ITER_STOP,

	/** Continue iterating. */
	BT_MESH_DFU_ITER_CONTINUE,
};

/** DFU image instance.
 *
 *  Each DFU image represents a single updateable firmware image.
 */
struct bt_mesh_dfu_img {
	/** Firmware ID. */
	const void *fwid;

	/** Length of the firmware ID. */
	size_t fwid_len;

	/** Update URI, or NULL.
	 *
	 *  Must use one of the http: or https: schemes.
	 */
	const char *uri;
};

/** DFU image slot for DFU distribution */
struct bt_mesh_dfu_slot {
	/** Size of the firmware in bytes. */
	size_t size;
	/** Length of the firmware ID. */
	size_t fwid_len;
	/** Length of the metadata. */
	size_t metadata_len;
	/** Length of the image URI. */
	size_t uri_len;
	/** Firmware ID. */
	uint8_t fwid[CONFIG_BT_MESH_DFU_FWID_MAXLEN];
	/** Metadata. */
	uint8_t metadata[CONFIG_BT_MESH_DFU_METADATA_MAXLEN];
	/** Image URI. */
	char uri[CONFIG_BT_MESH_DFU_URI_MAXLEN];
};

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_BLUETOOTH_MESH_DFU_H__ */

/** @} */
