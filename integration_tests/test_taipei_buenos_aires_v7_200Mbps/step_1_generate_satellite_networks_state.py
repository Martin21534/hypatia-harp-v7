"""產生 Taipei→Buenos Aires 共用的 Starlink-550 forwarding state。"""

import os
import sys

import exputil

sys.path.append("../../satgenpy")
sys.path.append("../../paper/satellite_networks_state")

import main_starlink_550
import satgen

from run_list import (
    dynamic_state_update_interval_ms,
    full_satellite_network_isls,
    simulation_end_time_s,
)

local_shell = exputil.LocalShell()
output_generated_data_dir = "temp/gen_data"
num_threads = 14

local_shell.remove_force_recursive(output_generated_data_dir)
local_shell.make_full_dir(output_generated_data_dir)

network_dir = output_generated_data_dir + "/" + full_satellite_network_isls
os.makedirs(network_dir, exist_ok=True)

print("Generating ground stations...")
with open(network_dir + "/ground_stations.basic.txt", "w+", encoding="utf-8") as output:
    output.write("0,Taipei,25.047009,121.54568,0\n")
    output.write("1,Buenos-Aires,-34.605083,-58.400368,0\n")

satgen.extend_ground_stations(
    network_dir + "/ground_stations.basic.txt",
    network_dir + "/ground_stations.txt",
)

print("Generating TLEs...")
satgen.generate_tles_from_scratch_manual(
    network_dir + "/tles.txt",
    main_starlink_550.NICE_NAME,
    main_starlink_550.NUM_ORBS,
    main_starlink_550.NUM_SATS_PER_ORB,
    main_starlink_550.PHASE_DIFF,
    main_starlink_550.INCLINATION_DEGREE,
    main_starlink_550.ECCENTRICITY,
    main_starlink_550.ARG_OF_PERIGEE_DEGREE,
    main_starlink_550.MEAN_MOTION_REV_PER_DAY,
)

print("Generating ISLs...")
satgen.generate_plus_grid_isls(
    network_dir + "/isls.txt",
    main_starlink_550.NUM_ORBS,
    main_starlink_550.NUM_SATS_PER_ORB,
    isl_shift=0,
    idx_offset=0,
)

print("Generating description...")
satgen.generate_description(
    network_dir + "/description.txt",
    main_starlink_550.MAX_GSL_LENGTH_M,
    main_starlink_550.MAX_ISL_LENGTH_M,
)

ground_stations = satgen.read_ground_stations_extended(
    network_dir + "/ground_stations.txt"
)
num_satellites = main_starlink_550.NUM_ORBS * main_starlink_550.NUM_SATS_PER_ORB

print("Generating GSL interfaces info...")
satgen.generate_simple_gsl_interfaces_info(
    network_dir + "/gsl_interfaces_info.txt",
    num_satellites,
    len(ground_stations),
    1,
    1,
    1,
    1,
)

print("Generating forwarding state...")
satgen.help_dynamic_state(
    output_generated_data_dir,
    num_threads,
    full_satellite_network_isls,
    dynamic_state_update_interval_ms,
    simulation_end_time_s,
    main_starlink_550.MAX_GSL_LENGTH_M,
    main_starlink_550.MAX_ISL_LENGTH_M,
    "algorithm_free_one_only_over_isls",
    True,
)

print("Success: generated Taipei-Buenos Aires satellite network state")
