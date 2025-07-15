#ifndef CORRELATION_STRATEGY_H
#define CORRELATION_STRATEGY_H

#include "strategy.h"
#include "../types/market_data_types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <queue>
#include <memory>
#include <fstream>

class CorrelationStrategy : public Strategy {
public:
    CorrelationStrategy(const std::string& correlation_csv_path,
                        double place_edge_percent = 0.01,
                        double cancel_edge_percent = 0.005,
                        double self_weight = 0.5,
                        const std::string& data_path = "");
    
    std::vector<OrderAction> onBookTopUpdate(const book_top_t& bookTop) override;
    std::vector<OrderAction> onFill(const book_fill_snapshot_t& fill) override;
    std::vector<OrderAction> onOrderFilled(uint64_t orderId, int64_t fillPrice, 
                                           uint32_t fillQty, bool isBid) override;
    
    void setSymbolId(uint64_t symbolId) override;
    std::string getName() const override;

private:
    // Structure to track correlated symbols
    struct CorrelatedSymbol {
        std::string symbol;
        double correlation;
        int64_t last_mid_price;
        
        CorrelatedSymbol() : symbol(""), correlation(0.0), last_mid_price(0) {}

        CorrelatedSymbol(const std::string& sym, double corr) 
            : symbol(sym), correlation(corr), last_mid_price(0) {}
        
        bool operator<(const CorrelatedSymbol& other) const {
            return correlation < other.correlation;
        }
    };
    
    // Maps from symbolId to name and back
    std::unordered_map<uint64_t, std::string> symbol_id_to_name_;
    std::unordered_map<std::string, uint64_t> symbol_name_to_id_;
    
    // Map of correlated symbols for each symbol
    std::unordered_map<std::string, std::vector<CorrelatedSymbol>> correlations_;
    
    // Track the latest mid prices for all symbols
    std::unordered_map<uint64_t, int64_t> symbol_mid_prices_;
    
    // Current symbol info
    uint64_t symbolId_;
    std::string symbol_name_;
    std::vector<CorrelatedSymbol> top_correlations_;
    
    // Strategy parameters
    double place_edge_percent_;
    double cancel_edge_percent_;
    double self_weight_;
    std::string data_path_;

    // Order tracking
    uint64_t nextOrderId_;
    uint64_t currentBidOrderId_;
    uint64_t currentAskOrderId_;
    int64_t currentBidPrice_;
    int64_t currentAskPrice_;
    int64_t lastTheoPrice_;
    
    // Active order tracking
    struct OrderInfo {
        uint64_t orderId;
        uint64_t creationTime;
        int64_t price;
        uint32_t quantity;
        bool isBid;
    };
    std::vector<OrderInfo> activeOrders_;
    
    // Helper methods
    void loadCorrelationData(const std::string& csv_path);
    void initializeSymbolMapping();
    int64_t calculateTheoreticalPrice(const book_top_t& bookTop);
    double getCorrelationFactor(double correlation);
    std::vector<OrderAction> checkForStaleOrders(uint64_t currentTimestamp);
    std::vector<OrderAction> updateOrdersForBookTop(const book_top_t& bookTop);
    void removeOrder(uint64_t orderId);
    
    static constexpr uint64_t TEN_MINUTES_NS = 600000000000ULL; // 10 minutes in nanoseconds
    static constexpr int MAX_CORRELATED_SYMBOLS = 10;

        struct SymbolData {
        std::string symbol;
        std::ifstream book_events_file;
        std::ifstream book_tops_file;
        std::ifstream book_fills_file;
        book_top_t last_book_top;
        bool is_valid;
    };

    std::string base_path_;
    bool using_book_events_;
    std::unordered_map<std::string, SymbolData> correlated_symbols_data_;

    void loadCorrelatedSymbolsData(const std::string& main_symbol_path);
    void processCorrelatedSymbolsData(uint64_t current_ts);

    std::string lowercase(const std::string& s);

    std::unordered_map<uint64_t, std::deque<std::pair<uint64_t, int64_t>>> symbol_price_history_;
    const size_t MAX_HISTORY_POINTS = 20;
    const uint64_t MAX_HISTORY_TIME_NS = 60'000'000'000; // 60 seconds in nanoseconds
};

#endif