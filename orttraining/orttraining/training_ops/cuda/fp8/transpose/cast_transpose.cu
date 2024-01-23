/*************************************************************************
 * Copyright (c) 2022-2023, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * See LICENSE for license information.
 ************************************************************************/

#include <cuda_runtime.h>
#include <iostream>
#include <cfloat>

#include "orttraining/training_ops/cuda/fp8/common.h"
#include "orttraining/training_ops/cuda/fp8/utils.cuh"

namespace onnxruntime {
namespace cuda {
namespace fp8 {

template <bool full_tile, int nvec_in, int nvec_out, typename IVec, typename OVec, typename CType>
inline __device__ void cast_and_transpose_regs(const IVec (&in)[nvec_out], OVec (&out_trans)[nvec_in],
                                               typename OVec::type* output_cast_tile, const size_t current_place,
                                               const size_t stride,
                                               CType& max,  // NOLINT(*)
                                               const CType scale, const bool valid_store) {
  using T = typename OVec::type;
  using OVecC = Vec<T, nvec_in>;
#pragma unroll
  for (unsigned int i = 0; i < nvec_out; ++i) {
    OVecC out_cast;
#pragma unroll
    for (unsigned int j = 0; j < nvec_in; ++j) {
      const CType tmp = static_cast<CType>(in[i].data.elt[j]);
      const T elt_o = T(scale * tmp);

      out_cast.data.elt[j] = elt_o;
      out_trans[j].data.elt[i] = elt_o;  // thread tile transpose

      __builtin_assume(max >= 0);
      max = fmaxf(fabsf(tmp), max);
    }
    if (full_tile || valid_store) {
      out_cast.store_to(output_cast_tile, current_place + stride * i);
    }
  }
}

// STUFF TO TUNE
constexpr unsigned int n_warps_per_tile = 4;

constexpr unsigned int max_threads_per_block = 256;
static_assert(n_warps_per_tile * THREADS_PER_WARP <= max_threads_per_block);
constexpr unsigned int cast_transpose_num_threads = n_warps_per_tile * THREADS_PER_WARP;

template <int nvec_in, int nvec_out, typename CType, typename IType, typename OType>
__global__ void __launch_bounds__(cast_transpose_num_threads)
    cast_transpose_kernel(const IType* const input, OType* const output_c, OType* const output_t,
                          const CType* const scale_ptr, CType* const amax, const size_t row_length,
                          const size_t num_rows, const size_t num_tiles) {
  using IVec = Vec<IType, nvec_in>;
  using OVec = Vec<OType, nvec_out>;

  extern __shared__ char scratch[];

  const int warp_id = threadIdx.x / THREADS_PER_WARP;
  const int my_id_in_warp = threadIdx.x % THREADS_PER_WARP;
  const size_t num_tiles_x = row_length / (nvec_in * THREADS_PER_WARP);
  const size_t tile_id = blockIdx.x * blockDim.x / (THREADS_PER_WARP * n_warps_per_tile) + warp_id / n_warps_per_tile;
  if (tile_id >= num_tiles) return;
  const size_t tile_id_x = tile_id % num_tiles_x;
  const size_t tile_id_y = tile_id / num_tiles_x;

  const IType* const my_input_tile =
      input + (tile_id_x * nvec_in + tile_id_y * row_length * nvec_out) * THREADS_PER_WARP;
  OType* const my_output_c_tile =
      output_c + (tile_id_x * nvec_in + tile_id_y * row_length * nvec_out) * THREADS_PER_WARP;
  OType* const my_output_t_tile = output_t + (tile_id_y * nvec_out + tile_id_x * num_rows * nvec_in) * THREADS_PER_WARP;
  OVec* const my_scratch = reinterpret_cast<OVec*>(scratch) +
                           (my_id_in_warp + warp_id / n_warps_per_tile * THREADS_PER_WARP) * (THREADS_PER_WARP + 1);

  IVec in[2][nvec_out];
  const unsigned int warp_id_in_tile = warp_id % n_warps_per_tile;
  constexpr unsigned int n_iterations = THREADS_PER_WARP / n_warps_per_tile;
  OVec out_space[n_iterations][nvec_in];

  const size_t stride = row_length / nvec_in;
  const size_t output_stride = num_rows / nvec_out;
  size_t current_stride = warp_id_in_tile * n_iterations * nvec_out * stride;
  unsigned int my_place = (my_id_in_warp + THREADS_PER_WARP - warp_id_in_tile * n_iterations) % THREADS_PER_WARP;
  CType max = 0;
  const CType scale = scale_ptr != nullptr ? *scale_ptr : 1;
#pragma unroll
  for (unsigned int i = 0; i < nvec_out; ++i) {
    in[0][i].load_from(my_input_tile, current_stride + my_place + stride * i);
  }
#pragma unroll
  for (unsigned int i = 0; i < n_iterations; ++i) {
    const size_t current_place = current_stride + my_place;
    const unsigned int my_place_in = (my_place + THREADS_PER_WARP - 1) % THREADS_PER_WARP;
    const unsigned int current_in = (i + 1) % 2;
    if (i < n_iterations - 1) {
#pragma unroll
      for (unsigned int j = 0; j < nvec_out; ++j) {
        in[current_in][j].load_from(my_input_tile, current_stride + my_place_in + stride * (nvec_out + j));
      }
    }
    OVec out_trans[nvec_in];  // NOLINT(*)
    cast_and_transpose_regs<true>(in[current_in ^ 1], out_trans, my_output_c_tile, current_place, stride, max, scale,
                                  true);
#pragma unroll
    for (unsigned int j = 0; j < nvec_in; ++j) {
      out_space[i][j].data.vec = out_trans[j].data.vec;
    }
    my_place = (my_place + THREADS_PER_WARP - 1) % THREADS_PER_WARP;
    current_stride += nvec_out * stride;
  }

  for (unsigned int i = 0; i < nvec_in; ++i) {
#pragma unroll
    for (unsigned int j = 0; j < n_iterations; ++j) {
      my_scratch[(my_id_in_warp + THREADS_PER_WARP - j - warp_id_in_tile * n_iterations) % THREADS_PER_WARP] =
          out_space[j][i];
    }
    __syncthreads();
    my_place = (my_id_in_warp + THREADS_PER_WARP - warp_id_in_tile * n_iterations) % THREADS_PER_WARP;
    current_stride = i * output_stride + warp_id_in_tile * n_iterations * output_stride * nvec_in;
    for (unsigned int j = 0; j < n_iterations; ++j) {
      my_scratch[j + warp_id_in_tile * n_iterations].store_to(my_output_t_tile, current_stride + my_place);
      my_place = (my_place + THREADS_PER_WARP - 1) % THREADS_PER_WARP;
      current_stride += output_stride * nvec_in;
    }
    __syncthreads();
  }

  /* warp tile amax reduce*/
  max = reduce_max<cast_transpose_num_threads / THREADS_PER_WARP>(max, warp_id);

  if (threadIdx.x == 0) {
    static_assert(std::is_same<CType, float>::value);
    if (amax != nullptr) atomicMaxFloat(amax, max);
  }
}

template <int nvec_in, int nvec_out, typename CType, typename IType, typename OType>
__global__ void __launch_bounds__(cast_transpose_num_threads)
    cast_transpose_kernel_notaligned(const IType* const input, OType* const output_c, OType* const output_t,
                                     const CType* const scale_ptr, CType* const amax, const size_t row_length,
                                     const size_t num_rows, const size_t num_tiles) {
  using IVec = Vec<IType, nvec_in>;
  using OVec = Vec<OType, nvec_out>;

  extern __shared__ char scratch[];

  const int warp_id = threadIdx.x / THREADS_PER_WARP;
  const int my_id_in_warp = threadIdx.x % THREADS_PER_WARP;
  const size_t num_tiles_x = (row_length + nvec_in * THREADS_PER_WARP - 1) / (nvec_in * THREADS_PER_WARP);
  const size_t tile_id = blockIdx.x * blockDim.x / (THREADS_PER_WARP * n_warps_per_tile) + warp_id / n_warps_per_tile;
  if (tile_id >= num_tiles) return;
  const size_t tile_id_x = tile_id % num_tiles_x;
  const size_t tile_id_y = tile_id / num_tiles_x;

  const IType* const my_input_tile =
      input + (tile_id_x * nvec_in + tile_id_y * row_length * nvec_out) * THREADS_PER_WARP;
  OType* const my_output_c_tile =
      output_c + (tile_id_x * nvec_in + tile_id_y * row_length * nvec_out) * THREADS_PER_WARP;
  OType* const my_output_t_tile = output_t + (tile_id_y * nvec_out + tile_id_x * num_rows * nvec_in) * THREADS_PER_WARP;
  const size_t stride = row_length / nvec_in;
  const size_t output_stride = num_rows / nvec_out;
  const size_t row_length_rest = stride - tile_id_x * THREADS_PER_WARP;
  const size_t row_height_rest = output_stride - tile_id_y * THREADS_PER_WARP;
  const unsigned int tile_length = row_length_rest > THREADS_PER_WARP ? THREADS_PER_WARP : row_length_rest;
  const unsigned int tile_height = row_height_rest > THREADS_PER_WARP ? THREADS_PER_WARP : row_height_rest;

  OVec* const my_scratch = reinterpret_cast<OVec*>(scratch) +
                           (my_id_in_warp + warp_id / n_warps_per_tile * THREADS_PER_WARP) * (THREADS_PER_WARP + 1);

  IVec in[2][nvec_out];
  const unsigned int warp_id_in_tile = warp_id % n_warps_per_tile;
  constexpr unsigned int n_iterations = THREADS_PER_WARP / n_warps_per_tile;
  OVec out_space[n_iterations][nvec_in];

  size_t current_stride = warp_id_in_tile * n_iterations * nvec_out * stride;
  unsigned int my_place = (my_id_in_warp + THREADS_PER_WARP - warp_id_in_tile * n_iterations) % THREADS_PER_WARP;
  CType max = 0;
  const CType scale = scale_ptr != nullptr ? *scale_ptr : 1;
  {
    const bool valid_load = my_place < tile_length && warp_id_in_tile * n_iterations < tile_height;
#pragma unroll
    for (unsigned int i = 0; i < nvec_out; ++i) {
      if (valid_load) {
        in[0][i].load_from(my_input_tile, current_stride + my_place + stride * i);
      } else {
        in[0][i].clear();
      }
    }
  }
#pragma unroll
  for (unsigned int i = 0; i < n_iterations; ++i) {
    const size_t current_place = current_stride + my_place;
    const unsigned int my_place_in = (my_place + THREADS_PER_WARP - 1) % THREADS_PER_WARP;
    const unsigned int current_in = (i + 1) % 2;
    if (i < n_iterations - 1) {
      const bool valid_load = my_place_in < tile_length && warp_id_in_tile * n_iterations + i + 1 < tile_height;
#pragma unroll
      for (unsigned int j = 0; j < nvec_out; ++j) {
        if (valid_load) {
          in[current_in][j].load_from(my_input_tile, current_stride + my_place_in + stride * (nvec_out + j));
        } else {
          in[current_in][j].clear();
        }
      }
    }
    OVec out_trans[nvec_in];  // NOLINT(*)
    const bool valid_store = my_place < tile_length && warp_id_in_tile * n_iterations + i < tile_height;
    cast_and_transpose_regs<false>(in[current_in ^ 1], out_trans, my_output_c_tile, current_place, stride, max, scale,
                                   valid_store);
#pragma unroll
    for (unsigned int j = 0; j < nvec_in; ++j) {
      out_space[i][j].data.vec = out_trans[j].data.vec;
    }
    my_place = (my_place + THREADS_PER_WARP - 1) % THREADS_PER_WARP;
    current_stride += nvec_out * stride;
  }

  for (unsigned int i = 0; i < nvec_in; ++i) {
#pragma unroll
    for (unsigned int j = 0; j < n_iterations; ++j) {
      my_scratch[(my_id_in_warp + THREADS_PER_WARP - j - warp_id_in_tile * n_iterations) % THREADS_PER_WARP] =
          out_space[j][i];
    }
    __syncthreads();
    my_place = (my_id_in_warp + THREADS_PER_WARP - warp_id_in_tile * n_iterations) % THREADS_PER_WARP;
    current_stride = i * output_stride + warp_id_in_tile * n_iterations * output_stride * nvec_in;
    for (unsigned int j = 0; warp_id_in_tile * n_iterations + j < tile_length; ++j) {
      const bool valid_store = my_place < tile_height;
      if (valid_store) {
        my_scratch[j + warp_id_in_tile * n_iterations].store_to(my_output_t_tile, current_stride + my_place);
      }
      my_place = (my_place + THREADS_PER_WARP - 1) % THREADS_PER_WARP;
      current_stride += output_stride * nvec_in;
    }
    __syncthreads();
  }

  /* warp tile amax reduce*/
  max = reduce_max<cast_transpose_num_threads / THREADS_PER_WARP>(max, warp_id);

  if (threadIdx.x == 0) {
    static_assert(std::is_same<CType, float>::value);
    if (amax != nullptr) atomicMaxFloat(amax, max);
  }
}

template <typename InputType, typename OutputType>
void CastTranspose(cudaStream_t stream, const InputType* input_data, OutputType* cast_output_data,
                   OutputType* transposed_output_data, const fp32* scale, fp32* amax, const size_t row_length,
                   const size_t num_rows) {
  typedef typename MappedType<InputType>::CudaType CudaInputType;
  typedef typename MappedType<OutputType>::CudaType CudaOutputType;
  const CudaInputType* cuda_input_data = reinterpret_cast<const CudaInputType*>(input_data);
  CudaOutputType* cuda_cast_output_data = reinterpret_cast<CudaOutputType*>(cast_output_data);
  CudaOutputType* cuda_transposed_output_data = reinterpret_cast<CudaOutputType*>(transposed_output_data);

// Launch specific cast-transpose kernel
#define LAUNCH_KERNEL(kernel, nvec_in, nvec_out, n_tiles, n_blocks, CudaInputType, CudaOutputType)                 \
  do {                                                                                                             \
    cudaFuncSetAttribute(kernel<nvec_in, nvec_out, fp32, CudaInputType, CudaOutputType>,                           \
                         cudaFuncAttributePreferredSharedMemoryCarveout, 100);                                     \
    kernel<nvec_in, nvec_out, fp32, CudaInputType, CudaOutputType>                                                 \
        <<<n_blocks, cast_transpose_num_threads,                                                                   \
           cast_transpose_num_threads / n_warps_per_tile*(THREADS_PER_WARP + 1) *                                  \
               sizeof(Vec<CudaOutputType, nvec_out>),                                                              \
           stream>>>(cuda_input_data, cuda_cast_output_data, cuda_transposed_output_data, scale, amax, row_length, \
                     num_rows, n_tiles);                                                                           \
  } while (false)

// Launch cast-transpose kernel for given vector sizes
#define LAUNCH_KERNEL_VEC_SIZES(load_size, store_size, CudaInputType, CudaOutputType)                            \
  do {                                                                                                           \
    constexpr int nvec_in = load_size / sizeof(CudaInputType);                                                   \
    constexpr int nvec_out = store_size / sizeof(CudaOutputType);                                                \
    const size_t n_tiles = get_n_tiles(load_size, store_size);                                                   \
    const size_t n_blocks = get_n_blocks(n_tiles);                                                               \
                                                                                                                 \
    const bool full_tile =                                                                                       \
        row_length % (nvec_in * THREADS_PER_WARP) == 0 && num_rows % (nvec_out * THREADS_PER_WARP) == 0;         \
                                                                                                                 \
    if (full_tile) {                                                                                             \
      LAUNCH_KERNEL(cast_transpose_kernel, nvec_in, nvec_out, n_tiles, n_blocks, CudaInputType, CudaOutputType); \
    } else {                                                                                                     \
      LAUNCH_KERNEL(cast_transpose_kernel_notaligned, nvec_in, nvec_out, n_tiles, n_blocks, CudaInputType,       \
                    CudaOutputType);                                                                             \
    }                                                                                                            \
  } while (false)

  // Estimate number of SMs
  // Note: H100 has 132 SMs, A100 has 108 SMs.
  // Note: Directly querying number of SMs with cudaGetDeviceProperties is
  // slow (>1 ms). Consider querying once and caching.
  const int n_sms = 128;

  // Helper functions to get kernel configuration
  auto get_n_tiles = [=](size_t load_size, size_t store_size) -> int {
    constexpr size_t threads_per_warp = static_cast<size_t>(THREADS_PER_WARP);
    size_t nvec_in = load_size / sizeof(CudaInputType);
    size_t nvec_out = store_size / sizeof(CudaOutputType);
    size_t n_tiles = DIVUP(row_length, nvec_in * threads_per_warp) * DIVUP(num_rows, nvec_out * threads_per_warp);
    return n_tiles;
  };
  auto get_n_blocks = [=](size_t n_tiles) -> int {
    size_t n_warps_per_block = cast_transpose_num_threads / THREADS_PER_WARP;
    size_t n_blocks = DIVUP(n_tiles * n_warps_per_tile, n_warps_per_block);
    return n_blocks;
  };

  // Estimate optimal vector sizes and run
  // Note: Consider reducing to 2B or 1B loads/stores for
  // sufficiently small matrices. Need to consider whether reduced
  // cache efficiency is worth increased SM utilization. Also need
  // to keep in mind whether datatype can fit.
  const size_t estimated_n_tiles = get_n_tiles(8, 8);
  const size_t estimated_n_blocks = get_n_blocks(estimated_n_tiles);
  if (estimated_n_blocks >= n_sms) {
    LAUNCH_KERNEL_VEC_SIZES(8, 8, CudaInputType, CudaOutputType);
  } else {
    LAUNCH_KERNEL_VEC_SIZES(4, 4, CudaInputType, CudaOutputType);
  }

#undef LAUNCH_KERNEL
#undef LAUNCH_KERNEL_VEC_SIZES
}

#define SPECIALIZED_CAST_TRANSPOSE_IMPL(InputType, OutputType)                                                         \
  template void CastTranspose<InputType, OutputType>(cudaStream_t stream, const InputType* input_data,                 \
                                                     OutputType* cast_output_data, OutputType* transposed_output_data, \
                                                     const fp32* scale, fp32* amax, const size_t row_length,           \
                                                     const size_t num_rows);

SPECIALIZED_CAST_TRANSPOSE_IMPL(MLFloat16, Float8E4M3FN)
SPECIALIZED_CAST_TRANSPOSE_IMPL(MLFloat16, Float8E5M2)

#undef SPECIALIZED_CAST_TRANSPOSE_IMPL

}  // namespace fp8
}  // namespace cuda
}  // namespace onnxruntime
