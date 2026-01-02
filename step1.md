## 第一步

第一步演进目标：Kernel 内置任务生成与调度 (On-Chip Task Generation)

我们暂时不修改底层的 SIU 算子（求交集的核心逻辑先不动），而是**替换掉 Kernel 最前端的输入部分**。

### 1. Host 端的变化：从“发任务”变为“发计划”

目前 Host 代码 (`host_v13_gemini_for_burst_1.cpp`) 里有大量的 `for` 循环在枚举 `v0` 和 `v1`。

**第一步改造：**

- 删除 Host 端枚举具体顶点的循环。
- 定义一个新的结构体 `QueryPlan`（查询计划）。
- Host 只需要把图数据 (`BCSR`) 和这个 `QueryPlan` 发给 Kernel。

**Query Plan 长什么样？**

为了简单起见，第一版可以设计成一个**静态指令表**。假设我们要匹配一个三角形 (v0-v1-v2-v0)，Plan 包含的内容：

1. **Level 0**: 选取所有顶点作为 `v0` 的候选。
2. **Level 1**: 获取 `v0` 的邻居N(v0)作为 `v1` 的候选。
3. **Level 2**: 获取 `v0` 的邻居 N(v0)和 `v1` 的邻居N(v1)，
4. **Level 3**：对v0的邻居集合和v1的邻居集合求交集得到 `v2` 的候选。
5. **Level 4**：对v2候选做filter
6. **Level5：Count**: 统计 `v2` 的数量。

### 2. Kernel 端的变化：引入“控制核心”

新的kernel思路：

**新增模块：`TaskScheduler` (任务调度器)**

- **输入**：接收 Host 发来的 `QueryPlan`。
- **状态存储**：任务需要一个片上RAM来存储。
    - 因为 Kernel 自己做扩展时，会产生中间状态。比如选中了 `v0=5`，需要把这个状态存下来，然后去处理它的邻居。
    - 我们第一步可以先用 **DFS (深度优先搜索)策略进行**，对片上存储压力最小，不需要复杂的内存管理。
- **逻辑**：
    1. 初始化：把“根任务”（遍历所有点）压入栈。
    2. 循环：
        - 从栈顶取出一个当前的 partial embedding (例如 `{v0=5, v1=10}`)。
        - 查看 `QueryPlan`，当前层级是 Level 2。
        - 由于从Plan中可以知道 ：如果是Level2，下一level就需要进行交集计算，需要计算N(v0)∩N(v1)（ N(5)∩N(10)）。
        - 把“求交集“任务交给SIU进行计算。

### 3. 挑战与解决方案：数据回流 (Feedback Loop)

这是当前架构和目标架构最大的区别。

- **当前**：SIU 算完交集，只需要输出一个数字（count），流水线是单向的。
- **目标**：SIU 算完 N(5)∩N(10)，得到的结果（例如 `{12, 15, 99}`）**不能丢掉**，必须**流回**到 `TaskScheduler`。
    - `TaskScheduler` 收到 `{12, 15, 99}` 后，会把它们变成新的扩展任务 `{v0=5, v1=10, v2=12}`, `{v0=5, v1=10, v2=15}`，`{v0=5, v1=10, v2=99}` 等，再次压入栈中，等待下一次的task调度。

**第一步的具体实施建议：**

1. **修改 RectangleTask 结构体**：
    - 现在的 RectangleTask 是写死的 `v0, v1, v2`。
    - 把它改成更通用的 `ExecutionTask`，包含：
        - `current_level`: 当前在匹配第几层。
        - `embedding[]`: 已经匹配的顶点数组。
        - `operand_pointers`: 需要取交集的邻居列表指针（指向图数据）。
2. **改造 SIU 的输出接口**：
    - 目前的 SIU 可能只吐出 count。
    - 需要让 SIU 支持 **Materialize (物化)** 模式：把交集结果写到一个 FIFO Stream 中，而不是只给计数器（不过在Rectangle中不需要，因为只要做一次集合交集）
3. **建立闭环**：
    - 在 Kernel 内部建立一个 `Stream Loop`。
    - `Scheduler` -> `SIU` /`LOAD_neighborlist`-> `Result FIFO` -> `Scheduler`。
    - `Scheduler` 不断从 `Result FIFO` 读取上一层的结果，生成下一层的任务。