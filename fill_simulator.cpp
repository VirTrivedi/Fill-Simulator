#include "fill_simulator.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>

FillSimulator::FillSimulator(const std::string& outputFilePath)
    : marketState_(),
      strategy_(nullptr),
      position_(0),
      cashFlow_(0),
      outputFilePath_(outputFilePath),
      outputFile_(),
      totalOrdersPlaced_(0),
      totalOrdersFilled_(0),
      totalBuyVolume_(0),
      totalSellVolume_(0),
      totalBuyCost_(0),
      totalSellProceeds_(0) {
    
    marketState_.lastValidMidPrice = 0;
    
    // Open output file
    outputFile_.open(outputFilePath_, std::ios::binary | std::ios::trunc);
    if (!outputFile_.is_open()) {
        throw std::runtime_error("Failed to open output file: " + outputFilePath_);
    }
}

FillSimulator::~FillSimulator() {
    if (outputFile_.is_open()) {
        outputFile_.close();
    }
}

// Set the strategy to use for processing book tops and fills
void FillSimulator::setStrategy(std::shared_ptr<Strategy> strategy) {
    strategy_ = strategy;
}

// Process a book top update
void FillSimulator::processBookTop(const book_top_t& bookTop) {
    static uint64_t lastProcessedTime = 0;
    static const uint64_t MIN_PROCESSING_INTERVAL = 100000;
    
    if (lastProcessedTime > 0 && (bookTop.ts - lastProcessedTime) < MIN_PROCESSING_INTERVAL) {
        return;
    }
    
    // Validate the book top
    static int invalidCount = 0;
    if (bookTop.top_level.bid_nanos <= 0 || 
        bookTop.top_level.ask_nanos <= 0 || 
        bookTop.top_level.bid_nanos >= bookTop.top_level.ask_nanos ||
        bookTop.top_level.bid_nanos == INT64_MAX ||
        bookTop.top_level.ask_nanos == INT64_MAX) {
        
        invalidCount++;
        if (invalidCount <= 10) {
            std::cout << "Warning: Invalid book top detected (bid: " << bookTop.top_level.bid_nanos 
                     << ", ask: " << bookTop.top_level.ask_nanos << ")" << std::endl;
        } else if (invalidCount == 11) {
            std::cout << "Further invalid book top warnings suppressed..." << std::endl;
        }
        return;
    }
    
    lastProcessedTime = bookTop.ts;
    marketState_.lastBookTop = bookTop;
    
    int64_t midPrice = (bookTop.top_level.bid_nanos + bookTop.top_level.ask_nanos) / 2;
    marketState_.lastValidMidPrice = midPrice;
            
    // Update bid levels
    marketState_.bidLevels[bookTop.top_level.bid_nanos] = bookTop.top_level.bid_qty;
    marketState_.bidLevels[bookTop.second_level.bid_nanos] = bookTop.second_level.bid_qty;
    marketState_.bidLevels[bookTop.third_level.bid_nanos] = bookTop.third_level.bid_qty;
    
    // Update ask levels
    marketState_.askLevels[bookTop.top_level.ask_nanos] = bookTop.top_level.ask_qty;
    marketState_.askLevels[bookTop.second_level.ask_nanos] = bookTop.second_level.ask_qty;
    marketState_.askLevels[bookTop.third_level.ask_nanos] = bookTop.third_level.ask_qty;
    
    auto actions = strategy_->onBookTopUpdate(bookTop);
    
    // Process each action
    for (const auto& action : actions) {
        switch (action.type) {
            case OrderAction::Type::ADD: {
                // Add new order
                OrderInfo order;
                order.orderId = action.orderId;
                order.symbolId = action.symbolId;
                order.sent_ts = action.sent_ts;
                order.md_ts = action.md_ts;
                order.price = action.price;
                order.quantity = action.quantity;
                order.filledQuantity = 0;
                order.isBid = action.isBid;
                order.isPostOnly = action.isPostOnly;

                activeOrders_[action.orderId] = order;
                totalOrdersPlaced_++;
                
                // Write the add order record to file
                OrderRecord record;
                record.timestamp = bookTop.ts;
                record.event_type = 1;  // Add order
                record.order_id = action.orderId;
                record.symbol_id = action.symbolId;
                record.price = action.price;
                record.quantity = action.quantity;
                record.is_bid = action.isBid;
                writeOrderRecord(record);

                // Check if the order would be immediately filled
                if (wouldOrderBeFilled(action.orderId, action.isBid, action.price, action.quantity)) {
                    if (action.isPostOnly) {
                        // For post-only orders that would immediately fill, cancel them instead
                        std::cout << "Canceling post-only " << (action.isBid ? "buy" : "sell") 
                                << " order at $" << static_cast<double>(action.price)/1e9 
                                << " that would cross the market" << std::endl;
                        activeOrders_.erase(action.orderId);

                        // Write cancel record for post-only that would cross
                        OrderRecord cancelRecord;
                        cancelRecord.timestamp = bookTop.ts;
                        cancelRecord.event_type = 2;  // Cancel order
                        cancelRecord.order_id = action.orderId;
                        cancelRecord.symbol_id = action.symbolId;
                        cancelRecord.price = action.price;
                        cancelRecord.quantity = action.quantity;
                        cancelRecord.is_bid = action.isBid;
                        writeOrderRecord(cancelRecord);
                    } else {
                        // For normal orders, process the fill as usual
                        int64_t fillPrice;
                        if (action.isBid) {
                            fillPrice = bookTop.top_level.ask_nanos;
                        } else {
                            fillPrice = bookTop.top_level.bid_nanos;
                        }
                        
                        processFill(action.orderId, fillPrice, action.quantity, action.isBid);
                    }
                }
                break;
            }
            case OrderAction::Type::CANCEL: {
                // Cancel existing order
                auto it = activeOrders_.find(action.orderId);
                if (it != activeOrders_.end()) {
                    activeOrders_.erase(it);

                    // Write cancel record
                    OrderRecord record;
                    record.timestamp = bookTop.ts;
                    record.event_type = 2;  // Cancel order
                    record.order_id = action.orderId;
                    record.symbol_id = it->second.symbolId;
                    record.price = it->second.price;
                    record.quantity = it->second.quantity;
                    record.is_bid = it->second.isBid;
                    writeOrderRecord(record);
                }
                break;
            }
            case OrderAction::Type::MODIFY: {
                // Modify existing order
                auto it = activeOrders_.find(action.orderId);
                if (it != activeOrders_.end()) {
                    // First log the cancel of the original order
                    OrderRecord cancelRecord;
                    cancelRecord.timestamp = bookTop.ts;
                    cancelRecord.event_type = 2;  // Cancel order
                    cancelRecord.order_id = action.orderId;
                    cancelRecord.symbol_id = it->second.symbolId;
                    cancelRecord.price = it->second.price;
                    cancelRecord.quantity = it->second.quantity;
                    cancelRecord.is_bid = it->second.isBid;
                    writeOrderRecord(cancelRecord);
                    
                    // Then log the add of the modified order
                    OrderRecord addRecord;
                    addRecord.timestamp = bookTop.ts;
                    addRecord.event_type = 1;  // Add order
                    addRecord.order_id = action.orderId;
                    addRecord.symbol_id = it->second.symbolId;
                    addRecord.price = action.price;
                    addRecord.quantity = action.quantity;
                    addRecord.is_bid = it->second.isBid;
                    writeOrderRecord(addRecord);
                    
                    // Update the order in memory
                    it->second.price = action.price;
                    it->second.quantity = action.quantity;
                    
                    // Check if the modified order would be immediately filled
                    if (wouldOrderBeFilled(action.orderId, it->second.isBid, action.price, action.quantity)) {
                        if (it->second.isPostOnly) {
                            // For post-only orders that would immediately fill, cancel them instead
                            std::cout << "Canceling post-only " << (it->second.isBid ? "buy" : "sell") 
                                    << " order at $" << static_cast<double>(action.price)/1e9 
                                    << " after modification that would cross the market" << std::endl;
                            activeOrders_.erase(action.orderId);

                            // Write cancel record for post-only
                            OrderRecord postOnlyCancelRecord;
                            postOnlyCancelRecord.timestamp = bookTop.ts;
                            postOnlyCancelRecord.event_type = 2;  // Cancel order
                            postOnlyCancelRecord.order_id = action.orderId;
                            postOnlyCancelRecord.symbol_id = it->second.symbolId;
                            postOnlyCancelRecord.price = action.price;
                            postOnlyCancelRecord.quantity = action.quantity;
                            postOnlyCancelRecord.is_bid = it->second.isBid;
                            writeOrderRecord(postOnlyCancelRecord);
                        } else {
                            // For normal orders, process the fill as usual
                            int64_t fillPrice;
                            if (it->second.isBid) {
                                fillPrice = bookTop.top_level.ask_nanos;
                            } else {
                                fillPrice = bookTop.top_level.bid_nanos;
                            }
                            
                            processFill(action.orderId, fillPrice, action.quantity, it->second.isBid);
                        }
                    }
                }
                break;
            }
        }
    }

    // Check if any existing orders would now be filled with the new market prices
    for (auto it = activeOrders_.begin(); it != activeOrders_.end();) {
        OrderInfo& order = it->second;
        
        if (wouldOrderBeFilled(order.orderId, order.isBid, order.price, order.quantity - order.filledQuantity)) {
            int64_t fillPrice;
            if (order.isBid) {
                fillPrice = bookTop.top_level.ask_nanos;
            } else {
                fillPrice = bookTop.top_level.bid_nanos;
            }
            
            uint32_t remainingQty = order.quantity - order.filledQuantity;
            processFill(order.orderId, fillPrice, remainingQty, order.isBid);
            
            auto findIt = activeOrders_.find(order.orderId);
            if (findIt == activeOrders_.end()) {
                it = activeOrders_.erase(it);
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }
}

// Process a book fill event
void FillSimulator::processBookFill(const book_fill_snapshot_t& fill) {
    auto actions = strategy_->onFill(fill);
    if (!actions.empty()) {
        std::cout << "Received " << actions.size() << " actions from fill event, processing skipped" << std::endl;
    }
}

// Check if an order would be filled based on current market state
bool FillSimulator::wouldOrderBeFilled(uint64_t /* orderId */, bool isBid, int64_t price, uint32_t quantity) {
    // Validate inputs
    if (price <= 0 || quantity == 0) {
        return false;
    }

    if (isBid) {
        // Buy order - check if price >= best ask
        int64_t bestAsk = marketState_.lastBookTop.top_level.ask_nanos;
        if (bestAsk <= 0 || bestAsk == INT64_MAX) return false;
        
        if (price >= bestAsk) {
            return true;
        }
    } else {
        // Sell order - check if price <= best bid
        int64_t bestBid = marketState_.lastBookTop.top_level.bid_nanos;
        if (bestBid <= 0 || bestBid == INT64_MAX) return false;
        
        if (price <= bestBid) {
            return true;
        }
    }
    return false;
}

// Process a fill event, updating position and cash flow
void FillSimulator::processFill(uint64_t orderId, int64_t fillPrice, uint32_t fillQty, bool isBid) {
    // Validate fill price to avoid overflow and unrealistic values
    if (fillPrice <= 0 || fillPrice == INT64_MAX || fillQty == 0) {
        std::cout << "WARNING: Skipping invalid fill with price: " << fillPrice << std::endl;
        return;
    }
        
    // Update order
    auto it = activeOrders_.find(orderId);
    if (it == activeOrders_.end()) {
        return;
    }
    
    OrderInfo& order = it->second;
    order.filledQuantity += fillQty;
    
    // Write fill record to file
    OrderRecord record;
    record.timestamp = marketState_.lastBookTop.ts;
    record.event_type = 3;  // Fill order
    record.order_id = orderId;
    record.symbol_id = order.symbolId;
    record.price = fillPrice;
    record.quantity = fillQty;
    record.is_bid = isBid;
    writeOrderRecord(record);

    // Update position and cash
    if (isBid) {
        // Buy order filled
        position_ += fillQty;
        int64_t cost = (fillPrice * static_cast<int64_t>(fillQty)) / 1000;
        cashFlow_ -= cost * 1000;
        
        totalBuyVolume_ += fillQty;
        totalBuyCost_ += static_cast<double>(fillPrice) * fillQty / 1e9;
    } else {
        // Sell order filled
        position_ -= fillQty;
        int64_t proceeds = (fillPrice * static_cast<int64_t>(fillQty)) / 1000;
        cashFlow_ += proceeds * 1000;
        
        totalSellVolume_ += fillQty;
        totalSellProceeds_ += static_cast<double>(fillPrice) * fillQty / 1e9;
    }
    
    totalOrdersFilled_++;
    
    // Notify strategy of the fill
    auto actions = strategy_->onOrderFilled(orderId, fillPrice, fillQty, isBid);
    
    // Remove fully filled orders
    if (order.filledQuantity >= order.quantity) {
        activeOrders_.erase(it);
    }
}

// Run the simulation with data from the specified files
void FillSimulator::runSimulation(const std::string& topsFilePath, const std::string& fillsFilePath) {
    // Open files
    std::ifstream topsFile(topsFilePath, std::ios::binary);
    std::ifstream fillsFile(fillsFilePath, std::ios::binary);
    
    if (!topsFile.is_open() || !fillsFile.is_open()) {
        throw std::runtime_error("Failed to open input files");
    }
    
    // Read headers
    book_tops_file_hdr_t topsHeader;
    book_fills_file_hdr_t fillsHeader;
    
    topsFile.read(reinterpret_cast<char*>(&topsHeader), sizeof(book_tops_file_hdr_t));
    fillsFile.read(reinterpret_cast<char*>(&fillsHeader), sizeof(book_fills_file_hdr_t));
    
    // Set symbol ID in strategy
    strategy_->setSymbolId(topsHeader.symbol_idx);
    
    // Read first records
    book_top_t bookTop;
    book_fill_snapshot_t bookFill;
    
    topsFile.read(reinterpret_cast<char*>(&bookTop), sizeof(book_top_t));
    bool hasMoreTops = topsFile.gcount() == sizeof(book_top_t);
    fillsFile.read(reinterpret_cast<char*>(&bookFill), sizeof(book_fill_snapshot_t));
    bool hasMoreFills = fillsFile.gcount() == sizeof(book_fill_snapshot_t);
    
    // Process events in order
    uint64_t processedTops = 0;
    uint64_t processedFills = 0;
    
    while (hasMoreTops || hasMoreFills) {
        if (!hasMoreFills || (hasMoreTops && bookTop.ts <= bookFill.ts)) {
            // Process book top
            processBookTop(bookTop);
            processedTops++;
            
            // Read next book top
            topsFile.read(reinterpret_cast<char*>(&bookTop), sizeof(book_top_t));
            hasMoreTops = topsFile.gcount() == sizeof(book_top_t);
        } else {
            // Process book fill
            processBookFill(bookFill);
            processedFills++;
            
            // Read next book fill
            fillsFile.read(reinterpret_cast<char*>(&bookFill), sizeof(book_fill_snapshot_t));
            hasMoreFills = fillsFile.gcount() == sizeof(book_fill_snapshot_t);
        }
        
        // Print progress
        if ((processedTops + processedFills) % 100000 == 0) {
            std::cout << "Processed " << processedTops << " tops and " 
                      << processedFills << " fills..." << std::endl;
            std::cout << "Current fills: " << totalOrdersFilled_ << " of " 
                      << totalOrdersPlaced_ << " orders" << std::endl;
            
            // Print current position and P&L
            int64_t midPrice = (marketState_.lastBookTop.top_level.bid_nanos + 
                               marketState_.lastBookTop.top_level.ask_nanos) / 2;
            
            int64_t positionValue = position_ * midPrice;
            std::cout << "Current position: " << position_ << " shares, value: $" 
                      << static_cast<double>(positionValue) / 1e9 << std::endl;
        }
    }
    
    std::cout << "Simulation complete. Processed " << processedTops << " tops and " 
              << processedFills << " fills." << std::endl;
              
    // Close files
    topsFile.close();
    fillsFile.close();
}

// Write an order record to the output file
void FillSimulator::writeOrderRecord(const OrderRecord& record) {
    if (outputFile_.is_open()) {
        outputFile_.write(reinterpret_cast<const char*>(&record), sizeof(OrderRecord));
    }
}

// Calculate final P&L and statistics based on the simulation results
void FillSimulator::calculateResults() {
    // Get final mid price
    int64_t finalMidPrice = marketState_.lastValidMidPrice;

    // Calculate position value using validated mid price
    int64_t closingValue = position_ * finalMidPrice;
    
    // Calculate P&L (cash flow + position value)
    double totalPnL = static_cast<double>(cashFlow_) / 1e9 + 
                     static_cast<double>(position_ * finalMidPrice) / 1e9;
    
    std::cout << "\n========= SIMULATION RESULTS =========\n";
    std::cout << "Strategy: " << strategy_->getName() << std::endl;
    std::cout << "Total Orders Placed: " << totalOrdersPlaced_ << std::endl;
    std::cout << "Total Orders Filled: " << totalOrdersFilled_ << std::endl;
    std::cout << "Fill Rate: " << (totalOrdersPlaced_ > 0 ? 
                                 100.0 * totalOrdersFilled_ / totalOrdersPlaced_ : 0) << "%" << std::endl;
    std::cout << "Total Buy Volume: " << totalBuyVolume_ << " shares for $" << totalBuyCost_ << std::endl;
    std::cout << "Total Sell Volume: " << totalSellVolume_ << " shares for $" << totalSellProceeds_ << std::endl;
    std::cout << "Final Position: " << position_ << " shares" << std::endl;
    std::cout << "Final Mid Price: $" << static_cast<double>(finalMidPrice) / 1e9 << std::endl;
    
    if (position_ != 0) {
        std::cout << "Closing Value: $" << static_cast<double>(closingValue) / 1e9 << std::endl;
    }
    
    std::cout << "Final P&L: $" << totalPnL << std::endl;
    
    if (totalPnL > 0) {
        std::cout << "Trading result: PROFIT" << std::endl;
    } else if (totalPnL < 0) {
        std::cout << "Trading result: LOSS" << std::endl;
    } else {
        std::cout << "Trading result: BREAKEVEN" << std::endl;
    }
    
    // Calculate additional statistics
    if (totalBuyVolume_ > 0 && totalSellVolume_ > 0) {
        double avgBuyPrice = totalBuyCost_ / totalBuyVolume_;
        double avgSellPrice = totalSellProceeds_ / totalSellVolume_;
        std::cout << "Average Buy Price: $" << avgBuyPrice << std::endl;
        std::cout << "Average Sell Price: $" << avgSellPrice << std::endl;
        std::cout << "Average Spread Captured: $" << (avgSellPrice - avgBuyPrice) << std::endl;
    }
    
    std::cout << "======================================\n";
}