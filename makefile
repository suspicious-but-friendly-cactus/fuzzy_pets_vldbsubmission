CXX ?= g++
UNAME_S ?= $(shell uname -s)
ARCH ?= $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null || echo /opt/homebrew/opt/openssl@3)
OPENSSL_INCLUDE_DIR ?= $(OPENSSL_PREFIX)/include
OPENSSL_LIB_DIR ?= $(OPENSSL_PREFIX)/lib
SYSTEM_INCLUDE_DIRS ?= /opt/homebrew/include

SEAL_PREFIX ?= $(shell brew --prefix seal 2>/dev/null || echo /opt/homebrew/opt/seal)
SEAL_INCLUDE_DIR ?= $(patsubst %/seal/seal.h,%,$(firstword $(wildcard $(SEAL_PREFIX)/include/SEAL-*/seal/seal.h $(SEAL_PREFIX)/include/seal/seal.h)))
SEAL_LIB_DIR ?= $(SEAL_PREFIX)/lib
SEAL_LDLIBS ?= -lseal
else
OPENSSL_PREFIX ?= /usr
OPENSSL_INCLUDE_DIR ?= $(firstword $(wildcard $(OPENSSL_PREFIX)/include /usr/local/include /usr/include))
OPENSSL_LIB_DIR ?= $(firstword $(wildcard $(OPENSSL_PREFIX)/lib/x86_64-linux-gnu $(OPENSSL_PREFIX)/lib /usr/local/lib /usr/lib/x86_64-linux-gnu /usr/lib))
SYSTEM_INCLUDE_DIRS ?= /usr/local/include /usr/include

SEAL_PREFIX ?= /usr/local
SEAL_INCLUDE_DIR ?= $(patsubst %/seal/seal.h,%,$(firstword $(wildcard $(SEAL_PREFIX)/include/SEAL-*/seal/seal.h $(SEAL_PREFIX)/include/seal/seal.h /usr/include/SEAL-*/seal/seal.h /usr/include/seal/seal.h $(HOME)/ours/SEAL/native/src/seal/seal.h $(HOME)/ours/SEAL/build/native/src/seal/seal.h)))
SEAL_LIB_FILE ?= $(firstword $(wildcard $(SEAL_PREFIX)/lib/libseal*.a $(SEAL_PREFIX)/lib/libseal*.so /usr/local/lib/libseal*.a /usr/local/lib/libseal*.so /usr/lib/x86_64-linux-gnu/libseal*.a /usr/lib/x86_64-linux-gnu/libseal*.so /usr/lib/libseal*.a /usr/lib/libseal*.so $(HOME)/ours/SEAL/build/lib/libseal*.a $(HOME)/ours/SEAL/build/lib/libseal*.so))
SEAL_LIB_DIR ?= $(dir $(SEAL_LIB_FILE))
SEAL_LDLIBS ?= $(if $(SEAL_LIB_FILE),$(SEAL_LIB_FILE),-lseal-4.3)
endif

SEAL_CXXFLAGS ?=
USE_BATCHPIR ?= 1
USE_OPENFHE ?= 0
OPENFHE_ROOT ?= openfhe
OPENFHE_BUILD ?= $(OPENFHE_ROOT)/build
SIMD_FLAGS ?=
ifeq ($(ARCH),x86_64)
SIMD_FLAGS += -msse2 -msse -msse4.1 -maes
endif
PIR_DOUBLE_OBJS = disco/contact-discovery/psetggm/pset_ggm.o \
	disco/contact-discovery/psetggm/xor.o \
	disco/contact-discovery/psetggm/answer.o \
	disco/contact-discovery/psetggm/AES.o \
	disco/contact-discovery/oprf_c/disco_gcaes_oprf_adapter.o
TEST_FUZZYPSI_PARTS = $(wildcard test_fuzzypsi_parts/*.inc.cpp)
BATCHPIR_ADAPTER_DEPS = batchpir_row_reader.h \
	vectorized_batchpir/header/batchpirclient.h \
	vectorized_batchpir/header/batchpirserver.h \
	vectorized_batchpir/header/batchpirparams.h \
	vectorized_batchpir/header/client.h \
	vectorized_batchpir/header/server.h \
	vectorized_batchpir/header/pirparams.h \
	vectorized_batchpir/src/batchpirclient.cpp \
	vectorized_batchpir/src/batchpirserver.cpp \
	vectorized_batchpir/src/batchpirparams.cpp \
	vectorized_batchpir/src/client.cpp \
	vectorized_batchpir/src/server.cpp \
	vectorized_batchpir/src/pirparams.cpp \
	vectorized_batchpir/src/utils.h
THREAD_FLAGS = -pthread
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra $(THREAD_FLAGS) $(SIMD_FLAGS) \
	$(addprefix -I,$(SYSTEM_INCLUDE_DIRS)) \
	-IUtils \
	-Idisco/contact-discovery \
	-IfrodoPIR/include \
	-IfrodoPIR/sha3/include \
	-IfrodoPIR/RandomShake/include \
	$(if $(OPENSSL_INCLUDE_DIR),-I$(OPENSSL_INCLUDE_DIR),)
LDFLAGS = $(THREAD_FLAGS) $(if $(OPENSSL_LIB_DIR),-L$(OPENSSL_LIB_DIR),)
LDLIBS = -lssl -lcrypto
ifneq ($(UNAME_S),Darwin)
LDLIBS += -ldl
endif
ifeq ($(USE_BATCHPIR),1)
CXXFLAGS += -DFUZZY_PETS_ENABLE_BATCHPIR=1 \
	-Ivectorized_batchpir \
	-Ivectorized_batchpir/header \
	$(if $(SEAL_PREFIX),-I$(SEAL_PREFIX)/include,) \
	$(if $(SEAL_INCLUDE_DIR),-I$(SEAL_INCLUDE_DIR),) \
	$(SEAL_CXXFLAGS)
LDFLAGS += $(if $(SEAL_LIB_DIR),-L$(SEAL_LIB_DIR),)
LDLIBS += $(SEAL_LDLIBS)
endif
ifeq ($(USE_OPENFHE),1)
CXXFLAGS += -DFUZZY_PETS_ENABLE_OPENFHE=1 \
	-isystem $(OPENFHE_ROOT)/src/pke/include \
	-isystem $(OPENFHE_ROOT)/src/core/include \
	-isystem $(OPENFHE_ROOT)/src/binfhe/include \
	-isystem $(OPENFHE_ROOT)/third-party/cereal/include \
	-isystem $(OPENFHE_BUILD)/src/pke \
	-isystem $(OPENFHE_BUILD)/src/core \
	-isystem $(OPENFHE_BUILD)/src/binfhe \
	-isystem $(OPENFHE_BUILD)
LDFLAGS += -L$(OPENFHE_BUILD)/lib \
	-L$(OPENFHE_BUILD)/src/pke \
	-L$(OPENFHE_BUILD)/src/core \
	-L$(OPENFHE_BUILD)/src/binfhe \
	-Wl,-rpath,$(OPENFHE_BUILD)/lib
LDLIBS += -lOPENFHEpke -lOPENFHEcore
endif

.PHONY: all clean help smoke

all: test

help:
	@printf '%s\n' \
		'Targets:' \
		'  make test                 Build the main ./test experiment binary.' \
		'  make USE_OPENFHE=1 test   Build ./test with CA-only OpenFHE support.' \
		'  make grid                 Build the older standalone grid_search binary.' \
		'  make smoke                Build and run a tiny direct Cuckoo sanity check.' \
		'  make clean                Remove local build outputs owned by this makefile.' \
		'' \
		'Common experiment:' \
		'  ./test --filter=Cuckoo --L=5 --k=5 --w=0.5 --server_size=1000 --client_size=1000 --filter_size=3152' \
		'  ./run_cuckoo_server_sweep.sh --help'

# -------- TEST --------
test: test_fuzzypsi.o batchpir_row_reader.o Utils/utils.o $(PIR_DOUBLE_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

smoke: test
	./test --filter=Cuckoo --L=3 --k=3 --w=0.5 --server_size=100 --client_size=20 --filter_size=512 --num_runs=1

# -------- GRID --------
grid: grid_search.o Utils/utils.o
	$(CXX) $(CXXFLAGS) -o $@ $^

# -------- compile rule --------
batchpir_row_reader.o: batchpir_row_reader.cpp $(BATCHPIR_ADAPTER_DEPS) makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

test_fuzzypsi.o: test_fuzzypsi.cpp $(TEST_FUZZYPSI_PARTS) makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.cpp makefile
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f test grid test_fuzzypsi.o batchpir_row_reader.o grid_search.o Utils/utils.o $(PIR_DOUBLE_OBJS)
