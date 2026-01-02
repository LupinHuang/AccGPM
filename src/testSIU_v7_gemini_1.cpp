//对v9重新探索，改进任务分发DATAFLOW和预取
//v14是v13的修改，把任务分发负载均衡稍微改了一下，用周期换slack，v13做了prefetch和round-robin的分离，并进行上板实测，
//taskv14对应的hw代号是v9，v9_2048是把头文件的max改成2048以后的
#include "merge_kernel_SIU_v8_gemini_1.h"
#include "kernel/task_executor_v16_origin8.hpp"
#include "kernel/siu_core_6.hpp"

extern "C" {
    
void subgraph_kernel_plan5_v1(
    // PE0: per-level dedicated ports (L0~L4)
    const data_t*        gmem_pe0_v1_L0,
    const data_t*        gmem_pe0_v2_L0,
    const data_t*        gmem_pe0_v1_L1,
    const data_t*        gmem_pe0_v2_L1,
    const data_t*        gmem_pe0_v1_L2,
    const data_t*        gmem_pe0_v2_L2,
    const data_t*        gmem_pe0_v1_L3,
    const data_t*        gmem_pe0_v2_L3,
    const data_t*        gmem_pe0_v1_L4,
    const data_t*        gmem_pe0_v2_L4,

    // PE1
    const data_t*        gmem_pe1_v1_L0,
    const data_t*        gmem_pe1_v2_L0,
    const data_t*        gmem_pe1_v1_L1,
    const data_t*        gmem_pe1_v2_L1,
    const data_t*        gmem_pe1_v1_L2,
    const data_t*        gmem_pe1_v2_L2,
    const data_t*        gmem_pe1_v1_L3,
    const data_t*        gmem_pe1_v2_L3,
    const data_t*        gmem_pe1_v1_L4,
    const data_t*        gmem_pe1_v2_L4,

    // PE2
    const data_t*        gmem_pe2_v1_L0,
    const data_t*        gmem_pe2_v2_L0,
    const data_t*        gmem_pe2_v1_L1,
    const data_t*        gmem_pe2_v2_L1,
    const data_t*        gmem_pe2_v1_L2,
    const data_t*        gmem_pe2_v2_L2,
    const data_t*        gmem_pe2_v1_L3,
    const data_t*        gmem_pe2_v2_L3,
    const data_t*        gmem_pe2_v1_L4,
    const data_t*        gmem_pe2_v2_L4,

    // PE3
    const data_t*        gmem_pe3_v1_L0,
    const data_t*        gmem_pe3_v2_L0,
    const data_t*        gmem_pe3_v1_L1,
    const data_t*        gmem_pe3_v2_L1,
    const data_t*        gmem_pe3_v1_L2,
    const data_t*        gmem_pe3_v2_L2,
    const data_t*        gmem_pe3_v1_L3,
    const data_t*        gmem_pe3_v2_L3,
    const data_t*        gmem_pe3_v1_L4,
    const data_t*        gmem_pe3_v2_L4,
    const int*           gmem_row_ptr_pe0,
    const int*           gmem_row_ptr_pe1,
    const int*           gmem_row_ptr_pe2,
    const int*           gmem_row_ptr_pe3,
    const QueryPlan5*    gmem_plan,
    int                  num_vertices,
    long long*           out_result
) {
// PE0 bundles: gmem_pe0_L0..L4
#pragma HLS INTERFACE m_axi port=gmem_pe0_v1_L0 bundle=gmem_pe0_L0 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe0_v2_L0 bundle=gmem_pe0_L0 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe0_v1_L1 bundle=gmem_pe0_L1 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe0_v2_L1 bundle=gmem_pe0_L1 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe0_v1_L2 bundle=gmem_pe0_L2 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe0_v2_L2 bundle=gmem_pe0_L2 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe0_v1_L3 bundle=gmem_pe0_L3 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe0_v2_L3 bundle=gmem_pe0_L3 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe0_v1_L4 bundle=gmem_pe0_L4 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe0_v2_L4 bundle=gmem_pe0_L4 offset=slave depth=100000 latency=3

// PE1 bundles: gmem_pe1_L0..L4
#pragma HLS INTERFACE m_axi port=gmem_pe1_v1_L0 bundle=gmem_pe1_L0 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe1_v2_L0 bundle=gmem_pe1_L0 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe1_v1_L1 bundle=gmem_pe1_L1 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe1_v2_L1 bundle=gmem_pe1_L1 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe1_v1_L2 bundle=gmem_pe1_L2 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe1_v2_L2 bundle=gmem_pe1_L2 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe1_v1_L3 bundle=gmem_pe1_L3 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe1_v2_L3 bundle=gmem_pe1_L3 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe1_v1_L4 bundle=gmem_pe1_L4 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe1_v2_L4 bundle=gmem_pe1_L4 offset=slave depth=100000 latency=3

// PE2 bundles: gmem_pe2_L0..L4
#pragma HLS INTERFACE m_axi port=gmem_pe2_v1_L0 bundle=gmem_pe2_L0 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe2_v2_L0 bundle=gmem_pe2_L0 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe2_v1_L1 bundle=gmem_pe2_L1 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe2_v2_L1 bundle=gmem_pe2_L1 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe2_v1_L2 bundle=gmem_pe2_L2 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe2_v2_L2 bundle=gmem_pe2_L2 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe2_v1_L3 bundle=gmem_pe2_L3 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe2_v2_L3 bundle=gmem_pe2_L3 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe2_v1_L4 bundle=gmem_pe2_L4 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe2_v2_L4 bundle=gmem_pe2_L4 offset=slave depth=100000 latency=3

// PE3 bundles: gmem_pe3_L0..L4
#pragma HLS INTERFACE m_axi port=gmem_pe3_v1_L0 bundle=gmem_pe3_L0 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe3_v2_L0 bundle=gmem_pe3_L0 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe3_v1_L1 bundle=gmem_pe3_L1 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe3_v2_L1 bundle=gmem_pe3_L1 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe3_v1_L2 bundle=gmem_pe3_L2 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe3_v2_L2 bundle=gmem_pe3_L2 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe3_v1_L3 bundle=gmem_pe3_L3 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe3_v2_L3 bundle=gmem_pe3_L3 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe3_v1_L4 bundle=gmem_pe3_L4 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_pe3_v2_L4 bundle=gmem_pe3_L4 offset=slave depth=100000 latency=3

#pragma HLS INTERFACE m_axi port=gmem_row_ptr_pe0 bundle=gmem8  offset=slave depth=10000
#pragma HLS INTERFACE m_axi port=gmem_row_ptr_pe1 bundle=gmem11 offset=slave depth=10000
#pragma HLS INTERFACE m_axi port=gmem_row_ptr_pe2 bundle=gmem12 offset=slave depth=10000
#pragma HLS INTERFACE m_axi port=gmem_row_ptr_pe3 bundle=gmem13 offset=slave depth=10000
#pragma HLS INTERFACE m_axi port=gmem_plan    bundle=gmem9 offset=slave depth=1
#pragma HLS INTERFACE m_axi port=out_result   bundle=gmem10 offset=slave depth=1

#pragma HLS INTERFACE s_axilite port=num_vertices
#pragma HLS INTERFACE s_axilite port=return

    const QueryPlan5 plan = gmem_plan[0];

    hls::stream<Embedding5> level0_streams[NUM_PE];
    hls::stream<long long> result_streams[NUM_PE];
#pragma HLS STREAM variable=level0_streams depth=64
#pragma HLS STREAM variable=result_streams depth=4

#pragma HLS DATAFLOW
    plan5_scan_all_stage(plan, num_vertices, level0_streams);

    // One plan-driven pipeline per PE, each bound to its own v1/v2 memory channels.
    plan5_pipeline_pe(
        plan,
        gmem_pe0_v1_L0, gmem_pe0_v2_L0,
        gmem_pe0_v1_L1, gmem_pe0_v2_L1,
        gmem_pe0_v1_L2, gmem_pe0_v2_L2,
        gmem_pe0_v1_L3, gmem_pe0_v2_L3,
        gmem_pe0_v1_L4, gmem_pe0_v2_L4,
        gmem_row_ptr_pe0,
        num_vertices,
        level0_streams[0],
        result_streams[0]
    );

    plan5_pipeline_pe(
        plan,
        gmem_pe1_v1_L0, gmem_pe1_v2_L0,
        gmem_pe1_v1_L1, gmem_pe1_v2_L1,
        gmem_pe1_v1_L2, gmem_pe1_v2_L2,
        gmem_pe1_v1_L3, gmem_pe1_v2_L3,
        gmem_pe1_v1_L4, gmem_pe1_v2_L4,
        gmem_row_ptr_pe1,
        num_vertices,
        level0_streams[1],
        result_streams[1]
    );

    plan5_pipeline_pe(
        plan,
        gmem_pe2_v1_L0, gmem_pe2_v2_L0,
        gmem_pe2_v1_L1, gmem_pe2_v2_L1,
        gmem_pe2_v1_L2, gmem_pe2_v2_L2,
        gmem_pe2_v1_L3, gmem_pe2_v2_L3,
        gmem_pe2_v1_L4, gmem_pe2_v2_L4,
        gmem_row_ptr_pe2,
        num_vertices,
        level0_streams[2],
        result_streams[2]
    );

    plan5_pipeline_pe(
        plan,
        gmem_pe3_v1_L0, gmem_pe3_v2_L0,
        gmem_pe3_v1_L1, gmem_pe3_v2_L1,
        gmem_pe3_v1_L2, gmem_pe3_v2_L2,
        gmem_pe3_v1_L3, gmem_pe3_v2_L3,
        gmem_pe3_v1_L4, gmem_pe3_v2_L4,
        gmem_row_ptr_pe3,
        num_vertices,
        level0_streams[3],
        result_streams[3]
    );

    result_collector(result_streams, out_result);
}

}
