基于现有架构/可复用的模块（不需要整个代码架构都必须拿来用，可以允许修改）（NUM_PE + PE/CE + SIU 交集计数流水线）

要实现 general.md 里那种“在线候选扩展 + 动态生成任务 + 回溯”，严格来说需要：

- `EXPAND` 能把候选集合里的顶点**逐个吐出来**（生成下一层 embedding）
- `INTERSECT` 最好也能**吐出交集元素**（v2/v3/...），而不是只有 count

而当前 SIU/PE 后端主要是 **INTERSECT_COUNT 输出计数**。

所以目前要做的优化：

SIU 增加 “emit vertices” 模式（输出交集顶点流），这样交集结果能继续 expand 到下一层。

**核心思想**：在 Kernel 里实例化 5 个“层处理器”（Level 0..4），外加一个 Count/Collector。候选构造方式必须落在支持的 option 里（expand / intersect / filter / materialize 等）。它们用 hls::stream 串起来，`#pragma HLS DATAFLOW` 跑成流水线。

1.1 在各层之间流动的不是 task，而是 Embedding

现在的架构里流动的是 ExecutionTask(op,v0,v1,v2,upper_bound)。

固定 5 层后建议定义一个更“通用的载体”：

- `Embedding`：携带最多 5 个已绑定顶点 `vid[5]` + 当前层数 `k` + 一些 bound/flags
- “Plan 指令”决定每一层怎么从 `Embedding` 产生下一个 `Embedding`

直觉上就像下面所列的，但实际上不一定每一层都是做这些事：

- Level0 输出 {v0}
- Level1 输入 {v0} 输出 {v0,v1}
- Level2 输入 {v0,v1} 输出 {v0,v1,v2}
- Level3 输入 {v0,v1,v2} 输出 {v0,v1,v2,v3}
- Level4 输入 {v0,v1,v2,v3} 输出 {v0,v1,v2,v3,v4}
- 最后 Count：对某种约束（通常需要一次 `INTERSECT_COUNT`）累加

1.2 每一层是一个“可配置模板单元”

每层都长得一样，但有一个 LevelConfig cfg[i] 决定它做什么：

典型 cfg 字段（和 general.md 对齐）：

- op：`SCAN_ALL / EXPAND_NBR / INTERSECT_COUNT / FILTER / NOOP`
- `srcA`、`srcB`：本层候选怎么构造（从 embedding 的哪个祖先或者候选集合获取交集的来源）
- `ordering`：如 `v_new < v_bound`（破除对称）
- `upper_bound_source`：upper bound 过滤
- `emit_to`：把生成的点写到 `vid[k]` 的哪个槽位

> 关键点：硬件不变，cfg 变。cfg 由 Host 写入（通过 gmem_plan 或 AXI-lite / 小的 m_axi 读取都行），因此换 pattern 不用重编译。
> 

目前架构中可复用或稍微调整修改就可以用的模块

- 已经有的 **“把顶点 id 解析成 start/len”** 的逻辑（gmem_row_ptr + resolver）
- 已经有的 **PE/CE/SIU 后端**：例如 pe_worker_from_execution_tasks(...) 这条链路

………………

### 需要新增

1. **Embedding Stream 通道**（Level-to-Level）
2. **5 个 LevelUnit 模板**（可以写成一个函数模板 + 5 次实例化）

**一个例子：5 层怎么跑：以 Triangle 为例（不需要 SIU emit）**

Triangle：v0-v1-v2-v0，最后就是 count：

对每个 (v0,v1)，统计 `|N(v0) ∩ N(v1) |` 加上filter

triangle可以用 3 层就够，但我们用“最多 5 层”的骨架来承载它：

- **Level0 (SCAN_ALL)**：输出所有可能的 {v0}
- **Level1 (EXPAND_NBR src=v0)**：输出 {v0,v1}（遍历 N(v0)），把输出结果存在片上中间结果缓冲中
- **Level2 (INTERSECT_COUNT srcA=v0, srcB=v1)**：不再输出 embedding，而是直接发 ExecutionTask(v1=v0, v2=v1, upper_bound=...) 给后端计数
- **Level3/4**：cfg 配成 NOOP（或直接不启用）

**如果某一层需要 “交集结果作为下一层的候选集合继续 expand”**，那就需要调整修改 SIU，输出交集结果