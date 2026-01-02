# AccGPM_dataflow架构
## 不再硬编码pattern

## 代码文件说明

### 主机端代码

- **`host/host_v13_gemini_for_burst_1.cpp`**: 主机端主程序
  - 负责加载图数据（CSR 格式）
  - 将图数据转换为 BCSR 格式
  - 构建 QueryPlan5 查询计划（支持最多 5 层级的子图匹配模式）
  - 通过 XRT 与 FPGA 通信，传输图数据和查询计划
  - 调用 FPGA 内核执行计算
  - 收集结果并与 CPU 基准结果对比验证

- **`host/xrt.ini`**: XRT 运行时配置文件
  - 配置 XRT 运行时的各种参数

### FPGA 内核代码

- **`src/testSIU_v7_gemini_1.cpp`**: FPGA 内核顶层入口
  - 定义内核接口 `subgraph_kernel_plan5_v1`
  - 实现基于 QueryPlan5 的流水线架构
  - 协调 4 个并行处理单元（PE）的工作
  - 每个 PE 拥有 5 个层级（L0-L4）的独立内存端口，避免 DATAFLOW 端口冲突

- **`src/kernel/task_executor_v16_origin8.hpp`**: 任务执行器和流水线实现
  - 实现基于 QueryPlan5 的流水线处理单元（PE）
  - **plan5_scan_all_stage**：初始扫描阶段，生成 v0 候选并分发到各 PE
  - **plan5_pipeline_pe**：多层级流水线处理，支持最多 5 层级的子图匹配
  - **plan5_level_unit**：可配置的层级处理单元，支持三种操作：
    - `PLAN_OP_NOOP`：无操作，直接转发
    - `PLAN_OP_EXPAND_NBR`：扩展邻居，从指定顶点的邻接表中生成候选
    - `PLAN_OP_INTERSECT_EMIT`：集合交集，对两个邻接表求交集并输出匹配顶点
  - **plan5_expand_nbr_stage**：邻居扩展阶段，从 BCSR 数据中展开位图并生成候选
  - **plan5_intersect_emit_stage**：交集计算阶段，使用 SIU 核心进行集合交集
  - 支持上界过滤（upper bound filtering）和对称性破除
  - 使用 Embedding5 流式数据结构传递部分匹配结果

- **`src/kernel/siu_core_6.hpp`**: SIU 核心算法实现
  - 实现 Sorted Intersection Unit（排序交集单元）
  - 包含 MIN Stage：选择两个排序序列的最小元素
  - 包含 CAS Stage：Bitonic 排序网络，进行并行排序和匹配检测
  - 包含 MERGE Stage：合并匹配的元素并计算位图交集
  - 包含 COMPACT Stage：压缩有效结果
  - 输出不是计数，是直接输出交集数组

- **`include/merge_kernel_SIU_v8_gemini_1.h`** 和 **`src/merge_kernel_SIU_v8_gemini_1.h`**: 内核头文件
  - 定义 BCSR 数据格式（32-bit 紧凑表示：24-bit 索引 + 8-bit 位图）
  - 定义 QueryPlan5 查询计划结构（支持最多 5 层级的配置）
  - 定义 LevelConfig 层级配置结构（操作类型、源/目标顶点槽位、上界过滤等）
  - 定义 Embedding5 流式数据结构（存储部分匹配的顶点序列）
  - 定义 ExecutionTask 任务结构（向后兼容）

### 配置文件

- **`env.sh`**: 环境变量配置脚本
  - 设置 Vitis、Vivado 和 XRT 的环境变量
  - 需要根据实际安装路径修改

- **`src/u55C_v6_gemini_1.cfg`**: FPGA 连接配置文件
  - 定义内核名称 `subgraph_kernel_plan5_v1` 和实例化名称 `siu_core_v5`
  - 配置 HBM Bank 映射：
    - 每个 PE 的每个层级（L0-L4）都有独立的 HBM Bank
    - PE0: HBM[0-4] (L0-L4)
    - PE1: HBM[6-10] (L0-L4)
    - PE2: HBM[11-15] (L0-L4)
    - PE3: HBM[16-20] (L0-L4)
    - 行指针数组：HBM[21, 24, 25, 26]
  - 配置性能分析选项

### 编译脚本

- **`sw_emu.sh`**: 软件仿真编译脚本
  - 编译主机程序
  - 生成软件仿真用的 xclbin 文件
  - 设置软件仿真模式并运行测试

- **`hw_emu.sh`**: 硬件仿真编译脚本
  - 编译主机程序
  - 生成硬件仿真用的 xclbin 文件
  - 设置硬件仿真模式并运行测试

- **`hardware_implement.sh`**: 硬件实现编译脚本
  - 编译主机程序
  - 生成实际硬件可用的 xclbin 比特流文件
  - 使用 Vivado 进行综合和实现（耗时最长）

## 环境配置

1. 设置环境变量（编辑 `env.sh` 中的路径）：
```bash
source env.sh
```

## 编译步骤

### 1. 编译主机程序

host的编译直接运行sw/hw_emu.sh即可

### 2. 编译 FPGA 内核

根据需求选择以下三种模式之一：

**软件仿真模式（最快，用于功能验证）**
```bash
./sw_emu.sh
```

**硬件仿真模式（较慢，用于性能估算）**
```bash
./hw_emu.sh
```

**硬件实现模式（最慢，生成实际可用的比特流）**
```bash
./hardware_implement.sh
```

## 运行程序

### 软件仿真模式
```bash
export XCL_EMULATION_MODE=sw_emu
./host/host_v13_gemini_for_burst_1.exe ./sw_emu_SIU/testSIU_sw_emu.xclbin ./Dataset/citeseer.txt
```

### 硬件仿真模式
```bash
export XCL_EMULATION_MODE=hw_emu
./host/host_v13_gemini_for_burst_1.exe ./hw_emu_SIU/testSIU_hw_emu.xclbin ./Dataset/citeseer.txt
```

### 硬件模式（实际 FPGA 运行）
```bash
unset XCL_EMULATION_MODE
./host/host_v13_gemini_for_burst_1.exe ./hw_SIU/testSIU_hw.xclbin ./Dataset/citeseer.txt
```

## 架构说明

### QueryPlan5 驱动的流水线架构

当前加速器采用基于查询计划的流水线架构，支持通用的子图模式匹配：

1. **查询计划（QueryPlan5）**：
   - 支持最多 5 层级的子图匹配模式
   - 每层可配置不同的操作类型（NOOP、EXPAND_NBR、INTERSECT_EMIT）
   - 支持上界过滤和对称性破除等优化

2. **并行处理单元（PE）**：
   - 4 个独立的 PE（PE0-PE3），通过 round-robin 方式分发任务
   - 每个 PE 拥有 5 个层级（L0-L4）的独立内存端口
   - 每个层级都有独立的 v1 和 v2 数据端口，避免 DATAFLOW 端口冲突

3. **流水线处理流程**：
   - **Level 0 (Scan)**：扫描所有顶点作为 v0 候选，分发到各 PE
   - **Level 1-4**：根据查询计划配置执行扩展或交集操作
   - **Count**：统计最终匹配的子图数量

4. **内存访问优化**：
   - 每个 PE 的每个层级都有独立的 HBM Bank，实现真正的并行访问
   - 支持 512-bit burst 读取，提高内存带宽利用率
   - 行指针数组缓存在片上 URAM 中，避免重复访问

## 输出说明

程序运行后会显示：
- 图的统计信息（顶点数、边数、BCSR 数据大小）
- 查询计划配置信息
- CPU 基准测试结果
- FPGA 加速结果
- 验证结果（PASS/FAIL）和加速比

示例输出：
```
================================================
  Subgraph Pattern Matching (QueryPlan5 + SIU)
================================================
INFO: Loading and processing graph...
  Vertices: 3312
  BCSR Data Size: 8462 elements
  Query Plan: Triangle matching (3 levels)

--- CPU Baseline ---
  Count: 6059
  Time:  4.63505 ms

--- FPGA Acceleration ---
INFO: Transferring data...
INFO: Starting Kernel...
  Count: 6059
  Kernel Time:  0.993686 ms

=== Summary ===
CSR-CPU   : 6059
FPGA-SIU  : 6059

PASS! Speedup (CPU/KERNEL): 4.6645x
```


## 数据集格式

输入图文件格式（CSR 格式）：
```
第一行：顶点数 边数
第二行开始：行指针数组（顶点数+1个整数）
接下来：列索引数组（边数个整数）
```

示例：
```
3 4
0 2 3 4
1 2 0 1
```
表示一个包含 3 个顶点、4 条边的图。

