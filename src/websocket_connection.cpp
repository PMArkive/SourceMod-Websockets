#include "websocket_connection.hpp"
#include "event_loop.hpp"
#include <boost/asio/strand.hpp>

websocket_connection::websocket_connection(string address, string endpoint, uint16_t port) {
    this->address = address;
    this->endpoint = endpoint;
    this->port = port;
    this->ws = make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(boost::asio::make_strand(event_loop.get_context()), event_loop.get_ssl_context());
    this->work = make_unique<boost::asio::io_context::work>(event_loop.get_context());
    this->resolver = make_shared<tcp::resolver>(event_loop.get_context());
}

void websocket_connection::connect() {
    char s_port[8];
    snprintf(s_port, sizeof(s_port), "%hu", this->port);
    tcp::resolver::query query(this->address.c_str(), s_port);
    
    extension.LogMessage("Resolving %s", address.c_str());
    this->resolver->async_resolve(query, beast::bind_front_handler(&websocket_connection::on_resolve, shared_from_this()));
}

void websocket_connection::on_resolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        extension.LogError("Error resolving %s: %s", this->address.c_str(), ec.message().c_str());
        return;
    }

    beast::get_lowest_layer(*this->ws).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(*this->ws).async_connect(results, beast::bind_front_handler(&websocket_connection::on_connect, shared_from_this()));
}

void websocket_connection::on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
    if (ec) {
        extension.LogError("Error connecting to %s: %s", this->address.c_str(), ec.message().c_str());
        return;
    }

    auto host = this->address + ":" + std::to_string(this->port);
    beast::get_lowest_layer(*this->ws).expires_after(std::chrono::seconds(30));
    if (!SSL_set_tlsext_host_name(this->ws->next_layer().native_handle(), host.c_str())) {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category());
        extension.LogError("SSL Error: %s", ec.message().c_str());
    }

    this->ws->next_layer().async_handshake(
        boost::asio::ssl::stream_base::client,
        beast::bind_front_handler(
            &websocket_connection::on_ssl_handshake,
            shared_from_this()));
}

void websocket_connection::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
        extension.LogError("SSL Handshake Error: %s", ec.message().c_str());
        return;
    }
    beast::get_lowest_layer(*this->ws).expires_never();

    this->ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    this->ws->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(beast::http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " sm-ext-websockets v" + SMEXT_CONF_VERSION);
    }));

    this->ws->async_handshake(this->address, this->endpoint.c_str(), beast::bind_front_handler(&websocket_connection::on_handshake, shared_from_this()));
}

void websocket_connection::on_handshake(beast::error_code ec) {
    if (ec) {
        extension.LogError("WebSocket Handshake Error: %s", ec.message().c_str());
        return;
    }

    this->ws->async_write(boost::asio::buffer("foobar"), beast::bind_front_handler(&websocket_connection::on_write, shared_from_this()));
    this->ws->async_read(this->buffer, beast::bind_front_handler(&websocket_connection::on_read, shared_from_this()));
}

void websocket_connection::on_write(beast::error_code ec, size_t bytes_transferred) {
    if (ec) {
        extension.LogError("WebSocket write error: %s", ec.message().c_str());
        return;
    }
}

void websocket_connection::on_read(beast::error_code ec, size_t bytes_transferred) {
    if (ec) {
        extension.LogError("WebSocket read error: %s", ec.message().c_str());
        return this->ws->async_read(this->buffer, beast::bind_front_handler(&websocket_connection::on_read, shared_from_this()));;
    }
    stringstream buffer;
    buffer << beast::make_printable(this->buffer.data());
    extension.LogMessage("Read %d bytes: %s", bytes_transferred, buffer.str().c_str());
    this->ws->async_read(this->buffer, beast::bind_front_handler(&websocket_connection::on_read, shared_from_this()));
}