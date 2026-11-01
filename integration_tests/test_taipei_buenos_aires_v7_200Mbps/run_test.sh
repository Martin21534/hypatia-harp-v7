#!/bin/bash

# 相同場景優先重用 V6/V5 已產生的 100 ms forwarding state；不存在才重新計算。
cd "$(dirname "$0")" || exit 1
source ../../env/bin/activate
python3 step_0_validate.py || exit 1
if screen -ls 2>/dev/null | grep -qE '\((Detached|Attached)\)'; then
    echo "A screen session is still running; nothing was regenerated or deleted."
    exit 1
fi
v7_state_dir="temp/gen_data/starlink_550_isls_plus_grid_ground_stations_taipei_buenos_aires_algorithm_free_one_only_over_isls/dynamic_state_100ms_for_200s"
v6_gen_data="../test_taipei_buenos_aires_v6_200Mbps/temp/gen_data"
v6_state_dir="$v6_gen_data/starlink_550_isls_plus_grid_ground_stations_taipei_buenos_aires_algorithm_free_one_only_over_isls/dynamic_state_100ms_for_200s"
v5_gen_data="../test_taipei_buenos_aires_v5_200Mbps/temp/gen_data"
v5_state_dir="$v5_gen_data/starlink_550_isls_plus_grid_ground_stations_taipei_buenos_aires_algorithm_free_one_only_over_isls/dynamic_state_100ms_for_200s"
if [ ! -f "$v7_state_dir/fstate_199900000000.txt" ] || [ ! -f "$v7_state_dir/gsl_if_bandwidth_199900000000.txt" ]; then
    if [ -f "$v6_state_dir/fstate_199900000000.txt" ] && [ -f "$v6_state_dir/gsl_if_bandwidth_199900000000.txt" ]; then
        rm -rf temp/gen_data
        mkdir -p temp
        cp -al "$v6_gen_data" temp/gen_data || exit 1
    elif [ -f "$v5_state_dir/fstate_199900000000.txt" ] && [ -f "$v5_state_dir/gsl_if_bandwidth_199900000000.txt" ]; then
        rm -rf temp/gen_data
        mkdir -p temp
        cp -al "$v5_gen_data" temp/gen_data || exit 1
    else
        python3 step_1_generate_satellite_networks_state.py || exit 1
    fi
fi
python3 step_2_generate_runs.py || exit 1
python3 step_3_run.py || exit 1
python3 step_4_generate_plots.py || exit 1
python3 step_5_verify.py || exit 1
