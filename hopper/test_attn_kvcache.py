import pytest
from einops import rearrange, repeat
import torch
import flash_attn
import flash_attn_interface
import itertools
import math
import time

def construct_local_mask(
    seqlen_q,
    seqlen_k,
    window_size=(-1, -1),  # -1 means infinite window size
    query_padding_mask=None,
    key_padding_mask=None,
    device=None,
    key_leftpad=None,
):
    row_idx = rearrange(torch.arange(seqlen_q, device=device, dtype=torch.long), "s -> s 1")
    col_idx = torch.arange(seqlen_k, device=device, dtype=torch.long)
    if key_leftpad is not None:
        key_leftpad = rearrange(key_leftpad, "b -> b 1 1 1")
        col_idx = repeat(col_idx, "s -> b 1 1 s", b=key_leftpad.shape[0])
        col_idx = torch.where(col_idx >= key_leftpad, col_idx - key_leftpad, 2**32)
    sk = (
        seqlen_k
        if key_padding_mask is None
        else rearrange(key_padding_mask.sum(-1), "b -> b 1 1 1")
    )
    sq = (
        seqlen_q
        if query_padding_mask is None
        else rearrange(query_padding_mask.sum(-1), "b -> b 1 1 1")
    )
    if window_size[0] < 0:
        return col_idx > row_idx + sk - sq + window_size[1]
    else:
        sk = torch.full_like(col_idx, seqlen_k) if key_padding_mask is None else sk
        return torch.logical_or(
            col_idx > torch.minimum(row_idx + sk - sq + window_size[1], sk),
            col_idx < row_idx + sk - sq - window_size[0],
        )


def attention_ref(
    q,
    k,
    v,
    query_padding_mask=None,
    key_padding_mask=None,
    attn_bias=None,
    dropout_p=0.0,
    dropout_mask=None,
    causal=False,
    window_size=(-1, -1),  # -1 means infinite window size
    softcap=0.0,
    upcast=True,
    reorder_ops=False,
    key_leftpad=None,
):
    """
    Arguments:
        q: (batch_size, seqlen_q, nheads, head_dim)
        k: (batch_size, seqlen_k, nheads_k, head_dim)
        v: (batch_size, seqlen_k, nheads_k, head_dim)
        query_padding_mask: (batch_size, seqlen_q)
        key_padding_mask: (batch_size, seqlen_k)
        attn_bias: broadcastable to (batch_size, nheads, seqlen_q, seqlen_k)
        dropout_p: float
        dropout_mask: (batch_size, nheads, seqlen_q, seqlen_k)
        causal: whether to apply causal masking
        window_size: (int, int), left and right window size
        upcast: whether to cast all inputs to fp32, do all computation in fp32, then cast
            output back to fp16/bf16.
        reorder_ops: whether to change the order of operations (scaling k instead of scaling q, etc.)
            without changing the math. This is to estimate the numerical error from operation
            reordering.
    Output:
        output: (batch_size, seqlen_q, nheads, head_dim)
        attention: (batch_size, nheads, seqlen_q, seqlen_k), softmax after dropout
    """
    if causal:
        window_size = (window_size[0], 0)
    dtype_og = q.dtype
    if upcast:
        q, k, v = q.float(), k.float(), v.float()
    seqlen_q, seqlen_k = q.shape[1], k.shape[1]
    k = repeat(k, "b s h d -> b s (h g) d", g=q.shape[2] // k.shape[2])
    v = repeat(v, "b s h d -> b s (h g) d", g=q.shape[2] // v.shape[2])
    d = q.shape[-1]
    if not reorder_ops:
        scores = torch.einsum("bthd,bshd->bhts", q / math.sqrt(d), k)
    else:
        scores = torch.einsum("bthd,bshd->bhts", q, k / math.sqrt(d))
    if softcap > 0:
        scores = scores / softcap
        scores = scores.tanh()
        scores = scores * softcap
    if key_padding_mask is not None:
        scores.masked_fill_(rearrange(~key_padding_mask, "b s -> b 1 1 s"), float("-inf"))
    if window_size[0] >= 0 or window_size[1] >= 0:
        local_mask = construct_local_mask(
            seqlen_q,
            seqlen_k,
            window_size,
            query_padding_mask,
            key_padding_mask,
            q.device,
            key_leftpad=key_leftpad,
        )
        scores.masked_fill_(local_mask, float("-inf"))
    if attn_bias is not None:
        scores = scores + attn_bias
    attention = torch.softmax(scores, dim=-1).to(v.dtype)
    # Some rows might be completely masked out so we fill them with zero instead of NaN
    if window_size[0] >= 0 or window_size[1] >= 0:
        attention = attention.masked_fill(torch.all(local_mask, dim=-1, keepdim=True), 0.0)
    # We want to mask here so that the attention matrix doesn't have any NaNs
    # Otherwise we'll get NaN in dV
    if query_padding_mask is not None:
        attention = attention.masked_fill(rearrange(~query_padding_mask, "b s -> b 1 s 1"), 0.0)
    dropout_scaling = 1.0 / (1 - dropout_p)
    # attention_drop = attention.masked_fill(~dropout_mask, 0.0) * dropout_scaling
    # output = torch.einsum('bhts,bshd->bthd', attention_drop , v)
    if dropout_mask is not None:
        attention_drop = attention.masked_fill(~dropout_mask, 0.0)
    else:
        attention_drop = attention
    output = torch.einsum("bhts,bshd->bthd", attention_drop, v * dropout_scaling)
    if query_padding_mask is not None:
        output.masked_fill_(rearrange(~query_padding_mask, "b s -> b s 1 1"), 0.0)
    return output.to(dtype=dtype_og), attention.to(dtype=dtype_og)


# @pytest.mark.parametrize("use_heuristic_only", [False])
@pytest.mark.parametrize("causal", [True, False])
@pytest.mark.parametrize("num_requests", [1])
@pytest.mark.parametrize("query_seqlen", [1, 16])
@pytest.mark.parametrize("context_seqlen", [1024])
@pytest.mark.parametrize("headdim", [128])
@pytest.mark.parametrize(
    "nheads_kv, gqa_ratio",
    [
        (1, 1),
        (2, 5),
        (3, 3),
        (1, 32),
        (5, 7),
        (8, 1),
        (1, 16),
        (12, 4),
        (8, 2),
    ],
)
def test_flash_attn_kvcache_nosplit(nheads_kv, gqa_ratio, num_requests, query_seqlen, context_seqlen, headdim, causal):
    device = "cuda"
    num_caches = num_requests
    cache_seqlen = context_seqlen
    nheads_q = nheads_kv * gqa_ratio

    k_cache = torch.randn(
        (num_caches, cache_seqlen, nheads_kv, headdim), device="cuda", dtype=torch.bfloat16
    )
    v_cache = torch.randn(
        (num_caches, cache_seqlen, nheads_kv, headdim), device="cuda", dtype=torch.bfloat16
    )
    # print(f"***{model_name}***")
    q = torch.randn((num_requests, query_seqlen, nheads_q, headdim), device="cuda", dtype=torch.bfloat16)
    cache_idxs = torch.randperm(num_caches, dtype=torch.int32, device="cuda")[:num_requests]
    cache_seqlens = torch.tensor([context_seqlen] * num_requests, dtype=torch.int32, device="cuda")
    torch.cuda.synchronize()

    out_ref, _ = attention_ref(
        q,
        k_cache,
        v_cache,
        causal=causal,
    )

    out_fa3, lse_fa3 = flash_attn_interface.flash_attn_with_kvcache(
                    q=q,
                    k_cache=k_cache,
                    v_cache=v_cache,
                    cache_seqlens=cache_seqlens,
                    cache_batch_idx=cache_idxs,
                    causal=causal,
                    num_splits=1,
                    return_softmax_lse=True,
                    gqa_parallel=False
                )


    torch.cuda.synchronize()
    assert ((out_ref - out_fa3).abs().max().item() <= 2e-3)
    assert ((out_ref - out_fa3).abs().mean().item() <= 1e-4)

@pytest.mark.parametrize("use_heuristic_only", [True])
# @pytest.mark.parametrize("use_heuristic_only", [False])
@pytest.mark.parametrize("causal", [True, False])
@pytest.mark.parametrize("num_requests", [1, 4, 16])
@pytest.mark.parametrize("query_seqlen", [1, 16, 32, 128])
@pytest.mark.parametrize("context_seqlen", [4096, 16384, 65536])
@pytest.mark.parametrize("headdim", [64, 128, 256])
@pytest.mark.parametrize("cache_seqlen_rand", [True, False])
@pytest.mark.parametrize(
    "nheads_kv, gqa_ratio",
    [
        (1, 1),
        (2, 5),
        (3, 3),
        (1, 32),
        (5, 7),
        (8, 1),
        (1, 16),
        (12, 4),
        (8, 2),
    ],
)
def test_flash_attn_kvcache_output(nheads_kv, gqa_ratio, num_requests, query_seqlen, context_seqlen, headdim, causal, use_heuristic_only, cache_seqlen_rand):
    device = "cuda"
    num_caches = 16
    if context_seqlen <= 65536:
        cache_seqlen = 65536
    else:
        cache_seqlen = context_seqlen
    nheads_q = nheads_kv * gqa_ratio
    if use_heuristic_only:
        max_splits = 1
    else:
        max_splits = 128

    k_cache = torch.randn(
        (num_caches, cache_seqlen, nheads_kv, headdim), device="cuda", dtype=torch.bfloat16
    )
    v_cache = torch.randn(
        (num_caches, cache_seqlen, nheads_kv, headdim), device="cuda", dtype=torch.bfloat16
    )
    # print(f"***{model_name}***")
    q = torch.randn((num_requests, query_seqlen, nheads_q, headdim), device="cuda", dtype=torch.bfloat16)
    cache_idxs = torch.randperm(num_caches, dtype=torch.int32, device="cuda")[:num_requests]
    cache_seqlens = torch.randint(query_seqlen, context_seqlen-1, (num_requests,), dtype=torch.int32).to(device) if cache_seqlen_rand else torch.tensor([context_seqlen] * num_requests, dtype=torch.int32, device="cuda")
    torch.cuda.synchronize()

    out_ref, lse_ref = flash_attn_interface.flash_attn_with_kvcache(
                    q=q,
                    k_cache=k_cache,
                    v_cache=v_cache,
                    cache_seqlens=cache_seqlens,
                    cache_batch_idx=cache_idxs,
                    causal=causal,
                    num_splits=1,
                    return_softmax_lse=True,
                    gqa_parallel=False
                )

    # i=0 case is with num splits heuristic
    for i in range(0, max_splits+1):
                out_fa3, lse_fa3 = flash_attn_interface.flash_attn_with_kvcache(
                    q=q,
                    k_cache=k_cache,
                    v_cache=v_cache,
                    cache_seqlens=cache_seqlens,
                    cache_batch_idx=cache_idxs,
                    causal=causal,
                    num_splits=i,
                    return_softmax_lse=True,
                    gqa_parallel=False,
                    max_seqlen_k_hint=context_seqlen
                )

                out_fa3_gqa, lse_fa3_gqa = flash_attn_interface.flash_attn_with_kvcache(
                    q=q,
                    k_cache=k_cache,
                    v_cache=v_cache,
                    cache_seqlens=cache_seqlens,
                    cache_batch_idx=cache_idxs,
                    causal=causal,
                    num_splits=i,
                    return_softmax_lse=True,
                    gqa_parallel=True,
                    max_seqlen_k_hint=context_seqlen
                )

                torch.cuda.synchronize()
                print ('output-max-diff', i, context_seqlen, (out_ref - out_fa3).abs().max().item())
                print ('output-mean-diff',i, context_seqlen, (out_ref - out_fa3).abs().mean().item())
                print ('output-max-diff gqa', i, context_seqlen, (out_ref - out_fa3_gqa).abs().max().item())
                print ('output-mean-diff gqa',i, context_seqlen, (out_ref - out_fa3_gqa).abs().mean().item())
                print ('lse-max-diff',i, context_seqlen, (lse_ref - lse_fa3).abs().max().item())
                print ('lse-mean-diff',i,  context_seqlen, (lse_ref - lse_fa3).abs().mean().item())
                print ('lse-max-diff gqa',i, context_seqlen, (lse_ref - lse_fa3_gqa).abs().max().item())
                print ('lse-mean-diff gqa',i, context_seqlen, (lse_ref - lse_fa3_gqa).abs().mean().item())

                if cache_seqlen_rand:
                    assert ((out_ref - out_fa3).abs().max().item() <= 1e-2)
                    assert ((out_ref - out_fa3_gqa).abs().max().item() <= 1e-2)
                    assert ((out_ref - out_fa3).abs().mean().item() <= 1e-3)
                    assert ((out_ref - out_fa3_gqa).abs().mean().item() <= 1e-3)
                else:
                    assert ((out_ref - out_fa3).abs().max().item() <= 2e-3)
                    assert ((out_ref - out_fa3_gqa).abs().max().item() <= 2e-3)
                    assert ((out_ref - out_fa3).abs().mean().item() <= 1e-4)
                    assert ((out_ref - out_fa3_gqa).abs().mean().item() <= 1e-4)
                assert ((lse_ref - lse_fa3).abs().max().item() <= 1e-3)
                assert ((lse_ref - lse_fa3).abs().mean().item() <= 1e-4)
                assert ((lse_ref - lse_fa3_gqa).abs().max().item() <= 1e-3)
                assert ((lse_ref - lse_fa3_gqa).abs().mean().item() <= 1e-4)

if __name__ == "__main__":
    main()