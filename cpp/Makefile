# C++ port of websocket_trading_bot_hybrid.py (+ bot_core.py, price_sources.py,
# amm_sizing.py). Needs a C++20 compiler and OpenSSL 3 headers/libs.
#   make            -> build/hybrid_bot, build/replay, build/unit_tests
#   make test       -> run the C++ unit tests
# OpenSSL is found via pkg-config, else Homebrew's openssl@3, else the system.

CXX      ?= c++
BUILD    := build
# No -ffast-math: py::round_n and the engine's float comparisons rely on exact IEEE.
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wno-unused-parameter -ffp-contract=off -pthread

OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
ifneq ($(shell pkg-config --exists openssl 2>/dev/null && echo yes),)
  SSL_CFLAGS := $(shell pkg-config --cflags openssl)
  SSL_LIBS   := $(shell pkg-config --libs openssl)
else ifneq ($(OPENSSL_PREFIX),)
  SSL_CFLAGS := -I$(OPENSSL_PREFIX)/include
  SSL_LIBS   := -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
else
  SSL_LIBS   := -lssl -lcrypto
endif

CORE_SRCS := src/json.cpp src/yaml.cpp src/log.cpp src/loop.cpp src/net.cpp \
             src/amm_sizing.cpp src/price_sources.cpp src/bot_core.cpp
CORE_OBJS := $(CORE_SRCS:src/%.cpp=$(BUILD)/%.o)
HEADERS   := $(wildcard src/*.hpp)

all: $(BUILD)/hybrid_bot $(BUILD)/replay $(BUILD)/unit_tests

$(BUILD)/%.o: src/%.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SSL_CFLAGS) -c $< -o $@

$(BUILD)/tests_%.o: tests/%.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SSL_CFLAGS) -Isrc -c $< -o $@

$(BUILD)/hybrid_bot: $(BUILD)/websocket_trading_bot_hybrid.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(SSL_LIBS) -o $@

$(BUILD)/replay: $(BUILD)/replay_main.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(SSL_LIBS) -o $@

$(BUILD)/unit_tests: $(BUILD)/tests_unit_tests.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $^ $(SSL_LIBS) -o $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
