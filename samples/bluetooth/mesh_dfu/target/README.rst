.. _ble_mesh_dfu_target:

Bluetooth: Mesh DFU Target
##########################

Overview
********

This sample demonstrates how a device firmware can be upgraded over Bluetooth mesh. The sample
implements the Target role of the :ref:`Bluetooth mesh DFU subsystem <bluetooth_mesh_dfu>`. To
validate a firmware, it uses firmware metadata format defined in :c:struct:`bt_mesh_dfu_metadata`.

This sample can be used as base image or can be transfered over Bluetooth mesh to update existing
nodes.

Provisioning
============

The sample supports provisioning over both the Advertising and the GATT Provisioning Bearers (i.e.
PB-ADV and PB-GATT).

Composition data
================

The following table shows the mesh composition data for this sample:

.. table::
   :align: left

   +---------------+
   | Element 1     |
   +===============+
   | Config Server |
   +---------------+
   | Health Server |
   +---------------+
   | BLOB Server   |
   +---------------+
   | DFU Server    |
   +---------------+

Requirements
************

This sample has been tested with nRF52840 DK (nrf52840dk_nrf52840) board. The sample depends on a
partition layout. It is configured as an application for the MCUboot bootloader and has to be
flashed at slot-0. It uses slot-1 to store a new firmware image. See :ref:`flash_map_api` for
details about the partition layout.

Building and running
********************

Building and signing the sample
===============================

This sample can be found under :zephyr_file:`samples/bluetooth/mesh_dfu/target` in the Zephyr tree.

To build and flash this sample, use the following commands:

.. zephyr-app-commands::
   :zephyr-app: samples/bluetooth/mesh_dfu/target
   :board: <board>
   :goals: build flash
   :compact:

The application image must be signed before it is flashed so the bootloader can verify it. In this
sample, the default signing key file is set to ``bootloader/mcuboot/root-rsa-2048.pem`` through the
:c:macro:`CONFIG_MCUBOOT_SIGNATURE_KEY_FILE` macro in the :file:`prj.conf` file. This is also a
default key file for the MCUboot.

When building this sample, signing the firmware will be done automatically by the Zephyr build
system. When flashing, ``west flash`` will use the signed binaries. See :ref:`west-sign` for more
information.

This sample can be transferred over a mesh network to update the existing nodes, in which case the
the firmware version needs to be increased when signing so it passes the validation during the
metadata check. To sign the firmware and set a new version, execute the following command:

.. code-block:: console

    west sign -t imgtool -- --key bootloader/mcuboot/root-rsa-2048.pem --version "2.0.0+0"

Building and flashing the MCUboot
=================================

The sample is configured as an application for the MCUboot bootloader and thus requires it to be
flashed on the device. Build and flash the MCUboot bootloader as it is described in
`Building MCUboot with Zephyr`_. Remember to set ``BOOT_SIGNATURE_KEY_FILE`` Kconfig option to the
same key as for signing the sample.

Interacting with the sample
***************************

Provisioning and configuration
==============================

The sample needs to be provisioned into an existing mesh network with an external provisioner
device. After the provisioning is completed, a Configuration Client needs to add an application key
to the device, which a Distributor will use in a firmware distribution process. The added
application key should be bound to the BLOB Server and DFU Server models instantiated on the device.

Performing a Device Firmware Upgrade
====================================

The firmware distribution process starts on a target node with checking a metadata supplied with
a new firmware. The following checks are performed in this sample:

* That the new firmware version is higher than the existing one
* That the new firmware fits into the flash storage

If the metadata check completes successfully, the sample selects a value from
:c:enum:`bt_mesh_dfu_effect` depending on whether the composition data changes after flashing the
new firmware or not. This has an effect on the Distributor and the Target node. Only 2 options are
supported by this sample:

:c:enum:`BT_MESH_DFU_EFFECT_NONE`
  This effect is chosen if the composition data of the new firmware doesn't change. In this case
  the device will stay provisioned after the new firmware is flashed.
:c:enum:`BT_MESH_DFU_EFFECT_UNPROV`
  This effect is chosen if the composition data in the new firmware changes. In this case, the
  device unprovisions itself before flashing the new firmware. The unprovisioning happens before the
  device reboots, so if the MCUboot fails to validate the new firmware, the device will
  boot unprovisioned anyway.

When the DFU transfer starts, the sample stores the new firmware at slot-1.

When the DFU transfer ends, the sample requests the MCUboot to replace slot-0 with slot-1 and
reboot the device. The MCUboot performs the validation of firmware located at slot-1. Upon
successful validation, the MCUboot replaces the old firmware with the new one and boots it. After
booting, the sample confirms the image so the old image does not get reverted at the next reboot.

When the sample is used as a new firmware, independently of the provisioning state, it sets the DFU
Server model to Idle state after booting. If the device stays provisioned, it lets the Distributor
successfully finalize the firmware distribution process. If the device is unprovisioned, it has no
effect on the DFU Server. The firmware distribution process then succeedes on the Distributor side,
if the Target node doesn't respond to the Distributor after flashing the new firmware.

For more information about the firmware distribution process, see :ref:`bluetooth_mesh_dfu`.

.. _Building MCUboot with Zephyr: https://www.mcuboot.com/documentation/readme-zephyr/#building-the-bootloader-itself
