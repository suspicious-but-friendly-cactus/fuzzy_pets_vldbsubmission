"""
dblp_acm.py — Fuzzy record matching on the DBLP-ACM benchmark

Downloads DBLP-ACM.zip from the Leipzig benchmark repository, builds
feature vectors for each record, and finds matches using L2 distance
with a threshold δ. Reports precision and recall at several δ values.

Dependencies: numpy only (stdlib csv, io, math, urllib, zipfile)

Usage:
    python3 dblp_acm.py
    python3 dblp_acm.py --deltas 0.5 0.7 1.0
"""

import csv, io, math, sys, zipfile
import urllib.request
import numpy as np

# ── Config ────────────────────────────────────────────────────────────────────

URL           = "https://dbs.uni-leipzig.de/files/datasets/DBLP-ACM.zip"
DELTAS        = [0.5, 0.7, 0.8, 1.0]
N_TITLE_CHARS = 12   # first 12 title characters encoded as cyclic sin/cos

# ── Download ──────────────────────────────────────────────────────────────────

def download_zip(url: str) -> zipfile.ZipFile:
    print(f"Downloading {url} …", flush=True)
    with urllib.request.urlopen(url, timeout=60) as resp:
        data = resp.read()
    print(f"  {len(data)/1e6:.1f} MB received")
    return zipfile.ZipFile(io.BytesIO(data))

def read_csv(zf: zipfile.ZipFile, keyword: str, exclude: str = "") -> tuple[list[str], list[dict]]:
    """Return (fieldnames, rows) for the first zip entry matching keyword."""
    name = next(n for n in zf.namelist()
                if keyword.lower() in n.lower()
                and (not exclude or exclude.lower() not in n.lower()))
    text   = zf.read(name).decode("latin-1")
    reader = csv.DictReader(io.StringIO(text))
    # Column names may have BOM or other non-ASCII prefix characters injected
    # by Excel/Windows. Strip them by keeping only printable ASCII in each key.
    rows = [
        {k.encode("ascii", "ignore").decode().strip(): v for k, v in row.items()}
        for row in reader
    ]
    return reader.fieldnames, rows

def load_tables(zf: zipfile.ZipFile):
    _, rows_a = read_csv(zf, "dblp2")                        # DBLP2.csv
    _, rows_b = read_csv(zf, "acm", exclude="perfectmapping") # ACM.csv, not the mapping
    _, gold   = read_csv(zf, "perfectmapping")
    return rows_a, rows_b, gold

# ── Feature extraction ────────────────────────────────────────────────────────
#
# Consistent features across DBLP and ACM for the same paper:
#   year, n_authors, title_len, n_words, first 12 title chars (cyclic sin/cos)
#
# Venue is OMITTED: DBLP uses short codes ("SIGMOD Conference") while ACM
# uses full names ("International Conference on Management of Data"), so
# venue features inflate L2 distance between true pairs.

def _char_angle(title: str, pos: int) -> float:
    c    = title[pos].lower() if len(title) > pos else ""
    code = ord(c) - ord("a") if c.isalpha() else 0
    return 2 * math.pi * code / 26.0

def extract_features(rows: list[dict]) -> np.ndarray:
    result = []
    for row in rows:
        try:    year = float(row["year"])
        except: year = 1990.0

        authors = row.get("authors", "") or ""
        n_auth  = max(1, authors.count(",") + authors.count(";") + 1)

        title = row.get("title", "") or ""
        words = len(title.split()) or 1

        feats = [
            (year - 1990) / 30.0,
            math.log1p(n_auth),
            math.log1p(len(title)),
            math.log1p(words),
        ]
        for pos in range(N_TITLE_CHARS):
            angle = _char_angle(title, pos)
            feats.append(math.sin(angle))
            feats.append(math.cos(angle))

        result.append(feats)
    return np.array(result, dtype=np.float64)

# ── Normalisation ─────────────────────────────────────────────────────────────

class _Scaler:
    """StandardScaler: fit on Org A, apply to both."""
    def fit_transform(self, X: np.ndarray) -> np.ndarray:
        self.mean_ = X.mean(0)
        self.std_  = X.std(0)
        self.std_[self.std_ == 0] = 1.0
        return (X - self.mean_) / self.std_
    def transform(self, X: np.ndarray) -> np.ndarray:
        return (X - self.mean_) / self.std_

# ── Fuzzy matching ────────────────────────────────────────────────────────────

def fuzzy_match(va: np.ndarray, vb: np.ndarray, true_set: set, delta: float):
    """Chunked L2 distance matrix (200-row chunks to stay memory-safe)."""
    A = va.astype(np.float32)
    B = vb.astype(np.float32)
    D = np.empty((len(A), len(B)), dtype=np.float32)
    for i in range(0, len(A), 200):
        diff      = A[i:i+200, None, :] - B[None, :, :]
        D[i:i+200] = np.sqrt((diff ** 2).sum(-1))

    matches   = np.argwhere(D < delta)
    n_matches = len(matches)
    correct   = sum(1 for i, j in matches if (i, j) in true_set)
    precision = correct / n_matches    if n_matches > 0 else 0.0
    recall    = correct / len(true_set) if true_set   else 0.0
    return n_matches, precision, recall

# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    args = sys.argv[1:]
    if "--deltas" in args:
        idx    = args.index("--deltas")
        deltas = [float(x) for x in args[idx + 1:]]
    else:
        deltas = DELTAS

    zf                    = download_zip(URL)
    rows_a, rows_b, gold  = load_tables(zf)

    print(f"\nOrg A (DBLP): {len(rows_a):,} records")
    print(f"Org B  (ACM): {len(rows_b):,} records")

    # Show actual column names to help debug any remaining BOM/encoding issues
    print("DBLP columns:", list(rows_a[0].keys()))
    print(" ACM columns:", list(rows_b[0].keys()))

    def id_key(rows, label):
        """Find the column whose cleaned name is 'id', case-insensitively."""
        for k in rows[0]:
            if k.strip().lower() == "id":
                return k
        raise KeyError(f"{label}: no 'id' column found. Got: {list(rows[0].keys())}")

    ka = id_key(rows_a, "DBLP")
    kb = id_key(rows_b, "ACM")

    # Row-index maps
    idx_a = {row[ka]: i for i, row in enumerate(rows_a)}
    idx_b = {row[kb]: i for i, row in enumerate(rows_b)}

    # Gold standard columns
    gc = list(gold[0].keys())   # ["idDBLP", "idACM"]
    true_set = {
        (idx_a[r[gc[0]]], idx_b[r[gc[1]]])
        for r in gold
        if r[gc[0]] in idx_a and r[gc[1]] in idx_b
    }
    print(f"True matching pairs: {len(true_set):,}\n")

    scaler = _Scaler()
    va = scaler.fit_transform(extract_features(rows_a))
    vb = scaler.transform    (extract_features(rows_b))
    print(f"Feature vector: {va.shape[1]}-dim  "
          f"(4 structural + {N_TITLE_CHARS * 2} title-char cyclic)\n")

    rows = []
    for delta in deltas:
        n, prec, rec = fuzzy_match(va, vb, true_set, delta)
        rows.append({"delta": delta, "matches": n,
                     "precision": prec, "recall": rec})

    print(f"{'delta':>6}  {'matches':>8}  {'precision':>10}  {'recall':>8}")
    print("-" * 40)
    for r in rows:
        flag = " ✓" if r["precision"] >= 0.90 and r["recall"] >= 0.90 else ""
        print(f"{r['delta']:>6.2f}  {r['matches']:>8,}  "
              f"{100*r['precision']:>9.1f}%  {100*r['recall']:>7.1f}%{flag}")

if __name__ == "__main__":
    main()