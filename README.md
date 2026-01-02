# GPM_old_code

## 代码文件说明

### 主机端代码

- **`host/host.cpp`**: 主机端主程序
  - 负责加载图数据（CSR 格式）
  - 将图数据转换为 BCSR 格式
  - 生成矩形匹配任务并进行粗粒度剪枝（Residue 位图剪枝）
  - 通过 XRT 与 FPGA 通信，传输数据和任务
  - 调用 FPGA 内核执行计算
  - 收集结果并与 CPU 基准结果对比验证

- **`host/xrt.ini`**: XRT 运行时配置文件
  - 配置 XRT 运行时的各种参数

### FPGA 内核代码

- **`src/testSIU.cpp`**: FPGA 内核顶层入口
  - 定义内核接口 `rectangle_kernel_v5`
  - 实现任务预取、分发和结果收集的流水线
  - 协调 4 个并行处理单元（PE）的工作

- **`src/kernel/task_executor.hpp`**: 任务执行器
  - 实现 PE（Processing Element）工作流程
  - 包含 Load Worker：从 HBM 加载邻接表数据，支持缓存复用和 512-bit burst 读取
  - 包含 Compute Worker：执行 SIU 集合交集计算
  - 实现任务分发和负载均衡

- **`src/kernel/siu_core.hpp`**: SIU 核心算法实现
  - 实现 Sorted Intersection Unit（排序交集单元）
  - 包含 MIN Stage：选择两个排序序列的最小元素
  - 包含 CAS Stage：Bitonic 排序网络，进行并行排序和匹配检测
  - 包含 MERGE Stage：合并匹配的元素并计算位图交集
  - 包含 COMPACT Stage：压缩有效结果

- **`include/merge_kernel_SIU.h`** 和 **`src/merge_kernel_SIU.h`**: 内核头文件
  - 定义 BCSR 数据格式（32-bit 紧凑表示：24-bit 索引 + 8-bit 位图）
  - 定义 RectangleTask 任务结构
  - 定义各种常量和数据结构

### 配置文件

- **`env.sh`**: 环境变量配置脚本
  - 设置 Vitis、Vivado 和 XRT 的环境变量
  - 需要根据实际安装路径修改

- **`u55C.cfg`**: FPGA 连接配置文件
  - 定义内核名称和实例化
  - 配置 HBM Bank 映射（HBM[0-6]）
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
./host/host.exe ./sw_emu_SIU/testSIU_sw_emu.xclbin ./Dataset/citeseer.txt
```

### 硬件仿真模式
```bash
export XCL_EMULATION_MODE=hw_emu
./host/host.exe ./hw_emu_SIU/testSIU_hw_emu.xclbin ./Dataset/citeseer.txt
```

### 硬件模式（实际 FPGA 运行）
```bash
unset XCL_EMULATION_MODE
./host/host.exe ./hw_SIU/testSIU_hw.xclbin ./Dataset/citeseer.txt
```

## 输出说明

程序运行后会显示：
- 图的统计信息（顶点数、边数、BCSR 数据大小）
- 生成的矩形任务数量
- CPU 基准测试结果
- FPGA 加速结果
- 验证结果（PASS/FAIL）和加速比

示例输出：
```
================================================
  Rectangle Pattern Matching (Coarse-grained + SIU)
================================================
INFO: Loading and processing graph...
  Vertices: 3312
  BCSR Data Size: 8462 elements
  Rectangle Tasks: 2358

--- CPU Baseline ---
  Count: 6059
  Time:  4.63505 ms

--- FPGA Acceleration ---
INFO: Transferring data...
INFO: Starting Kernel...
  [Batch 0] Tasks: 2358
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

