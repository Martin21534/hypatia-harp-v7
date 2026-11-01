"""依 run_list.py 產生 CUBIC、HARP 6.0 與 HARP 7.0 的 ns-3 run 設定。"""

import exputil

from run_list import get_tcp_run_list

local_shell = exputil.LocalShell()

# 一定要在清除舊結果之前檢查，避免誤刪仍在執行中的模擬輸出。
if local_shell.count_screens() != 0:
    print("A screen session is still running; no run data was deleted.")
    raise SystemExit(1)

local_shell.remove_force_recursive("temp/runs")
local_shell.remove_force_recursive("temp/pdf")
local_shell.remove_force_recursive("temp/data")

for run in get_tcp_run_list():
    run_dir = "temp/runs/" + run["name"]
    local_shell.make_full_dir(run_dir)

    config = run_dir + "/config_ns3.properties"
    local_shell.copy_file("templates/template_tcp_a_b_config_ns3.properties", config)
    replacements = {
        "[SATELLITE-NETWORK]": run["satellite_network"],
        "[DYNAMIC-STATE]": run["dynamic_state"],
        "[DYNAMIC-STATE-UPDATE-INTERVAL-NS]": run["dynamic_state_update_interval_ns"],
        "[SIMULATION-END-TIME-NS]": run["simulation_end_time_ns"],
        "[ISL-DATA-RATE-MEGABIT-PER-S]": run["isl_data_rate_megabit_per_s"],
        "[GSL-DATA-RATE-MEGABIT-PER-S]": run["gsl_data_rate_megabit_per_s"],
        "[ISL-MAX-QUEUE-SIZE-PKTS]": run["isl_queue_size_pkt"],
        "[GSL-MAX-QUEUE-SIZE-PKTS]": run["gsl_queue_size_pkt"],
        "[ENABLE-ISL-UTILIZATION-TRACKING]": (
            "true" if run["enable_isl_utilization_tracking"] else "false"
        ),
        "[TCP-INITIAL-SLOW-START-THRESHOLD-BYTE]": (
            run["tcp_initial_slow_start_threshold_byte"]
        ),
        "[TCP-SOCKET-TYPE]": run["tcp_socket_type"],
        "[GSL-RANDOM-LOSS-MODE]": run["random_loss_mode"],
        "[GSL-RANDOM-LOSS-SCHEDULE-FILENAME]": (
            run["random_loss_schedule_filename"]
        ),
        "[GSL-RANDOM-LOSS-SOURCE-NODE-ID]": run["random_loss_source_node_id"],
        "[GSL-RANDOM-LOSS-TARGET-NODE-ID]": run["random_loss_target_node_id"],
        "[GSL-RANDOM-LOSS-STREAM]": run["random_loss_stream"],
    }
    for placeholder, value in replacements.items():
        local_shell.sed_replace_in_file_plain(config, placeholder, str(value))

    utilization_line = ""
    if run["enable_isl_utilization_tracking"]:
        utilization_line = (
            "isl_utilization_tracking_interval_ns="
            + str(run["isl_utilization_tracking_interval_ns"])
        )
    local_shell.sed_replace_in_file_plain(
        config,
        "[ISL-UTILIZATION-TRACKING-INTERVAL-NS-COMPLETE]",
        utilization_line,
    )

    schedule = run_dir + "/schedule.csv"
    local_shell.copy_file("templates/template_tcp_a_b_schedule.csv", schedule)
    local_shell.sed_replace_in_file_plain(schedule, "[FROM]", str(run["from_id"]))
    local_shell.sed_replace_in_file_plain(schedule, "[TO]", str(run["to_id"]))

    random_loss_schedule = run_dir + "/" + run["random_loss_schedule_filename"]
    local_shell.copy_file(
        "templates/template_gsl_random_loss_schedule.csv", random_loss_schedule
    )

print("Success: generated CUBIC, HARP 6.0 and HARP 7.0 ns-3 runs")
