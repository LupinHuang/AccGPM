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

struct VertexCache {
    int vertex_id;
    int length;
    data_t blocks[MAX_BCSR_BLOCKS];

    VertexCache() : vertex_id(-1), length(0) {}
};

struct ComputeMeta {
    int v1_len;
    int v2_len;
    int v1_start;
    int v2_start;
    int upper_bound;
    bool is_end; 
};

// Cache wide words (512b) instead of unpacked data_t blocks.
// This allows zero-copy push to CE streams on a miss (stream directly), while
// still enabling a simple last-vertex reuse cache.
constexpr int MAX_WIDE_WORDS = (MAX_BCSR_BLOCKS + BLOCKS_PER_WIDE - 1) / BLOCKS_PER_WIDE;

struct VertexCacheWide {
    int vertex_id;
    int start;
    int length;
    ap_uint<AXI_WIDE_BITS> words[MAX_WIDE_WORDS];
    int num_chunks;

    VertexCacheWide() : vertex_id(-1), start(0), length(0), num_chunks(0) {}
};


inline void stream_two_vertices_wide_if_needed(
    const data_t* gmem_bcsr_data_v1,
    int start_v1,
    int length_v1,
    int vertex_id_v1,
    VertexCacheWide& cache_v1,
    hls::stream<ap_uint<AXI_WIDE_BITS>>& v1_out,
    const data_t* gmem_bcsr_data_v2,
    int start_v2,
    int length_v2,
    int vertex_id_v2,
    VertexCacheWide& cache_v2,
    hls::stream<ap_uint<AXI_WIDE_BITS>>& v2_out
) {
#pragma HLS INLINE off

    const int lane_offset_v1 = (length_v1 == 0) ? 0 : (start_v1 % BLOCKS_PER_WIDE);
    const int lane_offset_v2 = (length_v2 == 0) ? 0 : (start_v2 % BLOCKS_PER_WIDE);
    const int num_chunks_v1 = (length_v1 == 0) ? 0 : ((length_v1 + lane_offset_v1 + BLOCKS_PER_WIDE - 1) / BLOCKS_PER_WIDE);
    const int num_chunks_v2 = (length_v2 == 0) ? 0 : ((length_v2 + lane_offset_v2 + BLOCKS_PER_WIDE - 1) / BLOCKS_PER_WIDE);

    const bool hit_v1 = (num_chunks_v1 == 0) || ((cache_v1.vertex_id == vertex_id_v1) && (cache_v1.start == start_v1) &&
                                                (cache_v1.length == length_v1) && (cache_v1.num_chunks == num_chunks_v1));
    const bool hit_v2 = (num_chunks_v2 == 0) || ((cache_v2.vertex_id == vertex_id_v2) && (cache_v2.start == start_v2) &&
                                                (cache_v2.length == length_v2) && (cache_v2.num_chunks == num_chunks_v2));

    const ap_uint<AXI_WIDE_BITS>* wide_ptr_v1 =
        reinterpret_cast<const ap_uint<AXI_WIDE_BITS>*>(gmem_bcsr_data_v1);
    const ap_uint<AXI_WIDE_BITS>* wide_ptr_v2 =
        reinterpret_cast<const ap_uint<AXI_WIDE_BITS>*>(gmem_bcsr_data_v2);

    const int wide_idx_v1 = (num_chunks_v1 == 0) ? 0 : (start_v1 / BLOCKS_PER_WIDE);
    const int wide_idx_v2 = (num_chunks_v2 == 0) ? 0 : (start_v2 / BLOCKS_PER_WIDE);

    const int max_chunks = (num_chunks_v1 > num_chunks_v2) ? num_chunks_v1 : num_chunks_v2;

STREAM_WIDE_BOTH:
    for (int chunk = 0; chunk < max_chunks; ++chunk) {
#pragma HLS PIPELINE II=1
        if (chunk < num_chunks_v1) {
            ap_uint<AXI_WIDE_BITS> w = hit_v1 ? cache_v1.words[chunk] : wide_ptr_v1[wide_idx_v1 + chunk];
            v1_out.write(w);
            if (!hit_v1) {
                cache_v1.words[chunk] = w;
            }
        }
        if (chunk < num_chunks_v2) {
            ap_uint<AXI_WIDE_BITS> w = hit_v2 ? cache_v2.words[chunk] : wide_ptr_v2[wide_idx_v2 + chunk];
            v2_out.write(w);
            if (!hit_v2) {
                cache_v2.words[chunk] = w;
            }
        }
    }

    if (!hit_v1) {
        cache_v1.vertex_id = vertex_id_v1;
        cache_v1.start = start_v1;
        cache_v1.length = length_v1;
        cache_v1.num_chunks = num_chunks_v1;
    }
    if (!hit_v2) {
        cache_v2.vertex_id = vertex_id_v2;
        cache_v2.start = start_v2;
        cache_v2.length = length_v2;
        cache_v2.num_chunks = num_chunks_v2;
    }
}



/*inline int process_rectangle_task(
    const data_t* gmem_bcsr_data,
    const RectangleTask& task,
    VertexCache& cache_v1,
    VertexCache& cache_v2
) {
#pragma HLS INLINE off

    int size_v1 = task.v1_len;
    int size_v2 = task.v2_len;
    if (size_v1 > MAX_BCSR_BLOCKS) size_v1 = MAX_BCSR_BLOCKS;
    if (size_v2 > MAX_BCSR_BLOCKS) size_v2 = MAX_BCSR_BLOCKS;

    load_vertex_if_needed(gmem_bcsr_data, task.v1_start, size_v1, task.v1, cache_v1);
    load_vertex_if_needed(gmem_bcsr_data, task.v2_start, size_v2, task.v2, cache_v2);

    int ret = 0;
    // 每个周期输入一批 BATCH_SIZE（MIN 之后向量），由 BATCH_SIZE×FIFO 在输入侧解耦数据依赖
    siu_intersection_cached_fifo_pipelined(cache_v1.blocks, size_v1, cache_v2.blocks, size_v2, task.upper_bound, ret);
    return ret;
}*/

// ============================================================================
// PE Worker 与 任务调度相关定义
// ============================================================================

// 定义 PE 数量
#define NUM_PE 4

// 每个 PE 内复制的 SIU 计算引擎数量（复制 SIU 硬件，实现 task 间 SIU 重叠）
// 资源开销：local_v1/local_v2 + SIU pipeline 会按 NUM_CE 线性增长
constexpr int NUM_CE = 3;

inline void token_manager(
    hls::stream<bool>& stop_in,
    hls::stream<ap_uint<8>> return_tokens_in[NUM_CE],
    hls::stream<bool> done_in[NUM_CE],
    hls::stream<ap_uint<8>>& free_ce_out
) {
#pragma HLS INLINE off

    // 初始时把所有 CE token 放回池里
INIT_TOKENS:
    for (int i = 0; i < NUM_CE; ++i) {
#pragma HLS PIPELINE II=1
        free_ce_out.write((ap_uint<8>)i);
    }

    bool stop = false;
    int done_count = 0;

TOKEN_MGR_LOOP:
    while (done_count < NUM_CE) {
#pragma HLS PIPELINE II=1
        if (!stop && !stop_in.empty()) {
            stop = stop_in.read();
        }

    CHECK_DONE:
        for (int ce = 0; ce < NUM_CE; ++ce) {
#pragma HLS UNROLL
            if (!done_in[ce].empty()) {
                (void)done_in[ce].read();
                done_count++;
            }
        }

    DRAIN_RET_TOKENS:
        for (int ce = 0; ce < NUM_CE; ++ce) {
#pragma HLS UNROLL
            if (!return_tokens_in[ce].empty()) {
                ap_uint<8> t = return_tokens_in[ce].read();
                // stop 后 Loader 不再消费 free_ce_out，继续写会导致死锁；因此 stop 后只 drain 丢弃。
                if (!stop) {
                    free_ce_out.write(t);
                }
            }
        }
    }
}

inline void pe_load_stage_multi_ce(
    const data_t* gmem_bcsr_data_v1,
    const data_t* gmem_bcsr_data_v2,
    hls::stream<RectangleTask>& task_in,
    hls::stream<ap_uint<8>>& free_ce_in,
    hls::stream<bool>& stop_tokens_out,
    hls::stream<ComputeMeta> meta_out[NUM_CE],
    hls::stream<ap_uint<AXI_WIDE_BITS>> v1_wide_out[NUM_CE],
    hls::stream<ap_uint<AXI_WIDE_BITS>> v2_wide_out[NUM_CE]
) {
#pragma HLS INLINE off

    VertexCacheWide cache_v1;
    VertexCacheWide cache_v2;
#pragma HLS BIND_STORAGE variable=cache_v1.words type=ram_t2p impl=bram
#pragma HLS BIND_STORAGE variable=cache_v2.words type=ram_t2p impl=bram
#pragma HLS ARRAY_PARTITION variable=cache_v1.words cyclic factor=2 dim=1
#pragma HLS ARRAY_PARTITION variable=cache_v2.words cyclic factor=2 dim=1

LOAD_LOOP_MULTI_CE:
    while (true) {
#pragma HLS PIPELINE off
        RectangleTask task = task_in.read();

        if (task.v0 == -1) {
            // 通知 token manager：之后不要再往 free_ce_out 写（避免死锁）
            stop_tokens_out.write(true);

            // 广播 EOS 给所有 CE：让每个 CE 都能退出并输出自己的 local_total
            ComputeMeta end_meta;
            end_meta.is_end = true;
        BROADCAST_EOS:
            for (int ce = 0; ce < NUM_CE; ++ce) {
#pragma HLS UNROLL
                meta_out[ce].write(end_meta);
            }
            break;
        }

        // 只有拿到 free token 的 CE 才能接收一个新 task，保证每个 CE 最多 1 个 task in-flight
        ap_uint<8> ce_id = free_ce_in.read();

        ComputeMeta meta;
        meta.is_end = false;
        meta.upper_bound = task.upper_bound;

        int size_v1 = task.v1_len;
        if (size_v1 > MAX_BCSR_BLOCKS) size_v1 = MAX_BCSR_BLOCKS;
        meta.v1_len = size_v1;
        meta.v1_start = task.v1_start;

        int size_v2 = task.v2_len;
        if (size_v2 > MAX_BCSR_BLOCKS) size_v2 = MAX_BCSR_BLOCKS;
        meta.v2_len = size_v2;
        meta.v2_start = task.v2_start;

        // 先发 meta，compute engine 才能开始拉数据
        meta_out[(int)ce_id].write(meta);

        // Push 512b wide words directly to the target CE streams.
        // This removes the extra "cache.blocks[] -> stream" copy on a miss.
        stream_two_vertices_wide_if_needed(
            gmem_bcsr_data_v1, task.v1_start, size_v1, task.v1, cache_v1, v1_wide_out[(int)ce_id],
            gmem_bcsr_data_v2, task.v2_start, size_v2, task.v2, cache_v2, v2_wide_out[(int)ce_id]);
    }
}

inline void ce_compute_engine(
    ap_uint<8> ce_id,
    hls::stream<ComputeMeta>& meta_in,
    hls::stream<ap_uint<AXI_WIDE_BITS>>& v1_wide_in,
    hls::stream<ap_uint<AXI_WIDE_BITS>>& v2_wide_in,
    hls::stream<ap_uint<8>>& return_token_out,
    hls::stream<bool>& done_out,
    hls::stream<long long>& ce_sum_out
) {
#pragma HLS INLINE off

    long long local_total = 0;

    data_t local_v1[MAX_BCSR_BLOCKS];
    data_t local_v2[MAX_BCSR_BLOCKS];
#pragma HLS BIND_STORAGE variable=local_v1 type=ram_t2p impl=bram
#pragma HLS BIND_STORAGE variable=local_v2 type=ram_t2p impl=bram

CE_COMPUTE_LOOP:
    while (true) {
#pragma HLS PIPELINE off
        ComputeMeta meta = meta_in.read();
        if (meta.is_end) {
            break;
        }

        const int lane_offset_v1 = (meta.v1_len == 0) ? 0 : (meta.v1_start % BLOCKS_PER_WIDE);
        const int lane_offset_v2 = (meta.v2_len == 0) ? 0 : (meta.v2_start % BLOCKS_PER_WIDE);
        const int num_chunks_v1 = (meta.v1_len == 0) ? 0 : ((meta.v1_len + lane_offset_v1 + BLOCKS_PER_WIDE - 1) / BLOCKS_PER_WIDE);
        const int num_chunks_v2 = (meta.v2_len == 0) ? 0 : ((meta.v2_len + lane_offset_v2 + BLOCKS_PER_WIDE - 1) / BLOCKS_PER_WIDE);
        const int max_chunks = (num_chunks_v1 > num_chunks_v2) ? num_chunks_v1 : num_chunks_v2;

        data_t unpacked_v1[BLOCKS_PER_WIDE];
        data_t unpacked_v2[BLOCKS_PER_WIDE];
#pragma HLS ARRAY_PARTITION variable=unpacked_v1 complete
#pragma HLS ARRAY_PARTITION variable=unpacked_v2 complete

    RECV_WIDE_TO_LOCAL:
        for (int chunk = 0; chunk < max_chunks; ++chunk) {
#pragma HLS PIPELINE II=1
            if (chunk < num_chunks_v1) {
                ap_uint<AXI_WIDE_BITS> wide_word = v1_wide_in.read();
                unpack_wide_word(wide_word, unpacked_v1);
                for (int lane = 0; lane < BLOCKS_PER_WIDE; ++lane) {
#pragma HLS UNROLL
                    const int global_pos = chunk * BLOCKS_PER_WIDE + lane;
                    const int write_idx = global_pos - lane_offset_v1;
                    if (write_idx >= 0 && write_idx < meta.v1_len) {
                        local_v1[write_idx] = unpacked_v1[lane];
                    }
                }
            }

            if (chunk < num_chunks_v2) {
                ap_uint<AXI_WIDE_BITS> wide_word = v2_wide_in.read();
                unpack_wide_word(wide_word, unpacked_v2);
                for (int lane = 0; lane < BLOCKS_PER_WIDE; ++lane) {
#pragma HLS UNROLL
                    const int global_pos = chunk * BLOCKS_PER_WIDE + lane;
                    const int write_idx = global_pos - lane_offset_v2;
                    if (write_idx >= 0 && write_idx < meta.v2_len) {
                        local_v2[write_idx] = unpacked_v2[lane];
                    }
                }
            }
        }

        int ret = 0;
        siu_intersection_cached_fifo_pipelined(local_v1, meta.v1_len, local_v2, meta.v2_len, meta.upper_bound, ret);
        local_total += ret;

        // 归还 token：Loader 才能继续把新 task 分配给该 CE
        return_token_out.write(ce_id);
    }

    done_out.write(true);
    ce_sum_out.write(local_total);
}

inline void ce_sums_collector(
    hls::stream<long long> ce_sum_in[NUM_CE],
    hls::stream<long long>& result_stream
) {
#pragma HLS INLINE off
    long long total = 0;
COLLECT_CE_SUMS:
    for (int ce = 0; ce < NUM_CE; ++ce) {
#pragma HLS PIPELINE off
        total += ce_sum_in[ce].read();
    }
    result_stream.write(total);
}

/**
 * PE Worker: 持续从流中获取任务并处理
 * 协议：接收到 v0 == -1 的任务表示结束
 */
inline void pe_worker(
    const data_t* gmem_bcsr_data_v1,
    const data_t* gmem_bcsr_data_v2,
    hls::stream<RectangleTask>& task_stream,
    hls::stream<long long>& result_stream
) {
#pragma HLS INLINE off  //新加的

    hls::stream<ap_uint<8>> free_ce_stream;
    hls::stream<bool> stop_tokens_stream;
    hls::stream<ap_uint<8>> return_token_streams[NUM_CE];
    hls::stream<bool> done_streams[NUM_CE];
    hls::stream<ComputeMeta> meta_streams[NUM_CE];
    hls::stream<ap_uint<AXI_WIDE_BITS>> v1_wide_streams[NUM_CE];
    hls::stream<ap_uint<AXI_WIDE_BITS>> v2_wide_streams[NUM_CE];
    hls::stream<long long> ce_sum_streams[NUM_CE];

#pragma HLS STREAM variable=free_ce_stream depth=NUM_CE
#pragma HLS BIND_STORAGE variable=free_ce_stream type=fifo impl=srl

#pragma HLS STREAM variable=stop_tokens_stream depth=1
#pragma HLS BIND_STORAGE variable=stop_tokens_stream type=fifo impl=srl

#pragma HLS STREAM variable=return_token_streams depth=2
#pragma HLS STREAM variable=done_streams depth=1
#pragma HLS BIND_STORAGE variable=return_token_streams type=fifo impl=srl
#pragma HLS BIND_STORAGE variable=done_streams type=fifo impl=srl

#pragma HLS STREAM variable=meta_streams depth=8
#pragma HLS STREAM variable=v1_wide_streams depth=32
#pragma HLS STREAM variable=v2_wide_streams depth=32
#pragma HLS STREAM variable=ce_sum_streams depth=1

#pragma HLS BIND_STORAGE variable=meta_streams type=fifo impl=srl
#pragma HLS BIND_STORAGE variable=v1_wide_streams type=fifo impl=uram
#pragma HLS BIND_STORAGE variable=v2_wide_streams type=fifo impl=uram
#pragma HLS BIND_STORAGE variable=ce_sum_streams type=fifo impl=srl

#pragma HLS DATAFLOW
    token_manager(stop_tokens_stream, return_token_streams, done_streams, free_ce_stream);
    pe_load_stage_multi_ce(gmem_bcsr_data_v1, gmem_bcsr_data_v2, task_stream, free_ce_stream, stop_tokens_stream,
                          meta_streams, v1_wide_streams, v2_wide_streams);

    // 复制 NUM_CE 份 SIU 计算硬件：不同 task 的 SIU 计算可并行重叠
CE_ENGINES:
    for (int ce = 0; ce < NUM_CE; ++ce) {
#pragma HLS UNROLL
        ce_compute_engine((ap_uint<8>)ce,
                          meta_streams[ce],
                          v1_wide_streams[ce],
                          v2_wide_streams[ce],
                          return_token_streams[ce],
                          done_streams[ce],
                          ce_sum_streams[ce]);
    }

    ce_sums_collector(ce_sum_streams, result_stream);

}

/**
 * Task Dispatcher: 从 Global Memory 读取任务并分发给 PE
 * 采用简单的轮询 (Round-Robin) 分发策略
 */
inline void task_prefetcher(
    const RectangleTask* task_queue,
    int num_tasks,
    hls::stream<RectangleTask>& prefetched_tasks
) {
#pragma HLS INLINE off
PREFETCH_LOOP:
    for (int i = 0; i < num_tasks; ++i) {
#pragma HLS PIPELINE II=1
        prefetched_tasks.write(task_queue[i]);
    }
}
//分割线
inline void task_round_robin(
    hls::stream<RectangleTask>& prefetched_tasks,
    int num_tasks,
    hls::stream<RectangleTask> task_streams[NUM_PE]
) {
#pragma HLS INLINE off
    int dispatched = 0;
    int pe_ptr = 0;
    RectangleTask cached_task;
    bool has_cached_task = false;

DISPATCH_LOOP:
    while (dispatched < num_tasks) {
#pragma HLS PIPELINE II=2
        if (!has_cached_task) {
            cached_task = prefetched_tasks.read();
            has_cached_task = true;
        }

        int current_pe = pe_ptr;
        bool wrote = false;

        if (has_cached_task && !task_streams[current_pe].full()) {
            task_streams[current_pe].write(cached_task);
            wrote = true;
            dispatched++;
            has_cached_task = false;
        }

        pe_ptr = (current_pe + 1) % NUM_PE;
    }
}
//这个with_eos版本在round_robin后面加了结束信号的发送,是函数的封装
inline void task_round_robin_with_eos(
    hls::stream<RectangleTask>& prefetched_tasks,
    int num_tasks,
    hls::stream<RectangleTask> task_streams[NUM_PE]
) {
#pragma HLS INLINE off
    task_round_robin(prefetched_tasks, num_tasks, task_streams);

    RectangleTask end_signal;
    end_signal.v0 = -1;
    for (int i = 0; i < NUM_PE; ++i) {
#pragma HLS UNROLL
        task_streams[i].write(end_signal);
    }
}
//分割线

/**
 * Result Collector: 收集所有 PE 的结果并汇总
 */
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
