#include "websocket_client.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string_view>

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/bind_handler.hpp>
#include <boost/beast/http/field.hpp>

#include <openssl/ssl.h>
#include <openssl/err.h>

WebSocketClient::WebSocketClient(boost::asio::io_context &ioc,
                                 boost::asio::ssl::context &ssl_ctx,
                                 SpscQueue<MarketTick, 1024> &out_queue) noexcept
    : resolver_(boost::asio::make_strand(ioc)),
      ws_(boost::asio::make_strand(ioc), ssl_ctx),
      queue_(out_queue)
{
}

void WebSocketClient::run(std::string host, std::string port, std::string target)
{
    host_ = std::move(host);
    port_ = std::move(port);
    target_ = std::move(target);

    host_header_.clear();
    host_header_.reserve(host_.size() + 1U + port_.size());
    host_header_.append(host_);
    host_header_.push_back(':');
    host_header_.append(port_);

    // Set suggested timeouts for the websocket stream.
    ws_.set_option(boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::client));

    // Set a decorator to change the User-Agent of the handshake.
    ws_.set_option(boost::beast::websocket::stream_base::decorator(
        [](boost::beast::websocket::request_type &req)
        {
            req.set(boost::beast::http::field::user_agent, "llpme-ws-client");
        }));

    // Resolve the host.
    resolver_.async_resolve(host_, port_,
                            boost::beast::bind_front_handler(&WebSocketClient::on_resolve, this));
}

void WebSocketClient::close()
{
    ws_.async_close(boost::beast::websocket::close_code::normal,
                    boost::beast::bind_front_handler(&WebSocketClient::on_close, this));
}

void WebSocketClient::on_resolve(boost::beast::error_code ec, const tcp::resolver::results_type &results)
{
    if (ec)
    {
        fail("resolve", ec);
        return;
    }

    // Set a timeout on the operation.
    boost::beast::get_lowest_layer(ws_).expires_after(std::chrono::seconds(30));

    boost::beast::get_lowest_layer(ws_).async_connect(
        results, boost::beast::bind_front_handler(&WebSocketClient::on_connect, this));
}

void WebSocketClient::on_connect(boost::beast::error_code ec, const tcp::resolver::results_type::endpoint_type &)
{
    if (ec)
    {
        fail("connect", ec);
        return;
    }

    // SNI (Server Name Indication) for TLS.
    if (!SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str()))
    {
        boost::beast::error_code sni_ec{static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};
        fail("sni", sni_ec);
        return;
    }

    // Perform the SSL handshake.
    ws_.next_layer().async_handshake(boost::asio::ssl::stream_base::client,
                                     boost::beast::bind_front_handler(&WebSocketClient::on_ssl_handshake, this));
}

void WebSocketClient::on_ssl_handshake(boost::beast::error_code ec)
{
    if (ec)
    {
        fail("ssl_handshake", ec);
        return;
    }

    // Turn off the timeout on the tcp_stream; the websocket stream has its own.
    boost::beast::get_lowest_layer(ws_).expires_never();

    // Perform the websocket handshake.
    ws_.async_handshake(host_header_, target_,
                        boost::beast::bind_front_handler(&WebSocketClient::on_ws_handshake, this));
}

void WebSocketClient::on_ws_handshake(boost::beast::error_code ec)
{
    if (ec)
    {
        fail("ws_handshake", ec);
        return;
    }

    do_subscribe();
}

void WebSocketClient::do_subscribe()
{
    // Placeholder: most real venues require an explicit subscription message
    // before they will begin sending market data.
    static constexpr std::string_view kSubscriptionPayload =
        R"({"assets":["0x0000000000000000000000000000000000000000"],"type":"market"})";

    auto payload = std::make_shared<std::string>(kSubscriptionPayload);

    // JSON is sent as a text websocket message.
    ws_.text(true);

    ws_.async_write(boost::asio::buffer(*payload),
                    [this, payload](boost::beast::error_code ec, std::size_t)
                    {
                        if (ec)
                        {
                            fail("subscribe_write", ec);
                            return;
                        }

                        do_read();
                    });
}

void WebSocketClient::do_read()
{
    ws_.async_read(read_buffer_,
                   boost::beast::bind_front_handler(&WebSocketClient::on_read, this));
}

void WebSocketClient::on_read(boost::beast::error_code ec, std::size_t bytes_transferred)
{
    if (ec)
    {
        fail("read", ec);
        return;
    }

    const auto buf = read_buffer_.data();
    const std::size_t len = bytes_transferred;
    const char *const ptr = static_cast<const char *>(buf.data());

    if (ptr != nullptr && len != 0U)
    {
        (void)parser_.parse_tick(std::string_view(ptr, len), queue_);
    }

    read_buffer_.consume(read_buffer_.size());

    // Immediately queue the next read.
    do_read();
}

void WebSocketClient::on_close(boost::beast::error_code ec)
{
    if (ec)
    {
        fail("close", ec);
    }
}

void WebSocketClient::fail(const char *what, boost::beast::error_code ec) noexcept
{
    std::cerr << "WebSocketClient " << what << " error: " << ec.message() << std::endl;
}
