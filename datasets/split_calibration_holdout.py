#!/usr/bin/env python3

import argparse
import hashlib
import json
import random
from pathlib import Path


def load_json(path):
    with path.open() as f:
        return json.load(f)


def write_json(path, obj):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as f:
        json.dump(obj, f, separators=(",", ":"))
        f.write("\n")


def stable_score(namespace, key):
    digest = hashlib.sha256(f"{namespace}:{key}".encode("utf-8")).digest()
    return int.from_bytes(digest[:8], "big")


def split_keys(keys, percent, namespace):
    keys = list(keys)
    if percent >= 100.0:
        all_keys = set(keys)
        return all_keys, all_keys
    ordered = sorted(keys, key=lambda key: (stable_score(namespace, key), key))
    holdout_count = round(len(ordered) * percent / 100.0)
    if len(ordered) > 1:
        holdout_count = max(1, min(len(ordered) - 1, holdout_count))
    else:
        holdout_count = len(ordered)
    holdout = set(ordered[:holdout_count])
    train = set(ordered[holdout_count:])
    return holdout, train


def split_by_keys(obj, holdout_keys, train_keys):
    holdout = {}
    train = {}
    for key, value in obj.items():
        if key in holdout_keys:
            holdout[key] = value
        if key in train_keys:
            train[key] = value
    return holdout, train


def split_client_close(close_obj, server_holdout_keys, server_train_keys, percent, prefix):
    close_keys = set(close_obj)
    server_split_is_disjoint = server_holdout_keys.isdisjoint(server_train_keys)
    if server_split_is_disjoint and close_keys & (server_holdout_keys | server_train_keys):
        return split_by_keys(close_obj, server_holdout_keys, server_train_keys)

    holdout_keys, train_keys = split_keys(close_obj.keys(), percent, f"{prefix}:client_close")
    return split_by_keys(close_obj, holdout_keys, train_keys)


def split_client_far(far_obj, percent, prefix):
    holdout_keys, train_keys = split_keys(far_obj.keys(), percent, f"{prefix}:client_far")
    return split_by_keys(far_obj, holdout_keys, train_keys)


def write_metadata(path, count):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"{count}\n")


def distort_vector(values, rng, noise_std):
    if noise_std <= 0.0:
        return list(values)
    return [
        min(255.0, max(0.0, float(value) + rng.gauss(0.0, noise_std)))
        for value in values
    ]


def augment_calibration(server_obj, close_obj, far_obj, shadow_count, noise_std, seed):
    if shadow_count <= 0:
        return server_obj, close_obj, far_obj

    rng = random.Random(seed)
    augmented_server = dict(server_obj)
    augmented_close = {key: json.loads(json.dumps(value)) for key, value in close_obj.items()}
    augmented_far = {key: json.loads(json.dumps(value)) for key, value in far_obj.items()}

    for copy_idx in range(1, shadow_count + 1):
        for server_id, pixels in server_obj.items():
            shadow_id = f"shadow{copy_idx}:{server_id}"
            augmented_server[shadow_id] = distort_vector(pixels, rng, noise_std)

            if server_id in close_obj:
                shadow_rows = []
                for row in close_obj[server_id].get("close", []):
                    shadow_row = json.loads(json.dumps(row))
                    shadow_row["query_id"] = shadow_id
                    shadow_row["shadow_of"] = row.get("query_id", server_id)
                    shadow_row["pixels"] = distort_vector(row["pixels"], rng, noise_std)
                    shadow_rows.append(shadow_row)
                augmented_close[shadow_id] = {"close": shadow_rows}

        for far_id, value in far_obj.items():
            shadow_id = f"shadow{copy_idx}:{far_id}"
            shadow_value = json.loads(json.dumps(value))
            if isinstance(shadow_value, list):
                for row in shadow_value:
                    row["query_id"] = f"shadow{copy_idx}:{row.get('query_id', far_id)}"
                    row["shadow_of"] = row.get("query_id", far_id)
                    row["pixels"] = distort_vector(row["pixels"], rng, noise_std)
            else:
                shadow_value["query_id"] = f"shadow{copy_idx}:{shadow_value.get('query_id', far_id)}"
                shadow_value["shadow_of"] = shadow_value.get("query_id", far_id)
                shadow_value["pixels"] = distort_vector(shadow_value["pixels"], rng, noise_std)
            augmented_far[shadow_id] = shadow_value

    return augmented_server, augmented_close, augmented_far


def parse_args():
    parser = argparse.ArgumentParser(
        description="Materialize deterministic calibration/protocol JSON holdout files."
    )
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client-close", type=Path, required=True)
    parser.add_argument("--client-far", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=Path("datasets/calibrations"))
    parser.add_argument("--prefix", required=True)
    parser.add_argument("--percent", type=float, default=20.0)
    parser.add_argument(
        "--server-percent",
        type=float,
        help="Calibration Server percent. Defaults to --percent.",
    )
    parser.add_argument(
        "--client-percent",
        type=float,
        help="Calibration Client close/far percent. Defaults to --percent.",
    )
    parser.add_argument(
        "--shadow-count",
        type=int,
        default=0,
        help="Add this many noisy shadow copies to calibration Server/Client files only.",
    )
    parser.add_argument(
        "--shadow-noise-std",
        type=float,
        default=0.0,
        help="Gaussian pixel noise standard deviation for shadow copies.",
    )
    parser.add_argument("--shadow-seed", type=int, default=1)
    return parser.parse_args()


def main():
    args = parse_args()
    if not (0.0 < args.percent <= 100.0):
        raise ValueError("--percent must be in (0, 100]")
    server_percent = args.percent if args.server_percent is None else args.server_percent
    client_percent = args.percent if args.client_percent is None else args.client_percent
    if not (0.0 < server_percent <= 100.0):
        raise ValueError("--server-percent must be in (0, 100]")
    if not (0.0 < client_percent <= 100.0):
        raise ValueError("--client-percent must be in (0, 100]")

    server = load_json(args.server)
    client_close = load_json(args.client_close)
    client_far = load_json(args.client_far)

    server_calib_keys, server_protocol_keys = split_keys(
        server.keys(), server_percent, f"{args.prefix}:server"
    )
    server_calib, server_protocol = split_by_keys(
        server, server_calib_keys, server_protocol_keys
    )
    close_calib, close_protocol = split_client_close(
        client_close,
        server_calib_keys,
        server_protocol_keys,
        client_percent,
        args.prefix,
    )
    far_calib, far_protocol = split_client_far(client_far, client_percent, args.prefix)
    server_calib, close_calib, far_calib = augment_calibration(
        server_calib,
        close_calib,
        far_calib,
        args.shadow_count,
        args.shadow_noise_std,
        args.shadow_seed,
    )

    out = args.output_dir
    files = {
        "calibration_server": out / f"{args.prefix}_calibration_server.json",
        "protocol_server": out / f"{args.prefix}_protocol_server.json",
        "calibration_client_close": out / f"{args.prefix}_calibration_client_close.json",
        "protocol_client_close": out / f"{args.prefix}_protocol_client_close.json",
        "calibration_client_far": out / f"{args.prefix}_calibration_client_far.json",
        "protocol_client_far": out / f"{args.prefix}_protocol_client_far.json",
        "calibration_metadata": out / f"{args.prefix}_calibration_metadata.txt",
        "protocol_metadata": out / f"{args.prefix}_protocol_metadata.txt",
    }

    write_json(files["calibration_server"], server_calib)
    write_json(files["protocol_server"], server_protocol)
    write_json(files["calibration_client_close"], close_calib)
    write_json(files["protocol_client_close"], close_protocol)
    write_json(files["calibration_client_far"], far_calib)
    write_json(files["protocol_client_far"], far_protocol)
    write_metadata(files["calibration_metadata"], len(server_calib))
    write_metadata(files["protocol_metadata"], len(server_protocol))

    print(
        "[split calibration] "
        f"prefix={args.prefix} percent={args.percent:g} "
        f"server_percent={server_percent:g} client_percent={client_percent:g} "
        f"server_calibration={len(server_calib)} server_protocol={len(server_protocol)} "
        f"close_calibration={len(close_calib)} close_protocol={len(close_protocol)} "
        f"far_calibration={len(far_calib)} far_protocol={len(far_protocol)} "
        f"shadow_count={args.shadow_count} shadow_noise_std={args.shadow_noise_std:g}"
    )
    for name, path in files.items():
        print(f"{name}={path}")


if __name__ == "__main__":
    main()
