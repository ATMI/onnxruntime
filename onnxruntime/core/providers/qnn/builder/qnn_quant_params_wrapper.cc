// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/qnn/builder/qnn_quant_params_wrapper.h"
#include <cassert>
#include <optional>
#include "QnnTypes.h"
#include "core/providers/qnn/builder/qnn_model_wrapper.h"
#include "core/providers/qnn/builder/qnn_utils.h"

namespace onnxruntime {
namespace qnn {

QnnQuantParamsWrapper::QnnQuantParamsWrapper(const QnnQuantParamsWrapper& other)
    : params_(QNN_QUANTIZE_PARAMS_INIT) {
  Status status = Init(other.params_);
  assert(status.IsOK());  // Expect other QnnQuantParamsWrapper to always have a supported quantization encoding.
}

QnnQuantParamsWrapper& QnnQuantParamsWrapper::operator=(const QnnQuantParamsWrapper& other) {
  if (this != &other) {
    Status status = Init(other.params_);
    assert(status.IsOK());  // Expect other QnnQuantParamsWrapper to always have a supported quantization encoding.
  }

  return *this;
}

QnnQuantParamsWrapper::QnnQuantParamsWrapper(float scale, int32_t offset) {
  params_.encodingDefinition = QNN_DEFINITION_DEFINED;
  params_.quantizationEncoding = QNN_QUANTIZATION_ENCODING_SCALE_OFFSET;
  params_.scaleOffsetEncoding.scale = scale;
  params_.scaleOffsetEncoding.offset = offset;
}

QnnQuantParamsWrapper QnnQuantParamsWrapper::Copy() const {
  return QnnQuantParamsWrapper(*this);
}

Status QnnQuantParamsWrapper::Init(const Qnn_QuantizeParams_t& params) {
  if (scale_offset_data_) {
    scale_offset_data_.reset(nullptr);
    params_ = QNN_QUANTIZE_PARAMS_INIT;
  }

  if (params.encodingDefinition == QNN_DEFINITION_UNDEFINED) {
    params_ = params;
    return Status::OK();
  }

  switch (params.quantizationEncoding) {
    case QNN_QUANTIZATION_ENCODING_SCALE_OFFSET:
      params_ = params;
      break;
    case QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET: {
      params_.encodingDefinition = params.encodingDefinition;
      params_.quantizationEncoding = params.quantizationEncoding;
      params_.axisScaleOffsetEncoding.axis = params.axisScaleOffsetEncoding.axis;
      params_.axisScaleOffsetEncoding.numScaleOffsets = params.axisScaleOffsetEncoding.numScaleOffsets;

      // Deep copy the scaleOffset data.
      const uint32_t num_elems = params.axisScaleOffsetEncoding.numScaleOffsets;

      if (num_elems > 0) {
        const size_t num_bytes = num_elems * sizeof(Qnn_ScaleOffset_t);
        scale_offset_data_ = std::make_unique<char[]>(num_bytes);
        std::memcpy(scale_offset_data_.get(), params.axisScaleOffsetEncoding.scaleOffset, num_bytes);
        params_.axisScaleOffsetEncoding.scaleOffset = reinterpret_cast<Qnn_ScaleOffset_t*>(scale_offset_data_.get());
      } else {
        params_.axisScaleOffsetEncoding.scaleOffset = nullptr;
      }
      break;
    }
    case QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET: {
      const uint32_t num_elems = params.bwAxisScaleOffsetEncoding.numElements;

      params_.encodingDefinition = params.encodingDefinition;
      params_.quantizationEncoding = params.quantizationEncoding;
      params_.bwAxisScaleOffsetEncoding.axis = params.bwAxisScaleOffsetEncoding.axis;
      params_.bwAxisScaleOffsetEncoding.bitwidth = params.bwAxisScaleOffsetEncoding.bitwidth;
      params_.bwAxisScaleOffsetEncoding.numElements = num_elems;

      // Deep copy the scales[] and offsets[] arrays
      if (num_elems > 0) {
        const size_t num_scale_bytes = num_elems * sizeof(float);
        const size_t num_zp_bytes = num_elems * sizeof(int32_t);
        const size_t num_bytes = num_scale_bytes + num_zp_bytes;
        scale_offset_data_ = std::make_unique<char[]>(num_bytes);
        char* scales_begin = scale_offset_data_.get();
        char* zps_begin = scales_begin + num_scale_bytes;

        std::memcpy(scales_begin, params.bwAxisScaleOffsetEncoding.scales, num_scale_bytes);
        std::memcpy(zps_begin, params.bwAxisScaleOffsetEncoding.offsets, num_zp_bytes);
        params_.bwAxisScaleOffsetEncoding.scales = reinterpret_cast<float*>(scales_begin);
        params_.bwAxisScaleOffsetEncoding.offsets = reinterpret_cast<int32_t*>(zps_begin);
      } else {
        params_.bwAxisScaleOffsetEncoding.scales = nullptr;
        params_.bwAxisScaleOffsetEncoding.offsets = nullptr;
      }
      break;
    }
    default:
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Unsupported QNN quantization encoding: ", params.quantizationEncoding);
  }

  return Status::OK();
}

Status QnnQuantParamsWrapper::Init(const QnnModelWrapper& qnn_model_wrapper, const NodeUnitIODef& io_def) {
  const std::optional<NodeUnitIODef::QuantParam>& ort_quant_params = io_def.quant_param;

  if (scale_offset_data_) {
    scale_offset_data_.reset(nullptr);
    params_ = QNN_QUANTIZE_PARAMS_INIT;
  }

  if (!ort_quant_params.has_value()) {
    params_.encodingDefinition = QNN_DEFINITION_UNDEFINED;
    params_.quantizationEncoding = QNN_QUANTIZATION_ENCODING_UNDEFINED;
    return Status::OK();
  }

  std::vector<float> scales;
  std::vector<int32_t> zero_points;

  ORT_RETURN_IF_ERROR(qnn_model_wrapper.UnpackScales(ort_quant_params->scale.Name(), scales));

  bool is_int4_type = false;

  if (ort_quant_params->zero_point != nullptr) {
    int32_t onnx_tp_type = 0;
    ORT_RETURN_IF_ERROR(qnn_model_wrapper.UnpackZeroPoints(ort_quant_params->zero_point->Name(), zero_points, onnx_tp_type));

    is_int4_type = (onnx_tp_type == ONNX_NAMESPACE::TensorProto_DataType_INT4) ||
                   (onnx_tp_type == ONNX_NAMESPACE::TensorProto_DataType_UINT4);
  }

  const bool is_per_tensor = scales.size() == 1;

  if (is_per_tensor) {
    params_.encodingDefinition = QNN_DEFINITION_DEFINED;
    params_.quantizationEncoding = QNN_QUANTIZATION_ENCODING_SCALE_OFFSET;

    // Parse scale & zero_point
    params_.scaleOffsetEncoding.scale = scales[0];

    if (ort_quant_params->zero_point != nullptr) {
      ORT_RETURN_IF_NOT(zero_points.size() == 1, "Expected one zero-point value");
      params_.scaleOffsetEncoding.offset = zero_points[0];
    } else {
      params_.scaleOffsetEncoding.offset = 0;
    }
  } else if (!is_per_tensor && is_int4_type) {
    // Per-channel quantization for int4.
    const auto* io_shape = io_def.node_arg.Shape();
    ORT_RETURN_IF(io_shape == nullptr, "Input/output tensor proto must have a shape");
    const int32_t io_rank = io_shape->dim_size();

    constexpr int64_t DEFAULT_QDQ_AXIS = 1;
    int64_t axis = ort_quant_params->axis.value_or(DEFAULT_QDQ_AXIS);
    if (axis < 0) {
      axis += io_rank;
    }
    ORT_RETURN_IF_NOT(axis >= 0 && axis < io_rank,
                      "Quantization axis must be within the range [0, rank - 1]");

    const size_t num_elems = scales.size();
    const bool no_zero_points = zero_points.empty();
    ORT_RETURN_IF_NOT(num_elems > 1, "Expected more than one scale value");
    ORT_RETURN_IF_NOT(no_zero_points || zero_points.size() == num_elems,
                      "Expected the same number of zero-points and scales for per-channel quantization");

    params_.encodingDefinition = QNN_DEFINITION_DEFINED;
    params_.quantizationEncoding = QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET;
    params_.bwAxisScaleOffsetEncoding.axis = static_cast<int32_t>(*(ort_quant_params->axis));
    params_.bwAxisScaleOffsetEncoding.bitwidth = 4;
    params_.bwAxisScaleOffsetEncoding.numElements = static_cast<uint32_t>(num_elems);

    const size_t num_scale_bytes = num_elems * sizeof(float);
    const size_t num_zp_bytes = num_elems * sizeof(int32_t);
    const size_t num_bytes = num_scale_bytes + num_zp_bytes;
    scale_offset_data_ = std::make_unique<char[]>(num_bytes);

    char* scales_begin = scale_offset_data_.get();
    char* zps_begin = scales_begin + num_scale_bytes;
    gsl::span<float> scales_span(reinterpret_cast<float*>(scales_begin), num_elems);
    gsl::span<int32_t> zps_span(reinterpret_cast<int32_t*>(zps_begin), num_elems);

    for (size_t i = 0; i < num_elems; i++) {
      scales_span[i] = scales[i];
      zps_span[i] = no_zero_points ? 0 : zero_points[i];
    }

    params_.bwAxisScaleOffsetEncoding.scales = scales_span.data();
    params_.bwAxisScaleOffsetEncoding.offsets = zps_span.data();
  } else if (!is_per_tensor && !is_int4_type) {
    // Per-channel quantization without int4.
    const auto* io_shape = io_def.node_arg.Shape();
    ORT_RETURN_IF(io_shape == nullptr, "Input/output tensor proto must have a shape");
    const int32_t io_rank = io_shape->dim_size();

    constexpr int64_t DEFAULT_QDQ_AXIS = 1;
    int64_t axis = ort_quant_params->axis.value_or(DEFAULT_QDQ_AXIS);
    if (axis < 0) {
      axis += io_rank;
    }
    ORT_RETURN_IF_NOT(axis >= 0 && axis < io_rank,
                      "Quantization axis must be within the range [0, rank - 1]");

    params_.encodingDefinition = QNN_DEFINITION_DEFINED;
    params_.quantizationEncoding = QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET;

    const size_t num_elems = scales.size();
    const bool no_zero_points = zero_points.empty();
    ORT_RETURN_IF_NOT(num_elems > 1, "Expected more than one scale value");
    ORT_RETURN_IF_NOT(no_zero_points || zero_points.size() == num_elems,
                      "Expected the same number of zero-points and scales for per-channel quantization");

    const size_t num_bytes = num_elems * sizeof(Qnn_ScaleOffset_t);
    scale_offset_data_ = std::make_unique<char[]>(num_bytes);
    gsl::span<Qnn_ScaleOffset_t> data_span(reinterpret_cast<Qnn_ScaleOffset_t*>(scale_offset_data_.get()), num_elems);

    for (size_t i = 0; i < num_elems; i++) {
      data_span[i].scale = scales[i];
      data_span[i].offset = no_zero_points ? 0 : zero_points[i];
    }

    params_.axisScaleOffsetEncoding.axis = static_cast<int32_t>(axis);
    params_.axisScaleOffsetEncoding.numScaleOffsets = static_cast<uint32_t>(num_elems);
    params_.axisScaleOffsetEncoding.scaleOffset = data_span.data();
  }

  return Status::OK();
}
}  // namespace qnn
}  // namespace onnxruntime
