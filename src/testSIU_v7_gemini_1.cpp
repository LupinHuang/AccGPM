//对v9重新探索，改进任务分发DATAFLOW和预取
//v14是v13的修改，把任务分发负载均衡稍微改了一下，用周期换slack，v13做了prefetch和round-robin的分离，并进行上板实测，
//taskv14对应的hw代号是v9，v9_2048是把头文件的max改成2048以后的
#include "merge_kernel_SIU_v8_gemini_1.h"
#include "kernel/task_executor_v16_origin7.hpp"
#include "kernel/siu_core_6.hpp"

extern "C" {

void rectangle_kernel_v5(
    const data_t*        gmem_bcsr_data_v1_0,
    const data_t*        gmem_bcsr_data_v2_0,
    const data_t*        gmem_bcsr_data_v1_1,
    const data_t*        gmem_bcsr_data_v2_1,
    const data_t*        gmem_bcsr_data_v1_2,
    const data_t*        gmem_bcsr_data_v2_2,
    const data_t*        gmem_bcsr_data_v1_3,
    const data_t*        gmem_bcsr_data_v2_3,
    const int*           gmem_row_ptr,
    const RectangleTask* task_queue,
    int                  num_tasks,
    long long*           out_result
) {
// 为每个 PE 提供 v1/v2 两条独立读通道，解耦邻接表双路读取
#pragma HLS INTERFACE m_axi port=gmem_bcsr_data_v1_0 bundle=gmem0 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_bcsr_data_v2_0 bundle=gmem1 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_bcsr_data_v1_1 bundle=gmem2 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_bcsr_data_v2_1 bundle=gmem3 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_bcsr_data_v1_2 bundle=gmem4 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_bcsr_data_v2_2 bundle=gmem5 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_bcsr_data_v1_3 bundle=gmem6 offset=slave depth=100000 latency=3
#pragma HLS INTERFACE m_axi port=gmem_bcsr_data_v2_3 bundle=gmem7 offset=slave depth=100000 latency=3

#pragma HLS INTERFACE m_axi port=gmem_row_ptr   bundle=gmem8 offset=slave depth=10000
#pragma HLS INTERFACE m_axi port=task_queue     bundle=gmem9 offset=slave depth=100000
#pragma HLS INTERFACE m_axi port=out_result     bundle=gmem10 offset=slave depth=1
#pragma HLS INTERFACE s_axilite port=num_tasks
#pragma HLS INTERFACE s_axilite port=return

 //   (void)gmem_row_ptr; 

    hls::stream<RectangleTask> prefetched_tasks;
    hls::stream<RectangleTask> task_streams[NUM_PE];
    hls::stream<long long> result_streams[NUM_PE];
#pragma HLS STREAM variable=prefetched_tasks depth=64
#pragma HLS STREAM variable=task_streams depth=16
#pragma HLS STREAM variable=result_streams depth=4

#pragma HLS DATAFLOW

    // 1. 任务预取 + 分发
    task_prefetcher(task_queue, num_tasks, prefetched_tasks);
    task_round_robin_with_eos(prefetched_tasks, num_tasks, task_streams);

    // 2. 并行 PE 执行
    // 注意：这里不能用循环了，因为每个 PE 接收的指针参数不同
    // 我们手动展开调用，确保每个 PE 独占一个内存通道
    pe_worker(gmem_bcsr_data_v1_0, gmem_bcsr_data_v2_0, task_streams[0], result_streams[0]);
    pe_worker(gmem_bcsr_data_v1_1, gmem_bcsr_data_v2_1, task_streams[1], result_streams[1]);
    pe_worker(gmem_bcsr_data_v1_2, gmem_bcsr_data_v2_2, task_streams[2], result_streams[2]);
    pe_worker(gmem_bcsr_data_v1_3, gmem_bcsr_data_v2_3, task_streams[3], result_streams[3]);

    // 3. 结果收集
    result_collector(result_streams, out_result);
}

}
