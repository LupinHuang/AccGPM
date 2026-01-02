# run.tcl
#open_project -reset subgraph_kernel_plan5_v1
open_project subgraph_kernel_plan5_v1
set_top subgraph_kernel_plan5_v1

# 源文件 + 头文件搜索路径
add_files -cflags "-I./include" {src/testSIU_v7_gemini_1.cpp}

# 若有 testbench
#add_files -tb -cflags "-I./include" src/testbench.cpp

#open_solution -reset solution2 -flow_target vitis
open_solution solution1 -flow_target vitis


# Define technology and clock rate

set_part {xcu55c-fsvh2892-2L-e}
create_clock -period 4 -name default

config_cosim  -trace_level all

# Set variable to select which steps to execute
set hls_exec 1

# Set any optimization directives
# End of directives

if {$hls_exec == 0} {
    # Run C simulation
    csim_design

} elseif {$hls_exec == 1} {
    # Run Synthesis and Exit
    #csim_design
    csynth_design
  
    
} elseif {$hls_exec == 2} {
    # Run Synthesis, RTL Simulation and Exit
    csim_design
    csynth_design
    
    cosim_design  
} elseif {$hls_exec == 3} { 
    # Run Synthesis, RTL Simulation, RTL implementation and Exit
    csim_design
    csynth_design
    
    cosim_design
    export_design -format xo -rtl verilog -output set_int_kernel.xo
} else {
    # Default is to exit after setup
    #csynth_design
    close_solution
    close_project
    exit
}
config_export -version 2.0.1
#export_design -format ip_catalog -output set_int_kernel_ip 
#export_design -format xo -rtl verilog -output set_int_kernel.xo


close_solution
close_project
exit
