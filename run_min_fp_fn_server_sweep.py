#!/usr/bin/env python3
"""Find the lowest achievable FP/FN setup as Server size grows.

For each Server size this script exhaustively evaluates a grid over L, k, w, and
filter_size with cheap direct Cuckoo membership runs. It selects the best row by
the requested FP/FN objective, then runs the selected setup with PIR+OPRF timing
and writes a compact CSV with the FP/FN error, chosen parameters, total
communication, and total runtime.
"""

import argparse
import csv
import sys
import time
from pathlib import Path

import run_optimized_cuckoo_pir_single_oprf_sweep as base


DEFAULT_CANDIDATE_FILTER_SIZES = (
    "3152,10500,15752,52500,105000,262144,262144,524288,1048576,"
    "1572864,2097152,2621440,4718592"
)

CALIBRATION_FIELDS = [
    "server_size",
    "client_size",
    "filter_size",
    "L",
    "k",
    "w",
    "pir_single_shards",
    "pir_single_db_rows",
    "objective",
    "fp_fn_error",
    "avg_fp",
    "avg_fn",
    "avg_cuckoo_failed",
    "cuckoo_fail_rate",
    "avg_filter_build_time_ms",
    "avg_query_time_ms",
    "status",
    "log",
]

REQUESTED_SUMMARY_FIELDS = [
    "server_size",
    "client_size",
    "fp_fn_error",
    "avg_fp",
    "avg_fn",
    "filter_size",
    "filter_size_mb",
    "L",
    "k",
    "w",
    "pir_mode",
    "batchpir_batch_size",
    "total_communication_mb",
    "total_runtime_s",
    "client_runtime_s",
    "server_runtime_s",
    "filter_build_runtime_s",
    "pir_setup_runtime_s",
    "selection_note",
    "log",
]


def label_float(value):
    return "{:.9g}".format(value).replace(".", "_")


def numeric(row, key, default=0.0):
    value = row.get(key, "")
    return default if value == "" else float(value)


def fp_fn_error(fp, fn, objective, fn_weight):
    if objective == "max":
        return max(fp, fn)
    if objective == "sum":
        return fp + fn
    if objective == "weighted":
        return fn_weight * fn + (1.0 - fn_weight) * fp
    raise ValueError("unknown objective {!r}".format(objective))


def candidate_filter_sizes_for(server_size, server_index, L, args):
    sizes = set()
    if args.candidate_filter_sizes:
        if server_index >= len(args.candidate_filter_sizes):
            raise ValueError(
                "--candidate-filter-sizes has {} values, but Server index {} was requested".format(
                    len(args.candidate_filter_sizes),
                    server_index,
                )
            )
        sizes.add(base.round_filter_size(args.candidate_filter_sizes[server_index]))
    return sorted(size for size in sizes if size > 0)


def append_row(path, row, fieldnames):
    exists = path.exists()
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", newline="") as outfile:
        writer = csv.DictWriter(outfile, fieldnames=fieldnames)
        if not exists:
            writer.writeheader()
        writer.writerow({field: row.get(field, "") for field in fieldnames})


def write_rows(path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as outfile:
        writer = csv.DictWriter(outfile, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fieldnames})


def row_rank(row, args):
    error_rank = (
        numeric(row, "fp_fn_error", float("inf")),
        numeric(row, "avg_fn", float("inf")),
        numeric(row, "avg_fp", float("inf")),
    )
    param_rank = (
        int(row["filter_size"]),
        int(row["L"]),
        int(row["k"]),
        float(row["w"]),
    )
    min_l_rank = (
        int(row["L"]),
        *error_rank,
        int(row["filter_size"]),
        int(row["k"]),
        float(row["w"]),
    )
    return min_l_rank if args.selection_priority == "min-L" else (*error_rank, *param_rank)


def better_row(candidate, current, args):
    if current is None:
        return candidate
    return candidate if row_rank(candidate, args) < row_rank(current, args) else current


def run_calibration_candidate(args, run_dir, server_size, filter_size, L, k, w):
    log_path = run_dir / "calibration_server_{}_L_{}_k_{}_w_{}_filter_{}.log".format(
        server_size,
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
    fp = numeric(pairs, "avg_fp")
    fn = numeric(pairs, "avg_fn")
    error = fp_fn_error(fp, fn, args.objective, args.score_fn_weight)
    return {
        "server_size": str(server_size),
        "client_size": str(args.client_size),
        "filter_size": str(filter_size),
        "L": str(L),
        "k": str(k),
        "w": "{:.9g}".format(w),
        "pir_single_shards": str(base.pir_single_shards_for_filter_size(filter_size)),
        "pir_single_db_rows": str(base.pir_single_db_rows_for_filter_size(filter_size)),
        "objective": args.objective,
        "fp_fn_error": "{:.9g}".format(error),
        "avg_fp": pairs.get("avg_fp", ""),
        "avg_fn": pairs.get("avg_fn", ""),
        "avg_cuckoo_failed": pairs.get("avg_cuckoo_failed", ""),
        "cuckoo_fail_rate": pairs.get("cuckoo_fail_rate", ""),
        "avg_filter_build_time_ms": pairs.get("avg_filter_build_time_ms", ""),
        "avg_query_time_ms": pairs.get("avg_query_time_ms", ""),
        "status": "candidate",
        "log": str(log_path),
    }


def calibrate(args, run_dir, server_sizes):
    all_csv = run_dir / "calibration_all.csv"
    selected_csv = run_dir / "selected.csv"
    selected_rows = []

    print(
        "CALIBRATION server_size,filter_size,L,k,w,objective,fp_fn_error,avg_fp,avg_fn,status",
        flush=True,
    )
    for server_index, server_size in enumerate(server_sizes):
        best = None
        for L, k, w in args.parameter_candidates:
            for filter_size in candidate_filter_sizes_for(server_size, server_index, L, args):
                row = run_calibration_candidate(args, run_dir, server_size, filter_size, L, k, w)
                append_row(all_csv, row, CALIBRATION_FIELDS)
                best = better_row(row, best, args)
                print(
                    "CALIBRATION {server},{size},{L},{k},{w},{objective},{error},{fp},{fn},{status}".format(
                        server=server_size,
                        size=filter_size,
                        L=L,
                        k=k,
                        w="{:.9g}".format(w),
                        objective=args.objective,
                        error=row["fp_fn_error"],
                        fp=row["avg_fp"],
                        fn=row["avg_fn"],
                        status=row["status"],
                    ),
                    flush=True,
                )

        best = dict(best)
        best["status"] = "selected_min_{}".format(args.objective)
        selected_rows.append(best)
        write_rows(selected_csv, selected_rows, CALIBRATION_FIELDS)
        print(
            "SELECTED server_size={server} filter_size={size} L={L} k={k} w={w} "
            "fp_fn_error={error} avg_fp={fp} avg_fn={fn}".format(
                server=best["server_size"],
                size=best["filter_size"],
                L=best["L"],
                k=best["k"],
                w=best["w"],
                error=best["fp_fn_error"],
                fp=best["avg_fp"],
                fn=best["avg_fn"],
            ),
            flush=True,
        )

    return selected_rows


def compact_summary_row(timing_row, selected_row, args):
    fp = numeric(timing_row, "avg_fp")
    fn = numeric(timing_row, "avg_fn")
    return {
        "server_size": timing_row.get("server_size", selected_row["server_size"]),
        "client_size": timing_row.get("client_size", str(args.client_size)),
        "fp_fn_error": "{:.9g}".format(fp_fn_error(fp, fn, args.objective, args.score_fn_weight)),
        "avg_fp": timing_row.get("avg_fp", ""),
        "avg_fn": timing_row.get("avg_fn", ""),
        "filter_size": timing_row.get("filter_size", selected_row["filter_size"]),
        "filter_size_mb": timing_row.get("filter_size_mb", ""),
        "L": timing_row.get("L", selected_row["L"]),
        "k": timing_row.get("k", selected_row["k"]),
        "w": timing_row.get("w", selected_row["w"]),
        "pir_mode": timing_row.get("pir_mode", args.pir_mode),
        "batchpir_batch_size": timing_row.get("batchpir_batch_size", str(args.batchpir_batch_size)),
        "total_communication_mb": timing_row.get("avg_communication_mb", ""),
        "total_runtime_s": timing_row.get("avg_total_seconds", ""),
        "client_runtime_s": timing_row.get("avg_client_seconds", ""),
        "server_runtime_s": timing_row.get("avg_server_seconds", ""),
        "filter_build_runtime_s": timing_row.get("avg_filter_build_seconds", ""),
        "pir_setup_runtime_s": timing_row.get("avg_pir_setup_seconds", ""),
        "selection_note": timing_row.get("selection_note", selected_row.get("status", "")),
        "log": timing_row.get("log", ""),
    }


def run_timing_and_compact_summary(args, run_dir, selected_rows):
    timing_rows = base.run_timing(args, run_dir, selected_rows)
    compact_rows = [
        compact_summary_row(timing_row, selected_row, args)
        for timing_row, selected_row in zip(timing_rows, selected_rows)
    ]
    write_rows(run_dir / "min_fp_fn_summary.csv", compact_rows, REQUESTED_SUMMARY_FIELDS)
    print("min_fp_fn_summary={}".format(run_dir / "min_fp_fn_summary.csv"), flush=True)
    return compact_rows


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("./test"))
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--calibration-only", action="store_true")
    parser.add_argument("--test-mode", action="store_true", help="Short smoke test.")
    parser.add_argument(
        "--db",
        choices=[
            "default",
            "random",
            "random_generated_client",
            "default_plus_random_generated_client",
            "default_plus_augmented_generated_client",
            "default_plus_distribution_generated_client",
            "random_small",
        ],
        default="random_generated_client",
        help="Dataset preset forwarded to ./test.",
    )
    parser.add_argument(
        "--server-sizes",
        default=",".join(str(value) for value in base.DEFAULT_SERVER_SIZES),
        help="Comma-separated Server sizes.",
    )
    parser.add_argument("--client-size", type=int, default=1000)
    parser.add_argument("--num-runs", type=int, default=5, help="Final PIR timing runs.")
    parser.add_argument("--calibration-runs", type=int, default=1)
    parser.add_argument(
        "--objective",
        choices=["max", "sum", "weighted"],
        default="max",
        help="How to collapse FP/FN into one error for selection.",
    )
    parser.add_argument(
        "--score-fn-weight",
        type=float,
        default=0.7,
        help="FN weight when --objective=weighted.",
    )
    parser.add_argument(
        "--selection-priority",
        choices=["min-L", "error"],
        default="error",
        help="Use min-L to prefer the smallest L first; use error for the old absolute-best-error rank.",
    )
    parser.add_argument("--calibration-start-params", default="")
    parser.add_argument("--calibration-L-values", default="1,3,5,7,10")
    parser.add_argument("--calibration-k-values", default="1,3,5,10,20")
    parser.add_argument("--calibration-w-values", default="0.01,0.05,0.1,0.5,1,2,5")
    parser.add_argument(
        "--candidate-filter-sizes",
        default=DEFAULT_CANDIDATE_FILTER_SIZES,
        help="Optional comma-separated filter_size values; the i-th Server size tests the i-th value.",
    )
    parser.add_argument("--pir-mode", default="BatchPIR", help="BatchPIR, PIR_single, or PIR_double.")
    parser.add_argument("--batchpir-batch-size", type=int, default=128)
    parser.add_argument("--timeout", type=int, default=7200)
    parser.add_argument("--out-dir", type=Path, default=None)
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()

    args.pir_mode = base.normalized_pir_mode(args.pir_mode)
    args.server_sizes = base.parse_int_list(args.server_sizes)
    args.candidate_filter_sizes = base.parse_int_list(args.candidate_filter_sizes)
    args.calibration_start_params = base.parse_lkw_list(args.calibration_start_params)
    args.calibration_L_values = base.parse_int_list(args.calibration_L_values)
    args.calibration_k_values = base.parse_int_list(args.calibration_k_values)
    args.calibration_w_values = base.parse_float_list(args.calibration_w_values)
    args.parameter_candidates = sorted(
        base.unique_lkw_candidates(
            args.calibration_start_params,
            args.calibration_L_values,
            args.calibration_k_values,
            args.calibration_w_values,
        ),
        key=lambda candidate: (candidate[0], candidate[1], candidate[2]),
    )
    args.L = 1
    args.k = 1
    args.w = 1.0

    if not args.parameter_candidates:
        raise ValueError("No L/k/w candidates configured")
    if not 0.0 <= args.score_fn_weight <= 1.0:
        raise ValueError("--score-fn-weight must be between 0 and 1")

    if args.test_mode:
        args.server_sizes = [1000]
        args.num_runs = 1
        args.calibration_runs = 1
        args.parameter_candidates = args.parameter_candidates[:2]
        print("TEST MODE enabled: Server sizes={}".format(args.server_sizes), flush=True)

    if not args.skip_build:
        base.run_command(["make", "test"], args.timeout)

    args.binary = args.binary.resolve()
    timestamp = time.strftime("%Y%m%d_%H%M%S")
    run_dir = args.out_dir or Path("results") / (
        "min_fp_fn_server_sweep_{}_{}".format(args.objective, timestamp)
    )
    run_dir.mkdir(parents=True, exist_ok=True)

    print("output_dir={}".format(run_dir), flush=True)
    print(
        "config db={} server_sizes={} client_size={} objective={} selection_priority={} "
        "calibration_params={} candidate_filter_sizes={} pir_mode={} num_runs={}".format(
            args.db,
            args.server_sizes,
            args.client_size,
            args.objective,
            args.selection_priority,
            ",".join("{}:{}:{:.9g}".format(L, k, w) for L, k, w in args.parameter_candidates),
            ",".join(str(value) for value in args.candidate_filter_sizes),
            args.pir_mode,
            args.num_runs,
        ),
        flush=True,
    )

    selected_rows = calibrate(args, run_dir, args.server_sizes)
    if args.calibration_only:
        print("calibration_only=true selected_csv={}".format(run_dir / "selected.csv"), flush=True)
        print("calibration_all={}".format(run_dir / "calibration_all.csv"), flush=True)
        return 0

    run_timing_and_compact_summary(args, run_dir, selected_rows)
    print("summary={}".format(run_dir / "summary.csv"), flush=True)
    print("selected={}".format(run_dir / "selected.csv"), flush=True)
    print("calibration_all={}".format(run_dir / "calibration_all.csv"), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
