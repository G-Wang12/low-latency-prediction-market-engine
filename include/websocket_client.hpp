#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/core/flat_static_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/websocket/stream.hpp>

#include "market_parser.hpp"
#include "spsc_queue.hpp"

class WebSocketClient
{
public:
    // L2 snapshot/delta messages can be larger than single-tick mocks.
    static constexpr std::size_t kReadBufferBytes = 16384;

    WebSocketClient(boost::asio::io_context &ioc,
                    boost::asio::ssl::context &ssl_ctx,
                    SpscQueue<MarketTick, 1024> &out_queue) noexcept;

    WebSocketClient(const WebSocketClient &) = delete;
    WebSocketClient &operator=(const WebSocketClient &) = delete;

    void run(std::string host, std::string port, std::string target);

    void close();

private:
    using tcp = boost::asio::ip::tcp;

    void on_resolve(boost::beast::error_code ec, const tcp::resolver::results_type &results);
    void on_connect(boost::beast::error_code ec, const tcp::resolver::results_type::endpoint_type &);
    void on_ssl_handshake(boost::beast::error_code ec);
    void on_ws_handshake(boost::beast::error_code ec);
    void do_subscribe();
    void do_read();
    void on_read(boost::beast::error_code ec, std::size_t bytes_transferred);
    void on_close(boost::beast::error_code ec);

    void fail(const char *what, boost::beast::error_code ec) noexcept;

    tcp::resolver resolver_;
    boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>> ws_;

    boost::beast::flat_static_buffer<WebSocketClient::kReadBufferBytes> read_buffer_;

    SpscQueue<MarketTick, 1024> &queue_;
    MarketParser parser_{};

    std::string host_;
    std::string port_;
    std::string target_;
    std::string host_header_;
};
