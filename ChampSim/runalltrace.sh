#!/bin/bash

# 定义trace文件路径和ChampSim二进制文件的路径
trace_path="./trace/"
champsim_binary="./bin/champsim"

# 模拟的warmup和simulation指令数
warmup_instructions=2000000
simulation_instructions=10000000

# 运行配置脚本
./config.sh ./champsim_config.json

# 从配置文件中提取替换策略
replacement_policy=$(python3 -c "import json; print(json.load(open('champsim_config.json'))['LLC']['replacement'])")

# 输出当前替换策略
echo "Current LLC replacement policy is: $replacement_policy"

# 编译ChampSim
make

# 确保ChampSim二进制文件存在且可执行
if [ ! -f "$champsim_binary" ]; then
    echo "ChampSim binary not found at $champsim_binary."
    exit 1
fi

# 自动找到trace文件夹中的所有.trace.xz文件
traces=$(find $trace_path -name "*trace.xz")

# 遍历trace文件列表，并执行命令
for trace_file in $traces; do
    if [ ! -f "$trace_file" ]; then
        echo "Trace file not found: $trace_file"
        continue
    fi

    trace=$(basename $trace_file)
    echo "Running simulation for $trace using $replacement_policy replacement policy..."
    
    # 将 ChampSim 的输出重定向到临时文件
    temp_output=$(mktemp)
    $champsim_binary --warmup-instructions $warmup_instructions --simulation-instructions $simulation_instructions $trace_file > "$temp_output"

    # 提取并计算命中率
    total_access=$(grep -m 1 "LLC TOTAL" "$temp_output" | awk '{print $4}')
    total_hits=$(grep -m 1 "LLC TOTAL" "$temp_output" | awk '{print $6}')
    if [ ! -z "$total_access" ] && [ ! -z "$total_hits" ]; then
        miss_rate=$(echo "scale=4; (1 - $total_hits / $total_access) * 100" | bc)
        echo "LLC Cache Miss Rate for $trace: $miss_rate%"
    else
        echo "LLC Cache Miss Rate for $trace: Data not found"
    fi
    # 提取并计算L2C级别的缺失率
    l2c_total_access=$(grep -m 1 "cpu0_L2C TOTAL" "$temp_output" | awk '{print $4}')
    l2c_total_hits=$(grep -m 1 "cpu0_L2C TOTAL" "$temp_output" | awk '{print $6}')
    if [ ! -z "$l2c_total_access" ] && [ ! -z "$l2c_total_hits" ]; then
        l2c_miss_rate=$(echo "scale=4; (1 - $l2c_total_hits / $l2c_total_access) * 100" | bc)
        echo "L2C Cache Miss Rate for $trace: $l2c_miss_rate%"
    else
        echo "L2C Cache Miss Rate for $trace: Data not found"
    fi

    # 删除临时文件
    rm "$temp_output"
done


echo "All simulations completed."
