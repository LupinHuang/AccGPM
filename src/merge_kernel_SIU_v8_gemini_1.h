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

// ============================================================================
// Step1-A: A minimal generic task form (coexists with RectangleTask)
// - First supported op: INTERSECT_COUNT(v1, v2, upper_bound)
// - EOS is encoded as op==EXEC_OP_EOS
// ============================================================================
enum ExecutionOp : int {
    EXEC_OP_INTERSECT_COUNT = 0,
    EXEC_OP_EOS = -1,
};

struct ExecutionTask {
    int op;          // ExecutionOp
    int v0;          // optional context (kept for debugging/compat)
    int v1;          // operand A vertex id
    int v2;          // operand B vertex id
    int upper_bound; // apply v < upper_bound in SIU

    static ExecutionTask eos() {
        ExecutionTask t;
        t.op = EXEC_OP_EOS;
        t.v0 = -1;
        t.v1 = -1;
        t.v2 = -1;
        t.upper_bound = -1;
        return t;
    }

    bool is_eos() const {
        return op == EXEC_OP_EOS;
    }
};

// ============================================================================
// Step1-B: Host -> Kernel QueryPlan (kernel interprets, no feedback loop yet)
// ============================================================================
enum QueryPlanKind : int {
    PLAN_KIND_RECTANGLE_WEDGE_COUNT = 0,
};

// Flags are designed to be composable as the plan grows.
static constexpr int QUERYPLAN_FLAG_ORDERING_V2_LT_V1_LT_V0 = 1 << 0;
static constexpr int QUERYPLAN_FLAG_UPPER_BOUND_IS_V0 = 1 << 1;

struct QueryPlan {
    int kind;      // QueryPlanKind
    int v0_begin;  // inclusive
    int v0_end;    // exclusive; <=0 means num_vertices
    int flags;     // QUERYPLAN_FLAG_*
};

// ============================================================================
// Step0.9: A fixed-depth (<=5) configurable plan for generic subgraph matching
// - The xclbin stays fixed; Host updates the plan payload to switch patterns.
// - This is a pragmatic stepping stone toward a richer opcode model.
// ============================================================================
static constexpr int QUERYPLAN_MAX_LEVELS = 5;

enum PlanOp : int {
    PLAN_OP_NOOP = 0,
    PLAN_OP_SCAN_ALL = 1,        // produce embeddings with vid[0]=v0 (range comes from QueryPlan5)
    PLAN_OP_EXPAND_NBR = 2,      // expand neighbors of vid[src_slot0] -> write into vid[dst_slot]
    PLAN_OP_INTERSECT_EMIT = 3,  // intersect N(vid[src_slot0]) and N(vid[src_slot1]) and emit vertices
};

enum PlanBoundKind : int {
    PLAN_BOUND_NONE = 0,
    PLAN_BOUND_CONST = 1,
    PLAN_BOUND_VID_SLOT = 2,
};

// Level-local flags (composable)
static constexpr int LEVELCFG_FLAG_REQUIRE_NEW_LT_BOUND = 1 << 0; // require emitted vertex < bound

struct LevelConfig {
    int op;          // PlanOp
    int dst_slot;    // 0..4: where to write the newly produced vertex
    int src_slot0;   // 0..4: source vertex slot (EXPAND) or operand A (INTERSECT)
    int src_slot1;   // 0..4: operand B for INTERSECT
    int bound_kind;  // PlanBoundKind
    int bound_slot;  // if bound_kind==PLAN_BOUND_VID_SLOT
    int bound_const; // if bound_kind==PLAN_BOUND_CONST
    int flags;       // LEVELCFG_FLAG_*
};

struct QueryPlan5 {
    int num_levels;  // <= QUERYPLAN_MAX_LEVELS
    int v0_begin;    // inclusive
    int v0_end;      // exclusive; <=0 means num_vertices
    LevelConfig levels[QUERYPLAN_MAX_LEVELS];
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

    // Step1-B: plan-driven entrypoint (kernel generates tasks by interpreting QueryPlan)
    void rectangle_kernel_plan_v1(
        const data_t*    gmem_bcsr_data_v1_0,
        const data_t*    gmem_bcsr_data_v2_0,
        const data_t*    gmem_bcsr_data_v1_1,
        const data_t*    gmem_bcsr_data_v2_1,
        const data_t*    gmem_bcsr_data_v1_2,
        const data_t*    gmem_bcsr_data_v2_2,
        const data_t*    gmem_bcsr_data_v1_3,
        const data_t*    gmem_bcsr_data_v2_3,
        const int*       gmem_row_ptr,
        const QueryPlan* gmem_plan,
        int              num_vertices,
        long long*       out_result
    );

    // Step0.9: generic (<=5 level) plan-driven entrypoint
    void subgraph_kernel_plan5_v1(
        const data_t*     gmem_pe0_v1_L0,
        const data_t*     gmem_pe0_v2_L0,
        const data_t*     gmem_pe0_v1_L1,
        const data_t*     gmem_pe0_v2_L1,
        const data_t*     gmem_pe0_v1_L2,
        const data_t*     gmem_pe0_v2_L2,
        const data_t*     gmem_pe0_v1_L3,
        const data_t*     gmem_pe0_v2_L3,
        const data_t*     gmem_pe0_v1_L4,
        const data_t*     gmem_pe0_v2_L4,

        const data_t*     gmem_pe1_v1_L0,
        const data_t*     gmem_pe1_v2_L0,
        const data_t*     gmem_pe1_v1_L1,
        const data_t*     gmem_pe1_v2_L1,
        const data_t*     gmem_pe1_v1_L2,
        const data_t*     gmem_pe1_v2_L2,
        const data_t*     gmem_pe1_v1_L3,
        const data_t*     gmem_pe1_v2_L3,
        const data_t*     gmem_pe1_v1_L4,
        const data_t*     gmem_pe1_v2_L4,

        const data_t*     gmem_pe2_v1_L0,
        const data_t*     gmem_pe2_v2_L0,
        const data_t*     gmem_pe2_v1_L1,
        const data_t*     gmem_pe2_v2_L1,
        const data_t*     gmem_pe2_v1_L2,
        const data_t*     gmem_pe2_v2_L2,
        const data_t*     gmem_pe2_v1_L3,
        const data_t*     gmem_pe2_v2_L3,
        const data_t*     gmem_pe2_v1_L4,
        const data_t*     gmem_pe2_v2_L4,

        const data_t*     gmem_pe3_v1_L0,
        const data_t*     gmem_pe3_v2_L0,
        const data_t*     gmem_pe3_v1_L1,
        const data_t*     gmem_pe3_v2_L1,
        const data_t*     gmem_pe3_v1_L2,
        const data_t*     gmem_pe3_v2_L2,
        const data_t*     gmem_pe3_v1_L3,
        const data_t*     gmem_pe3_v2_L3,
        const data_t*     gmem_pe3_v1_L4,
        const data_t*     gmem_pe3_v2_L4,
        const int*        gmem_row_ptr_pe0,
        const int*        gmem_row_ptr_pe1,
        const int*        gmem_row_ptr_pe2,
        const int*        gmem_row_ptr_pe3,
        const QueryPlan5* gmem_plan,
        int               num_vertices,
        long long*        out_result
    );
}

#endif // MERGE_KERNEL_SIU_V8_GEMINI_1_H
