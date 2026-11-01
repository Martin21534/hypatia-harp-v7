"""從三組 ns-3 log 產生 CSV、單組圖與比較圖。"""

import exputil

from run_list import get_tcp_run_list

local_shell = exputil.LocalShell()
local_shell.remove_force_recursive("temp/pdf")
local_shell.remove_force_recursive("temp/data")
local_shell.make_full_dir("temp/pdf")
local_shell.make_full_dir("temp/data")

runs = get_tcp_run_list()
for run in runs:
    local_shell.make_full_dir("temp/pdf/" + run["name"])
    local_shell.make_full_dir("temp/data/" + run["name"])
    local_shell.perfect_exec(
        "cd ../../ns3-sat-sim/simulator/contrib/basic-sim/tools/plotting/plot_tcp_flow; "
        "python plot_tcp_flow.py "
        "../../../../../../../integration_tests/test_taipei_buenos_aires_v7_200Mbps/"
        "temp/runs/" + run["name"] + "/logs_ns3 "
        "../../../../../../../integration_tests/test_taipei_buenos_aires_v7_200Mbps/"
        "temp/data/" + run["name"] + " "
        "../../../../../../../integration_tests/test_taipei_buenos_aires_v7_200Mbps/"
        "temp/pdf/" + run["name"] + " 0 " + str(100 * 1000 * 1000),
        output_redirect=exputil.OutputRedirect.CONSOLE,
    )

by_type = {run["tcp_socket_type"]: run for run in runs}
local_shell.make_full_dir("temp/pdf/comparison")
local_shell.perfect_exec(
    "python3 plot_harp_comparison.py "
    + " ".join(
        "temp/data/" + by_type[tcp_type]["name"]
        for tcp_type in ("TcpCubic", "TcpHarpCubicV6", "TcpHarpCubicV7")
    )
    + " temp/pdf/comparison",
    output_redirect=exputil.OutputRedirect.CONSOLE,
)
