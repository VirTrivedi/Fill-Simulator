#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <map>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <variant>
#include "fill_simulator.h"
#include "strategies/strategy.h"

// Include TOML parser
#include "externals/toml11/toml.hpp"

// Include all strategy headers
#include "strategies/basic_strategy.h"
#include "strategies/theo_strategy.h"

// Helper function to check if file exists
bool file_exists(const std::string& filename) {
    struct stat buffer;
    return (stat(filename.c_str(), &buffer) == 0);
}

// Function to load configuration from TOML file
std::map<std::string, std::variant<uint64_t, double, bool>> loadConfigFromToml(const std::string& configFilePath) {
    std::map<std::string, std::variant<uint64_t, double, bool>> config;
    
    // Set default values
    config["strategy_md_latency_ns"] = static_cast<uint64_t>(1000);  // 1µs
    config["exchange_latency_ns"] = static_cast<uint64_t>(10000);  // 10µs
    config["use_queue_simulation"] = false;
    config["place_edge_percent"] = 0.1;
    config["cancel_edge_percent"] = 0.05;
    
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

        // Extract simulation settings
        if (data.contains("simulation")) {
            const auto& simulation = toml::find(data, "simulation");
            
            if (simulation.contains("use_queue_simulation")) {
                config["use_queue_simulation"] = toml::find<bool>(simulation, "use_queue_simulation");
            }
        }

        // Extract strategy parameters
        if (data.contains("strategy")) {
            const auto& strategy = toml::find(data, "strategy");
            
            if (strategy.contains("place_edge_percent")) {
                config["place_edge_percent"] = toml::find<double>(strategy, "place_edge_percent");
            }
            
            if (strategy.contains("cancel_edge_percent")) {
                config["cancel_edge_percent"] = toml::find<double>(strategy, "cancel_edge_percent");
            }
        }

        std::cout << "Loaded configuration from: " << configFilePath << std::endl;
        std::cout << "  Strategy MD Latency: " << std::get<uint64_t>(config["strategy_md_latency_ns"]) / 1000.0 << " µs" << std::endl;
        std::cout << "  Exchange Latency: " << std::get<uint64_t>(config["exchange_latency_ns"]) / 1000.0 << " µs" << std::endl;
        std::cout << "  Total round-trip latency: " 
                  << (std::get<uint64_t>(config["strategy_md_latency_ns"]) + 2 * std::get<uint64_t>(config["exchange_latency_ns"])) / 1000.0 << " µs" << std::endl;
        std::cout << "  Queue Simulation: " << (std::get<bool>(config["use_queue_simulation"]) ? "Enabled" : "Disabled") << std::endl;
        std::cout << "  Place Edge Percent: " << std::get<double>(config["place_edge_percent"]) << "%" << std::endl;
        std::cout << "  Cancel Edge Percent: " << std::get<double>(config["cancel_edge_percent"]) << "%" << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error loading TOML config file: " << e.what() << std::endl;
        std::cerr << "Using default values instead." << std::endl;
    }
    
    return config;
}

// Function to create strategy based on user choice
std::shared_ptr<Strategy> createStrategy(int choice, const std::map<std::string, std::variant<uint64_t, double, bool>>& config) {
    switch (choice) {
        case 1:
            return std::make_shared<BasicStrategy>();
        case 2: {
            // Extract TheoStrategy parameters from config
            double placeEdgePercent = std::get<double>(config.at("place_edge_percent"));
            double cancelEdgePercent = std::get<double>(config.at("cancel_edge_percent"));
            
            // Ensure cancel edge is less than place edge
            if (cancelEdgePercent >= placeEdgePercent) {
                std::cout << "Warning: Cancel edge must be less than place edge. Adjusting cancel edge to 80% of place edge." << std::endl;
                cancelEdgePercent = placeEdgePercent * 0.8;
            }
            
            std::cout << "Creating TheoStrategy with place_edge=" << placeEdgePercent 
                      << "%, cancel_edge=" << cancelEdgePercent << "%" << std::endl;
            
            return std::make_shared<TheoStrategy>(placeEdgePercent, cancelEdgePercent);
        }
        default:
            throw std::runtime_error("Invalid strategy choice");
    }
}

// Helper function to display available strategies
void displayAvailableStrategies() {
    std::cout << "\nAvailable Strategies:\n";
    std::cout << "1. Basic Strategy - Simple strategy that places orders at the top of the book\n";
    std::cout << "2. Theo Strategy - Advanced strategy that calculates theoretical value using a time-weighted EMA of trades and midpoints\n";
}

int main(int argc, char* argv[]) {
    // Load the config file first
    std::string latencyConfigFilePath;
    bool useQueueSimulation = false;
    
    if (argc < 2) {
        std::cerr << "Error: You must provide at least a config file path" << std::endl;
        std::cerr << "Usage: " << argv[0] << " <config_file>" << std::endl;
        return 1;
    }
    
    latencyConfigFilePath = argv[argc-1];
    
    // Load configuration and determine simulation mode
    auto config = loadConfigFromToml(latencyConfigFilePath);
    useQueueSimulation = std::get<bool>(config["use_queue_simulation"]);
    uint64_t strategyMdLatencyNs = std::get<uint64_t>(config["strategy_md_latency_ns"]);
    uint64_t exchangeLatencyNs = std::get<uint64_t>(config["exchange_latency_ns"]);
    
    // Check if the correct number of arguments was provided
    if ((useQueueSimulation && argc != 4) || (!useQueueSimulation && argc != 5)) {
        if (useQueueSimulation) {
            std::cerr << "Usage for queue simulation mode: " << argv[0] 
                     << " <book_events_file> <output_file> <config_file>" << std::endl;
        } else {
            std::cerr << "Usage for tops/fills mode: " << argv[0] 
                     << " <book_tops_file> <book_fills_file> <output_file> <config_file>" << std::endl;
        }
        return 1;
    }
    
    try {
        std::string outputFilePath;
        
        if (useQueueSimulation) {
            std::string bookEventsFilePath = argv[1];
            outputFilePath = argv[2];
            
            // Check if input file exists
            if (!file_exists(bookEventsFilePath)) {
                std::cerr << "Error: Book events file does not exist: " << bookEventsFilePath << std::endl;
                return 1;
            }
            
            // Create fill simulator with queue simulation
            FillSimulator simulator(outputFilePath, strategyMdLatencyNs, exchangeLatencyNs, true);
            
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
            auto strategy = createStrategy(strategyChoice, config);
            
            // Set strategy in simulator
            simulator.setStrategy(strategy);
            
            // Run simulation in queue mode
            std::cout << "\nStarting simulation with '" << strategy->getName() << "' strategy in queue simulation mode..." << std::endl;
            simulator.runQueueSimulation(bookEventsFilePath);

            // Calculate results
            simulator.calculateResults();
            
        } else {
            std::string topsFilePath = argv[1];
            std::string fillsFilePath = argv[2];
            outputFilePath = argv[3];
            
            // Check if input files exist
            if (!file_exists(topsFilePath)) {
                std::cerr << "Error: Book tops file does not exist: " << topsFilePath << std::endl;
                return 1;
            }
            
            if (!file_exists(fillsFilePath)) {
                std::cerr << "Error: Book fills file does not exist: " << fillsFilePath << std::endl;
                return 1;
            }
            
            // Create fill simulator without queue simulation
            FillSimulator simulator(outputFilePath, strategyMdLatencyNs, exchangeLatencyNs, false);
            
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
            auto strategy = createStrategy(strategyChoice, config);
            
            // Set strategy in simulator
            simulator.setStrategy(strategy);
            
            // Run simulation in standard mode
            std::cout << "\nStarting simulation with '" << strategy->getName() << "' strategy..." << std::endl;
            simulator.runSimulation(topsFilePath, fillsFilePath);
            
            // Calculate results
            simulator.calculateResults();
        }
        
        std::cout << "\nSimulation completed successfully." << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}