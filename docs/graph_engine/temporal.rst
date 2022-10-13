Temporal graph support
======================

DeepGNN suports temporal graphs, i.e. graphs with timestamps assigned to each element:
nodes, edges, or features. Lets explore how to use them.

Node
----

A new node can be added or removed to a graph at different times.
A JSON schema for temporal nodes is an extension of `regular<data_spec.html>`_ nodes with two new fields: `created_at`
and `removed_at` with values recorded in Unix time format. Here is a concrete example:

.. code-block:: json

	{
	"node_id": 42,
	"node_type": 0,
	"node_weight": 1.0,
	"binary_feature": {"0": "microsoft", "1": "xbox"},
	"created_at": 13,
	"removed_at": 23,
	"edge":[
		{
		"src_id": 42,
		"dst_id": 99,
		"edge_type": 0,
		"weight": 2.0,
		},
	],
	}

Here we describe a node with id `42`, two binary features, a single outgoing edge. Node it existed from time `13`` to time `23`.
If `created_at` field is not specified, it is assumed to be `0`. If `removed_at` field is not specified, node is assumed to be always present after it was created.
DeepGNN doesn't support changing node type over time yet. A simple workaround is to delete the node and create a new node with a new type or store node type as a feature.

Edge
----

An edge can be added to or removed from the graph at specific times. Edges have two fields: `created_at`
and `removed_at` with values recorded in Unix time format. Here is an extension of the previous example:


.. code-block:: json

	{
	"node_id": 42,
	"node_type": 0,
	"node_weight": 1.0,
	"binary_feature": {"0": "microsoft", "1": "xbox"},
	"created_at": 13,
	"removed_at": 23,
	"edge":[
		{
		"src_id": 42,
		"dst_id": 99,
		"edge_type": 0,
		"weight": 2.0,
		"created_at": 13,
		"removed_at": 15,
	},
	{
		"src_id": 42,
		"dst_id": 99,
		"edge_type": 0,
		"weight": 2.0,
		"created_at": 17,
	},
	{
		"src_id": 42,
		"dst_id": 101,
		"edge_type": 1,
		"weight": 1.0,
		"removed_at": 19,
	},
	],
}

Node with id `42` has two outgoing edges with types `0` and `1`. The first edge existed during intervals `[13, 15)` and `[17, 23)`, however
second edge was created with the node at time `13`` and removed at `19`. If `created_at` field is not specified, it is assumed to be the same as `created_at` of
the node, similarly `removed_at` default value is the same as `removed_at` of the node.

Feature
-------

Node or edge features can also change values over time. To support this behaviour we expand schema for existing features.
For binary and dense features, the format is not just a list of values, but union of two types:

1. a list of values for features that are present during the node/edge lifetime
2. a list of objects with three fields: values, `created_at` and `removed_at` .

The default values for `created_at` and `removed_at`  are similar to the nodes/edges above. Sparse features are extended in a similar
way, the accepted types are union of a sparse feature object and list of such objects. Lets look at an example:

.. code-block:: json

	{
	"node_id": 42,
	"node_type": 0,
	"node_weight": 1.0,
	"binary_feature": {
		"0": [
			{"values":["microsoft"], "created_at": 13, "removed_at": 20},},
			{"values":["xbox"], "created_at": 20}
		],
	},
	"created_at": 13,
	"removed_at": 23,
	"edge":[
		{
		"src_id": 42,
		"dst_id": 99,
		"edge_type": 0,
		"weight": 2.0,
		"created_at": 13,
		"removed_at": 23,
		"sparse_int32_feature":{
			"0": [
				{"coordinates": [[5, 13], [7, 25]], "values": [-1, 1024], "removed_at":21},
				{"coordinates": [[4, 2], [1, 3]], "values": [2, 4], "created_at":21},
			],
			"1": {"coordinates": [13, 42], "values": [1, 1]},
		},
		},
	],
	}

There are three temporal features in the example above: a binary feature belonging to node with id `42`
and two sparse integer feature attached to an edge with source node `42` and destination node `99`.
Feature with id `0` has 2 temporal values assigned to it split around time `21`. Feature with id `1` is
present during the entire life of edge, from `13` to `23`.
