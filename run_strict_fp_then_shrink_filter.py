#!/usr/bin/env python3
"""Calibrate with a stricter FP target, then shrink filter size to requested FP.

This is useful when a strong L/k/w choice produces FP well below the requested
target. After finding that L/k/w, the script reduces the Cuckoo filter size and
keeps the smallest filter that still meets the requested FP/FN targets.
"""

import argparse
import csv
import hashlib
import math
import sys
import time
from pathlib import Path

import run_optimized_cuckoo_pir_single_oprf_sweep as base


SELECTED_FIELDS = [
    "target_fp",
    "strict_fp",
    "target_fn",
    "server_size",
    "filter_size",
    "L",
    "k",
    "w",
    "pir_single_shards",
    "pir_single_db_rows",
    "status",
    "score",
    "strict_filter_size",
    "strict_avg_fp",
    "strict_avg_fn",
    "avg_fp",
    "avg_fn",
    "avg_cuckoo_failed",
    "cuckoo_fail_rate",
    "avg_filter_build_time_ms",
    "avg_query_time_ms",
    "log",
]


ALL_FIELDS = [
    "phase",
    *SELECTED_FIELDS,
]


def label_float(value):
    return "{:.9g}".format(value).replace(".", "_")


def numeric(row, key, default=0.0):
    value = row.get(key, "")
    return default if value == "" else float(value)


def write_rows(path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as outfile:
        writer = csv.DictWriter(outfile, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fieldnames})


def append_row(path, row, fieldnames):
    exists = path.exists()
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", newline="") as outfile:
        writer = csv.DictWriter(outfile, fieldnames=fieldnames)
        if not exists:
            writer.writeheader()
        writer.writerow({field: row.get(field, "") for field in fieldnames})


def read_rows(path):
    with path.open(newline="") as infile:
        return list(csv.DictReader(infile))


def cache_path(args):
    if args.calibration_cache is not None:
        return args.calibration_cache
    config = "|".join(
        [
            "strategy=strict_fp_then_shrink_v1",
            "client={}".format(args.client_size),
            "target_fp={:.9g}".format(args.target_fp),
            "strict_fp={:.9g}".format(args.strict_fp),
            "target_fn={:.9g}".format(args.target_fn),
            "calibration_runs={}".format(args.calibration_runs),
            "score_fn_weight={:.9g}".format(args.score_fn_weight),
            "strict_filter_factor={:.9g}".format(args.strict_filter_factor),
            "shrink_factors={}".format(",".join("{:.9g}".format(v) for v in args.shrink_factors)),
            "params={}".format(
                ",".join("{}:{}:{:.9g}".format(L, k, w) for L, k, w in args.calibration_param_candidates)
            ),
        ]
    )
    digest = hashlib.sha256(config.encode("utf-8")).hexdigest()[:12]
    return Path("results") / "calibration_cache" / (
        "strict_shrink_requested_fp_{}_strict_fp_{}_fn_{}_{}.csv".format(
            label_float(args.target_fp),
            label_float(args.strict_fp),
            label_float(args.target_fn),
            digest,
        )
    )


def round_filter_size(value):
    return base.round_filter_size(value)


def strict_filter_size_for(server_size, L, args):
    return round_filter_size(float(server_size) * float(L) * args.strict_filter_factor)


def shrink_filter_sizes_for(server_size, L, strict_filter_size, args):
    sizes = {strict_filter_size}
    for factor in args.shrink_factors:
        sizes.add(round_filter_size(float(server_size) * float(L) * factor))
    for size in args.candidate_filter_sizes:
        sizes.add(round_filter_size(size))
    return sorted(size for size in sizes if size > 0)


def make_row(args, phase, server_size, filter_size, L, k, w, pairs, status, strict_row=None, log_path=None):
    fp = numeric(pairs, "avg_fp")
    fn = numeric(pairs, "avg_fn")
    score = args.score_fn_weight * fn + (1.0 - args.score_fn_weight) * fp
    strict_fp = pairs.get("avg_fp", "")
    strict_fn = pairs.get("avg_fn", "")
    strict_filter_size = str(filter_size)
    if strict_row is not None:
        strict_fp = strict_row.get("avg_fp", "")
        strict_fn = strict_row.get("avg_fn", "")
        strict_filter_size = strict_row.get("filter_size", str(filter_size))

    return {
        "phase": phase,
        "target_fp": "{:.9g}".format(args.target_fp),
        "strict_fp": "{:.9g}".format(args.strict_fp),
        "target_fn": "{:.9g}".format(args.target_fn),
        "server_size": str(server_size),
        "filter_size": str(filter_size),
        "L": str(L),
        "k": str(k),
        "w": "{:.9g}".format(w),
        "pir_single_shards": str(base.pir_single_shards_for_filter_size(filter_size)),
        "pir_single_db_rows": str(base.pir_single_db_rows_for_filter_size(filter_size)),
        "status": status,
        "score": "{:.9g}".format(score),
        "strict_filter_size": strict_filter_size,
        "strict_avg_fp": strict_fp,
        "strict_avg_fn": strict_fn,
        "avg_fp": pairs.get("avg_fp", ""),
        "avg_fn": pairs.get("avg_fn", ""),
        "avg_cuckoo_failed": pairs.get("avg_cuckoo_failed", ""),
        "cuckoo_fail_rate": pairs.get("cuckoo_fail_rate", ""),
        "avg_filter_build_time_ms": pairs.get("avg_filter_build_time_ms", ""),
        "avg_query_time_ms": pairs.get("avg_query_time_ms", ""),
        "log": "" if log_path is None else str(log_path),
    }


def row_rank(row):
    return (
        numeric(row, "score", float("inf")),
        numeric(row, "avg_fn", float("inf")),
        numeric(row, "avg_fp", float("inf")),
        int(row["filter_size"]),
    )


def better_scored(candidate, current):
    if current is None:
        return candidate
    return candidate if row_rank(candidate) < row_rank(current) else current


def run_candidate(args, run_dir, phase, server_size, filter_size, L, k, w, strict_row=None):
    log_path = run_dir / "calibration_server_{}_{}_L_{}_k_{}_w_{}_filter_{}.log".format(
        server_size,
        phase,
        L,
        k,
        label_float(w),
        filter_size,
    )
    output = base.run_command(
        base.calibration_command(args, server_size, filter_size, L, k, w),
        args.timeout,
        log_path,
    )
    pairs = base.parse_avg_output(output)
    return make_row(args, phase, server_size, filter_size, L, k, w, pairs, "fail", strict_row, log_path)


def print_row(row):
    print(
        "CALIBRATION {phase},{server},{size},{L},{k},{w},{score},{fp},{fn},{status}".format(
            phase=row["phase"],
            server=row["server_size"],
            size=row["filter_size"],
            L=row["L"],
            k=row["k"],
            w=row["w"],
            score=row["score"],
            fp=row["avg_fp"],
            fn=row["avg_fn"],
            status=row["status"],
        ),
        flush=True,
    )


def load_partial_cache(path, server_sizes, args):
    if not path.exists() or args.force_calibration:
        return [], server_sizes
    rows = read_rows(path)
    if rows:
        missing_fields = [field for field in SELECTED_FIELDS if field not in rows[0]]
        if missing_fields:
            print("calibration_cache=ignored reason=missing_fields:{}".format(",".join(missing_fields)), flush=True)
            return [], server_sizes
    target_fp = "{:.9g}".format(args.target_fp)
    strict_fp = "{:.9g}".format(args.strict_fp)
    target_fn = "{:.9g}".format(args.target_fn)
    for row in rows:
        if (
            row.get("target_fp") != target_fp or
            row.get("strict_fp") != strict_fp or
            row.get("target_fn") != target_fn
        ):
            print("calibration_cache=ignored reason=target_mismatch", flush=True)
            return [], server_sizes
    by_server = {int(row["server_size"]): row for row in rows if row.get("server_size")}
    selected = [by_server[server_size] for server_size in server_sizes if server_size in by_server]
    missing = [server_size for server_size in server_sizes if server_size not in by_server]
    if missing:
        print(
            "calibration_cache=partial path={} rows={} missing={}".format(
                path,
                len(selected),
                ",".join(str(value) for value in missing),
            ),
            flush=True,
        )
    else:
        print("calibration_cache=loaded path={}".format(path), flush=True)
    return selected, missing


def calibrate(args, run_dir, server_sizes, cache):
    all_csv = run_dir / "calibration_all.csv"
    selected_csv = run_dir / "selected.csv"
    selected_rows, missing_server_sizes = load_partial_cache(cache, server_sizes, args)
    if selected_rows:
        write_rows(selected_csv, selected_rows, SELECTED_FIELDS)
    selected_by_server = {int(row["server_size"]): row for row in selected_rows}

    print(
        "CALIBRATION phase,server_size,filter_size,L,k,w,score,avg_fp,avg_fn,status",
        flush=True,
    )

    for server_size in server_sizes:
        if server_size in selected_by_server:
            print("CALIBRATION_RESUME server_size={} source=cache".format(server_size), flush=True)
            continue

        strict_choice = None
        strict_fallback = None
        for L, k, w in args.calibration_param_candidates:
            filter_size = strict_filter_size_for(server_size, L, args)
            row = run_candidate(args, run_dir, "strict_lkw", server_size, filter_size, L, k, w)
            fp = numeric(row, "avg_fp")
            fn = numeric(row, "avg_fn")
            if fp <= args.strict_fp and fn <= args.target_fn:
                row["status"] = "strict_lkw_selected"
                strict_choice = row
                print_row(row)
                break
            row["status"] = "strict_lkw_fail"
            strict_fallback = better_scored(row, strict_fallback)
            append_row(all_csv, row, ALL_FIELDS)
            print_row(row)

        if strict_choice is None:
            strict_choice = dict(strict_fallback)
            strict_choice["status"] = "strict_lkw_best_fallback"
            print(
                "WARNING Server={} did not meet strict FP/FN; shrinking best L/k/w anyway: L={} k={} w={} fp={} fn={}".format(
                    server_size,
                    strict_choice["L"],
                    strict_choice["k"],
                    strict_choice["w"],
                    strict_choice["avg_fp"],
                    strict_choice["avg_fn"],
                ),
                flush=True,
            )
        append_row(all_csv, strict_choice, ALL_FIELDS)

        L = int(strict_choice["L"])
        k = int(strict_choice["k"])
        w = float(strict_choice["w"])
        strict_filter_size = int(strict_choice["filter_size"])
        shrink_rows = []
        for filter_size in shrink_filter_sizes_for(server_size, L, strict_filter_size, args):
            row = run_candidate(args, run_dir, "shrink_filter", server_size, filter_size, L, k, w, strict_choice)
            fp = numeric(row, "avg_fp")
            fn = numeric(row, "avg_fn")
            ok = fp <= args.target_fp and fn <= args.target_fn
            row["status"] = "shrink_ok" if ok else "shrink_fail"
            shrink_rows.append(row)
            append_row(all_csv, row, ALL_FIELDS)
            print_row(row)

        ok_rows = [row for row in shrink_rows if row["status"] == "shrink_ok"]
        if ok_rows:
            # Primary objective: smallest filter that still satisfies requested FP/FN.
            chosen = min(
                ok_rows,
                key=lambda row: (
                    int(row["filter_size"]),
                    abs(args.target_fp - numeric(row, "avg_fp", args.target_fp)),
                    numeric(row, "avg_fn", float("inf")),
                ),
            )
            chosen = dict(chosen)
            chosen["status"] = "selected_smallest_requested_fp"
        else:
            chosen = dict(strict_choice)
            chosen["status"] = "no_shrink_candidate_met_requested_fp"
            print(
                "WARNING Server={} no shrunken filter met requested FP/FN; using strict L/k/w filter_size={}".format(
                    server_size,
                    chosen["filter_size"],
                ),
                flush=True,
            )

        selected_rows.append({field: chosen.get(field, "") for field in SELECTED_FIELDS})
        selected_by_server[server_size] = chosen
        write_rows(selected_csv, selected_rows, SELECTED_FIELDS)
        write_rows(cache, selected_rows, SELECTED_FIELDS)
        print("calibration_cache=checkpoint path={} rows={}".format(cache, len(selected_rows)), flush=True)

    return selected_rows


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("./test"))
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--server-sizes", default=",".join(str(value) for value in base.DEFAULT_SERVER_SIZES))
    parser.add_argument("--client-size", type=int, default=1000)
    parser.add_argument("--target-fp", type=float, default=0.03)
    parser.add_argument("--strict-fp", type=float, default=0.005)
    parser.add_argument("--target-fn", type=float, default=0.01)
    parser.add_argument("--num-runs", type=int, default=5)
    parser.add_argument("--calibration-runs", type=int, default=1)
    parser.add_argument("--pir-mode", default="BatchPIR")
    parser.add_argument("--batchpir-batch-size", type=int, default=128)
    parser.add_argument("--score-fn-weight", type=float, default=0.7)
    parser.add_argument("--strict-filter-factor", type=float, default=1.05)
    parser.add_argument(
        "--shrink-factors",
        default="1.05,1,0.95,0.9,0.85,0.8,0.75,0.7,0.65,0.6,0.55,0.5,0.45,0.4,0.35,0.3,0.25,0.2,0.15,0.1",
    )
    parser.add_argument("--calibration-start-params", default="1:3:0.05")
    parser.add_argument("--calibration-L-values", default="1,3,5,7")
    parser.add_argument("--calibration-k-values", default="1,3,5,10,20")
    parser.add_argument("--calibration-w-values", default="0.0001,0.001,0.05,0.01,0.05,0.1,0.5,1,2,5")
    parser.add_argument("--candidate-filter-sizes", default="")
    parser.add_argument("--calibration-cache", type=Path, default=None)
    parser.add_argument("--force-calibration", action="store_true")
    parser.add_argument("--calibration-only", action="store_true")
    parser.add_argument("--timeout", type=int, default=7200)
    parser.add_argument("--out-dir", type=Path, default=None)
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()

    args.shrink_factors = base.parse_float_list(args.shrink_factors)
    args.calibration_start_params = base.parse_lkw_list(args.calibration_start_params)
    args.calibration_L_values = base.parse_int_list(args.calibration_L_values)
    args.calibration_k_values = base.parse_int_list(args.calibration_k_values)
    args.calibration_w_values = base.parse_float_list(args.calibration_w_values)
    args.candidate_filter_sizes = base.parse_int_list(args.candidate_filter_sizes)
    args.candidate_shards = []
    args.shard_ratios = []
    args.capacity_factors = [args.strict_filter_factor]
    args.L = 1
    args.k = 1
    args.w = 1.0
    args.pir_mode = base.normalized_pir_mode(args.pir_mode)
    args.calibration_param_candidates = base.unique_lkw_candidates(
        args.calibration_start_params,
        args.calibration_L_values,
        args.calibration_k_values,
        args.calibration_w_values,
    )
    if args.strict_fp > args.target_fp:
        raise ValueError("--strict-fp should be <= --target-fp")

    server_sizes = base.parse_int_list(args.server_sizes)
    if not args.skip_build:
        base.run_command(["make", "test"], args.timeout)

    args.binary = args.binary.resolve()
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    run_dir = args.out_dir or Path("results") / (
        "strict_fp_then_shrink_requested_fp_{}_strict_fp_{}_fn_{}_{}".format(
            label_float(args.target_fp),
            label_float(args.strict_fp),
            label_float(args.target_fn),
            timestamp,
        )
    )
    run_dir.mkdir(parents=True, exist_ok=True)
    cache = cache_path(args)

    print("output_dir={}".format(run_dir), flush=True)
    print("calibration_cache={}".format(cache), flush=True)
    print(
        "config server_sizes={} client_size={} target_fp={} strict_fp={} target_fn={} pir_mode={} num_runs={}".format(
            server_sizes,
            args.client_size,
            args.target_fp,
            args.strict_fp,
            args.target_fn,
            args.pir_mode,
            args.num_runs,
        ),
        flush=True,
    )

    selected_rows = calibrate(args, run_dir, server_sizes, cache)
    if args.calibration_only:
        print("calibration_only=true selected_csv={}".format(run_dir / "selected.csv"), flush=True)
        return 0

    base.run_timing(args, run_dir, selected_rows)
    print("summary={}".format(run_dir / "summary.csv"), flush=True)
    print("selected={}".format(run_dir / "selected.csv"), flush=True)
    print("calibration_all={}".format(run_dir / "calibration_all.csv"), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
