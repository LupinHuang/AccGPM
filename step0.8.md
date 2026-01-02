将子图匹配过程看作一条**装配流水线**。每个“工位”（Level Processor）只负责给当前的半成品（Partial Embedding）加一个新顶点，或者检查一下质量（Filter）。

核心数据结构：`EmbeddingStream`

在各级处理器之间流动的不再是简单的 v0, v1，而是一个携带了上下文信息的**宽总线数据包**。

- **定义**：`Embedding` 结构体。
- **内容**：
    - `vertices[MAX_DEPTH]`: 存储当前已经匹配好的顶点 ID 数组。例如 `{v0_id, v1_id, v2_id, ...}`。
    - `valid_count`: 当前有效顶点的数量。
- **流向**：`Level 0` -> `Level 1` -> `Level 2` -> ... -> `Collector`。

 硬件模块设计：`GenericLevelUnit`

我们需要设计一个通用的硬件模块，它被实例化多次（例如 4 次，对应 Level 0 到 Level 3）。每个实例在运行时通过配置寄存器（Instruction）来决定自己的行为。

输入/输出

- **Input Stream**: 上一层传来的 `Embedding`（例如 {v0=5}）。
- **Output Stream**: 处理后生成的新的 `Embedding`（例如 {v0=5, v1=10}, {v0=5, v1=12}...）。
- **Config**: Host 下发的指令，告诉这一层该干什么。

内部逻辑（状态机）

这个单元内部需要处理 BCSR 数据。

**指令类型 A: `OP_SCAN_ALL` (通常用于 Level 0)**

- **行为**：忽略输入流。自己生成一个 `0` 到 num_vertices 的循环。
- **输出**：不断向下游发送 {v0=0}, {v0=1}, ...

**指令类型 B: `OP_EXPAND` (最核心的操作)**

- **配置参数**：`src_index` (基于哪个祖先扩展？)。
- **行为**：
    1. 从 Input Stream 读一个 Embedding `E`。
    2. 取出 `u = E.vertices[src_index]`。
    3. **查表 (Metadata Fetch)**: 查 `row_ptr[u]` 得到 start/len。
    4. **加载 (Load)**: 从 HBM 读取 `u` 的 BCSR 邻接表块。
    5. **遍历 (Iterate)**: 解析 BCSR 块，得到每一个邻居 v。
    6. **生成 (Emit)**: 复制 `E`，把 v 追加到末尾，形成新 Embedding `E'`。
    7. 向下游发送 `E'`。

**指令类型 C: `OP_INTERSECT` (你的 SIU 所在位置)**

- **配置参数**：`src_index_1`, `src_index_2`。
- **行为**：
    1. 读入 Embedding `E`。
    2. 取出 `u1 = E.vertices[src1]`, `u2 = E.vertices[src2]`。
    3. **双路加载**: 同时读取 `u1` 和 `u2` 的 BCSR 邻接表。
    4. **SIU 计算**: 调用 `siu_core` 模块求交集。
    5. **生成**: 对交集中的每个结果 v，生成新 Embedding（下一个顶点扩展的候选集合）。

架构连接图 (Topology)

假设我们要匹配三角形 (Triangle): v0 -- v1 -- v2 -- v0。

```
graph TD
Host[Host: Config Registers] -->|Inst 0: SCAN_ALL| L0
Host -->|Inst 1: EXPAND(src=0)| L1
Host -->|Inst 2: INTERSECT(src1=0, src2=1)| L2
subgraph FPGA Kernel
    L0[Level 0 Unit]
    L1[Level 1 Unit]
    L2[Level 2 Unit]

    L0 -->|Stream: {v0}| L1
    L1 -->|Stream: {v0, v1}| L2
    L2 -->|Stream: {v0, v1, v2}| Counter[Result Collector]
end

HBM[(HBM: BCSR Data)] -.-> L1
HBM -.-> L2

```