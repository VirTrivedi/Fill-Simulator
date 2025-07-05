#ifndef MARKET_DATA_TYPES_H
#define MARKET_DATA_TYPES_H

#include <cstdint>

#pragma pack(push, 1)

struct book_fills_file_hdr_t
{
    uint64_t feed_id;
    uint32_t dateint;
    uint32_t number_of_fills;
    uint64_t symbol_idx;
};
static_assert(sizeof(book_fills_file_hdr_t) == 24, "book_fills_file_hdr_t should be 24");

struct book_fill_snapshot_t
{
    uint64_t ts;
    uint64_t seq_no;
    uint64_t resting_order_id;
    bool was_hidden;
    int64_t trade_price;
    uint32_t trade_qty;
    uint64_t execution_id;
    uint32_t resting_original_qty;
    uint32_t resting_order_remaining_qty;
    uint64_t resting_order_last_update_ts;
    bool resting_side_is_bid;
    int64_t resting_side_price;
    uint32_t resting_side_qty;
    int64_t opposing_side_price;
    uint32_t opposing_side_qty;
    uint32_t resting_side_number_of_orders;
};
static_assert(sizeof(book_fill_snapshot_t) == 90, "book_fill_snapshot_t should be 90");

struct book_tops_file_hdr_t
{
    uint64_t feed_id;
    uint32_t dateint;
    uint32_t number_of_tops;
    uint64_t symbol_idx;
};
static_assert(sizeof(book_tops_file_hdr_t) == 24, "book_tops_file_hdr_t should be 24");

struct book_top_level_t
{
    int64_t bid_nanos;
    int64_t ask_nanos;
    uint32_t bid_qty;
    uint32_t ask_qty;
};
static_assert(sizeof(book_top_level_t) == 24, "book_top_level_t should be 24");

struct book_top_t
{
    uint64_t ts;
    uint64_t seqno;
    book_top_level_t top_level;
    book_top_level_t second_level;
    book_top_level_t third_level;
};
static_assert(sizeof(book_top_t) == 88, "book_top_t should be 88");

struct book_events_file_hdr_t
{
    uint64_t feed_id;
    uint32_t dateint;
    uint32_t number_of_events;
    uint64_t symbol_idx;
};
static_assert(sizeof(book_events_file_hdr_t) == 24, "book_events_file_hdr_t should be 24");

namespace book_event_type_e
{
    enum Enum : uint8_t
    {
        invalid = 0,
        add_order = 1,
        delete_order = 2,
        replace_order = 3,
        amend_order = 4,
        reduce_order = 5,
        execute_order = 6,
        execute_order_at_price = 7,
        clear_book = 8,
        session_event = 9,
        hidden_trade = 10
    };
}

struct book_event_hdr_t
{
    uint64_t ts;
    uint64_t seq_no;
    book_event_type_e::Enum type;
};
static_assert(sizeof(book_event_hdr_t) == 17, "book_event_hdr_t should be 17");

struct add_order_t
{
    int64_t price;
    uint64_t order_id;
    uint32_t qty;
    bool is_bid;
};
static_assert(sizeof(add_order_t) == 21, "add_order_t should be 21");

struct delete_order_t
{
    uint64_t order_id;
};
static_assert(sizeof(delete_order_t) == 8, "delete_order_t should be 8");

struct replace_order_t
{
    int64_t price;
    uint64_t orig_order_id;
    uint64_t new_order_id;
    uint32_t qty;
};
static_assert(sizeof(replace_order_t) == 28, "replace_order_t should be 28");

struct amend_order_t
{
    uint64_t order_id;
    uint32_t new_qty;
};
static_assert(sizeof(amend_order_t) == 12, "amend_order_t should be 12");

struct reduce_order_t
{
    uint64_t order_id;
    uint32_t cxled_qty;
};
static_assert(sizeof(reduce_order_t) == 12, "reduce_order_t should be 12");

struct execute_order_t
{
    uint64_t order_id;
    uint32_t traded_qty;
    uint64_t execution_id;
};
static_assert(sizeof(execute_order_t) == 20, "execute_order_t should be 20");

struct execute_order_at_price_t
{
    uint64_t order_id;
    uint32_t traded_qty;
    uint64_t execution_id;
    int64_t execution_price;
};
static_assert(sizeof(execute_order_at_price_t) == 28, "execute_order_at_price_t should be 28");

struct session_event_t
{
    bool allow_crossed_book;
};
static_assert(sizeof(session_event_t) == 1, "session_event_t should be 1");

struct hidden_trade_t
{
    int64_t fill_price;
    uint64_t resting_order_id;
    uint32_t fill_qty;
    bool resting_is_bid;
    uint64_t execution_id;
};
static_assert(sizeof(hidden_trade_t) == 29, "hidden_trade_t should be 29");

#pragma pack(pop)
#endif