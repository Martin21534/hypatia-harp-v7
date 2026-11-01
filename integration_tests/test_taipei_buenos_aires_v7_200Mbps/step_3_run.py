"""執行相同 random-loss 下的 CUBIC、HARP 6.0 與 HARP 7.0。"""

import time

import exputil

from run_list import get_tcp_run_list

local_shell = exputil.LocalShell()
max_num_processes = 2

if local_shell.count_screens() != 0:
    print("There is a screen already running. Please finish it before this test.")
    raise SystemExit(1)


def harp_v6_arguments():
    """V6/V7 共用 HARP 6.0 policy；CLI 使用屬性的定義 TypeId。"""
    return (
        " --ns3::TcpHarpCubicV6::EnableAdaptiveBeta=true"
        " --ns3::TcpHarpCubicV6::HandoverWindow=250ms"
        " --ns3::TcpHarpCubicV6::DirectionMergeWindow=1ms"
        " --ns3::TcpHarpCubicV6::NormalBeta=0.70"
        " --ns3::TcpHarpCubicV6::HandoverBeta=0.85"
        " --ns3::TcpHarpCubicV6::GapThresholdFloor=7"
        " --ns3::TcpHarpCubicV6::GapMargin=3"
        " --ns3::TcpHarpCubicV6::GapMadMultiplier=3.0"
        " --ns3::TcpHarpCubicV6::GapBaselineWindow=9"
        " --ns3::TcpHarpCubicV6::GapBaselineMinSamples=3"
    )


def wire_signal_arguments():
    """三組 TCP 共用的 on-wire handover mark/echo 條件。"""
    return (
        " --ns3::ArbiterSingleForward::EnableHarpV5PacketMarking=true"
        " --ns3::ArbiterSingleForward::HarpV5MarkDuration=250ms"
        " --ns3::TcpSocketBase::HarpV5EchoDuration=250ms"
    )


commands = []
for run in get_tcp_run_list():
    logs_dir = "temp/runs/" + run["name"] + "/logs_ns3"
    local_shell.remove_force_recursive(logs_dir)
    local_shell.make_full_dir(logs_dir)

    # 三種 TCP 都使用 PRR；random-loss 設定已固定在各自且相同的 config 中。
    arguments = (
        " --ns3::TcpL4Protocol::RecoveryType=ns3::TcpPrrRecovery"
        + wire_signal_arguments()
    )
    log_prefix = 'NS_LOG="ArbiterSingleForward=level_info|prefix_time" '
    if run["tcp_socket_type"] == "TcpHarpCubicV6":
        arguments += harp_v6_arguments()
        log_prefix = (
            'NS_LOG="TcpHarpCubicV6=level_info|prefix_time:'
            'ArbiterSingleForward=level_info|prefix_time" '
        )
    elif run["tcp_socket_type"] == "TcpHarpCubicV7":
        arguments += harp_v6_arguments()
        arguments += (
            " --ns3::TcpHarpCubicV7::MonitorInterval=100ms"
            " --ns3::TcpHarpCubicV7::WarmupIntervals=5"
            " --ns3::TcpHarpCubicV7::AssistEnabled=true"
            " --ns3::TcpHarpCubicV7::RandomThreshold=0.00025"
            " --ns3::TcpHarpCubicV7::RandomBeta=0.95"
            " --ns3::TcpHarpCubicV7::V2BandwidthWindow=5s"
            " --ns3::TcpHarpCubicV7::V2BandwidthQuantile=0.90"
        )
        # V6 component 記錄 handover policy；V7 component 記錄 ML normal-loss 決策。
        log_prefix = (
            'NS_LOG="TcpHarpCubicV7=level_info|prefix_time:'
            'TcpHarpCubicV6=level_info|prefix_time:'
            'ArbiterSingleForward=level_info|prefix_time" '
        )

    simulation_command = (
        "cd ../../ns3-sat-sim/simulator; "
        + log_prefix
        + './ns3 run "main_satnet --run_dir=../../integration_tests/'
        + "test_taipei_buenos_aires_v7_200Mbps/temp/runs/"
        + run["name"]
        + arguments
        + '" 2>&1 | tee "../../integration_tests/'
        + "test_taipei_buenos_aires_v7_200Mbps/"
        + logs_dir
        + '/console.txt"'
    )
    status_path = (
        "../../integration_tests/"
        + "test_taipei_buenos_aires_v7_200Mbps/"
        + logs_dir
        + "/simulation_exit_code.txt"
    )
    command = (
        "bash -o pipefail -c '"
        + simulation_command
        + "' && echo 0 > '"
        + status_path
        + "' || echo 1 > '"
        + status_path
        + "'"
    )
    commands.append(command)

print(f"Running commands (at most {max_num_processes} in parallel)...")
for index, command in enumerate(commands, start=1):
    print(f"Starting command {index} out of {len(commands)}: {command}")
    local_shell.detached_exec(command)
    while local_shell.count_screens() >= max_num_processes:
        time.sleep(2)

print("Waiting for remaining simulations...")
while local_shell.count_screens() > 0:
    time.sleep(2)

failed_runs = []
for run in get_tcp_run_list():
    status_path = "temp/runs/" + run["name"] + "/logs_ns3/simulation_exit_code.txt"
    if (
        not local_shell.file_exists(status_path)
        or local_shell.read_file(status_path).strip() != "0"
    ):
        failed_runs.append(run["name"])

if failed_runs:
    raise RuntimeError("Simulation failed or was interrupted: " + ", ".join(failed_runs))

print("Finished.")
