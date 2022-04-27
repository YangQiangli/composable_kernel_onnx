#ifndef CK_GRIDWISE_GEMM_AVX2_HPP
#define CK_GRIDWISE_GEMM_AVX2_HPP

#include "common_header.hpp"
#include "multi_index_transform_helper.hpp"
#include "tensor_descriptor.hpp"
#include "tensor_descriptor_helper.hpp"
#include "blockwise_gemm_avx2.hpp"
#include "threadwise_tensor_slice_transfer_avx2.hpp"
#include "threadwise_tensor_slice_transfer_avx2_specialization.hpp"
#include "dynamic_buffer_cpu.hpp"
#include <unistd.h>

namespace ck {
namespace cpu {

template <typename GridwiseGemm,
          typename FloatA,
          typename FloatB,
          typename FloatC,
          typename AGridDesc,
          typename BGridDesc,
          typename CGridDesc,
          typename AElementwiseOperation,
          typename BElementwiseOperation,
          typename CElementwiseOperation>
void kernel_gemm_avx_mxn(const FloatA* __restrict__ p_a_grid,
                         const FloatB* __restrict__ p_b_grid,
                         FloatC* __restrict__ p_c_grid,
                         const AGridDesc& a_grid_desc,
                         const BGridDesc& b_grid_desc,
                         const CGridDesc& c_grid_desc,
                         const AElementwiseOperation& a_element_op,
                         const BElementwiseOperation& b_element_op,
                         const CElementwiseOperation& c_element_op)
{
    GridwiseGemm::Run(p_a_grid,
                      p_b_grid,
                      p_c_grid,
                      a_grid_desc,
                      b_grid_desc,
                      c_grid_desc,
                      a_element_op,
                      b_element_op,
                      c_element_op);
}

template <typename FloatA,
          typename FloatB,
          typename FloatC,
          typename AGridDesc,
          typename BGridDesc,
          typename CGridDesc,
          typename AElementwiseOperation,
          typename BElementwiseOperation,
          typename CElementwiseOperation,
          ck::index_t MPerBlock, // block means data are designed to fit in cache (L1/L2/L3)
          ck::index_t NPerBlock,
          ck::index_t KPerBlock,
          typename ThreadwiseGemm_Dispatch,
          typename AThreadwiseCopy,
          typename BThreadwiseCopy,
          typename CThreadwiseCopy,
          typename BlockMNKAccessOrder, // how we accss gemm MNK to better fit in cache
          typename ThreadMNAccessOrder, // how we acces gemm MN to utilize micro kernel
          bool UseALocalBuffer,
          bool UseBLocalBuffer,
          bool UseCLocalBuffer // if true, will allocate a buffer and write to it in kernel, then
                               // copy back to block buffer (need CThreadwiseCopy).
                               // if false, will write to C directly (no need CThreadwiseCopy)
          >
struct GridwiseGemmAvx2_MxN
{
    static constexpr auto I0 = Number<0>{};
    static constexpr auto I1 = Number<1>{};

    // static constexpr auto Avx2RegisterVector = 8;   // 8 floats
    static constexpr index_t MemAlignmentByte = 32; // 256bit

    static auto GetABlockDescriptor(const ck::index_t m_per_blk, const ck::index_t k_per_blk)
    {
        if constexpr(std::is_same<typename ThreadwiseGemm_Dispatch::MatrixALayout,
                                  ck::tensor_layout::gemm::RowMajor>::value)
        {
            // A : M, K
            auto a_block_desc_m_k =
                make_naive_tensor_descriptor_packed(make_tuple(m_per_blk, k_per_blk));
            return a_block_desc_m_k;
        }
        else
        {
            // A : K, M
            auto a_block_desc_k_m = make_naive_tensor_descriptor_packed(
                make_tuple(k_per_blk,
                           math::integer_least_multiple(
                               m_per_blk, ThreadwiseGemm_Dispatch::MatrixAMinVectorSize)));
            return a_block_desc_k_m;
        }
    }

    static auto GetBBlockDescriptor(const ck::index_t k_per_blk, const ck::index_t n_per_blk)
    {
        // n_per_blk should be 8x
        if constexpr(std::is_same<typename ThreadwiseGemm_Dispatch::MatrixBLayout,
                                  ck::tensor_layout::gemm::RowMajor>::value)
        {
            // B : K, N
            auto b_block_desc_k_n =
                make_naive_tensor_descriptor_packed(make_tuple(k_per_blk, n_per_blk));
            return b_block_desc_k_n;
        }
        else
        {
            // B : N/8, K, N8
            auto b_block_desc_n0_k_n1 = make_naive_tensor_descriptor_packed(make_tuple(
                math::integer_divide_ceil(n_per_blk, ThreadwiseGemm_Dispatch::MatrixBMinVectorSize),
                k_per_blk,
                ThreadwiseGemm_Dispatch::MatrixBMinVectorSize));
            return b_block_desc_n0_k_n1;
        }
    }

    static auto GetCBlockDescriptor(const ck::index_t m_per_blk, const ck::index_t n_per_blk)
    {
        return make_naive_tensor_descriptor_packed(make_tuple(m_per_blk, n_per_blk));
    }

    static constexpr bool CheckValidity(const AGridDesc& a_grid_desc,
                                        const BGridDesc& b_grid_desc,
                                        const CGridDesc& c_grid_desc)
    {
        // TODO: also check validity of all components (blockwise-copy, threadwise-copy, etc)
        bool is_valid    = true;
        const auto GemmN = c_grid_desc.GetLength(I1);
        if constexpr(UseCLocalBuffer)
        {
            if(std::is_same<BlockMNKAccessOrder, ck::Sequence<0, 2, 1>>::value && NPerBlock < GemmN)
                is_valid &= false;
        }
        else
        {
            // TODO: need check c grid is simple transform?
            if(GemmN % 8 != 0)
                is_valid &= false;
        }
        return is_valid;
    }

    static void Run(const FloatA* __restrict__ p_a_grid,
                    const FloatB* __restrict__ p_b_grid,
                    FloatC* __restrict__ p_c_grid,
                    const AGridDesc& a_grid_desc,
                    const BGridDesc& b_grid_desc,
                    const CGridDesc& c_grid_desc,
                    const AElementwiseOperation& a_element_op,
                    const BElementwiseOperation& b_element_op,
                    const CElementwiseOperation& c_element_op)
    {
        ck::index_t m_per_block = MPerBlock;
        ck::index_t n_per_block = NPerBlock;
        ck::index_t k_per_block = KPerBlock;

        const auto GemmM = c_grid_desc.GetLength(I0);
        const auto GemmN = c_grid_desc.GetLength(I1);
        const auto GemmK = a_grid_desc.GetLength(I1);

        constexpr auto a_block_copy_dim = AGridDesc::GetNumOfDimension();

        constexpr auto b_block_copy_dim = BGridDesc::GetNumOfDimension();

        auto a_threadwise_copy = AThreadwiseCopy(a_grid_desc,
                                                 ck::make_zero_multi_index<a_block_copy_dim>(),
                                                 GetABlockDescriptor(m_per_block, k_per_block),
                                                 ck::make_zero_multi_index<a_block_copy_dim>(),
                                                 AElementwiseOperation{});

        auto b_threadwise_copy = BThreadwiseCopy(b_grid_desc,
                                                 ck::make_zero_multi_index<b_block_copy_dim>(),
                                                 GetBBlockDescriptor(k_per_block, n_per_block),
                                                 ck::make_zero_multi_index<b_block_copy_dim>(),
                                                 BElementwiseOperation{});

        auto c_threadwise_copy = CThreadwiseCopy(GetCBlockDescriptor(m_per_block, n_per_block),
                                                 ck::make_zero_multi_index<2>(),
                                                 c_grid_desc,
                                                 ck::make_zero_multi_index<2>(),
                                                 CElementwiseOperation{});

        DeviceAlignedMemCPU a_block_mem(m_per_block * k_per_block * sizeof(FloatA),
                                        MemAlignmentByte);
        DeviceAlignedMemCPU b_block_mem(k_per_block * n_per_block * sizeof(FloatB),
                                        MemAlignmentByte);
        DeviceAlignedMemCPU c_block_mem(m_per_block * n_per_block * sizeof(FloatC),
                                        MemAlignmentByte);

        auto a_grid_buf = ck::cpu::make_dynamic_buffer<ck::AddressSpaceEnum::Global>(
            reinterpret_cast<const FloatA*>(p_a_grid), a_grid_desc.GetElementSpaceSize());

        auto b_grid_buf = ck::cpu::make_dynamic_buffer<ck::AddressSpaceEnum::Global>(
            reinterpret_cast<const FloatB*>(p_b_grid), b_grid_desc.GetElementSpaceSize());

        auto c_grid_buf = ck::cpu::make_dynamic_buffer<ck::AddressSpaceEnum::Global>(
            reinterpret_cast<FloatC*>(p_c_grid), c_grid_desc.GetElementSpaceSize());

        auto a_block_buf = ck::cpu::make_dynamic_buffer<ck::AddressSpaceEnum::Global>(
            reinterpret_cast<FloatA*>(a_block_mem.mpDeviceBuf),
            a_block_mem.mMemSize / sizeof(FloatA));

        auto b_block_buf = ck::cpu::make_dynamic_buffer<ck::AddressSpaceEnum::Global>(
            reinterpret_cast<FloatB*>(b_block_mem.mpDeviceBuf),
            b_block_mem.mMemSize / sizeof(FloatB));

        auto c_block_buf = ck::cpu::make_dynamic_buffer<ck::AddressSpaceEnum::Global>(
            UseCLocalBuffer ? reinterpret_cast<FloatC*>(c_block_mem.mpDeviceBuf)
                            : reinterpret_cast<FloatC*>(p_c_grid),
            UseCLocalBuffer ? c_block_mem.mMemSize / sizeof(FloatC)
                            : c_grid_desc.GetElementSpaceSize());

        auto blockwise_gemm = BlockwiseGemmAvx2_MxN<
            FloatA,                                                  // FloatA,
            FloatB,                                                  // FloatB,
            FloatC,                                                  // FloatC,
            decltype(GetABlockDescriptor(m_per_block, k_per_block)), // ABlockDesc,
            decltype(GetBBlockDescriptor(k_per_block, n_per_block)), // BBlockDesc,
            decltype(GetCBlockDescriptor(m_per_block, n_per_block)), // CBlockDesc,
            KPerBlock,                                               // KPerBlock,
            ThreadwiseGemm_Dispatch,                                 // ThreadwiseGemm_Dispatch,
            ThreadMNAccessOrder>{}; // ThreadMNAccessOrder  // how we acces
                                    // gemm MN to utilize micro kernel>{};

        // TODO: openmp aware ordering
        //
        if constexpr(std::is_same<BlockMNKAccessOrder, ck::Sequence<0, 1, 2>>::value)
        {
            auto a_move_k_step       = ck::make_multi_index(0, k_per_block);
            auto b_move_k_step       = ck::make_multi_index(0, k_per_block, 0);
            const ck::index_t grid_m = math::integer_divide_ceil(GemmM, m_per_block);
            const ck::index_t grid_n = math::integer_divide_ceil(GemmN, n_per_block);

            const ck::index_t grid_size = grid_m * grid_n;
// This version does not consider K panel re-usage. simple for openmp
#pragma omp parallel for
            for(ck::index_t gid = 0; gid < grid_size; gid++)
            {
                ck::index_t i_mc = (gid / grid_n) * m_per_block;
                ck::index_t i_nc = (gid % grid_n) * n_per_block;

                ck::index_t mc_size = ck::math::min(GemmM - i_mc, m_per_block);
                ck::index_t nc_size =
                    ck::math::min(GemmN - i_nc, n_per_block); // TODO: nc need be 8x
                nc_size = math::integer_least_multiple(
                    nc_size, ThreadwiseGemm_Dispatch::MatrixBMinVectorSize);

                a_threadwise_copy.SetSrcSliceOrigin(a_grid_desc, ck::make_multi_index(i_mc, 0));
                b_threadwise_copy.SetSrcSliceOrigin(
                    b_grid_desc,
                    ck::make_multi_index(math::integer_divide_ceil(
                                             i_nc, ThreadwiseGemm_Dispatch::MatrixBMinVectorSize),
                                         0,
                                         0));

                auto c_block_desc =
                    UseCLocalBuffer ? GetCBlockDescriptor(mc_size, nc_size) : c_grid_desc;
                if constexpr(UseCLocalBuffer)
                {
                    c_threadwise_copy.SetDstSliceOrigin(c_grid_desc,
                                                        ck::make_multi_index(i_mc, i_nc));
                }
                else
                {
                    c_threadwise_copy.SetSrcSliceOrigin(c_block_desc,
                                                        ck::make_multi_index(i_mc, i_nc));
                    c_threadwise_copy.Run(c_block_desc, c_block_buf, c_grid_desc, c_grid_buf);
                }

                for(ck::index_t i_kc = 0; i_kc < GemmK; i_kc += k_per_block)
                {
                    ck::index_t kc_size = ck::math::min(GemmK - i_kc, k_per_block);

                    auto a_block_desc = GetABlockDescriptor(mc_size, kc_size);
                    auto b_block_desc = GetBBlockDescriptor(kc_size, nc_size);

                    // printf("==> i_m:%d, i_n:%d, i_k:%d, mc:%d, nc:%d, kc:%d(%d, %d)\n", i_mc,
                    // i_nc, i_kc, mc_size, nc_size, kc_size, KPerBlock, GemmK); fflush(stdout);

                    a_threadwise_copy.Run(a_grid_desc, a_grid_buf, a_block_desc, a_block_buf);

                    b_threadwise_copy.Run(b_grid_desc, b_grid_buf, b_block_desc, b_block_buf);

                    // for(auto i_elem = 0; i_elem < (mc_size * kc_size) ; i_elem++){
                    //    printf("A ==> %3d : %f(0x%08x)\n", i_elem,
                    //    (reinterpret_cast<float*>(a_block_buf.p_data_))[i_elem],
                    //    (reinterpret_cast<uint32_t*>(a_block_buf.p_data_))[i_elem]);
                    //}

                    // for(auto i_elem = 0; i_elem < (kc_size * nc_size) ; i_elem++){
                    //     printf("B ==> %3d : %f(0x%08x)\n", i_elem,
                    //     (reinterpret_cast<float*>(b_block_buf.p_data_))[i_elem],
                    //     (reinterpret_cast<uint32_t*>(b_block_buf.p_data_))[i_elem]);
                    // }
                    // printf("[%d] 2222 \n",__LINE__);
                    blockwise_gemm.Run(a_block_desc,
                                       a_block_buf,
                                       make_zero_multi_index<a_block_copy_dim>(),
                                       b_block_desc,
                                       b_block_buf,
                                       make_zero_multi_index<b_block_copy_dim>(),
                                       c_block_desc,
                                       c_block_buf,
                                       make_zero_multi_index<2>(),
                                       i_kc != 0);

                    // printf("[%d] 2222 \n",__LINE__);
                    if((i_kc + k_per_block) < GemmK)
                    {
                        a_threadwise_copy.MoveSrcSliceWindow(a_grid_desc, a_move_k_step);
                        b_threadwise_copy.MoveSrcSliceWindow(b_grid_desc, b_move_k_step);
                    }

                    // printf("[%d] 2222 \n",__LINE__);

                    // for(auto i_elem = 0; i_elem < (10) ; i_elem++){
                    //     printf("C ==> %3d : %f(0x%08x)\n", i_elem,
                    //     (reinterpret_cast<float*>(c_block_buf.p_data_))[i_elem],
                    //     (reinterpret_cast<uint32_t*>(c_block_buf.p_data_))[i_elem]);
                    // }
                }

                // for(auto i_elem = 0; i_elem < (c_block_mem.mMemSize / sizeof(FloatC)) ;
                // i_elem++){
                //     printf("C ==> %3d : %f(0x%08x)\n", i_elem,
                //     (reinterpret_cast<float*>(c_block_buf.p_data_))[i_elem],
                //     (reinterpret_cast<uint32_t*>(c_block_buf.p_data_))[i_elem]);
                // }
                if constexpr(UseCLocalBuffer)
                    c_threadwise_copy.Run(c_block_desc, c_block_buf, c_grid_desc, c_grid_buf);
            }
        }
        else if constexpr(std::is_same<BlockMNKAccessOrder, ck::Sequence<0, 2, 1>>::value)
        {
            auto a_move_k_step = ck::make_multi_index(0, k_per_block);
            auto b_move_k_step = ck::make_multi_index(
                math::integer_divide_ceil(n_per_block,
                                          ThreadwiseGemm_Dispatch::MatrixBMinVectorSize),
                0,
                0);

// only parallel in gemm m dim
#pragma omp parallel for
            for(ck::index_t i_mc = 0; i_mc < GemmM; i_mc += m_per_block)
            {
                ck::index_t mc_size = ck::math::min(GemmM - i_mc, m_per_block);
                a_threadwise_copy.SetSrcSliceOrigin(a_grid_desc, ck::make_multi_index(i_mc, 0));
                for(ck::index_t i_kc = 0; i_kc < GemmK; i_kc += k_per_block)
                {
                    ck::index_t kc_size = ck::math::min(GemmK - i_kc, k_per_block);

                    auto a_block_desc = GetABlockDescriptor(mc_size, kc_size);
                    a_threadwise_copy.Run(a_grid_desc, a_grid_buf, a_block_desc, a_block_buf);

                    b_threadwise_copy.SetSrcSliceOrigin(b_grid_desc,
                                                        ck::make_multi_index(0, i_kc, 0));

                    // TODO: if use local C buffer, then this nc loop need to loop only once
                    for(ck::index_t i_nc = 0; i_nc < GemmN; i_nc += n_per_block)
                    {
                        ck::index_t nc_size =
                            ck::math::min(GemmN - i_nc, n_per_block); // TODO: nc need be 8x
                        nc_size = math::integer_least_multiple(
                            nc_size, ThreadwiseGemm_Dispatch::MatrixBMinVectorSize);

                        auto b_block_desc = GetBBlockDescriptor(kc_size, nc_size);

                        b_threadwise_copy.Run(b_grid_desc, b_grid_buf, b_block_desc, b_block_buf);

                        auto c_block_desc =
                            UseCLocalBuffer ? GetCBlockDescriptor(mc_size, nc_size) : c_grid_desc;

                        if constexpr(!UseCLocalBuffer)
                        {
                            c_threadwise_copy.SetSrcSliceOrigin(c_block_desc,
                                                                ck::make_multi_index(i_mc, i_nc));
                            c_threadwise_copy.Run(
                                c_block_desc, c_block_buf, c_grid_desc, c_grid_buf);
                        }

                        blockwise_gemm.Run(a_block_desc,
                                           a_block_buf,
                                           make_zero_multi_index<a_block_copy_dim>(),
                                           b_block_desc,
                                           b_block_buf,
                                           make_zero_multi_index<b_block_copy_dim>(),
                                           c_block_desc,
                                           c_block_buf,
                                           make_zero_multi_index<2>(),
                                           i_kc != 0);

                        if((i_nc + n_per_block) < GemmN)
                            b_threadwise_copy.MoveSrcSliceWindow(b_grid_desc, b_move_k_step);

                        if constexpr(UseCLocalBuffer)
                        {
                            c_threadwise_copy.SetDstSliceOrigin(c_grid_desc,
                                                                ck::make_multi_index(i_mc, i_nc));
                            c_threadwise_copy.Run(
                                c_block_desc, c_block_buf, c_grid_desc, c_grid_buf);
                        }
                    }

                    if((i_kc + k_per_block) < GemmK)
                        a_threadwise_copy.MoveSrcSliceWindow(a_grid_desc, a_move_k_step);
                }
            }
        }
    }
};

} // namespace cpu
} // namespace ck

#endif