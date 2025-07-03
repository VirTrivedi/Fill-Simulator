#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <map>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include "fill_simulator.h"
#include "strategies/strategy.h"

// Include TOML parser
#include "externals/toml11/toml.hpp"

// Include all strategy headers
#include "strategies/basic_strategy.h"

// Helper function to check if file exists
bool file_exists(const std::string& filename) {
    struct stat buffer;
    return (stat(filename.c_str(), &buffer) == 0);
}

// Function to load configuration from TOML file
std::map<std::string, uint64_t> loadConfigFromToml(const std::string& configFilePath) {
    std::map<std::string, uint64_t> config;
    
    // Set default values
    config["strategy_md_latency_ns"] = 1000;  // 1µs
    config["exchange_latency_ns"] = 10000;  // 10µs
    
    if (!file_exists(configFilePath)) {
        std::cerr << "Warning: Config file not found: " << configFilePath << std::endl;
        std::cerr << "Using default values instead." << std::endl;
        return config;
    }
    
    try {
        // Parse the TOML file
        const auto data = toml::parse(configFilePath);
        
        // Extract latency values
        if (data.contains("latency")) {
            const auto& latency = toml::find(data, "latency");
            
            if (latency.contains("strategy_md_latency_ns")) {
                config["strategy_md_latency_ns"] = toml::find<uint64_t>(latency, "strategy_md_latency_ns");
            }
            
            if (latency.contains("exchange_latency_ns")) {
                config["exchange_latency_ns"] = toml::find<uint64_t>(latency, "exchange_latency_ns");
            }
        }

        std::cout << "Loaded configuration from: " << configFilePath << std::endl;
        std::cout << "  Strategy MD Latency: " << config["strategy_md_latency_ns"] / 1000.0 << " µs" << std::endl;
        std::cout << "  Exchange Latency: " << config["exchange_latency_ns"] / 1000.0 << " µs" << std::endl;
        std::cout << "  Total round-trip latency: " << (config["strategy_md_latency_ns"] + 2 * config["exchange_latency_ns"]) / 1000.0 << " µs" << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error loading TOML config file: " << e.what() << std::endl;
        std::cerr << "Using default values instead." << std::endl;
    }
    
    return config;
}

// Function to create strategy based on user choice
std::shared_ptr<Strategy> createStrategy(int choice) {
    switch (choice) {
        case 1:
            return std::make_shared<BasicStrategy>();
        default:
            throw std::runtime_error("Invalid strategy choice");
    }
}

// Helper function to display available strategies
void displayAvailableStrategies() {
    std::cout << "\nAvailable Strategies:\n";
    std::cout << "1. Basic Strategy - Simple strategy that places orders at the top of the book\n";
}

int main(int argc, char* argv[]) {
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " <book_tops_file> <book_fills_file> <output_file> <latency_config_file>" << std::endl;
        return 1;
    }
    
    std::string topsFilePath = argv[1];
    std::string fillsFilePath = argv[2];
    std::string outputFilePath = argv[3];
    std::string latencyConfigFilePath = argv[4];
    
    // Check if input files exist
    if (!file_exists(topsFilePath)) {
        std::cerr << "Error: Book tops file does not exist: " << topsFilePath << std::endl;
        return 1;
    }
    
    if (!file_exists(fillsFilePath)) {
        std::cerr << "Error: Book fills file does not exist: " << fillsFilePath << std::endl;
        return 1;
    }
    
    try {
        // Load configuration and set latency parameters
        auto config = loadConfigFromToml(latencyConfigFilePath);
        uint64_t strategyMdLatencyNs = config["strategy_md_latency_ns"];
        uint64_t exchangeLatencyNs = config["exchange_latency_ns"];

        // Create fill simulator
        FillSimulator simulator(outputFilePath, strategyMdLatencyNs, exchangeLatencyNs);
        
        // Display available strategies and get user choice
        displayAvailableStrategies();
        
        // Get user input for strategy choice
        int strategyChoice;
        std::cout << "\nEnter the number of the strategy you want to use: ";
        std::cin >> strategyChoice;
        
        // Validate input
        if (std::cin.fail()) {
            std::cin.clear();
            std::cin.ignore(10000, '\n');
            throw std::runtime_error("Invalid input. Please enter a number.");
        }
        
        // Create chosen strategy
        auto strategy = createStrategy(strategyChoice);
        
        // Set strategy in simulator
        simulator.setStrategy(strategy);
        
        // Run simulation
        std::cout << "\nStarting simulation with '" << strategy->getName() << "' strategy..." << std::endl;
        simulator.runSimulation(topsFilePath, fillsFilePath);
        
        // Calculate results
        simulator.calculateResults();
        
        std::cout << "\nSimulation completed successfully." << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}