HLS_SRC_DIR="./src"

g++ -g -std=c++17 -Wall -O0 ./host/host_v13_gemini_for_burst_1.cpp \
    -o ./host/host_v13_gemini_for_burst_1.exe \
    -I${HLS_SRC_DIR} -I${XILINX_HLS}/include/ -I${XILINX_VITIS}/include/ -I$XILINX_XRT/include/ -L$XILINX_XRT/lib -lxrt_coreutil -pthread

emconfigutil --platform xilinx_u55c_gen3x16_xdma_3_202210_1

# ----------------------------------------------------
# 阶段一：创建工作目录 
# ----------------------------------------------------
mkdir -p sw_emu_1


# ----------------------------------------------------
# 阶段二：为软件仿真编译硬件核
# ----------------------------------------------------
v++ -t sw_emu --platform xilinx_u55c_gen3x16_xdma_3_202210_1 -c -k subgraph_kernel_plan5_v1  -I./src -o ./sw_emu_1/test1.xo ./src/testSIU_v7_gemini_1.cpp
#--config ./sw_emu/hls_config.cfg
# ----------------------------------------------------
# 阶段三：链接生成用于软件仿真的 xclbin 文件
# ----------------------------------------------------
v++ -t sw_emu --platform xilinx_u55c_gen3x16_xdma_3_202210_1 -l ./sw_emu_1/test1.xo --config ./src/u55C_v6_gemini_1.cfg  -o ./sw_emu_1/testSIU_sw_emu.xclbin

export XCL_EMULATION_MODE=sw_emu

./host/host_v13_gemini_for_burst_1.exe \
 ./sw_emu_1/testSIU_sw_emu.xclbin \
 ./Dataset/test_graph_small.txt > sw_emu_small1.txt