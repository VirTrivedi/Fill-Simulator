#ifndef FILL_SIMULATOR_H
#define FILL_SIMULATOR_H

#include <string>
#include <map>
#include <unordered_map>
#include <memory>
#include <vector>
#include <list>
#include <fstream>
#include "types/market_data_types.h"
#include "strategies/strategy.h"

class FillSimulator {
public:
    FillSimulator(const std::string& outputFilePath, 
                  uint64_t strategyMdLatencyNs = 1000,
                  uint64_t exchangeLatencyNs = 10000,
                  bool useQueueSimulation = false);
    ~FillSimulator();
    
    void setStrategy(std::shared_ptr<Strategy> strategy);
    
    void processBookTop(const book_top_t& bookTop);
    void processBookFill(const book_fill_snapshot_t& fill);
    
    void runSimulation(const std::string& topsFilePath, const std::string& fillsFilePath);
    void runQueueSimulation(const std::string& bookEventsFilePath);

    void calculateResults();
    
private:
    bool wouldOrderBeFilled(uint64_t orderId, bool isBid, int64_t price, uint32_t quantity);

    void processFill(uint64_t orderId, int64_t fillPrice, uint32_t fillQty, bool isBid, 
                     uint64_t fillNotificationTime = 0);
    
    void processAction(const OrderAction& action, const book_top_t& bookTop);
    
    // Track market state
    struct MarketState {
        book_top_t lastBookTop;
        std::map<int64_t, uint32_t> bidLevels;
        std::map<int64_t, uint32_t> askLevels;
        int64_t lastValidMidPrice;
    };

    // Track order information
    struct OrderInfo {
        uint64_t orderId;
        uint32_t symbolId;
        uint64_t sent_ts;
        uint64_t md_ts;
        int64_t price;
        uint32_t quantity;
        uint32_t filledQuantity;
        bool isBid;
        bool isPostOnly;
        
        OrderInfo() : orderId(0), symbolId(0), sent_ts(0), md_ts(0), price(0), 
                    quantity(0), filledQuantity(0), isBid(false), isPostOnly(true) {}
    };

    struct OrderRecord {
        uint64_t timestamp;
        uint8_t event_type; // 1=add, 2=cancel, 3=fill, 4=replace
        uint64_t order_id;
        uint32_t symbol_id;
        int64_t price;
        int64_t old_price;
        uint32_t quantity;
        uint32_t old_quantity;
        bool is_bid;
    
        OrderRecord() : timestamp(0), event_type(0), order_id(0), symbol_id(0),
                    price(0), quantity(0), is_bid(false) {}
    };

    void writeOrderRecord(const OrderRecord& record);

    MarketState marketState_;
    std::shared_ptr<Strategy> strategy_;
    std::unordered_map<uint64_t, OrderInfo> activeOrders_;
    
    int64_t position_;
    int64_t cashFlow_;
    std::string outputFilePath_;
    std::ofstream outputFile_;
    
    uint64_t totalOrdersPlaced_;
    uint64_t totalOrdersFilled_;
    uint64_t totalBuyVolume_;
    uint64_t totalSellVolume_;
    double totalBuyCost_;
    double totalSellProceeds_;

    uint64_t strategyMdLatencyNs_;
    uint64_t exchangeLatencyNs_;
    
    uint64_t applyMdLatency(uint64_t timestamp) const;
    uint64_t applyExchangeLatency(uint64_t timestamp) const;

    struct LatencyStats {
        uint64_t totalMdEvents = 0;
        uint64_t totalMdToStrategyLatencyNs = 0;
        uint64_t totalStrategyToExchangeLatencyNs = 0;
        uint64_t totalExchangeToNotificationLatencyNs = 0;
    };
    
    LatencyStats latencyStats_;

    bool useQueueSimulation_;

    using price_t = int64_t;
    using qty_t = uint32_t;

    struct order_t {
        uint64_t order_id;
        qty_t qty;
        uint64_t timestamp;
    };

    // Using a list for the queue of orders at each price level
    using order_queue_t = std::list<order_t>;
    using level_t = std::pair<qty_t, order_queue_t>;
    using book_side_t = std::map<price_t, level_t>;

    // Order reference to quickly locate orders in the book
    struct order_ref_t {
        price_t price;
        bool is_bid;
        order_queue_t::iterator order_it;
    };
};

#endif