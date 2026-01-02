我的目标是进行通用pattern的子图匹配，也就是说，
Host 不再枚举具体 (v0,v1,v2,...) 的实例级任务，而是只下发 **pattern 的查询方案/执行计划（query plan）**；

Kernel 在板上根据 plan 进行 **在线候选扩展 + 动态生成/调度任务 + 累加匹配数**；

Task 不再等价于“交集计数”，而是更细粒度的“算子/微指令”（load / intersect / filter / materialize / emit-next 等）；

Host 负责：针对 pattern 产生 **查询方案**（顶点扩展顺序、每一层需要满足的边约束、对称性破除/去重规则、可能的上界约束等）。这部分如果 pattern 不同，生成的方案就完全不同

Kernel 负责：在数据图上进行 **实例级的顶点选择与扩展**（根顶点v0 选谁、下一层候选怎么由邻居/交集得到、如何过滤、什么时候产生下一层任务/回溯、最后怎么计数）

**Task 需要细粒度：把它从“rectangle专用结构体”升级成“可解释的算子任务”**

最终的 task 形态更像一个opcode + operands 的执行模型，而不是固定字段：“v0,v1,v2,start/len,upper_bound”

一个通用方向是把 task 定义成两类：

- **数据面任务（Data Tasks）**：对集合/候选做操作
    - `LOAD_NBR(u) -> S`：从 HBM 或片上缓存加载 N(u)（在本架构中用BCSR形式）
    - `INTERSECT(Sa, Sb) -> Sc`：对两个BCSR邻接表做集合交集，可以用我原本的SIU，只是输出部分需要改为输出交集的具体顶点，而不是只输出交集数量
    - `FILTER_LT(Sc, bound)` / `FILTER_BY_MASK(Sc, mask)`：剪枝/约束（类似当前的 upper_bound）
    - `MATERIALIZE(Sc) -> cache_id`：把本层候选落到片上缓存，供下一层（下一个点的extend）复用
- **控制面任务（Control Tasks）**：生成下一层扩展/回溯与计数
    - `EXPAND(level, partial_embedding, candidate_set)`：从候选集中取一个顶点赋值给该 level 的 pattern 顶点，生成子任务
    - `COUNT_IF_LAST(level, candidate_set)`：如果到了最后一层，直接把前一层的集合交集输出交集个数即可

这样 kernel 端就不仅能做交集，还能做“生成下一层工作”的控制逻辑。

**Kernel 端“自动生成 task”的关键：需要一个板上调度模型**

“kernel 端生成 task 并执行直到结束”，在硬件上一般落地为：

- 一个或多个 **work queue / task queue**（片上 RAM（BRAM/URAM）)
- 一个 **scheduler/dispatcher**（从队列取任务，分发给不同算子单元：load、intersect、filter、expand）
- 若干个 **operator engines**（本质是你现在已有的 SIU 交集引擎 + 加载/过滤/Compact等）
- 一个 **accumulator**（计数汇总）

这和你当前的结构有继承关系：现在已经有 task_prefetcher → round-robin → PE/CE → collector，只是 task 的语义被固定成“对 v1/v2 交集计数”。你要做的是把它变成“能跑多种 opcode 的解释执行/微码执行”。

**Host 下发的“pattern 查询方案”建议长什么样**

为了让 kernel 能“自动生成所需 task”，Host 下发的 plan 最好是结构化的，例如每一层给出：

- 本层 pattern 顶点：p[k]
- 依赖的已绑定顶点集合（在expand第k个定点之前的前面已经连接的i个顶点的祖先集合）：`{p[i] | i<k}`
- 候选构造规则：例如 Cand(p[k]) = N(img(p[a])) ∩ N(img(p[b])) ∩ ...（直接映射成若干 `LOAD + INTERSECT`）
- 过滤/去重规则：如 img(p[k]) < img(p[t])（对称性破除），或 upper_bound 这类 bound（映射成 `FILTER`）
- 是否需要物化候选：某些层要落 BRAM 以供多次复用（映射成 `MATERIALIZE`）

这样 kernel 才能做到：给定 partial embedding，按规则生成下一层候选集合，继而生成下一批任务。

**这套“完整架构”最大的工程难点**

实现的硬约束：

- **部分匹配（partial embeddings）数量会爆炸**：kernel 端生成任务意味着队列可能非常大，需要明确“片上存多少、溢出怎么 spill、spill 的格式/带宽”。
- **控制流复杂**：子图匹配天然有回溯/分支；硬件上需要把它转换成“任务队列驱动的迭代执行”，避免深递归。还要充分利用FPGA的并行性和pipeline/dataflow架构，尽量做出深度流水线提高吞吐
- **候选集合的表示**：交集可以用 BCSR block 流，复用或微调原来的SIU求交集来进行