// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/common.h"
#include "core/providers/coreml/builders/helper.h"
#include "core/providers/coreml/builders/impl/base_op_builder.h"
#include "core/providers/coreml/builders/op_builder_factory.h"
#include "core/providers/shared/utils/utils.h"

#ifdef __APPLE__OR__TEST__
#include "core/providers/coreml/builders/impl/builder_utils.h"
#include "core/providers/coreml/builders/model_builder.h"
#include "core/providers/coreml/shape_utils.h"
#endif

using namespace CoreML::Specification;
using namespace CoreML::Specification::MILSpec;

namespace onnxruntime {
namespace coreml {

class ConvOpBuilder : public BaseOpBuilder {
  // Add operator related
#ifdef __APPLE__OR__TEST__
 public:
  void AddInitializersToSkip(ModelBuilder& model_builder, const Node& node) const override;

 private:
  Status AddToModelBuilderImpl(ModelBuilder& model_builder, const Node& node,
                               const logging::Logger& logger) const override;
#endif

  // Operator support related
 private:
  bool IsOpSupportedImpl(const Node& /* node */, const OpBuilderInputParams& /* input_params */,
                         const logging::Logger& /* logger */) const override;

  bool SupportsMLProgram() const override { return true; }
};

// Add operator related

#ifdef __APPLE__OR__TEST__
void ConvOpBuilder::AddInitializersToSkip(ModelBuilder& model_builder, const Node& node) const {
  if (model_builder.CreateMLProgram()) {
    // TODO: See if converting to 'const' operation, unless we need a type conversion of the weight, works fine.
    return;
  }

  const auto& input_defs = node.InputDefs();

  // skip the weight and bias (if has it) for conv as we will directly set those as part of the NN layer
  model_builder.AddInitializerToSkip(input_defs[1]->Name());  // w

  if (input_defs.size() > 2) {
    model_builder.AddInitializerToSkip(input_defs[2]->Name());  // b
  }
}

/*
def translate_generic_op(op, parameters, blob_writer, literal_params=[]):
    inputs = {}
    for param_name, vars in op.inputs.items():
        if param_name.startswith("_"):
            continue
        if not isinstance(vars, (list, tuple)):
            vars = [vars]

        arguments = []
        for _var in vars:
            binding = pm.Argument.Binding()
            # use const value literals if requested
            if param_name in literal_params:
                binding.value.CopyFrom(create_immediate_value(_var))
            else:
                binding.name = _var.name
            arguments.append(binding)

        args = pm.Argument()
        args.arguments.extend(arguments)
        inputs[param_name] = args

    outputs = [
        pm.NamedValueType(name=v.name, type=types_to_proto(v.sym_type))
        for v in op.outputs
    ]
    blocks = None
    if len(op.blocks) > 0:
        blocks = [create_block(b, parameters, blob_writer) for b in op.blocks]

    op_type = op.op_type
    attr_dict = {}
    if op.op_type in SSAOpRegistry.custom_ops:
        op_type = "custom_layer"
        class_name = op.bindings.get("class_name", op.name)
        input_order = op.bindings.get("input_order", [])
        parameters = op.bindings.get("parameters", [])
        weights = op.bindings.get("weights", [])
        description = op.bindings.get("description", "")

        attr_dict["name"] = create_scalar_value(op.name)
        attr_dict["class_name"] = create_scalar_value(class_name)
        attr_dict["input_order"] = create_list_scalarvalue(input_order, str)
        attr_dict["parameters"] = create_list_scalarvalue(parameters, str)
        attr_dict["weights"] = create_list_scalarvalue(weights, str)
        attr_dict["description"] = create_scalar_value(description)

    attr_dict["name"] = create_scalar_value(op.name)

    return pm.Operation(
        type=op_type,
        blocks=blocks,
        inputs=inputs,
        attributes=attr_dict,
        outputs=outputs,
    )
*/

// TODO: Do we need this when creating a local immediate value for the op. Seems to depend on the op spec/impl as to
// whether it allows/expects that vs. adding a 'const' operation with the initializer
// Argument_Binding CreateArgumentBinding(const std::string& name, ONNX_NAMESPACE::TensorProto *const_value = nullptr) {
//  Argument_Binding binding;
//  binding.set_name(name);
//  if (const_value) {
//      binding.set_allocated_value(const_value);
//  }
//  return binding;
//}

Status ConvOpBuilder::AddToModelBuilderImpl(ModelBuilder& model_builder, const Node& node,
                                            const logging::Logger& logger) const {
  const auto& input_defs = node.InputDefs();
  const auto& output_defs = node.OutputDefs();
  const auto& input_name = input_defs[0]->Name();
  const auto& output_name = output_defs[0]->Name();

  NodeAttrHelper helper(node);

  if (model_builder.CreateMLProgram()) {
    // https://github.com/apple/coremltools/blob/7.1/coremltools/converters/mil/mil/ops/defs/iOS15/conv.py

    std::unique_ptr<Operation> conv_op = model_builder.CreateOperation(node, "conv");

    AddOperationInput(*conv_op, "x", input_name);
    AddOperationInput(*conv_op, "weight", input_defs[1]->Name());

    if (input_defs.size() > 2) {
      AddOperationInput(*conv_op, "bias", input_defs[2]->Name());
    }

    // ONNX attributes. Add as inputs if specified/required
    auto strides = helper.GetInt64s("strides");
    auto dilations = helper.GetInt64s("dilations");
    auto groups = helper.GetInt64("group");

    if (strides) {
      model_builder.AddOnnxAttributeAsOperationInput(*conv_op, "strides", *strides);
    }

    if (dilations) {
      model_builder.AddOnnxAttributeAsOperationInput(*conv_op, "dilations", *dilations);
    }

    if (groups) {
      model_builder.AddOnnxAttributeAsOperationInput(*conv_op, "groups", *groups);
    }

    AutoPadType auto_pad_type = StringToAutoPadType(helper.Get("auto_pad", "NOTSET"));

    // pad type (string)
    // valid - no pads  (ONNX auto_pad VALID)
    // custom - pads input  (ONNX NOTSET)
    // same - inferred to be `d_out[i] = ceil(d_in[i] / strides[i])`  (assuming == ONNX SAME_UPPER)
    // same_lower - as per same but any extra rows/cols are added at top/left if padding is odd (ONNX SAME_LOWER)
    switch (auto_pad_type) {
      case AutoPadType::NOTSET: {
        // use `pads` attribute.
        auto onnx_pads = helper.GetInt64s("pads");  // 'pads' must be provided if auto_pad is NOTSET
        if (onnx_pads) {
          model_builder.AddOnnxAttributeAsOperationInput(*conv_op, "pad_type", "custom");
          // need to re-order from x1_start, x2_start..., x1_end, x2_end... to
          // x1_start, x1_end, x2_start, x2_end,...
          size_t num_pads = onnx_pads->size();
          std::vector<int64_t> reordered_pads(num_pads, 0);
          for (size_t i = 0; i < num_pads; ++i) {
            auto cur_dim = i % (num_pads / 2);
            if (i % 2 == 0) {  // start value
              reordered_pads[cur_dim] = (*onnx_pads)[i];
            } else {  // end value
              reordered_pads[cur_dim + 1] = (*onnx_pads)[i];
            }
          }
          model_builder.AddOnnxAttributeAsOperationInput(*conv_op, "pad", *onnx_pads);
          break;
        }

        // in theory the pads may not be provided and in that case the default is no padding. as that is the same
        // as 'valid', fall through
        [[fallthrough]];
      }
      case AutoPadType::VALID:
        model_builder.AddOnnxAttributeAsOperationInput(*conv_op, "pad_type", "valid");
        break;
      case AutoPadType::SAME_UPPER:
        model_builder.AddOnnxAttributeAsOperationInput(*conv_op, "pad_type", "same");
        break;
      case AutoPadType::SAME_LOWER:
        model_builder.AddOnnxAttributeAsOperationInput(*conv_op, "pad_type", "same_lower");
        break;
    }

    // set output
    AddOperationOutput(*conv_op, *node.OutputDefs()[0]);
  } else {
    std::unique_ptr<COREML_SPEC::NeuralNetworkLayer> layer = model_builder.CreateNNLayer(node);

    auto strides = helper.Get("strides", std::vector<int64_t>{1, 1});
    auto dilations = helper.Get("dilations", std::vector<int64_t>{1, 1});
    auto onnx_pads = helper.Get("pads", std::vector<int64_t>{0, 0, 0, 0});
    const auto group = helper.Get("group", static_cast<int64_t>(1));

    const auto& weight_tensor = *model_builder.GetInitializerTensors().at(input_defs[1]->Name());
    std::vector<int64_t> weight_shape = {weight_tensor.dims().cbegin(), weight_tensor.dims().cend()};

    const bool is_1d_conv = (weight_shape.size() == 3);

    if (is_1d_conv) {
      // weight_shape needs to be expanded from MXCXH->MXCXHx1
      weight_shape.push_back(1);
    }

    // Strides/dilations for 1d conv is normally of length 1. Expand them by 1
    // to meet the required length 2 (for 2d conv it's normally 2)
    // Similarly 1d conv normally has a length 2 padding. Expand it to length 4 by adding additional zeros.
    if (is_1d_conv) {
      if (strides.size() < 2) {
        ORT_RETURN_IF_NOT(strides.size() == 1, "strides size does not equal 1 for Conv 1d");
        strides.push_back(1);
      }
      if (dilations.size() < 2) {
        ORT_RETURN_IF_NOT(dilations.size() == 1, "dilations size does not equal 1 for Conv 1d");
        dilations.push_back(1);
      }
      if (onnx_pads.size() < 4) {
        ORT_RETURN_IF_NOT(onnx_pads.size() == 2, "onnx_pads size does not equal 2 for Conv 1d");
        onnx_pads.insert(onnx_pads.begin() + 1, 0);
        onnx_pads.push_back(0);
      }
    }

    auto* coreml_conv = layer->mutable_convolution();

    std::string expand_output_name = model_builder.GetUniqueName(node.Name() + "_expandDims");

    if (is_1d_conv) {
      // Add an expanddims layer here. CoreML only supports 2d convolution, so for 1d Conv case
      // we need to add an additional dimension here to the input to make it "2d Conv" like.
      // NxCxH -> NxCxHx1
      auto expand_layer = model_builder.CreateNNLayer(node, "_Conv_expand");
      expand_layer->mutable_expanddims()->add_axes(-1);
      *expand_layer->mutable_input()->Add() = input_name;
      *expand_layer->mutable_output()->Add() = expand_output_name;
      model_builder.AddLayer(std::move(expand_layer));
    }

    coreml_conv->set_outputchannels(weight_shape[0]);  // M
    coreml_conv->set_kernelchannels(weight_shape[1]);  // C/Group
    coreml_conv->add_kernelsize(weight_shape[2]);      // H
    coreml_conv->add_kernelsize(weight_shape[3]);      // W
    coreml_conv->set_ngroups(group);
    *coreml_conv->mutable_stride() = {strides.cbegin(), strides.cend()};
    *coreml_conv->mutable_dilationfactor() = {dilations.cbegin(), dilations.cend()};

    coreml_conv->set_isdeconvolution(false);

    // Add Padding
    // Usually using autopadding is more efficient than using explicit padding
    // Try to see if we can map explicit padding to auto padding
    std::vector<int64_t> input_shape;
    ORT_RETURN_IF_NOT(GetShape(*input_defs[0], input_shape, logger), "Cannot get shape");
    AutoPadType auto_pad_type;
    ORT_RETURN_IF_ERROR(HandleAutoPad(input_shape, weight_shape[2], weight_shape[3],
                                      onnx_pads, strides, dilations,
                                      StringToAutoPadType(helper.Get("auto_pad", "NOTSET")),
                                      auto_pad_type));

    if (AutoPadType::SAME_UPPER == auto_pad_type || AutoPadType::SAME_LOWER == auto_pad_type) {
      auto* padding_type = coreml_conv->mutable_same();
      if (AutoPadType::SAME_LOWER == auto_pad_type) {  // default is SAME_UPPER
        padding_type->set_asymmetrymode(COREML_SPEC::SamePadding_SamePaddingMode_TOP_LEFT_HEAVY);
      }
    } else {
      auto* padding_type = coreml_conv->mutable_valid();
      if (AutoPadType::NOTSET == auto_pad_type && onnx_pads != std::vector<int64_t>{0, 0, 0, 0}) {
        // NOTSET is adding the explicit padding to the ValidPadding.paddingAmounts
        auto* height_border = padding_type->mutable_paddingamounts()->add_borderamounts();
        height_border->set_startedgesize(onnx_pads[0]);
        height_border->set_endedgesize(onnx_pads[2]);
        auto* width_border = padding_type->mutable_paddingamounts()->add_borderamounts();
        width_border->set_startedgesize(onnx_pads[1]);
        width_border->set_endedgesize(onnx_pads[3]);
      }
    }

    // Add weight
    ORT_RETURN_IF_ERROR(CreateCoreMLWeight(*coreml_conv->mutable_weights(), weight_tensor));

    // Add bias if present
    if (input_defs.size() > 2) {
      coreml_conv->set_hasbias(true);
      const auto& bias_tensor = *model_builder.GetInitializerTensors().at(input_defs[2]->Name());
      ORT_RETURN_IF_ERROR(CreateCoreMLWeight(*coreml_conv->mutable_bias(), bias_tensor));
    }

    if (is_1d_conv) {
      std::string conv_output_name = model_builder.GetUniqueName(node.Name() + "_conv_output");
      *layer->mutable_input()->Add() = expand_output_name;
      *layer->mutable_output()->Add() = conv_output_name;
      model_builder.AddLayer(std::move(layer));

      // Add a squeeze layer here. Since CoreML only supports 2d conv and we expanded the dimension by 1 before,
      // we need to squeeze it back from NxCxHx1->NxCxH.
      auto squeeze_layer = model_builder.CreateNNLayer(node, "_Conv_squeeze");
      squeeze_layer->mutable_squeeze()->add_axes(-1);
      *squeeze_layer->mutable_input()->Add() = conv_output_name;
      *squeeze_layer->mutable_output()->Add() = output_name;
      model_builder.AddLayer(std::move(squeeze_layer));
    } else {
      *layer->mutable_input()->Add() = input_name;
      *layer->mutable_output()->Add() = output_name;
      model_builder.AddLayer(std::move(layer));
    }
  }

  return Status::OK();
}
#endif

// Operator support related

bool ConvOpBuilder::IsOpSupportedImpl(const Node& node, const OpBuilderInputParams& input_params,
                                      const logging::Logger& logger) const {
  const auto& name = node.Name();
  const auto& input_defs = node.InputDefs();

  const auto& weight_name = input_defs[1]->Name();
  const auto* weight = input_params.graph_viewer.GetConstantInitializer(weight_name, true);

  if (input_params.create_mlprogram) {
    // ML Program supports non-const weight, 1D, 2D and 3D.
    // keep to 1D and 2D for consistency with the NeuralNetwork implementation for now.
    // add 3D support as/when needed.
  } else {
    if (!weight) {
      LOGS(logger, VERBOSE) << "The weight of Conv [" << name << "] must be a constant initializer";
      return false;
    }
  }

  // use the weight for the shape as it should always be known
  const auto* weight_shape = input_defs[1]->Shape();
  int64_t num_dims = weight_shape ? weight_shape->dim_size() : -1;

  // ONNX spec requires N and C as first 2 dims
  if (num_dims != 3 && num_dims != 4) {
    LOGS(logger, VERBOSE) << "Conv [" << name << "] is " << num_dims - 2 << "D. "
                          << "Only 1D and 2D Conv are supported currently.";
    return false;
  }

  if (input_defs.size() > 2 && !input_params.graph_viewer.GetConstantInitializer(input_defs[2]->Name(), true)) {
    LOGS(logger, VERBOSE) << "The bias of Conv [" << name << "] must be a constant initializer";
    return false;
  }

  // there's no equivalent to allow a manual kernel shape in CoreML.
  // it's OK if a specified kernel_shape matches what we would infer from the weight input.
  NodeAttrHelper helper(node);
  auto kernel_shape = helper.GetInt64s("kernel_shape");
  if (kernel_shape) {
    bool valid = true;
    if (static_cast<int64_t>(kernel_shape->size()) == num_dims - 2) {
      for (int i = 0; i < num_dims - 2; ++i) {
        // check the specified kernel shape matches the weight shape. skip the initial N and C dims in the latter.
        if ((*kernel_shape)[i] != weight_shape->dim()[i + 2].dim_value()) {
          valid = false;
          break;
        }
      }
    } else {
      valid = false;
    }

    if (!valid) {
      LOGS(logger, VERBOSE) << "Conv [" << name << "] kernel_shape attribute does not match the weight shape";
      return false;
    }
  }

  return true;
}

void CreateConvOpBuilder(const std::string& op_type, OpBuilderRegistrations& op_registrations) {
  op_registrations.builders.push_back(std::make_unique<ConvOpBuilder>());
  op_registrations.op_builder_map.emplace(op_type, op_registrations.builders.back().get());
}

}  // namespace coreml
}  // namespace onnxruntime
