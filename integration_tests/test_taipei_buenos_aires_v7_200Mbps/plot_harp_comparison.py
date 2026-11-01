"""把 CUBIC、HARP 6.0 與 HARP 7.0 疊在同一張圖。"""

import os
import subprocess
import sys

PLOTS = [
    (
        "tcp_flow_0_cwnd.csv",
        "comparison_cwnd.pdf",
        "Congestion window (segments)",
        "($3 / 1380.0)",
    ),
    (
        "tcp_flow_0_progress.csv",
        "comparison_progress.pdf",
        "Progress (Mbit)",
        "($3 * 8.0 / 1000000.0)",
    ),
    ("tcp_flow_0_rate_in_intervals.csv", "comparison_rate.pdf", "Rate (Mbit/s)", "($3)"),
    ("tcp_flow_0_rtt.csv", "comparison_rtt.pdf", "RTT (ms)", "($3 / 1000000.0)"),
]


def quote(value):
    return value.replace("\\", "\\\\").replace("'", "\\'")


def generate_plot(data_dirs, output_file, ylabel, expression):
    colors = ("#1f77b4", "#d62728", "#9467bd")
    labels = ("TCP CUBIC", "HARP-Cubic 6.0", "HARP-Cubic 7.0")
    plots = []
    for data_dir, color, label in zip(data_dirs, colors, labels):
        plots.append(
            "'%s' using ($2 / 1000000000.0):%s with lines linewidth 1.5 "
            "linecolor rgb '%s' title '%s'"
            % (quote(data_dir), expression, color, label)
        )

    script = "\n".join(
        [
            "set terminal pdfcairo enhanced color size 10,5.5",
            "set output '%s'" % quote(output_file),
            "set title 'TCP CUBIC vs HARP-Cubic 6.0 vs 7.0 (ISL 200 Mbps, DATA loss 0.5%)'",
            "set xlabel 'Simulation time (s)'",
            "set ylabel '%s'" % ylabel,
            "set datafile separator ','",
            "set grid",
            "set key outside top center horizontal",
            "plot " + ", ".join(plots),
        ]
    )
    subprocess.run(["gnuplot"], input=script, text=True, check=True)


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "Usage: plot_harp_comparison.py CUBIC_DIR V6_DIR V7_DIR OUTPUT_DIR"
        )

    input_dirs = sys.argv[1:4]
    output_dir = sys.argv[4]
    os.makedirs(output_dir, exist_ok=True)
    for filename, output_name, ylabel, expression in PLOTS:
        generate_plot(
            [os.path.join(path, filename) for path in input_dirs],
            os.path.join(output_dir, output_name),
            ylabel,
            expression,
        )


if __name__ == "__main__":
    main()
