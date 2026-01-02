HLS_SRC_DIR="./src"

g++ -g -std=c++17 -Wall -O0 ./host/host_v5.cpp \
    -o ./host/sw_emu_SIU_v5.exe \
    -I${HLS_SRC_DIR} -I${XILINX_HLS}/include/ -I${XILINX_VITIS}/include/ -I$XILINX_XRT/include/ -L$XILINX_XRT/lib -lxrt_coreutil -pthread

emconfigutil --platform xilinx_u55c_gen3x16_xdma_3_202210_1

# ----------------------------------------------------
# 阶段一：创建工作目录 
# ----------------------------------------------------
mkdir -p sw_emu_SIU_v6


# ----------------------------------------------------
# 阶段二：为软件仿真编译硬件核
# ----------------------------------------------------
v++ -t sw_emu --platform xilinx_u55c_gen3x16_xdma_3_202210_1 -c -k rectangle_kernel_v5  -I./src -o ./sw_emu_SIU_v6/testSIU_v6.xo ./src/testSIU_v6.cpp
#--config ./sw_emu/hls_config.cfg
# ----------------------------------------------------
# 阶段三：链接生成用于软件仿真的 xclbin 文件
# ----------------------------------------------------
v++ -t sw_emu --platform xilinx_u55c_gen3x16_xdma_3_202210_1 -l ./sw_emu_SIU_v6/testSIU_v6.xo --config ./src/u55C_v5.cfg  -o ./sw_emu_SIU_v6/testSIU_sw_emu.xclbin

export XCL_EMULATION_MODE=sw_emu

./host/sw_emu_SIU_v5.exe \
 ./sw_emu_SIU_v6/testSIU_sw_emu.xclbin \
 ./Dataset/citeseer.txt > run_sw_emu_SIU_v6_citeseer_.txt