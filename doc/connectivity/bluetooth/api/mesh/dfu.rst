.. _bluetooth_mesh_dfu:

Device Firmware Upgrade (DFU)
#############################

Bluetooth mesh supports the distribution of firmware images across a mesh network. The Bluetooth mesh DFU subsystem implements the Firmware update section of the Bluetooth Mesh Model Specification v1.1.

Bluetooth mesh DFU implements a distribution mechanism for firmware images, and does not put any restrictions on the size, format or usage of the images. The primary design goal of the subsystem is to provide the qualifiable parts of the Bluetooth mesh DFU specification, and leave the usage, firmware validation and deployment to the application.

The DFU specification is implemented in the Zephyr Bluetooth mesh DFU subsystem as three separate models:

.. toctree::
   :maxdepth: 1

   dfu_srv
   dfu_cli
   dfd_srv

Overview
********

DFU roles
=========

The Bluetooth mesh DFU subsystem defines three different roles the mesh nodes have to assume in the distribution of firmware images:

Firmware upgrade Target (Updating node)
   The Target is the receiver and user of the transferred firmware images. All its functionality is implemented by the :ref:`bluetooth_mesh_dfu_srv` model. A transfer may have any number of Targets, and they will all be updated concurrently.

Firmware upgrade Distributor
   The Distributor role serves two purposes in the DFU process. First, it's the Target of the upload procedure, then it distributes the uploaded image to the Target nodes. The Distributor does not select the parameters of the transfer, but relies on an Initiator to give it a list of Target nodes and transfer parameters. The Distributor functionality is implemented in two models, :ref:`bluetooth_mesh_dfd_srv` and :ref:`bluetooth_mesh_dfu_cli`. The :ref:`bluetooth_mesh_dfd_srv` is responsible for communicating with the Initiator, and the :ref:`bluetooth_mesh_dfu_cli` is responsible for distributing the image to the Target nodes.

Firmware upgrade Initiator
   The Initiator role is typically implemented by the same device that implements the Bluetooth mesh :ref:`Provisioner <bluetooth_mesh_provisioning>` and :ref:`Configurator <bluetooth_mesh_models_cfg_cli>` roles. The Initiator needs a full overview of the potential Target nodes and their firmware, and will control (and initiate) all firmware upgrades. The Initiator role is not implemented in the Zephyr Bluetooth mesh DFU subsystem.

Bluetooth mesh applications may combine the DFU roles in any way they'd like, and even take on multiple instances of the same role by instantiating the models on separate elements. For instance, the Distributor and Initiator role can be combined by instantiating the :ref:`bluetooth_mesh_dfu_cli` on the Initiator node and calling its API directly.

It's also possible to combine the Initiator and Distributor devices into a single device, and replace the Firmware Distribution Server model with a proprietary mechanism that will access the Firmware Update Client model directly, e.g. over a serial protocol.

.. note::
   All the DFU models instantiate one or more :ref:`bluetooth_mesh_blob`, and may need to be spread over multiple elements for certain role combinations.

Stages
======

The Bluetooth mesh DFU process is designed to act in three stages:

The Upload stage
   First, the image is uploaded to a Distributor in a mesh network by an external entity, such as a phone or gateway (the Initiator). During the Upload stage, the Initiator transfers the firmware image and all its metadata to the Distributor node inside the mesh network. The Distributor stores the firmware image and its metadata persistently, and awaits further instructions from the Initiator. The time required to complete the upload process depends on the size of the image. After the upload completes, the Initiator can disconnect from the network during the much more-time consuming Distribution stage. Once the firmware has been uploaded to the Distributor, the Initiator may trigger the Distribution stage at any time.

The Firmware capabilities check stage (optional)
  Before starting the Distribution stage, the Initiator may optionally check if Target nodes can accept the new firmware. Nodes that didn't respond or responded that they can't receive the new firmware will be excluded from the firmware distribution process.

The Distribution stage
   Before the firmware image can be distributed, the Initiator transfers the list of Target nodes and their designated firmware image index to the Distributor. Next, it tells the Distributor to start the firmware distributon process, which runs in the background while the Initiator and mesh network performs other duties. Once the firmware image has been transferred to the Target nodes, the Distributor may ask them to apply the firmware image immediately, and report back with their status and new firmware IDs.

Firmware images
===============

All upgradable parts of a mesh node's firmware should be represented as a firmware image. Each Target node holds a list of firmware images, each of which should be independently upgradeable and identifiable.

Firmware images are represented as a binary blob (the firmware itself) with the following additional information attached to it:

Firmware ID
   The firmware ID is used to identify a firmware image. The Initiator node may ask the Target nodes for a list of its current firmware IDs to determine whether a newer version of the firmware is available. The format of the firmware ID is vendor specific, but generally, it should include enough information for an Initiator node with knowledge of the format to determine the type of image as well as its version. The firmware ID is optional, and its max length is determined by :kconfig:option:`CONFIG_BT_MESH_DFU_FWID_MAXLEN`.

Firmware metadata
   The firmware metadata is used by the Target node to determine whether it should accept an incoming firmware upgrade, and what the effect of the upgrade would be. The metadata format is vendor specific, and should contain all information the Target node needs to verify the image, as well as any preparation the Target node has to make before the image is applied. Typical metadata information can be image signatures, changes to the node's composition data and the format of the binary blob. The Target node may perform a metadata check before accepting incoming transfers to determine whether the transfer should be started. The firmware metadata can be discarded by the Target after the metadata check, as other nodes will never request the metadata from the Target node. The firmware metadata is optional, and its max length is determined by :kconfig:option:`CONFIG_BT_MESH_DFU_METADATA_MAXLEN`.

   The Bluetooth mesh DFU subsystem in Zephyr provides its own metadata format (:c:struct:`bt_mesh_dfu_metadata`) together with a set of related functions that can be used by an end product. The support for it is enabled using the :kconfig:option:`CONFIG_BT_MESH_DFU_METADATA` option. The format of the metadata is presented in the table below.

+------------------------+--------------+----------------------------------------+
| Field                  | Size (Bytes) | Description                            |
+========================+==============+========================================+
| New firmware version   | 8 B          | 1 B: Major version                     |
|                        |              | 1 B: Minor version                     |
|                        |              | 2 B: Revision                          |
|                        |              | 4 B: Build number                      |
+------------------------+--------------+----------------------------------------+
| New firmware size      | 3 B          | Size in bytes for a new firmware       |
+------------------------+--------------+----------------------------------------+
| New firmware core type | 1 B          | Bit field:                             |
|                        |              | Bit 0: Application core                |
|                        |              | Bit 1: Network core                    |
|                        |              | Bit 2: Applications specific BLOB.     |
|                        |              | Other bits: RFU                        |
+------------------------+--------------+----------------------------------------+
| Hash of incoming       | 4 B          | Lower 4 octets of AES-CMAC             |
| composition data       | (Optional)   | (app-specific-key, composition data).  |
|                        |              | This field is present, if Bit 0 is set |
|                        |              | in the New firmware core type field.   |
+------------------------+--------------+----------------------------------------+
| New number of elements | 2 B          | Number of elements on the node         |
|                        | (Optional)   | after firmware is applied.             |
|                        |              | This field is present, if Bit 0 is set |
|                        |              | in the New firmware core type field.   |
+------------------------+--------------+----------------------------------------+
| Application-specific   | <variable>   | Application-specific data to allow     |
| data for new firmware  | (Optional)   | application to execut some             |
|                        |              | vendor-specific behaviors using        |
|                        |              | this data before it can respond        |
|                        |              | with a status message.                 |
+------------------------+--------------+----------------------------------------+

Firmware URI
   The firmware URI gives the Initiator information about where firmware upgrades for the image can be found. The URI points to an online resource the Initiator can interact with to get new versions of the firmware. This allows Initiators to perform upgrades for any node in the mesh network by interacting with the web server pointed to in the URI. The URI must point to a resource using the ``http`` or ``https`` schemes, and the targeted web server must behave according to the Check Firmware OOB procedure defined by the Bluetooth mesh model specification. The firmware URI is optional, and its max length is determined by :kconfig:option:`CONFIG_BT_MESH_DFU_URI_MAXLEN`.

   .. note::

      The out-of-band distribution mechanism is not supported.

Firmware effect
---------------

A new image may have the Composition Data page 0 different from the one allocated on a Target node. This may have an effect on the provisioning data of the node and how the Distributor finalizes the DFU procedure. Depending on the availability of the Remote Provisioning Server model on the old and new image, the device may either boot up unprovisioned after applying the new firmware or require to be re-provisioned. The complete list of available options is defined in :c:enum:`bt_mesh_dfu_effect`:

:c:enum:`BT_MESH_DFU_EFFECT_NONE`
   The device stays provisioned after the new firmware is programmed. This effect is chosen if the composition data of the new firmware doesn't change.
:c:enum:`BT_MESH_DFU_EFFECT_COMP_CHANGE_NO_RPR`
   This effect is chosen when the composition data changes and the device doesn't support the remote provisioning. The new composition data takes place only after re-provisioning.
:c:enum:`BT_MESH_DFU_EFFECT_COMP_CHANGE`
   This effect is chosen when the composition data changes and the device supports the remote provisioning. In this case, the device stays provisioned and the new composition data takes place after re-provisioning using the Remote Provisioning models.
:c:enum:`BT_MESH_DFU_EFFECT_UNPROV`
  This effect is chosen if the composition data in the new firmware changes, the device doesn't support the remote provisioning, and the new composition data takes effect after applying the firmware.

When the Target node receives the Firmware Update Metadata Check message, the DFU Server model calls the :c:member:`bt_mesh_dfu_srv_cb.check` callback, the application can then process the metadata and provide the effect value.


DFU procedures
**************

The DFU protocol is implemented as a set of procedures that must be performed in a certain order.

The Initiator controls the Upload stage of the DFU protocol, and all Distributor side handling of the upload subprocedures is implemented in the :ref:`bluetooth_mesh_dfd_srv`.

The Distribution stage is controlled by the Distributor, as implemented by the :ref:`bluetooth_mesh_dfu_cli`. The Target node implements all handling of these procedures in the :ref:`bluetooth_mesh_dfu_srv`, and notifies the application through a set of callbacks.

Uploading the firmware
======================

The upload firmware procedure uses the :ref:`bluetooth_mesh_blob` to transfer the firmware image from the Initiator to the Distributor. The Upload procedure works in two steps:

1. The Initiator generates a BLOB ID, and sends it to the Distributor's DFD Server, along with the firmware information and other input parameters of the BLOB transfer. The DFD server stores the information, and prepares its BLOB Server for the incoming transfer before it responds with a status message to the Initiator.
#. The Initiator's BLOB Client model transfers the firmware image to the Distributor's BLOB Server, which stores the image in a predetermined flash partition.

When the BLOB transfer finishes, the firmware image is ready for distribution. The Initiator may upload several firmware images to the Distributor, and ask it to distribute them in any order or at any time. Additional procedures are available for querying and deleting firmware images from the Distributor.

The following Distributor's capabilities related to firmware images can be configured using the configuration options:

* :kconfig:option:`CONFIG_BT_MESH_DFU_SLOT_CNT`: Amount of image slots available on the device.
* :kconfig:option:`CONFIG_BT_MESH_DFD_SRV_SLOT_MAX_SIZE`: Maximum allowed size for each image.
* :kconfig:option:`CONFIG_BT_MESH_DFD_SRV_SLOT_SPACE`: Available space for all images.

Populating the Distributor's target list
========================================

Before the Distributor can start distributing the firmware image, it needs a list of Target nodes to send the image to. The Initiator gets the full list of Target nodes either by querying the potential targets directly, or through some external authority. The Initiator uses this information to populate the Distributor's target list with the address and relevant firmware image index of each Target. The Initiator may send one or more "Distribution Receivers Add" messages to build the Distributor's target list, and "Distribution Receivers Delete All" to clear it.

The maximum number of receivers that can be added to the Distributor is configured through the :kconfig:option:`CONFIG_BT_MESH_DFD_SRV_TARGETS_MAX` configuration option.

Initiating the distribution
===========================

Once the Distributor has stored a firmware image and received a list of Targets, the Initiator may initiate the distribution procedure. The BLOB transfer parameters for the distribution are passed to the Distributor along with an upgrade policy. The upgrade policy decides whether the Distributor should request that the firmware is applied on the Target nodes or not. The Distributor stores the transfer parameters and starts distributing the firmware image to its list of Targets.

Firmware distribution
---------------------

The Distributor's DFU Client model uses its BLOB Client model's broadcast subsystem to communicate with all Target nodes. The firmware distribution is performed with the following steps:

1. The Distributor's DFU Client model generates a BLOB ID and sends it to each Target's DFU Server model, along with the other BLOB transfer parameters, the Target firmware image index and the firmware image Metadata. Each Target performs a metadata check and prepares their BLOB Server model for the transfer, before sending a status response to the DFU Client, indicating if the firmware upgrade will have any effect on the Bluetooth mesh state of the node.
#. The Distributor's BLOB Client model transfers the firmware image to all Target nodes.
#. Once the BLOB transfer has been received in full, the Target nodes' applications verify that the firmware is valid by performing checks such as signature verification or image checksums against the image metadata.
#. The Distributor's DFU Client model queries all Target nodes to ensure that they've all verified the firmware image.

If the distribution procedure completed with at least one Target node reporting that the image has been received and verified, the distribution procedure is considered successful.

.. note::
   The firmware distribution procedure only fails if *all* Targets are lost. It is up to the Initiator to request a list of failed Targets from the Distributor and initiate additional attempts to update the lost Targets after the current attempt is finished.

Suspending the distribution
---------------------------

The Initiator can also request the Distributor to suspend the firmware distribution. In this case, the Distributor will stop sending any messages to Target nodes. When the firmware distribution is resumed, the Distributor will continue sending the firmware from the last successfully transferred block.

Applying the firmware image
===========================

If the Initiator requested it, the Distributor can initiate the Apply firmware procedure on all Targets that successfully received and verified the firmware image. The Apply procedure takes no parameters, and to avoid ambiguity, it should be performed before a new transfer is initiated. The apply procedure consists of the following steps:

1. The Distributor's DFU Client model instructs all Target nodes that have verified the firmware image to apply it. The Target nodes' DFU Server models respond with a status message before calling their application's ``apply`` callback.
#. The Target's application performs any preparations needed before applying the transfer, such as storing a snapshot of the composition data or clearing its configuration.
#. The Target's application swaps the current firmware with the new image and updates its firmware image list with the new firmware ID.
#. The Distributor's DFU Client model requests the full list of firmware images from each Target, and scans through the list to make sure that the new firmware ID has replaced the old.

.. note::
   During the metadata check in the distribution procedure, the Target may have reported that it will become unprovisioned after the firmware image is applied. In this case, the Distributor's DFU Client model will send a request for the full firmware image list, and expect no response.

Cancelling the distribution
===========================

The firmware distribution can be cancelled at any time by the Initiator. In this case, the Distributor starts the cancelling procedure by sending a cancelling message to all Target nodes. The Distributor waits for the response from all Targets. Once all Target nodes have replied, or the request has timed out, the distribution procedure is cancelled. After this the distribution procedure can be started again from the ``Firmware distribution`` section.


.. _bluetooth_mesh_dfu_eval:

Evaluation
**********

The Bluetooth mesh subsystem provides a set of samples that can be used for evaluation of the Bluetooth mesh DFU specification and subsystem:

* :ref:`ble_mesh_dfu_distributor`
* :ref:`ble_mesh_dfu_target`

To configure and control the DFU procedure on the DFD server, it is required to have the DFD Client model. The Bluetooth mesh DFU subsystem in Zephyr provides a set of shell commands that can be used to substitute the need for the client. For the complete list of commands, see :ref:`bluetooth_mesh_shell_dfd_server`.

Provisioning and configuring the devices
========================================

After programming the samples onto the boards, they need to be provisioned into the same Bluetooth mesh network with an external provisioner device. After the provisioning is completed, a Configuration Client needs to add a common application key to all devices. The added application key must be bound:

* On the Distributor: to the DFD Server, DFU Client, BLOB Server and BLOB Client models instantiated on the primary element, and to the DFU Server and BLOB Server models instantiated on the secondary element of the device.
* On Target nodes: to the DFU Server and BLOB Server models instantiated on the primary element of the device.

The bound application key will be used in the firmware distribution procedure.

Uploading the firmware
======================

After configuring the models, a new image can be uploaded to the Distributor. To upload the image, follow the instructions provided in :ref:`ble_mesh_dfu_distributor`.

The uploaded image needs to be registered in the Bluetooth mesh DFU subsystem. To achieve this, issue the ``mesh dfu-slot-add`` shell command specifying size in bytes of the image that was uploaded to the Distributor. Optionally, you can provide firmware ID, metadata and Unique Resource Identifier (URI) parameters that come with the image. For example, to allocate a slot for :ref:`ble_mesh_dfu_target` sample with image size of 241236 bytes with firmware ID set to ``0200000000000000`` and metadata generated as described in ``Composing the firmware metadata`` section below, type the following command::

  mesh dfu-slot-add 241236 0200000000000000 020000000100000094cf24017c26f3710100

When the slot is added, the shell will print the slot ID. Take note of this ID as it will then be needed to start the DFU transfer::

  Adding slot (size: 241236)
  Slot added. ID: 0

.. note::
   To update any value in a slot, issue the ``mesh dfu-slot-del`` command specifying the ID of the allocated slot. Then add the slot again.

Populating the Distributor's target list
========================================

Add Target nodes to the DFU transfer by issuing the ``mesh dfd-receivers-add`` shell command, specifying the element address of a Target node with the DFU server instance and the image index on the Target node that needs to be updated. For example, for two Target nodes with addresses ``0x0004`` and ``0x0005`` respectively, and with image index 0::

  mesh dfd-receivers-add 0x0004,0;0x0005,0

.. note::
   To remove all receivers from the list, issue the ``mesh dfd-receivers-delete-all`` command.

Initiating the distribution
===========================

To start the DFU transfer, issue the ``mesh dfd-start`` shell command. This command requires two mandatory arguments: ``app_idx`` and ``slot_idx``:

* As ``app_idx``, use the application key index that is bound to the DFD Server and other DFU and BLOB models on the Distributor and Target nodes.
* As ``slot_idx``, use the ID of the slot allocated by the ``mesh dfu-slot-add`` shell command on the previous step.

For example, to run the DFU transfer in unicast mode, with AppKey index 0 and slot ID 0, call::

  mesh dfd-start 0 0

By default, the DFD server will request the DFU servers to apply the image immediately after the DFU transfer. To avoid applying the image immediately and only verify it, set the 4th argument to 0::

  mesh dfd-start 0 0 0 0

Firmware distribution
---------------------

The transfer will take a couple of minutes, depending on the number of Target nodes and the network quality. To check the transfer progress, call the ``mesh dfd-receivers-get`` shell command, for example::

  mesh dfd-receivers-get 0 2

The output may look like this::

  {
          "target_cnt": 1,
          "targets": {
                  "0": { "blob_addr": 0x0004, "phase": 2, "status": 0, "blob_status": 0, "progress": 50, "img_idx": 0 }
                  "1": { "blob_addr": 0x0005, "phase": 2, "status": 0, "blob_status": 0, "progress": 50, "img_idx": 0 }
          }
  }

To see the distribution status, phase and parameters of the DFU transfer, use the ``mesh dfd-get`` command. When the DFU transfer successfully completes, the phase will be set to  :c:enum:`BT_MESH_DFD_PHASE_TRANSFER_SUCCESS`, for example::

  { "status": 0, "phase": 2, "group": 0x0000, "app_idx": 0, "ttl": 255, "timeout_base": 0, "xfer_mode": 1, "apply": 0, "slot_idx": 0 }

The :c:enum:`bt_mesh_dfd_phase` enumeration contains the complete list of distribution phases.

Suspending the distribution
---------------------------

The firmware distribution can be suspended using the ``mesh dfd-suspend`` shell command. The distribution phase is switched to :c:enum:`BT_MESH_DFD_PHASE_TRANSFER_SUSPENDED` in this case.

To resume the DFU transfer, issue the ``mesh dfd-resume`` shell command.

Applying the firmware image
===========================

Depending on the upgrade policy set at the start of the DFU transfer, the DFD server will do the following:

* If ``policy_apply`` is set to true or omitted when the DFU transfer starts, the DFD server will immediately apply the new firmware on the Target nodes upon the DFU transfer completion.
* If ``policy_apply`` is set to false, the image needs to be applied manually using the ``mesh dfd-apply`` command once the DFU transfer is completed.

When the DFD server starts applying the transferred image, the distribution phase is set to :c:enum:`BT_MESH_DFD_PHASE_APPLYING_UPDATE`.

After applying the new firmware, the DFD server will immediately request firmware ID of the currently running firmware on the Target nodes to confirm that the new firmware has been applied successfully. Depending on the :c:enum:`bt_mesh_dfu_effect` value received from the Target nodes after the DFU transfer is started, the following cases are possible:

* If the image effect for a particular Target node is :c:enum:`BT_MESH_DFU_EFFECT_UNPROV`, the DFD server doesn't expect any reply from that Target node. If the Distributor doesn't receive any reply, it will repeat the request several times. If the Distributor eventually receives a reply, the DFU procedure for this particular Target node is considered unsuccessful. Otherwise, the DFU procedure is considered successful.
* In all other cases, the Distributor expects a reply from the Target node with the firmware ID equal to the firmware ID of the transferred image. If the Target node responds with a different firmware ID or doesn't respond at all after several requests, the DFU procedure for this particular Target node is considered unsuccessful. Otherwise, the DFU procedure is considered successful.

The DFU procedure ends after the Distributor stops polling the Target nodes. If the DFU procedure completes successfully for at least one Target node, the firmware distribution is considered as successful. In this case, the distribution phase is set to :c:enum:`BT_MESH_DFD_PHASE_COMPLETED`. If the DFU procedure doesn't complete successfully, the distribution phase is set to :c:enum:`BT_MESH_DFD_PHASE_FAILED`.

Cancelling the distribution
===========================

To cancel the firmware distribution procedure, use the ``mesh dfd-cancel`` shell command. The DFD server will start the cancelling procedure by sending a cancel message to all Targets and will switch phase to :c:enum:`BT_MESH_DFD_PHASE_CANCELING_UPDATE`. Once the cancelling procedure is completed, the phase is set to :c:enum:`BT_MESH_DFD_PHASE_IDLE`.

Recovering from failed distribution
===================================

If the firmware distribution fails for any reason, the list of Target nodes should be cleared and the distribution phase should be set to :c:enum:`BT_MESH_DFD_PHASE_IDLE` before making a new attempt. To do this, run the following shell commands::

  mesh dfd-receivers-delete-all
  mesh dfd-cancel

.. note::
   This does not affect the allocated image slots.

.. _bluetooth_mesh_dfu_eval_md:

Composing the firmware metadata
===============================

The Bluetooth mesh DFU subsystem provides a set of shell commands that can be used to compose a firmware metadata. The format of metadata is defined in the :c:struct:`bt_mesh_dfu_metadata` structure. For the complete list of commands, see :ref:`bluetooth_mesh_shell_dfu_metadata`.

To start composing metadata, issue the ``mesh dfu-comp-add`` shell command that encodes a Composition Data header. For example, for a Target node with with product ID 0x0059, company and version IDs zero, number of entries in the replay list 10 and Relay, Proxy and Friend features enabled, the command will be the following::

  mesh dfu-comp-add 0x59 0 0 10 7

Now you need to encode elements the are present on a new image. For each element to encode, issue the ``mesh dfu-comp-elem-add`` shell command specifying the location of the element, number of Bluetooth SIG and vendor models and their IDs. For example, for :ref:`ble_mesh_dfu_target` sample, which has only one element containing Configuration and Health Server models as well as DFU and BLOB Server models, the command will be the following::

  mesh dfu-comp-elem-add 1 4 0 0x0000 0x0002 0xBF42 0xBF44

.. note::
   In case of any mistakes being done during the enconding of the Composition Data, use the ``mesh dfu-comp-clear`` command to clear the cached value, then start composing the metadata from the beginning.

When all elements are added, generate a hash of the Composition Data using the ``mesh dfu-comp-hash-get`` shell command. For example, using the inputs from the commands above, the output of this command should be the following::

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

The generated hash will then be encoded into the metadata. Use the ``mesh dfu-metadata-encode`` shell command to encode the metadata. For example, using the Composition Data hash generated above, the command to encode the metadata for firmware version ``2.0.0+0``, with a size of 241236 bytes and targeted to application core, will be the following::

  mesh dfu-metadata-encode 2 0 0 0 241236 1 0x71f3267c 1

The output of the command will be the following::

  Metadata to be encoded:
          Version: 2.0.0+0
          Size: 241236
          Core Type: 0x1
          Composition data hash: 0x71f3267c
          Elements: 1
          User data length: 0
  Encoded metadata: 020000000100000094cf24017c26f3710100

API reference
*************

This section lists the types common to the Device Firmware Upgrade mesh models.

.. doxygengroup:: bt_mesh_dfu
   :project: Zephyr
   :members:

.. doxygengroup:: bt_mesh_dfu_metadata
   :project: Zephyr
   :members:
