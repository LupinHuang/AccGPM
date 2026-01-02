HLS_SRC_DIR="./src"

g++ -g -std=c++17 -Wall -O0 ./host/host_v13_gemini_for_burst.cpp \
    -o ./host/host_v13_gemini_for_burst.exe \
    -I${HLS_SRC_DIR} -I${XILINX_HLS}/include/ -I${XILINX_VITIS}/include/ -I$XILINX_XRT/include/ -L$XILINX_XRT/lib -lxrt_coreutil -pthread

emconfigutil --platform xilinx_u55c_gen3x16_xdma_3_202210_1

# ----------------------------------------------------
# 阶段一：创建工作目录 
# ----------------------------------------------------
mkdir -p hw_v16_MAX_2048_gemini
# ----------------------------------------------------
# 阶段二：编译硬件核
# ----------------------------------------------------
v++ -t hw --platform xilinx_u55c_gen3x16_xdma_3_202210_1 -c -k rectangle_kernel_v5  -I./src -o ./hw_v16_MAX_2048_gemini/testSIU_v16_gemini.xo ./src/testSIU_v7_gemini.cpp
# ----------------------------------------------------
# 阶段三：链接生成用于hw的 xclbin 文件
# ----------------------------------------------------
#v++ -t hw --platform xilinx_u55c_gen3x16_xdma_3_202210_1 -l ./hw_v15_MAX_2048_gemini/testSIU_v15_gemini.xo --config ./src/u55C_v6_gemini.cfg  -o ./hw_v15_MAX_2048_gemini/testSIU_hw.xclbin


v++ -l -t hw --platform xilinx_u55c_gen3x16_xdma_3_202210_1 --config ./src/u55C_v6_gemini.cfg --vivado.synth.jobs 256 --vivado.impl.jobs 256 ./hw_v16_MAX_2048_gemini/testSIU_v16_gemini.xo -o ./hw_v16_MAX_2048_gemini/testSIU_hw.xclbin

unset XCL_EMULATION_MODE


./host/host_v13_gemini_for_burst.exe \
 ./hw_v16_MAX_2048_gemini/testSIU_hw.xclbin \
 ./Dataset/citeseer.txt > run_hw_SIU_v16_MAX_gemini_citeseer_.txt