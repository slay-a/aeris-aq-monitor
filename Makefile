# Host build: the portable core, the simulator and the test suite.
# The ESP32-C3 firmware is built separately with ESP-IDF (see firmware/).
#
#   make            build the simulator and tests
#   make test       run the unit and integration tests
#   make sim        acceptance scenario against virtual sensors
#   make faults     same, with NACK and bit-flip injection
#   make dataset    regenerate the training set from the room model
#   make model      train, quantize and regenerate model_params.h
#   make serve      live dashboard at http://localhost:8080
#   make all-checks everything CI runs

# With -std=c11, glibc hides clock_gettime, nanosleep and parts of the socket
# API behind __STRICT_ANSI__, so the simulator fails to build on Linux while
# compiling fine on macOS. Request POSIX.1-2008 explicitly there. macOS wants
# _DARWIN_C_SOURCE instead: defining _POSIX_C_SOURCE on Darwin *hides* BSD
# socket extensions rather than exposing them.
ifeq ($(shell uname -s),Darwin)
POSIX_FLAGS := -D_DARWIN_C_SOURCE
else
POSIX_FLAGS := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
endif

CC      ?= cc
PYTHON  ?= python3
VENV    := ../.venv/bin/python
BUILD   := build

# -MMD -MP makes the compiler emit header dependencies, so regenerating
# model_params.h or vectors_model.h rebuilds everything that includes them.
# Without this a freshly trained model silently tests the previous one.
CFLAGS  := -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wconversion \
           -Wno-sign-conversion -Wpointer-arith -Wstrict-prototypes \
           -Wmissing-prototypes -MMD -MP $(POSIX_FLAGS) \
           -Icore/include -Isim/include \
           $(CFLAGS_EXTRA)
LDFLAGS := -lm -lpthread

CORE_SRC := $(wildcard core/src/*.c)
SIM_SRC  := $(wildcard sim/src/*.c)
TEST_SRC := $(wildcard tests/*.c)

CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/%.o)
SIM_OBJ  := $(SIM_SRC:%.c=$(BUILD)/%.o)
TEST_OBJ := $(TEST_SRC:%.c=$(BUILD)/%.o)
DEPS     := $(CORE_OBJ:.o=.d) $(SIM_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

SIM_BIN  := $(BUILD)/aeris_sim
TEST_BIN := $(BUILD)/aeris_tests

DATASET  := ml/data/features.csv
SESSIONS ?= 60

.PHONY: all test sim faults bench dataset model serve clean all-checks

all: $(SIM_BIN) $(TEST_BIN)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(SIM_BIN): $(CORE_OBJ) $(SIM_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS)

# The test binary links the core plus the simulator's room/bus models, but not
# sim_main.c or http_sim.c (they own main()).
TEST_SIM_OBJ := $(filter-out $(BUILD)/sim/src/sim_main.o $(BUILD)/sim/src/http_sim.o,$(SIM_OBJ))

$(TEST_BIN): $(CORE_OBJ) $(TEST_SIM_OBJ) $(TEST_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $^ -o $@ $(LDFLAGS)

test: $(TEST_BIN)
	./$(TEST_BIN)

sim: $(SIM_BIN)
	./$(SIM_BIN) --run --verbose

faults: $(SIM_BIN)
	./$(SIM_BIN) --faults

bench: $(SIM_BIN)
	./$(SIM_BIN) --bench

# 60x: a minute of room behaviour every second, so the chart fills while
# you watch and a cooking event plays out in about twenty seconds.
serve: $(SIM_BIN)
	./$(SIM_BIN) --serve 8080 --speed 60

$(DATASET): $(SIM_BIN)
	@mkdir -p ml/data
	./$(SIM_BIN) --dataset $(SESSIONS) $(DATASET)

dataset: $(DATASET)

# Training needs numpy. The venv path is resolved at run time so the target
# works both in CI (plain python3) and locally (the project venv).
model: $(DATASET)
	$(PYTHON) ml/train.py --data $(DATASET) \
	  --out core/include/aeris/model_params.h \
	  --vectors tests/vectors_model.h \
	  --report ml/data/report.json
	$(MAKE) --no-print-directory all

all-checks: all test faults
	./$(SIM_BIN) --run
	./$(SIM_BIN) --bench 50000

clean:
	rm -rf $(BUILD)

-include $(DEPS)
