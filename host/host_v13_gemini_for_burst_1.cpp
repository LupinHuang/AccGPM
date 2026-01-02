/**
 * Rectangle Pattern Matching Host Code for FPGA Acceleration (Coarse-grained + SIU)
 * Target: Alveo U55C with HBM
 */
//host_v5是正确的rectangle计数剪枝方式，即v3<v0，测试用v5，v6的方式是错误的，因为v6用了v3<v2,host_v11用于优化带宽和burst读取,其他和v9一样
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include <array>
#include <cstdint>
#include <limits>

// XRT头文件
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"
#include "xrt/xrt_bo.h"
#include "xrt/xrt_uuid.h"

// 复用 Kernel 头文件中的定义
#include "merge_kernel_SIU_v8_gemini_1.h"

// ============================================================================
// 图数据结构
// ============================================================================
struct Graph {
    int num_vertices;
    int num_edges;
    std::vector<int> row_ptr;
    std::vector<int> col_idx;
    
    Graph() : num_vertices(0), num_edges(0) {}
};

// Residue 位图配置：若两个顶点的位图按位与为零，则它们不可能有共同邻居
constexpr int RESIDUE_BITS = 8192;
constexpr int RESIDUE_WORDS = RESIDUE_BITS / 64;
using ResidueMask = std::array<uint64_t, RESIDUE_WORDS>;
constexpr size_t MAX_TASKS_PER_BATCH = 8'000'000ULL; // ~256MB of RectangleTask data

// ============================================================================
// BCSR 转换辅助函数
// ============================================================================
void convert_to_bcsr(const std::vector<int>& neighbors, 
                     std::vector<data_t>& bcsr_output,
                     int bitmap_width = BCSR_bitmap) 
{
    bcsr_output.clear(); // Ensure output is clean
    if (neighbors.empty()) return;
    
    // Defensive copy and sort to ensure correct BCSR grouping
    // (Even if input is expected to be sorted, this is cheap safety)
    std::vector<int> sorted_neighbors = neighbors;
    std::sort(sorted_neighbors.begin(), sorted_neighbors.end());
    
    int i = 0;
    while (i < sorted_neighbors.size()) {
        int base_idx = sorted_neighbors[i] / bitmap_width;
        bcsr_bitmap bitmap = 0;
        
        while (i < sorted_neighbors.size() && sorted_neighbors[i] / bitmap_width == base_idx) {
            int bit_pos = sorted_neighbors[i] % bitmap_width;
            bitmap[bit_pos] = 1;
            i++;
        }
        bcsr_output.push_back(data_t(base_idx, bitmap));
    }
}
// ============================================================================
static void decode_bcsr_neighbors(
    const std::vector<data_t> &bcsr_data,
    int start,
    int end,
    int upper_bound,               // <0 表示无限制；>=0 表示 v < upper_bound
    std::vector<int> &neighbors
) {
    neighbors.clear();

    for (int i = start; i < end; ++i) {
        const data_t &blk = bcsr_data[i];

        // 理论上 BCSR 邻接表里不会有 padding，但加一层保护无妨
        if (blk.isPadding())
            continue;

        bcsr_index idx = blk.get_index();
        bcsr_bitmap bm = blk.get_bitmap();

        if (bm == 0)
            continue;

        int base = static_cast<int>(idx) * BCSR_bitmap;

        // 如果有上界，且这一块的起始值已经 >= upper_bound，
        // 那么后面的所有块也都 >= upper_bound，可以直接结束
        if (upper_bound >= 0 && base >= upper_bound)
            break;

        // 在一个块内按 bit 顺序解码
        for (int bit = 0; bit < BCSR_bitmap; ++bit) {
            if (bm[bit]) {
                int v = base + bit;
                if (upper_bound >= 0 && v >= upper_bound) {
                    // 块内的后续 bit 对应的值更大，也可以停
                    break;
                }
                neighbors.push_back(v);
            }
        }
    }
}

// ============================================================================
// ============================================================================
// 图加载与预处理
// ============================================================================
bool load_and_process_graph(const std::string& filename, Graph& g, 
                            std::vector<data_t>& gmem_bcsr_data, 
                            std::vector<int>& gmem_row_ptr) 
{
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        std::cerr << "ERROR: 无法打开文件 " << filename << std::endl;
        return false;
    }
    
    infile >> g.num_vertices >> g.num_edges;
    
    g.row_ptr.resize(g.num_vertices + 1);
    for (int i = 0; i <= g.num_vertices; i++) infile >> g.row_ptr[i];
    
    int total_edges = g.row_ptr[g.num_vertices];
    g.col_idx.resize(total_edges);
    for (int i = 0; i < total_edges; i++) infile >> g.col_idx[i];
    
    infile.close();
    
    // 转换为 BCSR 格式并展平到一维数组
    gmem_row_ptr.resize(g.num_vertices + 1);
    gmem_bcsr_data.clear();
    
    gmem_row_ptr[0] = 0;
    for (int v = 0; v < g.num_vertices; v++) {
        std::vector<int> neighbors;
        for (int k = g.row_ptr[v]; k < g.row_ptr[v+1]; k++) {
            neighbors.push_back(g.col_idx[k]);
        }
        
        // 关键修复：对邻居进行排序
        // FPGA Kernel 和 Host Baseline 的优化逻辑都假设邻接表是有序的
        // 如果数据未排序，"break" 优化会导致漏算
        // std::sort(neighbors.begin(), neighbors.end()); // Moved inside convert_to_bcsr

        // 转换为 BCSR
        std::vector<data_t> bcsr_vec;
        convert_to_bcsr(neighbors, bcsr_vec, BCSR_bitmap);
        
        // 追加到全局数据
        gmem_bcsr_data.insert(gmem_bcsr_data.end(), bcsr_vec.begin(), bcsr_vec.end());
        
        // 更新行指针
        gmem_row_ptr[v+1] = gmem_bcsr_data.size();
    }
    
    return true;
}

static std::vector<ResidueMask> build_residue_masks(const Graph& g) {
    std::vector<ResidueMask> masks(g.num_vertices);
    for (int v = 0; v < g.num_vertices; ++v) {
        masks[v].fill(0ULL);
        for (int idx = g.row_ptr[v]; idx < g.row_ptr[v + 1]; ++idx) {
            int u = g.col_idx[idx];
            if (u < 0) {
                continue;
            }
            int residue = u % RESIDUE_BITS;
            if (residue < 0) {
                residue += RESIDUE_BITS;
            }
            int word = residue / 64;
            int bit = residue % 64;
            masks[v][word] |= (1ULL << bit);
        }
    }
    return masks;
}

// ============================================================================
// CPU Baseline (Wedge-based, strictly matching Kernel logic)
// ============================================================================
uint64_t rectangle_cpu(const Graph& g) {
    uint64_t total_count = 0;
    
    // 模拟 Kernel 的 Wedge-based 逻辑
    // 遍历 v0
    for (int v0 = 0; v0 < g.num_vertices; v0++) {
        std::vector<int> n_v0;
        for (int i = g.row_ptr[v0]; i < g.row_ptr[v0+1]; i++) {
            n_v0.push_back(g.col_idx[i]);
        }
        
        // 遍历 v1, v2 属于 N(v0)，且 v2 < v1 < v0
        // 注意：Kernel 中的逻辑是 v1 < v0, v2 < v1
        for (size_t i = 0; i < n_v0.size(); i++) {
            int v1 = n_v0[i];
            if (v1 >= v0) continue;
            
            for (size_t j = 0; j < n_v0.size(); j++) {
                int v2 = n_v0[j];
                if (v2 >= v1) continue; // Ensure v2 < v1
                
                // 现在有了 Wedge: v1 - v0 - v2
                // 计算 N(v1) 和 N(v2) 的交集
                // 约束：只统计 v3 < v2 的共同邻居
                int intersection_size = 0;
                
                int p1 = g.row_ptr[v1], end1 = g.row_ptr[v1+1];
                int p2 = g.row_ptr[v2], end2 = g.row_ptr[v2+1];
                
                while (p1 < end1 && p2 < end2) {
                    int u1 = g.col_idx[p1];
                    int u2 = g.col_idx[p2];
                    
                    // 优化：因为邻接表是有序的，如果遇到 >= v0 的邻居，
                    // 后面的肯定也都 >= v0，所以可以直接停止
                    if (u1 >= v0 || u2 >= v0) break;
                    
                    if (u1 == u2) {
                        intersection_size++;
                        p1++; p2++;
                    } else if (u1 < u2) {
                        p1++;
                    } else {
                        p2++;
                    }
                }
                
                total_count += intersection_size;
            }
        }
    }
    // 不需要除以 2，因为 v3 < v0 的约束保证了每个矩形只被统计一次
    return total_count;
}
// ============================================================================
uint64_t rectangle_bcsr_cpu(
    const std::vector<data_t> &bcsr_data,
    const std::vector<int> &bcsr_row_ptr,
    int num_vertices
) {
    uint64_t total_count = 0;

    std::vector<int> n_v0;
    std::vector<int> n_v1;
    std::vector<int> n_v2;

    // 遍历 v0
    for (int v0 = 0; v0 < num_vertices; v0++) {
        int start0 = bcsr_row_ptr[v0];
        int end0   = bcsr_row_ptr[v0 + 1];

        // 解码 N(v0)，这里不加上界（upper_bound = -1）
        decode_bcsr_neighbors(bcsr_data, start0, end0, -1, n_v0);
        if (n_v0.empty())
            continue;

        // 遍历 v1, v2 属于 N(v0)，且 v2 < v1 < v0
        for (size_t i = 0; i < n_v0.size(); i++) {
            int v1 = n_v0[i];
            if (v1 >= v0)
                continue;

            for (size_t j = 0; j < n_v0.size(); j++) {
                int v2 = n_v0[j];
                if (v2 >= v1)
                    continue;      // 保证 v2 < v1

                // 现在有 wedge: v1 - v0 - v2
                // 计算 N(v1) 和 N(v2) 的交集，约束 v3 < v0

                int start1 = bcsr_row_ptr[v1];
                int end1   = bcsr_row_ptr[v1 + 1];
                int start2 = bcsr_row_ptr[v2];
                int end2   = bcsr_row_ptr[v2 + 1];

                // 解码时直接加上界 upper_bound = v0
                decode_bcsr_neighbors(bcsr_data, start1, end1, v0, n_v1);
                decode_bcsr_neighbors(bcsr_data, start2, end2, v0, n_v2);

                // 现在 n_v1、n_v2 都是 < v0 的升序列表
                size_t p1 = 0, p2 = 0;
                int intersection_size = 0;

                while (p1 < n_v1.size() && p2 < n_v2.size()) {
                    int u1 = n_v1[p1];
                    int u2 = n_v2[p2];

                    if (u1 == u2) {
                        intersection_size++;
                        p1++; p2++;
                    } else if (u1 < u2) {
                        p1++;
                    } else {
                        p2++;
                    }
                }

                total_count += intersection_size;
            }
        }
    }

    // 不需要除以 2，同样是 v3 < v2 只数一次
    return total_count;
}

// ============================================================================
// Triangle CPU Baseline (BCSR, matches plan5 ordering)
// Pattern summary reference:
// for v0:
//   y0f0 = N(v0) bounded by v0
//   for v1 in y0f0:
//     count += | y0f0 ∩ N(v1) bounded by v1 |
// Ordering: v1 < v0, v2 < v1
// ============================================================================
static uint64_t triangle_bcsr_cpu(
    const std::vector<data_t> &bcsr_data,
    const std::vector<int> &bcsr_row_ptr,
    int num_vertices
) {
    uint64_t total = 0;
    std::vector<int> y0f0;
    std::vector<int> y1f1;

    for (int v0 = 0; v0 < num_vertices; ++v0) {
        const int start0 = bcsr_row_ptr[v0];
        const int end0 = bcsr_row_ptr[v0 + 1];
        // y0f0 = neighbors of v0 with v < v0
        decode_bcsr_neighbors(bcsr_data, start0, end0, v0, y0f0);
        if (y0f0.empty()) continue;

        for (int v1 : y0f0) {
            // N(v1) bounded by v1
            const int start1 = bcsr_row_ptr[v1];
            const int end1 = bcsr_row_ptr[v1 + 1];
            decode_bcsr_neighbors(bcsr_data, start1, end1, v1, y1f1);
            if (y1f1.empty()) continue;

            size_t p0 = 0;
            size_t p1 = 0;
            while (p0 < y0f0.size() && p1 < y1f1.size()) {
                const int a = y0f0[p0];
                const int b = y1f1[p1];
                if (a == b) {
                    total++;
                    p0++;
                    p1++;
                } else if (a < b) {
                    p0++;
                } else {
                    p1++;
                }
            }
        }
    }

    return total;
}
// ============================================================================ 

std::vector<RectangleTask> generate_rectangle_tasks(
    const Graph& g,
    const std::vector<int>& bcsr_row_ptr,
    const std::vector<ResidueMask>& residue_masks
) {
    std::vector<RectangleTask> tasks;
    std::vector<int> n_v0;
    n_v0.reserve(256);

    for (int v0 = 0; v0 < g.num_vertices; ++v0) {
        n_v0.clear();
        for (int idx = g.row_ptr[v0]; idx < g.row_ptr[v0 + 1]; ++idx) {
            n_v0.push_back(g.col_idx[idx]);
        }
        if (n_v0.size() < 2) continue;

        std::sort(n_v0.begin(), n_v0.end());

        std::vector<ResidueMask> local_masks(n_v0.size());
        std::vector<uint8_t> mask_ready(n_v0.size(), 0);
        std::vector<uint8_t> has_lower_neighbor(n_v0.size(), 0);

        auto ensure_mask = [&](size_t idx) -> bool {
            if (idx >= n_v0.size()) {
                return false;
            }
            if (!mask_ready[idx]) {
                mask_ready[idx] = 1;
                local_masks[idx].fill(0ULL);
                bool found = false;
                int vertex = n_v0[idx];
                for (int ptr = g.row_ptr[vertex]; ptr < g.row_ptr[vertex + 1]; ++ptr) {
                    int neighbor = g.col_idx[ptr];
                    if (neighbor >= v0 || neighbor < 0) {
                        continue;
                    }
                    found = true;
                    int residue = neighbor % RESIDUE_BITS;
                    if (residue < 0) {
                        residue += RESIDUE_BITS;
                    }
                    int word = residue / 64;
                    int bit = residue % 64;
                    local_masks[idx][word] |= (1ULL << bit);
                }
                has_lower_neighbor[idx] = found ? 1 : 0;
            }
            return has_lower_neighbor[idx] != 0;
        };

        for (size_t i = 0; i < n_v0.size(); ++i) {
            int v1 = n_v0[i];
            if (v1 >= v0) break;
            int deg_v1 = bcsr_row_ptr[v1 + 1] - bcsr_row_ptr[v1];
            if (deg_v1 == 0)
                continue;

            for (size_t j = 0; j < n_v0.size(); ++j) {
                int v2 = n_v0[j];
                if (v2 >= v1) break;
                int deg_v2 = bcsr_row_ptr[v2 + 1] - bcsr_row_ptr[v2];
                if (deg_v2 == 0)
                    continue;

                bool coarse_possible = false;
                for (int word = 0; word < RESIDUE_WORDS; ++word) {
                    if (residue_masks[v1][word] & residue_masks[v2][word]) {
                        coarse_possible = true;
                        break;
                    }
                }
                if (!coarse_possible)
                    continue;

                if (!ensure_mask(i) || !ensure_mask(j))
                    continue;

                bool possible_intersection = false;
                for (int word = 0; word < RESIDUE_WORDS; ++word) {
                    if (local_masks[i][word] & local_masks[j][word]) {
                        possible_intersection = true;
                        break;
                    }
                }
                if (!possible_intersection)
                    continue;

                RectangleTask task;
                task.v0 = v0;
                task.v1 = v1;
                task.v2 = v2;
                task.v1_start = bcsr_row_ptr[v1];
                task.v1_len = bcsr_row_ptr[v1 + 1] - bcsr_row_ptr[v1];
                task.v2_start = bcsr_row_ptr[v2];
                task.v2_len = bcsr_row_ptr[v2 + 1] - bcsr_row_ptr[v2];
                task.upper_bound = v0;
                tasks.push_back(task);
            }
        }
    }

    return tasks;
}


// ============================================================================
// Main
// ============================================================================
int main(int argc, char** argv) {
    std::cout << "================================================" << std::endl;
    std::cout << "  Plan5 Subgraph Matching (Triangle test)" << std::endl;
    std::cout << "================================================" << std::endl;
    
    if (argc < 3) {
        std::cerr << "用法: " << argv[0] << " <xclbin> <graph_file>" << std::endl;
        return EXIT_FAILURE;
    }
    
    std::string xclbin_path = argv[1];
    std::string graph_file = argv[2];
    
    Graph g;
    std::vector<data_t> gmem_bcsr_data;
    std::vector<int> gmem_row_ptr;
    
    std::cout << "INFO: Loading and processing graph..." << std::endl;
    if (!load_and_process_graph(graph_file, g, gmem_bcsr_data, gmem_row_ptr)) return EXIT_FAILURE;
    
    std::cout << "  Vertices: " << g.num_vertices << std::endl;
    std::cout << "  BCSR Data Size: " << gmem_bcsr_data.size() << " elements" << std::endl;

    // Use plan5 kernel path for functional correctness testing.
    const bool use_plan5_kernel = true;

    // Keep the old rectangle task path available for A/B experiments.
    const bool use_rectangle_task_kernel = false;

    std::vector<RectangleTask> rectangle_tasks;
    if (use_rectangle_task_kernel) {
        auto residue_masks = build_residue_masks(g);
        rectangle_tasks = generate_rectangle_tasks(g, gmem_row_ptr, residue_masks);
        std::cout << "  Rectangle Tasks: " << rectangle_tasks.size() << std::endl;
    } else {
        std::cout << "  Rectangle Tasks: (unused in this run)" << std::endl;
    }
    
    // CPU Baseline
    std::cout << "\n--- CPU Baseline ---" << std::endl;
    auto cpu_start = std::chrono::high_resolution_clock::now();
    uint64_t cpu_count = 0;
    if (use_plan5_kernel) {
        cpu_count = triangle_bcsr_cpu(gmem_bcsr_data, gmem_row_ptr, g.num_vertices);
    } else {
        cpu_count = rectangle_cpu(g);
    }
    auto cpu_end = std::chrono::high_resolution_clock::now();
    double cpu_time = std::chrono::duration<double, std::milli>(cpu_end - cpu_start).count();
    std::cout << "  Count: " << cpu_count << std::endl;
    std::cout << "  Time:  " << cpu_time << " ms" << std::endl;
    // CPU bcsr 版本验证（可选）
    //std::cout << "\n--- CPU Baseline (BCSR) ---" << std::endl;
    //auto bcsr_start = std::chrono::high_resolution_clock::now();
    //uint64_t bcsr_cpu_count = rectangle_bcsr_cpu(
    //gmem_bcsr_data,
    //gmem_row_ptr,
    //g.num_vertices);
    // FPGA Setup
    std::cout << "\n--- FPGA Acceleration ---" << std::endl;
    auto device = xrt::device(0);
    auto uuid = device.load_xclbin(xclbin_path);
    auto kernel = xrt::kernel(device, uuid,
                              use_plan5_kernel ? "subgraph_kernel_plan5_v1"
                              : (use_rectangle_task_kernel ? "rectangle_kernel_v5" : "rectangle_kernel_plan_v1"));

    // Allocate BOs.
    // For plan5 kernel, the top-level exposes per-PE per-level ports (L0~L4).
    // In the current RTL, v1/v2 of the same level share the same HLS bundle,
    // so they MUST map to the same memory bank. We therefore allocate one BO
    // per (PE,Level) bundle and bind both v1/v2 args to that same BO.
    constexpr int kPlan5NumPe = 4;
    constexpr int kPlan5Levels = QUERYPLAN_MAX_LEVELS; // 5

    std::array<xrt::bo, kPlan5NumPe * kPlan5Levels> bo_bcsr_plan5;
    std::array<xrt::bo, kPlan5NumPe> bo_row_plan5;
    xrt::bo bo_bcsr_v1_0;
    xrt::bo bo_bcsr_v2_0;
    xrt::bo bo_bcsr_v1_1;
    xrt::bo bo_bcsr_v2_1;
    xrt::bo bo_bcsr_v1_2;
    xrt::bo bo_bcsr_v2_2;
    xrt::bo bo_bcsr_v1_3;
    xrt::bo bo_bcsr_v2_3;
    xrt::bo bo_row;

    if (use_plan5_kernel) {
        // Arg order (plan5):
        //   PE0: 10 args (v1/v2 for L0~L4) => 0..9
        //   PE1: 10 args => 10..19
        //   PE2: 10 args => 20..29
        //   PE3: 10 args => 30..39
        //   40..43: gmem_row_ptr_pe0..pe3
        //   44: gmem_plan, 45: num_vertices, 46: out_result
        for (int pe = 0; pe < kPlan5NumPe; ++pe) {
            for (int lv = 0; lv < kPlan5Levels; ++lv) {
                const int arg_v1 = pe * 10 + lv * 2;
                bo_bcsr_plan5[pe * kPlan5Levels + lv] =
                    xrt::bo(device, gmem_bcsr_data.size() * sizeof(data_t), kernel.group_id(arg_v1));
            }
        }
        for (int pe = 0; pe < kPlan5NumPe; ++pe) {
            bo_row_plan5[pe] = xrt::bo(device, gmem_row_ptr.size() * sizeof(int), kernel.group_id(40 + pe));
        }
    } else {
        // Rectangle kernels keep the legacy 8 data ports + row_ptr.
        bo_bcsr_v1_0 = xrt::bo(device, gmem_bcsr_data.size() * sizeof(data_t), kernel.group_id(0));
        bo_bcsr_v2_0 = xrt::bo(device, gmem_bcsr_data.size() * sizeof(data_t), kernel.group_id(1));
        bo_bcsr_v1_1 = xrt::bo(device, gmem_bcsr_data.size() * sizeof(data_t), kernel.group_id(2));
        bo_bcsr_v2_1 = xrt::bo(device, gmem_bcsr_data.size() * sizeof(data_t), kernel.group_id(3));
        bo_bcsr_v1_2 = xrt::bo(device, gmem_bcsr_data.size() * sizeof(data_t), kernel.group_id(4));
        bo_bcsr_v2_2 = xrt::bo(device, gmem_bcsr_data.size() * sizeof(data_t), kernel.group_id(5));
        bo_bcsr_v1_3 = xrt::bo(device, gmem_bcsr_data.size() * sizeof(data_t), kernel.group_id(6));
        bo_bcsr_v2_3 = xrt::bo(device, gmem_bcsr_data.size() * sizeof(data_t), kernel.group_id(7));

        bo_row = xrt::bo(device, gmem_row_ptr.size() * sizeof(int), kernel.group_id(8));
    }

    // arg9:
    // - subgraph_kernel_plan5_v1: gmem_plan (QueryPlan5)
    // - rectangle_kernel_plan_v1: gmem_plan (QueryPlan)
    // - rectangle_kernel_v5: task_queue
    QueryPlan plan;
    QueryPlan5 plan5;
    if (use_plan5_kernel) {
        // Triangle plan:
        // Level0: EXPAND_NBR from v0 -> v1, require v1 < v0
        // Level1: INTERSECT_EMIT(N(v0), N(v1)) -> v2, require v2 < v1
        plan5.num_levels = 2;
        plan5.v0_begin = 0;
        plan5.v0_end = g.num_vertices;

        // Level 0
        plan5.levels[0].op = PLAN_OP_EXPAND_NBR;
        plan5.levels[0].dst_slot = 1;
        plan5.levels[0].src_slot0 = 0;
        plan5.levels[0].src_slot1 = 0;
        plan5.levels[0].bound_kind = PLAN_BOUND_VID_SLOT;
        plan5.levels[0].bound_slot = 0;
        plan5.levels[0].bound_const = 0;
        plan5.levels[0].flags = LEVELCFG_FLAG_REQUIRE_NEW_LT_BOUND;

        // Level 1
        plan5.levels[1].op = PLAN_OP_INTERSECT_EMIT;
        plan5.levels[1].dst_slot = 2;
        plan5.levels[1].src_slot0 = 0;
        plan5.levels[1].src_slot1 = 1;
        plan5.levels[1].bound_kind = PLAN_BOUND_VID_SLOT;
        plan5.levels[1].bound_slot = 1;
        plan5.levels[1].bound_const = 0;
        plan5.levels[1].flags = LEVELCFG_FLAG_REQUIRE_NEW_LT_BOUND;

        // Remaining levels are ignored because num_levels=2, but keep them deterministic.
        for (int i = 2; i < QUERYPLAN_MAX_LEVELS; ++i) {
            plan5.levels[i].op = PLAN_OP_NOOP;
            plan5.levels[i].dst_slot = 0;
            plan5.levels[i].src_slot0 = 0;
            plan5.levels[i].src_slot1 = 0;
            plan5.levels[i].bound_kind = PLAN_BOUND_NONE;
            plan5.levels[i].bound_slot = 0;
            plan5.levels[i].bound_const = 0;
            plan5.levels[i].flags = 0;
        }
    } else {
        plan.kind = PLAN_KIND_RECTANGLE_WEDGE_COUNT;
        plan.v0_begin = 0;
        plan.v0_end = g.num_vertices;
        plan.flags = QUERYPLAN_FLAG_ORDERING_V2_LT_V1_LT_V0 | QUERYPLAN_FLAG_UPPER_BOUND_IS_V0;
    }

        auto bo_plan_or_tasks = use_plan5_kernel
                        ? xrt::bo(device, sizeof(QueryPlan5), kernel.group_id(44))
                        : (!use_rectangle_task_kernel
                            ? xrt::bo(device, sizeof(QueryPlan), kernel.group_id(9))
                            : xrt::bo(device, std::max<size_t>(1, std::min<size_t>(rectangle_tasks.size(), MAX_TASKS_PER_BATCH)) * sizeof(RectangleTask),
                                kernel.group_id(9)));

    // out_result arg index differs between kernels:
    // - subgraph_kernel_plan5_v1: out_result is arg46
    // - rectangle_kernel_v5: out_result is arg11
    // - rectangle_kernel_plan_v1: out_result is arg11
    auto bo_res = xrt::bo(device, sizeof(long long), kernel.group_id(use_plan5_kernel ? 46 : 11));
    
    std::cout << "INFO: Transferring data..." << std::endl;
    if (use_plan5_kernel) {
        for (int pe = 0; pe < kPlan5NumPe; ++pe) {
            for (int lv = 0; lv < kPlan5Levels; ++lv) {
                bo_bcsr_plan5[pe * kPlan5Levels + lv].write(gmem_bcsr_data.data());
            }
        }
    } else {
        // 复制数据到所有通道（v1/v2 均为同一份图数据的副本）
        bo_bcsr_v1_0.write(gmem_bcsr_data.data());
        bo_bcsr_v2_0.write(gmem_bcsr_data.data());
        bo_bcsr_v1_1.write(gmem_bcsr_data.data());
        bo_bcsr_v2_1.write(gmem_bcsr_data.data());
        bo_bcsr_v1_2.write(gmem_bcsr_data.data());
        bo_bcsr_v2_2.write(gmem_bcsr_data.data());
        bo_bcsr_v1_3.write(gmem_bcsr_data.data());
        bo_bcsr_v2_3.write(gmem_bcsr_data.data());
    }

    if (use_plan5_kernel) {
        for (int pe = 0; pe < kPlan5NumPe; ++pe) {
            bo_row_plan5[pe].write(gmem_row_ptr.data());
        }
    } else {
        bo_row.write(gmem_row_ptr.data());
    }

    if (use_plan5_kernel) {
        bo_plan_or_tasks.write(&plan5, sizeof(QueryPlan5), 0);
        bo_plan_or_tasks.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    } else if (!use_rectangle_task_kernel) {
        bo_plan_or_tasks.write(&plan, sizeof(QueryPlan), 0);
        bo_plan_or_tasks.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    }

    if (use_plan5_kernel) {
        for (int pe = 0; pe < kPlan5NumPe; ++pe) {
            for (int lv = 0; lv < kPlan5Levels; ++lv) {
                bo_bcsr_plan5[pe * kPlan5Levels + lv].sync(XCL_BO_SYNC_BO_TO_DEVICE);
            }
        }
    } else {
        bo_bcsr_v1_0.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_bcsr_v2_0.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_bcsr_v1_1.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_bcsr_v2_1.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_bcsr_v1_2.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_bcsr_v2_2.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_bcsr_v1_3.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        bo_bcsr_v2_3.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    }
    if (use_plan5_kernel) {
        for (int pe = 0; pe < kPlan5NumPe; ++pe) {
            bo_row_plan5[pe].sync(XCL_BO_SYNC_BO_TO_DEVICE);
        }
    } else {
        bo_row.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    }

    std::cout << "INFO: Starting Kernel..." << std::endl;
    uint64_t fpga_count = 0;
    double kernel_time_ms = 0.0;

    if (use_plan5_kernel || !use_rectangle_task_kernel) {
        long long zero = 0;
        bo_res.write(&zero);
        bo_res.sync(XCL_BO_SYNC_BO_TO_DEVICE);

        auto run = xrt::run(kernel);
        if (use_plan5_kernel) {
            // Bind per-(PE,Level) BO to both v1/v2 args of that level.
            for (int pe = 0; pe < kPlan5NumPe; ++pe) {
                for (int lv = 0; lv < kPlan5Levels; ++lv) {
                    const int arg_v1 = pe * 10 + lv * 2;
                    const int arg_v2 = arg_v1 + 1;
                    const auto& bo = bo_bcsr_plan5[pe * kPlan5Levels + lv];
                    run.set_arg(arg_v1, bo);
                    run.set_arg(arg_v2, bo);
                }
            }

            // Common
            run.set_arg(40, bo_row_plan5[0]);
            run.set_arg(41, bo_row_plan5[1]);
            run.set_arg(42, bo_row_plan5[2]);
            run.set_arg(43, bo_row_plan5[3]);
            run.set_arg(44, bo_plan_or_tasks);
            run.set_arg(45, g.num_vertices);
            run.set_arg(46, bo_res);
        } else {
            run.set_arg(0, bo_bcsr_v1_0);
            run.set_arg(1, bo_bcsr_v2_0);
            run.set_arg(2, bo_bcsr_v1_1);
            run.set_arg(3, bo_bcsr_v2_1);
            run.set_arg(4, bo_bcsr_v1_2);
            run.set_arg(5, bo_bcsr_v2_2);
            run.set_arg(6, bo_bcsr_v1_3);
            run.set_arg(7, bo_bcsr_v2_3);
            run.set_arg(8, bo_row);
            run.set_arg(9, bo_plan_or_tasks);
            run.set_arg(10, g.num_vertices);
            run.set_arg(11, bo_res);
        }

        auto kernel_start = std::chrono::high_resolution_clock::now();
        run.start();
        run.wait();
        auto kernel_end = std::chrono::high_resolution_clock::now();
        kernel_time_ms += std::chrono::duration<double, std::milli>(kernel_end - kernel_start).count();

        long long raw_res = 0;
        bo_res.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
        bo_res.read(&raw_res);
        fpga_count = raw_res;
    } else {
        size_t task_count = rectangle_tasks.size();
        size_t processed_tasks = 0;
        size_t batch_id = 0;
        const size_t kernel_task_limit = static_cast<size_t>(std::numeric_limits<int>::max());

        while (processed_tasks < task_count || (task_count == 0 && batch_id == 0)) {
            size_t remaining = (task_count > processed_tasks) ? (task_count - processed_tasks) : 0;
            size_t current_batch = (task_count == 0) ? 0 : std::min<size_t>(MAX_TASKS_PER_BATCH, remaining);

            if (task_count == 0 && batch_id == 0) {
                RectangleTask dummy{};
                bo_plan_or_tasks.write(&dummy, sizeof(RectangleTask), 0);
                bo_plan_or_tasks.sync(XCL_BO_SYNC_BO_TO_DEVICE);
            } else if (current_batch > 0) {
                bo_plan_or_tasks.write(rectangle_tasks.data() + processed_tasks,
                                       current_batch * sizeof(RectangleTask), 0);
                bo_plan_or_tasks.sync(XCL_BO_SYNC_BO_TO_DEVICE);
            }

            long long zero = 0;
            bo_res.write(&zero);
            bo_res.sync(XCL_BO_SYNC_BO_TO_DEVICE);

            auto run = xrt::run(kernel);
            run.set_arg(0, bo_bcsr_v1_0);
            run.set_arg(1, bo_bcsr_v2_0);
            run.set_arg(2, bo_bcsr_v1_1);
            run.set_arg(3, bo_bcsr_v2_1);
            run.set_arg(4, bo_bcsr_v1_2);
            run.set_arg(5, bo_bcsr_v2_2);
            run.set_arg(6, bo_bcsr_v1_3);
            run.set_arg(7, bo_bcsr_v2_3);
            run.set_arg(8, bo_row);
            run.set_arg(9, bo_plan_or_tasks);
            if (current_batch > kernel_task_limit) {
                std::cerr << "ERROR: current batch size exceeds kernel argument width" << std::endl;
                return EXIT_FAILURE;
            }
            run.set_arg(10, static_cast<int>(current_batch));
            run.set_arg(11, bo_res);

            std::cout << "  [Batch " << batch_id << "] Tasks: " << current_batch << std::endl;
            auto kernel_start = std::chrono::high_resolution_clock::now();
            run.start();
            run.wait();
            auto kernel_end = std::chrono::high_resolution_clock::now();
            kernel_time_ms += std::chrono::duration<double, std::milli>(kernel_end - kernel_start).count();

            long long raw_res = 0;
            bo_res.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
            bo_res.read(&raw_res);
            fpga_count += raw_res;

            processed_tasks += current_batch;
            ++batch_id;

            if (task_count == 0) {
                break;
            }
        }
    }

    std::cout << "  Count: " << fpga_count << std::endl;
    std::cout << "  Kernel Time:  " << kernel_time_ms << " ms" << std::endl;
    

    // 对比三者
std::cout << "\n=== Summary ===" << std::endl;
std::cout << "CPU       : " << cpu_count      << std::endl;
//std::cout << "BCSR-CPU  : " << bcsr_cpu_count << std::endl;
std::cout << "FPGA      : " << fpga_count     << std::endl;
    if (cpu_count == fpga_count) {
        double speedup = kernel_time_ms > 0 ? cpu_time / kernel_time_ms : 0.0;
        std::cout << "\nPASS! Speedup (CPU/KERNEL): " << speedup << "x" << std::endl;
    } else {
        std::cout << "\nFAIL! Diff: " << (int64_t)(fpga_count - cpu_count) << std::endl;
    }
    
    return 0;
}
