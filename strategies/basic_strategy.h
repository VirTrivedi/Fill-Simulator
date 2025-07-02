#ifndef BASIC_MARKET_MAKER_H
#define BASIC_MARKET_MAKER_H

#include "strategy.h"
#include <map>
#include <string>
#include <vector>
#include <utility>

class BasicStrategy : public Strategy {
public:
    BasicStrategy();
    
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

    uint64_t symbolId_;
    uint64_t nextOrderId_;
    
    // Track all active orders
    std::vector<OrderInfo> activeOrders_;

    uint64_t currentBidOrderId_;
    uint64_t currentAskOrderId_;
    int64_t currentBidPrice_;
    int64_t currentAskPrice_;
    
    // Helper function to update orders based on the book top
    std::vector<OrderAction> updateOrdersForBookTop(const book_top_t& bookTop);

    // Helper function to remove an order from active orders
    void removeOrder(uint64_t orderId);
    
    // Helper to check for orders that need to be canceled
    std::vector<OrderAction> checkForStaleOrders(uint64_t currentTimestamp);
    static constexpr uint64_t TEN_MINUTES_NS = 10ULL * 60ULL * 1000000000ULL; // 10 minutes
};

#endif