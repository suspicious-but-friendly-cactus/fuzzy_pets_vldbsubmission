# Experiment Guide

## Direct `./test` Runs

The smallest useful direct Cuckoo run is:

```bash
./test \
  --filter=Cuckoo \
  --L=3 \
  --k=3 \
  --w=0.5 \
  --server_size=100 \
  --client_size=20 \
  --filter_size=512 \
  --num_runs=1
```

The main private-membership shape is:

```bash
./test \
  --filter=Cuckoo \
  --PIR_BatchPIR \
  --OPRF \
  --oprf_mechanism=GCAES \
  --oprf_addr=127.0.0.1:50051 \
  --L=5 \
  --k=5 \
  --w=0.5 \
  --server_size=1000 \
  --client_size=1000 \
  --filter_size=3152 \
  --pir_batchpir_batch_size=200 \
  --num_runs=1
```

For interactive GCAES OPRF, start:

```bash
disco/mobile_psi_cpp/build-linux/droidCrypto/psi/oprf/oprf_server \
  -port 50051 \
  -prf GCAES \
  -loop
```

## `run_cuckoo_server_sweep.sh`

The Cuckoo sweep script wraps the direct command above. It uses environment
variables for almost all configuration so long server runs can be resumed
without editing the script.

Default run:

```bash
./run_cuckoo_server_sweep.sh
```

Calibration only:

```bash
./run_cuckoo_server_sweep.sh calibrate_only
```

Measure paper rows, calibrate misses, and rerun:

```bash
./run_cuckoo_server_sweep.sh run_and_calib
```

Small controlled run:

```bash
SERVER_SIZES="8388608 16777216" \
CLIENT_SIZES="1000" \
CALIBRATION_FILTER_SIZE_PERCENT_INCREASES="33 66 100" \
./run_cuckoo_server_sweep.sh --target-fp 0.03
```

Target table selection:

- With no `--target-fp`, with `--target-fp min_fp`, or with `-target-fp min_fp`, the script loads `datasets/calibrations/calibration_gowalla_min_possible_fp.csv`.
- `--target-fp 0.03` loads `datasets/calibrations/calibration_gowalla_fp_lt_0_03_fn_lt_0_01.csv`.
- Numeric targets select the closest paper bucket: `<=0.015`, `<=0.035`, or `<=0.055`.
- Extra `./test` flags are forwarded by the sweep script, including `--cuckoo_pow2_buckets`.

The archived paper file `results/RESULTS USED IN PAPER/old_min_possible_fp.csv`
used 5 runs per point and BatchPIR batch size 128. For a closer reproduction of
that setup:

```bash
NUM_RUNS=5 PIR_BATCHPIR_BATCH_SIZE=128 PIR_BATCHPIR_FALLBACK_SIZES="128 100 64 32 16 8" \
./run_cuckoo_server_sweep.sh --target-fp=min_fp
```

## Important Environment Variables

- `SERVER_SIZES`: space-separated Server sizes.
- `CLIENT_SIZES`: space-separated Client sizes.
- `NUM_RUNS`: repetitions per `./test` point, default `1`.
- `PARAM_TABLE`: semicolon CSV containing at least `server_size`, `L`, `k`, `w`, `filter_size`.
- `SMALL_DB_PRESET`: preset for Server sizes up to `SMALL_PARAM_MAX_SERVER`.
- `BIG_DB_PRESET`: preset for larger Server sizes.
- `PIR_BATCHPIR_BATCH_SIZE`: default BatchPIR batch size.
- `PIR_BATCHPIR_FALLBACK_SIZES`: fallback batch sizes after known BatchPIR parameter failures.
- `OPRF_MECHANISM`: `GCAES` or `ECNR`.
- `OPRF_ADDR`: `HOST:PORT` for the interactive OPRF server.
- `CALIBRATION_TARGET_FP`, `CALIBRATION_TARGET_FN`: calibration thresholds.
- `RUN_AND_CALIB_MIN_SERVER`: skip smaller rows when resuming `run_and_calib`.

## Client Sweep

MNIST/FashionMNIST client-size sweeps default to the minimum-possible-FP calibration
among rows with `FN<=0.015` and `cuckoo_failed=0`:

```bash
./run_cuckoo_client_sweep.sh db=MNIST
./run_cuckoo_client_sweep.sh db=FashionMNIST
```

The default cache files live in `datasets/calibrations` and are used before
running a fresh calibration.
- `RUN_AND_CALIB_SKIP_PAIRS`: skip specific `server:client` pairs.

## Outputs

Each sweep creates a timestamped directory:

```text
results/cuckoo_server_sweep_<small-db>_to_<big-db>_<timestamp>/
```

Important files:

- `summary_target_<fp>.csv`: main measurement summary for normal mode.
- `seconds_target_<fp>.csv`: compact runtime table.
- `calibration/calibrated_params.csv`: selected calibration rows.
- `calibration/calibration_all.csv`: every calibration attempt.
- `run_and_calib_decisions.csv`: why a row was kept, recalibrated, or grown.
- `summary.csv`: accepted final rows in `run_and_calib` mode.

## Dataset Advice

Use generated-Client presets for large random runs. `--db=random` can require
parsing huge Client JSON files even for small Server sizes.

Good large-run presets:

- `random_generated_client`
- `gowalla_plus_augmented_generated_client`
- `gowalla_plus_distribution_generated_client`

Good smoke/local preset:

- `random_small`
