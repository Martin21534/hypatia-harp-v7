#!/usr/bin/env python3
"""不執行模擬，驗證 V7 比較情境與交替 GSL loss 排程。"""

import csv
from pathlib import Path

from run_list import get_tcp_run_list


test_dir = Path(__file__).resolve().parent
runs = get_tcp_run_list()
expected_types = ["TcpCubic", "TcpHarpCubicV6", "TcpHarpCubicV7"]
actual_types = [run["tcp_socket_type"] for run in runs]
if actual_types != expected_types:
    raise ValueError(f"Unexpected TCP variants: {actual_types}")

if len({run["name"] for run in runs}) != len(runs):
    raise ValueError("Run names must be unique")

shared_keys = (
    "isl_data_rate_megabit_per_s",
    "gsl_data_rate_megabit_per_s",
    "isl_queue_size_pkt",
    "gsl_queue_size_pkt",
    "satellite_network",
    "dynamic_state",
    "simulation_end_time_ns",
    "from_id",
    "to_id",
    "tcp_initial_slow_start_threshold_byte",
    "random_loss_mode",
    "random_loss_schedule_filename",
    "random_loss_source_node_id",
    "random_loss_target_node_id",
    "random_loss_stream",
)
for key in shared_keys:
    values = {str(run[key]) for run in runs}
    if len(values) != 1:
        raise ValueError(f"Runs do not share {key}: {sorted(values)}")

if runs[0]["random_loss_mode"] != "scheduled":
    raise ValueError("V7 comparison must use scheduled GSL random loss")
for run in runs:
    if run["random_loss_source_node_id"] != run["from_id"]:
        raise ValueError("Scheduled GSL loss source must match the TCP source")
    if run["random_loss_target_node_id"] != run["to_id"]:
        raise ValueError("Scheduled GSL loss target must match the TCP target")

simulation_end_time_ns = int(runs[0]["simulation_end_time_ns"])

template = (test_dir / "templates/template_tcp_a_b_config_ns3.properties").read_text(
    encoding="utf-8"
)
required_config_keys = (
    "enable_gsl_random_loss=true",
    "gsl_random_loss_mode=[GSL-RANDOM-LOSS-MODE]",
    'gsl_random_loss_schedule_filename="[GSL-RANDOM-LOSS-SCHEDULE-FILENAME]"',
    "gsl_random_loss_source_node_id=[GSL-RANDOM-LOSS-SOURCE-NODE-ID]",
    "gsl_random_loss_target_node_id=[GSL-RANDOM-LOSS-TARGET-NODE-ID]",
    "gsl_random_loss_stream=[GSL-RANDOM-LOSS-STREAM]",
)
for key in required_config_keys:
    if key not in template:
        raise ValueError("Missing template setting: " + key)

schedule_path = test_dir / "templates/template_gsl_random_loss_schedule.csv"
with schedule_path.open(newline="", encoding="utf-8") as schedule_file:
    schedule_reader = csv.DictReader(schedule_file)
    expected_fields = [
        "start_ns",
        "end_ns",
        "link_direction",
        "rate",
        "label",
    ]
    if schedule_reader.fieldnames != expected_fields:
        raise ValueError("Unexpected GSL random-loss schedule columns")
    schedule = list(schedule_reader)

if not schedule:
    raise ValueError("GSL random-loss schedule cannot be empty")

handover_times_ns = (
    39_400_000_000,
    63_800_000_000,
    73_600_000_000,
    149_200_000_000,
    165_100_000_000,
    166_600_000_000,
    191_600_000_000,
)
handover_guard_ns = 1_000_000_000
previous_end_ns = -1
previous_direction = None
labels = set()
seen_directions = set()
for row in schedule:
    start_ns = int(row["start_ns"])
    end_ns = int(row["end_ns"])
    direction = row["link_direction"]
    rate = float(row["rate"])
    label = row["label"]

    if start_ns < 0 or end_ns <= start_ns or end_ns > simulation_end_time_ns:
        raise ValueError(f"Invalid GSL loss window: {row}")
    if start_ns < previous_end_ns:
        raise ValueError("GSL uplink/downlink loss windows must not overlap")
    if direction not in {"uplink", "downlink"}:
        raise ValueError(f"Invalid GSL link direction: {direction}")
    if direction == previous_direction:
        raise ValueError("GSL loss windows must alternate uplink and downlink")
    if rate != 0.005:
        raise ValueError(f"Expected 0.5% loss inside each window, got {rate}")
    if not label or label in labels:
        raise ValueError(f"GSL loss labels must be unique: {label}")
    if any(
        start_ns < handover_ns + handover_guard_ns
        and end_ns > handover_ns - handover_guard_ns
        for handover_ns in handover_times_ns
    ):
        raise ValueError(f"GSL loss window overlaps a handover guard: {row}")

    previous_end_ns = end_ns
    previous_direction = direction
    labels.add(label)
    seen_directions.add(direction)

if seen_directions != {"uplink", "downlink"}:
    raise ValueError("Schedule must exercise both GSL uplink and downlink")

runner = (test_dir / "step_3_run.py").read_text(encoding="utf-8")
for setting in (
    "ArbiterSingleForward::EnableHarpV5PacketMarking=true",
    "ArbiterSingleForward::HarpV5MarkDuration=250ms",
    "TcpSocketBase::HarpV5EchoDuration=250ms",
    "TcpHarpCubicV6::EnableAdaptiveBeta=true",
    "TcpHarpCubicV6::NormalBeta=0.70",
    "TcpHarpCubicV6::HandoverBeta=0.85",
    "TcpHarpCubicV7::MonitorInterval=100ms",
    "TcpHarpCubicV7::WarmupIntervals=5",
    "TcpHarpCubicV7::AssistEnabled=true",
    "TcpHarpCubicV7::RandomThreshold=0.00025",
    "TcpHarpCubicV7::RandomBeta=0.95",
    "TcpHarpCubicV7::V2BandwidthWindow=5s",
    "TcpHarpCubicV7::V2BandwidthQuantile=0.90",
):
    if setting not in runner:
        raise ValueError("Missing V7 runner setting: " + setting)

if "+ wire_signal_arguments()" not in runner:
    raise ValueError("CUBIC/V6/V7 must all use the common on-wire HARP signal")

for inherited_name in ("EnableAdaptiveBeta", "NormalBeta", "HandoverBeta"):
    invalid = "TcpHarpCubicV7::" + inherited_name
    if invalid in runner:
        raise ValueError("Inherited CLI attribute must use its V6 TypeId: " + invalid)

print(
    "Validation passed: CUBIC/V6/V7 share alternating uplink/downlink loss "
    "windows that avoid handovers"
)
