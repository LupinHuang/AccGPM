HLS_SRC_DIR="./src"

g++ -g -std=c++17 -Wall -O0 ./host/host_v13_gemini_for_burst_1.cpp \
    -o ./host/host_v13_gemini_for_burst_1.exe \
    -I${HLS_SRC_DIR} -I${XILINX_HLS}/include/ -I${XILINX_VITIS}/include/ -I$XILINX_XRT/include/ -L$XILINX_XRT/lib -lxrt_coreutil -pthread

emconfigutil --platform xilinx_u55c_gen3x16_xdma_3_202210_1

# ----------------------------------------------------
# 阶段一：创建工作目录 
# ----------------------------------------------------
mkdir -p hw_emu_SIU_new_v13_test12_arraypar_gemini


# ----------------------------------------------------
# 阶段二：为软件仿真编译硬件核
# ----------------------------------------------------
#v++ -c --mode hls --platform xilinx_u55c_gen3x16_xdma_3_202210_1 --config ./src/hls_config2.cfg --work_dir ./hw_emu_SIU_v4

v++ -g -t hw_emu --platform xilinx_u55c_gen3x16_xdma_3_202210_1 -c -k rectangle_kernel_v5 -I./src -o ./hw_emu_SIU_new_v13_test12_arraypar_gemini/testSIU_new_v13_test12_gemini.xo ./src/testSIU_v7_gemini_1.cpp
#--config ./sw_emu/hls_config.cfg
# ----------------------------------------------------
# 阶段三：链接生成用于软件仿真的 xclbin 文件
# ----------------------------------------------------
v++ -g -t hw_emu --platform xilinx_u55c_gen3x16_xdma_3_202210_1 -l ./hw_emu_SIU_new_v13_test12_arraypar_gemini/testSIU_new_v13_test12_gemini.xo --config ./src/u55C_v6_gemini_1.cfg  -o ./hw_emu_SIU_new_v13_test12_arraypar_gemini/testSIU_hw_emu.xclbin

#export XCL_EMULATION_MODE=hw_emu

#./host/host_v13_gemini_for_burst_1.exe \
# ./hw_emu_SIU_new_v13_test6_gemini/testSIU_hw_emu.xclbin \
#  ./Dataset/2.txt > run_hw_emu_SIU_new_v13_test6_2_.txt