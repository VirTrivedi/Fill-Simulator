#include "basic_strategy.h"
#include <iostream>
#include <algorithm>

BasicStrategy::BasicStrategy() : 
    symbolId_(0), 
    nextOrderId_(1), 
    currentBidOrderId_(0),
    currentAskOrderId_(0),
    currentBidPrice_(0),
    currentAskPrice_(0) {}

std::string BasicStrategy::getName() const {
    return "Basic Strategy";
}

// Set the symbol ID for this strategy
void BasicStrategy::setSymbolId(uint64_t symbolId) {
    symbolId_ = symbolId;
}

// Handle book top updates
std::vector<OrderAction> BasicStrategy::onBookTopUpdate(const book_top_t& bookTop) {
    if (bookTop.top_level.bid_nanos <= 0 || bookTop.top_level.ask_nanos <= 0 ||
        bookTop.top_level.bid_nanos >= bookTop.top_level.ask_nanos) {
        return {};
    }

    // Check for orders that need to be canceled
    std::vector<OrderAction> cancelActions = checkForStaleOrders(bookTop.ts);
    
    // Get new order actions
    std::vector<OrderAction> newOrderActions = updateOrdersForBookTop(bookTop);
    
    // Combine both action sets
    cancelActions.insert(cancelActions.end(), newOrderActions.begin(), newOrderActions.end());
    return cancelActions;
}

// Handle book fills
std::vector<OrderAction> BasicStrategy::onFill(const book_fill_snapshot_t& /* fill */) {
    return {};
}

// Handle filled orders
std::vector<OrderAction> BasicStrategy::onOrderFilled(uint64_t orderId, int64_t /* fillPrice */,
                                                      uint32_t /* fillQty */, bool isBid) {
    // Check for invalid order ID
    if (orderId == 0) {
        return {};
    }
    
    // Update tracking variables
    if (isBid && orderId == currentBidOrderId_) {
        currentBidOrderId_ = 0;
    } else if (!isBid && orderId == currentAskOrderId_) {
        currentAskOrderId_ = 0;
    }

    // Find the order in active orders first
    auto it = std::find_if(activeOrders_.begin(), activeOrders_.end(), 
                           [orderId](const OrderInfo& order) { return order.orderId == orderId; });
    
    // Remove if the order exists
    if (it != activeOrders_.end()) {
        removeOrder(orderId);
    }

    return {};
}

// Helper function to remove an order from active orders list
void BasicStrategy::removeOrder(uint64_t orderId) {
    // Check for invalid order ID
    if (orderId == 0) {
        return;
    }

    activeOrders_.erase(
        std::remove_if(activeOrders_.begin(), activeOrders_.end(),
            [orderId](const OrderInfo& order) { return order.orderId == orderId; }),
        activeOrders_.end()
    );
    
    // Clear any tracking variables
    if (orderId == currentBidOrderId_) {
        currentBidOrderId_ = 0;
    }
    if (orderId == currentAskOrderId_) {
        currentAskOrderId_ = 0;
    }
}

// Helper function to check for orders that need to be canceled
std::vector<OrderAction> BasicStrategy::checkForStaleOrders(uint64_t currentTimestamp) {
    std::vector<OrderAction> actions;
    std::vector<uint64_t> orderIdsToRemove;

    // Check for active orders
    if (activeOrders_.empty()) {
        return actions;
    }

    for (const auto& order : activeOrders_) {
        if (currentTimestamp < order.creationTime) {
            continue;
        }
        
        if (currentTimestamp - order.creationTime >= ORDER_EXPIRY_TIME_NS) {
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

// Helper function to update orders based on the book top
std::vector<OrderAction> BasicStrategy::updateOrdersForBookTop(const book_top_t& bookTop) {
    std::vector<OrderAction> actions;
    
    if (bookTop.top_level.bid_nanos <= 0 || bookTop.top_level.ask_nanos <= 0 || 
        bookTop.top_level.bid_nanos >= bookTop.top_level.ask_nanos ||
        bookTop.top_level.bid_nanos == INT64_MAX || bookTop.top_level.ask_nanos == INT64_MAX) {
        return actions;
    }
    
    static bool placeBuyOrder = true;
    static uint64_t lastOrderTime = 0;
    
    if (bookTop.ts - lastOrderTime < 10000) {
        return actions;
    }
    
    if (placeBuyOrder) {
        // Place buy order at the bid price
        int64_t bidPrice = bookTop.top_level.bid_nanos;
        uint32_t bidQty = 1;
                
        // New buy order
        OrderAction newBid;
        newBid.type = OrderAction::Type::ADD;
        newBid.orderId = nextOrderId_++;
        newBid.symbolId = symbolId_;
        newBid.sent_ts = bookTop.ts;
        newBid.md_ts = bookTop.ts;
        newBid.price = bidPrice;
        newBid.quantity = bidQty;
        newBid.isBid = true;
        newBid.isPostOnly = false;
        actions.push_back(newBid);
        
        currentBidOrderId_ = newBid.orderId;
        currentBidPrice_ = bidPrice;

        // Add to active orders
        OrderInfo bidOrderInfo;
        bidOrderInfo.orderId = newBid.orderId;
        bidOrderInfo.creationTime = bookTop.ts;
        bidOrderInfo.price = bidPrice;
        bidOrderInfo.quantity = bidQty;
        bidOrderInfo.isBid = true;
        activeOrders_.push_back(bidOrderInfo);
    } else {
        // Place sell order at the ask price
        int64_t askPrice = bookTop.top_level.ask_nanos;
        uint32_t askQty = 1;
        
        // New sell order
        OrderAction newAsk;
        newAsk.type = OrderAction::Type::ADD;
        newAsk.orderId = nextOrderId_++;
        newAsk.symbolId = symbolId_;
        newAsk.sent_ts = bookTop.ts;
        newAsk.md_ts = bookTop.ts;
        newAsk.price = askPrice;
        newAsk.quantity = askQty;
        newAsk.isBid = false;
        newAsk.isPostOnly = false;
        actions.push_back(newAsk);
        
        currentAskOrderId_ = newAsk.orderId;
        currentAskPrice_ = askPrice;

        // Add to active orders
        OrderInfo askOrderInfo;
        askOrderInfo.orderId = newAsk.orderId;
        askOrderInfo.creationTime = bookTop.ts;
        askOrderInfo.price = askPrice;
        askOrderInfo.quantity = askQty;
        askOrderInfo.isBid = false;
        activeOrders_.push_back(askOrderInfo);
    }
    
    placeBuyOrder = !placeBuyOrder;
    lastOrderTime = bookTop.ts;
    
    return actions;
}