# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation.  All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------

from logging import getLogger
from typing import List, Optional
from fusion_base import Fusion
from fusion_utils import FusionUtils
from onnx import GraphProto, ModelProto, TensorProto, ValueInfoProto, helper, inliner
from onnx_model import OnnxModel
from fusion_options import FusionOptions

logger = getLogger(__name__)

class PostProcessCausalLMHead(Fusion):
    def __init__(
        self,
        model: OnnxModel,
    ):
        super().__init__(model, "DONOTUSE", [ #"model_modeling_mixformer_sequential_ParallelBlock_sub1_1",
                                             "model_modeling_mixformer_sequential_CausalLMHead_sub_1__1_1"])

    def fuse(
            self,
            node,
            input_name_to_nodes,
            output_name_to_node,
    ):
        print("node.input[0] ", node.input[0])
        print("node.input[1] ", node.input[1])
        node.input[0] = node.input[1]

class FissionTransformerBlockPhi(Fusion):
    def __init__(
        self,
        model: OnnxModel,
    ):
        super().__init__(model, "DONOTUSE", ["model_modeling_mixformer_sequential_ParallelBlock_sub1_1",
                                             "model_modeling_mixformer_sequential_ParallelBlock_sub2_1",
                                             ])

    def uname(self, layer_id, name):
        return name + "_" + str(layer_id)

    def get_layer_id(self, node):
        if node.op_type == "model_modeling_mixformer_sequential_ParallelBlock_sub1_1":
            return 1
        elif node.op_type == "model_modeling_mixformer_sequential_ParallelBlock_sub2_1":
            return 2

    def fuse(
            self,
            node,
            input_name_to_nodes,
            output_name_to_node,
    ):
        layer_id = self.get_layer_id(node)

        # transformer block input and output
        i_hidden_states = node.input[2]
        i_attn_mask = node.input[1]
        i_kv_cache = node.input[3]
        o_hidden_states = node.output[1]
        o_kv_cache = node.output[0]

        print("o_hidden_states ", o_hidden_states)
        print("o_kv_cache ", o_kv_cache)

        # internal nodes weights
        ln_weight = node.input[4] #float32[2560]
        ln_bias = node.input[5] #float32[2560]
        attn_qkv_weight = node.input[6] #float32[7680,2560] need transpose?
        attn_qkv_bias = node.input[7] #float32[7680]
        attn_out_weight = node.input[10] #float32[2560,2560] need transpose?
        attn_out_bias = node.input[11] #float32[2560]
        mlp_fc1_weight = node.input[12] #float32[10240,2560] need transpose?
        mlp_fc1_bias = node.input[13] #float32[10240]
        mlp_fc2_weight = node.input[14] #float32[2560,10240] need transpose?
        mlp_fc2_bias = node.input[15] #float32[2560]

        # opt graph construction.

        subgraph_nodes = [
            helper.make_node(
                'LayerNormalization',
                inputs=[i_hidden_states, ln_weight, ln_bias],
                outputs=[self.uname(layer_id, 'ln_out')],
                name=self.uname(layer_id, 'LayerNormalization'),
                epsilon=9.999999747378752e-06,
            ),
            helper.make_node(
                'Attention',
                inputs=[self.uname(layer_id, 'ln_out'), attn_qkv_weight, attn_qkv_bias, i_attn_mask, i_kv_cache],
                outputs=[self.uname(layer_id, 'attn_out'), o_kv_cache],
                name=self.uname(layer_id, 'Attention'),
                domain='com.microsoft',
                num_heads=32,
                unidirectional=1,
                do_rotary=1,
            ),
            helper.make_node(
                'MatMul',
                inputs=[self.uname(layer_id, 'attn_out'), attn_out_weight],
                outputs=[self.uname(layer_id, 'matmul_out')],
                name=self.uname(layer_id, 'OutProj_MatMul'),
            ),
            helper.make_node(
                'Add',
                inputs=[self.uname(layer_id, 'matmul_out'), attn_out_bias],
                outputs=[self.uname(layer_id, 'add_out')],
                name=self.uname(layer_id, 'OutProj_Add'),
            ),
            helper.make_node(
                'MatMul',
                inputs=[self.uname(layer_id, 'ln_out'), mlp_fc1_weight],
                outputs=[self.uname(layer_id, 'fc1_w_out')],
                name=self.uname(layer_id, 'FC1_MatMul'),
            ),
            helper.make_node(
                'Add',
                inputs=[self.uname(layer_id, 'fc1_w_out'), mlp_fc1_bias],
                outputs=[self.uname(layer_id, 'fc1_b_out')],
                name=self.uname(layer_id, 'FC1_Bias'),
            ),
            helper.make_node(
                'NewGelu', # check what is it and use onnx inlining if needed
                inputs=[self.uname(layer_id, 'fc1_b_out')],
                outputs=[self.uname(layer_id, 'new_gelu_out')],
                name=self.uname(layer_id, 'NewGelu'),
                domain='com.microsoft', # fix later
            ),
            helper.make_node(
                'MatMul',
                inputs=[self.uname(layer_id, 'new_gelu_out'), mlp_fc2_weight],
                outputs=[self.uname(layer_id, 'fc2_w_out')],
                name=self.uname(layer_id, 'FC2_MatMul'),
            ),
            helper.make_node(
                'Add',
                inputs=[self.uname(layer_id, 'fc2_w_out'), mlp_fc2_bias],
                outputs=[self.uname(layer_id, 'fc2_b_out')],
                name=self.uname(layer_id, 'FC2_Bias'),
            ),
            helper.make_node(
                'Add',
                inputs=[self.uname(layer_id, 'attn_out'), self.uname(layer_id, 'fc2_b_out')],
                outputs=[self.uname(layer_id, 'residual_1_out')],
                name=self.uname(layer_id, 'Residual_Add_1'),
            ),
            helper.make_node(
                'Add',
                inputs=[i_hidden_states, self.uname(layer_id, 'residual_1_out')],
                outputs=[o_hidden_states],
                name=self.uname(layer_id, 'Residual_Add_2'),
            ),
        ]

        for new_node in subgraph_nodes:
            self.nodes_to_add.append(new_node)
            self.node_name_to_graph_name[new_node.name] = self.this_graph_name

        self.nodes_to_remove.append(node)
        self.prune_graph = True


class PhiOnnxModel(OnnxModel):
    def __init__(self, model: ModelProto, num_heads: int = 0, head_size: int = 0):
        super().__init__(model)
        self.fission_transformer_block = FissionTransformerBlockPhi(self)
        self.postprocess_causal_lm_head = PostProcessCausalLMHead(self)

    def inline_model(self):
        self.model = inliner.inline_local_functions(self.model, False)

    def postprocess(self):
        self.prune_graph()

    def optimize(self, options: Optional[FusionOptions] = None, add_dynamic_axes: bool = False):
        self.fission_transformer_block.apply()
        self.postprocess_causal_lm_head.apply()
        self.inline_model()

    def get_fused_operator_statistics(self):
        """
        Returns node count of fused operators.
        """
        op_count = {}
        # ops = [
        #     "EmbedLayerNormalization",
        #     "Attention",
        #     "MultiHeadAttention",
        #     "Gelu",
        #     "FastGelu",
        #     "BiasGelu",
        #     "GemmFastGelu",
        #     "LayerNormalization",
        #     "SimplifiedLayerNormalization",
        #     "SkipLayerNormalization",
        #     "SkipSimplifiedLayerNormalization",
        #     "RotaryEmbedding",
        # ]
        # q_ops = ["QOrderedAttention", "QOrderedGelu", "QOrderedLayerNormalization", "QOrderedMatMul"]
        # for op in ops + q_ops:
        #     nodes = self.get_nodes_by_op_type(op)
        #     op_count[op] = len(nodes)

        # logger.info(f"Optimized operators: {op_count}")
        return op_count

    def is_fully_optimized(self, fused_op_count=None):
        """
        Returns True when the model is fully optimized.
        """
        return False
