NS3_VERSION="ns-3.45"

apply_patch_once() {
  target_dir="$1"
  patch_file="$2"
  label="$3"

  if [ ! -f "${patch_file}" ]; then
    echo "Missing required patch: ${patch_file}" >&2
    exit 1
  fi

  echo "Applying ${label}"
  if patch --dry-run --forward --batch --silent -d "${target_dir}" -p1 < "${patch_file}"; then
    patch --forward --batch -d "${target_dir}" -p1 < "${patch_file}" || exit 1
  elif patch --dry-run --reverse --batch --silent -d "${target_dir}" -p1 < "${patch_file}"; then
    echo "${label} is already applied"
  else
    echo "${label} is incompatible with ${target_dir}" >&2
    exit 1
  fi
}

# Usage help
if [ "$1" == "--help" ]; then
  echo "Usage: bash build.sh [--help, --debug_all, --debug_minimal, --optimized, --optimized_with_tests]"
  exit 0
fi

# Extract copy of ns-3
echo "Unzipping clean ns-3 (no overwrites)"
tar jxvf ${NS3_VERSION}.tar.bz2 || exit 1
cp -r ${NS3_VERSION}/* simulator/ || exit 1
rm -r ${NS3_VERSION} || exit 1

# Reconstruct the ns-3 core extensions that are intentionally not tracked under
# simulator/src/.  patch(1) is used because simulator/ is a subdirectory of the
# parent Hypatia repository and git apply can silently skip these paths.
apply_patch_once simulator \
  patches/ns-3.45-tcp-cubic-loss-hook.patch \
  "ML-CUBIC TcpCubic loss hook"
apply_patch_once simulator \
  patches/ns-3.45-harp-core.patch \
  "HARP TCP core support"

cd simulator || exit 1

# Recreate the per-flow congestion-control selection inside the pinned
# basic-sim submodule.  Keeping this as a parent-repository patch avoids relying
# on an unpublished dirty submodule checkout.
echo "Updating git submodules"
git submodule update --init --recursive || exit 1
apply_patch_once contrib/basic-sim \
  ../patches/basic-sim-per-flow-tcp.patch \
  "basic-sim per-flow TCP selection"

source ../env/bin/activate || echo "pass"

# Configure the build
if [ "$1" == "--debug_all" ]; then
  ./ns3 configure --build-profile=debug --enable-mpi --enable-examples --enable-tests --enable-gcov --out=build/debug_all || exit 1

elif [ "$1" == "--debug_minimal" ]; then
  ./ns3 configure --build-profile=debug --enable-mpi --out=build/debug_minimal || exit 1

elif [ "$1" == "--optimized" ]; then
  ./ns3 configure --build-profile=optimized --enable-mpi --out=build/optimized || exit 1

elif [ "$1" == "--optimized_with_tests" ]; then
  ./ns3 configure --build-profile=optimized --enable-mpi --enable-tests --out=build/optimized_with_tests || exit 1

elif [ "$1" == "" ]; then
  # Default is debug_all
  ./ns3 configure --build-profile=debug --enable-mpi --enable-examples --enable-tests --enable-gcov --out=build/debug_all || exit 1

else
  echo "Invalid build option: $1"
  echo "Usage: bash build.sh [--debug_all, --debug_minimal, --optimized, --optimized_with_tests]"
  exit 1
fi

# Perform the build
./ns3 -j4 || exit 1
