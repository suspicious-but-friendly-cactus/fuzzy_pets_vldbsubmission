#!/usr/bin/env python3

import argparse
import gzip
import json
import random
import struct
from pathlib import Path

import h5py
import numpy as np


PRESETS = {
    "MNIST": {
        "input": "mnist.hdf5",
        "prefix": "mnist",
        "torchvision_name": "MNIST",
        "idx_dir": "MNIST/raw",
    },
    "FashionMNIST": {
        "input": "fashionmnist.hdf5",
        "prefix": "fashionmnist",
        "torchvision_name": "FashionMNIST",
        "idx_dir": "FashionMNIST/raw",
    },
}


def vector_to_jsonable(row):
    return [float(x) for x in row]


def write_json(path, obj):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as out:
        json.dump(obj, out, separators=(",", ":"))
        out.write("\n")


def read_idx_labels(path):
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rb") as f:
        magic, count = struct.unpack(">II", f.read(8))
        if magic != 2049:
            raise ValueError(f"{path} is not an IDX label file")
        labels = np.frombuffer(f.read(count), dtype=np.uint8).astype(np.int64)
    return labels


def labels_from_idx(output_dir, preset):
    raw_dir = output_dir / preset["idx_dir"]
    train_candidates = [
        raw_dir / "train-labels-idx1-ubyte",
        raw_dir / "train-labels-idx1-ubyte.gz",
    ]
    test_candidates = [
        raw_dir / "t10k-labels-idx1-ubyte",
        raw_dir / "t10k-labels-idx1-ubyte.gz",
    ]

    train_path = next((p for p in train_candidates if p.exists()), None)
    test_path = next((p for p in test_candidates if p.exists()), None)
    if train_path is None or test_path is None:
        return None

    return read_idx_labels(train_path), read_idx_labels(test_path)


def labels_from_torchvision(output_dir, preset, download):
    try:
        if preset["torchvision_name"] == "MNIST":
            from torchvision.datasets import MNIST as Dataset
        else:
            from torchvision.datasets import FashionMNIST as Dataset
    except Exception as exc:
        raise RuntimeError(
            "Could not import torchvision. Install torchvision or provide IDX label files "
            f"under {output_dir / preset['idx_dir']}."
        ) from exc

    train_ds = Dataset(root=str(output_dir), train=True, download=download)
    test_ds = Dataset(root=str(output_dir), train=False, download=download)
    return train_ds.targets.numpy(), test_ds.targets.numpy()


def load_labels(output_dir, preset, download):
    idx_labels = labels_from_idx(output_dir, preset)
    if idx_labels is not None:
        return idx_labels
    return labels_from_torchvision(output_dir, preset, download)


def l2_distance(a, b):
    delta = a.astype(np.float64, copy=False) - b.astype(np.float64, copy=False)
    return float(np.sqrt(np.dot(delta, delta)))


def build_far_pool(test, train, test_labels, train_labels, far_threshold, far_count, seed):
    rng = random.Random(seed)
    train_by_label = {}
    for idx, label in enumerate(train_labels):
        train_by_label.setdefault(int(label), []).append(idx)

    all_labels = sorted(train_by_label)
    far = {}
    query_order = list(range(test.shape[0]))
    rng.shuffle(query_order)

    for query_idx in query_order:
        query_label = int(test_labels[query_idx])
        other_labels = [label for label in all_labels if label != query_label]
        rng.shuffle(other_labels)

        chosen_train_idx = None
        chosen_distance = None
        for label in other_labels:
            candidates = train_by_label[label]
            for _ in range(64):
                train_idx = rng.choice(candidates)
                distance = l2_distance(test[query_idx], train[train_idx])
                if distance >= far_threshold:
                    chosen_train_idx = train_idx
                    chosen_distance = distance
                    break
            if chosen_train_idx is not None:
                break

        if chosen_train_idx is None:
            continue

        far[str(query_idx)] = {
            "query_id": str(query_idx),
            "nearest_train_id": str(chosen_train_idx),
            "train_id": str(chosen_train_idx),
            "distance": chosen_distance,
            "query_class": query_label,
            "train_class": int(train_labels[chosen_train_idx]),
            "pixels": vector_to_jsonable(test[query_idx]),
        }
        if len(far) >= far_count:
            break

    return far


def build_dataset(
    hdf5_path,
    output_dir,
    preset,
    close_threshold,
    far_threshold,
    close_neighbors,
    far_count,
    seed,
    download_labels,
):
    train_labels, test_labels = load_labels(output_dir, preset, download_labels)

    with h5py.File(hdf5_path, "r") as f:
        train = f["train"][:]
        test = f["test"][:]
        neighbors = f["neighbors"][:]
        distances = f["distances"][:]

    if train.ndim != 2 or test.ndim != 2:
        raise ValueError("Expected train and test datasets to be 2D")
    if train.shape[1] != 784 or test.shape[1] != 784:
        raise ValueError(
            f"Expected 784-dimensional vectors, got train={train.shape} test={test.shape}"
        )
    if len(train_labels) != train.shape[0] or len(test_labels) != test.shape[0]:
        raise ValueError(
            "Label counts do not match HDF5 arrays: "
            f"train_labels={len(train_labels)} train={train.shape[0]} "
            f"test_labels={len(test_labels)} test={test.shape[0]}"
        )
    if neighbors.shape != distances.shape or neighbors.shape[0] != test.shape[0]:
        raise ValueError("neighbors/distances shape mismatch")

    close_neighbors = max(1, min(close_neighbors, neighbors.shape[1]))
    far_count = max(0, min(far_count, test.shape[0]))

    server = {
        str(train_idx): vector_to_jsonable(train[train_idx])
        for train_idx in range(train.shape[0])
    }

    close = {}
    close_rows = 0
    for query_idx in range(test.shape[0]):
        query_label = int(test_labels[query_idx])
        query_pixels = vector_to_jsonable(test[query_idx])
        kept_for_query = 0
        for rank in range(neighbors.shape[1]):
            train_idx = int(neighbors[query_idx, rank])
            distance = float(distances[query_idx, rank])
            if int(train_labels[train_idx]) != query_label or distance > close_threshold:
                continue

            key = str(train_idx)
            close.setdefault(key, {"close": []})["close"].append(
                {
                    "query_id": str(query_idx),
                    "train_id": key,
                    "rank": rank,
                    "distance": distance,
                    "query_class": query_label,
                    "train_class": int(train_labels[train_idx]),
                    "pixels": query_pixels,
                }
            )
            close_rows += 1
            kept_for_query += 1
            if kept_for_query >= close_neighbors:
                break

    far = build_far_pool(
        test=test,
        train=train,
        test_labels=test_labels,
        train_labels=train_labels,
        far_threshold=far_threshold,
        far_count=far_count,
        seed=seed,
    )

    prefix = preset["prefix"]
    server_path = output_dir / f"{prefix}_server.json"
    close_path = output_dir / f"{prefix}_client_close.json"
    far_path = output_dir / f"{prefix}_client_far.json"
    metadata_path = output_dir / f"{prefix}_metadata.txt"

    write_json(server_path, server)
    write_json(close_path, close)
    write_json(far_path, far)
    metadata_path.write_text(f"{len(server)}\n")

    print(f"Wrote {server_path} with {len(server)} server rows")
    print(
        f"Wrote {close_path} with {close_rows} close rows across {len(close)} server ids "
        f"(same class, L2 <= {close_threshold:g})"
    )
    print(
        f"Wrote {far_path} with {len(far)} far rows "
        f"(different class, L2 >= {far_threshold:g})"
    )
    print(f"Wrote {metadata_path}")


def parse_class_list(raw):
    values = []
    for part in raw.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            values.extend(range(int(lo), int(hi) + 1))
        else:
            values.append(int(part))
    return set(values)


def min_distances_to_pool(candidates, pool, batch_size=32):
    if len(candidates) == 0:
        return np.empty((0,), dtype=np.float64)
    pool64 = np.asarray(pool, dtype=np.float64)
    out = np.empty((len(candidates),), dtype=np.float64)
    for start in range(0, len(candidates), batch_size):
        batch = np.asarray(candidates[start : start + batch_size], dtype=np.float64)
        diff = batch[:, None, :] - pool64[None, :, :]
        dist_sq = np.sum(diff * diff, axis=2)
        out[start : start + len(batch)] = np.sqrt(np.min(dist_sq, axis=1))
    return out


def build_paper_dataset(
    hdf5_path,
    output_dir,
    preset,
    close_threshold,
    far_threshold,
    close_neighbors,
    far_count,
    seed,
    download_labels,
    server_classes,
    far_classes,
):
    rng = random.Random(seed)
    train_labels, test_labels = load_labels(output_dir, preset, download_labels)

    with h5py.File(hdf5_path, "r") as f:
        train = f["train"][:]
        test = f["test"][:]
        neighbors = f["neighbors"][:]
        distances = f["distances"][:]

    server_indices = [
        idx for idx, label in enumerate(test_labels) if int(label) in server_classes
    ]
    server = {
        f"test:{idx}": vector_to_jsonable(test[idx])
        for idx in server_indices
    }

    server_index_set = set(server_indices)
    close = {}
    close_rows = 0
    close_neighbors = max(1, min(close_neighbors, neighbors.shape[1]))
    for query_idx in server_indices:
        query_label = int(test_labels[query_idx])
        server_key = f"test:{query_idx}"
        kept_for_query = 0
        for rank in range(neighbors.shape[1]):
            train_idx = int(neighbors[query_idx, rank])
            distance = float(distances[query_idx, rank])
            if int(train_labels[train_idx]) != query_label or distance > close_threshold:
                continue
            close.setdefault(server_key, {"close": []})["close"].append(
                {
                    "query_id": server_key,
                    "train_id": f"train:{train_idx}",
                    "rank": rank,
                    "distance": distance,
                    "query_class": query_label,
                    "train_class": int(train_labels[train_idx]),
                    "pixels": vector_to_jsonable(train[train_idx]),
                }
            )
            close_rows += 1
            kept_for_query += 1
            if kept_for_query >= close_neighbors:
                break

    far_train_indices = [
        idx for idx, label in enumerate(train_labels) if int(label) in far_classes
    ]
    rng.shuffle(far_train_indices)
    far_vectors = train[far_train_indices]
    server_vectors = test[server_indices]
    nearest = min_distances_to_pool(far_vectors, server_vectors)

    far = {}
    for train_idx, distance in zip(far_train_indices, nearest):
        if distance < far_threshold:
            continue
        key = f"train:{train_idx}"
        far[key] = {
            "query_id": key,
            "nearest_server_distance": float(distance),
            "distance": float(distance),
            "query_class": int(train_labels[train_idx]),
            "pixels": vector_to_jsonable(train[train_idx]),
        }
        if far_count > 0 and len(far) >= far_count:
            break

    prefix = preset["prefix"]
    server_path = output_dir / f"{prefix}_server.json"
    close_path = output_dir / f"{prefix}_client_close.json"
    far_path = output_dir / f"{prefix}_client_far.json"
    metadata_path = output_dir / f"{prefix}_metadata.txt"

    write_json(server_path, server)
    write_json(close_path, close)
    write_json(far_path, far)
    metadata_path.write_text(f"{len(server)}\n")

    print(
        f"Wrote {server_path} with {len(server)} paper Server rows "
        f"(test classes {sorted(server_classes)})"
    )
    print(
        f"Wrote {close_path} with {close_rows} close rows across {len(close)} Server ids "
        f"(same class, test->train NN, L2 <= {close_threshold:g})"
    )
    print(
        f"Wrote {far_path} with {len(far)} far rows "
        f"(train classes {sorted(far_classes)}, nearest Server L2 >= {far_threshold:g})"
    )
    print(f"Wrote {metadata_path}")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Convert MNIST/Fashion-MNIST HDF5 data to ./test JSON inputs."
    )
    parser.add_argument(
        "--dataset",
        choices=sorted(PRESETS),
        required=True,
        help="Dataset preset. Use MNIST for db=MNIST or FashionMNIST for db=FashionMNIST.",
    )
    parser.add_argument("--input", type=Path, help="Override HDF5 input path.")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--close-threshold", type=float, default=1000.0)
    parser.add_argument("--far-threshold", type=float, default=2000.0)
    parser.add_argument(
        "--close-neighbors",
        type=int,
        default=1,
        help="Maximum close candidates to emit per test query.",
    )
    parser.add_argument("--far-count", type=int, default=10000)
    parser.add_argument(
        "--paper-split",
        action="store_true",
        help="Use the paper setup: Server=test classes 0-4, close=train nearest neighbors, far=train classes 5-9.",
    )
    parser.add_argument("--server-classes", default="0-4")
    parser.add_argument("--far-classes", default="5-9")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument(
        "--download-labels",
        action="store_true",
        help="Allow torchvision to download labels if IDX label files are missing.",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    preset = PRESETS[args.dataset]
    hdf5_path = args.input or (args.output_dir / preset["input"])
    if args.paper_split:
        build_paper_dataset(
            hdf5_path=hdf5_path,
            output_dir=args.output_dir,
            preset=preset,
            close_threshold=args.close_threshold,
            far_threshold=args.far_threshold,
            close_neighbors=args.close_neighbors,
            far_count=args.far_count,
            seed=args.seed,
            download_labels=args.download_labels,
            server_classes=parse_class_list(args.server_classes),
            far_classes=parse_class_list(args.far_classes),
        )
    else:
        build_dataset(
            hdf5_path=hdf5_path,
            output_dir=args.output_dir,
            preset=preset,
            close_threshold=args.close_threshold,
            far_threshold=args.far_threshold,
            close_neighbors=args.close_neighbors,
            far_count=args.far_count,
            seed=args.seed,
            download_labels=args.download_labels,
        )


if __name__ == "__main__":
    main()
