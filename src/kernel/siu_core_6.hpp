#pragma once
//siu_core_1是进行siu_core的第一次重大修改，进行了32bit的封装改进，测试SIU
#include "merge_kernel_SIU_v8_gemini_1.h"
#include <hls_stream.h>

#ifndef __SYNTHESIS__
#include <vector>
#include <string>
#include <iostream>
#endif

// -----------------------------------------------------------------------------
// SIU helper kernels
// -----------------------------------------------------------------------------

inline void single_cas_stage(const process_data_vector &in_buf,
                             process_data_vector &out_buf, int level)
{
#pragma HLS INLINE 
    const int stride = 1 << level;
    const int num_seg = BATCH_SIZE / (2 * stride);

CAS_Loop:
    for (int seg = 0; seg < num_seg; ++seg)
    {
#pragma HLS UNROLL
    CAS_STRIDE_LOOP:
        for (int j = 0; j < stride; ++j)
        {
#pragma HLS UNROLL
            const int left_Idx = seg * 2 * stride + j;
            const int right_Idx = left_Idx + stride;

            process_data_t l = in_buf.lane[left_Idx];
            process_data_t r = in_buf.lane[right_Idx];

            bool l_is_pad = l.bcsr_element.isPadding();
            bool r_is_pad = r.bcsr_element.isPadding();

            bool pick_L = r_is_pad || (!l_is_pad && (l.bcsr_element.get_index() <= r.bcsr_element.get_index()));
            
            process_data_t l_new = pick_L ? l : r;
            process_data_t r_new = pick_L ? r : l;

            bool matched = (!l_new.bcsr_element.isPadding() && !r_new.bcsr_element.isPadding() &&
                            l_new.bcsr_element.get_index() == r_new.bcsr_element.get_index());
            l_new.matched = matched;
            r_new.matched = matched;

            out_buf.lane[left_Idx] = l_new;
            out_buf.lane[right_Idx] = r_new;
        }
    }
}

inline void read_last_reg_stage(
    process_data_t last_element_reg_in, 
    hls::stream<process_data_t>& last_element_stream_out
) {
#pragma HLS PIPELINE II=1
    last_element_stream_out.write(last_element_reg_in);
}

inline void MIN_stage(
    const data_t inA[BATCH_SIZE],
    const data_t inB[BATCH_SIZE],
    hls::stream<process_data_t>& last_element_MIN_in, 
    hls::stream<process_data_vector>& out_bitonic_stream,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_A_MIN_out,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_B_MIN_out,
    hls::stream<process_data_t>& last_element_MIN_out 
) {       
#pragma HLS PIPELINE II=1
#pragma HLS ARRAY_PARTITION variable=inA complete dim=1
#pragma HLS ARRAY_PARTITION variable=inB complete dim=1

    process_data_t last_element_reg = last_element_MIN_in.read();

    process_data_vector IR_AFTER_MIN;
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_MIN.lane complete
    ap_uint<BATCH_SIZE> maskA_local = 0;
    ap_uint<BATCH_SIZE> maskB_local = 0;

    MIN_stage_loop:
    for (int i = 0; i < BATCH_SIZE; i++) {
#pragma HLS UNROLL
        int j = BATCH_SIZE - 1 - i;

        const data_t &Compare_inA = inA[i];
        const data_t &Compare_inB = inB[j];

        bool A_min = Compare_inB.isPadding() || 
                (!Compare_inA.isPadding() && Compare_inA.get_index() <= Compare_inB.get_index());

        IR_AFTER_MIN.lane[i].bcsr_element = A_min ? Compare_inA : Compare_inB;
        IR_AFTER_MIN.lane[i].from_where = A_min ? false : true; 
        IR_AFTER_MIN.lane[i].matched = false; 

        if (A_min && !Compare_inA.isPadding()) {
            maskA_local[i] = 1;
        }
        if (!A_min && !Compare_inB.isPadding()) {
            maskB_local[j] = 1;
        }
    }
    out_bitonic_stream.write(IR_AFTER_MIN);
    pop_mask_A_MIN_out.write(maskA_local);
    pop_mask_B_MIN_out.write(maskB_local);  
    last_element_MIN_out.write(last_element_reg); 
}

inline void CAS_stage(
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_A_MIN_in,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_B_MIN_in,
    hls::stream<process_data_t>& last_element_MIN_in, 
    hls::stream<process_data_vector>& in_bitonic_stream,
    hls::stream<process_data_vector>& out_sorted_stream,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_A_CAS_out,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_B_CAS_out,
    hls::stream<process_data_t>& last_element_CAS_out 
) {     

#pragma HLS PIPELINE II=1

        ap_uint<BATCH_SIZE> maskA_local = pop_mask_A_MIN_in.read();
        ap_uint<BATCH_SIZE> maskB_local = pop_mask_B_MIN_in.read();
        process_data_t last_element_reg = last_element_MIN_in.read(); 

        process_data_vector IR_AFTER_MIN;
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_MIN.lane complete
        IR_AFTER_MIN = in_bitonic_stream.read();
        
        process_data_vector IR_CAS[Num_of_process_log + 1];
#pragma HLS ARRAY_PARTITION variable=IR_CAS complete
       
        CAS_stage_init:
        for (int i = 0; i < BATCH_SIZE; i++) {
#pragma HLS UNROLL
            IR_CAS[0].lane[i] = IR_AFTER_MIN.lane[i];
        }

        CAS_stages_loop:
        for (int i = 0; i < Num_of_process_log; i++)
        {
#pragma HLS UNROLL
            single_cas_stage(IR_CAS[i], IR_CAS[i + 1], Num_of_process_log - 1 - i);
        }
                
        process_data_vector IR_AFTER_CAS;
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_CAS.lane complete

        CAS_stage_output_loop:
        for (int i = 0; i < BATCH_SIZE; i++) {
#pragma HLS UNROLL
            IR_AFTER_CAS.lane[i] = IR_CAS[Num_of_process_log].lane[i];
        }
        out_sorted_stream.write(IR_AFTER_CAS);
        
        pop_mask_A_CAS_out.write(maskA_local);
        pop_mask_B_CAS_out.write(maskB_local);
        last_element_CAS_out.write(last_element_reg); 
}

inline void Merge_stage(
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_A_CAS_in,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_B_CAS_in,
    hls::stream<process_data_t>& last_element_CAS_in, 
    hls::stream<process_data_vector>& in_sorted_stream,
    hls::stream<process_data_vector>& out_compact_stream,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_A_Merge_out,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_B_Merge_out,
    hls::stream<process_data_t>& last_element_Merge_out 
) {
#pragma HLS PIPELINE II=1

        ap_uint<BATCH_SIZE> maskA_local = pop_mask_A_CAS_in.read();
        ap_uint<BATCH_SIZE> maskB_local = pop_mask_B_CAS_in.read();
        process_data_t last_element_reg = last_element_CAS_in.read(); 

        process_data_vector IR_AFTER_CAS;
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_CAS.lane complete
        IR_AFTER_CAS = in_sorted_stream.read();
        process_data_vector IR_AFTER_Merge;
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_Merge.lane complete 

        process_data_t sorted_seg[BATCH_SIZE];
#pragma HLS ARRAY_PARTITION variable=sorted_seg complete

        READ_MERGE_LOOP:
        for (int i = 0; i < BATCH_SIZE; i++)
        {
#pragma HLS UNROLL
            sorted_seg[i] = IR_AFTER_CAS.lane[i];
        }
        
        Merge_stage_loop:
        for(int i = 0; i < BATCH_SIZE; i++ ) {
#pragma HLS UNROLL
            process_data_t left_element = (i == 0) ? last_element_reg : sorted_seg[i-1];
            process_data_t right_element = sorted_seg[i];
            
            IR_AFTER_Merge.lane[i] = right_element; 
            
            bool whether_matched = false;
            if (!left_element.bcsr_element.isPadding() && 
                !right_element.bcsr_element.isPadding() && 
                (left_element.bcsr_element.get_index() == right_element.bcsr_element.get_index()))
            {
                whether_matched = true;
            }
            if (whether_matched) {
                bcsr_bitmap intersect_bitmap = left_element.bcsr_element.get_bitmap() & right_element.bcsr_element.get_bitmap();
                IR_AFTER_Merge.lane[i].bcsr_element.set_bitmap(intersect_bitmap);
            } else {
                IR_AFTER_Merge.lane[i].bcsr_element.set_bitmap(0); 
                IR_AFTER_Merge.lane[i].bcsr_element.set_index(data_t::padding_index()); 
            }
            IR_AFTER_Merge.lane[i].matched = whether_matched;
        }
        out_compact_stream.write(IR_AFTER_Merge);
        
        last_element_reg = sorted_seg[BATCH_SIZE - 1]; 

        pop_mask_A_Merge_out.write(maskA_local);
        pop_mask_B_Merge_out.write(maskB_local);
        last_element_Merge_out.write(last_element_reg); 
}

inline void Compact_stage(
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_A_Merge_in,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_B_Merge_in,
    hls::stream<process_data_t>& last_element_Merge_in, 
    hls::stream<process_data_vector>& in_compact_stream,
    data_t out_intersection[BATCH_SIZE],
    hls::stream<process_data_t>& last_element_Compact_out,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_A_Compact_out,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_B_Compact_out
) {     
#pragma HLS PIPELINE II = 1
#pragma HLS ARRAY_PARTITION variable=out_intersection complete dim=1

        ap_uint<BATCH_SIZE> maskA_local = pop_mask_A_Merge_in.read();
        ap_uint<BATCH_SIZE> maskB_local = pop_mask_B_Merge_in.read();
        process_data_t last_element_reg = last_element_Merge_in.read(); 

        process_data_vector IR_AFTER_Merge = in_compact_stream.read();
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_Merge.lane complete dim=1

        data_t After_Compact[BATCH_SIZE];
        bool valid[BATCH_SIZE];
        int addr[BATCH_SIZE];

#pragma HLS ARRAY_PARTITION variable=After_Compact complete
#pragma HLS ARRAY_PARTITION variable=valid complete
#pragma HLS ARRAY_PARTITION variable=addr complete

        Compact_valid_loop:
        for (int i = 0; i < BATCH_SIZE; i++) {
#pragma HLS UNROLL
            valid[i] = !IR_AFTER_Merge.lane[i].bcsr_element.isPadding(); 
        }

        int prefix = 0;
    Compact_prefix_loop:
        for (int i = 0; i < BATCH_SIZE; i++) {
#pragma HLS UNROLL
            addr[i] = prefix;
            prefix += valid[i] ? 1 : 0;
        }

        Compact_prefill_loop:
        for (int i = 0; i < BATCH_SIZE; i++) {
#pragma HLS UNROLL
            After_Compact[i] = data_t::padding_element();
        }

        Compact_scatter_loop:
        for (int i = 0; i < BATCH_SIZE; i++) {
#pragma HLS UNROLL
            if (valid[i]) {
                After_Compact[addr[i]] = IR_AFTER_Merge.lane[i].bcsr_element;
            }
        }

        Write_output_loop:
        for (int i = 0; i < BATCH_SIZE; i++) {
#pragma HLS UNROLL
            out_intersection[i] = After_Compact[i];
        }
        
        pop_mask_A_Compact_out.write(maskA_local);
        pop_mask_B_Compact_out.write(maskB_local);
        last_element_Compact_out.write(last_element_reg); 
}

inline void write_outputs_stage(
    hls::stream<process_data_t>& last_element_stream_in,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_A_Compact_in,
    hls::stream<ap_uint<BATCH_SIZE>>& pop_mask_B_Compact_in,
    process_data_t& last_element_reg_out,
    ap_uint<BATCH_SIZE>& pop_mask_A_out,
    ap_uint<BATCH_SIZE>& pop_mask_B_out
) {
#pragma HLS PIPELINE II=1
    last_element_reg_out = last_element_stream_in.read();
    pop_mask_A_out = pop_mask_A_Compact_in.read();
    pop_mask_B_out = pop_mask_B_Compact_in.read();
}

inline void siu_core_dataflow(
    const data_t inA[BATCH_SIZE],
    const data_t inB[BATCH_SIZE],
    process_data_t last_element_reg_in,
    data_t out_intersection[BATCH_SIZE],
    ap_uint<BATCH_SIZE>& pop_mask_A,
    ap_uint<BATCH_SIZE>& pop_mask_B,
    process_data_t& last_element_reg_out
) {
#pragma HLS DATAFLOW
//#pragma HLS INLINE
    hls::stream<process_data_vector> min_to_cas_stream;
    hls::stream<process_data_vector> cas_to_merge_stream;
    hls::stream<process_data_vector> merge_to_compact_stream;
    
    hls::stream<ap_uint<BATCH_SIZE>> pop_mask_A_MIN_out;
    hls::stream<ap_uint<BATCH_SIZE>> pop_mask_B_MIN_out;
    hls::stream<ap_uint<BATCH_SIZE>> pop_mask_A_CAS_out;
    hls::stream<ap_uint<BATCH_SIZE>> pop_mask_B_CAS_out;
    hls::stream<ap_uint<BATCH_SIZE>> pop_mask_A_Merge_out;
    hls::stream<ap_uint<BATCH_SIZE>> pop_mask_B_Merge_out;
    
    hls::stream<process_data_t> last_element_read_stream; 
    hls::stream<process_data_t> last_element_MIN_out;
    hls::stream<process_data_t> last_element_CAS_out;
    hls::stream<process_data_t> last_element_Merge_out;
    hls::stream<process_data_t> last_element_final_stream; 

    hls::stream<ap_uint<BATCH_SIZE>> pop_mask_A_Compact_out;
    hls::stream<ap_uint<BATCH_SIZE>> pop_mask_B_Compact_out;

#pragma HLS STREAM variable=min_to_cas_stream       depth=2
#pragma HLS STREAM variable=cas_to_merge_stream     depth=2
#pragma HLS STREAM variable=merge_to_compact_stream depth=BATCH_SIZE
#pragma HLS STREAM variable=pop_mask_A_MIN_out       depth=2
#pragma HLS STREAM variable=pop_mask_B_MIN_out       depth=2
#pragma HLS STREAM variable=pop_mask_A_CAS_out       depth=2
#pragma HLS STREAM variable=pop_mask_B_CAS_out       depth=2
#pragma HLS STREAM variable=pop_mask_A_Merge_out     depth=2
#pragma HLS STREAM variable=pop_mask_B_Merge_out     depth=2
#pragma HLS STREAM variable=last_element_read_stream   depth=2
#pragma HLS STREAM variable=last_element_MIN_out     depth=2
#pragma HLS STREAM variable=last_element_CAS_out     depth=2
#pragma HLS STREAM variable=last_element_Merge_out   depth=2
#pragma HLS STREAM variable=last_element_final_stream  depth=2
#pragma HLS STREAM variable=pop_mask_A_Compact_out   depth=2
#pragma HLS STREAM variable=pop_mask_B_Compact_out   depth=2

    read_last_reg_stage(last_element_reg_in, last_element_read_stream);
    MIN_stage(inA, inB, last_element_read_stream, min_to_cas_stream, pop_mask_A_MIN_out, pop_mask_B_MIN_out, last_element_MIN_out);
    CAS_stage(pop_mask_A_MIN_out, pop_mask_B_MIN_out, last_element_MIN_out, min_to_cas_stream, cas_to_merge_stream, pop_mask_A_CAS_out, pop_mask_B_CAS_out, last_element_CAS_out);
    Merge_stage(pop_mask_A_CAS_out, pop_mask_B_CAS_out, last_element_CAS_out, cas_to_merge_stream, merge_to_compact_stream, pop_mask_A_Merge_out, pop_mask_B_Merge_out, last_element_Merge_out);
    Compact_stage(pop_mask_A_Merge_out, pop_mask_B_Merge_out, last_element_Merge_out, 
                  merge_to_compact_stream, out_intersection, 
                  last_element_final_stream, 
                  pop_mask_A_Compact_out, pop_mask_B_Compact_out);
    write_outputs_stage(last_element_final_stream, 
                        pop_mask_A_Compact_out, pop_mask_B_Compact_out,
                        last_element_reg_out, pop_mask_A, pop_mask_B);
}

// =============================================================================
// 新增：基于 BATCH_SIZE×FIFO 的“每拍一批”流式 SIU（用于消除 siu_intersection_cached 外层 while 的串行依赖）
// 设计要点：
// - 输入侧：把 cache_n_v1 / cache_n_v2 先按 lane（idx % BATCH_SIZE）灌入 BATCH_SIZE 个 FIFO。
// - 每个周期：从 A[i] FIFO 取队头，与 B[N-1-i] FIFO 队头比较，直接生成 MIN 之后的向量并推进对应 FIFO。
// - 后级：CAS/Merge/Compact 采用流式 while+PIPELINE II=1；Merge 内部用局部变量保存 last_element 状态，
//         在收到 is_last 后复位（避免 static 触发潜在 RAW 依赖问题）。
// - 功能保持不变：MIN 选择逻辑、CAS 网络、Merge 交集计算与 Compact 压实逻辑完全复用原实现。
// =============================================================================

static constexpr int SIU_FIFO_DEPTH = (MAX_BCSR_BLOCKS / BATCH_SIZE);
static_assert(SIU_FIFO_DEPTH * BATCH_SIZE == MAX_BCSR_BLOCKS,
              "MAX_BCSR_BLOCKS must be divisible by BATCH_SIZE for lane FIFOs");

inline data_t apply_upper_bound_and_filter(
    const data_t &in,
    int upper_bound,
    bool &stop_all
) {
#pragma HLS INLINE
    stop_all = false;
    if (in.isPadding()) {
        return data_t::padding_element();
    }

    data_t val = in;
    int base = val.get_index() * BCSR_bitmap;

    // 仅做局部掩码，不再通过 stop_all 触发数据相关的 break，从而降低 load_*_fifo 的控制依赖。
    if (upper_bound >= 0) {
        if (base >= upper_bound) {
            return data_t::padding_element();
        }
        if (base + BCSR_bitmap > upper_bound) {
            int keep = upper_bound - base;
            if (keep <= 0) {
                val.set_bitmap(0);
            } else if (keep < BCSR_bitmap) {
                bcsr_bitmap masked = (bcsr_bitmap(1) << keep) - 1;
                val.set_bitmap(val.get_bitmap() & masked);
            }
        }
    }

    if (val.get_bitmap() == 0) {
        return data_t::padding_element();
    }
    return val;
}

inline void Feeder_FIFO_MIN_stage(
    const data_t cache_n_v1[MAX_BCSR_BLOCKS],
    int size_v1,
    const data_t cache_n_v2[MAX_BCSR_BLOCKS],
    int size_v2,
    int upper_bound,
    hls::stream<process_data_vector> &min_to_cas_stream,
    hls::stream<bool> &ctrl_out
) {
#pragma HLS INLINE off

    // 8 个 lane FIFO（A/B 各一组），深度 256（2048/8）
    data_t fifoA[BATCH_SIZE][SIU_FIFO_DEPTH];
    data_t fifoB[BATCH_SIZE][SIU_FIFO_DEPTH];
#pragma HLS ARRAY_PARTITION variable=fifoA complete dim=1 
#pragma HLS ARRAY_PARTITION variable=fifoB complete dim=1

    ap_uint<9> headA[BATCH_SIZE];
    ap_uint<9> headB[BATCH_SIZE];
    ap_uint<9> countA[BATCH_SIZE];
    ap_uint<9> countB[BATCH_SIZE];
#pragma HLS ARRAY_PARTITION variable=headA complete
#pragma HLS ARRAY_PARTITION variable=headB complete
#pragma HLS ARRAY_PARTITION variable=countA complete
#pragma HLS ARRAY_PARTITION variable=countB complete

init_fifo_ptrs:
    for (int lane = 0; lane < BATCH_SIZE; ++lane) {
#pragma HLS UNROLL
        headA[lane] = 0;
        headB[lane] = 0;
        countA[lane] = 0;
        countB[lane] = 0;
    }

    // ----------------------
    // 预取：把 cache 按 idx%BATCH_SIZE 分发到对应 lane FIFO
    // ----------------------
    // 预先裁剪循环上界：当 upper_bound 生效时，最多遍历 ceil(upper_bound / BCSR_bitmap) 个块，避免数据相关的 break 破坏 II。
    const int max_block_a = (upper_bound < 0) ? size_v1 : ((upper_bound + BCSR_bitmap - 1) / BCSR_bitmap);
    const int limit_a = (max_block_a < size_v1) ? max_block_a : size_v1;
    load_A_fifo:
    for (int idx = 0; idx < limit_a; ++idx) {
#pragma HLS PIPELINE II=1
        bool stop_all = false;
        data_t filtered = apply_upper_bound_and_filter(cache_n_v1[idx], upper_bound, stop_all);
        if (!filtered.isPadding()) {
            const int lane = idx % BATCH_SIZE;
            if (countA[lane] < SIU_FIFO_DEPTH) {
                const ap_uint<9> wpos = (headA[lane] + countA[lane]) % SIU_FIFO_DEPTH;
                fifoA[lane][wpos] = filtered;
                countA[lane] = countA[lane] + 1;
            }
        }
    }

    const int max_block_b = (upper_bound < 0) ? size_v2 : ((upper_bound + BCSR_bitmap - 1) / BCSR_bitmap);
    const int limit_b = (max_block_b < size_v2) ? max_block_b : size_v2;
    load_B_fifo:
    for (int idx = 0; idx < limit_b; ++idx) {
#pragma HLS PIPELINE II=1
        bool stop_all = false;
        data_t filtered = apply_upper_bound_and_filter(cache_n_v2[idx], upper_bound, stop_all);
        if (!filtered.isPadding()) {
            const int lane = idx % BATCH_SIZE;
            if (countB[lane] < SIU_FIFO_DEPTH) {
                const ap_uint<9> wpos = (headB[lane] + countB[lane]) % SIU_FIFO_DEPTH;
                fifoB[lane][wpos] = filtered;
                countB[lane] = countB[lane] + 1;
            }
        }
    }

    // ----------------------
    // 主循环：每拍生成一批 MIN 之后的向量，并推进被选中的 lane FIFO
    // ----------------------

    // 为了切断 BRAM 读到 stream 写的一拍组合链，采用“前一拍写，当前拍算”的双缓冲。
    process_data_vector pending_ir;
#pragma HLS ARRAY_PARTITION variable=pending_ir.lane complete
    bool pending_ctrl = false;
    bool has_pending = false;
feed_stream_loop:
    while (true) {
#pragma HLS PIPELINE II=1
        // 先把上一拍算好的 batch 写出去，这样当前拍的 BRAM 读不再直接驱动 stream 写。
        if (has_pending) {
            min_to_cas_stream.write(pending_ir);
            ctrl_out.write(pending_ctrl);
            has_pending = false;
        }

        bool all_empty = true;
    check_empty_lanes:
        for (int lane = 0; lane < BATCH_SIZE; ++lane) {
#pragma HLS UNROLL
            if (countA[lane] != 0 || countB[lane] != 0) {
                all_empty = false;
            }
        }

        process_data_vector IR_AFTER_MIN;
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_MIN.lane complete

        // 当全部 FIFO 都空时，发一个 is_last=1 的“空 batch”用于冲刷并复位后级状态
        if (all_empty) {
empty_loop:
            for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
                IR_AFTER_MIN.lane[i].bcsr_element = data_t::padding_element();
                IR_AFTER_MIN.lane[i].from_where = false;
                IR_AFTER_MIN.lane[i].matched = false;
            }
            pending_ir = IR_AFTER_MIN;
            pending_ctrl = true;
            has_pending = true;
            break;
        }

        bool popA[BATCH_SIZE];
        bool popB[BATCH_SIZE];
#pragma HLS ARRAY_PARTITION variable=popA complete
#pragma HLS ARRAY_PARTITION variable=popB complete
    init_pop_flags:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            popA[i] = false;
            popB[i] = false;
        }

    build_min_vector:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            const int j = BATCH_SIZE - 1 - i;

            // 这里避免使用三目运算符去“选择一个内存读地址”，否则在部分 Vitis HLS 版本
            // 的 clang reflow 阶段可能触发 pointer-select elimination 的段错误（ICE）。
            data_t a = data_t::padding_element();
            data_t b = data_t::padding_element();
            if (countA[i] != 0) a = fifoA[i][headA[i]];
            if (countB[j] != 0) b = fifoB[j][headB[j]];

            bool A_min = b.isPadding() || (!a.isPadding() && (a.get_index() <= b.get_index()));
            data_t chosen = a;
            if (!A_min) chosen = b;

            IR_AFTER_MIN.lane[i].bcsr_element = chosen;
            IR_AFTER_MIN.lane[i].from_where = A_min ? false : true;
            IR_AFTER_MIN.lane[i].matched = false;

            if (A_min && !a.isPadding()) popA[i] = true;
            if (!A_min && !b.isPadding()) popB[j] = true;
        }

        // 推进 FIFO（被选中的 lane 出队一个）
    pop_fifoA:
        for (int lane = 0; lane < BATCH_SIZE; ++lane) {
#pragma HLS UNROLL
            if (popA[lane] && countA[lane] != 0) {
                headA[lane] = headA[lane] + 1;
                countA[lane] = countA[lane] - 1;
            }
        }
    pop_fifoB:
        for (int lane = 0; lane < BATCH_SIZE; ++lane) {
#pragma HLS UNROLL
            if (popB[lane] && countB[lane] != 0) {
                headB[lane] = headB[lane] + 1;
                countB[lane] = countB[lane] - 1;
            }
        }

        pending_ir = IR_AFTER_MIN;
        pending_ctrl = false;
        has_pending = true;
    }

    // flush 最后一拍缓冲（可能是尾部空 batch）
    if (has_pending) {
        min_to_cas_stream.write(pending_ir);
        ctrl_out.write(pending_ctrl);
    }
}

inline void CAS_stage_stream_fifo(
    hls::stream<process_data_vector> &in_min_stream,
    hls::stream<process_data_vector> &out_sorted_stream,
    hls::stream<bool> &ctrl_in,
    hls::stream<bool> &ctrl_out
) {
#pragma HLS INLINE off
CAS_STREAM_LOOP_FIFO:
    while (true) {
#pragma HLS PIPELINE II=1
    // 重要：先读 data 再读 ctrl，并在 data 输出后再输出 ctrl，避免 ctrl/data 走两条 FIFO 时产生“走位”导致 dataflow 死锁。
    process_data_vector IR_AFTER_MIN = in_min_stream.read();
    bool is_last = ctrl_in.read();
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_MIN.lane complete
        process_data_vector IR_CAS[Num_of_process_log + 1];
#pragma HLS ARRAY_PARTITION variable=IR_CAS complete

    CAS_stage_init:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            IR_CAS[0].lane[i] = IR_AFTER_MIN.lane[i];
        }
    CAS_stages_loop:
        for (int level = 0; level < Num_of_process_log; ++level) {
#pragma HLS UNROLL
            single_cas_stage(IR_CAS[level], IR_CAS[level + 1], Num_of_process_log - 1 - level);
        }

        process_data_vector IR_AFTER_CAS;
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_CAS.lane complete
    CAS_stage_output:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            IR_AFTER_CAS.lane[i] = IR_CAS[Num_of_process_log].lane[i];
        }

        out_sorted_stream.write(IR_AFTER_CAS);
        ctrl_out.write(is_last);
        if (is_last) break;
    }
}

inline void Merge_stage_stream_fifo(
    hls::stream<process_data_vector> &in_sorted_stream,
    hls::stream<process_data_vector> &out_compact_stream,
    hls::stream<bool> &ctrl_in,
    hls::stream<bool> &ctrl_out
) {
#pragma HLS INLINE off
    // 用“函数进程内局部变量”保持 last_element，避免 static 在综合时触发依赖检查问题。
    process_data_t last_element_reg;
    last_element_reg.bcsr_element = data_t::padding_element();
    last_element_reg.matched = false;
    last_element_reg.from_where = false;

MERGE_STREAM_LOOP_FIFO:
    while (true) {
#pragma HLS PIPELINE II=1
    process_data_vector IR_AFTER_CAS = in_sorted_stream.read();
    bool is_last = ctrl_in.read();
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_CAS.lane complete
        process_data_vector IR_AFTER_Merge;
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_Merge.lane complete

        process_data_t sorted_seg[BATCH_SIZE];
#pragma HLS ARRAY_PARTITION variable=sorted_seg complete

    READ_MERGE_LOOP:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            sorted_seg[i] = IR_AFTER_CAS.lane[i];
        }

    Merge_stage_loop:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            process_data_t left_element = (i == 0) ? last_element_reg : sorted_seg[i - 1];
            process_data_t right_element = sorted_seg[i];
            IR_AFTER_Merge.lane[i] = right_element;

            bool matched = false;
            if (!left_element.bcsr_element.isPadding() &&
                !right_element.bcsr_element.isPadding() &&
                (left_element.bcsr_element.get_index() == right_element.bcsr_element.get_index())) {
                matched = true;
            }
            if (matched) {
                bcsr_bitmap intersect_bitmap = left_element.bcsr_element.get_bitmap() & right_element.bcsr_element.get_bitmap();
                IR_AFTER_Merge.lane[i].bcsr_element.set_bitmap(intersect_bitmap);
            } else {
                IR_AFTER_Merge.lane[i].bcsr_element.set_bitmap(0);
                IR_AFTER_Merge.lane[i].bcsr_element.set_index(data_t::padding_index());
            }
            IR_AFTER_Merge.lane[i].matched = matched;
        }

        out_compact_stream.write(IR_AFTER_Merge);
    ctrl_out.write(is_last);
        last_element_reg = sorted_seg[BATCH_SIZE - 1];

        if (is_last) {
            // 任务结束复位，便于下一次调用
            last_element_reg.bcsr_element = data_t::padding_element();
            last_element_reg.matched = false;
            last_element_reg.from_where = false;
            break;
        }
    }
}

inline void Compact_stage_stream_fifo(
    hls::stream<process_data_vector> &in_compact_stream,
    hls::stream<data_t> out_intersection_stream[BATCH_SIZE],
    hls::stream<bool> &ctrl_in,
    hls::stream<bool> &ctrl_out
) {
#pragma HLS INLINE off
COMPACT_STREAM_LOOP_FIFO:
    while (true) {
#pragma HLS PIPELINE II=1
        // 多路“原子”握手：只有当输入(data+ctrl)就绪且所有输出 lane + ctrl 都有空间时，才进行一次完整的读/写。
        // 否则会出现“写了部分 lane 但卡在某个 full lane”的不平衡，进而与下游按顺序读取产生 dataflow 死锁。
        bool out_ready = !ctrl_out.full();
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            out_ready = out_ready && !out_intersection_stream[i].full();
        }

        if (!(out_ready && !in_compact_stream.empty() && !ctrl_in.empty())) {
            continue;
        }

        process_data_vector IR_AFTER_Merge = in_compact_stream.read();
        bool is_last = ctrl_in.read();
#pragma HLS ARRAY_PARTITION variable=IR_AFTER_Merge.lane complete

        data_t After_Compact[BATCH_SIZE];
        bool valid[BATCH_SIZE];
        int addr[BATCH_SIZE];

#pragma HLS ARRAY_PARTITION variable=After_Compact complete
#pragma HLS ARRAY_PARTITION variable=valid complete
#pragma HLS ARRAY_PARTITION variable=addr complete

    Compact_valid_loop:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            valid[i] = !IR_AFTER_Merge.lane[i].bcsr_element.isPadding();
        }

        int prefix = 0;
    Compact_prefix_loop:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            addr[i] = prefix;
            prefix += valid[i] ? 1 : 0;
        }

    Compact_prefill_loop:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            After_Compact[i] = data_t::padding_element();
        }

    Compact_scatter_loop:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            if (valid[i]) {
                After_Compact[addr[i]] = IR_AFTER_Merge.lane[i].bcsr_element;
            }
        }

    Write_output_loop_fifo:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            out_intersection_stream[i].write(After_Compact[i]);
        }

        ctrl_out.write(is_last);

        if (is_last) break;
    }
}

inline void Accumulate_stage_fifo(
    hls::stream<data_t> in_intersection_stream[BATCH_SIZE],
    hls::stream<bool> &ctrl_in,
    long long &intersection_count_out
) {
#pragma HLS INLINE off
    long long total = 0;
ACC_LOOP_FIFO:
    while (true) {
#pragma HLS PIPELINE II=1
        // 多路“原子”读取：只有当 8 路 lane + ctrl 全部有数据时才消费，避免按顺序 read() 导致的 lane 不平衡死锁。
        bool in_ready = !ctrl_in.empty();
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            in_ready = in_ready && !in_intersection_stream[i].empty();
        }

        if (!in_ready) {
            continue;
        }

        int batch_hits = 0;
    read_batch_fifo:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            data_t v = in_intersection_stream[i].read();
            if (!v.isPadding()) {
                batch_hits += v.popcount();
            }
        }

        bool is_last = ctrl_in.read();

        total += batch_hits;
        if (is_last) break;
    }
    intersection_count_out = total;
}
/////////////////////////分割线
//新加入plan5_store_intersection_blocks_fifo，输出数组形式的交集块结果

inline void plan5_store_intersection_blocks_fifo(
    hls::stream<data_t> in_intersection_stream[BATCH_SIZE],
    hls::stream<bool>& ctrl_in,
    data_t out_blocks[MAX_BCSR_BLOCKS],
    hls::stream<int>& out_len_stream
) {
#pragma HLS INLINE off
// 8-bank cyclic partition so an unrolled batch can scatter-write safely.
#pragma HLS ARRAY_PARTITION variable=out_blocks cyclic factor=8 dim=1
    int out_len = 0;
STORE_INTERSECTION_BLOCKS:
    while (true) {
#pragma HLS PIPELINE II=1
        bool in_ready = !ctrl_in.empty();
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            in_ready = in_ready && !in_intersection_stream[i].empty();
        }
        if (!in_ready) {
            continue;
        }

        // NOTE: Compact_stage_stream_fifo already compacts valid elements to
        // lanes [0..k-1] and fills the rest with padding.
        // So we can do fixed-lane writes out_blocks[out_len + i] for i<k.
        data_t blk[BATCH_SIZE];
        bool valid[BATCH_SIZE];
#pragma HLS ARRAY_PARTITION variable=blk complete
#pragma HLS ARRAY_PARTITION variable=valid complete

        int batch_count = 0;
    READ_BATCH:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            blk[i] = in_intersection_stream[i].read();
            valid[i] = !blk[i].isPadding();
            batch_count += valid[i] ? 1 : 0;
        }

        const int space_left = (out_len < MAX_BCSR_BLOCKS) ? (MAX_BCSR_BLOCKS - out_len) : 0;
        const int write_count = (batch_count < space_left) ? batch_count : space_left;

    PACKED_WRITE:
        for (int i = 0; i < BATCH_SIZE; ++i) {
#pragma HLS UNROLL
            if (i < write_count) {
                // i is a compile-time constant per unrolled lane; with cyclic factor=8
                // this maps to distinct banks each cycle.
                out_blocks[out_len + i] = blk[i];
            }
        }

        out_len += write_count;

        bool is_last = ctrl_in.read();
        if (is_last) {
            break;
        }
    }
    out_len_stream.write(out_len);
}

inline void plan5_read_out_len_stage(
    hls::stream<int>& in_len_stream,
    int& out_len
) {
#pragma HLS INLINE off
    out_len = in_len_stream.read();
}

/////////////////////////分割线
inline void store_siu_result_fifo(long long total_hits, int &out_result) {
#pragma HLS INLINE off
    out_result = static_cast<int>(total_hits);
}

// 顶层封装：保持与 siu_core_4 中 pipelined 接口类似的调用方式，便于 task_executor 替换。
inline void siu_intersection_cached_fifo_pipelined(
    const data_t cache_n_v1[MAX_BCSR_BLOCKS],
    int size_v1,
    const data_t cache_n_v2[MAX_BCSR_BLOCKS],
    int size_v2,
    int upper_bound,
    int &out_result
) {
#pragma HLS INLINE off
    hls::stream<process_data_vector> min_to_cas_stream;
    hls::stream<process_data_vector> cas_to_merge_stream;
    hls::stream<process_data_vector> merge_to_compact_stream;
    hls::stream<bool> ctrl_f2c;
    hls::stream<bool> ctrl_c2m;
    hls::stream<bool> ctrl_m2cp;
    hls::stream<bool> ctrl_cp2acc;

    hls::stream<data_t> compact_out[BATCH_SIZE];

#pragma HLS STREAM variable=min_to_cas_stream depth=2
#pragma HLS STREAM variable=cas_to_merge_stream depth=2
#pragma HLS STREAM variable=merge_to_compact_stream depth=BATCH_SIZE
#pragma HLS STREAM variable=ctrl_f2c depth=2
#pragma HLS STREAM variable=ctrl_c2m depth=2
#pragma HLS STREAM variable=ctrl_m2cp depth=2
#pragma HLS STREAM variable=ctrl_cp2acc depth=2
#pragma HLS STREAM variable=compact_out depth=2

    long long total_hits = 0;

#pragma HLS DATAFLOW
    Feeder_FIFO_MIN_stage(cache_n_v1, size_v1, cache_n_v2, size_v2, upper_bound,
                          min_to_cas_stream, ctrl_f2c);
    CAS_stage_stream_fifo(min_to_cas_stream, cas_to_merge_stream, ctrl_f2c, ctrl_c2m);
    Merge_stage_stream_fifo(cas_to_merge_stream, merge_to_compact_stream, ctrl_c2m, ctrl_m2cp);
    Compact_stage_stream_fifo(merge_to_compact_stream, compact_out, ctrl_m2cp, ctrl_cp2acc);
    Accumulate_stage_fifo(compact_out, ctrl_cp2acc, total_hits);

    store_siu_result_fifo(total_hits, out_result);
}
////////////////////
//新的顶层封装：支持输出交集块结果（输出：out_blocks + out_len）
inline void plan5_siu_collect_intersection_blocks(
    const data_t local_a[MAX_BCSR_BLOCKS],
    int len_a,
    const data_t local_b[MAX_BCSR_BLOCKS],
    int len_b,
    int upper_bound,
    data_t out_blocks[MAX_BCSR_BLOCKS],
    int& out_len
) {
#pragma HLS INLINE off
    hls::stream<process_data_vector> min_to_cas_stream;
    hls::stream<process_data_vector> cas_to_merge_stream;
    hls::stream<process_data_vector> merge_to_compact_stream;
    hls::stream<bool> ctrl_f2c;
    hls::stream<bool> ctrl_c2m;
    hls::stream<bool> ctrl_m2cp;
    hls::stream<bool> ctrl_cp2store;
    hls::stream<data_t> compact_out[BATCH_SIZE];
    hls::stream<int> out_len_stream;

#pragma HLS STREAM variable=min_to_cas_stream depth=2
#pragma HLS STREAM variable=cas_to_merge_stream depth=2
#pragma HLS STREAM variable=merge_to_compact_stream depth=BATCH_SIZE
#pragma HLS STREAM variable=ctrl_f2c depth=2
#pragma HLS STREAM variable=ctrl_c2m depth=2
#pragma HLS STREAM variable=ctrl_m2cp depth=2
#pragma HLS STREAM variable=ctrl_cp2store depth=2
#pragma HLS STREAM variable=compact_out depth=2
#pragma HLS STREAM variable=out_len_stream depth=2

#pragma HLS DATAFLOW
    Feeder_FIFO_MIN_stage(local_a, len_a, local_b, len_b, upper_bound, min_to_cas_stream, ctrl_f2c);
    CAS_stage_stream_fifo(min_to_cas_stream, cas_to_merge_stream, ctrl_f2c, ctrl_c2m);
    Merge_stage_stream_fifo(cas_to_merge_stream, merge_to_compact_stream, ctrl_c2m, ctrl_m2cp);
    Compact_stage_stream_fifo(merge_to_compact_stream, compact_out, ctrl_m2cp, ctrl_cp2store);
    plan5_store_intersection_blocks_fifo(compact_out, ctrl_cp2store, out_blocks, out_len_stream);
    plan5_read_out_len_stage(out_len_stream, out_len);
}