/******************************************************************************
 * Copyright (c) 2024, Jay Shah, Ganesh Bikshandi, Ying Zhang, Vijay Thakkar, Pradeep Ramani, Tri Dao.
 ******************************************************************************/

#pragma once

#include "cute/tensor.hpp"

#include <cutlass/cutlass.h>
#include <cutlass/arch/memory.h>
#include <cutlass/array.h>
#include <cutlass/numeric_types.h>
#include <cutlass/numeric_conversion.h>

#include "utils.h"

namespace flash {

using namespace cute;

template <class TileShape_MK_, int kLogMaxSplits_, int kNThreads, int AlignmentLSE_,
          bool Is_even_K, bool Varlen, class Element, class ElementAccum, class ArchTag_>
class FlashAttnFwdCombine {

public:

    // Type Aliases
    using TileShape_MK = TileShape_MK_;
    using ArchTag = ArchTag_;
    static constexpr int kMaxSplits = 1 << kLogMaxSplits_;
    static constexpr int AlignmentLSE = std::min(AlignmentLSE_, int(128 / 8 / sizeof(float)));
    static_assert(AlignmentLSE >= 1);
    static constexpr int kStages = 4;

    static_assert(ArchTag::kMinComputeCapability >= 75);
    static constexpr bool Has_cp_async = ArchTag::kMinComputeCapability >= 80;

    static constexpr uint32_t MaxThreadsPerBlock = kNThreads;
    static constexpr uint32_t MinBlocksPerMultiprocessor = 2;

    static constexpr int kBlockM = get<0>(TileShape_MK{});
    static constexpr int kHeadDim = get<1>(TileShape_MK{});

    static constexpr int kGmemElemsPerLoad = sizeof(cute::uint128_t) / sizeof(ElementAccum);
    static_assert(kHeadDim % kGmemElemsPerLoad == 0, "Headdim must be a multiple of kGmemElemsPerLoad");
    static constexpr int kBlockKGmem = kHeadDim % 128 == 0 ? 128 : (kHeadDim % 64 == 0 ? 64 : 32);
    static constexpr int kGmemThreadsPerRow = kBlockKGmem / kGmemElemsPerLoad;
    static_assert(MaxThreadsPerBlock % kGmemThreadsPerRow == 0, "MaxThreadsPerBlock must be a multiple of kGmemThreadsPerRow");
    using GmemCopyAtom = std::conditional_t<
        Has_cp_async,
        cute::Copy_Atom<SM80_CP_ASYNC_CACHEGLOBAL<uint128_t>, ElementAccum>,
        cute::Copy_Atom<AutoVectorizingCopyWithAssumedAlignment<128>, ElementAccum>
    >;
    using GmemLayoutAtom = Layout<Shape <Int<MaxThreadsPerBlock / kGmemThreadsPerRow>, Int<kGmemThreadsPerRow>>,
                                  Stride<Int<kGmemThreadsPerRow>, _1>>;
    using GmemTiledCopyAccum = decltype(
        make_tiled_copy(GmemCopyAtom{},
                        GmemLayoutAtom{},
                        Layout<Shape<_1, Int<kGmemElemsPerLoad>>>{}));  // Val layout, 4 vals per load
    using GmemTiledCopy = decltype(
        make_tiled_copy(Copy_Atom<AutoVectorizingCopyWithAssumedAlignment<128>, Element>{},
                        GmemLayoutAtom{},
                        Layout<Shape<_1, Int<kGmemElemsPerLoad>>>{}));  // Val layout, 4 vals per load

    using AlignmentTypeLSE = cute::uint_byte_t<static_cast<int>(sizeof(float)) * AlignmentLSE>;
    static constexpr int kGmemElemsPerLoadLSE = sizeof(AlignmentTypeLSE) / sizeof(float);
    static_assert(kBlockM % kGmemElemsPerLoadLSE == 0, "kBlockM must be a multiple of kGmemElemsPerLoadLSE");
    static_assert(kBlockM % 8 == 0, "kBlockM must be a multiple of 8");
    static constexpr int kBlockMSmem = kBlockM % 128 == 0 ? 128 : (kBlockM % 64 == 0 ? 64 : (kBlockM % 32 == 0 ? 32 : (kBlockM % 16 == 0 ? 16 : 8)));
    static constexpr int kGmemThreadsPerRowLSE = kBlockMSmem / kGmemElemsPerLoadLSE;
    static_assert(MaxThreadsPerBlock % kGmemThreadsPerRowLSE == 0, "MaxThreadsPerBlock must be a multiple of kGmemThreadsPerRowLSE");
    using GmemLayoutAtomLSE = Layout<Shape <Int<MaxThreadsPerBlock / kGmemThreadsPerRowLSE>, Int<kGmemThreadsPerRowLSE>>,
                                     Stride<Int<kGmemThreadsPerRowLSE>, _1>>;
    using GmemCopyAtomLSE = std::conditional_t<
        Has_cp_async,
        cute::Copy_Atom<SM80_CP_ASYNC_CACHEALWAYS<AlignmentTypeLSE>, float>,
        cute::Copy_Atom<AutoVectorizingCopyWithAssumedAlignment<AlignmentLSE * sizeof(float) * 8>, float>
    >;
    using GmemTiledCopyLSE = decltype(
        make_tiled_copy(GmemCopyAtomLSE{},
                        GmemLayoutAtomLSE{},
                        Layout<Shape<_1, Int<kGmemElemsPerLoadLSE>>>{}));  // Val layout, 4 vals per load

    // Otherwise we get IMA when some threads access sLSE, as we're not doing any masking
    static_assert((kBlockM * kMaxSplits * AlignmentLSE) % kNThreads == 0, "kNThreads must divide kBlockM * kMaxSplits * AlignmentLSE");
    // This works for kBlockMSmem = 8, 16, 32, 64, 128, no bank conflicts
    using SmemLSESwizzle = std::conditional_t<
        kBlockMSmem == 8,
        Swizzle<5, 0, 5>,
        std::conditional_t<kBlockMSmem == 16, Swizzle<4, 0, 4>, Swizzle<3, 2, 3>>
    >;
    using SmemLayoutAtomLSE =
        decltype(composition(SmemLSESwizzle{},
                 Layout<Shape<Int<8>, Int<kBlockMSmem>>,
                 Stride<Int<kBlockMSmem>, _1>>{}));
    using SmemLayoutLSE = decltype(tile_to_shape(SmemLayoutAtomLSE{}, Shape<Int<kMaxSplits>, Int<kBlockM>>{}));

    using SmemLayoutO = Layout<Shape<Int<kBlockM>, Int<kHeadDim>, Int<kStages>>,
                               Stride<Int<kHeadDim>, _1, Int<kBlockM * kHeadDim>>>;

    // We want each column (kMaxSplits) to be processed by threads in the same warp.
    // To reduce the number of shuffles, we want as few threads on the same column as possible.
    // E.g., if kBlockM is divisible by 64, and there are 256 threads, we want 4 threads (0, 1, 2, 4) per column
    // have have 64 such quads.
    static_assert(MaxThreadsPerBlock % kBlockMSmem == 0, "MaxThreadsPerBlock must be a multiple of kBlockMSmem");
    static constexpr int kSmemThreadsPerColLSEt = MaxThreadsPerBlock / kBlockMSmem;
    using S2RLayoutAtomLSE = Layout<Shape<Int<kSmemThreadsPerColLSEt>, Int<MaxThreadsPerBlock / kSmemThreadsPerColLSEt>>>;
    using S2RTiledCopyLSE = decltype(make_tiled_copy(cute::Copy_Atom<cute::DefaultCopy, float>{}, S2RLayoutAtomLSE{}, Layout<_1>{}));

    using ShapeOPartial = cute::Shape<int32_t, int32_t, int32_t, int32_t, int32_t>;  // (seqlen, d, num_splits, head, batch)
    using StrideOPartial = cute::Stride<int64_t, _1, int64_t, int64_t, int64_t>;
    using ShapeLSEPartial = cute::Shape<int32_t, int32_t, int32_t, int32_t>;  // (seqlen, num_splits, head, batch)
    using StrideLSEPartial = cute::Stride<_1, int64_t, int64_t, int64_t>;  // (seqlen, num_splits, head, batch)
    using ShapeO = cute::Shape<int32_t, int32_t, int32_t, int32_t>;  // (seqlen, d, head, batch)
    using StrideO = cute::Stride<int64_t, _1, int64_t, int64_t>;
    using ShapeLSE = cute::Shape<int32_t, int32_t, int32_t>;  // (seqlen, head, batch)
    using StrideLSE = cute::Stride<_1, int64_t, int64_t>;  // (seqlen, head, batch)

    struct SharedStorage : cute::aligned_struct<128> {
        cute::array_aligned<float, cute::cosize_v<SmemLayoutLSE>> smem_lse_partial;
        cute::array_aligned<ElementAccum, cute::cosize_v<SmemLayoutO>> smem_o_partial;
    };

    static constexpr int SharedStorageSize = sizeof(SharedStorage);


    // Device side arguments
    struct Arguments {
        ElementAccum const* ptr_O_partial;
        ShapeOPartial const shape_O_partial;
        StrideOPartial const stride_O_partial;
        float const* ptr_LSE_partial;
        ShapeLSEPartial const shape_LSE_partial;
        StrideLSEPartial const stride_LSE_partial;
        Element* ptr_O;
        StrideO const stride_O;
        float* ptr_LSE;
        StrideLSE const stride_LSE;
        int const* cu_seqlens = nullptr;
        int const* seqused = nullptr;
    };

    // Kernel entry point API
    struct Params {
        ElementAccum const* ptr_O_partial;
        ShapeOPartial const shape_O_partial;
        StrideOPartial const stride_O_partial;
        float const* ptr_LSE_partial;
        ShapeLSEPartial const shape_LSE_partial;
        StrideLSEPartial const stride_LSE_partial;
        Element* ptr_O;
        StrideO const stride_O;
        float* ptr_LSE;
        StrideLSE const stride_LSE;
        cutlass::FastDivmod seqlen_divmod, head_divmod;
        int const* cu_seqlens = nullptr;
        int const* seqused = nullptr;
    };

    // Convert to underlying arguments. In this case, a simple copy for the aliased type.
    static
    Params
    to_underlying_arguments(Arguments const& args) {
        assert(get<1>(args.shape_LSE_partial) <= kMaxSplits);
        return {
            args.ptr_O_partial,
            args.shape_O_partial,
            args.stride_O_partial,
            args.ptr_LSE_partial,
            args.shape_LSE_partial,
            args.stride_LSE_partial,
            args.ptr_O,
            args.stride_O,
            args.ptr_LSE,
            args.stride_LSE,
            cutlass::FastDivmod(get<0>(args.shape_LSE_partial)), cutlass::FastDivmod(get<2>(args.shape_LSE_partial)),
            args.cu_seqlens,
            args.seqused
        };
    }

    CUTLASS_DEVICE
    void
    operator()(Params const& params, char* smem_buf) {

        // TODO: split the work of computing pointers among threads in the same row

        SharedStorage& shared_storage = *reinterpret_cast<SharedStorage*>(smem_buf);
        Tensor sLSE = make_tensor(make_smem_ptr(shared_storage.smem_lse_partial.data()), SmemLayoutLSE{});
        Tensor sO = make_tensor(make_smem_ptr(shared_storage.smem_o_partial.data()), SmemLayoutO{});

        int const thread_idx = threadIdx.x;
        int const m_block = blockIdx.x;
        int const batch = !Varlen ? 0 : blockIdx.y;
        int const num_splits = get<1>(params.shape_LSE_partial);
        int const offset = !Varlen || params.cu_seqlens == nullptr ? 0 : params.cu_seqlens[batch];
        int const seqlen = !Varlen ? get<0>(params.shape_LSE_partial) : (params.seqused ? params.seqused[batch] : (params.cu_seqlens == nullptr ? get<0>(params.shape_LSE_partial) : params.cu_seqlens[batch + 1] - offset));
        int max_idx = seqlen * get<2>(params.shape_LSE_partial) * get<3>(params.shape_LSE_partial);

        cutlass::FastDivmod seqlen_divmod_dynamic(seqlen);

        // Step 1: load LSE_partial from gmem -> smem
        Tensor mLSEpartial = make_tensor(make_gmem_ptr(params.ptr_LSE_partial + offset * get<0>(params.stride_LSE_partial)), select<1, 0, 2, 3>(params.shape_LSE_partial), select<1, 0, 2, 3>(params.stride_LSE_partial));  // (num_splits, seqlen, head, batch)
        Tensor mLSEpartial_copy = cute::tiled_divide(mLSEpartial, Shape<_1, Int<kGmemElemsPerLoadLSE>>{});
        // if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 && thread_idx == 0) { print(mLSEpartial); printf("\n"); }
        GmemTiledCopyLSE gmem_tiled_copy_LSE;
        auto gmem_thr_copy_LSE = gmem_tiled_copy_LSE.get_thread_slice(thread_idx);
        Tensor tLSEsLSE = gmem_thr_copy_LSE.partition_D(sLSE);

        // Construct identity layout for sLSE
        Tensor cLSE = make_identity_tensor(make_shape(size<0>(sLSE), size<1>(sLSE)));    // (NUM_SPLITS, BLK_M) -> (num_splits, blk_m)
        // Repeat the partitioning with identity layouts
        Tensor tLSEcLSE = gmem_thr_copy_LSE.partition_S(cLSE);

        #pragma unroll
        for (int m = 0; m < size<2>(tLSEcLSE); ++m) {
            int mi = int(get<1>(tLSEcLSE(_0{}, _0{}, m)));
            int idx = m_block * kBlockM + mi;
            if (idx < max_idx) {
                int m_idx, bidh, bidb;
                if constexpr (!Varlen) {
                    bidb = params.head_divmod.divmod(bidh, params.seqlen_divmod.divmod(m_idx, idx));
                } else {
                    bidh = seqlen_divmod_dynamic.divmod(m_idx, idx);
                    bidb = 0;
                }
                #pragma unroll
                for (int s = 0; s < size<1>(tLSEcLSE); ++s) {
                    int si = get<0>(tLSEcLSE(_0{}, s, _0{}));
                    // if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 && thread_idx < 32) { printf("tidx = %d, m = %d, s = %d, addr = %p, bank = %d\n", thread_idx, m, s, reinterpret_cast<float *>(&(tLSEsLSE(_0{}, s, m))), reinterpret_cast<int>(&(tLSEsLSE(_0{}, s, m))) / 4 % 32);}
                    if (si < num_splits) {
                        cute::copy(gmem_tiled_copy_LSE, mLSEpartial_copy(_, si, m_idx, bidh, bidb), tLSEsLSE(_, s, m));
                    } else {
                        cute::fill(tLSEsLSE(_, s, m), -INFINITY);
                    }
                }
            } else {
                // We don't need to zero out the rest of the LSEs, as we will not write the output to gmem
                // cute::fill(tLSEsLSE(_, _, m), -INFINITY);
            }
        }
        if constexpr (Has_cp_async) { cute::cp_async_fence(); }

        // Step 2: Load O_partial from gmem -> smem for split = 0, 1, ..., kStages - 2.
        // We want these async loads to be in flight as we compute the LSE.
        GmemTiledCopyAccum gmem_tiled_copy_O_partial;
        auto gmem_thr_copy_O_partial = gmem_tiled_copy_O_partial.get_thread_slice(thread_idx);
        // Construct identity layout for gO
        Tensor cO = cute::make_identity_tensor(TileShape_MK{});  // (BLK_M,BLK_K) -> (blk_m,blk_k)
        // Repeat the partitioning with identity layouts
        Tensor tOcO = gmem_thr_copy_O_partial.partition_D(cO);

        // Precompute these values to avoid recomputing them in the loop
        Tensor tOmidx = make_tensor<int>(make_shape(size<1>(tOcO)));
        Tensor tObidh = make_tensor<int>(make_shape(size<1>(tOcO)));
        Tensor tObidb = make_tensor<int>(make_shape(size<1>(tOcO)));
        #pragma unroll
        for (int m = 0; m < size<1>(tOcO); ++m) {
            int mi = get<0>(tOcO(_0{}, m, _0{}));
            int idx = m_block * kBlockM + mi;
            if constexpr (!Varlen) {
                tObidb(m) = params.head_divmod.divmod(tObidh(m), params.seqlen_divmod.divmod(tOmidx(m), idx));
            } else {
                tObidh(m) = seqlen_divmod_dynamic.divmod(tOmidx(m), idx);
                tObidb(m) = 0;
            }
            if (idx >= max_idx) { tObidb(m) = -1; }
        }

        Tensor tOpO = make_tensor<bool>(make_shape(size<2>(tOcO)));
        if constexpr (!(Is_even_K)) {
            #pragma unroll
            for (int k = 0; k < size(tOpO); ++k) { tOpO(k) = get<1>(tOcO(_0{}, _0{}, k)) < get<1>(params.shape_O_partial); }
        }

        Tensor mOpartial = make_tensor(make_gmem_ptr(params.ptr_O_partial + offset * get<0>(params.stride_O_partial)), params.shape_O_partial, params.stride_O_partial);  // (seqlen, d, num_splits, head, batch)
        Tensor mOpartial_copy = cute::tiled_divide(mOpartial, Shape<_1, Int<kGmemElemsPerLoad>>{});
        Tensor tOsOpartial = gmem_thr_copy_O_partial.partition_D(sO);
        // if (cute::thread0()) { print(tOsOpartial); printf("\n"); }

        auto load_O_partial = [&] (int split, int stage) {
            #pragma unroll
            for (int m = 0; m < size<1>(tOcO); ++m) {
                if (tObidb(m) >= 0)  {
                    #pragma unroll
                    for (int k = 0; k < size<2>(tOcO); ++k) {
                        int k_idx = get<1>(tOcO(_0{}, _0{}, k)) / kGmemElemsPerLoad;
                        if (Is_even_K || tOpO(k)) {
                            cute::copy(gmem_tiled_copy_O_partial, mOpartial_copy(_, tOmidx(m), k_idx, split, tObidh(m), tObidb(m)), tOsOpartial(_, m, k, stage));
                        }
                    }
                }
            }
        };

        for (int s = 0; s < kStages - 1; ++s) {
            if (s < num_splits) { load_O_partial(s, s); }
            if constexpr (Has_cp_async) { cute::cp_async_fence(); }
        }

        // Step 3: load and transpose LSE_partial from smem -> rmem
        if constexpr (Has_cp_async) { cutlass::arch::cp_async_wait<kStages - 1>(); }
        __syncthreads();

        S2RTiledCopyLSE s2r_tiled_copy_LSE;
        auto s2r_thr_copy_LSE = s2r_tiled_copy_LSE.get_thread_slice(thread_idx);
        Tensor ts2rsLSE = s2r_thr_copy_LSE.partition_S(sLSE);
        Tensor ts2rrLSE = make_fragment_like(ts2rsLSE);
        cute::copy(s2r_tiled_copy_LSE, ts2rsLSE, ts2rrLSE);
        // if (cute::thread0()) { print(ts2rsLSE); printf("\n"); print_tensor(ts2rrLSE); printf("\n"); }

        // Step 4: compute the final LSE along the split dimension
        Tensor lse_sum = make_tensor<float>(make_shape(size<2>(ts2rrLSE)));
        static_assert(CUTE_STATIC_V(size<0>(ts2rrLSE)) == 1);
        #pragma unroll
        for (int m = 0; m < size<2>(ts2rrLSE); ++m) {
            float lse_max = ts2rrLSE(_0{}, _0{}, m);
            #pragma unroll
            for (int s = 1; s < size<1>(ts2rrLSE); ++s) { lse_max = max(lse_max, ts2rrLSE(_0{}, s, m)); }
            MaxOp<float> max_op;
            lse_max = Allreduce<kSmemThreadsPerColLSEt>::run(lse_max, max_op);
            float lse_max_cur = lse_max == -INFINITY ? 0.0f : lse_max;  // In case all local LSEs are -inf
            float lse_sum_cur = 0.f;
            #pragma unroll
            for (int s = 0; s < size<1>(ts2rrLSE); ++s) {
                float scale = expf(ts2rrLSE(_0{}, s, m) - lse_max_cur);
                lse_sum_cur += scale;
                // if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 && thread_idx < 32) { printf("tidx = %d, m = %d, s = %d, addr = %p, bank = %d\n", thread_idx, m, s, reinterpret_cast<float *>(&(ts2rsLSE(_0{}, s, m))), reinterpret_cast<int>(&(ts2rsLSE(_0{}, s, m))) / 4 % 32);}
                // ts2rsLSE(_0{}, m, s) = scale;
                ts2rrLSE(_0{}, s, m) = scale;
            }
            SumOp<float> sum_op;
            lse_sum_cur = Allreduce<kSmemThreadsPerColLSEt>::run(lse_sum_cur, sum_op);
            lse_sum(m) = logf(lse_sum_cur) + lse_max;
            float inv_sum = (lse_sum_cur == 0.f || lse_sum_cur != lse_sum_cur) ? 0.f : 1.f / lse_sum_cur;
            #pragma unroll
            for (int s = 0; s < size<1>(ts2rrLSE); ++s) { ts2rrLSE(_0{}, s, m) *= inv_sum; }
        }
        // Store the scales exp(lse - lse_logsum) back to smem
        cute::copy(s2r_tiled_copy_LSE, ts2rrLSE, ts2rsLSE);
        // if (blockIdx.x == 0 && blockIdx.y == 0 && blockIdx.z == 0 && thread_idx == 0) { print_tensor(lse_sum); printf("\n"); }

        // Step 5: store final LSE back to gmem
        auto shape_LSE = select<0, 2, 3>(params.shape_LSE_partial);
        Tensor mLSE = make_tensor(make_gmem_ptr(params.ptr_LSE + offset * get<0>(params.stride_LSE)), shape_LSE, params.stride_LSE);
        // Repeat the partitioning with identity layouts
        Tensor ts2rcLSE = s2r_thr_copy_LSE.partition_S(cLSE);
        #pragma unroll
        for (int m = 0; m < size<2>(ts2rrLSE); ++m) {
            if (get<0>(ts2rcLSE(_0{}, _0{}, m)) == 0) {  // Only the thread responsible for s=0 writes to gmem
                int mi = int(get<1>(ts2rcLSE(_0{}, _0{}, m)));
                int idx = m_block * kBlockM + mi;
                if (idx < max_idx) {
                    int m_idx, bidh, bidb;
                    if constexpr (!Varlen) {
                        bidb = params.head_divmod.divmod(bidh, params.seqlen_divmod.divmod(m_idx, idx));
                    } else {
                        bidh = seqlen_divmod_dynamic.divmod(m_idx, idx);
                        bidb = 0;
                    }
                    // printf("tidx = %d, m = %d, mi = %d, idx = %d, m_idx = %d, bidh = %d, bidb = %d, lse_sum = %f\n", thread_idx, m, mi, idx, m_idx, bidh, bidb, lse_sum(m));
                    mLSE(m_idx, bidh, bidb) = lse_sum(m);
                }
            }
        }
        // if (blockIdx.x == 0 && blockIdx.z == 0 && thread_idx == 0) { print(mLSE); printf("\n"); print(gLSE); printf("\n"); }

        // Step 6: read O_partial from gmem -> smem -> rmem and accumulate the final O
        __syncthreads();
        auto tOrOpartial_layout = gmem_thr_copy_O_partial.partition_S(make_tensor<ElementAccum>(TileShape_MK{})).layout();
        Tensor tOrOpartial = make_fragment_like<ElementAccum>(tOrOpartial_layout);
        Tensor tOrO = make_fragment_like(tOrOpartial);
        clear(tOrO);
        int stage_load = kStages - 1, stage_compute = 0;
        #pragma unroll 4 // Already tuned for speed
        for (int s = 0; s < num_splits; ++s) {
            Tensor scale_load = make_tensor<float>(make_shape(size<1>(tOrOpartial)));
            if (s + kStages - 1 < num_splits) {
                #pragma unroll
                for (int m = 0; m < size<1>(tOrOpartial); ++m) {
                    scale_load(m) = sLSE(s + kStages - 1, get<0>(tOcO(_0{}, m, _0{})));
                }
            }
            Tensor scale = make_tensor<float>(make_shape(size<1>(tOrOpartial)));
            #pragma unroll
            for (int m = 0; m < size<1>(tOrOpartial); ++m) { scale(m) = sLSE(s, get<0>(tOcO(_0{}, m, _0{}))); }

            // if (s + 1 < num_splits) { load_O_partial(s + 1, (s + 1) % 2); }
            if (s + kStages - 1 < num_splits) {
                #pragma unroll
                for (int m = 0; m < size<1>(tOcO); ++m) {
                    if (tObidb(m) >= 0 && scale_load(m) > 0.f)  {
                    // if (tObidb(m) >= 0)  {
                        #pragma unroll
                        for (int k = 0; k < size<2>(tOcO); ++k) {
                            int k_idx = get<1>(tOcO(_0{}, _0{}, k)) / kGmemElemsPerLoad;
                            if (Is_even_K || tOpO(k)) {
                                cute::copy(gmem_tiled_copy_O_partial, mOpartial_copy(_, tOmidx(m), k_idx, s + kStages - 1, tObidh(m), tObidb(m)), tOsOpartial(_, m, k, stage_load));
                            }
                        }
                    }
                }
            }
            // if (cute::thread0()) { print(tOsOpartial); printf("stage_load = %d, state_compute = %d\n", stage_load, stage_compute);}
            if constexpr (Has_cp_async) { cute::cp_async_fence(); }
            stage_load = stage_load < kStages - 1 ? stage_load + 1 : 0;

            if constexpr (Has_cp_async) { cutlass::arch::cp_async_wait<kStages - 1>(); }
            // We don't need __syncthreads() because each thread is just reading its own data from smem
            cute::copy(Copy_Atom<AutoVectorizingCopyWithAssumedAlignment<128>, ElementAccum>{},
                       tOsOpartial(_, _, _, stage_compute), tOrOpartial);
            stage_compute = stage_compute < kStages - 1 ? stage_compute + 1 : 0;

            // if (cute::thread0()) { print_tensor(tOrOpartial); }
            #pragma unroll
            for (int m = 0; m < size<1>(tOrOpartial); ++m) {
                if (tObidb(m) >= 0 && scale(m) > 0.f) {
                    #pragma unroll
                    for (int k = 0; k < size<2>(tOrOpartial); ++k) {
                        if (Is_even_K || tOpO(k)) {
                            #pragma unroll
                            for (int i = 0; i < size<0>(tOrOpartial); ++i) {
                                tOrO(i, m, k) += scale(m) * tOrOpartial(i, m, k);
                            }
                        }
                    }
                }
            }
        }

        // Step 7: Write the final O to gmem
        Tensor rO = flash::convert_type<Element>(tOrO);
        auto shape_O = select<0, 1, 3, 4>(params.shape_O_partial);
        Tensor mO = make_tensor(make_gmem_ptr(params.ptr_O + offset * get<0>(params.stride_O)), shape_O, params.stride_O);
        Tensor mO_copy = cute::tiled_divide(mO, Shape<_1, Int<kGmemElemsPerLoad>>{});
        GmemTiledCopy gmem_tiled_copy_O;
        auto gmem_thr_copy_O = gmem_tiled_copy_O.get_thread_slice(thread_idx);

        #pragma unroll
        for (int m = 0; m < size<1>(tOcO); ++m) {
            if (tObidb(m) >= 0)  {
                #pragma unroll
                for (int k = 0; k < size<2>(tOcO); ++k) {
                    int k_idx = get<1>(tOcO(_0{}, _0{}, k)) / kGmemElemsPerLoad;
                    if (Is_even_K || tOpO(k)) {
                        cute::copy(gmem_tiled_copy_O, rO(_, m, k), mO_copy(_, tOmidx(m), k_idx, tObidh(m), tObidb(m)));
                    }
                }
            }
        }

    }

};

} // namespace flash
