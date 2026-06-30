# Code Map

This repo is organized around one main experiment binary, `./test`, plus
wrappers that run repeatable sweeps.

## Main C++ Experiment

`test_fuzzypsi.cpp` owns the CLI, top-level validation, and dispatch. The
implementation is intentionally included as one translation unit because much
of the code relies on templates, feature macros, and shared globals.

The split files under `test_fuzzypsi_parts/` are:

- `globals_and_utils.inc.cpp`: runtime flags, paths, argument helpers, timers, and shared utility functions.
- `datasets.inc.cpp`: dataset preset selection and generated-Client dataset modes.
- `grid_search.inc.cpp`: direct Cuckoo L/k/w calibration.
- `oprf_filters.inc.cpp`: OPRF-derived Bloom/Cuckoo insertion and query key logic.
- `pir_readers.inc.cpp`: PIR row readers and adapter-facing helpers.
- `pir_membership.inc.cpp`: private membership queries over Bloom/Cuckoo filters.
- `ca_only.inc.cpp`: OpenFHE cardinality-only Bloom/Cuckoo paths.
- `experiment_runner.inc.cpp`: normal fuzzy PSI run loop, metrics, and reporting.
- `cli_args.inc.cpp`: command-line parsing, flag defaults, and argument validation.

When adding code, prefer putting helper functions near the behavior they serve.
Keep `test_fuzzypsi.cpp` mostly limited to help text and mode dispatch.

## Experiment Wrappers

`run_cuckoo_server_sweep.sh` is the main paper-style runner. It does four jobs:

- builds `./test`;
- selects or calibrates `L/k/w/filter_size`;
- runs Cuckoo + BatchPIR + OPRF measurements;
- writes logs, summary CSVs, and seconds CSVs under `results/`.

`run_optimized_cuckoo_pir_single_oprf_sweep.py` is a Python runner with more
structured parsing and calibration-cache handling. Despite the historical name,
the current default final private run is BatchPIR.

Other `run_*.py` and `run_*.sh` files are older or narrower experiment sweeps.
Before editing them, check whether the output file names under `results/` are
still used by paper plots.

## Dependencies

The large directories are dependency/vendor trees:

- `disco/`: contact-discovery, Punc-PIR, and OPRF code.
- `frodoPIR/`: FrodoPIR implementation and SHA3/RandomShake dependencies.
- `openfhe/`: OpenFHE source/build tree used by CA-only mode.
- `vectorized_batchpir/`: BatchPIR client/server implementation.

Treat these as external code unless the experiment adapter specifically needs a
change.

