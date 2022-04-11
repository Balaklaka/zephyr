.. _ble_mesh_dfu_distributor:

Bluetooth: Mesh Device Firmware Upgrade (DFU) distributor
#########################################################

Overview
********

This sample demonstrates how device firmware can be distributed over Bluetooth mesh network. The
sample implements the Firmware Distribution role of the
:ref:`Bluetooth mesh DFU subsystem <bluetooth_mesh_dfu>`.

The following are the key features of the sample:

* The sample is configured as an application for the :ref:`mcuboot`.
* The image management subsystem of the :ref:`mcu_mgr` is used to upload firmware images to the
  Distributor.
* A set of shell commands is provided to control the firmware distribution over a Bluetooth mesh
  network.
* Self-update is supported.

Provisioning
============

The sample supports provisioning over both the Advertising and the GATT Provisioning Bearers,
PB-ADV and PB-GATT respectively.

Composition data
================

To implement the Firmware Distribution role, the sample instantiates the
:ref:`bluetooth_mesh_dfd_srv` model. The DFD Server model and its base models are instantiated on
the primary element. Also the :ref:`bluetooth_mesh_dfu_srv` model is instantiated to support the
self-update of the sample. The DFU Server model and its base models are instantiated on the
secondary element. The following table shows the mesh composition data for this sample:

.. table::
   :align: left

   +---------------+-------------+
   | Element 1     | Element 2   |
   +===============+=============+
   | Config Server | BLOB Server |
   +---------------+-------------+
   | Health Server | DFU Server  |
   +---------------+-------------+
   | BLOB Client   |             |
   +---------------+-------------+
   | DFU Client    |             |
   +---------------+-------------+
   | BLOB Server   |             |
   +---------------+-------------+
   | DFD Server    |             |
   +---------------+-------------+

Logging
=======

In this sample, the UART console is occupied by the shell module. Therefore, it uses Segger RTT as
a logging backend. For the convenience, ``printk`` is also duplicated to Segger RTT.

Requirements
************

This sample has been tested with the nRF52840 DK (nrf52840dk_nrf52840) board.

For uploading an image to the Distributor, this sample also requires a smartphone with Nordic
Semiconductor's nRF Connect Device Manager mobile app installed in one of the following versions:

* `nRF Connect Device Manager for Android`_
* `nRF Connect Device Manager for iOS`_

Building and running
********************

Building and programming the MCUboot
====================================

The sample is configured as an application for the MCUboot bootloader and thus requires the
bootloader to be programmend onto the device before the application:

.. zephyr-app-commands::
   :zephyr-app: ../bootloader/mcuboot/boot/zephyr
   :board: <board>
   :goals: build flash
   :gen-args: -DBOOT_SIGNATURE_KEY_FILE=\"root-rsa-2048.pem\"
   :build-dir: build_mcuboot
   :compact:

See `Building MCUboot with Zephyr`_ for more information about building and programming the MCUboot
bootloader.

Building, signing and programming the sample
============================================

This sample can be found under :zephyr_file:`samples/bluetooth/mesh_dfu/distributor` in the Zephyr
tree.

To build this sample, use the following command:

.. zephyr-app-commands::
   :zephyr-app: samples/bluetooth/mesh_dfu/distributor
   :board: <board>
   :goals: build
   :compact:

The sample image must be signed before it is programmed so the bootloader can verify it. This is
done automatically by the Zephyr build system. The default signing key file is set to
``bootloader/mcuboot/root-rsa-2048.pem`` through the :c:macro:`CONFIG_MCUBOOT_SIGNATURE_KEY_FILE`
macro in the :file:`prj.conf` file. This is also a default key file for the MCUboot.

To program this sample, use the following command:

.. zephyr-app-commands::
   :zephyr-app: samples/bluetooth/mesh_dfu/distributor
   :board: <board>
   :goals: flash
   :compact:

When programming, ``west flash`` will use the signed binaries. See :ref:`west-sign` for more
information about the signing process.

Performing a Device Firmware Upgrade
************************************

The Bluetooth mesh defines the Firmware upgrade Initiator role to control the firmware distribution.
This sample supports, but doesn't require an external Initiator to control the DFU procedure.
The Bluetooth mesh DFU subsystem also provides a set of shell commands that can be used instead of
the Initiator. Follow the :ref:`bluetooth_mesh_dfu_eval` guide to perform the firmware
distribution without the Initiator.

The commands can be executed in two ways:

* Through the shell management subsystem of MCUmgr (for example, using nRF Connect Device Manager
  mobile application or :ref:`MCUmgr command-line tool <mcumgr_cli>`), or
* By accessing the :ref:`shell_api` module over UART.

Uploading a firmware image
==========================

A firmware image can be uploaded to the device in two ways:

* In-band, using BLOB models by an Initiator device, or
* Out-of-band, using the image management subsystem.

For out-of-band upload, the sample uses the image management subsystem of the :ref:`mcu_mgr`. The
management subsystem uses the Simple Management Protocol (SMP), provided by MCUmgr library, to
exchange commands and data between the SMP server (the sample device) and the SMP client. This
sample supports Bluetooth Low Energy as the SMP transport. See :ref:`device_mgmt` for more
information about MCUmgr and SMP.

In this sample, the device flash is split into fixed partitions using devicetree as defined in
:zephyr_file:`nrf52840dk_nrf52840.dts<boards/arm/nrf52840dk_nrf52840/nrf52840dk_nrf52840.dts>`.
The firmware image that is to be distributed over Bluetooth mesh network should be stored at slot-1.
The sample uses :ref:`flash_map_api` to read the firmware image from slot-1 when distributes it to
target nodes.

When the image is sent in-band, the DFD server will store the firmware image in slot-1.

To upload an image to slot-1 on the sample device out-of-band, use a smartphone with Nordic
Semiconductor's nRF Connect Device Manager mobile app installed on it.

.. note::
   Because the same slot (slot-1) is used by the MCUboot bootloader for local DFU, do not request
   to test the image when uploading the firmware to the sample device. Otherwise, the bootloader
   will try swapping the distributor image with the uploaded one at the next reboot.

Copy the new image to the mobile phone. Then, in the mobile app, do the following:

* Find and select the :guilabel:`Mesh DFU Distributor` device.
* Go to the :guilabel:`Image` tab.
* Press the :guilabel:`ADVANCED` button in the right top corner. This will allow uploading the image
  to slot-1 without swapping the image on the Distributor.
* Under the :guilabel:`Firmware Upload` area, press the :guilabel:`SELECT FILE` button and select
  the copied image.
* Press the :guilabel:`UPLOAD` button.
* Select :guilabel:`Application Core (0)` and press :guilabel:`OK`.

Once the image upload is done, the :guilabel:`State` field is set to :guilabel:`UPLOAD COMPLETE`.

Changing the firmware distribution phase
========================================

When the firmware distribution phase changes, the sample will print a corresponding message, for
example, when the distribution is completed, the sample will print::

  Distribution phase changed to Completed

Self-update
===========

This sample instantiates the DFU and BLOB Server models on its secondary element and thus can also
be updated over Bluetooth mesh by any other Distributor or by itself.

To update this sample, use the address of the secondary element of the sample as the address of the
target node.

When the Distributor updates itself, the DFU transfer will end immediately after start as the image
is already stored on the device.

.. note::
   Do not add other target nodes but the Distributor when performing a self-update. If the DFD
   server on the Distributor finds itself in the list of target nodes, it skips the DFU transfer as
   the image is already stored on the device. Thus, other nodes won't receive the image.

When this sample is used as a Target, it behaves as described in :ref:`ble_mesh_dfu_target_upgrade`.

.. _Building MCUboot with Zephyr: https://www.mcuboot.com/documentation/readme-zephyr/#building-the-bootloader-itself
.. _nRF Connect Device Manager for Android: https://play.google.com/store/apps/details?id=no.nordicsemi.android.nrfconnectdevicemanager
.. _nRF Connect Device Manager for iOS: https://apps.apple.com/us/app/nrf-connect-device-manager/id1519423539
