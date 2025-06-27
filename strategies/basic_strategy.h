#ifndef BASIC_MARKET_MAKER_H
#define BASIC_MARKET_MAKER_H

#include "strategy.h"
#include <map>
#include <string>

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
    uint64_t symbolId_;
    uint64_t nextOrderId_;
    
    uint64_t currentBidOrderId_;
    uint64_t currentAskOrderId_;
    int64_t currentBidPrice_;
    int64_t currentAskPrice_;
    
    std::vector<OrderAction> updateOrdersForBookTop(const book_top_t& bookTop);
};

#endif