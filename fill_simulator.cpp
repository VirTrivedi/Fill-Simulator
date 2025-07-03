#include "fill_simulator.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>

FillSimulator::FillSimulator(const std::string& outputFilePath,
                             uint64_t strategyMdLatencyNs,
                             uint64_t exchangeLatencyNs)
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
      totalSellProceeds_(0),
      strategyMdLatencyNs_(strategyMdLatencyNs),
      exchangeLatencyNs_(exchangeLatencyNs) {
    
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

// Helper methods to apply latency
uint64_t FillSimulator::applyMdLatency(uint64_t timestamp) const {
    return timestamp + strategyMdLatencyNs_;
}

uint64_t FillSimulator::applyExchangeLatency(uint64_t timestamp) const {
    return timestamp + exchangeLatencyNs_;
}

// Process a book top update
void FillSimulator::processBookTop(const book_top_t& bookTop) {
    static uint64_t lastProcessedTime = 0;
    static const uint64_t MIN_PROCESSING_INTERVAL = 100000;
    
    if (lastProcessedTime > 0 && (bookTop.ts - lastProcessedTime) < MIN_PROCESSING_INTERVAL) {
        return;
    }
    
    // Validate the book top
    if (bookTop.top_level.bid_nanos <= 0 || 
        bookTop.top_level.ask_nanos <= 0 || 
        bookTop.top_level.bid_nanos >= bookTop.top_level.ask_nanos ||
        bookTop.top_level.bid_nanos == INT64_MAX ||
        bookTop.top_level.ask_nanos == INT64_MAX) {
        
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
    
    // Create a copy of bookTop with adjusted timestamp
    book_top_t delayedBookTop = bookTop;
    delayedBookTop.ts = applyMdLatency(bookTop.ts);

    latencyStats_.totalMdEvents++;
    latencyStats_.totalMdToStrategyLatencyNs += strategyMdLatencyNs_;

    auto actions = strategy_->onBookTopUpdate(delayedBookTop);
    
    // Process each action
    for (const auto& action : actions) {
        // Apply exchange latency to the action
        uint64_t exchangeReceiveTime = applyExchangeLatency(delayedBookTop.ts);
        OrderAction delayedAction = action;
        
        if (delayedAction.sent_ts == 0) {
            delayedAction.sent_ts = delayedBookTop.ts;
        }
        delayedAction.md_ts = exchangeReceiveTime;
        
        latencyStats_.totalStrategyToExchangeLatencyNs += exchangeLatencyNs_;

        processAction(delayedAction, bookTop);
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
            uint64_t orderId = order.orderId;
            
            auto nextIt = std::next(it);
            
            // Apply additional latency for the fill notification
            uint64_t fillNotificationTime = applyExchangeLatency(order.md_ts);
            
            processFill(orderId, fillPrice, remainingQty, order.isBid, fillNotificationTime);
            
            if (activeOrders_.find(orderId) == activeOrders_.end()) {
                it = nextIt;
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
    // Add MD latency to the fill timestamp
    book_fill_snapshot_t delayedFill = fill;
    delayedFill.ts = applyMdLatency(fill.ts);

    latencyStats_.totalMdEvents++;
    latencyStats_.totalMdToStrategyLatencyNs += strategyMdLatencyNs_;

    auto actions = strategy_->onFill(delayedFill);
    
    // Process any actions returned by the strategy
    for (const auto& action : actions) {
        // Apply exchange latency
        uint64_t exchangeReceiveTime = applyExchangeLatency(delayedFill.ts);
        
        OrderAction delayedAction = action;
        if (delayedAction.sent_ts == 0) {
            delayedAction.sent_ts = delayedFill.ts;
        }
        delayedAction.md_ts = exchangeReceiveTime;
        
        latencyStats_.totalStrategyToExchangeLatencyNs += exchangeLatencyNs_;

        processAction(delayedAction, marketState_.lastBookTop);
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
void FillSimulator::processFill(uint64_t orderId, int64_t fillPrice, uint32_t fillQty, bool isBid, 
                                uint64_t fillNotificationTime) {
    // Check if the order exists
    auto orderIt = activeOrders_.find(orderId);
    if (orderIt == activeOrders_.end()) {
        std::cerr << "Warning: Attempted to fill non-existent order ID " << orderId << std::endl;
        return;
    }

    // Validate fill price to avoid overflow and unrealistic values
    if (fillPrice <= 0 || fillPrice == INT64_MAX || fillQty == 0) {
        std::cout << "Warning: Skipping invalid fill with price: " << fillPrice << std::endl;
        return;
    }
    
    if (fillNotificationTime == 0) {
        fillNotificationTime = applyExchangeLatency(marketState_.lastBookTop.ts);
    }

    if (fillNotificationTime > 0) {
        latencyStats_.totalExchangeToNotificationLatencyNs += exchangeLatencyNs_;
    }
    
    // Copy needed values before potentially erasing the order
    uint64_t symbolId = orderIt->second.symbolId;
    uint32_t totalQuantity = orderIt->second.quantity;
    
    // Update filled quantity
    orderIt->second.filledQuantity += fillQty;
    bool isFullyFilled = orderIt->second.filledQuantity >= totalQuantity;
    
    // Write fill record to file
    OrderRecord record;
    record.timestamp = fillNotificationTime;
    record.event_type = 3;  // Fill order
    record.order_id = orderId;
    record.symbol_id = symbolId;
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
    
    if (isFullyFilled) {
        activeOrders_.erase(orderIt);
    }
    
    book_top_t notificationBookTop = marketState_.lastBookTop;
    notificationBookTop.ts = fillNotificationTime;

    auto actions = strategy_->onOrderFilled(orderId, fillPrice, fillQty, isBid);
    
    // Process any additional actions from the strategy
    for (const auto& action : actions) {
        // Apply exchange latency
        uint64_t exchangeReceiveTime = applyExchangeLatency(fillNotificationTime);
        
        OrderAction delayedAction = action;
        if (delayedAction.sent_ts == 0) {
            delayedAction.sent_ts = fillNotificationTime;
        }
        delayedAction.md_ts = exchangeReceiveTime;
        
        latencyStats_.totalStrategyToExchangeLatencyNs += exchangeLatencyNs_;

        processAction(delayedAction, notificationBookTop);
    }
}

// Process a single order action
void FillSimulator::processAction(const OrderAction& action, const book_top_t& bookTop) {
    if (action.type == OrderAction::Type::ADD || action.type == OrderAction::Type::REPLACE) {
        if (wouldOrderBeFilled(action.orderId, action.isBid, action.price, action.quantity)) {
            latencyStats_.totalExchangeToNotificationLatencyNs += exchangeLatencyNs_;
        }
    }
    
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
            record.timestamp = action.md_ts;
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
                    cancelRecord.timestamp = action.md_ts;
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
                    
                    uint64_t fillNotificationTime = applyExchangeLatency(action.md_ts);

                    processFill(action.orderId, fillPrice, action.quantity, action.isBid, fillNotificationTime);
                }
            }
            break;
        }
        case OrderAction::Type::CANCEL: {
            // Cancel existing order
            auto it = activeOrders_.find(action.orderId);
            if (it != activeOrders_.end()) {
                // Save data for the order record before erasing
                uint64_t symbolId = it->second.symbolId;
                int64_t price = it->second.price;
                uint32_t quantity = it->second.quantity;
                bool isBid = it->second.isBid;
                
                // Now erase the order
                activeOrders_.erase(it);

                // Write cancel record
                OrderRecord record;
                record.timestamp = action.md_ts;
                record.event_type = 2;  // Cancel order
                record.order_id = action.orderId;
                record.symbol_id = symbolId;
                record.price = price;
                record.quantity = quantity;
                record.is_bid = isBid;
                writeOrderRecord(record);
            } else {
                std::cerr << "Warning: Attempted to cancel non-existent order ID " 
                        << action.orderId << std::endl;
            }
            break;
        }
        case OrderAction::Type::REPLACE: {
            // Replace existing order
            auto it = activeOrders_.find(action.orderId);
            if (it != activeOrders_.end()) {
                // Save the original order info for record keeping
                uint64_t symbolId = it->second.symbolId;
                int64_t oldPrice = it->second.price;
                uint32_t oldQuantity = it->second.quantity;
                bool isBid = it->second.isBid;
                
                // Update order properties atomically
                it->second.price = action.price;
                it->second.quantity = action.quantity;
                if (action.sent_ts > 0) {
                    it->second.sent_ts = action.sent_ts;
                }
                if (action.md_ts > 0) {
                    it->second.md_ts = action.md_ts;
                }
                
                // Log record with both old and new values
                OrderRecord modifyRecord;
                modifyRecord.timestamp = action.md_ts;
                modifyRecord.event_type = 4;  // Replace order
                modifyRecord.order_id = action.orderId;
                modifyRecord.symbol_id = symbolId;
                modifyRecord.old_price = oldPrice;
                modifyRecord.price = action.price;
                modifyRecord.old_quantity = oldQuantity;
                modifyRecord.quantity = action.quantity;
                modifyRecord.is_bid = isBid;
                writeOrderRecord(modifyRecord);
                
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
                        postOnlyCancelRecord.timestamp = action.md_ts;
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
                        
                        uint64_t fillNotificationTime = applyExchangeLatency(action.md_ts);
                        
                        processFill(action.orderId, fillPrice, action.quantity, it->second.isBid, fillNotificationTime);
                    }
                }
            }
            break;
        }
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
    
    std::cout << "\n========= LATENCY STATISTICS =========\n";
    // Calculate actual event counts for each type of latency
    uint64_t mdEvents = latencyStats_.totalMdEvents;
    uint64_t strategyToExchangeEvents = totalOrdersPlaced_;
    uint64_t exchangeToNotificationEvents = totalOrdersFilled_;

    if (mdEvents > 0) {
        std::cout << "Total MD Events: " << mdEvents << "\n";
        std::cout << "Average MD-to-Strategy Latency: " 
                << (latencyStats_.totalMdToStrategyLatencyNs / mdEvents) / 1000.0 
                << " μs\n";
    }

    if (strategyToExchangeEvents > 0) {
        std::cout << "Total Order Events: " << strategyToExchangeEvents << "\n";
        std::cout << "Average Strategy-to-Exchange Latency: " 
                << (latencyStats_.totalStrategyToExchangeLatencyNs / std::max(strategyToExchangeEvents, static_cast<uint64_t>(1))) / 1000.0 
                << " μs\n";
    }

    if (exchangeToNotificationEvents > 0) {
        std::cout << "Total Fill Events: " << exchangeToNotificationEvents << "\n";
        std::cout << "Average Exchange-to-Notification Latency: " 
                << (latencyStats_.totalExchangeToNotificationLatencyNs / std::max(exchangeToNotificationEvents, static_cast<uint64_t>(1))) / 1000.0 
                << " μs\n";
    }

    // Only calculate total if we have all three types of events
    if (mdEvents > 0 && strategyToExchangeEvents > 0 && exchangeToNotificationEvents > 0) {
        std::cout << "Average Total Round-Trip Latency: " 
                << (strategyMdLatencyNs_ / 1000.0 + 
                    (strategyToExchangeEvents > 0 ? latencyStats_.totalStrategyToExchangeLatencyNs / strategyToExchangeEvents : 0) / 1000.0 + 
                    (exchangeToNotificationEvents > 0 ? latencyStats_.totalExchangeToNotificationLatencyNs / exchangeToNotificationEvents : 0) / 1000.0)
                << " μs\n";
    }

    std::cout << "Expected Round-Trip Latency: " 
            << (strategyMdLatencyNs_ + 2 * exchangeLatencyNs_) / 1000.0 
            << " μs\n";
    std::cout << "======================================\n";

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