This branch includes our code for the VLDB submission. 

# Fuzzy PETS

Implementation of our Fuzzy PSI protocol using Cuckoo Filters, GCAES, E2LSH and BatchPIR/ChecklistPIR. 
The active experiment is `./test`; the main function is in `test_fuzzypsi.cpp`.

# Installation

## Build the main binary and the SEAL library:
```
git clone https://github.com/suspicious-but-friendly-cactus/fuzzy_pets_vldbsubmission
cd fuzzy_pets_vldbsubmission
python3 -m venv venv  
source venv/bin/activate 
sudo apt update 
pip install -r requirements.txt
sudo apt install nlohmann-json3-dev
sudo apt install git-lfs
sudo apt install default-jdk
git clone https://github.com/microsoft/SEAL.git  
cd SEAL  
cmake -S . -B build
cmake --build build -j$(nproc)
cd ../
make clean
make 
```

## Build the GCAES server:

Linux:
```
cd disco/mobile_psi_cpp
rm -rf build-linux
cmake -S . -B build-linux
cmake --build build-linux --target oprf_server
cd ../..
```

Mac:
```
cd disco/mobile_psi_cpp
rm -rf build-mac
cmake -S . -B build-mac
cmake --build build-mac --target oprf_server
cd ../..
```


Warnings during cmake come from the third party libraries of disco, BatchPIR and SEAL. They do not affect our code. 

## Fetch Dataset Artifacts

Dataset JSONs and zip archives are stored with Git LFS:

```bash
git lfs pull
```

The MNIST and FashionMNIST JSON files are ready after `git lfs pull`.

Unzip the Gowalla dataset before running `--db=gowalla`:

```bash
mkdir -p datasets/gowalla_client
unzip datasets/gowalla_client.zip -d datasets/gowalla_client
unzip datasets/gowalla_server.zip -d datasets
```


## Test Run

See the available flags:

```bash
./test --help
make help
```
Test run without OPRF/PIR:

```
./test --filter=Cuckoo --L=5 --k=5 --w=0.5 --server_size=1000 --client_size=1000 --filter_size=3152
```


## Running the experiments of the paper

First start the OPRF server on terminal 1:

Linux:
```bash
disco/mobile_psi_cpp/build-linux/droidCrypto/psi/oprf/oprf_server \
  -port 50051 \
  -prf GCAES \
  -loop
```

Mac:
```bash
disco/mobile_psi_cpp/build-mac/droidCrypto/psi/oprf/oprf_server \
  -port 50051 \
  -prf GCAES \
  -loop
```

**Do not run multiple experiments on the same server port!**

Then on another terminal:


## Location Experiment:

## Minimum possible error at 1% FN (Figure 5)
```
./run_cuckoo_server_sweep.sh -target-fp min_fp
```
## 1%/3%/5% FP at 1% FN (Figure 6, 7, 8 and Table 2)

```
./run_cuckoo_server_sweep.sh -target-fp 0.01
./run_cuckoo_server_sweep.sh -target-fp 0.03
./run_cuckoo_server_sweep.sh -target-fp 0.05
```

Results are written in results/[exp_name]/summary_.csv

Exp_name follows the format [db] [target_fp] [datetime]

## Scale to 1B

BatchPIR:
|client|= 2^10
```
./test  --filter=Cuckoo  --PIR_BatchPIR --OPRF_mechanism=GCAES --oprf_addr=127.0.0.1:50051 --num_runs=1   --db=default_plus_augmented_generated_client GCAES-OPRF --server_size=268435456 --client_size=1024 --filter_size=268435456  --L=3 --k=20  --w=0.05 --batch_size=200 > results/resultsB_210_batchpir
```

|client| = 2^13
```
./test  --filter=Cuckoo  --PIR_BatchPIR --OPRF_mechanism=GCAES --oprf_addr=127.0.0.1:50051 --num_runs=1   --db=default_plus_augmented_generated_client GCAES-OPRF --server_size=268435456 --client_size=8192 --filter_size=268435456  --L=3 --k=20  --w=0.05 --batch_size=200 > results/resultsB_213_batchpir
```

ChecklistPIR:
|client|= 2^10
```
./test  --filter=Cuckoo  --PIR_double --OPRF_mechanism=GCAES --oprf_addr=127.0.0.1:50051 --num_runs=1   --db=default_plus_augmented_generated_client GCAES-OPRF --server_size=268435456 --client_size=1024 --filter_size=268435456  --L=3 --k=20  --w=0.05 --batch_size=200 > results/resultsB_210_checklistpir
```

|client| = 2^13
```
./test  --filter=Cuckoo  --PIR_double --OPRF_mechanism=GCAES --oprf_addr=127.0.0.1:50051 --num_runs=1   --db=default_plus_augmented_generated_client GCAES-OPRF --server_size=268435456 --client_size=8192 --filter_size=268435456  --L=3 --k=20  --w=0.05 --batch_size=200 > results/resultsB_213_checklistpir
```

Results are written in results/resultsB[client][pir_scheme]

## Machine learning Experiment
```
./run_cuckoo_client_sweep.sh db=MNIST 
./run_cuckoo_client_sweep.sh db=FashionMNIST
```

## ACM-DBLP Experiment

Run ACM-DBLP for `|Client|=100` only:

```bash
./test \
  --filter=Cuckoo \
  --PIR_BatchPIR \
  --OPRF \
  --oprf_mechanism=GCAES \
  --oprf_addr=127.0.0.1:50051 \
  --num_runs=1 \
  --db=acm_dblp \
  --cuckoo_pow2_buckets \
  --L=12 \
  --k=10 \
  --w=3 \
  --server_size=2616 \
  --client_size=100 \
  --filter_size=41760 \
  --pir_batchpir_batch_size=100
```


## Test experiment

```bash
./test \
  --filter=Cuckoo \
  --PIR_BatchPIR \
  --OPRF \
  --oprf_mechanism=GCAES \
  --oprf_addr=127.0.0.1:50051 \
  --num_runs=1 \
  --db=gowalla \
  --L=5 \
  --k=5 \
  --w=0.5 \
  --server_size=1000 \
  --client_size=1000 \
  --filter_size=3152 \
  --pir_batchpir_batch_size=200
```

## Repository Map

- `test_fuzzypsi.cpp`: main CLI and top-level experiment dispatch.
- `test_fuzzypsi_parts/`: focused fragments of the FuzzyPSI protocol, included by `test_fuzzypsi.cpp`.
- `run_cuckoo_server_sweep.sh`: script to run multiple tests for multiple |S|, |C|.
- `run_optimized_cuckoo_pir_single_oprf_sweep.py`: Python calibration/sweep runner.
- `datasets/prepare_db_ML.py`, `datasets/prepare_db_location.py`: dataset preparation.
- `batchpir_row_reader.*`: adapter between experiment rows and BatchPIR.
- `bloom_filter.h`, `cuckoo_filter.h`, `lsh*.h`: core filter and LSH code.
- `disco/`, `frodoPIR/`, `openfhe/`, `vectorized_batchpir/`: third-party dependencies.


## Protocol Implementation

### Overview 
The main file is test_fuzzypsi.cpp. It runs a calibration (calibrate_lkw) if asked (meaning test all possible combinations of E2LSH parameters to get the target FP/FN rate) and than runs the actual protocol fuzzypsi()

The most important file however is test_fuzzypsi_parts/experiment_runner.inc.cpp. This is where our protocol is implemented:

Step 1: Load databases: for Gowalla, the default `--db=gowalla` preset loads the stored protocol split from `datasets/calibrations/gowalla_protocol_server.json`, `datasets/calibrations/gowalla_protocol_client_far.json` (client points far from any server point), and `datasets/calibrations/gowalla_protocol_client_close.json` (client points close to a server point). Sanity checks are available (please uncomment them if you wish).

Step 2: Create filter: initialises the filter, inserts all server values and does the PIR + OPRF setup.

Step 3: Membership query. For the client elements we call batched_pir_memberships_for_reader() which returns the membership status. The membership is appended on the list batched_reported.

Step 4: Membership Evaluation: For each client value we take the ground truth from client_is_close[i]. Then we compare with the reported one and calculate the statistics (FP/FN).

Step 5: Timing: We log and print the runtime of each component and bandwith where applicable. 


### Main Functionalities 

Below is a list of where the main functionalities of our protocol are implemented:

- `lsh_e2lsh.h`
- Cuckoo Filters: cuckoo_filter.h and test_fuzzypsi_parts//oprf_filters.inc.cpp
- PIR: batchpir_row_reader.cpp includes the third-party BatchPIR lirbary (vectorized_batchpir)

### Insertions and Membership check with OPRF

The GCAES server is provided as a separate executable (see the execution instructions above). Therefore, an OPRF server must already be running before executing our scripts.

### Filter Construction

The filter setup begins at:

test_fuzzypsi_parts/oprf_filters.inc.cpp → cuckoo_insert_oprf_parallel_wrapped

Following the call chain eventually leads to:

disco_oprf_eval_blocks

which enters the Disco codebase at:

disco/contact-discovery/oprf_c/disco_gcaes_oprf_adapter.cpp

From this point onward, the execution follows Disco's OPRF implementation. The code traverses the libdroidcrypto library and ultimately invokes the functionality provided by OPRFAESPSIServer.

### Membership Queries

Membership queries follow a similar execution path. However, instead of invoking the server-side functionality, the chain eventually reaches the client-side implementation and calls OPRFAESPSIClient from the Disco library.



## Build Notes

Default build:

```bash
make test
```

OpenFHE build:

```bash
make USE_OPENFHE=1 test
```

The makefile assumes local dependency trees in `disco/`, `frodoPIR/`,
`openfhe/`, and `vectorized_batchpir/`. On macOS it tries Homebrew OpenSSL and
SEAL paths; on Linux it tries common system paths.


## Third-Party Libraries

This project uses the following third-party libraries:

- **BatchPIR**  
  Repository: https://github.com/mhmughees/vectorized_batchpir

- **FrodoPIR**  
  Repository: https://github.com/itzmeanjan/frodoPIR

- **ChecklistPIR**  
  Repository: https://github.com/dimakogan/checklist  

- **Microsoft SEAL**  
  Repository: https://github.com/microsoft/SEAL

- **Disco**  
  Repository: https://github.com/contact-discovery/disco



### Disclaimer 
This repository is research code. Not for usage in production. 
