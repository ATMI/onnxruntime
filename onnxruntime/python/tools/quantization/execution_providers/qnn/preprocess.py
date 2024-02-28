# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License. See License.txt in the project root for
# license information.
# --------------------------------------------------------------------------
import logging
from pathlib import Path

import onnx

from ...fusions import FusionGelu, FusionLayerNormalization
from ...onnx_model import ONNXModel
from .fusion_lpnorm import FusionLpNormalization


def qnn_preprocess_model(model_input: Path, model_output: Path, fuse_layernorm: bool = False) -> bool:
    modified = False
    model = onnx.load_model(model_input)
    onnx_model = ONNXModel(model)

    # Fuse Erf sequence into a single Gelu
    fusion_gelu = FusionGelu(onnx_model)
    if fusion_gelu.apply():
        modified = True

    # Fuse ReduceL2 sequence into a single LpNormalization node with p == 2.
    fusion_lpnorm = FusionLpNormalization(onnx_model)
    if fusion_lpnorm.apply():
        modified = True

    # Optionally, fuse ReduceMean sequence into a single LayerNormalization node.
    if fuse_layernorm:
        onnx_opset = next(x for x in model.opset_import if x.domain == "" or x.domain == "ai.onnx")

        # Need opset >= 17 to use LayerNormalization.
        if onnx_opset.version < 17:
            logging.warning(
                "Unable to fuse ReduceMean sequence into a LayerNormalization node. "
                "ONNX model must use an opset >= 17 in order to use LayerNormalization, "
                f"but found version {onnx_opset.version}. Please use onnx.version_converter to update your model."
            )
        else:
            fusion_layernorm = FusionLayerNormalization(onnx_model)
            if fusion_layernorm.apply():
                modified = True

    # Make sure all nodes have a name.
    unnamed_node_prefix = "qnn_preproc_node_"
    available_suffix = onnx_model.get_largest_node_name_suffix(unnamed_node_prefix) + 1
    for node in onnx_model.model.graph.node:
        if node.op_type != "Constant" and not node.name:
            new_node_name = f"{unnamed_node_prefix}{available_suffix!s}"
            available_suffix += 1
            node.name = new_node_name
            modified = True
            logging.warning(f"Node of type {node.op_type} does not have a name. Renamed to {new_node_name}.")

    if modified:
        onnx_model.topological_sort()
        onnx.save_model(model, model_output)

    return modified
