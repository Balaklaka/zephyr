/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @defgroup bt_mesh_priv_beacon_cli Bluetooth Mesh Private Beacon Client
 * @{
 * @brief
 */

#ifndef ZEPHYR_INCLUDE_BLUETOOTH_MESH_PRIV_BEACON_CLI_H__
#define ZEPHYR_INCLUDE_BLUETOOTH_MESH_PRIV_BEACON_CLI_H__

#include <zephyr/bluetooth/mesh.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @def BT_MESH_MODEL_PRIV_BEACON_CLI
 *
 *  @brief Private Beacon Client model composition data entry.
 *
 *  @param cli_data Pointer to a @ref bt_mesh_priv_beacon_cli instance.
 */
#define BT_MESH_MODEL_PRIV_BEACON_CLI(cli_data)                                \
	BT_MESH_MODEL_CB(BT_MESH_MODEL_ID_PRIV_BEACON_CLI,                     \
			 bt_mesh_priv_beacon_cli_op, NULL, cli_data,           \
			 &bt_mesh_priv_beacon_cli_cb)

/** Mesh Private Beacon Client model */
struct bt_mesh_priv_beacon_cli {
	/** Timeout value in milliseconds. */
	int32_t timeout;
	struct bt_mesh_model *mod;
	uint16_t addr;
	uint32_t op;
	void *rsp;
	struct k_sem sem;
};

/** Private Beacon */
struct bt_mesh_priv_beacon {
	/** Private beacon is enabled */
	uint8_t enabled;
	/** Random refresh interval (in 10 second steps), or 0 to keep current
	 *  value.
	 */
	uint8_t rand_interval;
};

/** Private Node Identity */
struct bt_mesh_priv_node_id {
	/** Index of the NetKey. */
	uint16_t net_idx;
	/** Private Node Identity state */
	uint8_t state;
	/** Response status code. */
	uint8_t status;
};

/** @brief Set the target's Private Beacon state.
 *
 *  @param cli     Private Beacon Client instance.
 *  @param net_idx Network index to encrypt with.
 *  @param addr    Target node address.
 *  @param val     New Private Beacon value. Returns response status on success.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_priv_beacon_cli_set(struct bt_mesh_priv_beacon_cli *cli,
				uint16_t net_idx, uint16_t addr,
				struct bt_mesh_priv_beacon *val);

/** @brief Get the target's Private Beacon state.
 *
 *  @param cli     Private Beacon Client instance.
 *  @param net_idx Network index to encrypt with.
 *  @param addr    Target node address.
 *  @param val     Response buffer for Private Beacon value.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_priv_beacon_cli_get(struct bt_mesh_priv_beacon_cli *cli,
				uint16_t net_idx, uint16_t addr,
				struct bt_mesh_priv_beacon *val);

/** @brief Set the target's Private GATT Proxy state.
 *
 *  @param cli     Private Beacon Client instance.
 *  @param net_idx Network index to encrypt with.
 *  @param addr    Target node address.
 *  @param val     New Private GATT Proxy value. Returns response status on
 *                 success.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_priv_beacon_cli_gatt_proxy_set(struct bt_mesh_priv_beacon_cli *cli,
					   uint16_t net_idx, uint16_t addr,
					   uint8_t *val);

/** @brief Get the target's Private GATT Proxy state.
 *
 *  @param cli     Private Beacon Client instance.
 *  @param net_idx Network index to encrypt with.
 *  @param addr    Target node address.
 *  @param val     Response buffer for Private GATT Proxy value.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_priv_beacon_cli_gatt_proxy_get(struct bt_mesh_priv_beacon_cli *cli,
					   uint16_t net_idx, uint16_t addr,
					   uint8_t *val);

/** @brief Set the target's Private Node Identity state.
 *
 *  @param cli     Private Beacon Client instance.
 *  @param net_idx Network index to encrypt with.
 *  @param addr    Target node address.
 *  @param val     New Private Node Identity value. Returns response status on
 *                 success.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_priv_beacon_cli_node_id_set(struct bt_mesh_priv_beacon_cli *cli,
					uint16_t net_idx, uint16_t addr,
					struct bt_mesh_priv_node_id *val);

/** @brief Get the target's Private Node Identity state.
 *
 *  @param cli         Private Beacon Client instance.
 *  @param net_idx     Network index to encrypt with.
 *  @param addr        Target node address.
 *  @param key_net_idx Network index to get the Private Node Identity state of.
 *  @param val         Response buffer for Private Node Identity value.
 *
 *  @return 0 on success, or (negative) error code otherwise.
 */
int bt_mesh_priv_beacon_cli_node_id_get(struct bt_mesh_priv_beacon_cli *cli,
					uint16_t net_idx, uint16_t addr,
					uint16_t key_net_idx,
					struct bt_mesh_priv_node_id *val);

/** @cond INTERNAL_HIDDEN */
extern const struct bt_mesh_model_op bt_mesh_priv_beacon_cli_op[];
extern const struct bt_mesh_model_cb bt_mesh_priv_beacon_cli_cb;
/** @endcond */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_BLUETOOTH_MESH_PRIV_BEACON_CLI_H__ */

/** @} */
