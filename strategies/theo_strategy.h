#ifndef THEO_STRATEGY_H
#define THEO_STRATEGY_H

#include "strategy.h"
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <utility>

class TheoStrategy : public Strategy {
public:
    TheoStrategy(double placeEdgePercent = 0.01, double cancelEdgePercent = 0.005, 
                 double tradeWeight = 0.7, double emaDecay = 0.05);
    
    std::vector<OrderAction> onBookTopUpdate(const book_top_t& bookTop) override;
    std::vector<OrderAction> onFill(const book_fill_snapshot_t& fill) override;
    std::vector<OrderAction> onOrderFilled(uint64_t orderId, int64_t fillPrice, 
                                          uint32_t fillQty, bool isBid) override;
    
    void setSymbolId(uint64_t symbolId) override;
    std::string getName() const override;
    
private:
    struct OrderInfo {
        uint64_t orderId;
        uint64_t creationTime;
        int64_t price;
        uint32_t quantity;
        bool isBid;
    };

    // Theo value calculation
    int64_t calculateTheoValue(const book_top_t& bookTop);
    void updateTradeHistory(int64_t tradePrice, uint64_t timestamp);
    int64_t getTimeWeightedAvgPrice() const;
    
    uint64_t symbolId_;
    uint64_t nextOrderId_;
    
    // Track all active orders
    std::vector<OrderInfo> activeOrders_;

    uint64_t currentBidOrderId_;
    uint64_t currentAskOrderId_;
    int64_t currentBidPrice_;
    int64_t currentAskPrice_;
    
    // Theoretical value and edge parameters
    int64_t currentTheoValue_;
    double placeEdgePercent_;
    double cancelEdgePercent_;
    
    // Trade history and EMA calculation
    struct TradeInfo {
        int64_t price;
        uint64_t timestamp;
    };
    std::deque<TradeInfo> recentTrades_;
    double tradeWeight_;
    double emaDecay_;
    
    // Helper function to update orders based on the book top and theo
    std::vector<OrderAction> updateOrdersForBookTop(const book_top_t& bookTop);

    // Helper function to remove an order from active orders
    void removeOrder(uint64_t orderId);
    
    // Helper to check for orders that need to be canceled
    std::vector<OrderAction> checkOrdersAgainstTheo();
    std::vector<OrderAction> checkForStaleOrders(uint64_t currentTimestamp);
    
    // Order management helpers
    bool shouldCancelBid(int64_t bidPrice, int64_t theoValue);
    bool shouldCancelAsk(int64_t askPrice, int64_t theoValue);
    int64_t calculateBidPrice(int64_t theoValue);
    int64_t calculateAskPrice(int64_t theoValue);
    
    static constexpr uint64_t TEN_MINUTES_NS = 10ULL * 60ULL * 1000000000ULL;  // 10 minutes
    static constexpr int MAX_TRADE_HISTORY = 100;
};

#endif