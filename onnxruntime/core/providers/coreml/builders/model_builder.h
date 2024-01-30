// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/graph/graph_viewer.h"
#include "core/providers/coreml/builders/coreml_spec.h"
#include "core/providers/coreml/model/model.h"

// coremltools classes
namespace MPL {
class ModelPackage;
}

namespace MILBlob {
namespace Blob {
class StorageWriter;
}
}  // namespace MILBlob

namespace onnxruntime {
namespace coreml {

class IOpBuilder;
class Model;

class ModelBuilder {
 private:
  ModelBuilder(const GraphViewer& graph_viewer, const logging::Logger& logger,
               int32_t coreml_version, uint32_t coreml_flags);

 public:
  // Create the CoreML model, serialize to disk, load and compile using the CoreML API and return in `model`
  static Status Build(const GraphViewer& graph_viewer, const logging::Logger& logger,
                      int32_t coreml_version, uint32_t coreml_flags,
                      std::unique_ptr<Model>& model);

  ~ModelBuilder();

  const GraphViewer& GetGraphViewer() const { return graph_viewer_; }
  const InitializedTensorSet& GetInitializerTensors() const { return graph_viewer_.GetAllInitializedTensors(); }
  const ONNX_NAMESPACE::TensorProto* GetConstantInitializer(const std::string& name) const {
    return graph_viewer_.GetConstantInitializer(name, true);
  }

  /*
  const ONNX_NAMESPACE::TensorProto* GetInitializer(const std::string& name, bool& is_constant) const {
    is_constant = false;
    const auto* tensor = GetConstantInitializer(name);
    if (tensor) {
      is_constant = true;
    } else {
      // could be non-const initializer
      ORT_IGNORE_RETURN_VALUE(graph_viewer_.GetInitializedTensor(name, tensor));
    }

    return tensor;
  }
  */

  // the public CoreML version is the spec version +1 as CoreML 1.1 was spec version 2.
  // we only support CoreML 3 and later so the spec version is always version + 1.
  int32_t CoreMLVersion() const { return coreml_version_; }
  int32_t CoreMLSpecVersion() const { return coreml_version_ + 1; }

  bool CreateMLProgram() const { return create_ml_program_; }

  /*
   * NeuralNetworkLayer helpers
   */

  // Create a NeuralNetwork layer using the node name and optional suffix for the name.
  // If Node has no name a unique name will be generated from the node index and operator.
  std::unique_ptr<COREML_SPEC::NeuralNetworkLayer> CreateNNLayer(const Node& node, std::string_view suffix = "");

  // Add layer to the Core ML NeuralNetwork model
  void AddLayer(std::unique_ptr<COREML_SPEC::NeuralNetworkLayer> layer);

  /*
   * MLProgram helpers
   */

  // Create Operation, set type as well as the unique name attribute.
  std::unique_ptr<COREML_SPEC::MILSpec::Operation> CreateOperation(const Node& node, std::string_view op_type,
                                                                   std::string_view suffix = "");

  void AddTensorValueAsOperationInput(COREML_SPEC::MILSpec::Operation& op,
                                      std::string_view input_name,
                                      COREML_SPEC::MILSpec::Value&& input_value);

  //
  // Helpers for adding attributes from ONNX nodes as inputs to an ML Program Operation
  //

  // Add an `int` attribute as an Operation input. Converts to int32_t as that is what CoreML uses.
  void AddOnnxAttributeAsOperationInput(COREML_SPEC::MILSpec::Operation& op,
                                        std::string_view input_name,
                                        const int64_t attr_value);

  // Add an `ints` attribute as an Operation input. Converts to int32_t as that is what CoreML uses.
  void AddOnnxAttributeAsOperationInput(COREML_SPEC::MILSpec::Operation& op,
                                        std::string_view input_name,
                                        const std::vector<int64_t>& attr_value);

  // Add a `string` attribute as an Operation input. Converts to int32_t as that is what CoreML uses.
  void AddOnnxAttributeAsOperationInput(COREML_SPEC::MILSpec::Operation& op,
                                        std::string_view input_name,
                                        const std::string& attr_value);

  void AddConstantOperation(std::string_view name, const ONNX_NAMESPACE::TensorProto& initializer);

  void AddOperation(std::unique_ptr<COREML_SPEC::MILSpec::Operation> operation);

  /*
   * General helpers
   */

  // The initializer is processed separately (e.g. layout is transformed) by the operator builder,
  // so we don't do a copy of the original initializer into the model.
  void AddInitializerToSkip(const std::string& tensor_name);

  // There are some input which will not be used, add it to a list which will not
  // be added to CoreML model, since CoreML does not like input unused
  void AddInputToSkip(const std::string& input_name);

  std::string GetUniqueName(std::string_view base_name);
  std::string GetUniqueName(const Node& node, std::string_view suffix);

 private:
  // when generating an mlpackage, should a weight be written to the external file or added directly
  bool UseWeightFile(const onnx::TensorProto& weight);
  uint64_t AddWeightToFile(const onnx::TensorProto& weight);
  void AddConstantOperation(std::string_view name, COREML_SPEC::MILSpec::Value&& initializer);

  // Convert the ONNX model in graph_viewer_ to a CoreML::Specification::Model and serialize to disk.
  // We then load it using CoreML in order compile it.
  Status CreateModel();
  Status SaveModel();
  Status LoadModel(std::unique_ptr<Model>& model);

  // If a CoreML operation will use initializers directly, we will add the initializers to the skip list
  void PreprocessInitializers();

  // Copy and process all the initializers to CoreML model
  Status RegisterInitializers();

  Status ProcessNodes();
  Status RegisterModelInputs();
  Status RegisterModelOutputs();
  Status RegisterModelInputOutput(const NodeArg& node_arg, bool is_input);

  // Record the onnx scalar output names
  void AddScalarOutput(const std::string& output_name);

  // Record the onnx int64 type output names
  void AddInt64Output(const std::string& output_name);

  const GraphViewer& graph_viewer_;
  const logging::Logger& logger_;
  const int32_t coreml_version_;
  const uint32_t coreml_flags_;
  const bool create_ml_program_;         // ML Program (CoreML5, iOS 15+, macOS 12+) or NeuralNetwork (old)
  const std::string model_output_path_;  // create_ml_program_ ? dir for mlpackage : filename for mlmodel

  std::unique_ptr<CoreML::Specification::Model> coreml_model_;
  std::unordered_set<std::string> scalar_outputs_;
  std::unordered_set<std::string> int64_outputs_;
  std::unordered_map<std::string, OnnxTensorInfo> input_output_info_;

  std::unordered_map<std::string, int> initializer_usage_;
  std::unordered_set<std::string> skipped_inputs_;

  uint32_t name_token_{0};
  std::unordered_set<std::string> unique_names_;

  // mlprogram_main_ is the main block of the CoreML ML Program.
  // It is set in CreateModel to the CoreML Model.mlprogram.functions['main'].block_specializations['CoreML<ver>']
  // entry we create.
  COREML_SPEC::MILSpec::Block* mlprogram_main_{nullptr};
  std::unique_ptr<MPL::ModelPackage> mlpackage_;
  std::unique_ptr<MILBlob::Blob::StorageWriter> weights_file_writer_;
};

}  // namespace coreml
}  // namespace onnxruntime
