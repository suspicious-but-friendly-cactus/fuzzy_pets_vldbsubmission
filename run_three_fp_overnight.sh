#!/usr/bin/env bash
set -euo pipefail

target_fn="0.01"
target_fps=("0.05" "0.03" "0.01")
timestamp="$(date +%Y%m%d_%H%M%S)"
root_dir="results/overnight_fp_sweep_${timestamp}"

mkdir -p "${root_dir}"

echo "overnight_root=${root_dir}"
echo "target_fn=${target_fn}"
echo "target_fps=${target_fps[*]}"
echo "extra_args=$*"

for target_fp in "${target_fps[@]}"; do
  label="fp_${target_fp//./_}_fn_${target_fn//./_}"
  out_dir="${root_dir}/${label}"
  log_path="${root_dir}/${label}.log"

  echo
  echo "===== START target_fp=${target_fp} target_fn=${target_fn} out_dir=${out_dir} ====="
  echo "log=${log_path}"

  ./run_optimized_cuckoo_pir_single_oprf_sweep.py \
    --target-fp="${target_fp}" \
    --target-fn="${target_fn}" \
    --out-dir="${out_dir}" \
    "$@" 2>&1 | tee "${log_path}"

  echo "===== DONE target_fp=${target_fp} target_fn=${target_fn} ====="
done

echo
echo "all_done=true"
echo "overnight_root=${root_dir}"
