# Makefile for Fill Simulator
CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -O2 -g -I. -I./externals

SRC_DIR = .
STRATEGIES_DIR = strategies
TYPES_DIR = types
BUILD_DIR = build
BIN_DIR = bin
LATENCIES_DIR = latencies

MAIN_SRC = $(SRC_DIR)/main.cpp
SIMULATOR_SRC = $(SRC_DIR)/fill_simulator.cpp
STRATEGY_SRCS = $(wildcard $(STRATEGIES_DIR)/*.cpp)

MAIN_OBJ = $(BUILD_DIR)/main.o
SIMULATOR_OBJ = $(BUILD_DIR)/fill_simulator.o
STRATEGY_OBJS = $(patsubst $(STRATEGIES_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(STRATEGY_SRCS))

OBJS = $(MAIN_OBJ) $(SIMULATOR_OBJ) $(STRATEGY_OBJS)

DEPS = $(STRATEGIES_DIR)/strategy.h $(SRC_DIR)/fill_simulator.h $(TYPES_DIR)/market_data_types.h $(wildcard $(STRATEGIES_DIR)/*.h)

TARGET = $(BIN_DIR)/fill_simulator

all: directories $(TARGET)

directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)
	@mkdir -p $(LATENCIES_DIR)
	@mkdir -p externals/toml11

toml11:
	@if [ ! -f externals/toml11/toml.hpp ]; then \
		echo "Downloading toml11 library..."; \
		curl -L https://raw.githubusercontent.com/ToruNiina/toml11/main/single_include/toml.hpp \
			-o externals/toml11/toml.hpp; \
	fi

$(TARGET): toml11 $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $@

$(MAIN_OBJ): $(MAIN_SRC) $(DEPS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SIMULATOR_OBJ): $(SIMULATOR_SRC) $(DEPS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(STRATEGIES_DIR)/%.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

distclean: clean
	rm -rf $(LATENCIES_DIR) third_party

run: all
	@echo "Usage: ./$(TARGET) <book_tops_file> <book_fills_file> <output_file> <latency_config_file>"
	@echo "Example: ./$(TARGET) data/tops.dat data/fills.dat output.dat latencies/latency_config.toml"

.PHONY: all clean distclean run directories toml11