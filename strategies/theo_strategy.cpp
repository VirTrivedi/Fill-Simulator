#include "theo_strategy.h"
#include <iostream>
#include <algorithm>
#include <cmath>

TheoStrategy::TheoStrategy(double placeEdgePercent, double cancelEdgePercent, 
                           double tradeWeight, double emaDecay) 
    : symbolId_(0), 
      nextOrderId_(1), 
      currentBidOrderId_(0),
      currentAskOrderId_(0),
      currentBidPrice_(0),
      currentAskPrice_(0),
      currentTheoValue_(0),
      placeEdgePercent_(placeEdgePercent),
      cancelEdgePercent_(cancelEdgePercent),
      tradeWeight_(tradeWeight),
      emaDecay_(emaDecay) {}

std::string TheoStrategy::getName() const {
    return "Theoretical Value Strategy";
}

void TheoStrategy::setSymbolId(uint64_t symbolId) {
    symbolId_ = symbolId;
}

std::vector<OrderAction> TheoStrategy::onBookTopUpdate(const book_top_t& bookTop) {
    if (bookTop.top_level.bid_nanos <= 0 || bookTop.top_level.ask_nanos <= 0 ||
        bookTop.top_level.bid_nanos >= bookTop.top_level.ask_nanos) {
        return {};
    }

    // Calculate theoretical value from this book top
    int64_t theoValue = calculateTheoValue(bookTop);
    currentTheoValue_ = theoValue;
    
    // First, check if any existing orders need to be canceled
    std::vector<OrderAction> cancelActions = checkOrdersAgainstTheo();
    
    // Then check for stale orders
    std::vector<OrderAction> staleOrderActions = checkForStaleOrders(bookTop.ts);
    cancelActions.insert(cancelActions.end(), staleOrderActions.begin(), staleOrderActions.end());
    
    // Get new order actions
    std::vector<OrderAction> newOrderActions = updateOrdersForBookTop(bookTop);
    
    // Combine all actions
    cancelActions.insert(cancelActions.end(), newOrderActions.begin(), newOrderActions.end());
    return cancelActions;
}

std::vector<OrderAction> TheoStrategy::onFill(const book_fill_snapshot_t& fill) {
    // Update trade history
    updateTradeHistory(fill.trade_price, fill.ts);
    
    return {};
}

std::vector<OrderAction> TheoStrategy::onOrderFilled(uint64_t orderId, int64_t fillPrice,
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

    // Update trade history
    updateTradeHistory(fillPrice, 0);
    
    // Find and remove the order
    auto it = std::find_if(activeOrders_.begin(), activeOrders_.end(), 
                         [orderId](const OrderInfo& order) { return order.orderId == orderId; });
    
    if (it != activeOrders_.end()) {
        removeOrder(orderId);
    }

    return {};
}

void TheoStrategy::removeOrder(uint64_t orderId) {
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

std::vector<OrderAction> TheoStrategy::checkOrdersAgainstTheo() {
    std::vector<OrderAction> actions;
    std::vector<uint64_t> orderIdsToRemove;

    if (currentTheoValue_ <= 0) {
        return actions;
    }

    // Check each active order against the current theo value
    for (const auto& order : activeOrders_) {
        if (order.isBid) {
            // For bids, cancel if the price is too high relative to theo
            if (shouldCancelBid(order.price, currentTheoValue_)) {
                OrderAction cancelAction;
                cancelAction.type = OrderAction::Type::CANCEL;
                cancelAction.orderId = order.orderId;
                cancelAction.symbolId = symbolId_;
                actions.push_back(cancelAction);
                
                orderIdsToRemove.push_back(order.orderId);
                
                if (order.orderId == currentBidOrderId_) {
                    currentBidOrderId_ = 0;
                }
            }
        } else {
            // For asks, cancel if the price is too low relative to theo
            if (shouldCancelAsk(order.price, currentTheoValue_)) {
                OrderAction cancelAction;
                cancelAction.type = OrderAction::Type::CANCEL;
                cancelAction.orderId = order.orderId;
                cancelAction.symbolId = symbolId_;
                actions.push_back(cancelAction);
                
                orderIdsToRemove.push_back(order.orderId);
                
                if (order.orderId == currentAskOrderId_) {
                    currentAskOrderId_ = 0;
                }
            }
        }
    }
    
    // Remove canceled orders
    for (uint64_t orderId : orderIdsToRemove) {
        removeOrder(orderId);
    }
    
    return actions;
}

std::vector<OrderAction> TheoStrategy::checkForStaleOrders(uint64_t currentTimestamp) {
    std::vector<OrderAction> actions;
    std::vector<uint64_t> orderIdsToRemove;

    if (activeOrders_.empty()) {
        return actions;
    }

    for (const auto& order : activeOrders_) {
        if (currentTimestamp < order.creationTime) {
            continue;
        }
        
        if (currentTimestamp - order.creationTime >= TEN_MINUTES_NS) {
            // Order is older than 10 minutes, cancel it
            OrderAction cancelAction;
            cancelAction.type = OrderAction::Type::CANCEL;
            cancelAction.orderId = order.orderId;
            cancelAction.symbolId = symbolId_;
            actions.push_back(cancelAction);
            
            orderIdsToRemove.push_back(order.orderId);
            
            if (order.isBid && order.orderId == currentBidOrderId_) {
                currentBidOrderId_ = 0;
            } else if (!order.isBid && order.orderId == currentAskOrderId_) {
                currentAskOrderId_ = 0;
            }
        }
    }
    
    for (uint64_t orderId : orderIdsToRemove) {
        removeOrder(orderId);
    }
    
    return actions;
}

std::vector<OrderAction> TheoStrategy::updateOrdersForBookTop(const book_top_t& bookTop) {
    std::vector<OrderAction> actions;
    
    if (currentTheoValue_ <= 0) {
        return actions;
    }
    
    const int64_t MAX_REASONABLE_PRICE = 10000LL * 1000000000LL; // $10,000 in nanos

    if (bookTop.top_level.bid_nanos <= 0 || bookTop.top_level.ask_nanos <= 0 || 
        bookTop.top_level.bid_nanos >= bookTop.top_level.ask_nanos ||
        bookTop.top_level.bid_nanos > MAX_REASONABLE_PRICE || bookTop.top_level.ask_nanos > MAX_REASONABLE_PRICE) {
        return actions;
    }
    
    // Calculate the optimal bid and ask prices
    int64_t optimalBidPrice = calculateBidPrice(currentTheoValue_);
    int64_t optimalAskPrice = calculateAskPrice(currentTheoValue_);
    
    // Check if bid price is reasonable
    if (optimalBidPrice > 0 && optimalBidPrice < bookTop.top_level.ask_nanos) {
        if (currentBidOrderId_ == 0 || std::abs(optimalBidPrice - currentBidPrice_) > currentTheoValue_ * 0.001) {
            // Cancel existing bid if there is one
            if (currentBidOrderId_ != 0) {
                OrderAction cancelAction;
                cancelAction.type = OrderAction::Type::CANCEL;
                cancelAction.orderId = currentBidOrderId_;
                cancelAction.symbolId = symbolId_;
                actions.push_back(cancelAction);
                
                removeOrder(currentBidOrderId_);
                currentBidOrderId_ = 0;
            }
            
            // Place new bid order
            uint32_t bidQty = 1;
            OrderAction newBid;
            newBid.type = OrderAction::Type::ADD;
            newBid.orderId = nextOrderId_++;
            newBid.symbolId = symbolId_;
            newBid.sent_ts = bookTop.ts;
            newBid.md_ts = bookTop.ts;
            newBid.price = optimalBidPrice;
            newBid.quantity = bidQty;
            newBid.isBid = true;
            newBid.isPostOnly = true;
            actions.push_back(newBid);
            
            currentBidOrderId_ = newBid.orderId;
            currentBidPrice_ = optimalBidPrice;

            // Add to active orders
            OrderInfo bidOrderInfo;
            bidOrderInfo.orderId = newBid.orderId;
            bidOrderInfo.creationTime = bookTop.ts;
            bidOrderInfo.price = optimalBidPrice;
            bidOrderInfo.quantity = bidQty;
            bidOrderInfo.isBid = true;
            activeOrders_.push_back(bidOrderInfo);
            
            std::cout << "Placing bid at $" << static_cast<double>(optimalBidPrice) / 1e9 
                      << " (theo: $" << static_cast<double>(currentTheoValue_) / 1e9 << ")" << std::endl;
        }
    }
    
    // Check if ask price is reasonable
    if (optimalAskPrice > 0 && optimalAskPrice > bookTop.top_level.bid_nanos) {
        if (currentAskOrderId_ == 0 || std::abs(optimalAskPrice - currentAskPrice_) > currentTheoValue_ * 0.001) {
            // Cancel existing ask if there is one
            if (currentAskOrderId_ != 0) {
                OrderAction cancelAction;
                cancelAction.type = OrderAction::Type::CANCEL;
                cancelAction.orderId = currentAskOrderId_;
                cancelAction.symbolId = symbolId_;
                actions.push_back(cancelAction);
                
                removeOrder(currentAskOrderId_);
                currentAskOrderId_ = 0;
            }
            
            // Place new ask order
            uint32_t askQty = 1;
            OrderAction newAsk;
            newAsk.type = OrderAction::Type::ADD;
            newAsk.orderId = nextOrderId_++;
            newAsk.symbolId = symbolId_;
            newAsk.sent_ts = bookTop.ts;
            newAsk.md_ts = bookTop.ts;
            newAsk.price = optimalAskPrice;
            newAsk.quantity = askQty;
            newAsk.isBid = false;
            newAsk.isPostOnly = true;
            actions.push_back(newAsk);
            
            currentAskOrderId_ = newAsk.orderId;
            currentAskPrice_ = optimalAskPrice;

            // Add to active orders
            OrderInfo askOrderInfo;
            askOrderInfo.orderId = newAsk.orderId;
            askOrderInfo.creationTime = bookTop.ts;
            askOrderInfo.price = optimalAskPrice;
            askOrderInfo.quantity = askQty;
            askOrderInfo.isBid = false;
            activeOrders_.push_back(askOrderInfo);
            
            std::cout << "Placing ask at $" << static_cast<double>(optimalAskPrice) / 1e9 
                      << " (theo: $" << static_cast<double>(currentTheoValue_) / 1e9 << ")" << std::endl;
        }
    }
    
    return actions;
}

int64_t TheoStrategy::calculateTheoValue(const book_top_t& bookTop) {
    // Get mid price from book top
    int64_t midPrice = (bookTop.top_level.bid_nanos + bookTop.top_level.ask_nanos) / 2;
    
    // Get time-weighted average price from recent trades
    int64_t tradeAvg = getTimeWeightedAvgPrice();
    
    if (tradeAvg <= 0) {
        return midPrice;
    }
    
    // Blend the trade average and midpoint based on weights
    int64_t theoValue = static_cast<int64_t>(
        tradeWeight_ * tradeAvg + (1 - tradeWeight_) * midPrice);
    
    return theoValue;
}

void TheoStrategy::updateTradeHistory(int64_t tradePrice, uint64_t timestamp) {
    if (tradePrice <= 0) return;
    
    // Add new trade to history
    TradeInfo trade;
    trade.price = tradePrice;
    trade.timestamp = timestamp;
    recentTrades_.push_back(trade);
    
    // Limit the size of trade history
    if (recentTrades_.size() > MAX_TRADE_HISTORY) {
        recentTrades_.pop_front();
    }
}

int64_t TheoStrategy::getTimeWeightedAvgPrice() const {
    if (recentTrades_.empty()) {
        return 0;
    }
    
    if (recentTrades_.size() == 1) {
        return recentTrades_.front().price;
    }
    
    // Calculate exponentially weighted average
    double weightSum = 0;
    double priceSum = 0;
    double weight = 1.0;
    
    for (auto it = recentTrades_.rbegin(); it != recentTrades_.rend(); ++it) {
        priceSum += weight * static_cast<double>(it->price);
        weightSum += weight;
        weight *= (1.0 - emaDecay_);
    }
    
    return static_cast<int64_t>(priceSum / weightSum);
}

bool TheoStrategy::shouldCancelBid(int64_t bidPrice, int64_t theoValue) {
    double edge = (static_cast<double>(theoValue - bidPrice) / theoValue) * 100.0;
    
    // If edge is below cancel threshold, we should cancel
    return edge < cancelEdgePercent_;
}

bool TheoStrategy::shouldCancelAsk(int64_t askPrice, int64_t theoValue) {
    double edge = (static_cast<double>(askPrice - theoValue) / theoValue) * 100.0;
    
    // If edge is below cancel threshold, we should cancel
    return edge < cancelEdgePercent_;
}

int64_t TheoStrategy::calculateBidPrice(int64_t theoValue) {
    // For a bid, we want to be below theo by the edge percentage
    return static_cast<int64_t>(theoValue * (1.0 - (placeEdgePercent_ / 100.0)));
}

int64_t TheoStrategy::calculateAskPrice(int64_t theoValue) {
    // For an ask, we want to be above theo by the edge percentage
    return static_cast<int64_t>(theoValue * (1.0 + (placeEdgePercent_ / 100.0)));
}