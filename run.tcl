# run.tcl
#open_project -reset rectangle_kernel_v5
open_project rectangle_kernel_v5_new4
set_top rectangle_kernel_v5

# 源文件 + 头文件搜索路径
add_files -cflags "-I./include" {src/testSIU_v7_gemini_1.cpp}

# 若有 testbench
#add_files -tb -cflags "-I./include" src/testbench.cpp

#open_solution -reset solution2 -flow_target vitis
open_solution solution16 -flow_target vitis

#solution4是burts长度512，5是把cache.block改成16个并行ram,忘记改了，生成的新报告还是solution4，现在用taskv16测试solution5
#solution6是把siu的dataflow注释掉
#solution7是用BATCH=8，并且不注释dataflow，此时开始是task_v17。从BATCH=8，也就是rectangle_v5_1开始，时序下降100MHz
#solution8是task_executor19用了任务亲和度的调度方式的资源,忘记改了，新的还是覆盖solution7；把task_19的ii改为2，用kerne_v5_1的sulution13来看看时序，v20尝试再减少一些slack，同样ii=2，用kerne_v5_1的sulution14来看看时序。然后把task_v20的pe和result都inline off了，看一下dataflow，用kerne_v5_1的solution15；soluton16是用3.3ns；solution8尝试减少slack,solution9在8的基础上进行load和compute分离,solution10尝试ii减少,但是solution10上板有布线错误，尝试solution11解决布线错误，同时使load和compute分离
#solution12开始是对task_v18_1的探索，在task_v18的基础上进行修改
#从rectangle_v5_2开始用testSIU_v9,对task_v18_1的一些小修复看solution13，再修复一些deadlock，看solution14；task_v18_2看solution15，rouond-robin那里多打一拍
#在SIU_NEXT0_copy_for_test_origin里，rectangle_v5_new1是新的开始，solution1是先用testSIU_v7，文件夹为hw_emu_SIU_v19_gemini，用的是task_v16(优化后的时序,任务分发多打一拍),solution2用的v16_copy,尝试load compute分离，以及siu_core3
#rectangle_kernel_v5_new1的solution3试试新的siu代码，用的task_v16原版的copy和siu5，solution4用的siu3（原版）和task_v16_copy;solution5用流式SIU（siu_core4）和taskv16原版（这里都是testSIU_v7）,频率用200MHz试试,solution6用siu6+task_executor_v16_origin试试；solution7改头文件试试；solution8搞一个process_rectangle_task的dataflow，用task_executor_v16_origin1;solution9去掉缓存去掉load和compute的dataflow，去掉512bit，看看LOAD_VERTEX前的瓶颈是什么
#用glm4.6试试写SIU，在rectangle_kernel_v5_new2中，先用solution1试试
#rectangle_kernel_v5_new3把maxi的latency改为20试试，solution2尝试把origin5（原本是普通的load和compute并行）的PUSH和PULL分别并行（v1/2并行）
#rectangle_kernel_v5_new4把maxi的latency改为20,把v1和v2并行读取（放到不同的HBM端口）,用testSIU_v7_gemini_1；solution2使用compute和load流式进行(solution2把1覆盖了，忘记改了。。。)solution3用双重buffer尝试消除气泡;solution4用多个CE尝试，solution5尝试把PUSH和PULL都取消；solution6尝试直接把cache传给ce;solution7尝试继续优化去除pull/push；solution8优化并行，因为solution7是串行，但是读取仍然有气泡，要等待ce发出指令；solution9继续优化，在6_5origin基础上进行优化并行，效果不错，第一次超过sota，也就是TESTSIU24；solution10就直接试试3个CE，能不能消除ce之间的气泡;solution11试试4个CE；solution12试试3个CE，但是缓存用四个行来缓存4个点;solution13修改缓存判断;solution14尝试测试cache是否真的有效,把所有点预取了，这样可以探索缓存潜力;solution15试试ce设置为4，看看资源利用率，波形图显示ce=4的时候fifo读取仍然不是全速（ce计算还是慢）;solution16看看把缓存设为8行，然后array partition的资源消耗对比solution13
#rectangle_kernel_v5_new5尝试加pragma cache，用testSIU_v7_gemini_2,失败
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
