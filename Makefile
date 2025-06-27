# Makefile for Fill Simulator
CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra -O2 -g -I.

SRC_DIR = .
STRATEGIES_DIR = strategies
TYPES_DIR = types
BUILD_DIR = build
BIN_DIR = bin

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

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(MAIN_OBJ): $(MAIN_SRC) $(DEPS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SIMULATOR_OBJ): $(SIMULATOR_SRC) $(DEPS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(STRATEGIES_DIR)/%.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

run: all
	@echo "Usage: ./$(TARGET) <book_tops_file> <book_fills_file> <output_file>"

.PHONY: all clean run directories