.. _bluetooth_mesh_blob:

BLOB models
###########

The Binary Large Object (BLOB) models provide functionality for sending large binary objects from a single source to many targets over the mesh. It is the underlying transport method for the :ref:`bluetooth_mesh_dfu`, but may be used for other object transfer purposes.

The BLOB models support transfers of continuous binary objects of up to 4 GB (2\ :sup:`32` bytes). The BLOB transfer protocol has built-in recovery procedures for packet losses, and sets up checkpoints to ensure that all targets have received all the data before moving on. Data transfer order is not guaranteed.

BLOB Transfers are constrained by the transfer speed and reliability of the underlying Mesh network. Under ideal conditions, the BLOBs can be transferred at a rate of up to 1 kbps, allowing a 100 kB BLOB to be transferred in 10-15 minutes. However, network conditions, transfer capabilities and other limiting factors can easily degrade the data rate by several orders of magnitude. Tuning the parameters of the transfer according to the application and network configuration, as well as scheduling it to periods with low network traffic will offer significant improvements on the speed and reliability of the protocol, but achieving transfer rates close to the ideal rate is unlikely in actual deployments.

There are two BLOB models:

.. toctree::
   :maxdepth: 1

   blob_srv
   blob_cli

The BLOB Client is instantiated on the sender node, and the BLOB Server is instantiated on the receiver nodes.

Concepts
********

The BLOB transfer protocol introduces several new concepts to implement the BLOB transfer.


BLOBs
=====

BLOBs are binary objects up to 4 GB in size, that can contain any data the application would like to transfer through the mesh network. The BLOBs are continuous data objects, divided into blocks and chunks to make the transfers reliable and easy to process. No limitations are put on the contents or structure of the BLOB, and applications are free to define any encoding or compression they'd like on the data itself.

The BLOB transfer protocol does not provide any built-in integrity checks, encryption or authentication of the BLOB data. However, the underlying encryption of the Bluetooth Mesh protocol provides data integrity checks and protects the contents of the BLOB from third parties using network and application level encryption.

Blocks
------

The binary objects are divided into blocks, typically from a few hundred to several thousand bytes in size. Each block is transmitted separately, and the BLOB Client ensures that all BLOB Servers have received the full block before moving on to the next. The block size is determined by the transfer's ``block_size_log`` parameter, and is the same for all blocks in the transfer except the last, which may be smaller. For BLOB transfers stored in flash, the block size is typically a multiple of the flash page size of the target devices.

Chunks
------

Each block is divided into chunks. A chunk is the smallest data unit in the BLOB transfer, and must fit inside a single Bluetooth Mesh SDU (377 bytes or less). The mechanism for transferring chunks depends on the transfer mode.

When operating in *push mode*, the chunks are sent as unacknowledged packets from the BLOB Client to all targeted BLOB Servers. Once all chunks in a block have been sent, the BLOB Client asks each BLOB Server if they're missing any chunks, and resends them. This is repeated until all BLOB Servers have received all chunks, or the BLOB Client gives up.

When operating in *pull mode*, the BLOB Server will request a small number of chunks from the BLOB Client at a time, and wait for the BLOB Client to send them before requesting more chunks. This repeats until all chunks have been transferred, or the BLOB Server gives up.

.. _bluetooth_mesh_blob_stream:

BLOB streams
============

In the BLOB models' APIs, the BLOB data handling is separated from the high-level transfer handling. This split allows reuse of different BLOB storage and transfer strategies for different applications. While the high level transfer is controlled directly by the application, the BLOB data itself is accessed through a *BLOB stream*.

The BLOB stream is comparable to a standard library file stream. Through opening, closing, reading and writing, the BLOB model gets full access to the BLOB data, whether it's kept in flash, RAM, or on a peripheral. The BLOB stream is opened with an access mode before it's used, and the BLOB models will move around inside the BLOB's data in blocks and chunks, using the BLOB stream as an interface.

Interaction
-----------

Before the BLOB is read or written, the stream is opened by calling its :cpp:member:`open <bt_mesh_blob_io::open>` callback. When used with a BLOB Server, the BLOB stream is always opened in write mode, and when used with a BLOB Client, it's always opened in read mode.

For each block in the BLOB, the BLOB model starts by calling :cpp:member:`block_start <bt_mesh_blob_io::block_start>`. Then, depending on the mode, the BLOB stream's :cpp:member:`rd <bt_mesh_blob_io::rd>` or :cpp:member:`wr <bt_mesh_blob_io::wr>` callback is called repeatedly to move data to or from the BLOB. When the model is done processing the block, it calls :cpp:member:`block_end <bt_mesh_blob_io::block_end>`. When the transfer is complete, the BLOB stream is closed by calling :cpp:member:`close <bt_mesh_blob_io::close>`.

Implementations
---------------

The application may implement their own BLOB stream, or use the implementations provided by Zephyr:

.. toctree::
   :maxdepth: 2

   blob_flash


Transfer capabilities
=====================

Each BLOB Server may have different transfer capabilities. The transfer capabilities of each device is controlled through the following configuration options:

* :kconfig:option:`CONFIG_BT_MESH_BLOB_SIZE_MAX`
* :kconfig:option:`CONFIG_BT_MESH_BLOB_BLOCK_SIZE_MIN`
* :kconfig:option:`CONFIG_BT_MESH_BLOB_BLOCK_SIZE_MAX`
* :kconfig:option:`CONFIG_BT_MESH_BLOB_CHUNK_COUNT_MAX`

The :kconfig:option:`CONFIG_BT_MESH_BLOB_CHUNK_COUNT_MAX` option is also used by the BLOB Client and affects memory consumption by the BLOB Client model structure.

To ensure that the transfer can be received by as many Servers as possible, the BLOB Client can retrieve the capabilities of each BLOB Server before starting the transfer. The Client will transfer the BLOB with the highest possible block and chunk size.


Transfer modes
==============

BLOBs can be transferred using two transfer modes, push and pull.
In most cases, the transfer should be conducted in push mode.

In push mode, the send rate is controlled by the BLOB Client, which will push all the chunks of each block without any high level flow control. Push mode supports any number of targets, and should be the default transfer mode.

In pull mode, the BLOB Server will "pull" the chunks from the BLOB Client at its own rate. Pull mode can only be conducted with a single target, and is intended for transferring BLOBs to targets acting as :ref:`bluetooth_mesh_lpn`. When operating in pull mode, the BLOB Server will request chunks from the BLOB Client in small batches, and wait for them all to arrive before requesting more chunks. This process is repeated until the BLOB Server has received all chunks in a block. Then, the BLOB Client starts the next block, and the BLOB Server requests all chunks of that block.


.. _bluetooth_mesh_blob_timeout:

Transfer timeout
================

The timeout of the BLOB transfer is based on a Timeout Base value. Both client and server use the same Timeout Base value, but they calculate timeout differently.

The BLOB server uses the following formula to calculate the BLOB transfer timeout::

  10 * (Timeout Base + 1) seconds


For the BLOB client, the following formula is used::

  (10000 * (Timeout Base + 2)) + (100 * TTL) milliseconds

where TTL is Time-To-Live value set in the transfer.

API reference
*************

This section contains types and defines common to the BLOB models

.. doxygengroup:: bt_mesh_blob
   :project: Zephyr
   :members:
