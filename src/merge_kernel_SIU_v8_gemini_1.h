#ifndef MERGE_KERNEL_SIU_V8_GEMINI_1_H
#define MERGE_KERNEL_SIU_V8_GEMINI_1_H
//v7头文件把32bit进行封装,v8把batch设为8， Num_of_process_log设为3
#include <ap_int.h>
#include <hls_stream.h>

// ============================================================================
// 复用原有的 BCSR 定义
// ============================================================================
const int BCSR_bitmap = 8;
const int BCSR_index = 24;
using bcsr_index = ap_uint<BCSR_index>;
using bcsr_bitmap = ap_uint<BCSR_bitmap>;

struct data_t {
    static constexpr int TOTAL_BITS = BCSR_index + BCSR_bitmap;
    ap_uint<TOTAL_BITS> packed;

    static bcsr_index padding_index() {
        return bcsr_index(-1);
    }

    data_t() {}
    data_t(bcsr_index i,  bcsr_bitmap b) { set(i, b); }

    void set(bcsr_index i, bcsr_bitmap b) {
        set_index(i);
        set_bitmap(b);
    }

    bcsr_index get_index() const {
        return packed.range(BCSR_index - 1, 0);
    }

    void set_index(bcsr_index idx) {
        packed.range(BCSR_index - 1, 0) = idx;
    }

    bcsr_bitmap get_bitmap() const {
        return packed.range(BCSR_index + BCSR_bitmap - 1, BCSR_index);
    }

    void set_bitmap(bcsr_bitmap bm) {
        packed.range(BCSR_index + BCSR_bitmap - 1, BCSR_index) = bm;
    }

    bool isPadding() const {
        return get_index() == padding_index();
    }

    static data_t padding_element() {
        return data_t(padding_index(), 0);
    }

    int popcount() const {
        bcsr_bitmap bm = get_bitmap();
        if (bm == 0) return 0;
        int count = 0;
        for (int i = 0; i < BCSR_bitmap; i++) {
            if (bm[i]) {
                count++;
            }
        }
        return count;
    }

    bool operator<=(const data_t& other) const {
        return get_index() <= other.get_index();
    }

    bool operator==(const data_t& other) const {
        return packed == other.packed;
    }
};

static_assert(sizeof(data_t) * 8 == data_t::TOTAL_BITS, "data_t must stay packed to 32 bits");

struct process_data_t {
    data_t bcsr_element;
    bool matched;
    bool from_where;

    process_data_t() : bcsr_element(data_t::padding_element()), matched(false), from_where(false) {}
};

const int BATCH_SIZE = 8;
const int Num_of_process_log = 3;
static_assert((1 << Num_of_process_log) == BATCH_SIZE,
              "BATCH_SIZE must equal 2^Num_of_process_log for the bitonic network");
static constexpr int MAX_BCSR_BLOCKS = 2048;

struct RectangleTask {
    int v0;
    int v1;
    int v2;
    int v1_start;
    int v1_len;
    int v2_start;
    int v2_len;
    int upper_bound;
};

struct process_data_vector {
    process_data_t lane[BATCH_SIZE];
    bool last = false;
};

// ============================================================================
// 新的顶层接口定义
// ============================================================================
extern "C" {
    /**
     * Rectangle Counting Kernel V3 (Coarse-grained + SIU Pipeline)
     * 
     * @param gmem_bcsr_data  存储所有顶点的邻接表数据（BCSR格式，紧凑存储）
     * @param gmem_row_ptr    CSR行指针：gmem_row_ptr[v] 指向 v 的邻接表在 gmem_bcsr_data 中的起始偏移
     * @param num_vertices    顶点数量
     * @param out_result      输出结果
     */
    // 新接口：每个 PE 拥有 v1/v2 两条独立的 m_axi 通道，便于并行读取两个邻接表
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
    );
}

#endif // MERGE_KERNEL_SIU_V3_H
