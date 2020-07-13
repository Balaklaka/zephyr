.. _bluetooth_mesh_dfd_srv:

Device Firmware Distribution Server
###################################

The Device Firmware Distribution (DFD) Server model implements the Firmware Distributor role for the :ref:`bluetooth_mesh_dfu` subsystem. It extends the :ref:`bluetooth_mesh_blob_srv`, which it uses to receive the firmware image binary from the Initiator node. It also instantiates a :ref:`bluetooth_mesh_dfu_cli`, which it uses to distribute firmware updates throughout the mesh.

.. note::

   The DFD Server does not support out of band retrieval of firmware images.

The DFD Server does not have an API of its own, but relies on a DFD Client model on a different device to give it information and trigger image distribution and upload.

Firmware slots
**************

The DFD Server is capable of storing multiple firmware images for distribution. Each slot contains a separate firmware image with metadata, and can be distributed to other mesh nodes in the network in any order. The contents, format and size of the firmware images is vendor specific, and may contain data from other vendors. The DFD Server should never attempt to execute or modify them.

The slots are managed remotely by a DFD Client, which can both upload new slots and delete old ones. The application is notified of changes to the slots through the DFD Server's callbacks (:cpp:type:`bt_mesh_dfd_srv_cb`). While the metadata for each firmware slot is stored internally, the application must provide a :ref:`bluetooth_mesh_blob_stream` for reading and writing the firmware image.

API reference
*************

.. doxygengroup:: bt_mesh_dfd_srv
   :project: Zephyr
   :members:
