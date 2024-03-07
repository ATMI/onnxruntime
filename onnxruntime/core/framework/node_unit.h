// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <string>
#include <optional>
#include <vector>

#include "core/graph/basic_types.h"
#include "core/graph/graph.h"

namespace onnxruntime {

class GraphViewer;
class Node;
class NodeArg;
class Path;

namespace QDQ {
// Struct to represent a DQ->Op->Q node group
struct NodeGroup {
  std::vector<NodeIndex> dq_nodes;
  std::vector<NodeIndex> q_nodes;
  NodeIndex target_node;
};
}  // namespace QDQ

// Definition of one input or output
// If the optional quant_param is present, then this is a quantized input,
// otherwise this is a regular input
struct NodeUnitIODef {
  // The quantization parameter, scale is manadatory, and zero_point is optional
  struct QuantParam {
    const NodeArg& scale;
    const NodeArg* zero_point{nullptr};
  };

  const NodeArg& node_arg;
  const std::optional<QuantParam> quant_param;
};

/**
@class NodeUnit
Class to represent a single node or a QDQ group of nodes, which will be used as a single unit.
*/
class NodeUnit {
 public:
  // NodeUnit type
  enum class Type : uint8_t {
    SingleNode,  // The NodeUnit contains a single node
    QDQGroup,    // The NodeUnit contain a QDQ group of nodes, such as "DQ->Sigmoid->Q"
  };

 public:
  explicit NodeUnit(const Node& node);
  explicit NodeUnit(const GraphViewer& graph_viewer, const QDQ::NodeGroup& node_group);

  Type UnitType() const noexcept { return type_; }

  const std::vector<NodeUnitIODef>& Inputs() const noexcept { return inputs_; }
  const std::vector<NodeUnitIODef>& Outputs() const noexcept { return outputs_; }

  const std::string& Domain() const noexcept;
  const std::string& OpType() const noexcept;
  const std::string& Name() const noexcept;
  int SinceVersion() const noexcept;
  NodeIndex Index() const noexcept;
  const Path& ModelPath() const noexcept;
  ProviderType GetExecutionProviderType() const noexcept;

  const Node& GetNode() const noexcept { return target_node_; }
  const std::vector<const Node*>& GetDQNodes() const noexcept { return dq_nodes_; }
  const std::vector<const Node*>& GetQNodes() const noexcept { return q_nodes_; }
  std::vector<const Node*> GetAllNodesInGroup() const noexcept;

  /// Number of input edges to the logical node. For a QDQ node this is the count of input edges to the DQ nodes
  /// plus any other edges to the target node for inputs that are not via a DQ node.
  size_t InputEdgeCount() const { return input_edge_count_; }

  // output edges. src index is for outputs of the target node. dest index and node is for consumer of node unit
  // output. any Q nodes are hidden.
  Node::EdgeConstIterator OutputEdgesBegin() const;
  Node::EdgeConstIterator OutputEdgesEnd() const;

 private:
  // Initialization for a NodeUnit that contains a single node
  void InitForSingleNode();

  const std::vector<const Node*> dq_nodes_;  // dq nodes for this NodeUnit, not necessarily all inputs
  const Node& target_node_;
  const std::vector<const Node*> q_nodes_;  // q-nodes for this NodeUnit. not necessarily all outputs
  const Type type_;

  std::vector<NodeUnitIODef> inputs_;
  std::vector<NodeUnitIODef> outputs_;

  size_t input_edge_count_;  // total number of input edges

  // output edges, hiding any Q nodes involved. src_idx will be value from target node. only used for QDQ node group.
  Node::EdgeSet output_edges_;
};

// Get all the nodes in the given graph_viewer as NodeUnits (SingleNode or QDQGroup)
// And return a map to quick query the NodeUnit which contains the given Node,
// Note, the value of the map is owned by the vector of std::unique_ptr<NodeUnit>
std::pair<std::vector<std::unique_ptr<NodeUnit>>, std::unordered_map<const Node*, const NodeUnit*>>
GetAllNodeUnits(const GraphViewer& graph_viewer);

}  // namespace onnxruntime
