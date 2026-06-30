#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  ./run_cuckoo_server_sweep.sh [MODE] [--target-fp FP|min_fp] [-target-fp FP|min_fp] [--calibration-server-100] [--calibration-client-percent=N|max] [extra ./test args...]

Modes:
  calibrate              Run direct Cuckoo calibration first, then measurements.
  calibrate_only         Run calibration and stop after writing selected params.
  run_and_calib          Re-measure paper rows, recalibrate misses, then rerun.

Default mode:
  Uses PARAM_TABLE values exactly. Pass calibrate/calibrate_only to generate
  a fresh calibration table.

Important environment variables:
  SERVER_SIZES            Space-separated Server sizes for the sweep.
  CLIENT_SIZES              Space-separated Client sizes for the sweep.
  PARAM_TABLE            Semicolon CSV with server_size,L,k,w,filter_size.
  SMALL_DB_PRESET        DB preset for Server <= SMALL_PARAM_MAX_SERVER.
  BIG_DB_PRESET          DB preset for larger Server sizes.
  OPRF_MECHANISM         GCAES or ECNR. Default: GCAES.
  OPRF_ADDR              HOST:PORT for interactive OPRF. Default: 127.0.0.1:50051.
  PIR_MODE               BatchPIR or PIR_double. Default: BatchPIR.
  NUM_RUNS               Number of ./test repetitions per point. Default: 1.
  --target-fp FP         Select a paper calibration table for FP target:
                         <=0.015, <=0.035, or <=0.055. Example: 0.03 loads the 3%/1% table.
  --target-fp min_fp     Select the minimum-possible-error paper calibration table.
  -target-fp min_fp      Alias for --target-fp min_fp.
                         If omitted, min_fp is used.
  CALIBRATION_TARGET_FN  Fixed at 0.015.
  CALIBRATION_DATA_PERCENT Default: 20.
  CALIBRATION_SERVER_PERCENT Default: 100.
  CALIBRATION_CLIENT_PERCENT Default: max. With protocol splits, max means the
                             largest calibration percent that leaves enough
                             held-out Client rows for the requested CLIENT_SIZES.
  CALIBRATION_CACHE_TABLE Override cache path. Defaults to calibration_<db>_fp_<FP>.csv
                          or calibration_<db>_min_fp.csv next to PARAM_TABLE.
  --fresh-calibration     Run calibration, ignoring any existing cache, then refresh the cache.
  --fresh_calibration     Alias for --fresh-calibration.
  --gowalla-source-holdout
  --gowalla-pre-split-holdout
                         For Gowalla, split source data into calibration holdout
                         and protocol complement. Calibration uses only the
                         holdout; experiments use only the complement.
  --use-calibration-cache
  --use_calibration_cache
                         In non-calibration mode, use CALIBRATION_CACHE_TABLE as
                         PARAM_TABLE if it exists. Default keeps paper tables.
  Extra ./test args such as --cuckoo_pow2_buckets or --calibration_holdout are forwarded to ./test.

Examples:
  ./run_cuckoo_server_sweep.sh calibrate_only
  ./run_cuckoo_server_sweep.sh calibrate_only --calibration_holdout
  SERVER_SIZES="8388608 16777216" CLIENT_SIZES="1000" ./run_cuckoo_server_sweep.sh --target-fp 0.03
  RUN_AND_CALIB_MIN_SERVER=16777216 ./run_cuckoo_server_sweep.sh run_and_calib

Outputs:
  Results are written under results/<db>_<fp>[_pow2]_<timestamp>/.
USAGE
}

if [[ $# -gt 0 ]]; then
  case "$1" in
    -h|--help|help)
      usage
      exit 0
      ;;
  esac
fi

target_fp=""
target_fp_provided=0
CALIBRATE="${CALIBRATE:-0}"
CALIBRATE_ONLY="${CALIBRATE_ONLY:-0}"
RUN_AND_CALIB=0
FRESH_CALIBRATION="${FRESH_CALIBRATION:-0}"
GOWALLA_SOURCE_HOLDOUT="${GOWALLA_SOURCE_HOLDOUT:-0}"
USE_CALIBRATION_CACHE="${USE_CALIBRATION_CACHE:-0}"
CALIBRATION_SERVER_PERCENT_OVERRIDE=""
CALIBRATION_CLIENT_PERCENT_OVERRIDE=""
if [[ $# -gt 0 ]]; then
  case "$1" in
    calibrate|--calibrate)
      CALIBRATE=1
      shift
      ;;
    run_and_calib|--run_and_calib|--run-and-calib)
      RUN_AND_CALIB=1
      shift
      ;;
    calibrate_only|--calibrate-only|--calibrate_only)
      CALIBRATE=1
      CALIBRATE_ONLY=1
      shift
      ;;
  esac
fi

if [[ $# -gt 0 ]]; then
  case "$1" in
    --target-fp=*|-target-fp=*)
      target_fp="${1#--target-fp=}"
      target_fp="${target_fp#-target-fp=}"
      target_fp_provided=1
      shift
      ;;
    --target-fp|-target-fp)
      if [[ $# -lt 2 ]]; then
        echo "Usage: $0 [target_fp|--target-fp TARGET_FP] [extra ./test args...]" >&2
        exit 2
      fi
      target_fp="$2"
      target_fp_provided=1
      shift 2
      ;;
    --*)
      ;;
    *)
      target_fp="$1"
      target_fp_provided=1
      shift
      ;;
  esac
fi

extra_args=()
for arg in "$@"; do
  case "${arg}" in
    calibrate|--calibrate)
      CALIBRATE=1
      ;;
    run_and_calib|--run_and_calib|--run-and-calib)
      RUN_AND_CALIB=1
      ;;
    calibrate_only|--calibrate-only|--calibrate_only)
      CALIBRATE=1
      CALIBRATE_ONLY=1
      ;;
    --fresh-calibration|--fresh_calibration)
      FRESH_CALIBRATION=1
      ;;
    --gowalla-source-holdout|--gowalla_source_holdout|--gowalla-pre-split-holdout|--gowalla_pre_split_holdout)
      GOWALLA_SOURCE_HOLDOUT=1
      ;;
    --use-calibration-cache|--use_calibration_cache)
      USE_CALIBRATION_CACHE=1
      ;;
    --calibration-server-100|--calibration_server_100)
      CALIBRATION_SERVER_PERCENT_OVERRIDE="100"
      ;;
    --calibration-server-percent=*)
      CALIBRATION_SERVER_PERCENT_OVERRIDE="${arg#--calibration-server-percent=}"
      ;;
    --calibration_server_percent=*)
      CALIBRATION_SERVER_PERCENT_OVERRIDE="${arg#--calibration_server_percent=}"
      ;;
    --calibration-client-20|--calibration_client_20)
      CALIBRATION_CLIENT_PERCENT_OVERRIDE="20"
      ;;
    --calibration-client-100|--calibration_client_100)
      CALIBRATION_CLIENT_PERCENT_OVERRIDE="100"
      ;;
    --calibration-client-percent=*)
      CALIBRATION_CLIENT_PERCENT_OVERRIDE="${arg#--calibration-client-percent=}"
      ;;
    --calibration_client_percent=*)
      CALIBRATION_CLIENT_PERCENT_OVERRIDE="${arg#--calibration_client_percent=}"
      ;;
    --pir-mode=*)
      PIR_MODE="${arg#--pir-mode=}"
      ;;
    --pir_mode=*)
      PIR_MODE="${arg#--pir_mode=}"
      ;;
    --PIR_double|--pir_double|--USE_PIR_double|--use_pir_double)
      PIR_MODE="PIR_double"
      ;;
    --PIR_BatchPIR|--PIR_batchpir|--pir_batchpir|--USE_PIR_BatchPIR|--use_pir_batchpir)
      PIR_MODE="BatchPIR"
      ;;
    *)
      extra_args+=("${arg}")
      ;;
  esac
done
if (( ${#extra_args[@]} > 0 )); then
  set -- "${extra_args[@]}"
else
  set --
fi
if [[ "${FRESH_CALIBRATION}" == "1" && "${RUN_AND_CALIB}" != "1" ]]; then
  CALIBRATE=1
fi
if [[ "${CALIBRATE}" == "auto" ]]; then
  CALIBRATE=0
fi

target_is_min_fp="$(
  awk -v value="${target_fp}" -v provided="${target_fp_provided}" 'BEGIN {
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
    lower = tolower(value);
    gsub(/-/, "_", lower);
    if (provided != "1" || lower == "" || lower == "min" || lower == "min_fp" || lower == "min_possible_fp" || lower == "minimum_fp") {
      print "1";
    } else {
      print "0";
    }
  }'
)"

target_label="$(
  awk -v value="${target_fp}" -v provided="${target_fp_provided}" -v min_fp="${target_is_min_fp}" 'BEGIN {
    if (min_fp == "1") {
      print "min_fp";
      exit;
    }
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
    gsub(/\./, "_", value);
    gsub(/[^0-9A-Za-z_+-]/, "_", value);
    print value;
  }'
)"
folder_fp_label="$(
  awk -v value="${target_fp}" -v provided="${target_fp_provided}" -v min_fp="${target_is_min_fp}" 'BEGIN {
    if (min_fp == "1") {
      print "min_fp";
      exit;
    }
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
    if (value ~ /^0\.[0-9]+$/) {
      sub(/^0\./, "", value);
      while (length(value) < 3) value = "0" value;
      print value;
      exit;
    }
    if (value ~ /^\.[0-9]+$/) {
      sub(/^\./, "", value);
      while (length(value) < 3) value = "0" value;
      print value;
      exit;
    }
    gsub(/\./, "_", value);
    gsub(/[^0-9A-Za-z_+-]/, "_", value);
    print value;
  }'
)"
calibration_cache_label="$(
  awk -v value="${target_fp}" -v provided="${target_fp_provided}" -v min_fp="${target_is_min_fp}" 'BEGIN {
    if (min_fp == "1") {
      print "min_fp";
      exit;
    }
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
    gsub(/\./, "_", value);
    gsub(/[^0-9A-Za-z_+-]/, "_", value);
    print "fp_" value;
  }'
)"

SMALL_DB_PRESET="${SMALL_DB_PRESET:-gowalla}"
BIG_DB_PRESET="${BIG_DB_PRESET:-gowalla_plus_augmented_generated_client}"
if [[ "${SMALL_DB_PRESET}" == "default" ]]; then
  SMALL_DB_PRESET="gowalla"
fi
if [[ "${BIG_DB_PRESET}" == "default_plus_augmented_generated_client" ]]; then
  BIG_DB_PRESET="gowalla_plus_augmented_generated_client"
fi
if [[ "${BIG_DB_PRESET}" == "default_plus_random_generated_client" ]]; then
  BIG_DB_PRESET="gowalla_plus_random_generated_client"
fi
if [[ "${BIG_DB_PRESET}" == "default_plus_distribution_generated_client" ]]; then
  BIG_DB_PRESET="gowalla_plus_distribution_generated_client"
fi
CALIBRATION_DIR="${CALIBRATION_DIR:-datasets/calibrations}"
PIR_MODE="${PIR_MODE:-BatchPIR}"
case "${PIR_MODE}" in
  BatchPIR|batchpir|PIR_BatchPIR|PIR_batchpir)
    PIR_MODE="BatchPIR"
    PIR_FLAG="--PIR_BatchPIR"
    ;;
  PIR_double|pir_double|DoublePIR|doublepir)
    PIR_MODE="PIR_double"
    PIR_FLAG="--PIR_double"
    ;;
  *)
    echo "[script] Unknown PIR_MODE=${PIR_MODE}; use BatchPIR or PIR_double" >&2
    exit 2
    ;;
esac
paper_gowalla_param_table_for_target() {
  awk -v target="${target_fp}" -v min_fp="${target_is_min_fp}" -v dir="${CALIBRATION_DIR}" 'BEGIN {
    if (min_fp == "1") {
      print dir "/calibration_gowalla_min_possible_fp.csv";
    } else if (target + 0 <= 0.015) {
      print dir "/calibration_gowalla_fp_lt_0_015_fn_lt_0_01.csv";
    } else if (target + 0 <= 0.035) {
      print dir "/calibration_gowalla_fp_lt_0_03_fn_lt_0_01.csv";
    } else {
      print dir "/calibration_gowalla_fp_lt_0_05_fn_lt_0_01.csv";
    }
  }'
}
PARAM_TABLE_PROVIDED=0
if [[ -n "${PARAM_TABLE+x}" ]]; then
  PARAM_TABLE_PROVIDED=1
fi

if [[ "${SMALL_DB_PRESET}" == "gowalla" && "${BIG_DB_PRESET}" == gowalla_plus_* ]]; then
  DEFAULT_PARAM_TABLE="$(paper_gowalla_param_table_for_target)"
else
  DEFAULT_PARAM_TABLE="${CALIBRATION_DIR}/calibration.csv"
fi
if [[ ! -f "${DEFAULT_PARAM_TABLE}" ]]; then
  DEFAULT_PARAM_TABLE="${CALIBRATION_DIR}/calibration.csv"
fi
PARAM_TABLE="${PARAM_TABLE:-${DEFAULT_PARAM_TABLE}}"
PAPER_RUN_SUMMARY="${PAPER_RUN_SUMMARY:-results/RESULTS USED IN PAPER/Cuckoo Batch PIR Fp 0 03 FN 0 01/summary.csv}"
PARAM_TABLE_DIR="$(dirname "${PARAM_TABLE}")"

#read -r -a SERVER_SIZES <<< "${SERVER_SIZES:-1000 2000 5000 10000 20000 50000 100000 500000 1000000 1500000 2000000 2500000 4500000 8388608 16777216 33554432 67108864}"
#read -r -a CLIENT_SIZES <<< "${CLIENT_SIZES:-10000 1000 500 100}"

read -r -a SERVER_SIZES <<< "${SERVER_SIZES:-1000 2000 5000 10000 20000 50000 100000 500000 1000000 1500000 2000000 2500000 4500000}"

read -r -a CLIENT_SIZES <<< "${CLIENT_SIZES:-1000 }"

SMALL_PARAM_MAX_SERVER="${SMALL_PARAM_MAX_SERVER:-4500000}"
PIR_BATCHPIR_BATCH_SIZE="${PIR_BATCHPIR_BATCH_SIZE:-200}"
CLIENT_100_PIR_BATCHPIR_BATCH_SIZE="${CLIENT_100_PIR_BATCHPIR_BATCH_SIZE:-100}"
PIR_BATCHPIR_FALLBACK_SIZES="${PIR_BATCHPIR_FALLBACK_SIZES:-200 100 64 32 16 8}"
NUM_RUNS="${NUM_RUNS:-1}"
if [[ "${target_is_min_fp}" == "0" ]]; then
  CALIBRATION_TARGET_FP="${target_fp}"
  calibration_target_fp_provided=1
else
  CALIBRATION_TARGET_FP="${CALIBRATION_TARGET_FP:-1.0}"
  calibration_target_fp_provided=0
fi
CALIBRATION_TARGET_FN="0.015"
CALIBRATION_L_VALUES="${CALIBRATION_L_VALUES:-1 3 5 7 10}"
CALIBRATION_K_VALUES="${CALIBRATION_K_VALUES:-1 3 5 10 20}"
CALIBRATION_W_VALUES="${CALIBRATION_W_VALUES:-0.0001 0.001 0.01 0.05 0.1 0.5 1 2 5}"
CALIBRATION_FILTER_SIZE_PERCENT_INCREASES="${CALIBRATION_FILTER_SIZE_PERCENT_INCREASES:-33 66 100}"
CALIBRATION_ABORT_FAIL_RATE="${CALIBRATION_ABORT_FAIL_RATE:-0.02}"
CALIBRATION_ABORT_FILL_RATIO="${CALIBRATION_ABORT_FILL_RATIO:-0.98}"
CALIBRATION_ABORT_MIN_ATTEMPTS="${CALIBRATION_ABORT_MIN_ATTEMPTS:-1000000}"
CALIBRATION_PROGRESS_STEPS="${CALIBRATION_PROGRESS_STEPS:-20}"
FILTER_GROWTH_NUMERATOR="${FILTER_GROWTH_NUMERATOR:-5}"
FILTER_GROWTH_DENOMINATOR="${FILTER_GROWTH_DENOMINATOR:-4}"
MAX_FILTER_GROWTH_STEPS="${MAX_FILTER_GROWTH_STEPS:-8}"
BIG_CALIBRATION_SERVER_SIZES="${BIG_CALIBRATION_SERVER_SIZES:-${SERVER_SIZES[*]}}"
RUN_AND_CALIB_EXTRA_SERVER_SIZES="${RUN_AND_CALIB_EXTRA_SERVER_SIZES:-8388608 16777216 33554432 67108864}"
RUN_AND_CALIB_CLIENT_SIZES="${RUN_AND_CALIB_CLIENT_SIZES:-100 500 1000 10000}"
RUN_AND_CALIB_MIN_SERVER="${RUN_AND_CALIB_MIN_SERVER:-}"
RUN_AND_CALIB_SKIP_PAIRS="${RUN_AND_CALIB_SKIP_PAIRS:-}"
MEASUREMENT_TARGET_FP="${CALIBRATION_TARGET_FP}"
MEASUREMENT_TARGET_FN="${CALIBRATION_TARGET_FN}"
if [[ "${RUN_AND_CALIB}" == "1" ]]; then
  MEASUREMENT_TARGET_FP="${CALIBRATION_TARGET_FP}"
fi
if [[ -z "${USE_PROTOCOL_SPLIT:-}" ]]; then
  if [[ "${CALIBRATE}" == "1" || "${RUN_AND_CALIB}" == "1" ]]; then
    USE_PROTOCOL_SPLIT=1
  else
    USE_PROTOCOL_SPLIT=0
  fi
fi
CALIBRATED_PARAM_TABLE=""
CALIBRATION_CACHE_TABLE="${CALIBRATION_CACHE_TABLE:-${PARAM_TABLE_DIR}/calibration_${SMALL_DB_PRESET}_to_${BIG_DB_PRESET}_heldout_${calibration_cache_label}.csv}"
if [[ "${USE_CALIBRATION_CACHE}" == "1" && "${CALIBRATE}" != "1" && "${RUN_AND_CALIB}" != "1" && "${PARAM_TABLE_PROVIDED}" != "1" && -f "${CALIBRATION_CACHE_TABLE}" ]]; then
  PARAM_TABLE="${CALIBRATION_CACHE_TABLE}"
  PARAM_TABLE_DIR="$(dirname "${PARAM_TABLE}")"
fi
OPRF_MECHANISM="${OPRF_MECHANISM:-GCAES}"
OPRF_ADDR="${OPRF_ADDR:-127.0.0.1:50051}"
CALIBRATION_DATA_PERCENT="${CALIBRATION_DATA_PERCENT:-20}"
DEFAULT_CALIBRATION_SERVER_PERCENT="100"
if [[ "${GOWALLA_SOURCE_HOLDOUT}" == "1" ]]; then
  DEFAULT_CALIBRATION_SERVER_PERCENT="${CALIBRATION_DATA_PERCENT}"
fi
CALIBRATION_SERVER_PERCENT="${CALIBRATION_SERVER_PERCENT_OVERRIDE:-${CALIBRATION_SERVER_PERCENT:-${DEFAULT_CALIBRATION_SERVER_PERCENT}}}"
CALIBRATION_CLIENT_PERCENT="${CALIBRATION_CLIENT_PERCENT_OVERRIDE:-${CALIBRATION_CLIENT_PERCENT:-max}}"

dataset_prefix_for_db() {
  case "$1" in
    MNIST|mnist) printf 'mnist\n' ;;
    FashionMNIST|fashionmnist|FashionMNST|fashionmnst) printf 'fashionmnist\n' ;;
    febrl|FEBRL|febrl4|FEBRL4) printf 'febrl\n' ;;
    gowalla|default) printf 'gowalla\n' ;;
    acm_dblp|dblp_acm|acm_dplp) printf 'acm_dblp\n' ;;
    *) printf '%s\n' "$1" | tr '[:upper:]' '[:lower:]' ;;
  esac
}

source_paths_for_db() {
  case "$1" in
    MNIST|mnist)
      printf '%s\n' datasets/mnist_server.json datasets/mnist_client_close.json datasets/mnist_client_far.json
      ;;
    FashionMNIST|fashionmnist|FashionMNST|fashionmnst)
      printf '%s\n' datasets/fashionmnist_server.json datasets/fashionmnist_client_close.json datasets/fashionmnist_client_far.json
      ;;
    febrl|FEBRL|febrl4|FEBRL4)
      printf '%s\n' datasets/febrl_server.json datasets/febrl_client_close.json datasets/febrl_client_far.json
      ;;
    gowalla|default)
      printf '%s\n' datasets/gowalla_server.json datasets/gowalla_client/gowalla_client_close.json datasets/gowalla_client/gowalla_client_far.json
      ;;
    acm_dblp|dblp_acm|acm_dplp)
      printf '%s\n' datasets/acm_dblp_protocol/server.json datasets/acm_dblp_protocol/client_close.json datasets/acm_dblp_protocol/client_far.json
      ;;
    *)
      return 1
      ;;
  esac
}

json_client_counts() {
  local close_path="$1"
  local far_path="$2"

  python3 - "$close_path" "$far_path" <<'PY'
import json
import sys

close_path, far_path = sys.argv[1], sys.argv[2]
with open(close_path) as f:
    close = json.load(f)
with open(far_path) as f:
    far = json.load(f)

close_rows = sum(len(value.get("close", [])) for value in close.values())
far_rows = 0
for value in far.values():
    if isinstance(value, list):
        far_rows += len(value)
    else:
        far_rows += 1

print(close_rows, far_rows)
PY
}

max_requested_client_size() {
  local max_size=0
  local size

  for size in "${CLIENT_SIZES[@]}"; do
    if (( size > max_size )); then
      max_size="${size}"
    fi
  done

  printf '%s\n' "${max_size}"
}

max_client_calibration_percent() {
  local close_path="$1"
  local far_path="$2"
  local max_client
  local needed_close
  local needed_far
  local close_rows
  local far_rows

  max_client="$(max_requested_client_size)"
  needed_close=$(( max_client / 2 ))
  needed_far=$(( max_client - needed_close ))
  read -r close_rows far_rows < <(json_client_counts "${close_path}" "${far_path}")

  awk -v close_rows="${close_rows}" \
      -v far_rows="${far_rows}" \
      -v needed_close="${needed_close}" \
      -v needed_far="${needed_far}" 'BEGIN {
    if (close_rows <= 0 || far_rows <= 0) {
      print "0";
      exit;
    }
    close_pct = 100.0 * (close_rows - needed_close) / close_rows;
    far_pct = 100.0 * (far_rows - needed_far) / far_rows;
    pct = close_pct < far_pct ? close_pct : far_pct;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    pct -= 0.001;
    if (pct < 0) pct = 0;
    printf "%.3f\n", pct;
  }'
}

max_source_holdout_client_calibration_percent() {
  local close_path="$1"
  local far_path="$2"
  local close_rows
  local far_rows

  read -r close_rows far_rows < <(json_client_counts "${close_path}" "${far_path}")

  awk -v close_rows="${close_rows}" \
      -v far_rows="${far_rows}" 'BEGIN {
    if (close_rows <= 1 || far_rows <= 1) {
      print "0";
      exit;
    }
    close_pct = 100.0 * (close_rows - 1) / close_rows;
    far_pct = 100.0 * (far_rows - 1) / far_rows;
    pct = close_pct < far_pct ? close_pct : far_pct;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    pct -= 0.001;
    if (pct < 0) pct = 0;
    printf "%.3f\n", pct;
  }'
}

prepare_calibration_holdout_for_db() {
  local db="$1"
  local prefix
  local server_src
  local close_src
  local far_src
  local path
  local client_percent
  local source_paths=()

  prefix="$(dataset_prefix_for_db "${db}")"
  while IFS= read -r path; do
    source_paths+=("${path}")
  done < <(source_paths_for_db "${db}" || true)
  if (( ${#source_paths[@]} != 3 )); then
    echo "[script] no stored calibration split rule for db=${db}; using paths from preset directly" >&2
    return 0
  fi

  server_src="${source_paths[0]}"
  close_src="${source_paths[1]}"
  far_src="${source_paths[2]}"
  if [[ ! -f "${server_src}" || ! -f "${close_src}" || ! -f "${far_src}" ]]; then
    echo "[script] source dataset files missing for db=${db}; using paths from preset directly" >&2
    return 0
  fi

  client_percent="${CALIBRATION_CLIENT_PERCENT}"
  if [[ "${client_percent}" == "max" || "${client_percent}" == "MAX" ]]; then
    if [[ "${GOWALLA_SOURCE_HOLDOUT}" == "1" && "${prefix}" == "gowalla" ]]; then
      client_percent="$(
        max_source_holdout_client_calibration_percent "${close_src}" "${far_src}"
      )"
      echo "[script] resolved --calibration-client-percent=max to ${client_percent}% for db=${db} using Gowalla source holdout"
    else
      client_percent="$(
        max_client_calibration_percent "${close_src}" "${far_src}"
      )"
      echo "[script] resolved --calibration-client-percent=max to ${client_percent}% for db=${db} max CLIENT_SIZE=$(max_requested_client_size)"
    fi
    if awk -v pct="${client_percent}" 'BEGIN { exit !(pct <= 0) }'; then
      echo "[script] --calibration-client-percent=max left no Client rows for calibration." >&2
      echo "[script] For Gowalla CLIENT_SIZE=1000, all 500 far Client rows are reserved for the protocol split." >&2
      echo "[script] Use a smaller CLIENT_SIZES value, set USE_PROTOCOL_SPLIT=0, or pass --calibration-client-percent=100 to calibrate on all Client rows." >&2
      exit 2
    fi
  fi

  python3 datasets/split_calibration_holdout.py \
    --server "${server_src}" \
    --client-close "${close_src}" \
    --client-far "${far_src}" \
    --output-dir "${CALIBRATION_DIR}" \
    --prefix "${prefix}" \
    --percent "${CALIBRATION_DATA_PERCENT}" \
    --server-percent "${CALIBRATION_SERVER_PERCENT}" \
    --client-percent "${client_percent}"
}

calibration_path_args_for_db() {
  local db="$1"
  local prefix
  prefix="$(dataset_prefix_for_db "${db}")"
  if [[ -f "${CALIBRATION_DIR}/${prefix}_calibration_server.json" ]]; then
    printf '%s\n' \
      "--server_path=${CALIBRATION_DIR}/${prefix}_calibration_server.json" \
      "--client_close_path=${CALIBRATION_DIR}/${prefix}_calibration_client_close.json" \
      "--client_far_path=${CALIBRATION_DIR}/${prefix}_calibration_client_far.json" \
      "--metadata_path=${CALIBRATION_DIR}/${prefix}_calibration_metadata.txt"
  fi
}

protocol_path_args_for_db() {
  local db="$1"
  local prefix
  prefix="$(dataset_prefix_for_db "${db}")"
  if [[ -f "${CALIBRATION_DIR}/${prefix}_protocol_server.json" ]]; then
    printf '%s\n' \
      "--server_path=${CALIBRATION_DIR}/${prefix}_protocol_server.json" \
      "--client_close_path=${CALIBRATION_DIR}/${prefix}_protocol_client_close.json" \
      "--client_far_path=${CALIBRATION_DIR}/${prefix}_protocol_client_far.json" \
      "--metadata_path=${CALIBRATION_DIR}/${prefix}_protocol_metadata.txt"
  fi
}

if [[ "${CALIBRATE}" == "1" || "${RUN_AND_CALIB}" == "1" ]]; then
  prepare_calibration_holdout_for_db "${SMALL_DB_PRESET}"
  if [[ "${BIG_DB_PRESET}" != "${SMALL_DB_PRESET}" ]]; then
    prepare_calibration_holdout_for_db "${BIG_DB_PRESET}"
  fi
fi
if [[ -z "${FUZZY_PETS_DROIDCRYPTO_LIB:-}" ]]; then
  if [[ -f "disco/mobile_psi_cpp/build-mac/droidCrypto/libdroidcrypto.dylib" ]]; then
    export FUZZY_PETS_DROIDCRYPTO_LIB="disco/mobile_psi_cpp/build-mac/droidCrypto/libdroidcrypto.dylib"
  elif [[ -f "disco/mobile_psi_cpp/build-linux/droidCrypto/libdroidcrypto.so" ]]; then
    export FUZZY_PETS_DROIDCRYPTO_LIB="disco/mobile_psi_cpp/build-linux/droidCrypto/libdroidcrypto.so"
  fi
fi

if [[ ! -f "${PARAM_TABLE}" && "${CALIBRATE}" != "1" ]]; then
  echo "[script] Missing parameter table: ${PARAM_TABLE}" >&2
  echo "[script] Set PARAM_TABLE=/path/to/calibration.csv." >&2
  exit 2
fi
if [[ ! -f "${PARAM_TABLE}" && "${CALIBRATE}" == "1" ]]; then
  echo "[script] parameter table not found: ${PARAM_TABLE}; calibration will start from the configured L/k/w grid only"
fi
if [[ "${RUN_AND_CALIB}" == "1" && ! -f "${PAPER_RUN_SUMMARY}" ]]; then
  echo "[script] Missing paper summary: ${PAPER_RUN_SUMMARY}" >&2
  echo "[script] Set PAPER_RUN_SUMMARY=/path/to/summary.csv." >&2
  exit 2
fi

echo "[script] params/filter sizes: ${PARAM_TABLE}"
echo "[script] calibration cache: ${CALIBRATION_CACHE_TABLE}"
echo "[script] fresh calibration: ${FRESH_CALIBRATION}"
echo "[script] Gowalla source holdout: ${GOWALLA_SOURCE_HOLDOUT}"
echo "[script] use calibration cache as params: ${USE_CALIBRATION_CACHE}"
echo "[script] calibration data: server=${CALIBRATION_SERVER_PERCENT}% client=${CALIBRATION_CLIENT_PERCENT}% under ${CALIBRATION_DIR}; protocol uses complementary Client data unless percent is 100"
echo "[script] protocol split for measurement: ${USE_PROTOCOL_SPLIT}"
echo "[script] PIR mode: ${PIR_MODE}"
if [[ "${RUN_AND_CALIB}" == "1" ]]; then
  echo "[script] run_and_calib source summary: ${PAPER_RUN_SUMMARY}"
fi
echo "[script] dataset policy: Server <= ${SMALL_PARAM_MAX_SERVER} uses --db=${SMALL_DB_PRESET}; Server > ${SMALL_PARAM_MAX_SERVER} uses --db=${BIG_DB_PRESET}"
if [[ "${BIG_DB_PRESET}" == "gowalla_plus_augmented_generated_client" ]]; then
  echo "[script] big-Server data: original datasets/gowalla_server.json plus jittered extra Server points; Client is generated near the mixed Server set"
fi
if [[ "${PIR_MODE}" == "BatchPIR" ]]; then
  echo "[script] BatchPIR batch size: ${CLIENT_100_PIR_BATCHPIR_BATCH_SIZE} for Client=100, ${PIR_BATCHPIR_BATCH_SIZE} otherwise"
  echo "[script] BatchPIR fallback sizes on parms_id crash: ${PIR_BATCHPIR_FALLBACK_SIZES}"
else
  echo "[script] DoublePIR max cached shards: set with --pir_double_max_cached_shards=N; ./test default is 2"
fi
echo "[script] OPRF mechanism: ${OPRF_MECHANISM} at ${OPRF_ADDR}"
echo "[script] num runs per point: ${NUM_RUNS}"
if [[ "${target_is_min_fp}" == "1" ]]; then
  echo "[script] target FP table: min_fp"
else
  echo "[script] target FP table: ${target_fp}"
fi
if [[ "${CALIBRATE}" == "1" ]]; then
  if [[ "${calibration_target_fp_provided}" == "1" ]]; then
    echo "[script] calibration: enabled for big Server sizes; calibration runs without PIR/OPRF and selects rows with FP<=${CALIBRATION_TARGET_FP}, FN<=${CALIBRATION_TARGET_FN}"
    echo "[script] calibration selection: among target-passing rows, minimize L first, then filter_size; FP, FN, k, and w break ties"
  else
    echo "[script] calibration: enabled for big Server sizes; no FP target provided, so calibration selects among FN<=${CALIBRATION_TARGET_FN} rows, then minimizes observed FP"
    echo "[script] calibration selection: prefer FN target pass; then FP, FN, L, filter_size, k, and w break ties"
  fi
  echo "[script] calibration filter sizes: server_size plus {${CALIBRATION_FILTER_SIZE_PERCENT_INCREASES}} percent, rounded up to a multiple of 4"
  echo "[script] calibration data: calibration commands request each full Server size from the held-out 20% source pool"
  echo "[script] calibration aborts hopeless Cuckoo candidates at fail_rate>${CALIBRATION_ABORT_FAIL_RATE} or fill>=${CALIBRATION_ABORT_FILL_RATIO} after ${CALIBRATION_ABORT_MIN_ATTEMPTS} attempted inserts"
elif [[ "${RUN_AND_CALIB}" == "1" ]]; then
  echo "[script] run_and_calib: measures every Server from the paper summary plus ${RUN_AND_CALIB_EXTRA_SERVER_SIZES}; Client sizes: ${RUN_AND_CALIB_CLIENT_SIZES}"
  echo "[script] run_and_calib: fresh FP > ${CALIBRATION_TARGET_FP} or FN > ${CALIBRATION_TARGET_FN} triggers direct calibration without PIR/OPRF, then final BatchPIR+OPRF reruns until FP/FN targets are met"
  echo "[script] run_and_calib: if calibration/final rerun misses target, filter_size grows by ${FILTER_GROWTH_NUMERATOR}/${FILTER_GROWTH_DENOMINATOR} for up to ${MAX_FILTER_GROWTH_STEPS} growth steps"
  echo "[script] run_and_calib calibration aborts hopeless Cuckoo candidates at fail_rate>${CALIBRATION_ABORT_FAIL_RATE} or fill>=${CALIBRATION_ABORT_FILL_RATIO} after ${CALIBRATION_ABORT_MIN_ATTEMPTS} attempted inserts"
  if [[ -n "${RUN_AND_CALIB_MIN_SERVER}" || -n "${RUN_AND_CALIB_SKIP_PAIRS}" ]]; then
    echo "[script] run_and_calib resume filter: min_server=${RUN_AND_CALIB_MIN_SERVER:-none} skip_pairs=${RUN_AND_CALIB_SKIP_PAIRS:-none}"
  fi
else
  echo "[script] adaptive retries: disabled; using paper calibration values exactly"
fi

BASE_CMD=(
  ./test
  --filter=Cuckoo
  "${PIR_FLAG}"
  --OPRF
  "--oprf_mechanism=${OPRF_MECHANISM}"
  "--oprf_addr=${OPRF_ADDR}"
  "--num_runs=${NUM_RUNS}"
)

CALIBRATION_BASE_CMD=(
  ./test
  --filter=Cuckoo
  "--num_runs=${NUM_RUNS}"
)

case "${DEBUG:-0}" in
  1|true|TRUE|yes|YES|on|ON)
  BASE_CMD+=(--debug=1)
    CALIBRATION_BASE_CMD+=(--debug=1)
    DEBUG_PHASES_ENABLED=1
    ;;
  *)
    DEBUG_PHASES_ENABLED=0
    ;;
esac

make test

folder_db_label="$(
  awk -v small="${SMALL_DB_PRESET}" -v big="${BIG_DB_PRESET}" 'BEGIN {
    if ((small == "gowalla" || small == "default") && big ~ /^gowalla_plus_/) {
      print "gowalla";
      exit;
    }
    if (small == big || big == "") {
      print small;
      exit;
    }
    print small "_to_" big;
  }'
)"
folder_db_label="$(
  printf '%s\n' "${folder_db_label}" | sed 's/[^0-9A-Za-z_+-]/_/g'
)"
folder_suffix=""
for arg in "$@"; do
  case "${arg}" in
    --cuckoo_pow2_buckets|--cuckoo_power_of_two_buckets|--Cuckoo_power_of_two_buckets)
      folder_suffix="_pow2"
      break
      ;;
  esac
done

timestamp="$(date +%Y%m%d_%H%M%S)"
log_dir="results/${folder_db_label}_${folder_fp_label}${folder_suffix}_${timestamp}"
mkdir -p "${log_dir}"
summary_file="${log_dir}/summary_target_${target_label}.csv"
seconds_file="${log_dir}/seconds_target_${target_label}.csv"
summary_header="server_size,client_size,avg_fp,avg_fn,avg_total_seconds,max_total_seconds,avg_client_seconds,avg_server_seconds,avg_server_offline_seconds,avg_server_online_seconds,avg_filter_build_time_ms,avg_pir_setup_time_ms,avg_pir_single_setup_time_ms,avg_query_time_ms,avg_build_setup_query_ms,max_build_setup_query_ms,avg_communication_mb,avg_pir_query_communication_mb,avg_pir_response_communication_mb,avg_pir_setup_communication_mb,avg_cuckoo_failed,cuckoo_fail_rate,filter_size,pir_mode,L,k,w,pir_single_shards,pir_single_db_rows,batchpir_batch_size,selection_note,buckets,total_size_bits,filter_size_mb,log"

printf "%s\n" "${summary_header}" > "${summary_file}"
printf "client_size,server_size,seconds\n" > "${seconds_file}"
active_summary_file="${summary_file}"
active_seconds_file="${seconds_file}"

max_time=""
max_server=""
max_client=""
max_filter_mb=""
max_log=""

params_for_server() {
  local server_size="$1"

  if [[ ! -f "${PARAM_TABLE}" ]]; then
    return 4
  fi

  awk -F';' -v want_server="${server_size}" '
    function clean(value) {
      gsub(/\r$/, "", value);
      gsub(/^"|"$/, "", value);
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
      return value;
    }
    NR == 1 {
      for (i = 1; i <= NF; ++i) {
        name = clean($i);
        col[name] = i;
      }
      if (!("server_size" in col) && ("alice_size" in col)) col["server_size"] = col["alice_size"];
      if (!("client_size" in col) && ("bob_size" in col)) col["client_size"] = col["bob_size"];
      required = "server_size L k w filter_size";
      split(required, names, " ");
      for (i in names) {
        if (!(names[i] in col)) {
          print "[script] parameter table missing column: " names[i] > "/dev/stderr";
          exit 3;
        }
      }
      next;
    }
    clean($col["server_size"]) == want_server {
      print clean($col["L"]) " " clean($col["k"]) " " clean($col["w"]) " " clean($col["filter_size"]) " paper-calibration";
      found = 1;
      exit 0;
    }
    END {
      if (!found) {
        exit 4;
      }
    }
  ' "${PARAM_TABLE}"
}

params_from_table() {
  local table="$1"
  local client_size="$2"
  local server_size="$3"

  awk -F';' -v want_client="${client_size}" -v want_server="${server_size}" '
    function clean(value) {
      gsub(/\r$/, "", value);
      gsub(/^"|"$/, "", value);
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
      return value;
    }
    NR == 1 {
      for (i = 1; i <= NF; ++i) {
        name = clean($i);
        col[name] = i;
      }
      if (!("server_size" in col) && ("alice_size" in col)) col["server_size"] = col["alice_size"];
      if (!("client_size" in col) && ("bob_size" in col)) col["client_size"] = col["bob_size"];
      required = "server_size L k w filter_size";
      split(required, names, " ");
      for (i in names) {
        if (!(names[i] in col)) {
          print "[script] parameter table missing column: " names[i] " in " FILENAME > "/dev/stderr";
          exit 3;
        }
      }
      has_client = ("client_size" in col);
      next;
    }
    clean($col["server_size"]) == want_server && (!has_client || clean($col["client_size"]) == want_client) {
      source = has_client ? "direct-cuckoo-calibration" : "paper-calibration";
      print clean($col["L"]) " " clean($col["k"]) " " clean($col["w"]) " " clean($col["filter_size"]) " " source;
      found = 1;
      exit 0;
    }
    END {
      if (!found) {
        exit 4;
      }
    }
  ' "${table}"
}

params_for_pair() {
  local client_size="$1"
  local server_size="$2"

  if [[ -n "${CALIBRATED_PARAM_TABLE}" ]] && params_from_table "${CALIBRATED_PARAM_TABLE}" "${client_size}" "${server_size}"; then
    return 0
  fi

  params_from_table "${PARAM_TABLE}" "${client_size}" "${server_size}"
}

is_big_calibration_server_size() {
  local server_size="$1"
  local candidate

  for candidate in ${BIG_CALIBRATION_SERVER_SIZES}; do
    if [[ "${server_size}" == "${candidate}" ]]; then
      return 0
    fi
  done
  return 1
}

big_calibration_filter_size() {
  calibration_filter_sizes_for_server "$1" | head -n 1
}

calibration_filter_sizes_for_server() {
  local server_size="$1"
  local pct

  for pct in ${CALIBRATION_FILTER_SIZE_PERCENT_INCREASES}; do
    awk -v server="${server_size}" -v pct="${pct}" 'BEGIN {
      size = int((server * (100 + pct) + 99) / 100);
      if (size < 4) size = 4;
      rem = size % 4;
      if (rem != 0) size += 4 - rem;
      print size;
    }'
  done | sort -n -u
}

calibration_filter_size_candidates_for_server() {
  local server_size="$1"
  local paper_filter_size="${2:-}"

  {
    if [[ -n "${paper_filter_size}" ]]; then
      printf '%s\n' "${paper_filter_size}"
    fi
    calibration_filter_sizes_for_server "${server_size}"
  } | sort -n -u
}

calibration_server_size_for_full_size() {
  awk -v server="${1}" 'BEGIN {
    size = server + 0;
    if (size < 1) size = 1;
    print size;
  }'
}

select_calibration_row() {
  local table="$1"
  local client_size="$2"
  local server_size="$3"
  local target_fp="$4"
  local target_fn="$5"
  local target_fp_provided="$6"

  awk -F';' -v want_client="${client_size}" -v want_server="${server_size}" -v target_fp="${target_fp}" -v target_fn="${target_fn}" -v target_fp_provided="${target_fp_provided}" '
    function clean(value) {
      gsub(/\r$/, "", value);
      gsub(/^"|"$/, "", value);
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
      return value;
    }
    function numeric_ok(value) {
      return value != "" && value != "nan" && value != "NaN";
    }
    function fn_target_ok(value) {
      return numeric_ok(value) && value + 0 <= target_fn + 0;
    }
    function target_rank_better() {
      if (!have_selected) return 1;
      if (L + 0 != best_L + 0) return L + 0 < best_L + 0;
      if (filter_size + 0 != best_filter_size + 0) return filter_size + 0 < best_filter_size + 0;
      if (numeric_ok(fp) && numeric_ok(best_fp) && fp + 0 != best_fp + 0) return fp + 0 < best_fp + 0;
      if (numeric_ok(fn) && numeric_ok(best_fn) && fn + 0 != best_fn + 0) return fn + 0 < best_fn + 0;
      if (k + 0 != best_k + 0) return k + 0 < best_k + 0;
      return w + 0 < best_w + 0;
    }
    function min_fp_rank_better() {
      if (!have_best) return 1;
      candidate_fn_ok = fn_target_ok(fn);
      fallback_fn_ok = fn_target_ok(fallback_fn);
      if (candidate_fn_ok != fallback_fn_ok) return candidate_fn_ok;
      if (!numeric_ok(fallback_fp)) return numeric_ok(fp);
      if (!numeric_ok(fp)) return 0;
      if (fp + 0 != fallback_fp + 0) return fp + 0 < fallback_fp + 0;
      if (numeric_ok(fn) && numeric_ok(fallback_fn) && fn + 0 != fallback_fn + 0) return fn + 0 < fallback_fn + 0;
      if (L + 0 != fallback_L + 0) return L + 0 < fallback_L + 0;
      if (filter_size + 0 != fallback_filter_size + 0) return filter_size + 0 < fallback_filter_size + 0;
      if (k + 0 != fallback_k + 0) return k + 0 < fallback_k + 0;
      return w + 0 < fallback_w + 0;
    }
    NR == 1 {
      for (i = 1; i <= NF; ++i) col[clean($i)] = i;
      next;
    }
    clean($col["client_size"]) == want_client && clean($col["server_size"]) == want_server {
      filter_size = clean($col["filter_size"]);
      L = clean($col["L"]);
      k = clean($col["k"]);
      w = clean($col["w"]);
      fp = clean($col["avg_fp"]);
      fn = clean($col["avg_fn"]);
      cuckoo_failed = ("avg_cuckoo_failed" in col) ? clean($col["avg_cuckoo_failed"]) : "";
      cuckoo_fail_rate = ("cuckoo_fail_rate" in col) ? clean($col["cuckoo_fail_rate"]) : "";
      status = clean($col["status"]);
      row_log = clean($col["log"]);
      meets_target = numeric_ok(fp) && numeric_ok(fn) && fp + 0 <= target_fp + 0 && fn + 0 <= target_fn + 0;
      if (target_fp_provided == "1" && meets_target && target_rank_better()) {
        best_filter_size = filter_size;
        best_L = L;
        best_k = k;
        best_w = w;
        best_fp = fp;
        best_fn = fn;
        best_cuckoo_failed = cuckoo_failed;
        best_cuckoo_fail_rate = cuckoo_fail_rate;
        best_status = "selected";
        best_log = row_log;
        have_selected = 1;
      }
      if (min_fp_rank_better()) {
        fallback_filter_size = filter_size;
        fallback_L = L;
        fallback_k = k;
        fallback_w = w;
        fallback_fp = fp;
        fallback_fn = fn;
        fallback_cuckoo_failed = cuckoo_failed;
        fallback_cuckoo_fail_rate = cuckoo_fail_rate;
        fallback_status = "selected";
        fallback_log = row_log;
        have_best = 1;
      }
    }
    END {
      if (target_fp_provided == "1" && have_selected) {
        print best_filter_size " " best_L " " best_k " " best_w " " best_fp " " best_fn " " best_cuckoo_failed " " best_cuckoo_fail_rate " " best_status " " best_log;
      } else if (target_fp_provided == "1" && have_best) {
        print fallback_filter_size " " fallback_L " " fallback_k " " fallback_w " " fallback_fp " " fallback_fn " " fallback_cuckoo_failed " " fallback_cuckoo_fail_rate " no_candidate_met_target " fallback_log;
      } else if (have_best) {
        status = fn_target_ok(fallback_fn) ? fallback_status : "no_candidate_met_fn_target";
        print fallback_filter_size " " fallback_L " " fallback_k " " fallback_w " " fallback_fp " " fallback_fn " " fallback_cuckoo_failed " " fallback_cuckoo_fail_rate " " status " " fallback_log;
      } else {
        exit 4;
      }
    }
  ' "${table}"
}

csv_get_calibrated_lkw() {
  local table="$1"
  local client_size="$2"
  local server_size="$3"

  awk -F';' -v want_client="${client_size}" -v want_server="${server_size}" '
    function clean(value) {
      gsub(/\r$/, "", value);
      gsub(/^"|"$/, "", value);
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
      return value;
    }
    NR == 1 {
      for (i = 1; i <= NF; ++i) col[clean($i)] = i;
      next;
    }
    clean($col["client_size"]) == want_client && clean($col["server_size"]) == want_server {
      print clean($col["L"]) ":" clean($col["k"]) ":" clean($col["w"]);
      found = 1;
      exit 0;
    }
    END {
      if (!found) exit 4;
    }
  ' "${table}"
}

calibration_candidate_lkw() {
  local preferred="$1"
  local paper_lkw="$2"
  local seen=" "
  local L
  local k
  local w
  local triple

  for triple in "${preferred}" "${paper_lkw}"; do
    if [[ -n "${triple}" && "${seen}" != *" ${triple} "* ]]; then
      printf '%s\n' "${triple}"
      seen="${seen}${triple} "
    fi
  done

  for L in ${CALIBRATION_L_VALUES}; do
    for k in ${CALIBRATION_K_VALUES}; do
      for w in ${CALIBRATION_W_VALUES}; do
        triple="${L}:${k}:${w}"
        if [[ "${seen}" != *" ${triple} "* ]]; then
          printf '%s\n' "${triple}"
          seen="${seen}${triple} "
        fi
      done
    done
  done
}

next_l_value() {
  case "$1" in
    1) printf '3\n' ;;
    3) printf '5\n' ;;
    5) printf '7\n' ;;
    7) printf '10\n' ;;
    *) printf '%s\n' "$1" ;;
  esac
}

next_k_value() {
  case "$1" in
    1) printf '3\n' ;;
    3) printf '5\n' ;;
    5) printf '10\n' ;;
    10) printf '20\n' ;;
    *) printf '%s\n' "$1" ;;
  esac
}

next_w_value() {
  awk -v w="$1" 'BEGIN {
    if (w + 0 < 0.1) print "0.1";
    else if (w + 0 < 0.5) print "0.5";
    else if (w + 0 < 1) print "1";
    else if (w + 0 < 2) print "2";
    else if (w + 0 < 5) print "5";
    else print w;
  }'
}

larger_filter_size() {
  awk -v size="$1" -v num="${FILTER_GROWTH_NUMERATOR}" -v den="${FILTER_GROWTH_DENOMINATOR}" 'BEGIN {
    if (den + 0 <= 0) den = 4;
    if (num + 0 <= den + 0) num = den + 1;
    next_size = int((size * num + den - 1) / den);
    if (next_size < 1) next_size = 1;
    if (next_size <= size) next_size = size + 1;
    rem = next_size % 4;
    if (rem != 0) next_size += 4 - rem;
    printf "%d\n", next_size;
  }'
}

metric_gt() {
  awk -v value="$1" -v target="$2" 'BEGIN {
    exit !(value != "" && value != "nan" && value != "NaN" && value + 0 > target + 0)
  }'
}

metric_lt() {
  awk -v value="$1" -v target="$2" 'BEGIN {
    exit !(value != "" && value != "nan" && value != "NaN" && target != "" && target != "nan" && target != "NaN" && value + 0 < target + 0)
  }'
}

batchpir_batch_candidates() {
  local start="$1"
  local seen=" "
  local candidate

  if [[ "${PIR_MODE}" != "BatchPIR" ]]; then
    printf 'none\n'
    return 0
  fi

  printf '%s\n' "${start}"
  seen="${seen}${start} "

  for candidate in ${PIR_BATCHPIR_FALLBACK_SIZES}; do
    if (( candidate < start )) && [[ "${seen}" != *" ${candidate} "* ]]; then
      printf '%s\n' "${candidate}"
      seen="${seen}${candidate} "
    fi
  done
}

parse_last_runtime_metrics() {
  local log_file="$1"

  awk '
    function trim(text) {
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", text);
      return text;
    }
    function flush_row() {
      if (in_row) {
        last_fp = fp;
        last_fn = fn;
      }
      in_row = 0;
      fp = fn = "";
    }
    BEGIN {
      in_row = 0;
      fp = fn = last_fp = last_fn = "";
    }
    /^\[RUNTIME AVG\]/ {
      flush_row();
      in_row = 1;
      next;
    }
    in_row && /^[[:space:]]*FP Rate:/ {
      text = $0;
      sub(/^[[:space:]]*FP Rate:[[:space:]]*/, "", text);
      fp = trim(text);
      next;
    }
    in_row && /^[[:space:]]*FN Rate:/ {
      text = $0;
      sub(/^[[:space:]]*FN Rate:[[:space:]]*/, "", text);
      fn = trim(text);
      next;
    }
    END {
      flush_row();
      if (last_fp == "" || last_fn == "") {
        exit 4;
      }
      print last_fp " " last_fn;
    }
  ' "${log_file}"
}

calibration_meets_target() {
  local fp="$1"
  local fn="$2"

  awk -v fp="${fp}" -v fn="${fn}" -v target_fp="${CALIBRATION_TARGET_FP}" -v target_fn="${CALIBRATION_TARGET_FN}" 'BEGIN {
    exit !(fp != "" && fn != "" && fp != "nan" && fn != "nan" && fp != "NaN" && fn != "NaN" && fp + 0 < target_fp + 0 && fn + 0 < target_fn + 0)
  }'
}

calibration_score_lt() {
  local candidate_fp="$1"
  local candidate_fn="$2"
  local current_fp="$3"
  local current_fn="$4"

  awk -v cfp="${candidate_fp}" -v cfn="${candidate_fn}" -v bfp="${current_fp}" -v bfn="${current_fn}" 'BEGIN {
    cscore = 0.3 * cfp + 0.7 * cfn;
    bscore = 0.3 * bfp + 0.7 * bfn;
    exit !(cscore < bscore || (cscore == bscore && cfn < bfn) || (cscore == bscore && cfn == bfn && cfp < bfp))
  }'
}

run_calibration_pair() {
  local client_size="$1"
  local server_size="$2"
  local filter_size="$3"
  local run_db="$4"
  local candidate_lkw_csv="$5"
  local calibration_dir="$6"
  local all_csv="$7"
  shift 7
  local calibration_server_size
  local log_file
  local run_cmd
  local test_status
  local best_fields
  local path_arg

  calibration_server_size="$(calibration_server_size_for_full_size "${server_size}")"
  log_file="${calibration_dir}/calibration_client_${client_size}_server_${calibration_server_size}_full_server_${server_size}_filter_${filter_size}.log"

  run_cmd=("${CALIBRATION_BASE_CMD[@]}")
  run_cmd+=(--db="${run_db}")
  while IFS= read -r path_arg; do
    run_cmd+=("${path_arg}")
  done < <(calibration_path_args_for_db "${run_db}")
  run_cmd+=(
    --CALIBRATE_LKW
    --calibration_full_grid
    --calibration_lkw="${candidate_lkw_csv}"
    --calibration_target_fp="${CALIBRATION_TARGET_FP}"
    --calibration_target_fn="${CALIBRATION_TARGET_FN}"
    --calibration_abort_fail_rate="${CALIBRATION_ABORT_FAIL_RATE}"
    --calibration_abort_fill_ratio="${CALIBRATION_ABORT_FILL_RATIO}"
    --calibration_abort_min_attempts="${CALIBRATION_ABORT_MIN_ATTEMPTS}"
    --calibration_progress_steps="${CALIBRATION_PROGRESS_STEPS}"
    --client_size="${client_size}"
    --server_size="${calibration_server_size}"
    --filter_size="${filter_size}"
    "$@"
  )

  printf '[script] calibration CLIENT_SIZE=%s CALIBRATION_SERVER_SIZE=%s FULL_SERVER_SIZE=%s filter_size=%s log=%s\n' \
    "${client_size}" \
    "${calibration_server_size}" \
    "${server_size}" \
    "${filter_size}" \
    "${log_file}"

  set +e
  "${run_cmd[@]}" > "${log_file}" 2>&1
  test_status="$?"
  set -e

  if [[ "${test_status}" -ne 0 ]]; then
    echo "[script] calibration ./test failed with exit ${test_status}; log=${log_file}" >&2
    tail -n 40 "${log_file}" >&2 || true
    exit "${test_status}"
  fi

  awk -v log_path="${log_file}" -F'[ =]' '
    /^\[calibration\] / {
      delete kv;
      for (i = 2; i <= NF; i += 2) {
        kv[$i] = $(i + 1);
      }
      print kv["client_size"] ";" full_server_size ";" kv["filter_size"] ";" kv["L"] ";" kv["k"] ";" kv["w"] ";" kv["fp"] ";" kv["fn"] ";" kv["cuckoo_failed"] ";" kv["cuckoo_fail_rate"] ";" kv["status"] ";" log_path;
    }
  ' full_server_size="${server_size}" "${log_file}" >> "${all_csv}"

  if ! best_fields="$(
    awk -F'[ =]' '
      /^\[calibration best\] / {
        delete kv;
        for (i = 3; i <= NF; i += 2) {
          kv[$i] = $(i + 1);
        }
        print kv["L"] " " kv["k"] " " kv["w"] " " kv["fp"] " " kv["fn"] " " kv["cuckoo_failed"] " " kv["cuckoo_fail_rate"] " " kv["status"];
        found = 1;
        exit 0;
      }
      END {
        if (!found) exit 4;
      }
    ' "${log_file}"
  )"; then
    echo "[script] could not parse calibration best row from ${log_file}" >&2
    exit 1
  fi
  read -r calibration_best_L calibration_best_k calibration_best_w calibration_best_fp calibration_best_fn calibration_best_cuckoo_failed calibration_best_cuckoo_fail_rate calibration_best_status <<< "${best_fields}"
  calibration_best_log="${log_file}"
}

run_big_server_calibration() {
  local calibration_dir="${log_dir}/calibration"
  local all_csv="${calibration_dir}/calibration_all.csv"
  local selected_csv="${calibration_dir}/calibrated_params.csv"
  local client_index
  local client_size
  local previous_client_size=""
  local server_size
  local filter_size
  local selected_filter_size
  local filter_size_options
  local paper_line
  local paper_L
  local paper_k
  local paper_w
  local paper_filter_size
  local paper_source
  local paper_lkw
  local preferred_lkw
  local calibration_db
  local triple
  local candidate_lkw_csv
  local best_L
  local best_k
  local best_w
  local best_fp
  local best_fn
  local best_cuckoo_failed
  local best_cuckoo_fail_rate
  local best_log
  local status

  mkdir -p "${calibration_dir}"
  printf "client_size;server_size;filter_size;L;k;w;avg_fp;avg_fn;avg_cuckoo_failed;cuckoo_fail_rate;status;log\n" > "${all_csv}"
  printf "client_size;server_size;filter_size;L;k;w;avg_fp;avg_fn;avg_cuckoo_failed;cuckoo_fail_rate;status;log\n" > "${selected_csv}"

  for client_index in "${!CLIENT_SIZES[@]}"; do
    client_size="${CLIENT_SIZES[${client_index}]}"
    echo
    echo "================ CALIBRATION CLIENT_SIZE=${client_size} (direct Cuckoo, no PIR/OPRF) ================"

    for server_size in ${BIG_CALIBRATION_SERVER_SIZES}; do
      paper_lkw=""
      if paper_line="$(params_for_server "${server_size}")"; then
        read -r paper_L paper_k paper_w paper_filter_size paper_source <<< "${paper_line}"
        paper_lkw="${paper_L}:${paper_k}:${paper_w}"
      fi
      preferred_lkw=""
      if [[ -n "${previous_client_size}" ]]; then
        preferred_lkw="$(csv_get_calibrated_lkw "${selected_csv}" "${previous_client_size}" "${server_size}" || true)"
      fi

      best_L=""
      best_k=""
      best_w=""
      best_fp=""
      best_fn=""
      best_cuckoo_failed=""
      best_cuckoo_fail_rate=""
      best_log=""
      status="no_candidate_met_target"

      echo
      filter_size_options="$(calibration_filter_size_candidates_for_server "${server_size}" "${paper_filter_size}" | paste -sd ' ' -)"
      calibration_db="$(db_for_server "${server_size}")"
      echo "[script] calibrating CLIENT_SIZE=${client_size} CALIBRATION_SERVER_SIZE=$(calibration_server_size_for_full_size "${server_size}") FULL_SERVER_SIZE=${server_size} DB=${calibration_db} FILTER_SIZES=${filter_size_options} preferred=${preferred_lkw:-none} paper=${paper_lkw:-none}"
      candidate_lkw_csv=""
      while IFS= read -r triple; do
        if [[ -n "${candidate_lkw_csv}" ]]; then
          candidate_lkw_csv="${candidate_lkw_csv},${triple}"
        else
          candidate_lkw_csv="${triple}"
        fi
      done < <(calibration_candidate_lkw "${preferred_lkw}" "${paper_lkw}")

      while IFS= read -r filter_size; do
        run_calibration_pair "${client_size}" "${server_size}" "${filter_size}" "${calibration_db}" "${candidate_lkw_csv}" "${calibration_dir}" "${all_csv}" "$@"
      done < <(calibration_filter_size_candidates_for_server "${server_size}" "${paper_filter_size}")

      if ! selected_fields="$(select_calibration_row "${all_csv}" "${client_size}" "${server_size}" "${CALIBRATION_TARGET_FP}" "${CALIBRATION_TARGET_FN}" "${calibration_target_fp_provided}")"; then
        echo "[script] could not select calibration row for CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size}" >&2
        exit 1
      fi
      read -r selected_filter_size best_L best_k best_w best_fp best_fn best_cuckoo_failed best_cuckoo_fail_rate status best_log <<< "${selected_fields}"
      if [[ "${calibration_target_fp_provided}" == "1" && "${status}" != "selected" ]]; then
        echo "[script] WARNING no calibration row met FP<=${CALIBRATION_TARGET_FP} and FN<=${CALIBRATION_TARGET_FN} for CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size}; using minimum-FP fallback L=${best_L} filter_size=${selected_filter_size} FP=${best_fp} FN=${best_fn}" >&2
      fi

      printf "%s;%s;%s;%s;%s;%s;%s;%s;%s;%s;%s;%s\n" \
        "${client_size}" \
        "${server_size}" \
        "${selected_filter_size}" \
        "${best_L}" \
        "${best_k}" \
        "${best_w}" \
        "${best_fp}" \
        "${best_fn}" \
        "${best_cuckoo_failed}" \
        "${best_cuckoo_fail_rate}" \
        "${status}" \
        "${best_log}" >> "${selected_csv}"
      echo "[script] selected calibration CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size}: L=${best_L} k=${best_k} w=${best_w} filter_size=${selected_filter_size} FP=${best_fp} FN=${best_fn} cuckoo_failed=${best_cuckoo_failed:-} cuckoo_fail_rate=${best_cuckoo_fail_rate:-} status=${status}"
    done

    previous_client_size="${client_size}"
  done

  CALIBRATED_PARAM_TABLE="${selected_csv}"
  mkdir -p "$(dirname "${CALIBRATION_CACHE_TABLE}")"
  cp "${selected_csv}" "${CALIBRATION_CACHE_TABLE}"
  echo "[script] calibration selected params: ${CALIBRATED_PARAM_TABLE}"
  echo "[script] calibration cached params: ${CALIBRATION_CACHE_TABLE}"
  echo "[script] calibration all attempts: ${all_csv}"
}

calibration_safe_args() {
  local arg

  for arg in "$@"; do
    case "${arg}" in
      --PIR_*|--OPRF|--pir_batchpir_batch_size=*|--pir_double_max_cached_shards=*)
        ;;
      *)
        printf '%s\n' "${arg}"
        ;;
    esac
  done
}

db_for_server() {
  local server_size="$1"

  if (( server_size > SMALL_PARAM_MAX_SERVER )); then
    printf '%s\n' "${BIG_DB_PRESET}"
  else
    printf '%s\n' "${SMALL_DB_PRESET}"
  fi
}

batchpir_batch_size_for_client() {
  local client_size="$1"

  if [[ "${PIR_MODE}" != "BatchPIR" ]]; then
    printf '\n'
    return 0
  fi

  if [[ "${client_size}" == "100" ]]; then
    printf '%s\n' "${CLIENT_100_PIR_BATCHPIR_BATCH_SIZE}"
  else
    printf '%s\n' "${PIR_BATCHPIR_BATCH_SIZE}"
  fi
}

arg_list_has_cuckoo_pow2() {
  local arg
  for arg in "$@"; do
    case "${arg}" in
      --cuckoo_pow2_buckets|--cuckoo_power_of_two_buckets|--Cuckoo_power_of_two_buckets)
        return 0
        ;;
    esac
  done
  return 1
}

actual_cuckoo_filter_slots_for_request() {
  local requested_filter_size="$1"
  local server_size="$2"

  awk -v size="${requested_filter_size}" -v server="${server_size}" -v pir_mode="${PIR_MODE}" '
    function ceil_div(n, d) { return int((n + d - 1) / d) }
    function next_pow2(v, p) {
      p = 1;
      while (p < v) p *= 2;
      return p;
    }
    BEGIN {
      bucket_size = 4;
      shards = 1;
      if (pir_mode == "BatchPIR") {
        if (server + 0 >= 1073741824) {
          shards = 4;
        } else if (server + 0 >= 536870912) {
          shards = 2;
        }
      }
      shard_size = ceil_div(size + 0, shards);
      buckets = next_pow2(ceil_div(shard_size, bucket_size));
      print shards * buckets * bucket_size;
    }
  '
}

run_and_calib_should_skip_pair() {
  local server_size="$1"
  local client_size="$2"
  local pair

  if [[ -n "${RUN_AND_CALIB_MIN_SERVER}" ]] && (( server_size < RUN_AND_CALIB_MIN_SERVER )); then
    return 0
  fi

  for pair in ${RUN_AND_CALIB_SKIP_PAIRS}; do
    case "${pair}" in
      "${server_size}:${client_size}"|"${server_size},${client_size}"|"${server_size}/${client_size}")
        return 0
        ;;
    esac
  done

  return 1
}

run_and_calib_server_sizes() {
  {
    awk -F',' '
      function clean(value) {
        gsub(/\r$/, "", value);
        gsub(/^"|"$/, "", value);
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
        return value;
      }
      NR == 1 {
        for (i = 1; i <= NF; ++i) col[clean($i)] = i;
        next;
      }
      {
        print clean($col["server_size"]);
      }
    ' "${PAPER_RUN_SUMMARY}"
    printf "%s\n" ${RUN_AND_CALIB_EXTRA_SERVER_SIZES}
  } | awk 'NF { print }' | sort -n -u
}

summary_params_for_server() {
  local server_size="$1"

  awk -F',' -v want_server="${server_size}" '
    function clean(value) {
      gsub(/\r$/, "", value);
      gsub(/^"|"$/, "", value);
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
      return value;
    }
    NR == 1 {
      for (i = 1; i <= NF; ++i) {
        col[clean($i)] = i;
      }
      required = "server_size client_size filter_size L k w batchpir_batch_size avg_fp avg_fn";
      split(required, names, " ");
      for (i in names) {
        if (!(names[i] in col)) {
          print "[script] summary missing column: " names[i] > "/dev/stderr";
          exit 3;
        }
      }
      next;
    }
    clean($col["server_size"]) == want_server {
      print clean($col["filter_size"]) ";" clean($col["L"]) ";" clean($col["k"]) ";" clean($col["w"]) ";" clean($col["batchpir_batch_size"]) ";" clean($col["avg_fp"]) ";" clean($col["avg_fn"]) ";" clean($col["client_size"]);
      found = 1;
      exit 0;
    }
    END {
      if (!found) exit 4;
    }
  ' "${PAPER_RUN_SUMMARY}"
}

run_and_calib_summary_rows() {
  local server_size
  local client_size
  local params_line
  local filter_size
  local old_L
  local old_k
  local old_w
  local batch_from_summary
  local paper_fp
  local paper_fn
  local paper_client
  local table_source
  local batch_size

  while IFS= read -r server_size; do
    if params_line="$(summary_params_for_server "${server_size}")"; then
      IFS=';' read -r filter_size old_L old_k old_w batch_from_summary paper_fp paper_fn paper_client <<< "${params_line}"
    else
      if ! params_line="$(params_for_server "${server_size}")"; then
        echo "[script] No starting L/k/w/filter_size found for run_and_calib server_size=${server_size}" >&2
        exit 2
      fi
      read -r old_L old_k old_w filter_size table_source <<< "${params_line}"
      batch_from_summary=""
      paper_fp=""
      paper_fn=""
      paper_client=""
    fi

    for client_size in ${RUN_AND_CALIB_CLIENT_SIZES}; do
      if run_and_calib_should_skip_pair "${server_size}" "${client_size}"; then
        continue
      fi
      batch_size="$(batchpir_batch_size_for_client "${client_size}")"
      if [[ "${client_size}" == "${paper_client}" ]]; then
        printf "%s;%s;%s;%s;%s;%s;%s;%s;%s\n" "${server_size}" "${client_size}" "${filter_size}" "${old_L}" "${old_k}" "${old_w}" "${batch_size}" "${paper_fp}" "${paper_fn}"
      else
        printf "%s;%s;%s;%s;%s;%s;%s;;\n" "${server_size}" "${client_size}" "${filter_size}" "${old_L}" "${old_k}" "${old_w}" "${batch_size}"
      fi
    done
  done < <(run_and_calib_server_sizes)
}

run_and_calib_from_summary() {
  local calibration_dir="${log_dir}/run_and_calib_calibration"
  local all_csv="${calibration_dir}/calibration_all.csv"
  local selected_csv="${calibration_dir}/calibrated_params.csv"
  local decisions_csv="${log_dir}/run_and_calib_decisions.csv"
  local initial_summary_file="${log_dir}/run_and_calib_initial_measurements.csv"
  local final_attempt_summary_file="${log_dir}/run_and_calib_final_attempts.csv"
  local final_summary_file="${log_dir}/summary.csv"
  local initial_seconds_file="${log_dir}/run_and_calib_initial_seconds.csv"
  local final_attempt_seconds_file="${log_dir}/run_and_calib_final_attempt_seconds.csv"
  local final_seconds_file="${log_dir}/run_and_calib_final_seconds.csv"
  local server_size
  local client_size
  local filter_size
  local old_L
  local old_k
  local old_w
  local paper_batchpir_batch_size
  local paper_fp
  local paper_fn
  local run_db
  local candidate_lkw_csv
  local triple
  local selected_batchpir_batch_size
  local batch_attempt
  local run_succeeded
  local candidate_batch_size
  local status
  local initial_fp
  local initial_fn
  local initial_L
  local initial_k
  local initial_w
  local current_filter_size
  local growth_step
  local seed_lkw
  local accepted
  local calibration_action
  local final_fp
  local final_fn
  local processed_count=0
  local kept_count=0
  local calibrated_count=0

  mkdir -p "${calibration_dir}"
  printf "client_size;server_size;filter_size;L;k;w;avg_fp;avg_fn;avg_cuckoo_failed;cuckoo_fail_rate;status;log\n" > "${all_csv}"
  printf "client_size;server_size;filter_size;L;k;w;avg_fp;avg_fn;avg_cuckoo_failed;cuckoo_fail_rate;status;log\n" > "${selected_csv}"
  printf "%s\n" "${summary_header}" > "${initial_summary_file}"
  printf "%s\n" "${summary_header}" > "${final_attempt_summary_file}"
  printf "%s\n" "${summary_header}" > "${final_summary_file}"
  printf "client_size,server_size,seconds\n" > "${initial_seconds_file}"
  printf "client_size,server_size,seconds\n" > "${final_attempt_seconds_file}"
  printf "client_size,server_size,seconds\n" > "${final_seconds_file}"
  printf "server_size,client_size,filter_size,old_L,old_k,old_w,paper_fp,paper_fn,initial_fp,initial_fn,action,final_L,final_k,final_w,calibration_fp,calibration_fn,final_fp,final_fn,calibration_status,calibration_log\n" > "${decisions_csv}"

  while IFS=';' read -r server_size client_size filter_size old_L old_k old_w paper_batchpir_batch_size paper_fp paper_fn; do
    processed_count=$((processed_count + 1))
    run_db="$(db_for_server "${server_size}")"
    selected_batchpir_batch_size="${paper_batchpir_batch_size}"
    if [[ -z "${selected_batchpir_batch_size}" ]]; then
      selected_batchpir_batch_size="$(batchpir_batch_size_for_client "${client_size}")"
    fi

    echo
    echo "================ RUN_AND_CALIB INITIAL SERVER_SIZE=${server_size} CLIENT_SIZE=${client_size} PAPER_FP=${paper_fp} PAPER_FN=${paper_fn} ================"
    echo "[script] first measuring with paper params under BatchPIR+OPRF: L=${old_L} k=${old_k} w=${old_w}"
    active_summary_file="${initial_summary_file}"
    active_seconds_file="${initial_seconds_file}"
    batch_attempt=1
    run_succeeded=0
    for candidate_batch_size in $(batchpir_batch_candidates "${selected_batchpir_batch_size}"); do
      if (( batch_attempt > 1 )); then
        echo "[script] retrying RUN_AND_CALIB initial CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size} with smaller BatchPIR batch_size=${candidate_batch_size}"
      fi
      if run_one_attempt "${client_size}" "${server_size}" "${filter_size}" "${candidate_batch_size}" "${batch_attempt}" "${old_L}" "${old_k}" "${old_w}" "run-and-calib-initial" "${run_db}" "$@"; then
        run_succeeded=1
        break
      else
        status="$?"
        if [[ "${status}" -ne 75 ]]; then
          exit "${status}"
        fi
      fi
      batch_attempt=$((batch_attempt + 1))
    done

    if [[ "${run_succeeded}" == "0" ]]; then
      echo "[script] all BatchPIR fallback batch sizes failed for RUN_AND_CALIB initial CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size}" >&2
      exit 75
    fi

    initial_fp="${last_fp}"
    initial_fn="${last_fn}"
    initial_L="${last_L}"
    initial_k="${last_k}"
    initial_w="${last_w}"

    active_summary_file="${final_summary_file}"
    active_seconds_file="${final_seconds_file}"
    if calibration_meets_target "${initial_fp}" "${initial_fn}"; then
      kept_count=$((kept_count + 1))
      printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,keep_initial,%s,%s,%s,,,%s,%s,initial_accepted,\n" \
        "${server_size}" \
        "${client_size}" \
        "${filter_size}" \
        "${old_L}" \
        "${old_k}" \
        "${old_w}" \
        "${paper_fp}" \
        "${paper_fn}" \
        "${initial_fp}" \
        "${initial_fn}" \
        "${initial_L}" \
        "${initial_k}" \
        "${initial_w}" \
        "${initial_fp}" \
        "${initial_fn}" >> "${decisions_csv}"
      echo "[script] run_and_calib keep initial SERVER_SIZE=${server_size} CLIENT_SIZE=${client_size}: fresh FP=${initial_fp} FN=${initial_fn}"
      tail -n 1 "${initial_summary_file}" >> "${final_summary_file}"
      tail -n 1 "${initial_seconds_file}" >> "${final_seconds_file}"
      continue
    fi

    calibrated_count=$((calibrated_count + 1))
    current_filter_size="${filter_size}"
    growth_step=0
    seed_lkw="${old_L}:${old_k}:${old_w}"
    accepted=0

    while (( growth_step <= MAX_FILTER_GROWTH_STEPS )); do
      candidate_lkw_csv=""
      while IFS= read -r triple; do
        if [[ -n "${candidate_lkw_csv}" ]]; then
          candidate_lkw_csv="${candidate_lkw_csv},${triple}"
        else
          candidate_lkw_csv="${triple}"
        fi
      done < <(calibration_candidate_lkw "${seed_lkw}" "")

      if (( growth_step == 0 )); then
        echo "[script] fresh run failed target for SERVER_SIZE=${server_size} CLIENT_SIZE=${client_size}: FP=${initial_fp} FN=${initial_fn}; calibrating without PIR/OPRF"
      else
        echo "[script] retrying calibration with larger filter for SERVER_SIZE=${server_size} CLIENT_SIZE=${client_size}: filter_size=${current_filter_size} growth_step=${growth_step}/${MAX_FILTER_GROWTH_STEPS}"
      fi

      run_calibration_pair "${client_size}" "${server_size}" "${current_filter_size}" "${run_db}" "${candidate_lkw_csv}" "${calibration_dir}" "${all_csv}" "$@"

      printf "%s;%s;%s;%s;%s;%s;%s;%s;%s;%s;%s;%s\n" \
        "${client_size}" \
        "${server_size}" \
        "${current_filter_size}" \
        "${calibration_best_L}" \
        "${calibration_best_k}" \
        "${calibration_best_w}" \
        "${calibration_best_fp}" \
        "${calibration_best_fn}" \
        "${calibration_best_cuckoo_failed}" \
        "${calibration_best_cuckoo_fail_rate}" \
        "${calibration_best_status}" \
        "${calibration_best_log}" >> "${selected_csv}"

      if [[ "${calibration_best_status}" != "selected" ]]; then
        calibration_action="calibration_miss_grow_filter"
        printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,,,%s,%s\n" \
          "${server_size}" \
          "${client_size}" \
          "${current_filter_size}" \
          "${old_L}" \
          "${old_k}" \
          "${old_w}" \
          "${paper_fp}" \
          "${paper_fn}" \
          "${initial_fp}" \
          "${initial_fn}" \
          "${calibration_action}" \
          "${calibration_best_L}" \
          "${calibration_best_k}" \
          "${calibration_best_w}" \
          "${calibration_best_fp}" \
          "${calibration_best_fn}" \
          "${calibration_best_status}" \
          "${calibration_best_log}" >> "${decisions_csv}"
        if (( growth_step >= MAX_FILTER_GROWTH_STEPS )); then
          echo "[script] ERROR calibration did not meet FP/FN target after ${MAX_FILTER_GROWTH_STEPS} filter growth steps for SERVER_SIZE=${server_size} CLIENT_SIZE=${client_size}" >&2
          exit 2
        fi
        current_filter_size="$(larger_filter_size "${current_filter_size}")"
        seed_lkw="${calibration_best_L}:${calibration_best_k}:${calibration_best_w}"
        growth_step=$((growth_step + 1))
        continue
      fi

      echo "[script] final measuring with calibrated params under BatchPIR+OPRF: filter_size=${current_filter_size} L=${calibration_best_L} k=${calibration_best_k} w=${calibration_best_w}"
      active_summary_file="${final_attempt_summary_file}"
      active_seconds_file="${final_attempt_seconds_file}"
      batch_attempt=1
      run_succeeded=0
      for candidate_batch_size in $(batchpir_batch_candidates "${selected_batchpir_batch_size}"); do
        if (( batch_attempt > 1 )); then
          echo "[script] retrying RUN_AND_CALIB final CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size} with smaller BatchPIR batch_size=${candidate_batch_size}"
        fi
        if run_one_attempt "${client_size}" "${server_size}" "${current_filter_size}" "${candidate_batch_size}" "${batch_attempt}" "${calibration_best_L}" "${calibration_best_k}" "${calibration_best_w}" "run-and-calib-final-calibrated" "${run_db}" "$@"; then
          run_succeeded=1
          break
        else
          status="$?"
          if [[ "${status}" -ne 75 ]]; then
            exit "${status}"
          fi
        fi
        batch_attempt=$((batch_attempt + 1))
      done

      if [[ "${run_succeeded}" == "0" ]]; then
        echo "[script] all BatchPIR fallback batch sizes failed for RUN_AND_CALIB final CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size}" >&2
        exit 75
      fi

      final_fp="${last_fp}"
      final_fn="${last_fn}"
      if calibration_meets_target "${final_fp}" "${final_fn}"; then
        accepted=1
        printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,accepted_calibrated,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
          "${server_size}" \
          "${client_size}" \
          "${current_filter_size}" \
          "${old_L}" \
          "${old_k}" \
          "${old_w}" \
          "${paper_fp}" \
          "${paper_fn}" \
          "${initial_fp}" \
          "${initial_fn}" \
          "${calibration_best_L}" \
          "${calibration_best_k}" \
          "${calibration_best_w}" \
          "${calibration_best_fp}" \
          "${calibration_best_fn}" \
          "${final_fp}" \
          "${final_fn}" \
          "${calibration_best_status}" \
          "${calibration_best_log}" >> "${decisions_csv}"
        tail -n 1 "${final_attempt_summary_file}" >> "${final_summary_file}"
        tail -n 1 "${final_attempt_seconds_file}" >> "${final_seconds_file}"
        active_summary_file="${final_summary_file}"
        active_seconds_file="${final_seconds_file}"
        echo "[script] accepted calibrated SERVER_SIZE=${server_size} CLIENT_SIZE=${client_size}: filter_size=${current_filter_size} final FP=${final_fp} FN=${final_fn}"
        break
      fi

      printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,final_miss_grow_filter,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
        "${server_size}" \
        "${client_size}" \
        "${current_filter_size}" \
        "${old_L}" \
        "${old_k}" \
        "${old_w}" \
        "${paper_fp}" \
        "${paper_fn}" \
        "${initial_fp}" \
        "${initial_fn}" \
        "${calibration_best_L}" \
        "${calibration_best_k}" \
        "${calibration_best_w}" \
        "${calibration_best_fp}" \
        "${calibration_best_fn}" \
        "${final_fp}" \
        "${final_fn}" \
        "${calibration_best_status}" \
        "${calibration_best_log}" >> "${decisions_csv}"

      if (( growth_step >= MAX_FILTER_GROWTH_STEPS )); then
        echo "[script] ERROR final BatchPIR+OPRF run did not meet FP/FN target after ${MAX_FILTER_GROWTH_STEPS} filter growth steps for SERVER_SIZE=${server_size} CLIENT_SIZE=${client_size}: FP=${final_fp} FN=${final_fn}" >&2
        exit 2
      fi
      echo "[script] final run missed target for SERVER_SIZE=${server_size} CLIENT_SIZE=${client_size}: FP=${final_fp} FN=${final_fn}; increasing filter size"
      current_filter_size="$(larger_filter_size "${current_filter_size}")"
      seed_lkw="${calibration_best_L}:${calibration_best_k}:${calibration_best_w}"
      growth_step=$((growth_step + 1))
    done

    if [[ "${accepted}" != "1" ]]; then
      echo "[script] ERROR no accepted result for SERVER_SIZE=${server_size} CLIENT_SIZE=${client_size}" >&2
      exit 2
    fi
  done < <(run_and_calib_summary_rows)

  active_summary_file="${summary_file}"
  active_seconds_file="${seconds_file}"
  echo "[script] run_and_calib processed=${processed_count} kept_initial=${kept_count} calibrated_and_reran=${calibrated_count}"
  echo "[script] run_and_calib initial measurements=${initial_summary_file}"
  echo "[script] run_and_calib final attempts=${final_attempt_summary_file}"
  echo "[script] run_and_calib final measurements=${final_summary_file}"
  echo "[script] run_and_calib initial seconds=${initial_seconds_file}"
  echo "[script] run_and_calib final attempt seconds=${final_attempt_seconds_file}"
  echo "[script] run_and_calib final seconds=${final_seconds_file}"
  echo "[script] run_and_calib decisions=${decisions_csv}"
  echo "[script] run_and_calib selected params=${selected_csv}"
  echo "[script] run_and_calib all calibration attempts=${all_csv}"
}

run_one_attempt() {
  local client_size="$1"
  local server_size="$2"
  local filter_size="$3"
  local batchpir_batch_size="$4"
  local attempt="$5"
  local attempt_L="$6"
  local attempt_k="$7"
  local attempt_w="$8"
  local param_source="$9"
  local run_db="${10}"
  shift 10
  local log_file
  local run_cmd
  local statuses
  local test_status
  local tee_status
  local parsed_rows
  local path_arg
  local actual_filter_size

  if [[ "${PIR_MODE}" != "BatchPIR" ]]; then
    batchpir_batch_size=""
  fi

  if [[ "${attempt}" == "1" ]]; then
    if [[ "${PIR_MODE}" == "BatchPIR" ]]; then
      log_file="${log_dir}/client_${client_size}_server_${server_size}_filter_${filter_size}_batch_${batchpir_batch_size}.log"
    else
      log_file="${log_dir}/client_${client_size}_server_${server_size}_filter_${filter_size}_${PIR_MODE}.log"
    fi
  else
    if [[ "${PIR_MODE}" == "BatchPIR" ]]; then
      log_file="${log_dir}/client_${client_size}_server_${server_size}_filter_${filter_size}_batch_${batchpir_batch_size}_attempt_${attempt}_L_${attempt_L}_k_${attempt_k}.log"
    else
      log_file="${log_dir}/client_${client_size}_server_${server_size}_filter_${filter_size}_${PIR_MODE}_attempt_${attempt}_L_${attempt_L}_k_${attempt_k}.log"
    fi
  fi

  run_cmd=("${BASE_CMD[@]}")
  run_cmd+=(--db="${run_db}")
  if [[ "${USE_PROTOCOL_SPLIT}" == "1" ]]; then
    while IFS= read -r path_arg; do
      run_cmd+=("${path_arg}")
    done < <(protocol_path_args_for_db "${run_db}")
  fi
  run_cmd+=(
    --L="${attempt_L}"
    --k="${attempt_k}"
    --w="${attempt_w}"
  )
  if [[ "${PIR_MODE}" == "BatchPIR" ]]; then
    run_cmd+=(--pir_batchpir_batch_size="${batchpir_batch_size}")
  fi
  run_cmd+=(
    --client_size="${client_size}"
    --server_size="${server_size}"
    --filter_size="${filter_size}"
    "$@"
  )

  actual_filter_size="${filter_size}"
  if arg_list_has_cuckoo_pow2 "$@"; then
    actual_filter_size="$(actual_cuckoo_filter_slots_for_request "${filter_size}" "${server_size}")"
  fi

  printf '[script] REQUESTED FILTER SIZE ON CSV: %s, ACTUAL SIZE OF FILTER: %s\n' "${filter_size}" "${actual_filter_size}"
  printf '[script] log=%s\n' "${log_file}"
  printf '[script] command:'
  printf ' %q' "${run_cmd[@]}"
  printf '\n'

  set +e
  "${run_cmd[@]}" 2>&1 | tee "${log_file}"
  statuses=("${PIPESTATUS[@]}")
  set -e
  test_status="${statuses[0]}"
  tee_status="${statuses[1]}"

  if [[ "${tee_status}" -ne 0 ]]; then
    echo "[script] tee failed with exit ${tee_status}; log may be incomplete: ${log_file}" >&2
  fi
  if [[ "${test_status}" -ne 0 ]]; then
    if [[ "${PIR_MODE}" == "BatchPIR" ]] && grep -q "parms_id is not valid for encryption parameters" "${log_file}"; then
      echo "[script] BatchPIR parameters crashed for batch_size=${batchpir_batch_size}; will retry with a smaller batch size if available." >&2
      echo "[script] failing log: ${log_file}" >&2
      return 75
    fi
    if [[ "${test_status}" -eq 137 ]]; then
      echo "[script] ./test was killed by SIGKILL (exit 137), usually because the OS/server ran out of memory." >&2
      if [[ "${DEBUG_PHASES_ENABLED}" == "1" ]]; then
        echo "[script] The last '[phase] ... start' line above is the part that was running when it died." >&2
      else
        echo "[script] Re-run with DEBUG=1 to print '[phase]' breadcrumbs before the crash." >&2
      fi
    else
      echo "[script] ./test failed with exit ${test_status}." >&2
    fi
    echo "[script] failing log: ${log_file}" >&2
    exit "${test_status}"
  fi

  parsed_rows="$(
    awk '
      function trim(text) {
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", text);
        return text;
      }
      function csv(text) {
        text = trim(text);
        gsub(/"/, "\"\"", text);
        return "\"" text "\"";
      }
      function reset_row() {
        mode = filter = server = client = filter_mb = fp = fn = total = client_runtime = server_runtime = server_offline_runtime = server_online_runtime = filter_build_runtime = pir_setup_runtime = comm = pir_query_comm = pir_response_comm = pir_setup_comm = cuckoo_failed = cuckoo_fail_rate = "";
      }
      function flush_row() {
        if (in_row && total != "") {
          print csv(mode) "," csv(filter) "," csv(server) "," csv(client) "," csv(best_L) "," csv(best_k) "," csv(best_w) "," csv(filter_mb) "," csv(fp) "," csv(fn) "," csv(total) "," csv(client_runtime) "," csv(server_runtime) "," csv(server_offline_runtime) "," csv(server_online_runtime) "," csv(filter_build_runtime) "," csv(pir_setup_runtime) "," csv(comm) "," csv(pir_query_comm) "," csv(pir_response_comm) "," csv(pir_setup_comm) "," csv(cuckoo_failed) "," csv(cuckoo_fail_rate);
        }
        in_row = 0;
        reset_row();
      }
      BEGIN {
        in_row = 0;
        reset_row();
      }
      /^\[best params\]/ {
        line = $0;
        for (i = 1; i <= split(line, parts, /[[:space:]]+/); i++) {
          split(parts[i], kv, "=");
          if (kv[1] == "L") best_L = kv[2];
          else if (kv[1] == "k") best_k = kv[2];
          else if (kv[1] == "w") best_w = kv[2];
        }
        next;
      }
      /^\[RUNTIME AVG\]/ {
        flush_row();
        in_row = 1;
        line = $0;
        sub(/^\[RUNTIME AVG\][[:space:]]*/, "", line);
        if (match(line, / filter=/)) {
          mode = substr(line, 6, RSTART - 6);
          line = substr(line, RSTART + 1);
        }
        for (i = 1; i <= split(line, parts, /[[:space:]]+/); i++) {
          split(parts[i], kv, "=");
          if (kv[1] == "filter") filter = kv[2];
          else if (kv[1] == "server_size") server = kv[2];
          else if (kv[1] == "client_size") client = kv[2];
        }
        next;
      }
      in_row && /^[[:space:]]*Filter Size:/ {
        text = $0;
        sub(/^[[:space:]]*Filter Size:[[:space:]]*/, "", text);
        sub(/[[:space:]]*MB[[:space:]]*$/, "", text);
        filter_mb = text;
        next;
      }
      in_row && /^[[:space:]]*FP Rate:/ {
        text = $0;
        sub(/^[[:space:]]*FP Rate:[[:space:]]*/, "", text);
        fp = text;
        next;
      }
      in_row && /^[[:space:]]*FN Rate:/ {
        text = $0;
        sub(/^[[:space:]]*FN Rate:[[:space:]]*/, "", text);
        fn = text;
        next;
      }
      in_row && /^[[:space:]]*Avg Cuckoo Failed Inserts:/ {
        text = $0;
        sub(/^[[:space:]]*Avg Cuckoo Failed Inserts:[[:space:]]*/, "", text);
        cuckoo_failed = text;
        next;
      }
      in_row && /^[[:space:]]*Avg Cuckoo Fail Rate:/ {
        text = $0;
        sub(/^[[:space:]]*Avg Cuckoo Fail Rate:[[:space:]]*/, "", text);
        cuckoo_fail_rate = text;
        next;
      }
      in_row && /^[[:space:]]*Total Runtime \(build.*PIR setup \+ Client query\/results\):/ {
        text = $0;
        sub(/^[[:space:]]*Total Runtime \(build.*PIR setup \+ Client query\/results\):[[:space:]]*/, "", text);
        sub(/[[:space:]]*s[[:space:]]*$/, "", text);
        total = text;
        next;
      }
      in_row && /^[[:space:]]*Client Runtime \(query generation \+ decode\/results\):/ {
        text = $0;
        sub(/^[[:space:]]*Client Runtime \(query generation \+ decode\/results\):[[:space:]]*/, "", text);
        sub(/[[:space:]]*s[[:space:]]*$/, "", text);
        client_runtime = text;
        next;
      }
      in_row && /^[[:space:]]*Server Runtime \(filter construction \+ PIR setup\/answers\):/ {
        text = $0;
        sub(/^[[:space:]]*Server Runtime \(filter construction \+ PIR setup\/answers\):[[:space:]]*/, "", text);
        sub(/[[:space:]]*s[[:space:]]*$/, "", text);
        server_runtime = text;
        next;
      }
      in_row && /^[[:space:]]*Server Offline Runtime \(filter construction \+ PIR setup\):/ {
        text = $0;
        sub(/^[[:space:]]*Server Offline Runtime \(filter construction \+ PIR setup\):[[:space:]]*/, "", text);
        sub(/[[:space:]]*s[[:space:]]*$/, "", text);
        server_offline_runtime = text;
        next;
      }
      in_row && /^[[:space:]]*Server Online Runtime \(PIR answers\):/ {
        text = $0;
        sub(/^[[:space:]]*Server Online Runtime \(PIR answers\):[[:space:]]*/, "", text);
        sub(/[[:space:]]*s[[:space:]]*$/, "", text);
        server_online_runtime = text;
        next;
      }
      in_row && /^[[:space:]]*Filter Build Runtime:/ {
        text = $0;
        sub(/^[[:space:]]*Filter Build Runtime:[[:space:]]*/, "", text);
        sub(/[[:space:]]*s[[:space:]]*$/, "", text);
        filter_build_runtime = text;
        next;
      }
      in_row && /^[[:space:]]*PIR Setup Runtime:/ {
        text = $0;
        sub(/^[[:space:]]*PIR Setup Runtime:[[:space:]]*/, "", text);
        sub(/[[:space:]]*s[[:space:]]*$/, "", text);
        pir_setup_runtime = text;
        next;
      }
      in_row && /^[[:space:]]*Total Communication Cost/ {
        text = $0;
        sub(/^[[:space:]]*Total Communication Cost[^:]*:[[:space:]]*/, "", text);
        sub(/[[:space:]]*MB[[:space:]]*$/, "", text);
        comm = text;
        next;
      }
      in_row && /^[[:space:]]*PIR Query Communication Cost \(Client -> Server\):/ {
        text = $0;
        sub(/^[[:space:]]*PIR Query Communication Cost \(Client -> Server\):[[:space:]]*/, "", text);
        sub(/[[:space:]]*MB[[:space:]]*$/, "", text);
        pir_query_comm = text;
        next;
      }
      in_row && /^[[:space:]]*PIR Answer Communication Cost \(Server -> Client\):/ {
        text = $0;
        sub(/^[[:space:]]*PIR Answer Communication Cost \(Server -> Client\):[[:space:]]*/, "", text);
        sub(/[[:space:]]*MB[[:space:]]*$/, "", text);
        pir_response_comm = text;
        next;
      }
      in_row && /^[[:space:]]*PIR Setup Exchange Cost \(metadata\/keys, not DB build\):/ {
        text = $0;
        sub(/^[[:space:]]*PIR Setup Exchange Cost \(metadata\/keys, not DB build\):[[:space:]]*/, "", text);
        sub(/[[:space:]]*MB[[:space:]]*$/, "", text);
        pir_setup_comm = text;
        next;
      }
      END {
        flush_row();
      }
    ' "${log_file}"
  )"

  if [[ -z "${parsed_rows}" ]]; then
    echo "No [RUNTIME AVG] block found for Client size ${client_size}, Server size ${server_size}" >&2
    exit 1
  fi

  while IFS=, read -r mode filter server client parsed_L parsed_k parsed_w filter_mb fp fn total client_runtime server_runtime server_offline_runtime server_online_runtime filter_build_runtime pir_setup_runtime comm pir_query_comm pir_response_comm pir_setup_comm cuckoo_failed cuckoo_fail_rate; do
    fp_plain="${fp//\"/}"
    fn_plain="${fn//\"/}"
    total_plain="${total//\"/}"
    server_plain="${server//\"/}"
    client_plain="${client//\"/}"
    filter_mb_plain="${filter_mb//\"/}"
    parsed_L_plain="${parsed_L//\"/}"
    parsed_k_plain="${parsed_k//\"/}"
    parsed_w_plain="${parsed_w//\"/}"
    client_runtime_plain="${client_runtime//\"/}"
    server_runtime_plain="${server_runtime//\"/}"
    comm_plain="${comm//\"/}"
    pir_query_comm_plain="${pir_query_comm//\"/}"
    pir_response_comm_plain="${pir_response_comm//\"/}"
    pir_setup_comm_plain="${pir_setup_comm//\"/}"
    filter_build_runtime_plain="${filter_build_runtime//\"/}"
    pir_setup_runtime_plain="${pir_setup_runtime//\"/}"
    server_offline_runtime_plain="${server_offline_runtime//\"/}"
    server_online_runtime_plain="${server_online_runtime//\"/}"
    cuckoo_failed_plain="${cuckoo_failed//\"/}"
    cuckoo_fail_rate_plain="${cuckoo_fail_rate//\"/}"
    meets_target="$(
      awk -v fp="${fp_plain}" -v fn="${fn_plain}" -v target_fp="${MEASUREMENT_TARGET_FP}" -v target_fn="${MEASUREMENT_TARGET_FN}" 'BEGIN {
        if (fp != "" && fn != "" && fp != "nan" && fn != "nan" && fp != "NaN" && fn != "NaN" && fp + 0 < target_fp + 0 && fn + 0 < target_fn + 0) print "true";
        else print "false";
      }'
    )"
    paper_metrics="$(
      awk -v size="${filter_size}" -v total_s="${total_plain}" -v build_s="${filter_build_runtime_plain}" 'BEGIN {
        buckets = int(size / 4);
        db_rows = 65536;
        if (buckets <= 1024) db_rows = 1024;
        else if (buckets <= 4096) db_rows = 4096;
        else if (buckets <= 16384) db_rows = 16384;
        shards = int((buckets + db_rows - 1) / db_rows);
        total_bits = size * 32;
        build_ms = (build_s == "" ? "" : build_s * 1000.0);
        total_ms = (total_s == "" ? "" : total_s * 1000.0);
        printf "%d %d %d %d %s %s\n", shards, db_rows, buckets, total_bits, build_ms, total_ms;
      }'
    )"
    read -r pir_single_shards pir_single_db_rows buckets total_size_bits avg_filter_build_time_ms avg_build_setup_query_ms <<< "${paper_metrics}"
    avg_pir_setup_time_ms="$(
      awk -v setup_s="${pir_setup_runtime_plain}" 'BEGIN {
        if (setup_s == "") print "";
        else print setup_s * 1000.0;
      }'
    )"

    printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
      "${server_plain}" \
      "${client_plain}" \
      "${fp_plain}" \
      "${fn_plain}" \
      "${total_plain}" \
      "${total_plain}" \
      "${client_runtime_plain}" \
      "${server_runtime_plain}" \
      "${server_offline_runtime_plain}" \
      "${server_online_runtime_plain}" \
      "${avg_filter_build_time_ms}" \
      "${avg_pir_setup_time_ms}" \
      "" \
      "" \
      "${avg_build_setup_query_ms}" \
      "${avg_build_setup_query_ms}" \
      "${comm_plain}" \
      "${pir_query_comm_plain}" \
      "${pir_response_comm_plain}" \
      "${pir_setup_comm_plain}" \
      "${cuckoo_failed_plain}" \
      "${cuckoo_fail_rate_plain}" \
      "${filter_size}" \
      "${PIR_MODE}" \
      "${parsed_L_plain}" \
      "${parsed_k_plain}" \
      "${parsed_w_plain}" \
      "${pir_single_shards}" \
      "${pir_single_db_rows}" \
      "${batchpir_batch_size}" \
      "${param_source}" \
      "${buckets}" \
      "${total_size_bits}" \
      "${filter_mb_plain}" \
      "\"${log_file}\"" >> "${active_summary_file}"

    printf "%s,%s,%s\n" "${client_plain}" "${server_plain}" "${total_plain}" >> "${active_seconds_file}"

    if [[ -z "${max_time}" ]] || awk -v candidate="${total_plain}" -v current="${max_time}" 'BEGIN { exit !(candidate > current) }'; then
      max_time="${total_plain}"
      max_server="${server_plain}"
      max_client="${client_plain}"
      max_filter_mb="${filter_mb_plain}"
      max_log="${log_file}"
    fi

    last_fp="${fp_plain}"
    last_fn="${fn_plain}"
    last_L="${parsed_L_plain}"
    last_k="${parsed_k_plain}"
    last_w="${parsed_w_plain}"
  done <<< "${parsed_rows}"
}

if [[ "${CALIBRATE}" == "1" ]]; then
  if [[ "${FRESH_CALIBRATION}" != "1" && -f "${CALIBRATION_CACHE_TABLE}" ]]; then
    CALIBRATED_PARAM_TABLE="${CALIBRATION_CACHE_TABLE}"
    echo "[script] calibration cache hit: ${CALIBRATED_PARAM_TABLE}; skipping calibration"
  else
    if [[ "${FRESH_CALIBRATION}" == "1" && -f "${CALIBRATION_CACHE_TABLE}" ]]; then
      echo "[script] fresh calibration requested; ignoring existing cache: ${CALIBRATION_CACHE_TABLE}"
    else
      echo "[script] calibration cache miss: ${CALIBRATION_CACHE_TABLE}; running calibration"
    fi
    calibration_args=()
    while IFS= read -r calibration_arg; do
      calibration_args+=("${calibration_arg}")
    done < <(calibration_safe_args "$@")
    if (( ${#calibration_args[@]} > 0 )); then
      run_big_server_calibration "${calibration_args[@]}"
    else
      run_big_server_calibration
    fi
  fi
  if [[ "${CALIBRATE_ONLY}" == "1" ]]; then
    echo
    echo "================ CALIBRATION_ONLY OUTPUTS ================"
    echo "output_dir=${log_dir}"
    echo "selected_params=${CALIBRATED_PARAM_TABLE}"
    echo "calibration_cache=${CALIBRATION_CACHE_TABLE}"
    if [[ -f "${log_dir}/calibration/calibration_all.csv" ]]; then
      echo "all_calibration_attempts=${log_dir}/calibration/calibration_all.csv"
    fi
    exit 0
  fi
fi

if [[ "${RUN_AND_CALIB}" == "1" ]]; then
  run_and_calib_args=()
  while IFS= read -r run_and_calib_arg; do
    run_and_calib_args+=("${run_and_calib_arg}")
  done < <(calibration_safe_args "$@")
  if (( ${#run_and_calib_args[@]} > 0 )); then
    run_and_calib_from_summary "${run_and_calib_args[@]}"
  else
    run_and_calib_from_summary
  fi
  echo
  echo "================ RUN_AND_CALIB OUTPUTS ================"
  echo "output_dir=${log_dir}"
  echo
  echo "================ MAX BUILD+SETUP+QUERY TIME ================"
  echo "client_size=${max_client} server_size=${max_server} filter_size_mb=${max_filter_mb} max_total_runtime_s=${max_time}"
  echo "initial_measurements=${log_dir}/run_and_calib_initial_measurements.csv"
  echo "final_measurements=${log_dir}/summary.csv"
  echo "initial_seconds=${log_dir}/run_and_calib_initial_seconds.csv"
  echo "final_seconds=${log_dir}/run_and_calib_final_seconds.csv"
  echo "decisions=${log_dir}/run_and_calib_decisions.csv"
  echo "log=${max_log}"
  exit 0
fi

for client_size in "${CLIENT_SIZES[@]}"; do
  for server_size in "${SERVER_SIZES[@]}"; do

  selected_db="$(db_for_server "${server_size}")"
  selected_batchpir_batch_size="$(batchpir_batch_size_for_client "${client_size}")"

  if ! params_line="$(params_for_pair "${client_size}" "${server_size}")"; then
    echo "[script] No L/k/w/filter_size found for client_size=${client_size} server_size=${server_size} in calibrated params or ${PARAM_TABLE}" >&2
    exit 2
  fi
  read -r selected_L selected_k selected_w filter_size selected_param_source <<< "${params_line}"

  echo
  if [[ "${PIR_MODE}" == "BatchPIR" ]]; then
    echo "================ CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size} DB=${selected_db} FILTER_SIZE=${filter_size} PIR=${PIR_MODE} BATCH_SIZE=${selected_batchpir_batch_size} L=${selected_L} k=${selected_k} w=${selected_w} (${selected_param_source}) ================"
  else
    echo "================ CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size} DB=${selected_db} FILTER_SIZE=${filter_size} PIR=${PIR_MODE} L=${selected_L} k=${selected_k} w=${selected_w} (${selected_param_source}) ================"
  fi
  batch_attempt=1
  run_succeeded=0
  for candidate_batch_size in $(batchpir_batch_candidates "${selected_batchpir_batch_size}"); do
    if (( batch_attempt > 1 )); then
      echo "[script] retrying CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size} with smaller BatchPIR batch_size=${candidate_batch_size}"
    fi

    if run_one_attempt "${client_size}" "${server_size}" "${filter_size}" "${candidate_batch_size}" "${batch_attempt}" "${selected_L}" "${selected_k}" "${selected_w}" "${selected_param_source}" "${selected_db}" "$@"; then
      run_succeeded=1
      break
    else
      status="$?"
      if [[ "${status}" -ne 75 ]]; then
        exit "${status}"
      fi
    fi
    batch_attempt=$((batch_attempt + 1))
  done

  if [[ "${run_succeeded}" == "0" ]]; then
    if [[ "${PIR_MODE}" == "BatchPIR" ]]; then
      echo "[script] all BatchPIR fallback batch sizes failed for CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size}" >&2
    else
      echo "[script] ${PIR_MODE} run failed for CLIENT_SIZE=${client_size} SERVER_SIZE=${server_size}" >&2
    fi
    exit 75
  fi
  done
done

echo
echo "================ BUILD+SETUP+QUERY SECONDS ================"
cat "${seconds_file}"
echo
echo "================ MAX BUILD+SETUP+QUERY TIME ================"
echo "client_size=${max_client} server_size=${max_server} filter_size_mb=${max_filter_mb} max_total_runtime_s=${max_time}"
echo "summary=${summary_file}"
echo "seconds=${seconds_file}"
echo "log=${max_log}"
