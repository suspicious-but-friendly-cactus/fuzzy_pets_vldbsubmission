#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  ./run_cuckoo_client_sweep.sh [MODE] [--target-fp FP|min_fp] [-target-fp FP|min_fp] [db=MNIST|FashionMNIST] [--calibration-20|--calibration-100] [--calibration-server-100] [--calibration-client-percent=N] [extra ./test args...]

Modes:
  calibrate              Run direct Cuckoo calibration first, then measurements.
  calibrate_only         Run calibration and stop after writing selected params.

Default sweep:
  Fixed Server size: 5139
  Client sizes:      50 100 150 200 250 300 350 400 450 500

Selection:
  --target-fp FP         Select calibrated params with FP<=FP and FN<=0.015.
                         Among passing rows, minimize L, then filter_size.
  --target-fp min_fp     Select the minimum-possible-FP calibration.
  -target-fp min_fp      Alias for --target-fp min_fp.
  omitted               Select the minimum-possible-FP calibration.
  db=MNIST              Set DB_PRESET. Also accepts db=FashionMNIST or --db=...
  --calibration-20       Calibrate on stored 20% split, run protocol on the other 80%.
  --calibration-100      Calibrate and run protocol on 100% of the active dataset.
  --calibration-percent=N
                         Materialize N% for both Server and Client before running.
  --calibration-server-100
                         Use 100% actual Server rows for calibration.
  --calibration-server-percent=N
                         Materialize N% of Server rows for calibration.
  --calibration-client-percent=N|max
                         Materialize N% of Client close/far rows for calibration.
  --calibration-shadow=N Add N noisy shadow copies to calibration data only.
  --calibration-shadow-noise=STD
                         Gaussian pixel noise stddev for shadow copies.

Important environment variables:
  SERVER_SIZE            Fixed Server size. Default: 5139.
  CLIENT_SIZES           Space-separated Client sizes. Default: 50..500 step 50.
  DB_PRESET              ./test --db preset. Default: MNIST.
  OPRF_MECHANISM         GCAES or ECNR. Default: GCAES.
  OPRF_ADDR              HOST:PORT for interactive OPRF. Default: 127.0.0.1:50051.
  CALIBRATION_CACHE_TABLE Override cache path.
  CALIBRATION_DATA_PERCENT Default: 20.
  CALIBRATION_SERVER_PERCENT Default: 100.
  CALIBRATION_CLIENT_PERCENT Default: max.
  CALIBRATION_SHADOW_COUNT Default: 0.
  CALIBRATION_SHADOW_NOISE_STD Default: 0.
  CALIBRATION_TARGET_FN  Default: 0.015.
                         For db=acm_dblp/acm_dplp default: 0.08.
  CALIBRATION_REQUIRE_ZERO_CUCKOO_FAILED
                         Default: 1. Calibration only selects rows with
                         cuckoo_failed=0.
  ACM_DBLP_CUCKOO_POW2   Add --cuckoo_pow2_buckets by default for ACM-DBLP.
                         Default: 1.
  Extra ./test args such as --calibration_holdout are forwarded to calibration.

Examples:
  ./run_cuckoo_client_sweep.sh calibrate_only
  ./run_cuckoo_client_sweep.sh --target-fp 0.03
  ./run_cuckoo_client_sweep.sh db=FashionMNIST calibrate_only
  CALIBRATION_L_VALUES="10 20" CALIBRATION_K_VALUES="10" CALIBRATION_W_VALUES="1600 2000" ./run_cuckoo_client_sweep.sh calibrate_only

Outputs:
  Results are written under results/cuckoo_client_sweep_<db>_server_<server>_<timestamp>/.
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
CALIBRATE="${CALIBRATE:-auto}"
CALIBRATE_ONLY="${CALIBRATE_ONLY:-0}"
CALIBRATION_DATA_PERCENT_OVERRIDE=""
CALIBRATION_SERVER_PERCENT_OVERRIDE=""
CALIBRATION_CLIENT_PERCENT_OVERRIDE=""
CALIBRATION_SHADOW_COUNT_OVERRIDE=""
CALIBRATION_SHADOW_NOISE_STD_OVERRIDE=""

if [[ $# -gt 0 ]]; then
  case "$1" in
    calibrate|--calibrate)
      CALIBRATE=1
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
    --db=*|db=*)
      DB_PRESET="${1#--db=}"
      DB_PRESET="${DB_PRESET#db=}"
      shift
      ;;
    --target-fp=*|-target-fp=*)
      target_fp="${1#--target-fp=}"
      target_fp="${target_fp#-target-fp=}"
      target_fp_provided=1
      shift
      ;;
    --db|db)
      if [[ $# -lt 2 ]]; then
        echo "Usage: $0 [db=MNIST|--db MNIST] [extra ./test args...]" >&2
        exit 2
      fi
      DB_PRESET="$2"
      shift 2
      ;;
    --target-fp|-target-fp)
      if [[ $# -lt 2 ]]; then
        echo "Usage: $0 [--target-fp TARGET_FP] [extra ./test args...]" >&2
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
    calibrate_only|--calibrate-only|--calibrate_only)
      CALIBRATE=1
      CALIBRATE_ONLY=1
      ;;
    --calibration-20|--calibration_20)
      CALIBRATION_DATA_PERCENT_OVERRIDE="20"
      ;;
    --calibration-100|--calibration_100|--calibration-full|--calibration_full)
      CALIBRATION_DATA_PERCENT_OVERRIDE="100"
      ;;
    --calibration-percent=*)
      CALIBRATION_DATA_PERCENT_OVERRIDE="${arg#--calibration-percent=}"
      ;;
    --calibration_percent=*)
      CALIBRATION_DATA_PERCENT_OVERRIDE="${arg#--calibration_percent=}"
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
    --calibration-shadow=*)
      CALIBRATION_SHADOW_COUNT_OVERRIDE="${arg#--calibration-shadow=}"
      ;;
    --calibration_shadow=*)
      CALIBRATION_SHADOW_COUNT_OVERRIDE="${arg#--calibration_shadow=}"
      ;;
    --calibration-shadow)
      echo "--calibration-shadow requires --calibration-shadow=N form" >&2
      exit 2
      ;;
    --calibration-shadow-noise=*)
      CALIBRATION_SHADOW_NOISE_STD_OVERRIDE="${arg#--calibration-shadow-noise=}"
      ;;
    --calibration_shadow_noise=*)
      CALIBRATION_SHADOW_NOISE_STD_OVERRIDE="${arg#--calibration_shadow_noise=}"
      ;;
    --db=*|db=*)
      DB_PRESET="${arg#--db=}"
      DB_PRESET="${DB_PRESET#db=}"
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

CALIBRATION_REQUIRE_ZERO_CUCKOO_FAILED="${CALIBRATION_REQUIRE_ZERO_CUCKOO_FAILED:-1}"

DB_PRESET="${DB_PRESET:-MNIST}"
if [[ "${DB_PRESET}" == "default" ]]; then
  DB_PRESET="gowalla"
fi
if [[ "${DB_PRESET}" == "FashionMNST" || "${DB_PRESET}" == "fashionmnst" ]]; then
  DB_PRESET="FashionMNIST"
fi
if [[ "${DB_PRESET}" == "dblp_acm" || "${DB_PRESET}" == "acm_dplp" ]]; then
  DB_PRESET="acm_dblp"
fi
ACM_DBLP_DB=0
if [[ "${DB_PRESET}" == "acm_dblp" ]]; then
  ACM_DBLP_DB=1
fi

if [[ "${CALIBRATE}" == "auto" ]]; then
  CALIBRATE=1
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
calibration_target_fp_provided=0
if [[ "${target_is_min_fp}" == "0" ]]; then
  calibration_target_fp_provided=1
fi

target_label="$(
  awk -v value="${target_fp}" -v min_fp="${target_is_min_fp}" 'BEGIN {
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
calibration_cache_label="$(
  awk -v value="${target_fp}" -v min_fp="${target_is_min_fp}" -v zero_failed="${CALIBRATION_REQUIRE_ZERO_CUCKOO_FAILED}" 'BEGIN {
    if (min_fp == "1") {
      label = "min_fp";
      if (zero_failed == "1") label = label "_zero_cuckoo_failed";
      print label;
      exit;
    }
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
    gsub(/\./, "_", value);
    gsub(/[^0-9A-Za-z_+-]/, "_", value);
    label = "fp_" value;
    if (zero_failed == "1") label = label "_zero_cuckoo_failed";
    print label;
  }'
)"

if [[ "${ACM_DBLP_DB}" == "1" ]]; then
  SERVER_SIZE="${SERVER_SIZE:-2616}"
  read -r -a CLIENT_SIZES <<< "${CLIENT_SIZES:-20 40 60 80 100}"
else
  SERVER_SIZE="${SERVER_SIZE:-5139}"
  read -r -a CLIENT_SIZES <<< "${CLIENT_SIZES:-50 100 150 200 250 300 350 400 450 500}"
fi
PIR_BATCHPIR_BATCH_SIZE="${PIR_BATCHPIR_BATCH_SIZE:-200}"
CLIENT_100_PIR_BATCHPIR_BATCH_SIZE="${CLIENT_100_PIR_BATCHPIR_BATCH_SIZE:-100}"
PIR_BATCHPIR_FALLBACK_SIZES="${PIR_BATCHPIR_FALLBACK_SIZES:-200 100 64 32 16 8}"
if [[ "${target_is_min_fp}" == "0" ]]; then
  CALIBRATION_TARGET_FP="${target_fp}"
else
  CALIBRATION_TARGET_FP="${CALIBRATION_TARGET_FP:-1.0}"
fi
if [[ "${ACM_DBLP_DB}" == "1" ]]; then
  CALIBRATION_TARGET_FN="${CALIBRATION_TARGET_FN:-0.08}"
  CALIBRATION_LKW_CANDIDATES="${CALIBRATION_LKW_CANDIDATES:-8:8:3 8:10:3 10:8:3 10:10:3 12:10:3 15:10:3 10:12:3 12:12:3 15:12:3 15:15:3 8:8:5 8:10:5 10:8:5 10:10:5 12:10:5 15:10:5 10:12:5 12:12:5 15:12:5 15:15:5 8:8:7 8:10:7 10:8:7 10:10:7 12:10:7 15:10:7 10:12:7 12:12:7 15:12:7 15:15:7 20:15:7 8:8:9 8:10:9 10:8:9 10:10:9 12:10:9 15:10:9 10:12:9 12:12:9 15:12:9 15:15:9 20:15:9 10:10:12 12:10:12 15:10:12 10:12:12 12:12:12 15:12:12 15:15:12 20:15:12}"
else
  CALIBRATION_TARGET_FN="${CALIBRATION_TARGET_FN:-0.015}"
  CALIBRATION_LKW_CANDIDATES="${CALIBRATION_LKW_CANDIDATES:-10:10:1600 10:10:1800 10:10:2000 12:10:2000 15:10:2000 20:10:2500 10:15:1800 15:15:2000 20:15:2500 20:20:2500 20:12:2500 25:12:2500 30:12:2500 20:15:3000 25:15:3000 30:15:3000 35:15:3500 40:15:3500 35:20:4000 40:20:4500 45:25:5000}"
fi
CALIBRATION_L_VALUES="${CALIBRATION_L_VALUES:-1 3 5 7 10 12 15 17 20 25 30 35 40 45 50 60}"
CALIBRATION_K_VALUES="${CALIBRATION_K_VALUES:-1 3 5 7 10 12 15 17 20 25 30 35 40 45}"
CALIBRATION_W_VALUES="${CALIBRATION_W_VALUES:-800 1000 1200 1400 1600 1800 2000 2500 3000 3500 4000 4500 5000 6000}"
CALIBRATION_FILTER_SIZE_PERCENT_INCREASES="${CALIBRATION_FILTER_SIZE_PERCENT_INCREASES:-33 66 100 150 200 300}"
CALIBRATION_ABORT_FAIL_RATE="${CALIBRATION_ABORT_FAIL_RATE:-0.02}"
CALIBRATION_ABORT_FILL_RATIO="${CALIBRATION_ABORT_FILL_RATIO:-0.98}"
CALIBRATION_ABORT_MIN_ATTEMPTS="${CALIBRATION_ABORT_MIN_ATTEMPTS:-1000000}"
CALIBRATION_PROGRESS_STEPS="${CALIBRATION_PROGRESS_STEPS:-20}"
CALIBRATION_DIR="${CALIBRATION_DIR:-datasets/calibrations}"
CALIBRATION_DATA_PERCENT="${CALIBRATION_DATA_PERCENT_OVERRIDE:-${CALIBRATION_DATA_PERCENT:-20}}"
CALIBRATION_SERVER_PERCENT="${CALIBRATION_SERVER_PERCENT_OVERRIDE:-${CALIBRATION_SERVER_PERCENT:-100}}"
CALIBRATION_CLIENT_PERCENT="${CALIBRATION_CLIENT_PERCENT_OVERRIDE:-${CALIBRATION_CLIENT_PERCENT:-max}}"
CALIBRATION_SHADOW_COUNT="${CALIBRATION_SHADOW_COUNT_OVERRIDE:-${CALIBRATION_SHADOW_COUNT:-0}}"
CALIBRATION_SHADOW_NOISE_STD="${CALIBRATION_SHADOW_NOISE_STD_OVERRIDE:-${CALIBRATION_SHADOW_NOISE_STD:-0}}"
OPRF_MECHANISM="${OPRF_MECHANISM:-GCAES}"
OPRF_ADDR="${OPRF_ADDR:-127.0.0.1:50051}"
ACM_DBLP_CUCKOO_POW2="${ACM_DBLP_CUCKOO_POW2:-1}"

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

CALIBRATION_PATH_ARGS=()
PROTOCOL_PATH_ARGS=()
CALIBRATION_SERVER_SIZE="${SERVER_SIZE}"
PROTOCOL_SERVER_SIZE="${SERVER_SIZE}"

metadata_count_or_default() {
  local path="$1"
  local fallback="$2"
  local value

  if [[ -f "${path}" ]]; then
    value="$(awk 'NR == 1 { print $1; exit }' "${path}")"
    if [[ "${value}" =~ ^[0-9]+$ ]] && (( value > 0 )); then
      printf '%s\n' "${value}"
      return 0
    fi
  fi

  printf '%s\n' "${fallback}"
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
    # Use a tiny margin below the rounded boundary so round(percent*n/100)
    # does not consume the last required protocol row.
    pct -= 0.001;
    if (pct < 0) pct = 0;
    printf "%.3f\n", pct;
  }'
}

prepare_calibration_holdout() {
  local prefix
  local server_src
  local close_src
  local far_src
  local split_output
  local source_paths=()

  prefix="$(dataset_prefix_for_db "${DB_PRESET}")"
  while IFS= read -r path; do
    source_paths+=("${path}")
  done < <(source_paths_for_db "${DB_PRESET}" || true)
  if (( ${#source_paths[@]} != 3 )); then
    echo "[script] no stored calibration split rule for db=${DB_PRESET}; using paths from preset directly" >&2
    return 0
  fi
  server_src="${source_paths[0]}"
  close_src="${source_paths[1]}"
  far_src="${source_paths[2]}"
  if [[ ! -f "${server_src}" || ! -f "${close_src}" || ! -f "${far_src}" ]]; then
    echo "[script] source dataset files missing for db=${DB_PRESET}; using paths from preset directly" >&2
    return 0
  fi
  if [[ "${CALIBRATION_CLIENT_PERCENT}" == "max" || "${CALIBRATION_CLIENT_PERCENT}" == "MAX" ]]; then
    CALIBRATION_CLIENT_PERCENT="$(
      max_client_calibration_percent "${close_src}" "${far_src}"
    )"
    echo "[script] resolved --calibration-client-percent=max to ${CALIBRATION_CLIENT_PERCENT}% for max CLIENT_SIZE=$(max_requested_client_size)"
  fi

  split_output="$(
    python3 datasets/split_calibration_holdout.py \
      --server "${server_src}" \
      --client-close "${close_src}" \
      --client-far "${far_src}" \
      --output-dir "${CALIBRATION_DIR}" \
      --prefix "${prefix}" \
      --percent "${CALIBRATION_DATA_PERCENT}" \
      --server-percent "${CALIBRATION_SERVER_PERCENT}" \
      --client-percent "${CALIBRATION_CLIENT_PERCENT}" \
      --shadow-count "${CALIBRATION_SHADOW_COUNT}" \
      --shadow-noise-std "${CALIBRATION_SHADOW_NOISE_STD}"
  )"
  printf '%s\n' "${split_output}"

  CALIBRATION_PATH_ARGS=(
    "--server_path=${CALIBRATION_DIR}/${prefix}_calibration_server.json"
    "--client_close_path=${CALIBRATION_DIR}/${prefix}_calibration_client_close.json"
    "--client_far_path=${CALIBRATION_DIR}/${prefix}_calibration_client_far.json"
    "--metadata_path=${CALIBRATION_DIR}/${prefix}_calibration_metadata.txt"
  )
  PROTOCOL_PATH_ARGS=(
    "--server_path=${CALIBRATION_DIR}/${prefix}_protocol_server.json"
    "--client_close_path=${CALIBRATION_DIR}/${prefix}_protocol_client_close.json"
    "--client_far_path=${CALIBRATION_DIR}/${prefix}_protocol_client_far.json"
    "--metadata_path=${CALIBRATION_DIR}/${prefix}_protocol_metadata.txt"
  )
  CALIBRATION_SERVER_SIZE="$(
    metadata_count_or_default "${CALIBRATION_DIR}/${prefix}_calibration_metadata.txt" "${SERVER_SIZE}"
  )"
  PROTOCOL_SERVER_SIZE="$(
    metadata_count_or_default "${CALIBRATION_DIR}/${prefix}_protocol_metadata.txt" "${SERVER_SIZE}"
  )"
}

prepare_calibration_holdout

calibration_percent_label="$(
  awk -v server="${CALIBRATION_SERVER_PERCENT}" -v client="${CALIBRATION_CLIENT_PERCENT}" 'BEGIN {
    value = "server_" server "_client_" client;
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
    gsub(/\./, "_", value);
    gsub(/[^0-9A-Za-z_+-]/, "_", value);
    print value;
  }'
)"
calibration_shadow_label="$(
  awk -v count="${CALIBRATION_SHADOW_COUNT}" -v noise="${CALIBRATION_SHADOW_NOISE_STD}" 'BEGIN {
    label = "shadow_" count "_noise_" noise;
    gsub(/\./, "_", label);
    gsub(/[^0-9A-Za-z_+-]/, "_", label);
    print label;
  }'
)"
CALIBRATION_CACHE_TABLE="${CALIBRATION_CACHE_TABLE:-${CALIBRATION_DIR}/calibration_${DB_PRESET}_calib_${calibration_percent_label}_${calibration_shadow_label}_filter_scaled_${calibration_cache_label}.csv}"

echo "[script] fixed server size: ${SERVER_SIZE}"
echo "[script] calibration server size: ${CALIBRATION_SERVER_SIZE}"
echo "[script] protocol server size: ${PROTOCOL_SERVER_SIZE}"
echo "[script] client sizes: ${CLIENT_SIZES[*]}"
echo "[script] db preset: ${DB_PRESET}"
echo "[script] calibration cache: ${CALIBRATION_CACHE_TABLE}"
echo "[script] calibration data: server=${CALIBRATION_SERVER_PERCENT}% client=${CALIBRATION_CLIENT_PERCENT}% under ${CALIBRATION_DIR}; protocol uses complementary Client data unless percent is 100"
echo "[script] calibration shadows: count=${CALIBRATION_SHADOW_COUNT} noise_std=${CALIBRATION_SHADOW_NOISE_STD} (calibration files only)"
echo "[script] OPRF mechanism: ${OPRF_MECHANISM} at ${OPRF_ADDR}"
if [[ -n "${CALIBRATION_LKW_CANDIDATES}" ]]; then
  echo "[script] calibration L/k/w candidates: ${CALIBRATION_LKW_CANDIDATES}"
else
  echo "[script] calibration L values: ${CALIBRATION_L_VALUES}"
  echo "[script] calibration k values: ${CALIBRATION_K_VALUES}"
  echo "[script] calibration w values: ${CALIBRATION_W_VALUES}"
fi
if [[ "${calibration_target_fp_provided}" == "1" ]]; then
  echo "[script] calibration selection: FP<=${CALIBRATION_TARGET_FP}, FN<=${CALIBRATION_TARGET_FN}; minimize L then filter_size"
else
  echo "[script] calibration selection: minimum possible FP among rows with FN<=${CALIBRATION_TARGET_FN}"
fi
if [[ "${CALIBRATION_REQUIRE_ZERO_CUCKOO_FAILED}" == "1" ]]; then
  echo "[script] calibration selection additionally requires cuckoo_failed=0"
fi
echo "[script] calibration base filter sizes: server_size plus {${CALIBRATION_FILTER_SIZE_PERCENT_INCREASES}} percent, rounded up to a multiple of 4"
echo "[script] calibration actual filter size: base_filter_size * L"

if (( ${#PROTOCOL_PATH_ARGS[@]} > 0 )); then
  protocol_close_path=""
  protocol_far_path=""
  for arg in "${PROTOCOL_PATH_ARGS[@]}"; do
    case "${arg}" in
      --client_close_path=*) protocol_close_path="${arg#--client_close_path=}" ;;
      --client_far_path=*) protocol_far_path="${arg#--client_far_path=}" ;;
    esac
  done
  if [[ -n "${protocol_close_path}" && -n "${protocol_far_path}" ]]; then
    read -r protocol_close_rows protocol_far_rows < <(json_client_counts "${protocol_close_path}" "${protocol_far_path}")
    echo "[script] protocol client pool: close_rows=${protocol_close_rows} far_rows=${protocol_far_rows}"
    for requested_client_size in "${CLIENT_SIZES[@]}"; do
      needed_close=$(( requested_client_size / 2 ))
      needed_far=$(( requested_client_size - needed_close ))
      if (( protocol_close_rows < needed_close || protocol_far_rows < needed_far )); then
        echo "[script] ERROR protocol client pool too small for CLIENT_SIZE=${requested_client_size}: need close=${needed_close} far=${needed_far}, have close=${protocol_close_rows} far=${protocol_far_rows}" >&2
        echo "[script] Reduce --calibration-client-percent, reduce CLIENT_SIZES, or allow a less strict far threshold." >&2
        exit 2
      fi
    done
  fi
fi

BASE_CMD=(
  ./test
  --filter=Cuckoo
  --PIR_BatchPIR
  --OPRF
  "--oprf_mechanism=${OPRF_MECHANISM}"
  "--oprf_addr=${OPRF_ADDR}"
  --num_runs=1
)

CALIBRATION_BASE_CMD=(
  ./test
  --filter=Cuckoo
  --num_runs=1
)

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

case "${ACM_DBLP_DB}:${ACM_DBLP_CUCKOO_POW2}" in
  1:1|1:true|1:TRUE|1:yes|1:YES|1:on|1:ON)
    if ! arg_list_has_cuckoo_pow2 "$@"; then
      BASE_CMD+=(--cuckoo_pow2_buckets)
      CALIBRATION_BASE_CMD+=(--cuckoo_pow2_buckets)
    fi
    ;;
esac

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

timestamp="$(date +%Y%m%d_%H%M%S)"
log_dir="results/cuckoo_client_sweep_${DB_PRESET}_server_${SERVER_SIZE}_${timestamp}"
mkdir -p "${log_dir}"
summary_file="${log_dir}/summary_target_${target_label}.csv"
seconds_file="${log_dir}/seconds_target_${target_label}.csv"
summary_header="server_size,client_size,avg_fp,avg_fn,avg_total_seconds,max_total_seconds,avg_client_seconds,avg_server_seconds,avg_server_offline_seconds,avg_server_online_seconds,avg_filter_build_time_ms,avg_pir_setup_time_ms,avg_pir_single_setup_time_ms,avg_query_time_ms,avg_build_setup_query_ms,max_build_setup_query_ms,avg_communication_mb,avg_pir_query_communication_mb,avg_pir_response_communication_mb,avg_pir_setup_communication_mb,avg_cuckoo_failed,cuckoo_fail_rate,filter_size,pir_mode,L,k,w,pir_single_shards,pir_single_db_rows,batchpir_batch_size,selection_note,buckets,total_size_bits,filter_size_mb,log"
printf "%s\n" "${summary_header}" > "${summary_file}"
printf "client_size,server_size,seconds\n" > "${seconds_file}"

max_time=""
max_client=""
max_filter_mb=""
max_log=""
CALIBRATED_PARAM_TABLE=""

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

calibration_candidate_lkw() {
  local L
  local k
  local w
  local triple

  if [[ -n "${CALIBRATION_LKW_CANDIDATES}" ]]; then
    for triple in ${CALIBRATION_LKW_CANDIDATES//,/ }; do
      if [[ -n "${triple}" ]]; then
        printf '%s\n' "${triple}"
      fi
    done
    return
  fi

  for L in ${CALIBRATION_L_VALUES}; do
    for k in ${CALIBRATION_K_VALUES}; do
      for w in ${CALIBRATION_W_VALUES}; do
        printf '%s:%s:%s\n' "${L}" "${k}" "${w}"
      done
    done
  done
}

batchpir_batch_size_for_client() {
  local client_size="$1"

  if [[ "${client_size}" == "100" ]]; then
    printf '%s\n' "${CLIENT_100_PIR_BATCHPIR_BATCH_SIZE}"
  else
    printf '%s\n' "${PIR_BATCHPIR_BATCH_SIZE}"
  fi
}

batchpir_batch_candidates() {
  local start="$1"
  local seen=" "
  local candidate

  printf '%s\n' "${start}"
  seen="${seen}${start} "

  for candidate in ${PIR_BATCHPIR_FALLBACK_SIZES}; do
    if (( candidate < start )) && [[ "${seen}" != *" ${candidate} "* ]]; then
      printf '%s\n' "${candidate}"
      seen="${seen}${candidate} "
    fi
  done
}

calibration_safe_args() {
  local arg

  for arg in "$@"; do
    case "${arg}" in
      --PIR_*|--OPRF|--pir_batchpir_batch_size=*)
        ;;
      *)
        printf '%s\n' "${arg}"
        ;;
    esac
  done
}

select_calibration_row() {
  local table="$1"
  local client_size="$2"
  local target_fp="$3"
  local target_fn="$4"
  local target_fp_provided="$5"

  awk -F';' -v want_client="${client_size}" -v target_fp="${target_fp}" -v target_fn="${target_fn}" -v target_fp_provided="${target_fp_provided}" -v require_zero_failed="${CALIBRATION_REQUIRE_ZERO_CUCKOO_FAILED}" '
    function clean(value) {
      gsub(/\r$/, "", value);
      gsub(/^"|"$/, "", value);
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value);
      return value;
    }
    function numeric_ok(value) {
      return value != "" && value != "nan" && value != "NaN";
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
    function min_fp_at_fn_rank_better() {
      if (!have_selected) return 1;
      if (fp + 0 != best_fp + 0) return fp + 0 < best_fp + 0;
      if (fn + 0 != best_fn + 0) return fn + 0 < best_fn + 0;
      if (L + 0 != best_L + 0) return L + 0 < best_L + 0;
      if (filter_size + 0 != best_filter_size + 0) return filter_size + 0 < best_filter_size + 0;
      if (k + 0 != best_k + 0) return k + 0 < best_k + 0;
      return w + 0 < best_w + 0;
    }
    function fallback_rank_better() {
      if (!have_fallback) return 1;
      if (zero_failed != fallback_zero_failed) return zero_failed > fallback_zero_failed;
      if (numeric_ok(fn) && numeric_ok(fallback_fn) && fn + 0 != fallback_fn + 0) return fn + 0 < fallback_fn + 0;
      if (numeric_ok(fp) && numeric_ok(fallback_fp) && fp + 0 != fallback_fp + 0) return fp + 0 < fallback_fp + 0;
      if (L + 0 != fallback_L + 0) return L + 0 < fallback_L + 0;
      if (filter_size + 0 != fallback_filter_size + 0) return filter_size + 0 < fallback_filter_size + 0;
      if (k + 0 != fallback_k + 0) return k + 0 < fallback_k + 0;
      return w + 0 < fallback_w + 0;
    }
    function remember_best(status_value) {
      best_filter_size = filter_size;
      best_L = L;
      best_k = k;
      best_w = w;
      best_fp = fp;
      best_fn = fn;
      best_status = status_value;
      best_log = row_log;
      have_selected = 1;
    }
    NR == 1 {
      for (i = 1; i <= NF; ++i) col[clean($i)] = i;
      next;
    }
    clean($col["client_size"]) == want_client {
      filter_size = clean($col["filter_size"]);
      L = clean($col["L"]);
      k = clean($col["k"]);
      w = clean($col["w"]);
      fp = clean($col["avg_fp"]);
      fn = clean($col["avg_fn"]);
      cuckoo_failed = clean($col["cuckoo_failed"]);
      row_log = clean($col["log"]);
      meets_fn = numeric_ok(fn) && fn + 0 <= target_fn + 0;
      meets_target = meets_fn && numeric_ok(fp) && fp + 0 <= target_fp + 0;
      zero_failed = require_zero_failed != "1" || (numeric_ok(cuckoo_failed) && cuckoo_failed + 0 == 0);

      if (target_fp_provided == "1") {
        if (zero_failed && meets_target && target_rank_better()) remember_best("selected");
      } else if (zero_failed && meets_fn && numeric_ok(fp) && min_fp_at_fn_rank_better()) {
        remember_best("selected");
      }

      if (fallback_rank_better()) {
        fallback_filter_size = filter_size;
        fallback_L = L;
        fallback_k = k;
        fallback_w = w;
        fallback_fp = fp;
        fallback_fn = fn;
        fallback_zero_failed = zero_failed;
        fallback_log = row_log;
        have_fallback = 1;
      }
    }
    END {
      if (have_selected) {
        print best_filter_size " " best_L " " best_k " " best_w " " best_fp " " best_fn " " best_status " " best_log;
      } else if (have_fallback) {
        if (require_zero_failed == "1" && !fallback_zero_failed) {
          print fallback_filter_size " " fallback_L " " fallback_k " " fallback_w " " fallback_fp " " fallback_fn " no_zero_cuckoo_failure_candidate " fallback_log;
        } else {
          print fallback_filter_size " " fallback_L " " fallback_k " " fallback_w " " fallback_fp " " fallback_fn " no_candidate_met_fn_target " fallback_log;
        }
      } else {
        exit 4;
      }
    }
  ' "${table}"
}

params_from_table() {
  local table="$1"
  local client_size="$2"

  awk -F';' -v want_client="${client_size}" '
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
    clean($col["client_size"]) == want_client {
      print clean($col["L"]) " " clean($col["k"]) " " clean($col["w"]) " " clean($col["filter_size"]) " direct-cuckoo-calibration";
      found = 1;
      exit 0;
    }
    END {
      if (!found) exit 4;
    }
  ' "${table}"
}

run_calibration_pair() {
  local client_size="$1"
  local filter_size="$2"
  local candidate_lkw_csv="$3"
  local calibration_dir="$4"
  local all_csv="$5"
  shift 5
  local log_file
  local run_cmd
  local test_status
  local candidate_label

  candidate_label="${candidate_lkw_csv//:/_}"
  candidate_label="${candidate_label//,/_}"
  candidate_label="${candidate_label//./_}"
  log_file="${calibration_dir}/calibration_client_${client_size}_server_${CALIBRATION_SERVER_SIZE}_filter_${filter_size}_lkw_${candidate_label}.log"

  run_cmd=("${CALIBRATION_BASE_CMD[@]}")
  run_cmd+=(--db="${DB_PRESET}")
  run_cmd+=("${CALIBRATION_PATH_ARGS[@]}")
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
    --server_size="${CALIBRATION_SERVER_SIZE}"
    --filter_size="${filter_size}"
    "$@"
  )

  printf '[script] calibration CLIENT_SIZE=%s CALIBRATION_SERVER_SIZE=%s NOMINAL_SERVER_SIZE=%s filter_size=%s log=%s\n' \
    "${client_size}" \
    "${CALIBRATION_SERVER_SIZE}" \
    "${SERVER_SIZE}" \
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
      print kv["client_size"] ";" kv["server_size"] ";" kv["filter_size"] ";" kv["L"] ";" kv["k"] ";" kv["w"] ";" kv["fp"] ";" kv["fn"] ";" kv["FP_count"] ";" kv["TN_count"] ";" kv["TP_count"] ";" kv["FN_count"] ";" kv["cuckoo_failed"] ";" kv["cuckoo_fail_rate"] ";" kv["status"] ";" log_path;
    }
  ' "${log_file}" >> "${all_csv}"
}

run_client_calibration() {
  local calibration_dir="${log_dir}/calibration"
  local all_csv="${calibration_dir}/calibration_all.csv"
  local selected_csv="${calibration_dir}/calibrated_params.csv"
  local client_size
  local base_filter_size
  local candidate_filter_size
  local filter_size_options
  local triple
  local candidate_L
  local selected_fields
  local selected_filter_size
  local protocol_filter_size
  local best_L
  local best_k
  local best_w
  local best_fp
  local best_fn
  local status
  local best_log

  mkdir -p "${calibration_dir}"
  printf "client_size;server_size;filter_size;L;k;w;avg_fp;avg_fn;FP_count;TN_count;TP_count;FN_count;cuckoo_failed;cuckoo_fail_rate;status;log\n" > "${all_csv}"
  printf "client_size;server_size;filter_size;L;k;w;avg_fp;avg_fn;status;log\n" > "${selected_csv}"

  for client_size in "${CLIENT_SIZES[@]}"; do
    echo
    echo "================ CALIBRATION CLIENT_SIZE=${client_size} CALIBRATION_SERVER_SIZE=${CALIBRATION_SERVER_SIZE} (direct Cuckoo, no PIR/OPRF) ================"
    filter_size_options="$(calibration_filter_sizes_for_server "${CALIBRATION_SERVER_SIZE}" | paste -sd ' ' -)"
    echo "[script] calibrating CLIENT_SIZE=${client_size} CALIBRATION_SERVER_SIZE=${CALIBRATION_SERVER_SIZE} NOMINAL_SERVER_SIZE=${SERVER_SIZE} BASE_FILTER_SIZES=${filter_size_options}"
    echo "[script] calibration filter size policy: actual_filter_size = base_filter_size * L"

    while IFS= read -r triple; do
      candidate_L="${triple%%:*}"
      while IFS= read -r base_filter_size; do
        candidate_filter_size="$(
          awk -v base="${base_filter_size}" -v L="${candidate_L}" 'BEGIN {
            size = int(base * L);
            if (size < 4) size = 4;
            rem = size % 4;
            if (rem != 0) size += 4 - rem;
            print size;
          }'
        )"
        echo "[script] candidate ${triple}: base_filter_size=${base_filter_size} L=${candidate_L} actual_filter_size=${candidate_filter_size}"
        run_calibration_pair "${client_size}" "${candidate_filter_size}" "${triple}" "${calibration_dir}" "${all_csv}" "$@"
      done < <(calibration_filter_sizes_for_server "${CALIBRATION_SERVER_SIZE}")
    done < <(calibration_candidate_lkw)

    if ! selected_fields="$(select_calibration_row "${all_csv}" "${client_size}" "${CALIBRATION_TARGET_FP}" "${CALIBRATION_TARGET_FN}" "${calibration_target_fp_provided}")"; then
      echo "[script] could not select calibration row for CLIENT_SIZE=${client_size}" >&2
      exit 1
    fi
    read -r selected_filter_size best_L best_k best_w best_fp best_fn status best_log <<< "${selected_fields}"
    protocol_filter_size="$(
      awk -v calibration_filter="${selected_filter_size}" \
          -v calibration_server="${CALIBRATION_SERVER_SIZE}" \
          -v protocol_server="${PROTOCOL_SERVER_SIZE}" 'BEGIN {
        if (calibration_server + 0 <= 0) {
          size = calibration_filter + 0;
        } else {
          size = int(((calibration_filter + 0) * (protocol_server + 0) / (calibration_server + 0)) + 0.999999);
        }
        if (size < 4) size = 4;
        rem = size % 4;
        if (rem != 0) size += 4 - rem;
        print size;
      }'
    )"
    if [[ "${status}" == "no_zero_cuckoo_failure_candidate" ]]; then
      echo "[script] ERROR no calibration row had cuckoo_failed=0 for CLIENT_SIZE=${client_size}." >&2
      echo "[script] Increase CALIBRATION_FILTER_SIZE_PERCENT_INCREASES, for example:" >&2
      echo "[script]   CALIBRATION_FILTER_SIZE_PERCENT_INCREASES=\"33 66 100 150 200 300 500 800\"" >&2
      echo "[script] all calibration attempts: ${all_csv}" >&2
      exit 1
    fi
    if [[ "${status}" != "selected" ]]; then
      echo "[script] WARNING no calibration row met FN<=${CALIBRATION_TARGET_FN} for CLIENT_SIZE=${client_size}; using fallback L=${best_L} filter_size=${selected_filter_size} FP=${best_fp} FN=${best_fn}" >&2
    fi
    echo "[script] filter scaling CLIENT_SIZE=${client_size}: calibration_filter_size=${selected_filter_size} calibration_server_size=${CALIBRATION_SERVER_SIZE} protocol_server_size=${PROTOCOL_SERVER_SIZE} protocol_filter_size=${protocol_filter_size}"

    printf "%s;%s;%s;%s;%s;%s;%s;%s;%s;%s\n" \
      "${client_size}" \
      "${PROTOCOL_SERVER_SIZE}" \
      "${protocol_filter_size}" \
      "${best_L}" \
      "${best_k}" \
      "${best_w}" \
      "${best_fp}" \
      "${best_fn}" \
      "${status}" \
      "${best_log}" >> "${selected_csv}"
    echo "[script] selected CLIENT_SIZE=${client_size}: L=${best_L} k=${best_k} w=${best_w} calibration_filter_size=${selected_filter_size} protocol_filter_size=${protocol_filter_size} FP=${best_fp} FN=${best_fn} status=${status}"
  done

  CALIBRATED_PARAM_TABLE="${selected_csv}"
  mkdir -p "$(dirname "${CALIBRATION_CACHE_TABLE}")"
  cp "${selected_csv}" "${CALIBRATION_CACHE_TABLE}"
  echo "[script] calibration selected params: ${CALIBRATED_PARAM_TABLE}"
  echo "[script] calibration cached params: ${CALIBRATION_CACHE_TABLE}"
  echo "[script] calibration all attempts: ${all_csv}"
}

run_one_attempt() {
  local client_size="$1"
  local filter_size="$2"
  local batchpir_batch_size="$3"
  local attempt="$4"
  local attempt_L="$5"
  local attempt_k="$6"
  local attempt_w="$7"
  local param_source="$8"
  shift 8
  local log_file
  local run_cmd
  local statuses
  local test_status
  local tee_status
  local parsed_rows
  local fp_plain
  local fn_plain
  local total_plain
  local server_plain
  local client_plain
  local filter_mb_plain
  local parsed_L_plain
  local parsed_k_plain
  local parsed_w_plain
  local client_runtime_plain
  local server_runtime_plain
  local comm_plain
  local pir_query_comm_plain
  local pir_response_comm_plain
  local pir_setup_comm_plain
  local filter_build_runtime_plain
  local paper_metrics
  local pir_single_shards
  local pir_single_db_rows
  local buckets
  local total_size_bits
  local avg_filter_build_time_ms
  local avg_build_setup_query_ms

  if [[ "${attempt}" == "1" ]]; then
    log_file="${log_dir}/client_${client_size}_server_${PROTOCOL_SERVER_SIZE}_filter_${filter_size}_batch_${batchpir_batch_size}.log"
  else
    log_file="${log_dir}/client_${client_size}_server_${PROTOCOL_SERVER_SIZE}_filter_${filter_size}_batch_${batchpir_batch_size}_attempt_${attempt}_L_${attempt_L}_k_${attempt_k}.log"
  fi

  run_cmd=("${BASE_CMD[@]}")
  run_cmd+=(--db="${DB_PRESET}")
  run_cmd+=("${PROTOCOL_PATH_ARGS[@]}")
  run_cmd+=(
    --L="${attempt_L}"
    --k="${attempt_k}"
    --w="${attempt_w}"
    --pir_batchpir_batch_size="${batchpir_batch_size}"
    --client_size="${client_size}"
    --server_size="${PROTOCOL_SERVER_SIZE}"
    --filter_size="${filter_size}"
    "$@"
  )

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
    if grep -q "parms_id is not valid for encryption parameters" "${log_file}"; then
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
        mode = filter = server = client = filter_mb = fp = fn = fp_count = tn_count = tp_count = fn_count = cuckoo_failed = cuckoo_fail_rate = total = client_runtime = server_runtime = server_offline_runtime = server_online_runtime = filter_build_runtime = pir_setup_runtime = comm = pir_query_comm = pir_response_comm = pir_setup_comm = "";
      }
      function flush_row() {
        if (in_row && total != "") {
          print csv(mode) "," csv(filter) "," csv(server) "," csv(client) "," csv(best_L) "," csv(best_k) "," csv(best_w) "," csv(filter_mb) "," csv(fp) "," csv(fn) "," csv(fp_count) "," csv(tn_count) "," csv(tp_count) "," csv(fn_count) "," csv(cuckoo_failed) "," csv(cuckoo_fail_rate) "," csv(total) "," csv(client_runtime) "," csv(server_runtime) "," csv(server_offline_runtime) "," csv(server_online_runtime) "," csv(filter_build_runtime) "," csv(pir_setup_runtime) "," csv(comm) "," csv(pir_query_comm) "," csv(pir_response_comm) "," csv(pir_setup_comm);
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
      in_row && /^[[:space:]]*Avg FP Count:/ {
        text = $0;
        sub(/^[[:space:]]*Avg FP Count:[[:space:]]*/, "", text);
        fp_count = text;
        next;
      }
      in_row && /^[[:space:]]*Avg TN Count:/ {
        text = $0;
        sub(/^[[:space:]]*Avg TN Count:[[:space:]]*/, "", text);
        tn_count = text;
        next;
      }
      in_row && /^[[:space:]]*Avg TP Count:/ {
        text = $0;
        sub(/^[[:space:]]*Avg TP Count:[[:space:]]*/, "", text);
        tp_count = text;
        next;
      }
      in_row && /^[[:space:]]*Avg FN Count:/ {
        text = $0;
        sub(/^[[:space:]]*Avg FN Count:[[:space:]]*/, "", text);
        fn_count = text;
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
    echo "No [RUNTIME AVG] block found for Client size ${client_size}, Server size ${PROTOCOL_SERVER_SIZE}" >&2
    exit 1
  fi

  while IFS=, read -r mode filter server client parsed_L parsed_k parsed_w filter_mb fp fn fp_count tn_count tp_count fn_count cuckoo_failed cuckoo_fail_rate total client_runtime server_runtime server_offline_runtime server_online_runtime filter_build_runtime pir_setup_runtime comm pir_query_comm pir_response_comm pir_setup_comm; do
    fp_plain="${fp//\"/}"
    fn_plain="${fn//\"/}"
    fp_count_plain="${fp_count//\"/}"
    tn_count_plain="${tn_count//\"/}"
    tp_count_plain="${tp_count//\"/}"
    fn_count_plain="${fn_count//\"/}"
    cuckoo_failed_plain="${cuckoo_failed//\"/}"
    cuckoo_fail_rate_plain="${cuckoo_fail_rate//\"/}"
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
      "BatchPIR" \
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
      "\"${log_file}\"" >> "${summary_file}"

    printf "%s,%s,%s\n" "${client_plain}" "${server_plain}" "${total_plain}" >> "${seconds_file}"
    echo "[script] result CLIENT_SIZE=${client_plain} SERVER_SIZE=${server_plain} FP=${fp_plain} FN=${fn_plain} missed_close=${fn_count_plain} cuckoo_failed=${cuckoo_failed_plain} cuckoo_fail_rate=${cuckoo_fail_rate_plain}"

    if [[ -z "${max_time}" ]] || awk -v candidate="${total_plain}" -v current="${max_time}" 'BEGIN { exit !(candidate > current) }'; then
      max_time="${total_plain}"
      max_client="${client_plain}"
      max_filter_mb="${filter_mb_plain}"
      max_log="${log_file}"
    fi
  done <<< "${parsed_rows}"
}

if [[ "${CALIBRATE}" == "1" ]]; then
  if [[ -f "${CALIBRATION_CACHE_TABLE}" ]]; then
    CALIBRATED_PARAM_TABLE="${CALIBRATION_CACHE_TABLE}"
    echo "[script] calibration cache hit: ${CALIBRATED_PARAM_TABLE}; skipping calibration"
  else
    echo "[script] calibration cache miss: ${CALIBRATION_CACHE_TABLE}; running calibration"
    calibration_args=()
    while IFS= read -r calibration_arg; do
      calibration_args+=("${calibration_arg}")
    done < <(calibration_safe_args "$@")
    if (( ${#calibration_args[@]} > 0 )); then
      run_client_calibration "${calibration_args[@]}"
    else
      run_client_calibration
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
else
  CALIBRATED_PARAM_TABLE="${CALIBRATION_CACHE_TABLE}"
  if [[ ! -f "${CALIBRATED_PARAM_TABLE}" ]]; then
    echo "[script] Missing calibrated parameter table: ${CALIBRATED_PARAM_TABLE}" >&2
    echo "[script] Run with calibration enabled, or set CALIBRATION_CACHE_TABLE=/path/to/calibrated_params.csv." >&2
    exit 2
  fi
fi

for client_size in "${CLIENT_SIZES[@]}"; do
  selected_batchpir_batch_size="$(batchpir_batch_size_for_client "${client_size}")"

  if ! params_line="$(params_from_table "${CALIBRATED_PARAM_TABLE}" "${client_size}")"; then
    echo "[script] No L/k/w/filter_size found for client_size=${client_size} in ${CALIBRATED_PARAM_TABLE}" >&2
    exit 2
  fi
  read -r selected_L selected_k selected_w filter_size selected_param_source <<< "${params_line}"

  echo
  echo "================ CLIENT_SIZE=${client_size} PROTOCOL_SERVER_SIZE=${PROTOCOL_SERVER_SIZE} DB=${DB_PRESET} FILTER_SIZE=${filter_size} BATCH_SIZE=${selected_batchpir_batch_size} L=${selected_L} k=${selected_k} w=${selected_w} (${selected_param_source}) ================"
  batch_attempt=1
  run_succeeded=0
  for candidate_batch_size in $(batchpir_batch_candidates "${selected_batchpir_batch_size}"); do
    if (( batch_attempt > 1 )); then
      echo "[script] retrying CLIENT_SIZE=${client_size} PROTOCOL_SERVER_SIZE=${PROTOCOL_SERVER_SIZE} with smaller BatchPIR batch_size=${candidate_batch_size}"
    fi

    if run_one_attempt "${client_size}" "${filter_size}" "${candidate_batch_size}" "${batch_attempt}" "${selected_L}" "${selected_k}" "${selected_w}" "${selected_param_source}" "$@"; then
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
    echo "[script] all BatchPIR fallback batch sizes failed for CLIENT_SIZE=${client_size} PROTOCOL_SERVER_SIZE=${PROTOCOL_SERVER_SIZE}" >&2
    exit 75
  fi
done

echo
echo "================ BUILD+SETUP+QUERY SECONDS ================"
cat "${seconds_file}"
echo
echo "================ MAX BUILD+SETUP+QUERY TIME ================"
echo "client_size=${max_client} server_size=${PROTOCOL_SERVER_SIZE} filter_size_mb=${max_filter_mb} max_total_runtime_s=${max_time}"
echo "summary=${summary_file}"
echo "seconds=${seconds_file}"
echo "log=${max_log}"
