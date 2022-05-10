.. _bluetooth_mesh_models_op_agg_cli:

Opcodes Aggregator Client
#########################

The Opcodes Aggregator Client model is a foundation model defined by the Bluetooth
mesh specification. It is enabled with the :kconfig:option:`CONFIG_BT_MESH_OP_AGG_CLI` option.

The Opcodes Aggregator Client model is introduced in the Bluetooth Mesh Profile
Specification version 1.1, and is used to support the functionality of dispatching
a sequence of access layer messages to nodes supporting the Opcodes Aggregator Server model.

The Opcodes Aggregator Client model communicates with a
:ref:`bluetooth_mesh_models_op_agg_srv` model using the device key of the
target node and the application keys configured by the Configuration Client.

The Opcodes Aggregator Client model is implicitly bound to the device key on initialization.

The Opcodes Aggregator Client model should be bound to the same application keys that the client models,
used to produce the sequence of messages, are bound to.

The Opcodes Aggregator Client model is optional, and shall be instantiated only on the primary
element.

To be able to aggregate a message from a client model, it should support an asynchronous
API e.g. through callbacks.

API reference
*************

.. doxygengroup:: bt_mesh_op_agg_cli
   :project: Zephyr
   :members:
