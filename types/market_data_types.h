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

#pragma pack(pop)
#endif