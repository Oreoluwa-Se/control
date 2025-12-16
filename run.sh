#!/usr/bin/env bash
set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"

# -------- Defaults --------
BUILD_DIR="${SCRIPT_DIR}/build"
BUILD_TYPE="Release"                  # Debug|Release|RelWithDebInfo|MinSizeRel
GENERATOR=""                          # Auto-pick Ninja/Unix Makefiles if empty
JOBS=""
SHARED="OFF"                           # FLOWCONTROL_BUILD_SHARED
APP="ON"                              # FLOWCONTROL_BUILD_APP
LIB="ON"                              # FLOWCONTROL_BUILD_LIB
SANITIZERS="OFF"                      # FLOWCONTROL_ENABLE_SANITIZERS
TESTS="OFF"                           # BUILD_TESTING
VERBOSE="OFF"
EXPORT_COMPILE_COMMANDS="ON"
INSTALL_PREFIX=""                     # CMAKE_INSTALL_PREFIX
DO_INSTALL="OFF"
USE_SUDO="OFF"
TARGET=""                             # Specific target to build
RUN_AFTER="OFF"                       # Run flowcontrol_cli after build
EXE_ARGS=""                           # Args for flowcontrol_cli
CMAKE_DEFS=()                         # Extra -D... definitions
CLEAN="OFF"
GDB="OFF"                             # Run under gdb when running executable
GDB_ARGS=""                           # Extra args to gdb (e.g. -ex 'break main')
FOLLOW_BUILD_SHARED_LIBS="OFF"


detect_jobs() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  elif [[ "$(uname -s)" == "Darwin" ]]; then
    sysctl -n hw.ncpu
  else
    echo 4
  fi
}

is_multi_config() {
  # Return 0 (true) if generator is multi-config (Xcode, Ninja Multi-Config, MSVC)
  if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    if grep -qE '^CMAKE_CONFIGURATION_TYPES:' "${BUILD_DIR}/CMakeCache.txt"; then
      return 0
    fi
  fi
  if [[ -n "${GENERATOR}" ]] && [[ "${GENERATOR}" =~ (Xcode|Multi-Config|Visual\ Studio) ]]; then
    return 0
  fi
  return 1
}

usage() {
  cat <<'USAGE'
Usage: ./run.sh [options] [-- -DExtra=Defs ...]

Common:
  -B, --build-dir <dir>        Build directory (default: ./build)
  -t, --type <cfg>             Build type: Debug|Release|RelWithDebInfo|MinSizeRel (default: Release)
  -G, --generator <name>       CMake generator (default: Ninja if available, else Unix Makefiles)
  -j, --jobs <N>               Parallel build jobs (default: auto-detect)
  -T, --target <name>          Build a specific target (e.g., flowcontrol, flowcontrol_cli)

Project options:
      --shared=ON|OFF          Build library as shared or static (default: OFF)
      --app=ON|OFF             Build CLI in src/app (default: ON)
      --lib=ON|OFF             Build the flowcontrol library (default: ON)
      --sanitizers=ON|OFF      Enable ASan/UBSan in Debug (default: OFF)
      --tests=ON|OFF           Build & run tests (default: OFF)
      --export-cc=ON|OFF       CMAKE_EXPORT_COMPILE_COMMANDS (default: ON)
      --shared-default          Set -DBUILD_SHARED_LIBS=ON (lets CMake default shared behavior kick in when top-level)


Install & run:
      --install[=<prefix>]     Run "cmake --install" (optional: set prefix)
      --sudo                   Use sudo for install
  -R, --run                    Run the flowcontrol_cli after build
      --args "<args>"          Arguments to pass to flowcontrol_cli
      --gdb                    Run under gdb (implies -R). Recommended with -t Debug
      --gdb-args "<args>"      Extra arguments passed to gdb (e.g., -ex 'break main')

Other:
  -v, --verbose                Verbose build output
  -C, --clean                  Remove the build directory and exit
  -h, --help                   Show this help


Examples:
  ./run.sh -t Debug --sanitizers=ON -j 8
  ./run.sh --shared=OFF --app=OFF --tests=ON
  ./run.sh --install=/usr/local
  ./run.sh -T flowcontrol_cli -R
  ./run.sh -t Debug --gdb --gdb-args "-ex 'break main'"
USAGE
}

# -------- Parse args --------
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0;;
    -B|--build-dir) BUILD_DIR="$2"; shift 2;;
    -t|--type) BUILD_TYPE="$2"; shift 2;;
    -G|--generator) GENERATOR="$2"; shift 2;;
    -j|--jobs) JOBS="$2"; shift 2;;
    -T|--target) TARGET="$2"; shift 2;;
    -v|--verbose) VERBOSE="ON"; shift;;
    -C|--clean) CLEAN="ON"; shift;;

    --shared=*) SHARED="${1#*=}"; shift;;
    --app=*) APP="${1#*=}"; shift;;
    --lib=*) LIB="${1#*=}"; shift;;
    --sanitizers=*) SANITIZERS="${1#*=}"; shift;;
    --tests=*) TESTS="${1#*=}"; shift;;
    --export-cc=*) EXPORT_COMPILE_COMMANDS="${1#*=}"; shift;;

    --install) DO_INSTALL="ON"; shift;;
    --install=*) DO_INSTALL="ON"; INSTALL_PREFIX="${1#*=}"; shift;;
    --sudo) USE_SUDO="ON"; shift;;

    -R|--run) RUN_AFTER="ON"; shift;;
    --args) EXE_ARGS="$2"; shift 2;;

    --gdb) GDB="ON"; RUN_AFTER="ON"; shift;;
    --gdb-args) GDB_ARGS="$2"; shift 2;;
    --shared-default) FOLLOW_BUILD_SHARED_LIBS="ON"; shift;;


    # Pass-through of arbitrary -D... to CMake
    -D*) CMAKE_DEFS+=("$1"); shift;;

    --) shift; while [[ $# -gt 0 ]]; do CMAKE_DEFS+=("$1"); shift; done; break;;
    *) echo "Unknown option: $1"; usage; exit 1;;
  esac
done

# Defaults for auto values
[[ -z "${JOBS}" ]] && JOBS="$(detect_jobs)"
if [[ -z "${GENERATOR}" ]]; then
  if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
  else
    GENERATOR="Unix Makefiles"
  fi
fi

if [[ "${CLEAN}" == "ON" ]]; then
  echo "[clean] rm -rf ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
  exit 0
fi

mkdir -p "${BUILD_DIR}"

if [[ "${TESTS}" == "ON" && "${LIB}" != "ON" ]]; then
  echo "[warn] --tests=ON requires --lib=ON. Forcing LIB=ON."
  LIB="ON"
fi


# -------- Configure --------
CONFIG_ARGS=(
  -S "${SCRIPT_DIR}"
  -B "${BUILD_DIR}"
  -G "${GENERATOR}"
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
  -DFLOWCONTROL_BUILD_LIB="${LIB}"
  -DFLOWCONTROL_BUILD_SHARED="${SHARED}"
  -DFLOWCONTROL_BUILD_APP="${APP}"
  -DFLOWCONTROL_ENABLE_SANITIZERS="${SANITIZERS}"
  -DBUILD_TESTING="${TESTS}"
  -DCMAKE_EXPORT_COMPILE_COMMANDS="${EXPORT_COMPILE_COMMANDS}"
)

if [[ -n "${INSTALL_PREFIX}" ]]; then
  CONFIG_ARGS+=(-DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}")
fi

# Append user -D… args
CONFIG_ARGS+=("${CMAKE_DEFS[@]}")

if [[ "${FOLLOW_BUILD_SHARED_LIBS}" == "ON" ]]; then
  CONFIG_ARGS+=(-DBUILD_SHARED_LIBS=ON)
fi

echo "[configure] cmake ${CONFIG_ARGS[*]}"
cmake "${CONFIG_ARGS[@]}"

# -------- Build --------
BUILD_ARGS=(--build "${BUILD_DIR}" --parallel "${JOBS}")
is_multi_config && BUILD_ARGS+=(--config "${BUILD_TYPE}")
[[ "${VERBOSE}" == "ON" ]] && BUILD_ARGS+=(--verbose)

if [[ -n "${TARGET}" ]]; then
  BUILD_ARGS+=(--target "${TARGET}")
fi

echo "[build] cmake ${BUILD_ARGS[*]}"
if [[ "${VERBOSE}" == "ON" ]]; then
  cmake "${BUILD_ARGS[@]}"
else
  cmake "${BUILD_ARGS[@]}" >/dev/null
fi

# -------- Tests --------
if [[ "${TESTS}" == "ON" ]]; then
  echo "[test] ctest"
  CTEST_ARGS=(--test-dir "${BUILD_DIR}" -j "${JOBS}" --output-on-failure)
  is_multi_config && CTEST_ARGS+=(-C "${BUILD_TYPE}")
  ctest "${CTEST_ARGS[@]}"
fi

# -------- Install --------
if [[ "${DO_INSTALL}" == "ON" ]]; then
  INSTALL_ARGS=(--install "${BUILD_DIR}")
  is_multi_config && INSTALL_ARGS+=(--config "${BUILD_TYPE}")
  echo "[install] cmake ${INSTALL_ARGS[*]}"
  if [[ "${USE_SUDO}" == "ON" ]]; then
    sudo cmake "${INSTALL_ARGS[@]}"
  else
    cmake "${INSTALL_ARGS[@]}"
  fi
fi

# -------- Run CLI (if requested) --------
if [[ "${RUN_AFTER}" == "ON" ]]; then
  EXE_NAME="flowcontrol_cli"
  EXE_PATHS=(
    "${BUILD_DIR}/${EXE_NAME}"
    "${BUILD_DIR}/${BUILD_TYPE}/${EXE_NAME}"
    "${BUILD_DIR}/bin/${EXE_NAME}"
    "${BUILD_DIR}/${BUILD_TYPE}/bin/${EXE_NAME}"
  )

  FOUND=""
  for p in "${EXE_PATHS[@]}"; do
    if [[ -x "$p" ]]; then FOUND="$p"; break; fi
  done

  if [[ -z "${FOUND}" ]]; then
    echo "[run] Could not find built executable '${EXE_NAME}'. Did you build with --app=ON ?"
    exit 1
  fi

  if [[ "${GDB}" == "ON" ]]; then
    if [[ "${BUILD_TYPE}" != "Debug" ]] && ! is_multi_config; then
      echo "[gdb] Note: BUILD_TYPE='${BUILD_TYPE}'. For best results use -t Debug."
    fi
    if command -v gdb >/dev/null 2>&1; then
      echo "[gdb] gdb ${GDB_ARGS} --args ${FOUND} ${EXE_ARGS}"
      exec gdb ${GDB_ARGS} --args "${FOUND}" ${EXE_ARGS}
    elif [[ "$(uname -s)" == "Darwin" ]] && command -v lldb >/dev/null 2>&1; then
      echo "[gdb] gdb not found; falling back to lldb"
      exec lldb ${GDB_ARGS} -- "${FOUND}" ${EXE_ARGS}
    else
      echo "[gdb] gdb not found on PATH. Install gdb or omit --gdb."
      exit 1
    fi
  fi

  echo "[run] ${FOUND} ${EXE_ARGS}"
  exec "${FOUND}" ${EXE_ARGS}
fi

echo "[done] Build complete in: ${BUILD_DIR}"

