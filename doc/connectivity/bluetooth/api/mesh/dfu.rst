.. _bluetooth_mesh_dfu:

Device Firmware Upgrade (DFU)
#############################

Bluetooth mesh supports the distribution of firmware images across a mesh network. The the Bluetooth mesh DFU subsystem implements the Firmware update section of the Bluetooth Mesh Model Specification v1.1.

Bluetooth mesh DFU implements a distribution mechanism for firmware images, and does not put any restrictions on the size, format or usage of the images. The primary design goal of the subsystem is to provide the qualifiable parts of the Bluetooth mesh DFU specification, and leave the usage, firmware validation and deployment to the application.

The DFU specification is implemented as three separate models:

.. toctree::
   :maxdepth: 1

   dfu_srv
   dfu_cli
   dfd_srv

Overview
********

The Bluetooth mesh DFU process is designed to act in three stages:

The Upload stage
   First, the image is uploaded to a Distributor in a mesh network by an external entity, such as a phone or gateway (the Initiator). During the Upload stage, the Initiator transfers the firmware image and all its metadata to the Distributor node inside the mesh network. The Distributor stores the firmware image and its metadata persistently, and awaits further instructions from the Initiator. The time required to complete the upload process depends on the size of the image. After the upload completes, the Initiator can disconnect from the network during the much more-time consuming Distribution stage. Once the firmware has been uploaded to the Distributor, the Initiator may trigger the Distribution stage at any time.

The Firmware capabilities check stage
  Before starting the distribution phase, the Initiator may optionally check if Target nodes can accept the new firmware. Nodes that didn't respond or responded that they can't receive the new firmware will be excluded from the firmware distribution process.

The Distribution stage
   Before the firmware image can be distributed, the Initiator transfers the list of Target nodes and their designated firmware image index to the Distributor. Next, it tells the Distributor to start the firmware distributon process, which runs in the background while the Initiator and mesh network performs other duties. Once the firmware image has been transferred to the Target nodes, the Distributor may ask them to apply the firmware image immediately, and report back with their status and new firmware IDs.

DFU roles
*********

The Bluetooth mesh DFU subsystem defines three different roles the mesh nodes have to assume in the distribution of firmware images:

Firmware upgrade Target (Updating node)
   The Target is the receiver and user of the transferred firmware images. All its functionality is implemented by the :ref:`bluetooth_mesh_dfu_srv` model. A transfer may have any number of targets, and they will all be updated concurrently.

Firmware upgrade Distributor
   The Distributor role serves two purposes in the DFU process. First, it's the Target of the upload procedure, then it distributes the uploaded image to the Target nodes. The Distributor does not select the parameters of the transfer, but relies on an Initiator to give it a list of Target nodes and transfer parameters. The Distributor functionality is implemented in two models, :ref:`bluetooth_mesh_dfd_srv` and :ref:`bluetooth_mesh_dfu_cli`. The :ref:`bluetooth_mesh_dfd_srv` is responsible for communicating with the Initiator, and the :ref:`bluetooth_mesh_dfu_cli` is responsible for distributing the image to the Target nodes.

Firmware upgrade Initiator
   The Initiator role is typically implemented by the same device that implements the Bluetooth mesh :ref:`Provisioner <bluetooth_mesh_provisioning>` and :ref:`Configurator <bluetooth_mesh_models_cfg_cli>` roles. The Initiator needs a full overview of the potential Target nodes and their firmware, and will control (and initiate) all firmware upgrades. The Initiator role is not implemented in the Zephyr Bluetooth mesh DFU subsystem.

Bluetooth mesh applications may combine the DFU roles in any way they'd like, and even take on multiple instances of the same role by instantiating the models on separate elements. For instance, the Distributor and Initiator role can be combined by instantiating the :ref:`bluetooth_mesh_dfu_cli` on the Initiator node and calling its API directly.

It's also possible to replace the Initiator and the Distribution Server model on the Distributor with a proprietary mechanism, such as a serial protocol.

.. note::
   All the DFU models instantiate one or more :ref:`bluetooth_mesh_blob`, and may need to be spread over multiple elements for certain role combinations.

Firmware images
***************

All upgradable parts of a mesh node's firmware should be represented as a firmware image. Each Target node holds a list of firmware images, each of which should be independently upgradeable and identifiable.

Firmware images are represented as a binary blob (the firmware itself) with the following additional information attached to it:

Firmware ID
   The firmware ID is used to identify a firmware image. The Initiator node may ask the Target nodes for a list of its current firmware IDs to determine whether a newer version of the firmware is available. The format of the firmware ID is vendor specific, but generally, it should include enough information for an Initiator node with knowledge of the format to determine the type of image as well as its version. The firmware ID is optional, and its max length is determined by :kconfig:option:`CONFIG_BT_MESH_DFU_FWID_MAXLEN`.

Firmware metadata
   The firmware metadata is used by the Target node to determine whether it should accept an incoming firmware upgrade, and what the effect of the upgrade would be. The metadata format is vendor specific, and should contain all information the Target node needs to verify the image, as well as any preparation the Target node has to make before the image is applied. Typical metadata information can be image signatures, changes to the node's composition data and the format of the binary blob. The Target node may perform a metadata check before accepting incoming transfers to determine whether the transfer should be started. The firmware metadata can be discarded by the target after the metadata check, as other nodes will never request the metadata from the Target node. The firmware metadata is optional, and its max length is determined by :kconfig:option:`CONFIG_BT_MESH_DFU_METADATA_MAXLEN`.

   The Bluetooth mesh DFU subsystem in Zephyr :c:struct:`provides <bt_mesh_dfu_metadata>` its own metadata format together with a set of related functions that can be used by an end product. The support for it can be enabled using :kconfig:option:`CONFIG_BT_MESH_DFU_METADATA`. The format of the metadata is the following:

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
   The firmware URI gives the Initiator information about where firmware upgrades for the image can be found. The URI shall point to an online resource the Initiator can interact with to get new versions of the firmware. This allows Initiators to perform upgrades for any node in the mesh network by interacting with the web server pointed to in the URI. The URI must point to a resource using the ``http`` or ``https`` schemes, and the targeted web server must behave according to the Check Firmware OOB procedure defined by the Bluetooth mesh model specification. The firmware URI is optional, and its max length is determined by :kconfig:option:`CONFIG_BT_MESH_DFU_URI_MAXLEN`.

   .. note::

      The out-of-band distribution mechanism is not supported.


DFU Procedure
*************

The DFU protocol is implemented as a set of procedures that must be performed in a certain order.

The Initiator controls the Upload stage of the DFU protocol, and all Distributor side handling of the upload subprocedures is implemented in the :ref:`bluetooth_mesh_dfd_srv`.

The Distribution stage is controlled by the Distributor, as implemented by the :ref:`bluetooth_mesh_dfu_cli`. The Target node implements all handling of these procedures in the :ref:`bluetooth_mesh_dfu_srv`, and notifies the application through a set of callbacks.

Upload firmware
===============

The upload firmware procedure uses the :ref:`bluetooth_mesh_blob` to transfer the firmware image from the Initiator to the Distributor. The Upload procedure works in two steps:

1. The Initiator generates a BLOB ID, and sends it to the Distributor's DFD Server, along with the firmware information and other input parameters of the BLOB transfer. The DFD server stores the information, and prepares its BLOB Server for the incoming transfer before it responds with a status message to the Initiator.
#. The Initiator's BLOB Client model transfers the firmware image to the Distributor's BLOB Server, which stores the image in a predetermined flash partition.

When the BLOB transfer finishes, the firmware image is ready for distribution. The Initiator may upload several firmware images to the Distributor, and ask it to distribute them in any order or at any time. Additional procedures are available for querying and deleting firmware images from the Distributor.

Populate the Distributor's target list
======================================

Before the Distributor can start distributing the firmware image, it needs a list of Target nodes to send the image to. The Initiator gets the full list of Target nodes either by querying the potential targets directly, or through some external authority. The Initiator uses this information to populate the Distributor's target list with the address and relevant firmware image index of each target. The Initiator may send one or more "Distribution Receivers Add" messages to build the Distributor's target list, and "Distribution Receivers Delete All" to clear it.

Initiating distribution
=======================

Once the Distributor has stored a firmware image and received a list of targets, the Initiator may initiate the distribution procedure. The BLOB transfer parameters for the distribution are passed to the Distributor along with an upgrade policy. The upgrade policy decides whether the Distributor should request that the firmware is applied on the Target nodes or not. The Distributor stores the transfer parameters and starts distributing the firmware image to its list of targets.

Firmware distribution
=====================

The Distributor's DFU Client model uses its BLOB Client model's broadcast subsystem to communicate with all Target nodes. The firmware distribution is performed with the following steps:

1. The Distributor's DFU Client model generates a BLOB ID and sends it to each target's DFU Server model, along with the other BLOB transfer parameters, the target firmware image index and the firmware image Metadata. Each target performs a metadata check and prepares their BLOB Server model for the transfer, before sending a status response to the DFU Client, indicating if the firmware upgrade will have any effect on the Bluetooth mesh state of the node.
#. The Distributor's BLOB Client model transfers the firmware image to all Target nodes.
#. Once the BLOB transfer has been received in full, the Target nodes' applications verify that the firmware is valid by performing checks such as signature verification or image checksums against the image metadata.
#. The Distributor's DFU Client model queries all Target nodes to ensure that they've all verified the firmware image.

If the distribution procedure completed with at least one Target node reporting that the image has been received and verified, the distribution procedure is considered successful.

.. note::
   The firmware distribution procedure only fails if *all* targets are lost. It is up to the Initiator to request a list of failed targets from the Distributor and initiate additional attempts to update the lost targets.

Applying the firmware image
===========================

If the Initiator requested it, the Distributor can initiate the Apply firmware procedure on all targets that successfully received and verified the firmware image. The Apply procedure takes no parameters, and to avoid ambiguity, it should be performed before a new transfer is initiated. The apply procedure consists of the following steps:

1. The Distributor's DFU Client model instructs all Target nodes that have verified the firmware image to apply it. The Target nodes' DFU Server models respond with a status message before calling their application's ``apply`` callback.
#. The target application performs any preparations needed before applying the transfer, such as storing a snapshot of the composition data or clearing its configuration.
#. The target application swaps the current firmware with the new image and updates its firmware image list with the new firmware ID.
#. The Distributor's DFU Client model requests the full list of firmware images from each target, and scans through the list to make sure that the new firmware ID has replaced the old.

.. note::
   During the metadata check in the distribution procedure, the target may have reported that it will become unprovisioned after the firmware image is applied. In this case, the Distributor's DFU Client model will send a request for the full firmware image list, and expect no response.

Evaluation
**********

The :ref:`bluetooth_mesh_shell` module contains a demo implementation of the Bluetooth mesh DFU feature, and can be used for evaluation of basic DFU operation. The shell module does not transfer any actual firmware, but demonstrates the DFU transport capability by transferring random data from a Distributor to up to 32 Target nodes.

To enable the DFU feature in the shell, build the mesh shell application (tests/bluetooth/mesh_shell) with the following additional configuration values enabled:

.. code-block::

   CONFIG_BT_MESH_BLOB_SRV=y
   CONFIG_BT_MESH_BLOB_CLI=y
   CONFIG_BT_MESH_DFU_SRV=y
   CONFIG_BT_MESH_DFU_CLI=y

Two or more boards are needed to perform a DFU transfer, where one will act as the Firmware upgrade Distributor, and one or more devices will act as Firmware upgrade Targets.

After flashing the shell application to the boards and performing initial initialization, provision the Distributor with address ``0x0001`` and the Targets with addresses ``0x0002`` and up (as described in the :ref:`shell documentation <bluetooth_mesh_shell>`). Next, each device must be configured with the same application key, using ``mesh app-key-add 0 0``. Finally, the BLOB Server, BLOB Client, DFU Server and DFU Client models must be bound to a common application key, using the ``mesh mod-app-bind`` command.

Once the models have been configured, issue the following command on the Distributor device to allocate a DFU image slot of 4096 bytes::

   mesh dfu-slot-add 4096

Then, add register each Target node as a DFU target by issuing the following command on the Distributor::

   mesh dfu-target <target-addr> 0

Where ``<target-addr>`` is the unicast address of each Target node (e.g. ``0x0002``), and ``0`` represents "Image 0" on the target, which would typically be the application.

To check that a DFU Target node is reachable from the Distributor, call::

   mesh appidx 0
   mesh dst <target-addr>
   mesh dfu-target-state

If the target is operational, it should report its status and phase as ``0``.

Next, to start the DFU transfer from the Distributor to the registered Target nodes, call::

   mesh dfu-send 0

where ``0`` is the DFU image slot we previously allocated with ``mesh-dfu-slot-add``.

The transfer will take a couple of minutes, depending on the number of Target nodes and network quality. To check the transfer progress, call::

   mesh dfu-progress

on the Distributor or Target nodes.

Once the transfer ends, the Distributor will print ``DFU ended: <status>``, where a ``status`` of ``0`` indicates success. Other status codes are listed in :c:enum:`bt_mesh_dfu_status`.

Finally, to apply the image on the Target nodes, call the following command on the Distributor::

   mesh dfu-apply

In a real application, this would cause the Target nodes to install the new firmware image and reboot. In the shell application, the Target nodes will just print "Applying DFU transfer..." and go back to their initial state.

API reference
*************

This section lists the types common to the Device Firmware Upgrade mesh models.

.. doxygengroup:: bt_mesh_dfu
   :project: Zephyr
   :members:

.. doxygengroup:: bt_mesh_dfu_metadata
   :project: Zephyr
   :members:
