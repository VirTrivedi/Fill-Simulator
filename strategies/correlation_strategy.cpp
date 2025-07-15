#include "correlation_strategy.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <filesystem>

CorrelationStrategy::CorrelationStrategy(const std::string& correlation_csv_path,
                                       double place_edge_percent,
                                       double cancel_edge_percent,
                                       double self_weight,
                                       const std::string& data_path)
    : symbolId_(0),
      symbol_name_(""),
      place_edge_percent_(place_edge_percent),
      cancel_edge_percent_(cancel_edge_percent),
      self_weight_(self_weight),
      data_path_(data_path),
      nextOrderId_(1),
      currentBidOrderId_(0),
      currentAskOrderId_(0),
      currentBidPrice_(0),
      currentAskPrice_(0),
      lastTheoPrice_(0) {
    
    // Load correlation data
    loadCorrelationData(correlation_csv_path);
    
    // Initialize symbol ID to name mapping
    initializeSymbolMapping();
    
    std::cout << "Correlation Strategy initialized with:" << std::endl;
    std::cout << "  - Place edge: " << place_edge_percent_ << "%" << std::endl;
    std::cout << "  - Cancel edge: " << cancel_edge_percent_ << "%" << std::endl;
    std::cout << "  - Self weight: " << self_weight_ << std::endl;
    std::cout << "  - Data path: " << (data_path_.empty() ? "Not specified" : data_path_) << std::endl;
    std::cout << "  - Loaded data for " << correlations_.size() << " symbols" << std::endl;
}

std::string CorrelationStrategy::getName() const {
    return "Correlation Strategy";
}

// Helper function to convert string to lowercase
std::string CorrelationStrategy::lowercase(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                  [](unsigned char c) { return std::tolower(c); });
    return result;
}

void CorrelationStrategy::setSymbolId(uint64_t symbolId) {
    symbolId_ = symbolId;
    
    // Convert symbol ID to name
    auto it = symbol_id_to_name_.find(symbolId);
    if (it != symbol_id_to_name_.end()) {
        symbol_name_ = it->second;
        
        // Get top correlations for this symbol
        auto corr_it = correlations_.find(symbol_name_);
        if (corr_it != correlations_.end()) {
            top_correlations_ = corr_it->second;
            std::cout << "Found " << top_correlations_.size() << " correlated symbols for " << symbol_name_ << std::endl;
            
            // Print top correlations
            for (const auto& corr : top_correlations_) {
                std::cout << "  - " << corr.symbol << ": " << corr.correlation << std::endl;
            }
            
            // Use the data path from constructor
            std::string main_symbol_path;
            
            if (!data_path_.empty()) {
                main_symbol_path = data_path_;
            } else {
                // Ask user for the path as a fallback
                std::cout << "Enter path to the main symbol's data file: ";
                std::cin >> main_symbol_path;
            }
            
            std::cout << "Using data file: " << main_symbol_path << std::endl;
            loadCorrelatedSymbolsData(main_symbol_path);
        } else {
            std::cout << "No correlation data found for symbol " << symbol_name_ << std::endl;
        }
    } else {
        std::cout << "Warning: Unknown symbol ID " << symbolId << std::endl;
        symbol_name_ = "UNKNOWN";
    }
}

void CorrelationStrategy::loadCorrelationData(const std::string& csv_path) {
    std::ifstream file(csv_path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open correlation CSV file: " << csv_path << std::endl;
        exit(1);
    }
    
    // Read header
    std::string header;
    std::getline(file, header);
    
    // Verify header format
    std::stringstream header_ss(header);
    std::string col1, col2, col3;
    std::getline(header_ss, col1, ',');
    std::getline(header_ss, col2, ',');
    std::getline(header_ss, col3, ',');
    
    if (col1 != "symbol1" || col2 != "symbol2" || col3 != "overall_correlation") {
        std::cerr << "Warning: CSV header doesn't match expected format 'symbol1,symbol2,overall_correlation'" << std::endl;
        std::cerr << "Actual header: " << header << std::endl;
        std::cerr << "Attempting to continue with best effort parsing..." << std::endl;
    }
    
    // Read data
    std::string line;
    int line_count = 0;
    while (std::getline(file, line)) {
        line_count++;
        std::stringstream ss(line);
        std::string symbol1, symbol2;
        double correlation;
        
        // Parse line
        if (!std::getline(ss, symbol1, ',') || 
            !std::getline(ss, symbol2, ',') || 
            !(ss >> correlation)) {
            
            std::cerr << "Warning: Could not parse line " << line_count << ": " << line << std::endl;
            continue;
        }
        
        // Store correlation in both directions
        correlations_[symbol1].push_back(CorrelatedSymbol(symbol2, correlation));
        correlations_[symbol2].push_back(CorrelatedSymbol(symbol1, correlation));
    }
    
    // Sort and limit to top N correlations
    for (auto& [symbol, corrs] : correlations_) {
        std::sort(corrs.begin(), corrs.end(), 
                 [](const CorrelatedSymbol& a, const CorrelatedSymbol& b) {
                     return std::abs(a.correlation) > std::abs(b.correlation);
                 });
        
        // Trim to top MAX_CORRELATED_SYMBOLS
        if (corrs.size() > MAX_CORRELATED_SYMBOLS) {
            corrs.resize(MAX_CORRELATED_SYMBOLS);
        }
    }
    
    std::cout << "Loaded correlations for " << correlations_.size() << " symbols from " 
              << line_count << " correlation pairs" << std::endl;
}

void CorrelationStrategy::initializeSymbolMapping() {
    std::string symbol_map_file;
    std::cout << "Enter path to symbol mapping CSV file: ";
    std::cin >> symbol_map_file;
    
    std::ifstream file(symbol_map_file);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open symbol mapping file: " << symbol_map_file << std::endl;
        exit(1);
    }
    
    // Read header
    std::string header;
    std::getline(file, header);
    
    // Determine delimiter based on header content
    char delimiter = ' ';
    if (header.find(',') != std::string::npos) {
        delimiter = ',';
        std::cout << "Detected comma-separated format" << std::endl;
    } else if (header.find('\t') != std::string::npos) {
        delimiter = '\t';
        std::cout << "Detected tab-separated format" << std::endl;
    }
    
    // Verify header format
    std::stringstream header_ss(header);
    std::string col1, col2;
    std::getline(header_ss, col1, delimiter);
    std::getline(header_ss, col2, delimiter);
    
    // Trim whitespace
    col1.erase(0, col1.find_first_not_of(" \t\r\n"));
    col1.erase(col1.find_last_not_of(" \t\r\n") + 1);
    col2.erase(0, col2.find_first_not_of(" \t\r\n"));
    col2.erase(col2.find_last_not_of(" \t\r\n") + 1);
    
    if (col1 != "stock_locate" || col2 != "symbol") {
        std::cerr << "Warning: CSV header doesn't match expected format 'stock_locate,symbol'" << std::endl;
        std::cerr << "Actual header: " << header << std::endl;
        std::cerr << "Attempting to continue with best effort parsing..." << std::endl;
    }
    
    // Read data
    std::string line;
    int loaded_count = 0;
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string locate_str, symbol;
        
        // Parse line using delimiter
        if (!std::getline(ss, locate_str, delimiter) || 
            !std::getline(ss, symbol)) {
            
            std::cerr << "Warning: Could not parse line: " << line << std::endl;
            continue;
        }
        
        // Trim whitespace
        locate_str.erase(0, locate_str.find_first_not_of(" \t\r\n"));
        locate_str.erase(locate_str.find_last_not_of(" \t\r\n") + 1);
        
        symbol.erase(0, symbol.find_first_not_of(" \t\r\n"));
        symbol.erase(symbol.find_last_not_of(" \t\r\n") + 1);
        
        // Convert locate to uint64_t
        uint64_t locate = 0;
        try {
            locate = std::stoull(locate_str);
        } catch (const std::exception& e) {
            std::cerr << "Warning: Could not convert stock_locate to number: " << locate_str << std::endl;
            continue;
        }
        
        // Store the mapping both ways
        symbol_id_to_name_[locate] = symbol;
        symbol_name_to_id_[symbol] = locate;
        loaded_count++;
    }
    
    std::cout << "Loaded " << loaded_count << " symbol mappings from " << symbol_map_file << std::endl;
}

void CorrelationStrategy::loadCorrelatedSymbolsData(const std::string& main_symbol_path) {
    // Determine file format and base path
    using_book_events_ = main_symbol_path.find("book_events") != std::string::npos;
    
    // Extract base path and file pattern
    std::string file_pattern;
    size_t last_slash = main_symbol_path.find_last_of('/');
    if (last_slash != std::string::npos) {
        base_path_ = main_symbol_path.substr(0, last_slash + 1);
        file_pattern = main_symbol_path.substr(last_slash + 1);
    } else {
        base_path_ = "./";
        file_pattern = main_symbol_path;
    }
    
    // Extract exchange and symbol
    size_t first_dot = file_pattern.find('.');
    size_t second_dot = file_pattern.find('.', first_dot + 1);
    
    if (first_dot == std::string::npos || second_dot == std::string::npos) {
        std::cerr << "Error: Could not parse file pattern: " << file_pattern << std::endl;
        return;
    }
    
    std::string exchange = file_pattern.substr(0, first_dot);
    std::string file_type = file_pattern.substr(first_dot + 1, second_dot - first_dot - 1);
    
    std::cout << "Loading data for correlated symbols using pattern: " << exchange 
              << "." << file_type << ".SYMBOL.bin" << std::endl;
    
    // Load data for each correlated symbol
    for (const auto& corr : top_correlations_) {
        SymbolData symbol_data;
        symbol_data.symbol = corr.symbol;
        symbol_data.is_valid = true;
        
        if (using_book_events_) {
            // Construct path for book_events
            std::string events_path = base_path_ + exchange + ".book_events." + corr.symbol + ".bin";
            std::cout << "  Opening " << events_path << " for " << corr.symbol << std::endl;
            
            symbol_data.book_events_file.open(events_path, std::ios::binary);
            if (!symbol_data.book_events_file.is_open()) {
                std::cerr << "    Failed to open book events file for " << corr.symbol << std::endl;
                symbol_data.is_valid = false;
            } else {
                // Skip the header
                book_events_file_hdr_t header;
                symbol_data.book_events_file.read(reinterpret_cast<char*>(&header), sizeof(book_events_file_hdr_t));
                if (!symbol_data.book_events_file) {
                    std::cerr << "    Failed to read header from book events file for " << corr.symbol << std::endl;
                    symbol_data.is_valid = false;
                } else {
                    std::cout << "    Successfully opened book events file for " << corr.symbol 
                              << " (symbol_idx: " << header.symbol_idx << ")" << std::endl;
                }
            }
        } else {
            // Construct paths for book_tops and book_fills
            std::string tops_path = base_path_ + exchange + ".book_tops." + corr.symbol + ".bin";
            std::string fills_path = base_path_ + exchange + ".book_fills." + corr.symbol + ".bin";
            
            std::cout << "  Opening " << tops_path << " for " << corr.symbol << std::endl;
            symbol_data.book_tops_file.open(tops_path, std::ios::binary);
            if (!symbol_data.book_tops_file.is_open()) {
                std::cerr << "    Failed to open book tops file for " << corr.symbol << std::endl;
                symbol_data.is_valid = false;
            } else {
                book_tops_file_hdr_t header;
                symbol_data.book_tops_file.read(reinterpret_cast<char*>(&header), sizeof(book_tops_file_hdr_t));
                if (!symbol_data.book_tops_file) {
                    std::cerr << "    Failed to read header from book tops file for " << corr.symbol << std::endl;
                    symbol_data.is_valid = false;
                } else {
                    std::cout << "    Successfully opened book tops file for " << corr.symbol 
                              << " (symbol_idx: " << header.symbol_idx << ")" << std::endl;
                }
            }
            
            std::cout << "  Opening " << fills_path << " for " << corr.symbol << std::endl;
            symbol_data.book_fills_file.open(fills_path, std::ios::binary);
            if (!symbol_data.book_fills_file.is_open()) {
                std::cerr << "    Failed to open book fills file for " << corr.symbol << std::endl;
                symbol_data.is_valid = false;
            } else {
                book_fills_file_hdr_t header;
                symbol_data.book_fills_file.read(reinterpret_cast<char*>(&header), sizeof(book_fills_file_hdr_t));
                if (!symbol_data.book_fills_file) {
                    std::cerr << "    Failed to read header from book fills file for " << corr.symbol << std::endl;
                    symbol_data.is_valid = false;
                } else {
                    std::cout << "    Successfully opened book fills file for " << corr.symbol 
                              << " (symbol_idx: " << header.symbol_idx << ")" << std::endl;
                }
            }
            
            // Read the first book top to initialize
            if (symbol_data.is_valid) {
                book_top_t bookTop;
                symbol_data.book_tops_file.read(reinterpret_cast<char*>(&bookTop), sizeof(book_top_t));
                if (symbol_data.book_tops_file.gcount() == sizeof(book_top_t)) {
                    symbol_data.last_book_top = bookTop;
                    
                    // Store the initial mid price
                    uint64_t symbol_id = symbol_name_to_id_[corr.symbol];
                    symbol_mid_prices_[symbol_id] = (bookTop.top_level.bid_nanos + bookTop.top_level.ask_nanos) / 2;
                } else {
                    std::cerr << "    Failed to read initial book top for " << corr.symbol << std::endl;
                    symbol_data.is_valid = false;
                }
            }
        }
        
        if (symbol_data.is_valid) {
            correlated_symbols_data_[corr.symbol] = std::move(symbol_data);
        }
    }
    
    std::cout << "Successfully loaded data for " << correlated_symbols_data_.size() << " correlated symbols" << std::endl;
}

// Process market data for correlated symbols up to the current timestamp
void CorrelationStrategy::processCorrelatedSymbolsData(uint64_t current_ts) {
    if (using_book_events_) {
        // Process book events files
        for (auto& [symbol, data] : correlated_symbols_data_) {
            if (!data.is_valid) continue;
            
            // Read book events until we reach current_ts
            book_event_hdr_t eventHeader;
            bool has_update = false;
            bool topChanged = false;
            
            // Maintain a simplified top-of-book for each symbol
            int64_t best_bid = 0;
            int64_t best_ask = INT64_MAX;
            
            while (data.book_events_file.peek() != EOF) {
                // Read event header
                data.book_events_file.read(reinterpret_cast<char*>(&eventHeader), sizeof(book_event_hdr_t));
                
                if (!data.book_events_file) {
                    data.is_valid = false;
                    break;
                }
                
                if (eventHeader.ts > current_ts) {
                    data.book_events_file.seekg(-static_cast<int>(sizeof(book_event_hdr_t)), std::ios::cur);
                    break;
                }
                
                has_update = true;
                
                // Process based on event type
                switch (eventHeader.type) {
                    case book_event_type_e::add_order: {
                        add_order_t addOrder;
                        data.book_events_file.read(reinterpret_cast<char*>(&addOrder), sizeof(add_order_t));
                        
                        if (!data.book_events_file) {
                            data.is_valid = false;
                            break;
                        }
                        
                        // Update best bid/ask
                        if (addOrder.is_bid) {
                            if (addOrder.price > best_bid) {
                                best_bid = addOrder.price;
                                topChanged = true;
                            }
                        } else {
                            if (addOrder.price < best_ask) {
                                best_ask = addOrder.price;
                                topChanged = true;
                            }
                        }
                        break;
                    }
                    
                    case book_event_type_e::delete_order: {
                        delete_order_t deleteOrder;
                        data.book_events_file.read(reinterpret_cast<char*>(&deleteOrder), sizeof(delete_order_t));
                        break;
                    }
                    
                    case book_event_type_e::replace_order: {
                        replace_order_t replaceOrder;
                        data.book_events_file.read(reinterpret_cast<char*>(&replaceOrder), sizeof(replace_order_t));
                        topChanged = true;
                        break;
                    }
                    
                    case book_event_type_e::amend_order: {
                        amend_order_t amendOrder;
                        data.book_events_file.read(reinterpret_cast<char*>(&amendOrder), sizeof(amend_order_t));
                        break;
                    }
                    
                    case book_event_type_e::reduce_order: {
                        reduce_order_t reduceOrder;
                        data.book_events_file.read(reinterpret_cast<char*>(&reduceOrder), sizeof(reduce_order_t));
                        break;
                    }
                    
                    case book_event_type_e::execute_order: {
                        execute_order_t executeOrder;
                        data.book_events_file.read(reinterpret_cast<char*>(&executeOrder), sizeof(execute_order_t));
                        topChanged = true;
                        break;
                    }
                    
                    case book_event_type_e::execute_order_at_price: {
                        execute_order_at_price_t executeOrderAtPrice;
                        data.book_events_file.read(reinterpret_cast<char*>(&executeOrderAtPrice), sizeof(execute_order_at_price_t));
                        topChanged = true;
                        break;
                    }
                    
                    case book_event_type_e::clear_book: {
                        // Clear the book
                        best_bid = 0;
                        best_ask = INT64_MAX;
                        topChanged = true;
                        break;
                    }
                    
                    case book_event_type_e::session_event: {
                        session_event_t sessionEvent;
                        data.book_events_file.read(reinterpret_cast<char*>(&sessionEvent), sizeof(session_event_t));
                        break;
                    }
                    
                    case book_event_type_e::hidden_trade: {
                        hidden_trade_t hiddenTrade;
                        data.book_events_file.read(reinterpret_cast<char*>(&hiddenTrade), sizeof(hidden_trade_t));
                        break;
                    }
                    
                    default:
                        // Skip any unknown event types
                        break;
                }
            }
            
            if (has_update && topChanged && best_bid > 0 && best_ask < INT64_MAX && best_bid < best_ask) {
                // Update the mid price
                uint64_t symbol_id = symbol_name_to_id_[symbol];
                int64_t mid_price = (best_bid + best_ask) / 2;
                symbol_mid_prices_[symbol_id] = mid_price;
                
                // Update the stored book top
                data.last_book_top.ts = eventHeader.ts;
                data.last_book_top.seqno = eventHeader.seq_no;
                data.last_book_top.top_level.bid_nanos = best_bid;
                data.last_book_top.top_level.ask_nanos = best_ask;
            }
        }
        return;
    }
    
    // Process book tops files
    for (auto& [symbol, data] : correlated_symbols_data_) {
        if (!data.is_valid) continue;
        
        // Read book tops until we reach current_ts
        book_top_t bookTop;
        bool has_update = false;
        
        while (data.book_tops_file.peek() != EOF) {
            // Peek at the next book top's timestamp without consuming it
            data.book_tops_file.read(reinterpret_cast<char*>(&bookTop), sizeof(book_top_t));
            
            if (!data.book_tops_file) {
                data.is_valid = false;
                break;
            }
            
            if (bookTop.ts > current_ts) {
                data.book_tops_file.seekg(-static_cast<int>(sizeof(book_top_t)), std::ios::cur);
                break;
            }
            
            data.last_book_top = bookTop;
            has_update = true;
        }
        
        if (has_update) {
            // Update mid price for this symbol
            uint64_t symbol_id = symbol_name_to_id_[symbol];
            
            // Validate book top
            if (data.last_book_top.top_level.bid_nanos > 0 && 
                data.last_book_top.top_level.ask_nanos > 0 &&
                data.last_book_top.top_level.bid_nanos < data.last_book_top.top_level.ask_nanos) {
                
                int64_t mid_price = (data.last_book_top.top_level.bid_nanos + 
                                    data.last_book_top.top_level.ask_nanos) / 2;
                symbol_mid_prices_[symbol_id] = mid_price;
            }
        }
    }
}

std::vector<OrderAction> CorrelationStrategy::onBookTopUpdate(const book_top_t& bookTop) {
    // Skip invalid book tops
    if (bookTop.top_level.bid_nanos <= 0 || bookTop.top_level.ask_nanos <= 0 ||
        bookTop.top_level.bid_nanos >= bookTop.top_level.ask_nanos) {
        return {};
    }
    
    // Process data for correlated symbols up to this timestamp
    processCorrelatedSymbolsData(bookTop.ts);
    
    // Calculate mid price for this symbol
    int64_t mid_price = (bookTop.top_level.bid_nanos + bookTop.top_level.ask_nanos) / 2;
    symbol_mid_prices_[symbolId_] = mid_price;
    
    // Check for stale orders
    std::vector<OrderAction> actions = checkForStaleOrders(bookTop.ts);
    
    // Update orders based on new theoretical price
    std::vector<OrderAction> newActions = updateOrdersForBookTop(bookTop);
    
    // Combine actions
    actions.insert(actions.end(), newActions.begin(), newActions.end());
    return actions;
}

std::vector<OrderAction> CorrelationStrategy::onFill(const book_fill_snapshot_t& fill) {
    // Update the mid price if this is for our current symbol
    if (fill.resting_side_is_bid) {
        int64_t bid = fill.resting_side_price;
        int64_t ask = fill.opposing_side_price;
        if (bid > 0 && ask > 0 && bid < ask) {
            symbol_mid_prices_[symbolId_] = (bid + ask) / 2;
        }
    }
    return {};
}

std::vector<OrderAction> CorrelationStrategy::onOrderFilled(uint64_t orderId, int64_t /* fillPrice */,
                                                         uint32_t /* fillQty */, bool isBid) {
    // Remove filled order from tracking
    removeOrder(orderId);
    
    // Reset order ID tracking if needed
    if (isBid && orderId == currentBidOrderId_) {
        currentBidOrderId_ = 0;
    } else if (!isBid && orderId == currentAskOrderId_) {
        currentAskOrderId_ = 0;
    }
    
    return {};
}

int64_t CorrelationStrategy::calculateTheoreticalPrice(const book_top_t& bookTop) {
    // Start with this symbol's mid price
    int64_t mid_price = (bookTop.top_level.bid_nanos + bookTop.top_level.ask_nanos) / 2;
    uint64_t current_ts = bookTop.ts;

    // Store price in history
    auto& history = symbol_price_history_[symbolId_];
    history.push_back({current_ts, mid_price});

    // Trim old data
    while (!history.empty() && (history.size() > MAX_HISTORY_POINTS || 
        current_ts - history.front().first > MAX_HISTORY_TIME_NS)) {
        history.pop_front();
    }

    // Calculate time-weighted average
    double total_weight = 0;
    double weighted_price_sum = 0;

    for (const auto& [ts, price] : history) {
        double time_weight = 1.0 - std::min(1.0, (current_ts - ts) / static_cast<double>(MAX_HISTORY_TIME_NS));
        weighted_price_sum += price * time_weight;
        total_weight += time_weight;
    }

    // Use time-weighted average as the base price
    if (total_weight > 0) {
        mid_price = static_cast<int64_t>(weighted_price_sum / total_weight);
    }

    for (auto& corr : top_correlations_) {
        // Skip if we don't have a price for this symbol
        uint64_t corr_symbol_id = 0;
        auto it = symbol_name_to_id_.find(corr.symbol);
        if (it != symbol_name_to_id_.end()) {
            corr_symbol_id = it->second;
        }
        
        auto price_it = symbol_mid_prices_.find(corr_symbol_id);
        if (price_it == symbol_mid_prices_.end() || price_it->second <= 0) {
            continue;
        }
        
        // Store the mid price
        corr.last_mid_price = price_it->second;
        
        // Calculate correlation factor and weight
        double corr_factor = getCorrelationFactor(corr.correlation);
        double weight = (1.0 - self_weight_) * corr_factor;
        
        double contribution;
        if (corr.correlation >= 0) {
            // Positive correlation
            contribution = weight * corr.last_mid_price;
        } else {
            // Negative correlation
            contribution = weight * (2 * mid_price - corr.last_mid_price);
        }
        
        weighted_price_sum += contribution;
        total_weight += weight;
    }
    
    // Normalize if we have weights
    if (total_weight > 0) {
        return static_cast<int64_t>(weighted_price_sum / total_weight);
    }
    
    return mid_price;
}

double CorrelationStrategy::getCorrelationFactor(double correlation) {
    // Convert correlation to weight
    return std::abs(correlation);
}

std::vector<OrderAction> CorrelationStrategy::checkForStaleOrders(uint64_t currentTimestamp) {
    std::vector<OrderAction> actions;
    std::vector<uint64_t> orderIdsToRemove;
    
    // Check for active orders
    for (const auto& order : activeOrders_) {
        if (currentTimestamp - order.creationTime >= TEN_MINUTES_NS) {
            // Order is older than 10 minutes, cancel it
            OrderAction cancelAction;
            cancelAction.type = OrderAction::Type::CANCEL;
            cancelAction.orderId = order.orderId;
            cancelAction.symbolId = symbolId_;
            actions.push_back(cancelAction);
            
            // Track which orders to remove
            orderIdsToRemove.push_back(order.orderId);
            
            // Update tracking variables if needed
            if (order.isBid && order.orderId == currentBidOrderId_) {
                currentBidOrderId_ = 0;
            } else if (!order.isBid && order.orderId == currentAskOrderId_) {
                currentAskOrderId_ = 0;
            }
        }
    }

    // Remove canceled orders from active orders list
    for (uint64_t orderId : orderIdsToRemove) {
        removeOrder(orderId);
    }
    
    return actions;
}

std::vector<OrderAction> CorrelationStrategy::updateOrdersForBookTop(const book_top_t& bookTop) {
    std::vector<OrderAction> actions;
    
    // Calculate theoretical fair price based on correlated symbols
    int64_t theoPrice = calculateTheoreticalPrice(bookTop);
    
    // If theo price hasn't changed significantly, don't update orders
    if (std::abs(theoPrice - lastTheoPrice_) < static_cast<int64_t>(theoPrice * 0.0001)) {
        return actions;
    }
    
    lastTheoPrice_ = theoPrice;
    
    // Calculate place and cancel edges using the percentages
    int64_t bidPlaceEdge = static_cast<int64_t>(theoPrice * (1.0 - place_edge_percent_ / 100.0));
    int64_t askPlaceEdge = static_cast<int64_t>(theoPrice * (1.0 + place_edge_percent_ / 100.0));
    
    int64_t bidCancelEdge = static_cast<int64_t>(theoPrice * (1.0 - cancel_edge_percent_ / 100.0));
    int64_t askCancelEdge = static_cast<int64_t>(theoPrice * (1.0 + cancel_edge_percent_ / 100.0));
    
    // Apply minimum tick size (1 cent)
    const int64_t MIN_TICK = 1000;
    bidPlaceEdge = (bidPlaceEdge / MIN_TICK) * MIN_TICK;
    askPlaceEdge = ((askPlaceEdge + MIN_TICK - 1) / MIN_TICK) * MIN_TICK;
    
    // Don't cross the market
    if (bidPlaceEdge >= bookTop.top_level.ask_nanos) {
        bidPlaceEdge = bookTop.top_level.ask_nanos - MIN_TICK;
    }
    if (askPlaceEdge <= bookTop.top_level.bid_nanos) {
        askPlaceEdge = bookTop.top_level.bid_nanos + MIN_TICK;
    }
    
    // Check if we need to cancel existing orders
    if (currentBidOrderId_ > 0 && (currentBidPrice_ > bidCancelEdge || currentBidPrice_ < bookTop.top_level.bid_nanos)) {
        // Verify order exists before canceling
        auto it = std::find_if(activeOrders_.begin(), activeOrders_.end(),
                            [this](const OrderInfo& order) { return order.orderId == currentBidOrderId_; });
        if (it != activeOrders_.end()) {
            OrderAction cancelBid;
            cancelBid.type = OrderAction::Type::CANCEL;
            cancelBid.orderId = currentBidOrderId_;
            cancelBid.symbolId = symbolId_;
            actions.push_back(cancelBid);
            
            removeOrder(currentBidOrderId_);
            currentBidOrderId_ = 0;
        } else {
            // Reset tracking if order doesn't exist
            currentBidOrderId_ = 0;
        }
    }

    if (currentAskOrderId_ > 0 && (currentAskPrice_ < askCancelEdge || currentAskPrice_ > bookTop.top_level.ask_nanos)) {
        // Verify order exists before canceling
        auto it = std::find_if(activeOrders_.begin(), activeOrders_.end(),
                            [this](const OrderInfo& order) { return order.orderId == currentAskOrderId_; });
        if (it != activeOrders_.end()) {
            OrderAction cancelAsk;
            cancelAsk.type = OrderAction::Type::CANCEL;
            cancelAsk.orderId = currentAskOrderId_;
            cancelAsk.symbolId = symbolId_;
            actions.push_back(cancelAsk);
            
            removeOrder(currentAskOrderId_);
            currentAskOrderId_ = 0;
        } else {
            // Reset tracking if order doesn't exist
            currentAskOrderId_ = 0;
        }
    }

    // Place new orders if needed
    if (currentBidOrderId_ == 0 && bidPlaceEdge < bookTop.top_level.ask_nanos) {
        OrderAction newBid;
        newBid.type = OrderAction::Type::ADD;
        newBid.orderId = nextOrderId_++;
        newBid.symbolId = symbolId_;
        newBid.sent_ts = bookTop.ts;
        newBid.md_ts = bookTop.ts;
        newBid.price = bidPlaceEdge;
        newBid.quantity = 1;
        newBid.isBid = true;
        newBid.isPostOnly = true;
        actions.push_back(newBid);
        
        // Update tracking
        currentBidOrderId_ = newBid.orderId;
        currentBidPrice_ = bidPlaceEdge;
        
        // Add to active orders
        OrderInfo bidOrderInfo;
        bidOrderInfo.orderId = newBid.orderId;
        bidOrderInfo.creationTime = bookTop.ts;
        bidOrderInfo.price = bidPlaceEdge;
        bidOrderInfo.quantity = 1;
        bidOrderInfo.isBid = true;
        activeOrders_.push_back(bidOrderInfo);
    }
    
    if (currentAskOrderId_ == 0 && askPlaceEdge > bookTop.top_level.bid_nanos) {
        OrderAction newAsk;
        newAsk.type = OrderAction::Type::ADD;
        newAsk.orderId = nextOrderId_++;
        newAsk.symbolId = symbolId_;
        newAsk.sent_ts = bookTop.ts;
        newAsk.md_ts = bookTop.ts;
        newAsk.price = askPlaceEdge;
        newAsk.quantity = 1;
        newAsk.isBid = false;
        newAsk.isPostOnly = true;
        actions.push_back(newAsk);
        
        // Update tracking
        currentAskOrderId_ = newAsk.orderId;
        currentAskPrice_ = askPlaceEdge;
        
        // Add to active orders
        OrderInfo askOrderInfo;
        askOrderInfo.orderId = newAsk.orderId;
        askOrderInfo.creationTime = bookTop.ts;
        askOrderInfo.price = askPlaceEdge;
        askOrderInfo.quantity = 1;
        askOrderInfo.isBid = false;
        activeOrders_.push_back(askOrderInfo);
    }
    
    return actions;
}

void CorrelationStrategy::removeOrder(uint64_t orderId) {
    // Find and remove the order from active orders
    auto it = std::find_if(activeOrders_.begin(), activeOrders_.end(),
                          [orderId](const OrderInfo& order) { return order.orderId == orderId; });
    
    if (it != activeOrders_.end()) {
        activeOrders_.erase(it);
    }
}