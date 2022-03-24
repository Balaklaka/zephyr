.. _ble_mesh_dfu_distributor:

Bluetooth: Mesh Device Firmware Upgrade (DFU) distributor
#########################################################

Overview
********

This sample demonstrates how device firmware can be distributed over Bluetooth mesh network. The
sample implements the Firmware Distribution role of the
:ref:`Bluetooth mesh DFU subsystem <bluetooth_mesh_dfu>`.

The following are the key features of the sample:

* The image management subsystem of the :ref:`mcu_mgr` is used to upload a new firmware to the
  Distributor.
* A set of shell commands is provided to control the DFU procedure.
* Self-update is supported.
* Support for an external initiator device.

To examine the sample, you can use the :ref:`ble_mesh_dfu_target` sample as a firmware for target
nodes.

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

The sample image must be signed before it is programmed so the bootloader can verify it. This is done
automatically by the Zephyr build system. The default signing key file is set to
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

Using the sample as a new firmware
==================================

This sample can be transferred over a mesh network to update the existing nodes. In that case, the
firmware version needs to be increased upon signing to pass the validation during the metadata
check. To sign the firmware and set a new version, execute the following command:

.. code-block:: console

    west sign -t imgtool -- --key ../bootloader/mcuboot/root-rsa-2048.pem --version "2.0.0+0"

Provisioning and configuration
******************************

The sample needs to be provisioned into an existing mesh network with an external provisioner
device. After the provisioning is completed, a Configuration Client needs to add an application key
to the device.

The added application key should be bound to the DFD Server, DFU Client, BLOB Server and BLOB Client
models instantiated on the primary element, and to the DFU Server and BLOB Server models
instantiated on the secondary element of the device.

The Distributor will use the bound application key in the firmware distribution procedure,
therefore, the same application key should be bound to the DFU Server and BLOB Server models
instantiated on target nodes.

Performing a Device Firmware Upgrade
************************************

The DFU procedure for this sample consists of the following steps:

1. Uploading a new image to the distributor device.
#. Preparing for the DFU transfer.
#. Starting the DFU transfer.
#. Controlling the DFU transfer.
#. Applying the new image.
#. Completing the DFU procedure.

.. note::
   This sample can also work with an external initiator. In this case, the DFU procedure is fully
   controlled by the Initiator.

Uploading a new image to the distributor device
===============================================

The sample uses the image management subsystem of the :ref:`mcu_mgr` to help upload a new image to
the Distributor. The management subsystem uses the Simple Management Protocol (SMP), provided by
MCUmgr library, to exchange commands and data between the SMP server (distributor) and the SMP
client. The DFU distributor sample supports Bluetooth Low Energy as the SMP transport. See
:ref:`device_mgmt` for more information about MCUmgr and SMP.

The sample uses devicetree to split flash into partitions. Slot-1 partition is used to store a new
image that will be transferred to target nodes and used for self-update. For more information about
the partition layout, read :ref:`flash_map_api`.

To upload an image to slot-1 on the distributor device, use a smartphone with Nordic Semiconductor's
nRF Connect Device Manager mobile app installed on it.

.. note::
   Because the same slot (slot-1) is used by the MCUboot bootloader for local DFU, do not trigger
   the image swapping when uploading the firmware to the Distributor. Otherwise, the bootloader
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

.. note::
   When using an external initiator device, the image can be uploaded in-band or using SMP over
   Bluetooth Low Energy.

Preparing for the DFU transfer
==============================

To configure and control the DFU procedure, the sample provides a set of shell commands. See
:ref:`bluetooth_mesh_shell_dfd_server` for the complete list of commands. The commands can
be executed in two ways:

* Through the shell management subsystem of MCUmgr (for example, using nRF Connect Device Manager
  or :ref:`MCUmgr command-line tool <mcumgr_cli>`), or
* By accessing the shell module over UART.

.. note::
   When using an external initiator device, the DFU procedure can also be controlled by
   a Firmware Distribution Client model.

How to issue shell commands through MCUmgr client is out of scope of this sample.

To access the shell module over UART:

1. Connect the development kit to the computer using a USB cable. The development kit is assigned
   a COM port (Windows), ttyACM device (Linux) or tty.usbmodem (MacOS).
2. Connect to the kit that runs this sample with a terminal emulator that supports VT100/ANSI
   escape characters (for example, PuTTY).
3. Enable local echo in the terminal to see the text you are typing.

The DFU transfer preparation consists of two steps:

1. Registering an image slot
#. Adding DFU targets

Registering an image slot
-------------------------

After the image has been uploaded to the distributor device, it needs to be registered in the
Bluetooth mesh DFU subsystem. To do that, add a DFU image slot using the ``mesh dfu-slot-add``
shell command specifying size in bytes of the image that was uploaded to slot-1. Optionally, you can
provide firmware ID, metadata and Unique Resource Identifier (URI) parameters of the image. For
example, to allocate slot for an image size 20540, type the following command::

  mesh dfu-slot-add 20540 [<firmware-id> [<metadata> [<uri>]]]

When the slot is added, the shell will print the slot ID. Remember this ID as it will then be needed
to start the DFU transfer::

  Adding slot (size: 20540)
  Slot added. ID: 0

To update the image slot, first, remove it using the ``mesh dfu-slot-del`` shell command and then
add it again.

Getting the firmware ID
^^^^^^^^^^^^^^^^^^^^^^^

When using :ref:`ble_mesh_dfu_target` as a target node image or for updating of this sample, the
firmware ID corresponds to the image version and is encoded in the format defined by the
:c:struct:`mcuboot_img_sem_ver` structure. For example, when the new version is ``2.0.0+0``, the
encoded value will be ``0200000000000000``.

Getting the metadata
^^^^^^^^^^^^^^^^^^^^

Both this sample and :ref:`ble_mesh_dfu_target` sample use firmware metadata format defined in the
:c:struct:`bt_mesh_dfu_metadata` structure. To prepare metadata for the DFU transfer, the mesh
shell provides a set of commands defined in :ref:`bluetooth_mesh_shell_dfu_metadata`.

Start with ``mesh dfu-comp-add`` command to encode a Composition Data header specifying::

  mesh dfu-comp-add 0x59 0 0 10 7

Encode each element of the new image using ``mesh dfu-comp-elem-add`` command. When updating
:ref:`ble_mesh_dfu_target`::

  mesh dfu-comp-elem-add 1 4 0 0x0000 0x0002 0xBF42 0xBF44

When updating the distributor sample::

  mesh dfu-comp-elem-add 1 6 0 0x0000 0x0002 0xBF42 0xBF43 0xBF45 0xBF46
  mesh dfu-comp-elem-add 2 2 0 0xBF42 0xBF44

Generate hash of the Composition Data using ``mesh dfu-comp-hash-get``. When updating
:ref:`ble_mesh_dfu_target`, the output should be::

  Composition data to be hashed:
          CID: 0x0059
          PID: 0x0000
          VID: 0x0000
          CPRL: 10
          Features: 0x7
          Elem: 1
                  NumS: 4
                  NumV: 0
                  SIG Model ID: 0x0000
                  SIG Model ID: 0x0002
                  SIG Model ID: 0xbf42
                  SIG Model ID: 0xbf44
  Composition data hash: 0x71f3267c

When updating the distributor sample, the output should be::

  Composition data to be hashed:
          CID: 0x0059
          PID: 0x0000
          VID: 0x0000
          CPRL: 10
          Features: 0x7
          Elem: 1
                  NumS: 6
                  NumV: 0
                  SIG Model ID: 0x0000
                  SIG Model ID: 0x0002
                  SIG Model ID: 0xbf42
                  SIG Model ID: 0xbf43
                  SIG Model ID: 0xbf45
                  SIG Model ID: 0xbf46
          Elem: 2
                  NumS: 2
                  NumV: 0
                  SIG Model ID: 0xbf42
                  SIG Model ID: 0xbf44
  Composition data hash: 0xa9ff3629

Use ``mesh dfu-metadata-encode`` shell command to encode the metadata specifying:

* ``major``, ``minor``, ``revision`` and ``build_num`` to the values used in ``west sign`` command
  from ``Using the sample as a new firmware`` section.
* ``size`` as a size of the signed bin file.
* ``core type`` set to 1.
* ``hash`` set to the value generated above.
* ``elem`` to the number of elements on the new image.

For example, when updating :ref:`ble_mesh_dfu_target` with firmware version ``2.0.0+0``::

  mesh dfu-metadata-encode 2 0 0 0 <signed bin size> 1 0x71f3267c 1

When updating the distributor sample with firmware version ``2.0.0+0``::

  mesh dfu-metadata-encode 2 0 0 0 <signed bin size> 1 0xa9ff3629 2

The encoded metadata value printed by the shell can now be used when adding a new image slot.

Adding DFU targets
------------------

Add target nodes to the DFU transfer using ``mesh dfd-receivers-add`` shell command specifying
element address of a target node, where the DFU server is instantiated, and the image index on the
target node, which needs to be updated. For example, assuming that there are two target nodes with
addresses ``0x0004`` and ``0x0005`` accordingly with image index 0::

  mesh dfd-receivers-add 0x0004,0;0x0005,0

Starting the DFU transfer
=========================

To start the DFU transfer use the ``mesh dfd-start`` shell command. This command requires two
mandatory arguments: ``app_idx`` and ``slot_idx``:

* As ``app_idx``, use the application key index that is bound to the DFD Server and its base models.
  The same application key should be bound to the DFU Server and BLOB Server models on target nodes.
* As ``slot_idx``, use the ID of the allocated slot on the previous step.

This will run the DFU transfer in unicast mode with other arguments set to their default values as
described in the command description in :ref:`bluetooth_mesh_shell_dfd_server`.

Once the DFU transfer is started, the shell will print::

  Distribution phase changed to Transfer Active

Controlling the DFU transfer
============================

You can view the progress of the DFU transfer using the ``mesh dfd-receivers-get`` shell command.
For example, to get the status of the two target nodes participating in the transfer, type the
following command::

  mesh dfd-receivers-get 0 2

Once the DFU transfer is successfully completed, the shell will print::

  Distribution phase changed to Transfer Success

Applying the new image
======================

If ``policy_apply`` is set to true or omitted when the DFU transfer starts, the DFD server will
automatically apply the new firmware on the target nodes upon DFU transfer completion.

If ``policy_apply`` is set to false, the image needs to be applied manually using the
``mesh dfd-apply`` command once the DFU transfer is completed.

When the DFD server asks the target nodes to apply the transferred image, the shell will print::

  Distribution phase changed to Applying Update

Completing the DFU procedure
============================

After applying the new firmware, the DFD server will automatically try to confirm that the new
image has been loaded on the target nodes by requesting the firmware ID of the currently running
firmware. Depending on the :c:enum:`bt_mesh_dfu_effect` value received from the target nodes after
the DFU transfer is started, the following cases are possible:

* If the image effect for a particular target node is :c:enum:`BT_MESH_DFU_EFFECT_UNPROV`, the
  Distributor doesn't expect any reply from that target node. If the Distributor doesn't receive
  any reply, it will retry the request until it runs out of attempts. If the Distributor receives
  a reply, the DFU procedure for this particular target node is considered unsuccessful.
  Otherwise, the DFU procedure is considered successful.
* In all other cases, the Distributor expects a reply from the target node with the firmware ID
  equal to the firmware ID of the transferred image. If the target node responds with a different
  firmware ID or doesn't respond at all, the DFU procedure for this particular target node is
  considered unsuccessful. Otherwise, the DFU procedure is considered successful.

The DFU procedure ends after the Distributor stops polling the target nodes. If the DFU
procedure completes successfully for at least one target node, the shell will print::

  Distribution phase changed to Completed

Othewise, the shell will print::

  Distribution phase changed to Failed

Cancelling the DFU procedure
****************************

To cancel the DFU procedure, use the ``mesh dfd-cancel`` shell command. In this case, the shell
will print::

  Distribution phase changed to Cancelling Update

When the cancelling is completed, the shell will print::

  Distribution phase changed to Idle

Performing a self-update
************************

This sample instantiates the DFU Server model on its secondary element and thus can also be updated
over Bluetooth mesh by any other Distributor or by itself.

To update this sample, use the address of the secondary element of the sample as the address of the
target node.

When the Distributor updates itself, the DFU transfer will end immediately after start as the image
is already stored on the device.

.. note::
   Do not add other target nodes but the Distributor when performing a self-update. If the DFD
   server on the Distributor finds itself in the list of target nodes, it skips the DFU transfer as
   the image is already stored on the device. Thus, other nodes won't receive the image.

When the Distributor is updated, it behaves as described in section
:ref:`ble_mesh_dfu_target_upgrade` of the :ref:`ble_mesh_dfu_target` sample.

Recovering from failed DFU procedure
************************************

If the DFU procedure fails for any reason, run the following shell commands before making a new
attempt::

  mesh dfd-receivers-delete-all
  mesh dfd-cancel

This will remove all target nodes from the DFU procedure and return the DFD server to the idle
phase. The added DFU image slots will not be removed.

.. _Building MCUboot with Zephyr: https://www.mcuboot.com/documentation/readme-zephyr/#building-the-bootloader-itself
.. _nRF Connect Device Manager for Android: https://play.google.com/store/apps/details?id=no.nordicsemi.android.nrfconnectdevicemanager
.. _nRF Connect Device Manager for iOS: https://apps.apple.com/us/app/nrf-connect-device-manager/id1519423539
