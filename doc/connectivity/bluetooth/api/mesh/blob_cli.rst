.. _bluetooth_mesh_blob_cli:

BLOB Client model
#################

The Binary Large Object (BLOB) Client is the sender of the BLOB transfer. It supports sending BLOBs of any size to any number of targets, in both push and pull mode.

Usage
*****

Initialization
==============

The BLOB Client is instantiated on an element with a set of event handler callbacks:

.. code-block:: C

   static const struct bt_mesh_blob_cli_cb blob_cb = {
         /* Callbacks */
   };

   static struct bt_mesh_blob_cli blob_cli = {
         .cb = &blob_cb,
   };

   static struct bt_mesh_model models[] = {
         BT_MESH_MODEL_BLOB_CLI(&blob_cli),
   };

Transfer context
================

Both the transfer capabilities retrieval procedure and the BLOB transfer uses an instance of a :cpp:type:`bt_mesh_blob_cli_inputs` to determine how to perform the transfer. The BLOB Client Inputs structure must at least be initialized with a list of targets, an application key and a TTL value before it is used in a procedure:

.. code-block:: c

   static struct bt_mesh_blob_target targets[3] = {
           { .addr = 0x0001 },
           { .addr = 0x0002 },
           { .addr = 0x0003 },
   };
   static struct bt_mesh_blob_cli_inputs inputs = {
           .app_idx = MY_APP_IDX,
           .ttl = BT_MESH_TTL_DEFAULT,
   };

   sys_slist_init(&inputs.targets);
   sys_slist_append(&inputs.targets, &targets[0].n);
   sys_slist_append(&inputs.targets, &targets[1].n);
   sys_slist_append(&inputs.targets, &targets[2].n);

Note that all BLOB Servers in the transfer must be bound to the chosen application key.


Group address
-------------

The application may additionally specify a group address in the context structure. If the group is not :c:macro:`BT_MESH_ADDR_UNASSIGNED`, the messages in the transfer will be sent to the group address, instead of being sent individually to each target node. All target nodes must ensure that their BLOB Server model subscribes to this group address.

Using group addresses for transferring the BLOBs can generally increase the transfer speed, as the BLOB Client sends each message to all targets at the same time. However, sending messages to group addresses in Bluetooth Mesh is generally less reliable than sending to unicast addresses, as there is no transport level acknowledgment mechanism for groups. This can lead to longer recovery periods at the end of each block, and increases the risk of losing targets. Using group addresses for BLOB transfers will generally only pay off if the target list is extensive, and the effectiveness of each addressing strategy will vary heavily between different deployments.

Transfer timeout
----------------

If a target node fails to respond to an acknowledged message within the BLOB Client's time limit, the target node is dropped from the transfer. The application can reduce the chances of this by giving the BLOB Client extra time through the context structure. The extra time may be set in 10 second increments, up to 182 hours, in addition to the base time of 20 seconds. The wait time scales automatically with the transfer TTL.

Note that the BLOB Client will only move forwards with the transfer if all target nodes have responded or the Client timed out. If just one of the target nodes is made permanently unavailable, the transfer will be blocked until the BLOB Client times out. Increasing the wait time will increase this delay.

Transfer capabilities retrieval
===============================

It is generally recommended to retrieve transfer capabilities before starting a transfer. The procedure populates the transfer capabilities from all targets with the most liberal set of parameters that allows all target nodes to participate in the transfer. Any targets that fail to respond or responds with incompatible transfer parameters will be dropped.

Target nodes are prioritized according to their order in the target list. If a target node is found to be incompatible with any of the nodes before it, for instance by reporting a non-overlapping block size range, it will be dropped. Lost targets will be reported through the :cpp:member:`lost_target <bt_mesh_blob_cli_cb::lost_target>` callback.

The end of the procedure is signalled through the :cpp:member:`caps <bt_mesh_blob_cli_cb::caps>` callback, and the resulting capabilities can be used to determine the block and chunk sizes required for the BLOB transfer.

BLOB Transfer
=============

The BLOB Transfer is started by calling :cpp:func:`bt_mesh_blob_cli_send` function, which (in addition to the aforementioned transfer inputs) requires a set of transfer parameters and a BLOB stream instance. The transfer parameters include the 64 bit BLOB ID, the BLOB size, the transfer mode, the block size in logarithmic representation and the chunk size. The BLOB ID is application defined, but must match the BLOB ID the BLOB Servers have been started with.

The transfer will run until it either completes successfully for at least one target node, or is cancelled. The end of the transfer is communicated to the application through the :cpp:member:`end <bt_mesh_blob_cli_cb::end>` callback. Lost targets will be reported through the :cpp:member:`lost_target <bt_mesh_blob_cli_cb::lost_target>` callback.

API reference
*************

.. doxygengroup:: bt_mesh_blob_cli
   :project: Zephyr
   :members:
