#pragma once
//加入动态负载均衡功能  //v15版本加入了burst读取增加的优化，先进行测试
// 使用 FIFO/每拍一批 的 SIU（基于 siu_core_6）
//#include "kernel/siu_core_6.hpp"        //仿真上板用这个
#include "siu_core_6.hpp"                 //综合看资源用这个

constexpr int AXI_WIDE_BITS = 512;
constexpr int DATA_T_BITS = sizeof(data_t) * 8;
static_assert(DATA_T_BITS == 32, "data_t is expected to be 32 bits once packed");
static_assert(AXI_WIDE_BITS % DATA_T_BITS == 0, "AXI width must be multiple of data_t");
constexpr int BLOCKS_PER_WIDE = AXI_WIDE_BITS / DATA_T_BITS;

// Keep NUM_PE available early for plan-driven pipelines.
#ifndef NUM_PE
#define NUM_PE 4
#endif

using data_t_bits = ap_uint<DATA_T_BITS>;

inline data_t decode_data_t(data_t_bits bits) {
#pragma HLS INLINE
    data_t value;
    value.set_index(bits.range(BCSR_index - 1, 0));
    value.set_bitmap(bits.range(BCSR_index + BCSR_bitmap - 1, BCSR_index));
    return value;
}

inline void unpack_wide_word(ap_uint<AXI_WIDE_BITS> wide_word, data_t out_blocks[BLOCKS_PER_WIDE]) {
#pragma HLS INLINE
#pragma HLS ARRAY_PARTITION variable=out_blocks complete
    for (int lane = 0; lane < BLOCKS_PER_WIDE; ++lane) {
#pragma HLS UNROLL
        const int high = (lane + 1) * DATA_T_BITS - 1;
        const int low = lane * DATA_T_BITS;
        data_t_bits slice = wide_word.range(high, low);
        out_blocks[lane] = decode_data_t(slice);
    }
}


// --------------------------------------------------------------------------
// On-chip cache for gmem_row_ptr (avoid multiple DATAFLOW processes reading
// the same m_axi port). Tune this if you use larger graphs.
// --------------------------------------------------------------------------
#ifndef PLAN5_ROW_PTR_CACHE_MAX_ENTRIES
#define PLAN5_ROW_PTR_CACHE_MAX_ENTRIES 131072
#endif

// ============================================================================
// Step1-A: Vertex -> (start,len) resolver and ExecutionTask adapter
// - start/len are in units of BCSR blocks (data_t), matching RectangleTask.
// - This stage exists so upstream generators can emit vertex IDs and let the
//   kernel resolve physical ranges via row_ptr.
// ============================================================================
struct NeighborRange {
    int start;
    int len;
};

// A generic two-operand operator payload for Step1-A:
// INTERSECT_COUNT(Adj(a_start,a_len), Adj(b_start,b_len), upper_bound)
//
// This is the abstraction you described: "两个 operand 邻接表 + 上界过滤 + 输出计数归约".
// It intentionally does NOT carry rectangle-specific fields.
struct IntersectCountTask {
    int a_vertex;    // kept for cache key / debug
    int b_vertex;    // kept for cache key / debug
    int a_start;     // BCSR block offset (data_t index)
    int a_len;       // number of BCSR blocks (data_t)
    int b_start;     // BCSR block offset (data_t index)
    int b_len;       // number of BCSR blocks (data_t)
    int upper_bound; // apply v < upper_bound in SIU
};

inline NeighborRange resolve_vertex_range(
    const int* gmem_row_ptr,
    int num_vertices,
    int vertex_id
) {
#pragma HLS INLINE off
    NeighborRange r;
    if (vertex_id < 0 || vertex_id >= num_vertices) {
        r.start = 0;
        r.len = 0;
        return r;
    }

    const int start = gmem_row_ptr[vertex_id];
    const int end = gmem_row_ptr[vertex_id + 1];
    r.start = start;
    r.len = end - start;
    return r;
}

inline void execution_task_to_intersect_count_task_stage(
    const int* gmem_row_ptr,
    int num_vertices,
    hls::stream<ExecutionTask>& exec_in,
    hls::stream<IntersectCountTask>& out_tasks
) {
#pragma HLS INLINE off

ADAPT_EXEC_TO_INTERSECT:
    while (true) {
#pragma HLS PIPELINE II=1
        ExecutionTask t = exec_in.read();

        if (t.is_eos()) {
            IntersectCountTask eos;
            eos.a_vertex = -1;
            eos.b_vertex = -1;
            eos.a_start = 0;
            eos.a_len = 0;
            eos.b_start = 0;
            eos.b_len = 0;
            eos.upper_bound = -1;
            out_tasks.write(eos);
            break;
        }

        if (t.op != EXEC_OP_INTERSECT_COUNT) {
            continue;
        }

        NeighborRange r1 = resolve_vertex_range(gmem_row_ptr, num_vertices, t.v1);
        NeighborRange r2 = resolve_vertex_range(gmem_row_ptr, num_vertices, t.v2);

        IntersectCountTask out;
        out.a_vertex = t.v1;
        out.b_vertex = t.v2;
        out.a_start = r1.start;
        out.a_len = r1.len;
        out.b_start = r2.start;
        out.b_len = r2.len;
        out.upper_bound = t.upper_bound;
        out_tasks.write(out);
    }
}

// ============================================================================
// Step0.9: Fixed-depth (<=5) generic plan pipeline (Embedding stream)
// - Build a reusable LevelUnit chain driven by QueryPlan5::levels[].
// - This is intentionally independent from the existing PE/CE intersect-count
//   engines; it enables INTERSECT_EMIT (emit vertices) so deeper expansions work.
// ============================================================================

struct Embedding5 {
    int vid[QUERYPLAN_MAX_LEVELS];
    ap_uint<3> valid; // 0..5, number of valid vertices
    bool eos;

    static Embedding5 make_eos() {
        Embedding5 e;
#pragma HLS ARRAY_PARTITION variable=e.vid complete dim=1
        for (int i = 0; i < QUERYPLAN_MAX_LEVELS; ++i) {
#pragma HLS UNROLL
            e.vid[i] = -1;
        }
        e.valid = 0;
        e.eos = true;
        return e;
    }

    bool is_eos() const {
        return eos;
    }
};

inline int plan5_resolve_bound(const LevelConfig& cfg, const Embedding5& in) {
#pragma HLS INLINE
    switch (cfg.bound_kind) {
        case PLAN_BOUND_CONST:
            return cfg.bound_const;
        case PLAN_BOUND_VID_SLOT:
            // Trust host to provide valid slot in range [0, QUERYPLAN_MAX_LEVELS-1]
            return in.vid[cfg.bound_slot];
        case PLAN_BOUND_NONE:
        default:
            return 0x7fffffff;
    }
}

inline bool plan5_check_new_lt_bound(const LevelConfig& cfg, int new_vid, int bound) {
#pragma HLS INLINE
    const bool need = (cfg.flags & LEVELCFG_FLAG_REQUIRE_NEW_LT_BOUND) != 0;
    if (!need) return true;
    return new_vid < bound;
}

inline void plan5_forward_stage(
    hls::stream<Embedding5>& in_stream,
    hls::stream<Embedding5>& out_stream
) {
#pragma HLS INLINE off
FORWARD_LOOP:
    while (true) {
#pragma HLS PIPELINE II=1
        Embedding5 e = in_stream.read();
        out_stream.write(e);
        if (e.is_eos()) break;
    }
}

inline void plan5_scan_all_stage(
    const QueryPlan5& plan,
    int num_vertices,
    hls::stream<Embedding5> out_streams[NUM_PE]
) {
#pragma HLS INLINE off
    int pe_rr = 0;
    int v0_begin = plan.v0_begin;
    if (v0_begin < 0) v0_begin = 0;
    int v0_end = (plan.v0_end <= 0) ? num_vertices : plan.v0_end;
    if (v0_end > num_vertices) v0_end = num_vertices;

V0_SCAN:
    for (int v0 = v0_begin; v0 < v0_end; ++v0) {
#pragma HLS PIPELINE II=1
        Embedding5 e;
#pragma HLS ARRAY_PARTITION variable=e.vid complete dim=1
        for (int i = 0; i < QUERYPLAN_MAX_LEVELS; ++i) {
#pragma HLS UNROLL
            e.vid[i] = -1;
        }
        e.vid[0] = v0;
        e.valid = 1;
        e.eos = false;
        out_streams[pe_rr].write(e);
        pe_rr = (pe_rr + 1) % NUM_PE;
    }

SEND_EOS:
    for (int pe = 0; pe < NUM_PE; ++pe) {
#pragma HLS UNROLL
        out_streams[pe].write(Embedding5::make_eos());
    }
}

inline void plan5_expand_nbr_stage(
    const LevelConfig& cfg,
    const data_t* gmem_bcsr_data_scan,
    const int* gmem_row_ptr,
    int num_vertices,
    hls::stream<Embedding5>& in_stream,
    hls::stream<Embedding5>& out_stream
) {
#pragma HLS INLINE off
    const int src_slot = cfg.src_slot0;
    const int dst_slot = cfg.dst_slot;

EXPAND_LOOP:
    while (true) {
        Embedding5 in = in_stream.read();
        if (in.is_eos()) {
            out_stream.write(in);
            break;
        }

        const int u = (src_slot >= 0 && src_slot < QUERYPLAN_MAX_LEVELS) ? in.vid[src_slot] : -1;
        if (u < 0 || u >= num_vertices) {
            continue;
        }
        const int start = gmem_row_ptr[u];
        const int end = gmem_row_ptr[u + 1];

        const int bound = plan5_resolve_bound(cfg, in);

    BLK_LOOP:
        for (int i = start; i < end; ++i) {
#pragma HLS PIPELINE II=1
            const data_t blk = gmem_bcsr_data_scan[i];  //读取block
            const int base = (int)blk.get_index() * BCSR_bitmap;
            if ((cfg.flags & LEVELCFG_FLAG_REQUIRE_NEW_LT_BOUND) && base >= bound) {
                break;
            }
            const bcsr_bitmap bm = blk.get_bitmap();

        BIT_LOOP:
            for (int bit = 0; bit < BCSR_bitmap; ++bit) {
#pragma HLS PIPELINE II=1
                if (!bm[bit]) continue;
                const int v = base + bit;
                if (!plan5_check_new_lt_bound(cfg, v, bound)) {
                    continue;
                }

                Embedding5 out = in;
#pragma HLS ARRAY_PARTITION variable=out.vid complete dim=1
                if (dst_slot >= 0 && dst_slot < QUERYPLAN_MAX_LEVELS) {
                    out.vid[dst_slot] = v;
                    ap_uint<3> new_valid = in.valid;
                    if (dst_slot + 1 > (int)new_valid) new_valid = (ap_uint<3>)(dst_slot + 1);
                    out.valid = new_valid;
                }
                out.eos = false;
                out_stream.write(out);
            }
        }
    }
}

inline void plan5_intersect_emit_stage(
    const LevelConfig& cfg,
    const data_t* gmem_bcsr_data_a,
    const data_t* gmem_bcsr_data_b,
    const int* gmem_row_ptr,
    int num_vertices,
    hls::stream<Embedding5>& in_stream,
    hls::stream<Embedding5>& out_stream
) {
#pragma HLS INLINE off
    const int src_a = cfg.src_slot0;
    const int src_b = cfg.src_slot1;
    const int dst_slot = cfg.dst_slot;

    data_t local_a[MAX_BCSR_BLOCKS];
    data_t local_b[MAX_BCSR_BLOCKS];
    data_t inter_blocks[MAX_BCSR_BLOCKS];
#pragma HLS BIND_STORAGE variable=local_a type=ram_t2p impl=bram
#pragma HLS BIND_STORAGE variable=local_b type=ram_t2p impl=bram
#pragma HLS BIND_STORAGE variable=inter_blocks type=ram_t2p impl=bram
#pragma HLS ARRAY_PARTITION variable=inter_blocks cyclic factor=8 dim=1

INTERSECT_LOOP:
    while (true) {
        Embedding5 in = in_stream.read();
        if (in.is_eos()) {
            out_stream.write(in);
            break;
        }

        const int va = (src_a >= 0 && src_a < QUERYPLAN_MAX_LEVELS) ? in.vid[src_a] : -1;
        const int vb = (src_b >= 0 && src_b < QUERYPLAN_MAX_LEVELS) ? in.vid[src_b] : -1;
        if (va < 0 || va >= num_vertices || vb < 0 || vb >= num_vertices) {
            continue;
        }

        int ia = gmem_row_ptr[va];
        int ea = gmem_row_ptr[va + 1];
        int ib = gmem_row_ptr[vb];
        int eb = gmem_row_ptr[vb + 1];
        if (ia >= ea || ib >= eb) {
            continue;
        }

        const int bound = plan5_resolve_bound(cfg, in);

        int len_a = ea - ia;
        if (len_a > MAX_BCSR_BLOCKS) len_a = MAX_BCSR_BLOCKS;
        int len_b = eb - ib;
        if (len_b > MAX_BCSR_BLOCKS) len_b = MAX_BCSR_BLOCKS;

    LOAD_A_LOCAL:
        for (int i = 0; i < len_a; ++i) {
#pragma HLS PIPELINE II=1
            local_a[i] = gmem_bcsr_data_a[ia + i];
        }
    LOAD_B_LOCAL:
        for (int i = 0; i < len_b; ++i) {
#pragma HLS PIPELINE II=1
            local_b[i] = gmem_bcsr_data_b[ib + i];
        }

        const bool need_bound = (cfg.flags & LEVELCFG_FLAG_REQUIRE_NEW_LT_BOUND) != 0;
        const int upper_bound = need_bound ? bound : -1;

        int inter_len = 0;
        plan5_siu_collect_intersection_blocks(local_a, len_a, local_b, len_b, upper_bound, inter_blocks, inter_len);

    EMIT_INTERSECTION_BLOCKS:
        for (int bi = 0; bi < inter_len; ++bi) {
#pragma HLS PIPELINE II=1
            const data_t blk = inter_blocks[bi];
            const int base = (int)blk.get_index() * BCSR_bitmap;
            if (need_bound && base >= bound) {
                break;
            }
            const bcsr_bitmap bm = blk.get_bitmap();

        EMIT_BITS:
            for (int bit = 0; bit < BCSR_bitmap; ++bit) {
#pragma HLS PIPELINE II=1
                if (!bm[bit]) continue;
                const int v = base + bit;
                if (!plan5_check_new_lt_bound(cfg, v, bound)){
                    continue;
                } 

                Embedding5 out = in;
#pragma HLS ARRAY_PARTITION variable=out.vid complete dim=1
                if (dst_slot >= 0 && dst_slot < QUERYPLAN_MAX_LEVELS) {
                    out.vid[dst_slot] = v;
                    ap_uint<3> new_valid = in.valid;
                    if (dst_slot + 1 > (int)new_valid) new_valid = (ap_uint<3>)(dst_slot + 1);
                    out.valid = new_valid;
                }
                out.eos = false;
                out_stream.write(out);
            }
        }
    }
}

inline void plan5_level_unit(
    const LevelConfig& cfg,
    const data_t* gmem_bcsr_data_v1,
    const data_t* gmem_bcsr_data_v2,
    const int* gmem_row_ptr,
    int num_vertices,
    hls::stream<Embedding5>& in_stream,
    hls::stream<Embedding5>& out_stream
) {
#pragma HLS INLINE off
    if (cfg.op == PLAN_OP_NOOP) {
        plan5_forward_stage(in_stream, out_stream);
    } else if (cfg.op == PLAN_OP_EXPAND_NBR) {
        plan5_expand_nbr_stage(cfg, gmem_bcsr_data_v1, gmem_row_ptr, num_vertices, in_stream, out_stream);
    } else if (cfg.op == PLAN_OP_INTERSECT_EMIT) {
        plan5_intersect_emit_stage(cfg, gmem_bcsr_data_v1, gmem_bcsr_data_v2, gmem_row_ptr, num_vertices, in_stream, out_stream);
    } else {
        // unsupported op -> just forward
        plan5_forward_stage(in_stream, out_stream);
    }
}

inline void plan5_count_embeddings(
    hls::stream<Embedding5>& in_stream,
    hls::stream<long long>& out_count
) {
#pragma HLS INLINE off
    long long total = 0;
COUNT_LOOP:
    while (true) {
#pragma HLS PIPELINE II=1
        Embedding5 e = in_stream.read();
        if (e.is_eos()) break;
        total++;
    }
    out_count.write(total);
}

inline void plan5_pipeline_pe_df(
    const LevelConfig& cfg0,
    const LevelConfig& cfg1,
    const LevelConfig& cfg2,
    const LevelConfig& cfg3,
    const LevelConfig& cfg4,
    const data_t* gmem_bcsr_data_v1_L0,
    const data_t* gmem_bcsr_data_v2_L0,
    const data_t* gmem_bcsr_data_v1_L1,
    const data_t* gmem_bcsr_data_v2_L1,
    const data_t* gmem_bcsr_data_v1_L2,
    const data_t* gmem_bcsr_data_v2_L2,
    const data_t* gmem_bcsr_data_v1_L3,
    const data_t* gmem_bcsr_data_v2_L3,
    const data_t* gmem_bcsr_data_v1_L4,
    const data_t* gmem_bcsr_data_v2_L4,
    const int* row_ptr0,
    const int* row_ptr1,
    const int* row_ptr2,
    const int* row_ptr3,
    const int* row_ptr4,
    int num_vertices,
    hls::stream<Embedding5>& in0,
    hls::stream<long long>& out_count
) {
#pragma HLS INLINE off
    hls::stream<Embedding5> s1;
    hls::stream<Embedding5> s2;
    hls::stream<Embedding5> s3;
    hls::stream<Embedding5> s4;
    hls::stream<Embedding5> s5;

#pragma HLS STREAM variable=s1 depth=64
#pragma HLS STREAM variable=s2 depth=64
#pragma HLS STREAM variable=s3 depth=64
#pragma HLS STREAM variable=s4 depth=64
#pragma HLS STREAM variable=s5 depth=64

#pragma HLS DATAFLOW
    plan5_level_unit(cfg0, gmem_bcsr_data_v1_L0, gmem_bcsr_data_v2_L0, row_ptr0, num_vertices, in0, s1);
    plan5_level_unit(cfg1, gmem_bcsr_data_v1_L1, gmem_bcsr_data_v2_L1, row_ptr1, num_vertices, s1, s2);
    plan5_level_unit(cfg2, gmem_bcsr_data_v1_L2, gmem_bcsr_data_v2_L2, row_ptr2, num_vertices, s2, s3);
    plan5_level_unit(cfg3, gmem_bcsr_data_v1_L3, gmem_bcsr_data_v2_L3, row_ptr3, num_vertices, s3, s4);
    plan5_level_unit(cfg4, gmem_bcsr_data_v1_L4, gmem_bcsr_data_v2_L4, row_ptr4, num_vertices, s4, s5);
    plan5_count_embeddings(s5, out_count);
}

inline void plan5_pipeline_pe(
    const QueryPlan5& plan,
    // Dedicated memory ports per level to avoid DATAFLOW m_axi port conflicts.
    const data_t* gmem_bcsr_data_v1_L0,
    const data_t* gmem_bcsr_data_v2_L0,
    const data_t* gmem_bcsr_data_v1_L1,
    const data_t* gmem_bcsr_data_v2_L1,
    const data_t* gmem_bcsr_data_v1_L2,
    const data_t* gmem_bcsr_data_v2_L2,
    const data_t* gmem_bcsr_data_v1_L3,
    const data_t* gmem_bcsr_data_v2_L3,
    const data_t* gmem_bcsr_data_v1_L4,
    const data_t* gmem_bcsr_data_v2_L4,
    const int* gmem_row_ptr,
    int num_vertices,
    hls::stream<Embedding5>& in0,
    hls::stream<long long>& out_count
) {
#pragma HLS INLINE off

    // Cache row_ptr into on-chip RAM before entering DATAFLOW.
    // Then replicate it into 5 independent memories so each DATAFLOW process
    // (each plan5_level_unit) reads its own RAM and avoids port conflicts.
    // Note: Requires num_vertices + 1 <= PLAN5_ROW_PTR_CACHE_MAX_ENTRIES.
    int row_ptr_l0[PLAN5_ROW_PTR_CACHE_MAX_ENTRIES];
    int row_ptr_l1[PLAN5_ROW_PTR_CACHE_MAX_ENTRIES];
    int row_ptr_l2[PLAN5_ROW_PTR_CACHE_MAX_ENTRIES];
    int row_ptr_l3[PLAN5_ROW_PTR_CACHE_MAX_ENTRIES];
    int row_ptr_l4[PLAN5_ROW_PTR_CACHE_MAX_ENTRIES];
#pragma HLS BIND_STORAGE variable=row_ptr_l0 type=ram_2p impl=uram
#pragma HLS BIND_STORAGE variable=row_ptr_l1 type=ram_2p impl=uram
#pragma HLS BIND_STORAGE variable=row_ptr_l2 type=ram_2p impl=uram
#pragma HLS BIND_STORAGE variable=row_ptr_l3 type=ram_2p impl=uram
#pragma HLS BIND_STORAGE variable=row_ptr_l4 type=ram_2p impl=uram
ROW_PTR_LOAD_AND_REPL:
    for (int i = 0; i < num_vertices + 1; ++i) {
#pragma HLS PIPELINE II=1
        const int v = gmem_row_ptr[i];
        row_ptr_l0[i] = v;
        row_ptr_l1[i] = v;
        row_ptr_l2[i] = v;
        row_ptr_l3[i] = v;
        row_ptr_l4[i] = v;
    }

    const int* row_ptr0 = row_ptr_l0;
    const int* row_ptr1 = row_ptr_l1;
    const int* row_ptr2 = row_ptr_l2;
    const int* row_ptr3 = row_ptr_l3;
    const int* row_ptr4 = row_ptr_l4;

    // Local copy of level configs; missing levels are treated as NOOP.
    LevelConfig cfgs[QUERYPLAN_MAX_LEVELS];
#pragma HLS ARRAY_PARTITION variable=cfgs complete dim=1
CFG_INIT:
    for (int i = 0; i < QUERYPLAN_MAX_LEVELS; ++i) {
#pragma HLS UNROLL
        if (i < plan.num_levels) {
            cfgs[i] = plan.levels[i];
        } else {
            LevelConfig c;
            c.op = PLAN_OP_NOOP;
            c.dst_slot = 0;
            c.src_slot0 = 0;
            c.src_slot1 = 0;
            c.bound_kind = PLAN_BOUND_NONE;
            c.bound_slot = 0;
            c.bound_const = 0;
            c.flags = 0;
            cfgs[i] = c;
        }
    }

    // Run DATAFLOW network after row_ptr is fully populated.
    plan5_pipeline_pe_df(
        cfgs[0], cfgs[1], cfgs[2], cfgs[3], cfgs[4],
        gmem_bcsr_data_v1_L0, gmem_bcsr_data_v2_L0,
        gmem_bcsr_data_v1_L1, gmem_bcsr_data_v2_L1,
        gmem_bcsr_data_v1_L2, gmem_bcsr_data_v2_L2,
        gmem_bcsr_data_v1_L3, gmem_bcsr_data_v2_L3,
        gmem_bcsr_data_v1_L4, gmem_bcsr_data_v2_L4,
        row_ptr0, row_ptr1, row_ptr2, row_ptr3, row_ptr4,
        num_vertices,
        in0, out_count
    );

}
inline void result_collector(
    hls::stream<long long> result_streams[NUM_PE],
    long long* out_result
) {
    #pragma HLS INLINE off  //新加的
    long long total_count = 0;

    for (int i = 0; i < NUM_PE; ++i) {
#pragma HLS PIPELINE off
        long long partial_res = result_streams[i].read();
        total_count += partial_res;
    }

    out_result[0] = total_count;
}

