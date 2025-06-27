#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <map>
#include <sys/stat.h>
#include "fill_simulator.h"
#include "strategies/strategy.h"

// Include all strategy headers
#include "strategies/basic_strategy.h"

// Helper function to check if file exists
bool file_exists(const std::string& filename) {
    struct stat buffer;
    return (stat(filename.c_str(), &buffer) == 0);
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
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <book_tops_file> <book_fills_file> <output_file>" << std::endl;
        return 1;
    }
    
    std::string topsFilePath = argv[1];
    std::string fillsFilePath = argv[2];
    std::string outputFilePath = argv[3];
    
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
        // Create fill simulator
        FillSimulator simulator(outputFilePath);
        
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