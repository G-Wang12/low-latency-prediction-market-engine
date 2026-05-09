#include <gtest/gtest.h>

#include "market_parser.hpp"
#include "position_manager.hpp"
#include "spsc_queue.hpp"

// Dummy test to verify test framework is working.
// This confirms GoogleTest integration for the low-latency engine's unit tests.
TEST(DummyTest, BasicAssertion)
{
    EXPECT_EQ(1 + 1, 2);
    EXPECT_TRUE(true);
    EXPECT_FALSE(false);
}

TEST(SpscQueueTest, EmptyAndFullAndFifo)
{
    SpscQueue<int, 8> q;

    int out = 0;
    EXPECT_FALSE(q.pop(out));

    for (int i = 0; i < 8; ++i)
    {
        EXPECT_TRUE(q.push(i));
    }
    EXPECT_FALSE(q.push(123));

    for (int i = 0; i < 8; ++i)
    {
        EXPECT_TRUE(q.pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(q.pop(out));
}

TEST(MarketParserTest, ParsesBidAndAsk)
{
    MarketParser parser;

    SpscQueue<MarketTick, 1024> q;
    EXPECT_TRUE(parser.parse_tick(
        R"({"bids":[["0.57","1200"],["0.56","300"]],"asks":[["0.59","500"]]})",
        q));

    MarketTick tick{};
    ASSERT_TRUE(q.pop(tick));
    EXPECT_EQ(tick.price, static_cast<std::uint8_t>(57));
    EXPECT_EQ(tick.size, 1200U);
    EXPECT_TRUE(tick.is_bid);

    ASSERT_TRUE(q.pop(tick));
    EXPECT_EQ(tick.price, static_cast<std::uint8_t>(56));
    EXPECT_EQ(tick.size, 300U);
    EXPECT_TRUE(tick.is_bid);

    ASSERT_TRUE(q.pop(tick));
    EXPECT_EQ(tick.price, static_cast<std::uint8_t>(59));
    EXPECT_EQ(tick.size, 500U);
    EXPECT_FALSE(tick.is_bid);
}

TEST(MarketParserTest, ParsesPolymarketBook)
{
    MarketParser parser;

    SpscQueue<MarketTick, 1024> q;
    EXPECT_TRUE(parser.parse_tick(
        R"({"event_type":"book","asset_id":"abc","market":"0x0","bids":[{"price":".48","size":"30"},{"price":"0.49","size":"20"}],"asks":[{"price":".52","size":"25"}],"timestamp":"123","hash":"0xabc"})",
        q));

    MarketTick tick{};
    ASSERT_TRUE(q.pop(tick));
    EXPECT_EQ(tick.price, static_cast<std::uint8_t>(48));
    EXPECT_EQ(tick.size, 30U);
    EXPECT_TRUE(tick.is_bid);

    ASSERT_TRUE(q.pop(tick));
    EXPECT_EQ(tick.price, static_cast<std::uint8_t>(49));
    EXPECT_EQ(tick.size, 20U);
    EXPECT_TRUE(tick.is_bid);

    ASSERT_TRUE(q.pop(tick));
    EXPECT_EQ(tick.price, static_cast<std::uint8_t>(52));
    EXPECT_EQ(tick.size, 25U);
    EXPECT_FALSE(tick.is_bid);
}

TEST(MarketParserTest, ParsesPolymarketPriceChange)
{
    MarketParser parser;

    SpscQueue<MarketTick, 1024> q;
    EXPECT_TRUE(parser.parse_tick(
        R"({"event_type":"price_change","market":"0x0","price_changes":[{"asset_id":"abc","price":"0.50","size":"200","side":"BUY","hash":"h"},{"asset_id":"abc","price":"0.52","size":"0","side":"SELL","hash":"h2"}],"timestamp":"123"})",
        q));

    MarketTick tick{};
    ASSERT_TRUE(q.pop(tick));
    EXPECT_EQ(tick.price, static_cast<std::uint8_t>(50));
    EXPECT_EQ(tick.size, 200U);
    EXPECT_TRUE(tick.is_bid);

    ASSERT_TRUE(q.pop(tick));
    EXPECT_EQ(tick.price, static_cast<std::uint8_t>(52));
    EXPECT_EQ(tick.size, 0U);
    EXPECT_FALSE(tick.is_bid);
}

TEST(MarketParserTest, ParsesPolymarketArrayBatch)
{
    MarketParser parser;

    SpscQueue<MarketTick, 1024> q;
    EXPECT_TRUE(parser.parse_tick(
        R"([{"event_type":"price_change","market":"0x0","price_changes":[{"asset_id":"abc","price":"0.50","size":"200","side":"BUY","hash":"h"}],"timestamp":"123"}])",
        q));

    MarketTick tick{};
    ASSERT_TRUE(q.pop(tick));
    EXPECT_EQ(tick.price, static_cast<std::uint8_t>(50));
    EXPECT_EQ(tick.size, 200U);
    EXPECT_TRUE(tick.is_bid);
}

TEST(LimitOrderBookTest, BestBidAskHandleDeletionAndEmpty)
{
    LimitOrderBook book;

    EXPECT_EQ(book.get_best_bid(), LimitOrderBook::kNoBid);
    EXPECT_EQ(book.get_best_ask(), LimitOrderBook::kNoAsk);

    // Add two bid levels; best should be the higher price.
    book.apply_tick(MarketTick{static_cast<std::uint8_t>(57), 100U, true});
    book.apply_tick(MarketTick{static_cast<std::uint8_t>(58), 200U, true});
    EXPECT_EQ(book.get_best_bid(), static_cast<std::uint8_t>(58));

    // Wipe out the best bid with size=0; best should fall back.
    book.apply_tick(MarketTick{static_cast<std::uint8_t>(58), 0U, true});
    EXPECT_EQ(book.get_best_bid(), static_cast<std::uint8_t>(57));

    // Add an ask level then wipe it; ask should return the empty sentinel.
    book.apply_tick(MarketTick{static_cast<std::uint8_t>(59), 123U, false});
    EXPECT_EQ(book.get_best_ask(), static_cast<std::uint8_t>(59));
    book.apply_tick(MarketTick{static_cast<std::uint8_t>(59), 0U, false});
    EXPECT_EQ(book.get_best_ask(), LimitOrderBook::kNoAsk);
}

TEST(PositionManagerTest, LongOpenCloseAndUnrealized)
{
    PositionManager pm;

    pm.add_fill(+10, 0.50); // buy 10 @ 0.50
    EXPECT_EQ(pm.position_size(), 10);
    EXPECT_DOUBLE_EQ(pm.average_entry_price(), 0.50);
    EXPECT_DOUBLE_EQ(pm.realized_pnl(), 0.0);

    // Mark-to-market at mid=0.55 => +10 * (0.55 - 0.50) = +0.50
    EXPECT_DOUBLE_EQ(pm.get_unrealized_pnl(0.55), 0.50);

    // Sell 4 @ 0.60 => realized += 4 * (0.60 - 0.50) = +0.40
    pm.add_fill(-4, 0.60);
    EXPECT_EQ(pm.position_size(), 6);
    EXPECT_DOUBLE_EQ(pm.average_entry_price(), 0.50);
    EXPECT_DOUBLE_EQ(pm.realized_pnl(), 0.40);
}

TEST(PositionManagerTest, FlipThroughZeroResetsEntry)
{
    PositionManager pm;

    // Start long 5 @ 0.40
    pm.add_fill(+5, 0.40);
    EXPECT_EQ(pm.position_size(), 5);
    EXPECT_DOUBLE_EQ(pm.average_entry_price(), 0.40);

    // Sell 8 @ 0.45: close 5 (+0.25 pnl), then open short 3 @ 0.45
    pm.add_fill(-8, 0.45);
    EXPECT_EQ(pm.position_size(), -3);
    EXPECT_DOUBLE_EQ(pm.realized_pnl(), 5.0 * (0.45 - 0.40));
    EXPECT_DOUBLE_EQ(pm.average_entry_price(), 0.45);

    // For a short 3 @ 0.45, mid=0.40 => unrealized = -3 * (0.40 - 0.45) = +0.15
    EXPECT_DOUBLE_EQ(pm.get_unrealized_pnl(0.40), 0.15);
}
