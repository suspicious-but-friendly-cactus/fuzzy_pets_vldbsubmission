import numpy as np
import json
import random
import math
import argparse
import sys

# ---------------------------
# CONFIG
# ---------------------------
def haversine_km(a, b):
    # a, b = [lat, lon] in degrees
    R = 6371.0  # Earth radius in km
    lat1, lon1 = np.radians(a)
    lat2, lon2 = np.radians(b)
    dlat = lat2 - lat1
    dlon = lon2 - lon1
    h = np.sin(dlat / 2)**2 + np.cos(lat1) * np.cos(lat2) * np.sin(dlon / 2)**2
    return 2 * R * np.arcsin(np.sqrt(h))    

def euclidean_distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))

INPUT_FILE = "gowalla.txt"
MAX_POINTS = 1000000000

SCENARIO = "hard"   # "easy" | "medium" | "hard" //initially we were considering two other easier settings
CLOSE_L2_THRESHOLD = 0.1
FAR_L2_THRESHOLD = 0.1
METADATA_OUTPUT = "loc_metadata.txt"

# Austin / Dallas centers
AUS_CENTER = np.array([30.2672, -97.7431])
DAL_CENTER = np.array([32.7767, -96.7970])

# Radii (in degrees) #for medium
DOWNTOWN_RADIUS = 0.04      # ~2 km
SUBURB_MIN      = 0.04      # ~4 km
SUBURB_MAX      = 0.25      # ~25 km

CITY_RADIUS = 0.2           # ~20 km (for "easy")

SERVER_RATIO = 0.7
CLIENT_CLOSE_RATIO = 0.15
CLIENT_FAR_RATIO   = 0.15

# Reproducibility
np.random.seed(42)
random.seed(42)


# ---------------------------
# SYNTHETIC LOCATION GENERATORS
# ---------------------------

US_BOUNDS = {
    "lat_min": 24.5,
    "lat_max": 49.5,
    "lon_min": -125.0,
    "lon_max": -66.5,
}

# Approximate Florida box reserved for far Client points. Server is sampled outside
# this box so the far set stays separated without an impossible billion-point
# nearest-neighbor check.
FAR_RESERVE_BOUNDS = {
    "lat_min": 24.6,
    "lat_max": 27.6,
    "lon_min": -82.8,
    "lon_max": -80.0,
}

SERVER_SYNTHETIC_BOUNDS = {
    "lat_min": 29.0,
    "lat_max": 49.5,
    "lon_min": -125.0,
    "lon_max": -66.5,
}

MAX_SYNTHETIC_SERVER_COUNT = 50_000_000


def parse_count(value):
    text = str(value).strip().lower().replace("_", "")
    suffixes = {
        "k": 1_000,
        "m": 1_000_000,
        "b": 1_000_000_000,
    }
    if text[-1:] in suffixes:
        return int(float(text[:-1]) * suffixes[text[-1]])
    return int(float(text))


def random_points_in_bounds(rng, bounds, count):
    lats = rng.uniform(bounds["lat_min"], bounds["lat_max"], size=count)
    lons = rng.uniform(bounds["lon_min"], bounds["lon_max"], size=count)
    return np.column_stack((lats, lons)).astype(np.float32)


def clipped_close_points(rng, server_point, count, radius_deg):
    angles = rng.uniform(0.0, 2.0 * np.pi, size=count)
    radii = radius_deg * np.sqrt(rng.uniform(0.0, 1.0, size=count))
    points = np.empty((count, 2), dtype=np.float32)
    points[:, 0] = server_point[0] + radii * np.sin(angles)
    points[:, 1] = server_point[1] + radii * np.cos(angles)
    points[:, 0] = np.clip(points[:, 0], US_BOUNDS["lat_min"], US_BOUNDS["lat_max"])
    points[:, 1] = np.clip(points[:, 1], US_BOUNDS["lon_min"], US_BOUNDS["lon_max"])
    return points


def write_json_entry(f, first, key, value):
    if not first:
        f.write(",")
    f.write("\n")
    json.dump(str(key), f)
    f.write(":")
    json.dump(value, f, separators=(",", ":"))
    return False


def generate_synthetic_locations(args):
    suffix = "_ppp" if args.mode in {"poisson-ppp", "poisson-point-process"} else "_random"
    if args.server_output is None:
        args.server_output = f"server{suffix}.json"
    if args.client_close_output is None:
        args.client_close_output = f"client_close{suffix}.json"
    if args.client_far_output is None:
        args.client_far_output = f"client_far{suffix}.json"
    if args.metadata_output is None:
        args.metadata_output = f"loc_metadata{suffix}.txt"

    rng = np.random.default_rng(args.seed)
    requested_server_count = parse_count(args.server_count)
    server_count = min(requested_server_count, MAX_SYNTHETIC_SERVER_COUNT)
    if requested_server_count > MAX_SYNTHETIC_SERVER_COUNT:
        print(
            f"[INFO] Capping Server count from {requested_server_count} "
            f"to {MAX_SYNTHETIC_SERVER_COUNT}"
        )
    if args.mode in {"poisson-ppp", "poisson-point-process"}:
        server_count = int(rng.poisson(server_count))
        if server_count > MAX_SYNTHETIC_SERVER_COUNT:
            print(
                f"[INFO] Capping Poisson Server count from {server_count} "
                f"to {MAX_SYNTHETIC_SERVER_COUNT}"
            )
            server_count = MAX_SYNTHETIC_SERVER_COUNT

    client_close_per_server = args.client_close_per_server
    client_far_count = parse_count(args.client_far_count)

    if server_count < 0:
        raise ValueError("--server-count must be nonnegative")
    if client_close_per_server < 0:
        raise ValueError("--client-close-per-server must be nonnegative")
    if client_far_count < 0:
        raise ValueError("--client-far-count must be nonnegative")
    if args.chunk_size <= 0:
        raise ValueError("--chunk-size must be positive")
    if args.progress_every <= 0:
        raise ValueError("--progress-every must be positive")
    if args.close_radius_deg < 0:
        raise ValueError("--close-radius-deg must be nonnegative")

    print(f"[SCENARIO] Synthetic {args.mode}")
    print(f"[INFO] Server count: {server_count}")
    print(f"[INFO] Client close count: {server_count * client_close_per_server}")
    print(f"[INFO] Client far count: {client_far_count}")
    print(f"[INFO] chunk size: {args.chunk_size}")

    first_server = True
    first_close = True
    first_far = True

    with open(args.server_output, "w") as server_f, \
         open(args.client_close_output, "w") as close_f, \
         open(args.client_far_output, "w") as far_f:
        server_f.write("{")
        close_f.write("{")
        far_f.write("{")

        written = 0
        while written < server_count:
            chunk_count = min(args.chunk_size, server_count - written)
            server_points = random_points_in_bounds(
                rng,
                SERVER_SYNTHETIC_BOUNDS,
                chunk_count,
            )

            for local_idx, p in enumerate(server_points):
                server_idx = written + local_idx
                p_list = [float(p[0]), float(p[1])]
                first_server = write_json_entry(server_f, first_server, server_idx, p_list)

                close_points = clipped_close_points(
                    rng,
                    p,
                    client_close_per_server,
                    args.close_radius_deg,
                )
                close_neighbors = []
                for close_p in close_points:
                    d = float(np.linalg.norm(close_p - p))
                    close_neighbors.append({
                        "distance": d,
                        "pixels": [float(close_p[0]), float(close_p[1])],
                    })
                first_close = write_json_entry(
                    close_f,
                    first_close,
                    server_idx,
                    {"close": close_neighbors},
                )

            written += chunk_count
            if written % args.progress_every == 0 or written == server_count:
                print(f"[INFO] Wrote {written}/{server_count} Server points")

        far_written = 0
        while far_written < client_far_count:
            chunk_count = min(args.chunk_size, client_far_count - far_written)
            far_points = random_points_in_bounds(rng, FAR_RESERVE_BOUNDS, chunk_count)
            for local_idx, p in enumerate(far_points):
                far_idx = far_written + local_idx
                first_far = write_json_entry(
                    far_f,
                    first_far,
                    far_idx,
                    {
                        "train_id": -1,
                        "distance": float(args.far_distance_hint),
                        "pixels": [float(p[0]), float(p[1])],
                    },
                )
            far_written += chunk_count

        server_f.write("\n}\n")
        close_f.write("\n}\n")
        far_f.write("\n}\n")

    with open(args.metadata_output, "w") as f:
        f.write(f"{server_count}\n")

    print("[DONE] Synthetic JSON files generated:")
    print(f" - {args.server_output}")
    print(f" - {args.client_close_output}")
    print(f" - {args.client_far_output}")
    print(f" - {args.metadata_output} (Server N = {server_count})")


def maybe_run_synthetic_mode():
    parser = argparse.ArgumentParser(
        description="Generate location JSON from Gowalla or synthetic US-ish locations."
    )
    parser.add_argument(
        "--mode",
        choices=["gowalla", "uniform-random", "poisson-ppp", "poisson-point-process"],
        default="gowalla",
        help="Synthetic modes stream generated JSON. Default keeps the old Gowalla flow.",
    )
    parser.add_argument("--server-count", default="50m", help="Server count/PPP mean, capped at 50m.")
    parser.add_argument("--client-close-per-server", type=int, default=10)
    parser.add_argument("--client-far-count", default="10000")
    parser.add_argument("--close-radius-deg", type=float, default=0.01)
    parser.add_argument("--far-distance-hint", type=float, default=1.0)
    parser.add_argument("--chunk-size", type=int, default=100_000)
    parser.add_argument("--progress-every", type=int, default=1_000_000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--server-output", default=None)
    parser.add_argument("--client-close-output", default=None)
    parser.add_argument("--client-far-output", default=None)
    parser.add_argument("--metadata-output", default=None)
    args = parser.parse_args()

    if args.mode == "gowalla":
        return

    generate_synthetic_locations(args)
    sys.exit(0)


maybe_run_synthetic_mode()

from scipy.spatial import cKDTree


# ---------------------------
# HELPERS
# ---------------------------

def l2(a, b):
    return np.linalg.norm(a - b)


def min_dist_to_server(point, server_points):
    return np.min(np.linalg.norm(server_points - point, axis=1))


def nearest_server_point(p, server_points):

    dists = np.linalg.norm(server_points - p, axis=1)

    idx = np.argmin(dists)

    return server_points[idx]
# ---------------------------
# LOAD DATA
# ---------------------------
def print_stats(name, lat_diffs, lon_diffs):

    if len(lat_diffs) == 0:

        return

    print(f"\n[STATS] {name}")

    print(f"  LAT diff -> min: {np.min(lat_diffs):.6f}, max: {np.max(lat_diffs):.6f}, mean: {np.mean(lat_diffs):.6f}")

    print(f"  LON diff -> min: {np.min(lon_diffs):.6f}, max: {np.max(lon_diffs):.6f}, mean: {np.mean(lon_diffs):.6f}")

points = []
with open(INPUT_FILE, "r") as f:
    for i, line in enumerate(f):
        #if i >= MAX_POINTS:
        ##    break
        if i%1000000==0:
            print(
                f"[INFO] Processed {i} lines"
            );

        parts = line.strip().split()
        if len(parts) < 5:
            continue

        lat = float(parts[2])
        lon = float(parts[3])

        points.append(np.array([lat, lon], dtype=np.float32))

points = np.array(points, dtype=np.float32)


print(f"[INFO] Loaded points: {len(points)}")


# ---------------------------
# SCENARIO SELECTION
# ---------------------------

if SCENARIO == "easy":
    print("[SCENARIO] Easy (Austin vs Dallas)")

    austin = []
    dallas = []

    for p in points:
        if l2(p, AUS_CENTER) <= CITY_RADIUS:
            austin.append(p)
        elif l2(p, DAL_CENTER) <= CITY_RADIUS:
            dallas.append(p)

    austin = np.array(austin)
    dallas = np.array(dallas)

    print(f"[INFO] Austin points: {len(austin)}")
    print(f"[INFO] Dallas points: {len(dallas)}")

    np.random.shuffle(austin)
    np.random.shuffle(dallas)

    split = int(SERVER_RATIO * len(austin))

    server = austin[:split]

    close_count = int(CLIENT_CLOSE_RATIO * len(austin))
    far_count   = int(CLIENT_FAR_RATIO * len(austin))

    client_close = austin[split: split + close_count]
    client_far   = dallas[:far_count]


elif SCENARIO == "medium":
    print("[SCENARIO] Medium (Downtown vs Suburbs Austin)")

    downtown = []
    suburbs = []

    for p in points:
        d = l2(p, AUS_CENTER)

        if d <= DOWNTOWN_RADIUS:
            downtown.append(p)
        elif SUBURB_MIN <= d <= SUBURB_MAX:
            suburbs.append(p)

    downtown = np.array(downtown)
    suburbs = np.array(suburbs)

    print(f"[INFO] Downtown points: {len(downtown)}")
    print(f"[INFO] Suburb points: {len(suburbs)}")

    idx = np.arange(len(downtown))
    np.random.shuffle(idx)

    split = int(SERVER_RATIO * len(idx))

    server = downtown[idx[:split]]

    close_count = int(CLIENT_CLOSE_RATIO * len(idx))
    far_count   = int(CLIENT_FAR_RATIO * len(idx))

    client_close = downtown[idx[split: split + close_count]]
    client_far   = suburbs[:far_count]


elif SCENARIO == "hard":
    print("[SCENARIO] Hard (Random + distance-based split)")

    idx = np.arange(len(points))
    np.random.shuffle(idx)

    split = int(SERVER_RATIO * len(idx))

    server = points[idx[:split]]
    candidates = points[idx[split:]]

    ref_tree = cKDTree(server)
    dists, nearest_idx = ref_tree.query(candidates, k=1)

    nearest_points = server[nearest_idx]
    coord_diffs = np.abs(candidates - nearest_points)

    close_mask = dists <= CLOSE_L2_THRESHOLD
    far_mask = dists > FAR_L2_THRESHOLD

    client_close = candidates[close_mask]
    client_far = candidates[far_mask]

    lat_diff_close = coord_diffs[close_mask, 0]
    lon_diff_close = coord_diffs[close_mask, 1]

    lat_diff_far = coord_diffs[far_mask, 0]
    lon_diff_far = coord_diffs[far_mask, 1]

    print_stats("Client CLOSE (coord diffs)", lat_diff_close, lon_diff_close)
    print_stats("Client FAR (coord diffs)", lat_diff_far, lon_diff_far)

else:
    raise ValueError("Invalid SCENARIO")


# ---------------------------
# SANITY CHECK
# ---------------------------

print(f"[INFO] Server size: {len(server)}")
print(f"[INFO] Client close size: {len(client_close)}")
print(f"[INFO] Client far size: {len(client_far)}")


# ---------------------------
# BUILD JSON
# ---------------------------

server_json = {}
for i, p in enumerate(server):
    server_json[str(i)] = [float(p[0]), float(p[1])]

server_tree = cKDTree(server)

if len(client_close) > 0:
    client_close_dists, _ = server_tree.query(client_close, k=1)
else:
    client_close_dists = []

client_close_json = {}
for i, (p, d) in enumerate(zip(client_close, client_close_dists)):
    client_close_json[str(i)] = {
        "close": [
            {
                "distance": float(d),
                "pixels": [float(p[0]), float(p[1])]
            }
        ]
    }

if len(client_far) > 0:
    client_far_dists, _ = server_tree.query(client_far, k=1)
else:
    client_far_dists = []

client_far_json = {}
for i, (p, d) in enumerate(zip(client_far, client_far_dists)):
    client_far_json[str(i)] = {
        "train_id": -1,
        "distance": float(d),
        "pixels": [float(p[0]), float(p[1])]
    }


# ---------------------------
# SAVE FILES
# ---------------------------

server_output = "gowalla_server.json"
client_close_output = "gowalla_client_close.json"
client_far_output = "gowalla_client_far.json"

with open(server_output, "w") as f:
    json.dump(server_json, f)

with open(client_close_output, "w") as f:
    json.dump(client_close_json, f)

with open(client_far_output, "w") as f:
    json.dump(client_far_json, f)

with open(METADATA_OUTPUT, "w") as f:
    f.write(f"{len(server)}\n")


print("[DONE] JSON files generated:")
print(f" - {server_output}")
print(f" - {client_close_output}")
print(f" - {client_far_output}")
print(f" - {METADATA_OUTPUT} (Server N = {len(server)})")
