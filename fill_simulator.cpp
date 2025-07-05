#include "fill_simulator.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>

FillSimulator::FillSimulator(const std::string& outputFilePath,
                             uint64_t strategyMdLatencyNs,
                             uint64_t exchangeLatencyNs,
                             bool useQueueSimulation)
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
      exchangeLatencyNs_(exchangeLatencyNs),
      useQueueSimulation_(useQueueSimulation) {
    
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
    
    const int64_t MAX_REASONABLE_PRICE = 10000LL * 1000000000LL; // $10,000 in nanos

    // Validate the book top
    if (bookTop.top_level.bid_nanos <= 0 || 
        bookTop.top_level.ask_nanos <= 0 || 
        bookTop.top_level.bid_nanos >= bookTop.top_level.ask_nanos ||
        bookTop.top_level.bid_nanos > MAX_REASONABLE_PRICE ||
        bookTop.top_level.ask_nanos > MAX_REASONABLE_PRICE) {
        
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
        int64_t cost = fillPrice * static_cast<int64_t>(fillQty);
        cashFlow_ -= cost;
        
        totalBuyVolume_ += fillQty;
        totalBuyCost_ += static_cast<double>(cost) / 1e9;
    } else {
        // Sell order filled
        position_ -= fillQty;
        int64_t proceeds = fillPrice * static_cast<int64_t>(fillQty);
        cashFlow_ += proceeds;
        
        totalSellVolume_ += fillQty;
        totalSellProceeds_ += static_cast<double>(proceeds) / 1e9;
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

void FillSimulator::runQueueSimulation(const std::string& bookEventsFilePath) {
    // Open the book events file
    std::ifstream bookEventsFile(bookEventsFilePath, std::ios::binary);
    if (!bookEventsFile.is_open()) {
        throw std::runtime_error("Failed to open book events file: " + bookEventsFilePath);
    }
    
    // Read the header
    book_events_file_hdr_t header;
    bookEventsFile.read(reinterpret_cast<char*>(&header), sizeof(book_events_file_hdr_t));
    
    // Set symbol ID in strategy
    strategy_->setSymbolId(header.symbol_idx);
    
    // Initialize order book data structures
    book_side_t bid_book;
    book_side_t ask_book;
    
    // Map to quickly find orders
    std::unordered_map<uint64_t, order_ref_t> order_map;
    
    // Process book events
    book_event_hdr_t eventHeader;
    uint64_t processedEvents = 0;
    
    // Variables to track the current best bid/ask
    book_top_t currentTop;
    currentTop.ts = 0;
    currentTop.seqno = 0;
    currentTop.top_level.bid_nanos = 0;
    currentTop.top_level.ask_nanos = INT64_MAX;
    currentTop.top_level.bid_qty = 0;
    currentTop.top_level.ask_qty = 0;
    
    auto updateTopLevels = [&]() {
        // Update best bid
        if (!bid_book.empty()) {
            auto bestBidIt = bid_book.rbegin();
            currentTop.top_level.bid_nanos = bestBidIt->first;
            currentTop.top_level.bid_qty = bestBidIt->second.first;
            
            // Try to populate second and third levels if they exist
            auto secondBidIt = std::next(bestBidIt);
            if (secondBidIt != bid_book.rend()) {
                currentTop.second_level.bid_nanos = secondBidIt->first;
                currentTop.second_level.bid_qty = secondBidIt->second.first;
                
                auto thirdBidIt = std::next(secondBidIt);
                if (thirdBidIt != bid_book.rend()) {
                    currentTop.third_level.bid_nanos = thirdBidIt->first;
                    currentTop.third_level.bid_qty = thirdBidIt->second.first;
                } else {
                    currentTop.third_level.bid_nanos = 0;
                    currentTop.third_level.bid_qty = 0;
                }
            } else {
                currentTop.second_level.bid_nanos = 0;
                currentTop.second_level.bid_qty = 0;
                currentTop.third_level.bid_nanos = 0;
                currentTop.third_level.bid_qty = 0;
            }
        } else {
            currentTop.top_level.bid_nanos = 0;
            currentTop.top_level.bid_qty = 0;
            currentTop.second_level.bid_nanos = 0;
            currentTop.second_level.bid_qty = 0;
            currentTop.third_level.bid_nanos = 0;
            currentTop.third_level.bid_qty = 0;
        }
        
        // Update best ask
        if (!ask_book.empty()) {
            auto bestAskIt = ask_book.begin();
            currentTop.top_level.ask_nanos = bestAskIt->first;
            currentTop.top_level.ask_qty = bestAskIt->second.first;
            
            // Try to populate second and third levels if they exist
            auto secondAskIt = std::next(bestAskIt);
            if (secondAskIt != ask_book.end()) {
                currentTop.second_level.ask_nanos = secondAskIt->first;
                currentTop.second_level.ask_qty = secondAskIt->second.first;
                
                auto thirdAskIt = std::next(secondAskIt);
                if (thirdAskIt != ask_book.end()) {
                    currentTop.third_level.ask_nanos = thirdAskIt->first;
                    currentTop.third_level.ask_qty = thirdAskIt->second.first;
                } else {
                    currentTop.third_level.ask_nanos = INT64_MAX;
                    currentTop.third_level.ask_qty = 0;
                }
            } else {
                currentTop.second_level.ask_nanos = INT64_MAX;
                currentTop.second_level.ask_qty = 0;
                currentTop.third_level.ask_nanos = INT64_MAX;
                currentTop.third_level.ask_qty = 0;
            }
        } else {
            currentTop.top_level.ask_nanos = INT64_MAX;
            currentTop.top_level.ask_qty = 0;
            currentTop.second_level.ask_nanos = INT64_MAX;
            currentTop.second_level.ask_qty = 0;
            currentTop.third_level.ask_nanos = INT64_MAX;
            currentTop.third_level.ask_qty = 0;
        }

        const int64_t MAX_REASONABLE_PRICE = 10000LL * 1000000000LL; // $10,000 in nanos
    
        // Validate bid prices
        if (currentTop.top_level.bid_nanos > MAX_REASONABLE_PRICE) {
            currentTop.top_level.bid_nanos = 0;
            currentTop.top_level.bid_qty = 0;
        }
        
        // Validate ask prices
        if (currentTop.top_level.ask_nanos > MAX_REASONABLE_PRICE && 
            currentTop.top_level.ask_nanos != INT64_MAX) {
            currentTop.top_level.ask_nanos = INT64_MAX;
            currentTop.top_level.ask_qty = 0;
        }
    };

    std::cout << "Starting queue simulation, processing book events from " << bookEventsFilePath << std::endl;
    
    while (bookEventsFile.read(reinterpret_cast<char*>(&eventHeader), sizeof(book_event_hdr_t))) {
        // Update timestamp in the current top
        currentTop.ts = eventHeader.ts;
        currentTop.seqno = eventHeader.seq_no;
        
        bool topChanged = false;
        
        switch (eventHeader.type) {
            case book_event_type_e::add_order: {
                add_order_t addOrder;
                bookEventsFile.read(reinterpret_cast<char*>(&addOrder), sizeof(add_order_t));
                
                // Add order to appropriate book side
                book_side_t& book = addOrder.is_bid ? bid_book : ask_book;
                price_t price = addOrder.price;
                
                // Create new level if it doesn't exist
                if (book.find(price) == book.end()) {
                    book[price] = std::make_pair(0, order_queue_t());
                }
                
                // Add order to queue and update total quantity
                auto& level = book[price];
                level.first += addOrder.qty;
                level.second.push_back({addOrder.order_id, addOrder.qty, eventHeader.ts});
                
                // Store reference to the order
                order_ref_t ref = {
                    price,
                    addOrder.is_bid,
                    std::prev(level.second.end())
                };
                order_map[addOrder.order_id] = ref;
                
                // Check if top of book changed
                if (addOrder.is_bid && (bid_book.empty() || price >= bid_book.rbegin()->first)) {
                    topChanged = true;
                } else if (!addOrder.is_bid && (ask_book.empty() || price <= ask_book.begin()->first)) {
                    topChanged = true;
                }
                break;
            }
            
            case book_event_type_e::delete_order: {
                delete_order_t deleteOrder;
                bookEventsFile.read(reinterpret_cast<char*>(&deleteOrder), sizeof(delete_order_t));
                
                auto orderIt = order_map.find(deleteOrder.order_id);
                if (orderIt != order_map.end()) {
                    const auto& ref = orderIt->second;
                    book_side_t& book = ref.is_bid ? bid_book : ask_book;
                    auto levelIt = book.find(ref.price);
                    
                    if (levelIt != book.end()) {
                        // Update the quantity at this price level
                        levelIt->second.first -= ref.order_it->qty;
                        
                        // Check if we need to update top of book
                        if ((ref.is_bid && ref.price == currentTop.top_level.bid_nanos) ||
                            (!ref.is_bid && ref.price == currentTop.top_level.ask_nanos)) {
                            topChanged = true;
                        }
                        
                        // Remove the order from the queue
                        levelIt->second.second.erase(ref.order_it);
                        
                        // If level is now empty, remove it
                        if (levelIt->second.first == 0) {
                            book.erase(levelIt);
                        }
                    }
                    
                    // Remove from order map
                    order_map.erase(orderIt);
                }
                break;
            }
            
            case book_event_type_e::replace_order: {
                replace_order_t replaceOrder;
                bookEventsFile.read(reinterpret_cast<char*>(&replaceOrder), sizeof(replace_order_t));
                
                // First, delete the original order
                auto orderIt = order_map.find(replaceOrder.orig_order_id);
                if (orderIt != order_map.end()) {
                    const auto& ref = orderIt->second;
                    book_side_t& book = ref.is_bid ? bid_book : ask_book;
                    auto levelIt = book.find(ref.price);
                    
                    if (levelIt != book.end()) {
                        // Update the quantity at this price level
                        levelIt->second.first -= ref.order_it->qty;
                        
                        // Check if we need to update top of book
                        if ((ref.is_bid && ref.price == currentTop.top_level.bid_nanos) ||
                            (!ref.is_bid && ref.price == currentTop.top_level.ask_nanos)) {
                            topChanged = true;
                        }
                        
                        // Remove the order from the queue
                        levelIt->second.second.erase(ref.order_it);
                        
                        // If level is now empty, remove it
                        if (levelIt->second.first == 0) {
                            book.erase(levelIt);
                        }
                    }
                    
                    // Remove from order map
                    order_map.erase(orderIt);
                }
                
                // Add the new order
                bool isBid = (orderIt != order_map.end()) ? orderIt->second.is_bid : 
                             (replaceOrder.price > 0);
                
                book_side_t& book = isBid ? bid_book : ask_book;
                price_t price = replaceOrder.price;
                
                // Create new level if it doesn't exist
                if (book.find(price) == book.end()) {
                    book[price] = std::make_pair(0, order_queue_t());
                }
                
                // Add order to queue and update total quantity
                auto& level = book[price];
                level.first += replaceOrder.qty;
                level.second.push_back({replaceOrder.new_order_id, replaceOrder.qty, eventHeader.ts});
                
                // Store reference to the order
                order_ref_t ref = {
                    price,
                    isBid,
                    std::prev(level.second.end())
                };
                order_map[replaceOrder.new_order_id] = ref;
                
                // Check if top of book changed
                if (isBid && (bid_book.empty() || price >= bid_book.rbegin()->first)) {
                    topChanged = true;
                } else if (!isBid && (ask_book.empty() || price <= ask_book.begin()->first)) {
                    topChanged = true;
                }
                break;
            }
            
            case book_event_type_e::amend_order: {
                amend_order_t amendOrder;
                bookEventsFile.read(reinterpret_cast<char*>(&amendOrder), sizeof(amend_order_t));
                
                auto orderIt = order_map.find(amendOrder.order_id);
                if (orderIt != order_map.end()) {
                    const auto& ref = orderIt->second;
                    book_side_t& book = ref.is_bid ? bid_book : ask_book;
                    auto levelIt = book.find(ref.price);
                    
                    if (levelIt != book.end()) {
                        // Calculate the delta in qty
                        uint32_t oldQty = ref.order_it->qty;
                        uint32_t qtyDelta = amendOrder.new_qty - oldQty;
                        
                        // Update the order quantity
                        ref.order_it->qty = amendOrder.new_qty;
                        ref.order_it->timestamp = eventHeader.ts;
                        
                        // Update the level quantity
                        levelIt->second.first += qtyDelta;
                        
                        // Check if this affects top of book
                        if ((ref.is_bid && ref.price == currentTop.top_level.bid_nanos) ||
                            (!ref.is_bid && ref.price == currentTop.top_level.ask_nanos)) {
                            topChanged = true;
                        }
                    }
                }
                break;
            }
            
            case book_event_type_e::reduce_order: {
                reduce_order_t reduceOrder;
                bookEventsFile.read(reinterpret_cast<char*>(&reduceOrder), sizeof(reduce_order_t));
                
                auto orderIt = order_map.find(reduceOrder.order_id);
                if (orderIt != order_map.end()) {
                    const auto& ref = orderIt->second;
                    book_side_t& book = ref.is_bid ? bid_book : ask_book;
                    auto levelIt = book.find(ref.price);
                    
                    if (levelIt != book.end()) {
                        // Update the order quantity
                        ref.order_it->qty -= reduceOrder.cxled_qty;
                        ref.order_it->timestamp = eventHeader.ts;
                        
                        // Update the level quantity
                        levelIt->second.first -= reduceOrder.cxled_qty;
                        
                        // If order is fully canceled, remove it
                        if (ref.order_it->qty == 0) {
                            levelIt->second.second.erase(ref.order_it);
                            order_map.erase(orderIt);
                            
                            // If level is now empty, remove it
                            if (levelIt->second.first == 0) {
                                book.erase(levelIt);
                            }
                            
                            // Check if top of book changed
                            if ((ref.is_bid && ref.price == currentTop.top_level.bid_nanos) ||
                                (!ref.is_bid && ref.price == currentTop.top_level.ask_nanos)) {
                                topChanged = true;
                            }
                        }
                    }
                }
                break;
            }

            case book_event_type_e::execute_order: {
                execute_order_t executeOrder;
                bookEventsFile.read(reinterpret_cast<char*>(&executeOrder), sizeof(execute_order_t));
                
                auto orderIt = order_map.find(executeOrder.order_id);
                if (orderIt != order_map.end()) {
                    const auto& ref = orderIt->second;
                    book_side_t& book = ref.is_bid ? bid_book : ask_book;
                    auto levelIt = book.find(ref.price);
                    
                    if (levelIt != book.end()) {
                        // Get the order
                        auto& order = *(ref.order_it);
                        
                        // Create a fill notification
                        book_fill_snapshot_t fill;
                        fill.ts = eventHeader.ts;
                        fill.seq_no = eventHeader.seq_no;
                        fill.resting_order_id = executeOrder.order_id;
                        fill.was_hidden = false;
                        fill.trade_price = ref.price;
                        fill.trade_qty = executeOrder.traded_qty;
                        fill.execution_id = executeOrder.execution_id;
                        fill.resting_original_qty = order.qty;
                        fill.resting_order_remaining_qty = order.qty - executeOrder.traded_qty;
                        fill.resting_order_last_update_ts = order.timestamp;
                        fill.resting_side_is_bid = ref.is_bid;
                        fill.resting_side_price = ref.price;
                        fill.resting_side_qty = levelIt->second.first;
                        
                        // Set opposing side info
                        if (ref.is_bid) {
                            fill.opposing_side_price = ask_book.empty() ? INT64_MAX : ask_book.begin()->first;
                            fill.opposing_side_qty = ask_book.empty() ? 0 : ask_book.begin()->second.first;
                        } else {
                            fill.opposing_side_price = bid_book.empty() ? 0 : bid_book.rbegin()->first;
                            fill.opposing_side_qty = bid_book.empty() ? 0 : bid_book.rbegin()->second.first;
                        }
                        
                        // Update order quantity
                        order.qty -= executeOrder.traded_qty;
                        levelIt->second.first -= executeOrder.traded_qty;
                        
                        // Process the fill through our simulator
                        processBookFill(fill);
                        
                        // If order is fully executed, remove it
                        if (order.qty == 0) {
                            levelIt->second.second.erase(ref.order_it);
                            order_map.erase(orderIt);
                            
                            // If level is now empty, remove it
                            if (levelIt->second.first == 0) {
                                book.erase(levelIt);
                            }
                            
                            // Check if top of book changed
                            if ((ref.is_bid && ref.price == currentTop.top_level.bid_nanos) ||
                                (!ref.is_bid && ref.price == currentTop.top_level.ask_nanos)) {
                                topChanged = true;
                            }
                        }
                    }
                }
                break;
            }
            
            case book_event_type_e::execute_order_at_price: {
                execute_order_at_price_t executeOrder;
                bookEventsFile.read(reinterpret_cast<char*>(&executeOrder), sizeof(execute_order_at_price_t));
                
                auto orderIt = order_map.find(executeOrder.order_id);
                if (orderIt != order_map.end()) {
                    const auto& ref = orderIt->second;
                    book_side_t& book = ref.is_bid ? bid_book : ask_book;
                    auto levelIt = book.find(ref.price);
                    
                    if (levelIt != book.end()) {
                        // Get the order
                        auto& order = *(ref.order_it);
                        
                        // Create a fill notification using execution price
                        book_fill_snapshot_t fill;
                        fill.ts = eventHeader.ts;
                        fill.seq_no = eventHeader.seq_no;
                        fill.resting_order_id = executeOrder.order_id;
                        fill.was_hidden = false;
                        fill.trade_price = executeOrder.execution_price;
                        fill.trade_qty = executeOrder.traded_qty;
                        fill.execution_id = executeOrder.execution_id;
                        fill.resting_original_qty = order.qty;
                        fill.resting_order_remaining_qty = order.qty - executeOrder.traded_qty;
                        fill.resting_order_last_update_ts = order.timestamp;
                        fill.resting_side_is_bid = ref.is_bid;
                        fill.resting_side_price = ref.price;
                        fill.resting_side_qty = levelIt->second.first;
                        
                        // Set opposing side info
                        if (ref.is_bid) {
                            fill.opposing_side_price = ask_book.empty() ? INT64_MAX : ask_book.begin()->first;
                            fill.opposing_side_qty = ask_book.empty() ? 0 : ask_book.begin()->second.first;
                        } else {
                            fill.opposing_side_price = bid_book.empty() ? 0 : bid_book.rbegin()->first;
                            fill.opposing_side_qty = bid_book.empty() ? 0 : bid_book.rbegin()->second.first;
                        }
                        
                        // Update order quantity
                        order.qty -= executeOrder.traded_qty;
                        levelIt->second.first -= executeOrder.traded_qty;
                        
                        // Process the fill through our simulator
                        processBookFill(fill);
                        
                        // If order is fully executed, remove it
                        if (order.qty == 0) {
                            levelIt->second.second.erase(ref.order_it);
                            order_map.erase(orderIt);
                            
                            // If level is now empty, remove it
                            if (levelIt->second.first == 0) {
                                book.erase(levelIt);
                            }
                            
                            // Check if top of book changed
                            if ((ref.is_bid && ref.price == currentTop.top_level.bid_nanos) ||
                                (!ref.is_bid && ref.price == currentTop.top_level.ask_nanos)) {
                                topChanged = true;
                            }
                        }
                    }
                }
                break;
            }
                        
            case book_event_type_e::clear_book: {
                // Clear the entire book
                bid_book.clear();
                ask_book.clear();
                order_map.clear();
                topChanged = true;
                break;
            }
            
            default:
                // Skip any other event types
                break;
        }
        
        // Update top of book if needed
        if (topChanged) {
            updateTopLevels();
            
            // Now process the updated book top through our strategy
            processBookTop(currentTop);
        }
        
        processedEvents++;
        
        // Print progress
        if (processedEvents % 100000 == 0) {
            std::cout << "Processed " << processedEvents << " book events..." << std::endl;
            std::cout << "Current book: Bid " << bid_book.size() << " levels, Ask " 
                      << ask_book.size() << " levels, " << order_map.size() << " active orders" << std::endl;
            std::cout << "Current fills: " << totalOrdersFilled_ << " of " 
                      << totalOrdersPlaced_ << " orders" << std::endl;
            
            // Print current position and P&L if we have valid prices
            if (currentTop.top_level.bid_nanos > 0 && currentTop.top_level.ask_nanos < INT64_MAX) {
                int64_t midPrice = (currentTop.top_level.bid_nanos + currentTop.top_level.ask_nanos) / 2;
                int64_t positionValue = position_ * midPrice;
                std::cout << "Current position: " << position_ << " shares, value: $" 
                          << static_cast<double>(positionValue) / 1e9 << std::endl;
            }
        }
    }
    
    std::cout << "Simulation complete. Processed " << processedEvents << " book events." << std::endl;
    
    // Close file
    bookEventsFile.close();
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