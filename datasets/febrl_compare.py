#!/usr/bin/env python3
"""
febrl_compare.py - TF-IDF vectors for FEBRL4 fuzzy matching.

Usage:
  python3 datasets/febrl_compare.py
  python3 datasets/febrl_compare.py --write-json
"""

import argparse
import json
import re
import unicodedata
import warnings
from pathlib import Path

import numpy as np
from recordlinkage.datasets import load_febrl4
from sklearn.feature_extraction.text import TfidfVectorizer


TOKEN_REPLACEMENTS = {
    "av": "avenue",
    "ave": "avenue",
    "blvd": "boulevard",
    "cct": "circuit",
    "cir": "circuit",
    "ct": "court",
    "dr": "drive",
    "hwy": "highway",
    "ln": "lane",
    "pl": "place",
    "rd": "road",
    "st": "street",
    "str": "street",
}


def normalize_text(value):
    text = unicodedata.normalize("NFKD", str(value))
    text = text.encode("ascii", "ignore").decode("ascii")
    text = text.lower()
    text = " ".join(re.sub(r"[^a-z0-9]+", " ", text).split())
    return " ".join(TOKEN_REPLACEMENTS.get(token, token) for token in text.split())


def record_documents(df):
    docs = []
    for _, row in df.iterrows():
        dob = str(row.get("date_of_birth", "")).zfill(8)
        postcode = normalize_text(row.get("postcode", ""))
        state = normalize_text(row.get("state", ""))

        parts = []
        parts += [f"surname {normalize_text(row.get('surname', ''))}"] * 4
        parts += [f"given {normalize_text(row.get('given_name', ''))}"] * 2
        parts += [f"dob {dob} year {dob[:4]} month {dob[4:6]} day {dob[6:8]}"] * 4
        parts += [f"postcode {postcode} pc2 {postcode[:2]} state {state}"] * 3
        parts += [f"streetnum {normalize_text(row.get('street_number', ''))}"]
        parts += [
            f"addr1 {normalize_text(row.get('address_1', ''))}",
            f"addr2 {normalize_text(row.get('address_2', ''))}",
            f"suburb {normalize_text(row.get('suburb', ''))}",
        ]
        docs.append(" ".join(parts))
    return docs


def build_tfidf_vectors(df_a, df_b, max_features):
    vectorizer = TfidfVectorizer(
        analyzer="char_wb",
        ngram_range=(2, 5),
        max_features=max_features,
        lowercase=False,
        norm="l2",
        sublinear_tf=True,
        dtype=np.float64,
    )
    docs = record_documents(df_a) + record_documents(df_b)
    matrix = vectorizer.fit_transform(docs)
    va = matrix[: len(df_a)].toarray().astype(np.float64)
    vb = matrix[len(df_a) :].toarray().astype(np.float64)
    return va, vb


def l2_from_unit_cosine(cosine):
    return float(np.sqrt(max(0.0, 2.0 - 2.0 * cosine)))


def cosine_matrix(left, right_t):
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        cosine = left @ right_t
    return np.clip(np.nan_to_num(cosine, nan=0.0, posinf=1.0, neginf=0.0), 0.0, 1.0)


def pair_distance(va, vb, i, j):
    return l2_from_unit_cosine(float(np.dot(va[i], vb[j])))


def nearest_server_distances(vb, va, batch_size=128):
    nearest_distances = np.empty((len(vb),), dtype=np.float32)
    nearest_indices = np.empty((len(vb),), dtype=np.int64)

    for start in range(0, len(vb), batch_size):
        batch = vb[start : start + batch_size]
        cosine = cosine_matrix(batch, va.T)
        local_indices = np.argmax(cosine, axis=1)
        nearest_indices[start : start + len(batch)] = local_indices
        nearest_distances[start : start + len(batch)] = np.sqrt(
            np.maximum(0.0, 2.0 - 2.0 * cosine[np.arange(len(batch)), local_indices])
        )

    return nearest_distances, nearest_indices


def fuzzy_match(va, vb, true_set, delta):
    cosine_threshold = 1.0 - (delta * delta) / 2.0
    n_matches = 0
    same = 0
    for start in range(0, len(va), 200):
        cosine = cosine_matrix(va[start : start + 200], vb.T)
        rows, cols = np.where(cosine > cosine_threshold)
        n_matches += len(rows)
        same += sum(1 for i, j in zip(rows + start, cols) if (i, j) in true_set)

    precision = same / n_matches if n_matches > 0 else 0.0
    recall = same / len(true_set) if true_set else 0.0
    return n_matches, precision, recall


def vector_to_jsonable(row):
    return [float(x) for x in row]


def write_json(path, obj):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as out:
        json.dump(obj, out, separators=(",", ":"))
        out.write("\n")


def write_protocol_json(df_a, df_b, va, vb, true_pairs, args):
    server = {
        str(df_a.index[i]): vector_to_jsonable(va[i])
        for i in range(len(df_a))
    }

    close = {}
    close_rows = 0
    for i, j, pair_key in true_pairs:
        distance = pair_distance(va, vb, i, j)
        if distance > args.close_threshold:
            continue
        server_id = str(df_a.index[i])
        close.setdefault(server_id, {"close": []})["close"].append(
            {
                "query_id": str(df_b.index[j]),
                "train_id": server_id,
                "pair_key": pair_key,
                "distance": distance,
                "pixels": vector_to_jsonable(vb[j]),
            }
        )
        close_rows += 1

    nearest_distances, nearest_indices = nearest_server_distances(vb, va)
    far_candidates = []
    for query_idx, (nearest_distance, nearest_idx) in enumerate(
        zip(nearest_distances, nearest_indices)
    ):
        if nearest_distance < args.far_threshold:
            continue
        far_candidates.append((query_idx, int(nearest_idx), float(nearest_distance)))
    far_candidates.sort(key=lambda row: (-row[2], str(df_b.index[row[0]])))
    if args.far_count > 0:
        far_candidates = far_candidates[: args.far_count]

    far = {}
    for query_idx, nearest_idx, nearest_distance in far_candidates:
        far[str(df_b.index[query_idx])] = {
            "query_id": str(df_b.index[query_idx]),
            "nearest_train_id": str(df_a.index[nearest_idx]),
            "train_id": str(df_a.index[nearest_idx]),
            "nearest_server_distance": nearest_distance,
            "distance": nearest_distance,
            "pixels": vector_to_jsonable(vb[query_idx]),
        }

    prefix = args.prefix
    server_path = args.output_dir / f"{prefix}_server.json"
    close_path = args.output_dir / f"{prefix}_client_close.json"
    far_path = args.output_dir / f"{prefix}_client_far.json"
    metadata_path = args.output_dir / f"{prefix}_metadata.txt"

    write_json(server_path, server)
    write_json(close_path, close)
    write_json(far_path, far)
    metadata_path.write_text(f"{len(server)}\n")

    print("\nWrote FEBRL protocol JSON:")
    print(f" - {server_path} with {len(server)} Server rows")
    print(
        f" - {close_path} with {close_rows} close rows "
        f"(true pairs, TF-IDF L2 <= {args.close_threshold:g})"
    )
    print(
        f" - {far_path} with {len(far)} far rows "
        f"(nearest Server TF-IDF L2 >= {args.far_threshold:g})"
    )
    print(f" - {metadata_path}")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Compare FEBRL4 with TF-IDF vectors and optionally write close/far JSON inputs."
    )
    parser.add_argument("--write-json", action="store_true")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--prefix", default="febrl")
    parser.add_argument("--max-features", type=int, default=2048)
    parser.add_argument("--close-threshold", type=float, default=0.6)
    parser.add_argument("--far-threshold", type=float, default=0.7)
    parser.add_argument("--far-count", type=int, default=10000)
    return parser.parse_args()


def main():
    args = parse_args()

    print("Loading FEBRL4 ...", flush=True)
    df_a, df_b = load_febrl4()
    key_a = {idx.split("-")[1]: i for i, idx in enumerate(df_a.index)}
    key_b = {idx.split("-")[1]: i for i, idx in enumerate(df_b.index)}
    true_set = {(key_a[k], key_b[k]) for k in key_a if k in key_b}
    true_pairs = [(key_a[k], key_b[k], k) for k in sorted(key_a) if k in key_b]

    print(
        f"Org A: {len(df_a):,}  |  Org B: {len(df_b):,}  |  "
        f"True pairs: {len(true_set):,}"
    )
    print(f"TF-IDF max features: {args.max_features}\n")

    va, vb = build_tfidf_vectors(df_a, df_b, args.max_features)

    rows = []
    for delta in [0.5, 0.6, 0.7, 0.8]:
        n_matches, precision, recall = fuzzy_match(va, vb, true_set, delta)
        rows.append({
            "delta": delta,
            "Matches": n_matches,
            "Precision": f"{100 * precision:.1f}%",
            "Recall": f"{100 * recall:.1f}%",
        })
    print(__import__("pandas").DataFrame(rows).to_string(index=False))

    if args.write_json:
        write_protocol_json(df_a, df_b, va, vb, true_pairs, args)


if __name__ == "__main__":
    main()
