"""Taipei→Buenos Aires 200 Mbps：比較 CUBIC、HARP 6.0 與 HARP 7.0。"""

# 每 100 ms 重新計算衛星 forwarding state。
dynamic_state_update_interval_ms = 100
simulation_end_time_s = 200
enable_isl_utilization_tracking = True
isl_utilization_tracking_interval_ns = 1 * 1000 * 1000 * 1000

# 三個版本共用完全相同的網路與排程式 GSL random-loss，只改 TCP 演算法。
isl_data_rate_megabit_per_s = 200.0
gsl_data_rate_megabit_per_s = 400.0
isl_queue_size_pkt = 500
gsl_queue_size_pkt = 500
tcp_initial_slow_start_threshold_pkt = 1100
tcp_segment_size_byte = 1380
tcp_initial_slow_start_threshold_byte = (
    tcp_initial_slow_start_threshold_pkt * tcp_segment_size_byte
)

random_loss_mode = "scheduled"
random_loss_schedule_filename = "gsl_random_loss_schedule.csv"
random_loss_source_node_id = 1584
random_loss_target_node_id = 1585
random_loss_stream = 1

dynamic_state_update_interval_ns = dynamic_state_update_interval_ms * 1000 * 1000
simulation_end_time_ns = simulation_end_time_s * 1000 * 1000 * 1000
dynamic_state = (
    "dynamic_state_"
    + str(dynamic_state_update_interval_ms)
    + "ms_for_"
    + str(simulation_end_time_s)
    + "s"
)

# Starlink-550 有 1584 顆衛星，因此 1584/1585 是 Taipei/Buenos Aires 地面站。
full_satellite_network_isls = (
    "starlink_550_isls_plus_grid_ground_stations_"
    "taipei_buenos_aires_algorithm_free_one_only_over_isls"
)

chosen_pairs = [
    ("starlink_550_taipei_buenos_aires_cubic", 1584, 1585, "TcpCubic"),
    ("starlink_550_taipei_buenos_aires_harp_cubic_v6", 1584, 1585, "TcpHarpCubicV6"),
    ("starlink_550_taipei_buenos_aires_harp_cubic_v7", 1584, 1585, "TcpHarpCubicV7"),
]


def get_tcp_run_list():
    runs = []
    for prefix, from_id, to_id, tcp_type in chosen_pairs:
        runs.append(
            {
                "name": (
                    f"{prefix}_{from_id}_to_{to_id}_with_{tcp_type}"
                    f"_isl_{isl_data_rate_megabit_per_s}Mbps"
                    f"_gsl_{gsl_data_rate_megabit_per_s}Mbps"
                ),
                "isl_data_rate_megabit_per_s": isl_data_rate_megabit_per_s,
                "gsl_data_rate_megabit_per_s": gsl_data_rate_megabit_per_s,
                "isl_queue_size_pkt": isl_queue_size_pkt,
                "gsl_queue_size_pkt": gsl_queue_size_pkt,
                "satellite_network": full_satellite_network_isls,
                "dynamic_state": dynamic_state,
                "dynamic_state_update_interval_ns": dynamic_state_update_interval_ns,
                "simulation_end_time_ns": simulation_end_time_ns,
                "enable_isl_utilization_tracking": enable_isl_utilization_tracking,
                "isl_utilization_tracking_interval_ns": isl_utilization_tracking_interval_ns,
                "from_id": from_id,
                "to_id": to_id,
                "tcp_socket_type": tcp_type,
                "tcp_initial_slow_start_threshold_byte": (
                    tcp_initial_slow_start_threshold_byte
                ),
                "random_loss_mode": random_loss_mode,
                "random_loss_schedule_filename": random_loss_schedule_filename,
                "random_loss_source_node_id": random_loss_source_node_id,
                "random_loss_target_node_id": random_loss_target_node_id,
                "random_loss_stream": random_loss_stream,
            }
        )
    return runs
