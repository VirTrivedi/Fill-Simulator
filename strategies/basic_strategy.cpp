#include "basic_strategy.h"
#include <iostream>

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
    
    return updateOrdersForBookTop(bookTop);
}

// Handle book fills
std::vector<OrderAction> BasicStrategy::onFill(const book_fill_snapshot_t& /* fill */) {
    return {};
}

// Handle filled orders
std::vector<OrderAction> BasicStrategy::onOrderFilled(uint64_t orderId, int64_t /* fillPrice */,
                                                      uint32_t /* fillQty */, bool isBid) {
    if (isBid && orderId == currentBidOrderId_) {
        currentBidOrderId_ = 0;
    } else if (!isBid && orderId == currentAskOrderId_) {
        currentAskOrderId_ = 0;
    }
    
    return {};
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
        // Place buy order at the ask price
        int64_t bidPrice = bookTop.top_level.ask_nanos;
        uint32_t bidQty = 1;
        
        // Cancel existing bid
        if (currentBidOrderId_ != 0) {
            OrderAction cancelBid;
            cancelBid.type = OrderAction::Type::CANCEL;
            cancelBid.orderId = currentBidOrderId_;
            actions.push_back(cancelBid);
        }
        
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
    } else {
        // Place sell order at the bid price
        int64_t askPrice = bookTop.top_level.bid_nanos;
        uint32_t askQty = 1;
        
        // Cancel existing ask
        if (currentAskOrderId_ != 0) {
            OrderAction cancelAsk;
            cancelAsk.type = OrderAction::Type::CANCEL;
            cancelAsk.orderId = currentAskOrderId_;
            actions.push_back(cancelAsk);
        }
        
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
    }
    
    placeBuyOrder = !placeBuyOrder;
    lastOrderTime = bookTop.ts;
    
    return actions;
}