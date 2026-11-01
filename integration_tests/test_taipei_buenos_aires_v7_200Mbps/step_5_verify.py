"""確認三組輸出、相同的排程式 GSL loss、V7 決策 log 與比較圖。"""

import csv
from pathlib import Path

import exputil

from run_list import (
    dynamic_state,
    dynamic_state_update_interval_ns,
    full_satellite_network_isls,
    get_tcp_run_list,
    simulation_end_time_ns,
)

local_shell = exputil.LocalShell()
test_dir = Path(__file__).resolve().parent


def require(path):
    if not local_shell.file_exists(path):
        raise ValueError("Missing expected file: " + path)


def read_properties(path):
    values = {}
    for raw_line in Path(path).read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip().strip('"')
    return values


def read_loss_schedule(path):
    with Path(path).open(newline="", encoding="utf-8") as schedule_file:
        reader = csv.DictReader(schedule_file)
        expected_fields = [
            "start_ns",
            "end_ns",
            "link_direction",
            "rate",
            "label",
        ]
        if reader.fieldnames != expected_fields:
            raise ValueError(f"Unexpected loss schedule columns in {path}")
        rows = [
            (
                int(row["start_ns"]),
                int(row["end_ns"]),
                row["link_direction"],
                float(row["rate"]),
                row["label"],
            )
            for row in reader
        ]
    if not rows:
        raise ValueError("GSL random-loss schedule cannot be empty: " + str(path))
    return rows


def verify_drop_log(path, schedule):
    windows_by_label = {row[4]: row for row in schedule}
    if len(windows_by_label) != len(schedule):
        raise ValueError("GSL random-loss schedule labels are not unique")

    seen_directions = set()
    with Path(path).open(newline="", encoding="utf-8") as drop_file:
        reader = csv.DictReader(drop_file)
        expected_fields = [
            "time_ns",
            "packet_uid",
            "link_direction",
            "window_label",
            "rate",
        ]
        if reader.fieldnames != expected_fields:
            raise ValueError(f"Unexpected GSL drop-log columns in {path}")

        for row in reader:
            time_ns = int(row["time_ns"])
            direction = row["link_direction"]
            label = row["window_label"]
            rate = float(row["rate"])
            if label not in windows_by_label:
                raise ValueError(f"Unknown loss-window label in {path}: {label}")

            start_ns, end_ns, expected_direction, expected_rate, _ = (
                windows_by_label[label]
            )
            if direction != expected_direction:
                raise ValueError(
                    f"Drop direction does not match {label} in {path}: {direction}"
                )
            if not start_ns <= time_ns < end_ns:
                raise ValueError(
                    f"Drop at {time_ns} ns is outside half-open window {label}"
                )
            if abs(rate - expected_rate) > 1e-12:
                raise ValueError(f"Drop rate does not match {label} in {path}: {rate}")
            if any(
                handover_ns - HANDOVER_GUARD_NS
                <= time_ns
                < handover_ns + HANDOVER_GUARD_NS
                for handover_ns in HANDOVER_TIMES_NS
            ):
                raise ValueError(
                    f"Drop at {time_ns} ns falls inside a handover guard in {path}"
                )
            seen_directions.add(direction)

    if seen_directions != {"uplink", "downlink"}:
        raise ValueError(
            f"Both uplink and downlink must have actual drops in {path}; "
            f"found {sorted(seen_directions)}"
        )


HANDOVER_TIMES_NS = (
    39_400_000_000,
    63_800_000_000,
    73_600_000_000,
    149_200_000_000,
    165_100_000_000,
    166_600_000_000,
    191_600_000_000,
)
HANDOVER_GUARD_NS = 1_000_000_000


network_dir = "temp/gen_data/" + full_satellite_network_isls
for name in (
    "description.txt",
    "ground_stations.txt",
    "gsl_interfaces_info.txt",
    "isls.txt",
    "tles.txt",
):
    require(network_dir + "/" + name)

state_dir = network_dir + "/" + dynamic_state
for time_ns in range(0, simulation_end_time_ns, dynamic_state_update_interval_ns):
    require(state_dir + "/fstate_" + str(time_ns) + ".txt")
    require(state_dir + "/gsl_if_bandwidth_" + str(time_ns) + ".txt")

runs = get_tcp_run_list()
random_keys = (
    "enable_gsl_random_loss",
    "gsl_random_loss_mode",
    "gsl_random_loss_schedule_filename",
    "gsl_random_loss_source_node_id",
    "gsl_random_loss_target_node_id",
    "gsl_random_loss_stream",
)
template_schedule = read_loss_schedule(
    test_dir / "templates/template_gsl_random_loss_schedule.csv"
)
random_profiles = []
for run in runs:
    run_name = run["name"]
    run_root = "temp/runs/" + run_name
    logs_dir = run_root + "/logs_ns3"
    finished = logs_dir + "/finished.txt"
    require(finished)
    if local_shell.read_file(finished).strip() != "Yes":
        raise ValueError("Simulation did not finish: " + run_name)

    properties = read_properties(run_root + "/config_ns3.properties")
    missing_keys = [key for key in random_keys if key not in properties]
    if missing_keys:
        raise ValueError(f"Missing random-loss settings in {run_name}: {missing_keys}")
    random_profile = tuple(properties[key] for key in random_keys)
    random_profiles.append(random_profile)

    expected_profile = (
        "true",
        run["random_loss_mode"],
        run["random_loss_schedule_filename"],
        str(run["random_loss_source_node_id"]),
        str(run["random_loss_target_node_id"]),
        str(run["random_loss_stream"]),
    )
    if random_profile != expected_profile:
        raise ValueError(
            f"Unexpected random-loss profile in {run_name}: {random_profile}"
        )

    run_schedule_path = run_root + "/" + properties[
        "gsl_random_loss_schedule_filename"
    ]
    require(run_schedule_path)
    run_schedule = read_loss_schedule(run_schedule_path)
    if run_schedule != template_schedule:
        raise ValueError(f"Random-loss schedule differs in {run_name}")

    drop_log = logs_dir + "/gsl_random_loss_drops.csv"
    require(drop_log)
    verify_drop_log(drop_log, run_schedule)

    for name in ("cwnd", "progress", "rate_in_intervals", "rtt"):
        require("temp/data/" + run_name + "/tcp_flow_0_" + name + ".csv")

    if run["tcp_socket_type"] == "TcpHarpCubicV7":
        console = Path(logs_dir + "/console.txt").read_text(
            encoding="utf-8", errors="replace"
        )
        ml_decisions = [
            line for line in console.splitlines() if "HarpV7MlDecision" in line
        ]
        if not ml_decisions:
            raise ValueError("V7 run did not emit any HarpV7MlDecision log")
        if not any("control_ready=1" in line for line in ml_decisions):
            raise ValueError("V7 model never became ready after warm-up")
        if not any("assisted=1" in line for line in ml_decisions):
            raise ValueError("V7 active run never applied random-loss assistance")
        if "HarpV6Decision" not in console:
            raise ValueError("V7 run did not retain the HARP 6.0 decision path")

if len(set(random_profiles)) != 1:
    raise ValueError("CUBIC/V6/V7 random-loss profiles differ")

for name in ("cwnd", "progress", "rate", "rtt"):
    require("temp/pdf/comparison/comparison_" + name + ".pdf")

print(
    "Verification completed: shared alternating GSL loss, actual uplink/downlink "
    "drops outside handovers, warm ML, assist, and V6 path found"
)
