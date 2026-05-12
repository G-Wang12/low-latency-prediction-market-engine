#pragma once

#include "order_book.hpp"

class IAlphaSignal
{
public:
    virtual ~IAlphaSignal() = default;

    // Update internal state from the latest tick/book and return a confidence score in [-1.0, 1.0].
    virtual double update(const LimitOrderBook &book, const MarketTick &tick) = 0;
};
