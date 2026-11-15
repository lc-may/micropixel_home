// SPDX-License-Identifier: Apache-2.0
#include "network.hpp"

#include <fcntl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core_http_client.h"
#include "core_mqtt.h"

struct NetworkContext {
    micropixel::pc::SocketStream* stream{};
    std::string* received{};
    bool published{}, subscribed{};
    uint32_t receive_timeout_ms{10000};
};
namespace micropixel::pc {
namespace {
using Clock = std::chrono::steady_clock;
bool Cancelled(std::atomic<bool>* cancel) { return cancel && cancel->load(); }
uint32_t Millis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
int32_t Read(NetworkContext_t* c, void* buffer, size_t length) { return c->stream->Read(buffer, length); }
int32_t HttpRead(NetworkContext_t* c, void* buffer, size_t length) {
    return c->stream->ReadWithTimeout(buffer, length, c->receive_timeout_ms);
}
int32_t Write(NetworkContext_t* c, const void* buffer, size_t length) { return c->stream->Write(buffer, length); }
bool Fail(std::string& error, const char* text) {
    error = text;
    return false;
}
std::string HttpAuthority(const Url& url) {
    auto host = url.host.find(':') != std::string::npos ? "[" + url.host + "]" : url.host;
    if (url.port != (url.tls ? "443" : "80")) host += ":" + url.port;
    return host;
}
void MqttEvent(MQTTContext_t* ctx, MQTTPacketInfo_t* packet, MQTTDeserializedInfo_t* info) {
    auto* state = ctx->transportInterface.pNetworkContext;
    if (info->deserializationResult != MQTTSuccess) return;
    if (packet->type == MQTT_PACKET_TYPE_SUBACK) state->subscribed = true;
    if ((packet->type & 0xf0U) == MQTT_PACKET_TYPE_PUBLISH && info->pPublishInfo &&
        info->pPublishInfo->payloadLength <= 4096) {
        state->received->assign(static_cast<const char*>(info->pPublishInfo->pPayload),
                                info->pPublishInfo->payloadLength);
        state->published = true;
    }
}
}  // namespace
bool ParseUrl(const std::string& text, Url& url, std::string& error) {
    if (text.size() > 2048 || text.find_first_of("\r\n\t ") != std::string::npos) return Fail(error, "Invalid URL");
    const auto separator = text.find("://");
    if (separator == std::string::npos) return Fail(error, "A URL needs http://, https://, mqtt:// or mqtts://");
    url = {};
    url.scheme = text.substr(0, separator);
    if (url.scheme != "http" && url.scheme != "https" && url.scheme != "mqtt" && url.scheme != "mqtts")
        return Fail(error, "Unsupported URL scheme");
    url.tls = url.scheme == "https" || url.scheme == "mqtts";
    const auto end = text.find_first_of("/?#", separator + 3);
    const auto authority =
        text.substr(separator + 3, end == std::string::npos ? std::string::npos : end - separator - 3);
    if (authority.empty() || authority.find('@') != std::string::npos)
        return Fail(error, "Missing host; embedded URL credentials are unsupported");
    if (authority[0] == '[') {
        const auto bracket = authority.find(']');
        if (bracket == std::string::npos) return Fail(error, "Invalid IPv6 host");
        url.host = authority.substr(1, bracket - 1);
        if (bracket + 1 < authority.size()) {
            if (authority[bracket + 1] != ':') return Fail(error, "Invalid host");
            url.port = authority.substr(bracket + 2);
        }
    } else {
        const auto colon = authority.find(':');
        url.host = authority.substr(0, colon);
        if (colon != std::string::npos) url.port = authority.substr(colon + 1);
    }
    if (url.host.empty() || url.host.size() > 253) return Fail(error, "Invalid host name");
    if (url.port.empty())
        url.port = url.scheme == "http" ? "80" : url.scheme == "https" ? "443" : url.scheme == "mqtt" ? "1883" : "8883";
    if (url.port.size() > 5 || url.port.find_first_not_of("0123456789") != std::string::npos ||
        std::atoi(url.port.c_str()) < 1 || std::atoi(url.port.c_str()) > 65535)
        return Fail(error, "Invalid TCP port");
    url.path = end == std::string::npos ? "/" : text.substr(end);
    if (auto fragment = url.path.find('#'); fragment != std::string::npos) url.path.resize(fragment);
    if (url.path.empty() || url.path[0] != '/') url.path = "/" + url.path;
    return true;
}
struct SocketStream::State {
    int fd = -1;
    bool tls{};
    std::atomic<bool>* cancel{};
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    mbedtls_ctr_drbg_context random;
    mbedtls_entropy_context entropy;
    State() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&ca);
        mbedtls_ctr_drbg_init(&random);
        mbedtls_entropy_init(&entropy);
    }
    ~State() {
        if (fd >= 0) close(fd);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&config);
        mbedtls_x509_crt_free(&ca);
        mbedtls_ctr_drbg_free(&random);
        mbedtls_entropy_free(&entropy);
    }
    static int Send(void* c, const unsigned char* p, size_t size) {
        auto& s = *static_cast<State*>(c);
        const auto n = send(s.fd, p, size, MSG_NOSIGNAL);
        return n >= 0                                                      ? n
               : errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE
                                                                           : MBEDTLS_ERR_NET_SEND_FAILED;
    }
    static int Receive(void* c, unsigned char* p, size_t size) {
        auto& s = *static_cast<State*>(c);
        const auto n = recv(s.fd, p, size, 0);
        return n >= 0                                                      ? n
               : errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ
                                                                           : MBEDTLS_ERR_NET_RECV_FAILED;
    }
};
SocketStream::SocketStream() : state_(std::make_unique<State>()) {}
SocketStream::~SocketStream() = default;
bool SocketStream::Connect(const Url& url, std::atomic<bool>* cancel, std::string& error) {
    auto& s = *state_;
    s.cancel = cancel;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int status = getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &addresses);
    if (status) {
        error = gai_strerror(status);
        return false;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> holder(addresses, freeaddrinfo);
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    for (auto* address = addresses; address && Clock::now() < deadline && !Cancelled(cancel);
         address = address->ai_next) {
        s.fd = socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, address->ai_protocol);
        if (s.fd < 0) continue;
        if (connect(s.fd, address->ai_addr, address->ai_addrlen) == 0) break;
        if (errno == EINPROGRESS) {
            pollfd p{s.fd, POLLOUT, 0};
            bool connected = false;
            while (Clock::now() < deadline && !Cancelled(cancel)) {
                const int ready = poll(&p, 1, 25);
                if (ready < 0 && errno == EINTR) continue;
                if (ready <= 0) continue;
                int code = 0;
                socklen_t length = sizeof(code);
                connected = getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &code, &length) == 0 && code == 0;
                break;
            }
            if (connected) break;
        }
        close(s.fd);
        s.fd = -1;
    }
    if (s.fd < 0 || Cancelled(cancel)) return Fail(error, "TCP connection failed or cancelled");
    s.tls = url.tls;
    if (!s.tls) return true;
    const char* ca_file = std::getenv("MICROPIXEL_CA_FILE");
    if (mbedtls_x509_crt_parse_file(&s.ca, ca_file ? ca_file : "/etc/ssl/certs/ca-certificates.crt") < 0)
        return Fail(error, "Cannot load trusted CA certificates");
    constexpr unsigned char personalization[] = "micropixel-pc";
    int result = mbedtls_ctr_drbg_seed(&s.random, mbedtls_entropy_func, &s.entropy, personalization,
                                       sizeof(personalization) - 1);
    if (!result)
        result = mbedtls_ssl_config_defaults(&s.config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                             MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&s.config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&s.config, &s.ca, nullptr);
    mbedtls_ssl_conf_rng(&s.config, mbedtls_ctr_drbg_random, &s.random);
    if (!result) result = mbedtls_ssl_setup(&s.ssl, &s.config);
    if (!result) result = mbedtls_ssl_set_hostname(&s.ssl, url.host.c_str());
    mbedtls_ssl_set_bio(&s.ssl, &s, State::Send, State::Receive, nullptr);
    const auto tls_deadline = Clock::now() + std::chrono::seconds(10);
    if (!result) do {
            if (Cancelled(cancel) || Clock::now() > tls_deadline)
                return Fail(error, "TLS handshake timed out or cancelled");
            result = mbedtls_ssl_handshake(&s.ssl);
            if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE) {
                pollfd p{s.fd, static_cast<short>(result == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT), 0};
                (void)poll(&p, 1, 25);
            }
        } while (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (result) {
        char text[192];
        mbedtls_strerror(result, text, sizeof(text));
        error = text;
        return false;
    }
    return mbedtls_ssl_get_verify_result(&s.ssl) == 0 || Fail(error, "TLS certificate verification failed");
}
int32_t SocketStream::Read(void* buffer, size_t length) {
    auto& s = *state_;
    if (Cancelled(s.cancel)) return -1;
    pollfd p{s.fd, POLLIN, 0};
    if (!s.tls || !mbedtls_ssl_get_bytes_avail(&s.ssl)) {
        const int result = poll(&p, 1, 10);
        if (result == 0 || (result < 0 && errno == EINTR)) return 0;
        if (result < 0) return -1;
    }
    const int n = s.tls ? mbedtls_ssl_read(&s.ssl, static_cast<unsigned char*>(buffer), length)
                        : State::Receive(&s, static_cast<unsigned char*>(buffer), length);
    return n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ? 0 : n > 0 ? n : -1;
}
int32_t SocketStream::ReadWithTimeout(void* buffer, size_t length, uint32_t timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const auto result = Read(buffer, length);
        if (result != 0) return result;
        if (Clock::now() >= deadline) return -1;
    }
}
int32_t SocketStream::Write(const void* buffer, size_t length) {
    auto& s = *state_;
    if (Cancelled(s.cancel)) return -1;
    const int n = s.tls ? mbedtls_ssl_write(&s.ssl, static_cast<const unsigned char*>(buffer), length)
                        : State::Send(&s, static_cast<const unsigned char*>(buffer), length);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
        pollfd p{s.fd, POLLOUT, 0};
        (void)poll(&p, 1, 10);
        return 0;
    }
    return n > 0 ? n : -1;
}
bool HttpGet(const std::string& address, std::vector<uint8_t>& body, std::string& error, std::atomic<bool>* cancel,
             size_t limit) {
    if (!limit || limit > 128U * 1024 * 1024) return Fail(error, "Invalid HTTP size limit");
    std::string current = address;
    for (unsigned redirects = 0; redirects <= 3; ++redirects) {
        Url url;
        if (!ParseUrl(current, url, error)) return false;
        if (url.scheme != "http" && url.scheme != "https") return Fail(error, "Expected HTTP or HTTPS");
        SocketStream stream;
        if (!stream.Connect(url, cancel, error)) return false;
        NetworkContext context{&stream};
        TransportInterface_t transport{pc::Read, pc::Write, nullptr, &context};
        std::array<uint8_t, 2048> headers{};
        HTTPRequestHeaders_t request{headers.data(), headers.size(), 0};
        const std::string host = HttpAuthority(url);
        HTTPRequestInfo_t info{
            "GET", 3, url.path.c_str(), url.path.size(), host.c_str(), host.size(), HTTP_REQUEST_NO_USER_AGENT_FLAG};
        if (HTTPClient_InitializeRequestHeaders(&request, &info) != HTTPSuccess ||
            HTTPClient_AddHeader(&request, "Connection", 10, "close", 5) != HTTPSuccess ||
            HTTPClient_AddHeader(&request, "Accept-Encoding", 15, "identity", 8) != HTTPSuccess)
            return Fail(error, "HTTP request header overflow");
        std::vector<uint8_t> response_bytes(limit + 16384);
        HTTPResponse_t response{};
        response.pBuffer = response_bytes.data();
        response.bufferLen = response_bytes.size();
        response.getTime = Millis;
        const auto status = HTTPClient_Send(&transport, &request, nullptr, 0, &response, 0);
        if (status != HTTPSuccess) {
            error = HTTPClient_strerror(status);
            return false;
        }
        if (response.statusCode >= 300 && response.statusCode < 400) {
            const char* location = nullptr;
            size_t size = 0;
            if (redirects == 3 || HTTPClient_ReadHeader(&response, "Location", 8, &location, &size) != HTTPSuccess)
                return Fail(error, "Invalid or excessive HTTP redirects");
            std::string next(location, size);
            if (next.starts_with('/')) next = url.scheme + "://" + host + next;
            if (url.tls && !next.starts_with("https://")) return Fail(error, "Refusing HTTPS redirect to plaintext");
            current = next;
            continue;
        }
        if (response.statusCode < 200 || response.statusCode >= 300) {
            error = "HTTP status " + std::to_string(response.statusCode);
            return false;
        }
        if (response.bodyLen > limit) return Fail(error, "HTTP body exceeds size limit");
        if (response.bodyLen)
            body.assign(response.pBody, response.pBody + response.bodyLen);
        else
            body.clear();
        return true;
    }
    return false;
}
bool HttpExchange(bool post, const std::string& address, const std::string& payload, std::span<const HttpHeader> extra,
                  HttpResponse& result, std::string& error, std::atomic<bool>* cancel, size_t limit,
                  uint32_t receive_timeout_ms) {
    result = {};
    if (receive_timeout_ms < 100 || receive_timeout_ms > 60000 || (!post && !payload.empty()) || !limit ||
        limit > 128U * 1024 * 1024 || payload.size() > 128U * 1024 || extra.size() > 16)
        return Fail(error, "Invalid HTTP POST size limit");
    Url url;
    if (!ParseUrl(address, url, error)) return false;
    if (url.scheme != "http" && url.scheme != "https") return Fail(error, "Expected HTTP or HTTPS");
    SocketStream stream;
    if (!stream.Connect(url, cancel, error)) return false;
    NetworkContext context{&stream};
    context.receive_timeout_ms = receive_timeout_ms;
    TransportInterface_t transport{pc::HttpRead, pc::Write, nullptr, &context};
    std::vector<uint8_t> headers(8192);
    HTTPRequestHeaders_t request{headers.data(), headers.size(), 0};
    const std::string host = HttpAuthority(url);
    HTTPRequestInfo_t info{
        post ? "POST" : "GET",          post ? 4U : 3U, url.path.c_str(), url.path.size(), host.c_str(), host.size(),
        HTTP_REQUEST_NO_USER_AGENT_FLAG};
    if (HTTPClient_InitializeRequestHeaders(&request, &info) != HTTPSuccess ||
        HTTPClient_AddHeader(&request, "Connection", 10, "close", 5) != HTTPSuccess ||
        HTTPClient_AddHeader(&request, "Accept-Encoding", 15, "identity", 8) != HTTPSuccess)
        return Fail(error, "HTTP request header overflow");
    for (const auto& [name, value] : extra) {
        auto lower = name;
        for (char& c : lower)
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (name.empty() ||
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") !=
                std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos || value.find('\0') != std::string::npos ||
            lower == "host" || lower == "content-length" || lower == "transfer-encoding" || lower == "connection" ||
            lower == "accept-encoding")
            return Fail(error, "Invalid HTTP request header");
        if (HTTPClient_AddHeader(&request, name.data(), name.size(), value.data(), value.size()) != HTTPSuccess)
            return Fail(error, "HTTP request header overflow");
    }
    std::vector<uint8_t> buffer(limit + 16384);
    HTTPResponse_t response{};
    response.pBuffer = buffer.data();
    response.bufferLen = buffer.size();
    response.getTime = Millis;
    auto status = HTTPClient_Send(&transport, &request, reinterpret_cast<const uint8_t*>(payload.data()),
                                  payload.size(), &response, 0);
    if (status != HTTPSuccess) {
        error = HTTPClient_strerror(status);
        return false;
    }
    if (response.headersLen > 8192 || response.bodyLen > limit) return Fail(error, "HTTP response exceeds size limit");
    result.status = response.statusCode;
    if (response.headersLen)
        result.headers.assign(reinterpret_cast<const char*>(response.pHeaders), response.headersLen);
    if (response.bodyLen) result.body.assign(response.pBody, response.pBody + response.bodyLen);
    return true;
}
bool HttpPost(const std::string& address, const std::string& payload, std::span<const HttpHeader> headers,
              std::vector<uint8_t>& body, std::string& error, std::atomic<bool>* cancel, size_t limit) {
    HttpResponse response;
    if (!HttpExchange(true, address, payload, headers, response, error, cancel, limit)) return false;
    if (response.status < 200 || response.status >= 300) {
        error = "HTTP status " + std::to_string(response.status);
        return false;
    }
    body = std::move(response.body);
    return true;
}
bool MqttRoundTrip(const std::string& address, const std::string& topic, const std::string& payload,
                   std::string& received, std::string& error, std::atomic<bool>* cancel) {
    if (topic.empty() || topic.size() > 128 || payload.size() > 4096 || topic.find_first_of("+#") != std::string::npos)
        return Fail(error, "Invalid MQTT topic or payload length");
    Url url;
    if (!ParseUrl(address, url, error)) return false;
    if (url.scheme != "mqtt" && url.scheme != "mqtts") return Fail(error, "Expected mqtt:// or mqtts://");
    SocketStream stream;
    if (!stream.Connect(url, cancel, error)) return false;
    NetworkContext state{&stream, &received};
    TransportInterface_t transport{pc::Read, pc::Write, nullptr, &state};
    std::array<uint8_t, 8192> bytes{};
    MQTTFixedBuffer_t buffer{bytes.data(), bytes.size()};
    MQTTContext_t context{};
    auto status = MQTT_Init(&context, &transport, Millis, MqttEvent, &buffer);
    if (status != MQTTSuccess) return Fail(error, "MQTT initialization failed");
    const std::string client = "micropixel-pc-" + std::to_string(getpid()) + "-" + std::to_string(Millis());
    MQTTConnectInfo_t info{};
    info.cleanSession = true;
    info.pClientIdentifier = client.c_str();
    info.clientIdentifierLength = client.size();
    info.keepAliveSeconds = 30;
    bool session = false;
    status = MQTT_Connect(&context, &info, nullptr, 5000, &session);
    if (status != MQTTSuccess) {
        error = MQTT_Status_strerror(status);
        return false;
    }
    MQTTSubscribeInfo_t subscription{};
    subscription.qos = MQTTQoS0;
    subscription.pTopicFilter = topic.c_str();
    subscription.topicFilterLength = topic.size();
    status = MQTT_Subscribe(&context, &subscription, 1, MQTT_GetPacketId(&context));
    const auto deadline = Clock::now() + std::chrono::seconds(8);
    while (status == MQTTSuccess && !state.subscribed && Clock::now() < deadline && !Cancelled(cancel))
        status = MQTT_ProcessLoop(&context);
    if (status == MQTTSuccess && state.subscribed) {
        MQTTPublishInfo_t message{};
        message.qos = MQTTQoS0;
        message.pTopicName = topic.c_str();
        message.topicNameLength = topic.size();
        message.pPayload = payload.data();
        message.payloadLength = payload.size();
        status = MQTT_Publish(&context, &message, 0);
    }
    while (status == MQTTSuccess && !state.published && Clock::now() < deadline && !Cancelled(cancel))
        status = MQTT_ProcessLoop(&context);
    (void)MQTT_Disconnect(&context);
    if (status != MQTTSuccess) {
        error = MQTT_Status_strerror(status);
        return false;
    }
    return (state.published && received == payload) || Fail(error, "MQTT echo timed out or differed from the payload");
}
}  // namespace micropixel::pc
