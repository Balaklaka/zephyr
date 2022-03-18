/** @file
 * @brief Bluetooth Mesh DFD shell functions
 *
 * This is not to be included by the application
 */

/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __SHELL_DFD_H
#define __SHELL_DFD_H

int cmd_dfd_receivers_add(const struct shell *shell_ctx, size_t argc, char *argv[]);
int cmd_dfd_receivers_delete_all(const struct shell *shell_ctx, size_t argc, char *argv[]);
int cmd_dfd_receivers_get(const struct shell *shell_ctx, size_t argc, char *argv[]);

int cmd_dfd_capabilities_get(const struct shell *shell_ctx, size_t argc, char *argv[]);

int cmd_dfd_get(const struct shell *shell_ctx, size_t argc, char *argv[]);
int cmd_dfd_start(const struct shell *shell_ctx, size_t argc, char *argv[]);
int cmd_dfd_suspend(const struct shell *shell_ctx, size_t argc, char *argv[]);
int cmd_dfd_cancel(const struct shell *shell_ctx, size_t argc, char *argv[]);
int cmd_dfd_apply(const struct shell *shell_ctx, size_t argc, char *argv[]);

int cmd_dfd_fw_get(const struct shell *shell_ctx, size_t argc, char *argv[]);
int cmd_dfd_fw_get_by_idx(const struct shell *shell_ctx, size_t argc, char *argv[]);
int cmd_dfd_fw_delete(const struct shell *shell_ctx, size_t argc, char *argv[]);
int cmd_dfd_fw_delete_all(const struct shell *shell_ctx, size_t argc, char *argv[]);

int cmd_dfd_instance_get_all(const struct shell *sh, size_t argc, char *argv[]);
int cmd_dfd_instance_set(const struct shell *sh, size_t argc, char *argv[]);

#endif
