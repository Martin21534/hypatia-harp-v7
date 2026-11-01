# Low earth orbit satellite network simulation using ns-3

Hypatia makes use of ns-3 to simulate the satellite networks at packet-level
granularity. It builds upon two ns-3 modules:

* `satellite` : Satellite movement calculation using SGP-4. This module is used
  by Hypatia to calculate the channel delay for each packet which traverses
  either a GSl or ISL. It was written by Pedro Silva at INESC-TEC. It can
  be found at:
  
  https://gitlab.inesctec.pt/pmms/ns3-satellite
  
  A copy of it is included in this repository with minor modifications.
  It is located at: simulator/src/satellite

* `basic-sim` : Simulation framework to make e.g., running end-to-end 
  TCP flows more easier. It can be found at:
  
  https://github.com/snkas/basic-sim
  
  It is added as git submodule (commit: f67ce509) at: simulator/contrib/basic-sim
  
  The git submodule refers to:
  
  https://github.com/snkas/basic-sim/tree/f67ce5092cda7d294922ff3270e8e1ea23d26622


## Getting started

1. Install dependencies (inherited from `basic-sim` ns-3 module):
   ```
   sudo apt-get update
   sudo apt-get -y install openmpi-bin openmpi-common openmpi-doc libopenmpi-dev lcov gnuplot
   pip install numpy statsmodels
   pip install git+https://github.com/snkas/exputilpy.git@v1.6
   git submodule update --init --recursive
   ```

2. Build optimized:
   ```
   bash build.sh --optimized
   ```

`build.sh` extracts ns-3.45 and automatically applies the CUBIC extension
hook, the HARP TCP core integration, and the per-flow TCP selection patch for
the pinned `basic-sim` submodule. Patch application is idempotent, so a normal
rebuild does not require manual `git apply` commands.

To verify the HARP/ML targets after a debug build, run from `simulator/`:

```
./ns3 build main_satnet ml-cubic-inference-check test-runner
./ns3 run "ml-cubic-inference-check --v2=1"
./ns3 run "test-runner --suite=tcp-harp-cubic-v6 --verbose"
./ns3 run "test-runner --suite=tcp-harp-cubic-v7 --verbose"
```
