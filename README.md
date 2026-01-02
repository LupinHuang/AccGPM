
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

