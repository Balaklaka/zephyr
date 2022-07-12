#!/usr/bin/env bash
# Copyright 2022 Nordic Semiconductor
# SPDX-License-Identifier: Apache-2.0

source $(dirname "${BASH_SOURCE[0]}")/../../_mesh_test.sh

conf=prj_pst_conf
RunTest mesh_dfu dfu_dist_dfu_self_update dfu_target_dfu_no_change -- -argstest targets=2
