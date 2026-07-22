/*
 * Wing OSC Communication Implementation
 * Based on Patric-Gilles Maillot's Behringer Wing OSC Manual
 * 
 * This module implements the Behringer Wing OSC (Open Sound Control) protocol.
 * It handles:
 * 1. UDP/OSC client connection to Wing console
 * 2. Sending OSC queries to discover channel information
 * 3. Receiving and parsing OSC responses
 * 4. Real-time subscription to Wing updates
 * 5. Channel data caching and callbacks
 * 
 * Protocol Details:
 * - Transport: UDP/OSC (default port 2223)
 * - Handshake: Sends "WING?" probe on port 2222 to locate console
 * - Channel data: Queried via /ch/[number]/[property] OSC addresses
 * - USB routing: Calculated from channel allocation info
 * - Colors: Wing palette (48 colors) mapped to REAPER RGB
 * 
 * Key responsibilities:
 * - Establish and maintain connection to Wing console
 * - Build and send OSC query messages
 * - Parse and validate OSC responses
 * - Manage channel information cache
 * - Trigger callbacks when data is received
 */

#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include "wingconnector/wing_osc.h"
#include "internal/playback_allocator.h"
#include "internal/osc_builder.h"
#include "internal/playback_naming.h"
#include "internal/logger.h"
#include "internal/osc_helpers.h"
#include "internal/osc_routing.h"
#include "internal/source_input_mode_plan.h"
#include "internal/stereo_channel_plan.h"

#if defined(_WIN32)
#ifdef ReceivedPacket
#undef ReceivedPacket
#endif
#ifdef ReceivedBundle
#undef ReceivedBundle
#endif
#ifdef BeginMessage
#undef BeginMessage
#endif
#ifdef EndMessage
#undef EndMessage
#endif
#ifdef MessageTerminator
#undef MessageTerminator
#endif
#ifdef ReceivedMessage
#undef ReceivedMessage
#endif
#endif

#include "osc/OscReceivedElements.h"
#include "osc/OscPacketListener.h"
#include "osc/OscOutboundPacketStream.h"
#include "ip/UdpSocket.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <fstream>
#include <ctime>
#include <cstdio>
#include <cmath>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifdef ReceivedPacket
#undef ReceivedPacket
#endif
#ifdef ReceivedBundle
#undef ReceivedBundle
#endif
#ifdef ReceivedMessage
#undef ReceivedMessage
#endif
#ifdef BeginMessage
#undef BeginMessage
#endif
#ifdef EndMessage
#undef EndMessage
#endif
#endif

namespace {
constexpr uint16_t kWingHandshakePort = 2222;  // Wing discovery port
constexpr const char* kWingHandshakeProbe = "WING?";  // Discovery message
constexpr int kHandshakeTimeoutMs = 1500;  // Discovery timeout
constexpr int kBurstPollTimeoutMs = 20;
constexpr int kBurstTotalTimeoutMs = 150;
constexpr int kBurstIdleTimeoutMs = 25;
constexpr int kAsyncQuerySettlingMs = 120;

struct DirectOscReply {
    std::string address;
    bool has_string = false;
    std::string string_value;
    bool has_int = false;
    int int_value = 0;
};

bool TryParseEmbeddedInt(const std::string& value, int& parsed_out) {
    int parsed = 0;
    bool has_digit = false;
    for (char c : value) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            parsed = (parsed * 10) + (c - '0');
            has_digit = true;
        } else if (has_digit) {
            break;
        }
    }

    if (!has_digit) {
        return false;
    }

    parsed_out = parsed;
    return true;
}

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

bool SetSocketRecvTimeout(NativeSocket sock, int timeout_ms) {
#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(timeout_ms);
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0;
#else
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

void CloseNativeSocket(NativeSocket& sock) {
#if defined(_WIN32)
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
#else
    if (sock >= 0) {
        ::close(sock);
        sock = -1;
    }
#endif
}

bool CreateUdpQuerySocket(const std::string& wing_ip,
                          uint16_t wing_port,
                          NativeSocket& sock_out,
                          sockaddr_in& dest_out,
                          int recv_timeout_ms = kBurstPollTimeoutMs) {
#if defined(_WIN32)
    NativeSocket sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return false;
    }
#else
    NativeSocket sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return false;
    }
#endif

    if (!SetSocketRecvTimeout(sock, recv_timeout_ms)) {
        CloseNativeSocket(sock);
        return false;
    }

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(wing_port);
    if (inet_pton(AF_INET, wing_ip.c_str(), &dest.sin_addr) != 1) {
        CloseNativeSocket(sock);
        return false;
    }

    sock_out = sock;
    dest_out = dest;
    return true;
}

bool SendOscQueryPacket(NativeSocket sock,
                        const sockaddr_in& dest,
                        const std::string& address,
                        uint16_t reply_port = 0) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, sizeof(buffer));
    std::string routed_address = address;
    if (reply_port > 0) {
        routed_address = "/%" + std::to_string(reply_port) + address;
    }
    p << osc::BeginMessage(routed_address.c_str()) << osc::EndMessage;

    auto bytes_sent = sendto(sock, p.Data(), static_cast<int>(p.Size()), 0,
                             reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
#if defined(_WIN32)
    return bytes_sent != SOCKET_ERROR;
#else
    return bytes_sent >= 0;
#endif
}

bool TryReceiveDirectOscReply(NativeSocket sock, DirectOscReply& reply_out) {
    char recv_buffer[1024];
    sockaddr_in from{};
#if defined(_WIN32)
    int from_len = sizeof(from);
    int received = recvfrom(sock, recv_buffer, sizeof(recv_buffer), 0,
                            reinterpret_cast<sockaddr*>(&from), &from_len);
    if (received == SOCKET_ERROR) {
        return false;
    }
#else
    socklen_t from_len = sizeof(from);
    ssize_t received = recvfrom(sock, recv_buffer, sizeof(recv_buffer), 0,
                                reinterpret_cast<sockaddr*>(&from), &from_len);
    if (received < 0) {
        return false;
    }
#endif

    try {
        osc::ReceivedPacket packet(recv_buffer, static_cast<int>(received));
        if (!packet.IsMessage()) {
            return false;
        }
        osc::ReceivedMessage msg(packet);
        reply_out = DirectOscReply{};
        reply_out.address = msg.AddressPattern();
        for (auto arg = msg.ArgumentsBegin(); arg != msg.ArgumentsEnd(); ++arg) {
            if (!reply_out.has_string && arg->IsString()) {
                reply_out.has_string = true;
                reply_out.string_value = arg->AsString();
                continue;
            }
            if (!reply_out.has_int && arg->IsInt32()) {
                reply_out.has_int = true;
                reply_out.int_value = arg->AsInt32();
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::map<std::string, DirectOscReply> QueryOscAddressesDirectRaw(const std::string& wing_ip,
                                                                 uint16_t wing_port,
                                                                 const std::vector<std::string>& addresses,
                                                                 int total_timeout_ms = kBurstTotalTimeoutMs,
                                                                 int idle_timeout_ms = kBurstIdleTimeoutMs) {
    // Several extension monitors can query the desk from different threads.
    // Keep each temporary-port request/reply transaction intact so their
    // bursts do not compete for the WING's OSC response capacity.
    static std::mutex direct_query_mutex;
    std::lock_guard<std::mutex> query_lock(direct_query_mutex);

    std::map<std::string, DirectOscReply> replies;
    if (addresses.empty()) {
        return replies;
    }
    std::set<std::string> unique_addresses(addresses.begin(), addresses.end());
    unique_addresses.erase("");
    if (unique_addresses.empty()) {
        return replies;
    }

    NativeSocket sock = kInvalidSocket;
    sockaddr_in dest{};
    if (!CreateUdpQuerySocket(wing_ip, wing_port, sock, dest, kBurstPollTimeoutMs)) {
        return replies;
    }

    uint16_t local_port = 0;
    sockaddr_in local_addr{};
    socklen_t local_addr_len = sizeof(local_addr);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local_addr), &local_addr_len) == 0) {
        local_port = ntohs(local_addr.sin_port);
    }

    for (const auto& address : unique_addresses) {
        if (address.empty()) {
            continue;
        }
        SendOscQueryPacket(sock, dest, address, local_port);
    }

    auto receive_replies = [&]() {
        const auto started = std::chrono::steady_clock::now();
        auto last_reply = started;
        while (true) {
            DirectOscReply reply;
            if (TryReceiveDirectOscReply(sock, reply)) {
                replies[reply.address] = reply;
                last_reply = std::chrono::steady_clock::now();
                if (replies.size() >= unique_addresses.size()) {
                    break;
                }
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
            const auto idle_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reply).count();
            if (total_elapsed >= total_timeout_ms ||
                (!replies.empty() && idle_elapsed >= idle_timeout_ms)) {
                break;
            }
        }
    };

    receive_replies();

    // UDP can lose an individual reply even while the rest of a batch arrives.
    // Retry only the missing paths once, using the same reply port.
    if (replies.size() < unique_addresses.size()) {
        for (const auto& address : unique_addresses) {
            if (!address.empty() && replies.find(address) == replies.end()) {
                SendOscQueryPacket(sock, dest, address, local_port);
            }
        }
        receive_replies();
    }

    CloseNativeSocket(sock);
    return replies;
}

bool QueryInputModeDirectRaw(const std::string& wing_ip,
                             uint16_t wing_port,
                             const std::string& grp,
                             int input,
                             std::string& mode_out) {
    std::string address = "/io/in/" + grp + "/" + std::to_string(input) + "/mode";
    auto replies = QueryOscAddressesDirectRaw(wing_ip, wing_port, {address}, 120, 20);
    auto it = replies.find(address);
    if (it == replies.end() || !it->second.has_string) {
        return false;
    }
    mode_out = it->second.string_value;
    return true;
}

bool QueryStringAddressDirectRaw(const std::string& wing_ip,
                                 uint16_t wing_port,
                                 const std::string& address,
                                 std::string& value_out);

bool QueryInputNameDirectRaw(const std::string& wing_ip,
                             uint16_t wing_port,
                             const std::string& grp,
                             int input,
                             std::string& name_out) {
    std::string address = "/io/in/" + grp + "/" + std::to_string(input) + "/name";
    return QueryStringAddressDirectRaw(wing_ip, wing_port, address, name_out);
}

bool QueryStringAddressDirectRaw(const std::string& wing_ip,
                                 uint16_t wing_port,
                                 const std::string& address,
                                 std::string& value_out) {
    auto replies = QueryOscAddressesDirectRaw(wing_ip, wing_port, {address}, 120, 20);
    auto it = replies.find(address);
    if (it == replies.end() || !it->second.has_string) {
        return false;
    }
    value_out = it->second.string_value;
    return true;
}

bool IsDirectOutputSourceGroup(const std::string& grp) {
    static const std::set<std::string> groups = {
        "LCL", "AUX", "A", "B", "C", "SC", "USB", "CRD", "CARD", "MOD", "REC", "AES", "USR",
        "BUS", "MAIN", "MTX", "SEND", "MON"
    };
    return groups.count(grp) > 0;
}

bool IsDirectInputQueryGroup(const std::string& grp) {
    static const std::set<std::string> groups = {
        "A", "LCL", "USR", "$BUS", "$MAIN", "$MTX", "$SEND", "$MON"
    };
    return groups.count(grp) > 0;
}

std::string NormalizeDeskSourceGroup(const std::string& grp) {
    if (grp == "CARD") {
        return "CRD";
    }
    return grp;
}
}

namespace WingConnector {

static inline osc::BeginMessage MakeOscBeginToken(const char* address) {
    return osc::BeginMessage(address);
}

static inline osc::MessageTerminator MakeOscEndToken() {
    return {};
}

/**
 * WingOscListener - Internal OSC packet receiver
 * 
 * Implements osc::OscPacketListener to receive and process incoming OSC messages
 * from the Wing console. Routes messages to appropriate handlers based on address.
 */
class WingOscListener : public osc::OscPacketListener {
public:
    WingOscListener(WingOSC* parent) : parent_(parent) {}
    
protected:
    /**
     * ProcessMessage() - Handle incoming OSC message
     * 
     * Called by oscpack when an OSC message arrives.
     * Uses OscRouter to classify the message address and dispatch to handlers.
     * 
     * Args:
     *   m              - The received OSC message
     *   remoteEndpoint - Source IP/port (unused)
     */
    void ProcessMessage(const osc::ReceivedMessage& m,
                       const IpEndpointName& /* remoteEndpoint */) override {
        try {
            std::string address = m.AddressPattern();
            
            // Use efficient router to classify and handle messages
            switch (OscRouter::ClassifyAddress(address)) {
                case OscRouter::AddressType::CHANNEL:
                case OscRouter::AddressType::USB_INPUT:
                case OscRouter::AddressType::ANALOG_INPUT:
                case OscRouter::AddressType::DIGITAL_INPUT:
                    parent_->HandleOscMessage(address, &m, 0);
                    break;
                    
                case OscRouter::AddressType::CONSOLE_INFO:
                    parent_->Log("Wing console info received");
                    break;
                    
                case OscRouter::AddressType::UNKNOWN:
                    // Silently ignore unknown addresses
                    break;
            }
        }
        catch (osc::Exception& e) {
            parent_->Log(std::string("OSC parsing error: ") + e.what());
        }
    }
    
private:
    WingOSC* parent_;
};

/**
 * WingOSC Constructor
 * 
 * Initializes OSC client parameters but does not connect.
 * Call Start() to begin listening.
 * 
 * Args:
 *   wing_ip    - IP address of Behringer Wing console
 *   wing_port  - OSC port on Wing (default 2223)
 *   listen_port - Local port to listen on (typically same as wing_port)
 */
WingOSC::WingOSC(const std::string& wing_ip, uint16_t wing_port, uint16_t listen_port)
    : wing_ip_(wing_ip)
    , wing_port_(wing_port)
    , listen_port_(listen_port)
    , running_(false)
    , osc_socket_(nullptr)
    , osc_listener_(nullptr)
{
}

/**
 * WingOSC Destructor
 * Ensures connection is closed and resources freed
 */
WingOSC::~WingOSC() {
    Stop();
}

/**
 * Start() - Begin listening for OSC messages
 * 
 * Creates UDP socket and listener thread. If already running, returns true.
 * 
 * Returns: true if successful, false on error
 */
bool WingOSC::Start() {
    if (running_) {
        return true;
    }
    
    try {
        last_connection_diagnostic_.clear();
        // Create UDP socket for listening
        IpEndpointName endpoint(IpEndpointName::ANY_ADDRESS, listen_port_);
        osc_listener_ = new WingOscListener(this);
        osc_socket_ = new UdpListeningReceiveSocket(endpoint, osc_listener_);
        
        // Start listener thread
        running_ = true;
        listener_thread_ = std::make_unique<std::thread>(&WingOSC::ListenerThread, this);
        
        Log("Wing OSC: Listening on port " + std::to_string(listen_port_));
        return true;
    }
    catch (std::exception& e) {
        last_connection_diagnostic_ =
            "Failed to start OSC listener on local port " + std::to_string(listen_port_) +
            " (" + std::string(e.what()) + ").";
        Log(std::string("Failed to start OSC server: ") + e.what());
        return false;
    }
}

void WingOSC::Stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    // Stop listener
    if (osc_socket_) {
        osc_socket_->AsynchronousBreak();
    }
    
    // Wait for thread
    if (listener_thread_ && listener_thread_->joinable()) {
        listener_thread_->join();
    }

    if (osc_socket_) {
        delete osc_socket_;
        osc_socket_ = nullptr;
    }
    if (osc_listener_) {
        delete osc_listener_;
        osc_listener_ = nullptr;
    }
    
    Log("Wing OSC: Stopped");
}

void WingOSC::ListenerThread() {
    if (!osc_socket_) {
        return;
    }
    try {
        osc_socket_->Run();
    }
    catch (std::exception& e) {
        Log(std::string("OSC listener error: ") + e.what());
    }
}

bool WingOSC::TestConnection() {
    try {
        last_connection_diagnostic_.clear();
        if (!PerformHandshake()) {
            if (last_connection_diagnostic_.empty()) {
                last_connection_diagnostic_ = "Wing handshake failed before OSC queries could start.";
            }
            Log("Wing handshake failed");
            return false;
        }

        char buffer[256];
        osc::OutboundPacketStream p(buffer, 256);
        
        // Send info request
        p << MakeOscBeginToken("/xinfo")
          << MakeOscEndToken();

        if (!SendRawPacket(p.Data(), p.Size())) {
            last_connection_diagnostic_ =
                "Wing handshake succeeded, but sending the first OSC info probe to " + wing_ip_ +
                ":" + std::to_string(wing_port_) + " failed.";
            Log("Failed to send Wing info probe");
            return false;
        }
        
        last_connection_diagnostic_.clear();
        Log("Wing OSC info probe sent to " + wing_ip_ + ":" + std::to_string(wing_port_));
        return true;
    }
    catch (std::exception& e) {
        last_connection_diagnostic_ = "Connection test failed while preparing the first OSC probe (" + std::string(e.what()) + ").";
        Log(std::string("Connection test failed: ") + e.what());
        return false;
    }
}

std::string WingOSC::FormatChannelNum(int num) {
    // Wing uses plain integer channel numbers (e.g. 1..48), not zero-padded
    return std::to_string(num);
}

bool WingOSC::PerformHandshake() {
    if (handshake_complete_) {
        return true;
    }

    last_connection_diagnostic_.clear();
    Log("Wing handshake: probing " + wing_ip_ + ":" + std::to_string(kWingHandshakePort));

#if defined(_WIN32)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        last_connection_diagnostic_ = "Windows socket initialization failed before the Wing handshake started.";
        Log("WSAStartup failed for Wing handshake");
        return false;
    }
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        last_connection_diagnostic_ = "Failed to open the UDP socket used for the Wing handshake probe.";
        Log("Failed to open Wing handshake socket");
        WSACleanup();
        return false;
    }
#else
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        last_connection_diagnostic_ = "Failed to open the UDP socket used for the Wing handshake probe.";
        Log("Failed to open Wing handshake socket");
        return false;
    }
#endif

    auto closeSocket = [&]() {
#if defined(_WIN32)
        if (sock != INVALID_SOCKET) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
        WSACleanup();
#else
        if (sock >= 0) {
            ::close(sock);
            sock = -1;
        }
#endif
    };

#if defined(_WIN32)
    DWORD timeout = kHandshakeTimeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv{};
    tv.tv_sec = kHandshakeTimeoutMs / 1000;
    tv.tv_usec = (kHandshakeTimeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(kWingHandshakePort);
    if (inet_pton(AF_INET, wing_ip_.c_str(), &dest.sin_addr) != 1) {
        last_connection_diagnostic_ = "The configured Wing IP address is invalid: " + wing_ip_ + ".";
        Log("Invalid Wing IP address");
        closeSocket();
        return false;
    }

    const size_t payload_len = std::strlen(kWingHandshakeProbe);
    auto bytes_sent = sendto(sock, kWingHandshakeProbe, payload_len, 0,
                             reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
#if defined(_WIN32)
    if (bytes_sent == SOCKET_ERROR) {
        last_connection_diagnostic_ =
            "Failed to send the UDP handshake probe to " + wing_ip_ + ":" + std::to_string(kWingHandshakePort) + ".";
        Log("Failed to send Wing handshake probe");
        closeSocket();
        return false;
    }
#else
    if (bytes_sent < 0) {
        last_connection_diagnostic_ =
            "Failed to send the UDP handshake probe to " + wing_ip_ + ":" + std::to_string(kWingHandshakePort) + ".";
        Log("Failed to send Wing handshake probe");
        closeSocket();
        return false;
    }
#endif

    char buffer[512];
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
#if defined(_WIN32)
    int received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                            reinterpret_cast<sockaddr*>(&from), &from_len);
    if (received == SOCKET_ERROR) {
        last_connection_diagnostic_ =
            "Timed out waiting for the Wing handshake reply on UDP port " + std::to_string(kWingHandshakePort) + ".";
        Log("Wing handshake timed out");
        closeSocket();
        return false;
    }
#else
    ssize_t received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                                reinterpret_cast<sockaddr*>(&from), &from_len);
    if (received <= 0) {
        last_connection_diagnostic_ =
            "Timed out waiting for the Wing handshake reply on UDP port " + std::to_string(kWingHandshakePort) + ".";
        Log("Wing handshake timed out");
        closeSocket();
        return false;
    }
#endif

    buffer[received] = '\0';
    std::string response(buffer);
    Log("Wing handshake raw response: " + response);
    response.erase(std::remove(response.begin(), response.end(), '\r'), response.end());
    response.erase(std::remove(response.begin(), response.end(), '\n'), response.end());

    if (response.rfind("WING", 0) != 0) {
        last_connection_diagnostic_ = "Received an unexpected handshake reply from " + wing_ip_ + ": " + response;
        Log("Unexpected Wing handshake response: " + response);
        closeSocket();
        return false;
    }

    std::vector<std::string> tokens;
    std::stringstream ss(response);
    std::string token;
    while (std::getline(ss, token, ',')) {
        tokens.push_back(token);
    }

    if (tokens.size() >= 2) {
        wing_info_.console_ip = tokens[1];
    }
    if (tokens.size() >= 3) {
        wing_info_.name = tokens[2];
    }
    if (tokens.size() >= 4) {
        wing_info_.model = tokens[3];
    }
    if (tokens.size() >= 5) {
        wing_info_.serial = tokens[4];
    }
    if (tokens.size() >= 6) {
        wing_info_.firmware = tokens[5];
    }

    handshake_complete_ = true;
    last_connection_diagnostic_.clear();
    std::string log_msg = "Wing handshake OK";
    if (!wing_info_.model.empty()) {
        log_msg += ": " + wing_info_.model;
    }
    if (!wing_info_.name.empty()) {
        log_msg += " (" + wing_info_.name + ")";
    }
    if (!wing_info_.firmware.empty()) {
        log_msg += " FW " + wing_info_.firmware;
    }
    Log(log_msg);

    closeSocket();
    return true;
}

std::vector<WingInfo> WingOSC::DiscoverWings(int timeout_ms) {
    std::vector<WingInfo> results;

#if defined(_WIN32)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return results;
    }
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return results;
    }
#else
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return results;
    }
#endif

    auto closeSocket = [&]() {
#if defined(_WIN32)
        if (sock != INVALID_SOCKET) { closesocket(sock); sock = INVALID_SOCKET; }
        WSACleanup();
#else
        if (sock >= 0) { ::close(sock); sock = -1; }
#endif
    };

    // Enable broadcast
    int broadcast_flag = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&broadcast_flag), sizeof(broadcast_flag));

    // Short per-call recvfrom timeout so we can loop collecting multiple responses
    constexpr int kPollMs = 150;
#if defined(_WIN32)
    DWORD tv_val = kPollMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv_val), sizeof(tv_val));
#else
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = kPollMs * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // Broadcast "WING?" to port 2222
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(kWingHandshakePort);
    inet_pton(AF_INET, "255.255.255.255", &dest.sin_addr);
    sendto(sock, kWingHandshakeProbe, std::strlen(kWingHandshakeProbe), 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

    // Collect responses until the total timeout expires
    auto start = std::chrono::steady_clock::now();
    std::set<std::string> seen_ips;

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            break;
        }

        char buffer[512];
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
#if defined(_WIN32)
        int received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                                reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received == SOCKET_ERROR) { continue; }
#else
        ssize_t received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                                    reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received <= 0) { continue; }
#endif

        buffer[received] = '\0';
        std::string response(buffer);
        response.erase(std::remove(response.begin(), response.end(), '\r'), response.end());
        response.erase(std::remove(response.begin(), response.end(), '\n'), response.end());

        if (response.rfind("WING", 0) != 0) {
            continue;
        }

        // Use actual source IP from the UDP packet (more reliable than the reported one)
        char src_ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, src_ip, sizeof(src_ip));
        std::string ip_str(src_ip);

        if (seen_ips.count(ip_str)) {
            continue;  // Duplicate response from same device
        }
        seen_ips.insert(ip_str);

        // Parse: WING,<console_ip>,<name>,<model>,<serial>,<firmware>
        std::vector<std::string> tokens;
        std::istringstream ss(response);
        std::string token;
        while (std::getline(ss, token, ',')) {
            tokens.push_back(token);
        }

        WingInfo info;
        info.console_ip = ip_str;
        if (tokens.size() >= 3) info.name     = tokens[2];
        if (tokens.size() >= 4) info.model    = tokens[3];
        if (tokens.size() >= 5) info.serial   = tokens[4];
        if (tokens.size() >= 6) info.firmware = tokens[5];

        results.push_back(info);
    }

    closeSocket();
    return results;
}

bool WingOSC::SendRawPacket(const char* data, std::size_t size) {
    if (!data || size == 0) {
        return false;
    }
    auto* socket = osc_socket_;
    if (!socket) {
        Log("Wing OSC: Socket not initialized for sending");
        return false;
    }
    std::lock_guard<std::mutex> lock(send_mutex_);
    try {
        IpEndpointName dest(wing_ip_.c_str(), wing_port_);
        socket->SendTo(dest, data, size);
        return true;
    }
    catch (std::exception& e) {
        Log(std::string("Wing OSC send error: ") + e.what());
        return false;
    }
}

std::map<std::string, std::string> WingOSC::QueryStringAddressesDirect(const std::vector<std::string>& addresses,
                                                                       int total_timeout_ms,
                                                                       int idle_timeout_ms) const {
    std::map<std::string, std::string> values;
    auto replies = QueryOscAddressesDirectRaw(wing_ip_, wing_port_, addresses, total_timeout_ms, idle_timeout_ms);
    for (const auto& [address, reply] : replies) {
        if (reply.has_string) {
            values[address] = reply.string_value;
        }
    }
    return values;
}

std::map<std::string, int> WingOSC::QueryIntAddressesDirect(const std::vector<std::string>& addresses,
                                                            int total_timeout_ms,
                                                            int idle_timeout_ms) const {
    std::map<std::string, int> values;
    auto replies = QueryOscAddressesDirectRaw(wing_ip_, wing_port_, addresses, total_timeout_ms, idle_timeout_ms);
    for (const auto& [address, reply] : replies) {
        const bool prefer_native_int =
            (address.size() >= 4 && address.compare(address.size() - 4, 4, "/col") == 0) ||
            (address.size() >= 6 && address.compare(address.size() - 6, 6, "/color") == 0);
        if (prefer_native_int && reply.has_int) {
            values[address] = reply.int_value;
            continue;
        }

        int parsed = 0;
        if (reply.has_string && TryParseEmbeddedInt(reply.string_value, parsed)) {
            values[address] = parsed;
            continue;
        }

        if (reply.has_int) {
            values[address] = reply.int_value;
        }
    }
    return values;
}

bool WingOSC::GetSelectedStripIndex(int& strip_index_one_based) const {
    static const std::string kSelectedStripAddress = "/$ctl/$stat/selidx";
    const auto values = QueryIntAddressesDirect({kSelectedStripAddress}, 120, 20);
    auto it = values.find(kSelectedStripAddress);
    if (it == values.end()) {
        return false;
    }

    // Live desk validation and Patrick-Gilles Maillot's WING protocol notes
    // show that reads return the 0-based strip id even though writes use 1..76.
    strip_index_one_based = it->second + 1;
    return true;
}

std::map<int, ManagedChannelInputState> WingOSC::QueryManagedChannelInputStatesDirect(const std::vector<int>& channel_numbers) const {
    std::map<int, ManagedChannelInputState> states;
    if (channel_numbers.empty()) {
        return states;
    }

    std::vector<std::string> string_addresses;
    std::vector<std::string> int_addresses;
    string_addresses.reserve(channel_numbers.size());
    int_addresses.reserve(channel_numbers.size());
    for (int channel_number : channel_numbers) {
        if (channel_number <= 0) {
            continue;
        }
        const std::string ch = FormatChannelNum(channel_number);
        states[channel_number].channel_number = channel_number;
        string_addresses.push_back("/ch/" + ch + "/in/conn/grp");
        int_addresses.push_back("/ch/" + ch + "/in/conn/in");
    }

    const auto groups = QueryStringAddressesDirect(string_addresses, 140, 20);
    const auto inputs = QueryIntAddressesDirect(int_addresses, 140, 20);
    std::vector<std::string> mode_addresses;
    std::map<int, std::string> channel_mode_paths;
    for (int channel_number : channel_numbers) {
        const std::string ch = FormatChannelNum(channel_number);
        const auto group_it = groups.find("/ch/" + ch + "/in/conn/grp");
        const auto input_it = inputs.find("/ch/" + ch + "/in/conn/in");
        if (group_it == groups.end() || input_it == inputs.end() ||
            group_it->second.empty() || group_it->second == "OFF" || input_it->second <= 0) {
            continue;
        }
        const std::string mode_path = "/io/in/" + group_it->second + "/" +
                                      std::to_string(input_it->second) + "/mode";
        channel_mode_paths[channel_number] = mode_path;
        mode_addresses.push_back(mode_path);
    }
    const auto modes = QueryStringAddressesDirect(mode_addresses, 180, 30);
    for (int channel_number : channel_numbers) {
        if (channel_number <= 0) {
            continue;
        }
        ManagedChannelInputState state;
        state.channel_number = channel_number;

        const std::string ch = FormatChannelNum(channel_number);
        const std::string group_path = "/ch/" + ch + "/in/conn/grp";
        const std::string input_path = "/ch/" + ch + "/in/conn/in";
        auto group_it = groups.find(group_path);
        auto input_it = inputs.find(input_path);
        if (group_it == groups.end() || input_it == inputs.end()) {
            states[channel_number] = state;
            continue;
        }

        state.source_group = group_it->second;
        state.source_input = input_it->second;
        state.readable = true;

        auto mode_path_it = channel_mode_paths.find(channel_number);
        if (mode_path_it != channel_mode_paths.end()) {
            auto mode_it = modes.find(mode_path_it->second);
            if (mode_it != modes.end()) {
                state.stereo_linked = (mode_it->second == "ST" || mode_it->second == "MS");
                state.stereo_readable = true;
            }
        }

        states[channel_number] = state;
    }

    return states;
}

std::map<int, ManagedChannelDisplayState> WingOSC::QueryManagedChannelDisplayStatesDirect(const std::vector<int>& channel_numbers) const {
    std::map<int, ManagedChannelDisplayState> states;
    if (channel_numbers.empty()) {
        return states;
    }

    std::vector<std::string> name_addresses;
    std::vector<std::string> color_addresses;
    std::vector<std::string> active_name_addresses;
    std::vector<std::string> active_color_addresses;
    std::vector<std::string> link_addresses;
    name_addresses.reserve(channel_numbers.size());
    color_addresses.reserve(channel_numbers.size());
    active_name_addresses.reserve(channel_numbers.size());
    active_color_addresses.reserve(channel_numbers.size());
    link_addresses.reserve(channel_numbers.size());
    for (int channel_number : channel_numbers) {
        if (channel_number <= 0) {
            continue;
        }
        const std::string ch = FormatChannelNum(channel_number);
        states[channel_number].channel_number = channel_number;
        name_addresses.push_back("/ch/" + ch + "/name");
        color_addresses.push_back("/ch/" + ch + "/col");
        active_name_addresses.push_back("/ch/" + ch + "/$name");
        active_color_addresses.push_back("/ch/" + ch + "/$col");
        link_addresses.push_back("/ch/" + ch + "/clink");
    }

    const auto name_values = QueryStringAddressesDirect(name_addresses, 120, 20);
    const auto active_name_values = QueryStringAddressesDirect(active_name_addresses, 120, 20);
    const auto color_values = QueryIntAddressesDirect(color_addresses, 120, 20);
    const auto active_color_values = QueryIntAddressesDirect(active_color_addresses, 120, 20);
    const auto link_values = QueryIntAddressesDirect(link_addresses, 120, 20);

    for (int channel_number : channel_numbers) {
        if (channel_number <= 0) {
            continue;
        }

        ManagedChannelDisplayState state;
        state.channel_number = channel_number;

        const std::string ch = FormatChannelNum(channel_number);
        const std::string name_path = "/ch/" + ch + "/name";
        const std::string color_path = "/ch/" + ch + "/col";
        const std::string active_name_path = "/ch/" + ch + "/$name";
        const std::string active_color_path = "/ch/" + ch + "/$col";
        const std::string link_path = "/ch/" + ch + "/clink";

        auto name_it = name_values.find(name_path);
        if (name_it != name_values.end()) {
            state.name = name_it->second;
        }

        auto color_it = color_values.find(color_path);
        if (color_it != color_values.end()) {
            state.color_id = color_it->second;
        }

        auto link_it = link_values.find(link_path);
        if (link_it != link_values.end()) {
            state.customization_linked = (link_it->second != 0);
        }

        if (state.customization_linked) {
            auto active_name_it = active_name_values.find(active_name_path);
            if (active_name_it != active_name_values.end() && !active_name_it->second.empty()) {
                state.name = active_name_it->second;
            }

            auto active_color_it = active_color_values.find(active_color_path);
            if (active_color_it != active_color_values.end()) {
                state.color_id = active_color_it->second;
            }
        }

        state.readable = !state.name.empty() || state.color_id >= 0;
        states[channel_number] = state;
    }

    return states;
}

void WingOSC::SendQueryBurst(const std::vector<std::string>& addresses) {
    for (const auto& address : addresses) {
        if (address.empty()) {
            continue;
        }
        char buffer[256];
        osc::OutboundPacketStream p(buffer, sizeof(buffer));
        p << MakeOscBeginToken(address.c_str()) << MakeOscEndToken();
        SendRawPacket(p.Data(), p.Size());
    }
}

void WingOSC::Log(const std::string& message) const {
    // Delegate to unified logger
    Logger::Debug("%s", message.c_str());
}

// Wing OSC Commands based on Patrick-Gilles Maillot's manual
void WingOSC::GetChannelName(int channel_num) {
    std::string ch = FormatChannelNum(channel_num);
    std::string address = "/ch/" + ch + "/name";
    SendOscMessage(address.c_str(), [this](const char* data, std::size_t size) {
        return SendRawPacket(data, size);
    });
}

void WingOSC::SetChannelName(int channel_num, const std::string& name) {
    if (channel_num <= 0) {
        return;
    }
    char buffer[256];
    osc::OutboundPacketStream p(buffer, sizeof(buffer));
    const std::string address = "/ch/" + FormatChannelNum(channel_num) + "/name";
    p << MakeOscBeginToken(address.c_str())
      << name.c_str()
      << MakeOscEndToken();
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set channel " + std::to_string(channel_num) + " name");
        return;
    }
    Log("[OSC] " + address + " = " + name);
}

void WingOSC::SetChannelColor(int channel_num, int color_index) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);

    const std::string ch = FormatChannelNum(channel_num);
    const std::string address = "/ch/" + ch + "/col";
    p << MakeOscBeginToken(address.c_str())
      << (int32_t)color_index
      << MakeOscEndToken();

    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("Error setting channel color for channel " + std::to_string(channel_num));
        return;
    }
    Log("Set channel " + std::to_string(channel_num) + " color to " + std::to_string(color_index));
}

void WingOSC::SetChannelCustomizationLinked(int channel_num, bool enable) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);

    const std::string ch = FormatChannelNum(channel_num);
    const std::string address = "/ch/" + ch + "/clink";
    p << MakeOscBeginToken(address.c_str())
      << (int32_t)(enable ? 1 : 0)
      << MakeOscEndToken();

    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("Error setting channel customization link for channel " + std::to_string(channel_num));
        return;
    }
    Log("Set channel " + std::to_string(channel_num) + " customization link to " + std::string(enable ? "on" : "off"));
}

void WingOSC::GetChannelColor(int channel_num) {
    std::string ch = FormatChannelNum(channel_num);
    std::string address = "/ch/" + ch + "/col";
    SendOscMessage(address.c_str(), [this](const char* data, std::size_t size) {
        return SendRawPacket(data, size);
    });
}

void WingOSC::GetChannelIcon(int channel_num) {
    std::string ch = FormatChannelNum(channel_num);
    std::string address = "/ch/" + ch + "/icon";
    SendOscMessage(address.c_str(), [this](const char* data, std::size_t size) {
        return SendRawPacket(data, size);
    });
}

void WingOSC::GetChannelScribbleColor(int channel_num) {
    // Wing does not expose a separate scribble color path;
    // channel color (col) serves the same purpose - skip.
    (void)channel_num;
}

// Channel routing queries for virtual soundcheck
void WingOSC::GetChannelSourceRouting(int channel_num) {
    std::string ch = FormatChannelNum(channel_num);
    
    // Query primary source: /ch/N/in/conn/grp
    std::string addr_grp = "/ch/" + ch + "/in/conn/grp";
    SendOscMessage(addr_grp.c_str(), [this](const char* data, std::size_t size) {
        return SendRawPacket(data, size);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    
    // Query primary source: /ch/N/in/conn/in
    std::string addr_in = "/ch/" + ch + "/in/conn/in";
    SendOscMessage(addr_in.c_str(), [this](const char* data, std::size_t size) {
        return SendRawPacket(data, size);
    });
}

void WingOSC::GetChannelAltRouting(int channel_num) {
    std::string ch = FormatChannelNum(channel_num);
    
    // Query ALT source: /ch/N/in/conn/altgrp
    std::string addr_altgrp = "/ch/" + ch + "/in/conn/altgrp";
    SendOscMessage(addr_altgrp.c_str(), [this](const char* data, std::size_t size) {
        return SendRawPacket(data, size);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    
    // Query ALT source: /ch/N/in/conn/altin
    std::string addr_altin = "/ch/" + ch + "/in/conn/altin";
    SendOscMessage(addr_altin.c_str(), [this](const char* data, std::size_t size) {
        return SendRawPacket(data, size);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    
    // Query ALT source: /ch/N/in/set/altsrc
    std::string addr_altsrc = "/ch/" + ch + "/in/set/altsrc";
    SendOscMessage(addr_altsrc.c_str(), [this](const char* data, std::size_t size) {
        return SendRawPacket(data, size);
    });
}

void WingOSC::GetChannelStereoLink(int channel_num) {
    // Legacy: queries /ch/N/clink which only reflects channel link, not source stereo.
    // Use QueryChannelSourceStereo for accurate source-based stereo detection.
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    std::string address = "/ch/" + FormatChannelNum(channel_num) + "/clink";
    p << MakeOscBeginToken(address.c_str()) << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());
}

void WingOSC::QueryChannelSourceStereo(int channel_num) {
    // Queries /io/in/{grp}/{num}/mode to get true source stereo status.
    // Returns "M" (mono), "ST" (stereo), or "MS" (mid-side).
    // Can be called only after source routing has been queried.
    std::string grp;
    int input = 0;
    std::string raw_grp;
    int raw_input = 0;
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        auto it = channel_data_.find(channel_num);
        if (it == channel_data_.end()) return;
        raw_grp = it->second.primary_source_group;
        raw_input = it->second.primary_source_input;
        auto resolved = ResolveRoutingChainLocked(it->second.primary_source_group,
                                                  it->second.primary_source_input);
        grp = resolved.first;
        input = resolved.second;
    }
    
    if (raw_grp.empty() || raw_grp == "OFF" || raw_input <= 0) {
        Log("CH" + std::to_string(channel_num) + ": no source, skipping stereo mode query");
        return;
    }

    std::string mode;
    if (!QueryInputModeDirectRaw(wing_ip_, wing_port_, grp, input, mode)) {
        if (!QueryInputModeDirectRaw(wing_ip_, wing_port_, raw_grp, raw_input, mode)) {
            Log("CH" + std::to_string(channel_num) + ": stereo mode query failed for " +
                grp + ":" + std::to_string(input) + " and fallback " +
                raw_grp + ":" + std::to_string(raw_input));
            return;
        }
    }

    const bool is_stereo = (mode == "ST" || mode == "MS");
    std::lock_guard<std::mutex> lock(data_mutex_);
    auto it = channel_data_.find(channel_num);
    if (it != channel_data_.end()) {
        it->second.stereo_linked = is_stereo;
    }
}

// Channel routing configuration for virtual soundcheck
void WingOSC::SetChannelPrimarySource(int channel_num, const std::string& grp, int in) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);

    std::string ch = FormatChannelNum(channel_num);
    const std::string normalized_grp = NormalizeDeskSourceGroup(grp);

    std::string addr_grp = "/ch/" + ch + "/in/conn/grp";
    p.Clear();
    p << MakeOscBeginToken(addr_grp.c_str())
      << normalized_grp.c_str()
      << MakeOscEndToken();
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("Error setting primary source group for channel " + std::to_string(channel_num));
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::string addr_in = "/ch/" + ch + "/in/conn/in";
    p.Clear();
    p << MakeOscBeginToken(addr_in.c_str())
      << (int32_t)in
      << MakeOscEndToken();
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("Error setting primary source input for channel " + std::to_string(channel_num));
        return;
    }

    Log("Set channel " + std::to_string(channel_num) + " primary source to " + normalized_grp + " " + std::to_string(in));
}

void WingOSC::SetChannelAltSource(int channel_num, const std::string& grp, int in) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    std::string ch = FormatChannelNum(channel_num);
    const std::string normalized_grp = NormalizeDeskSourceGroup(grp);
    
    // Set ALT group: /ch/N/in/conn/altgrp "USB"
    std::string addr_altgrp = "/ch/" + ch + "/in/conn/altgrp";
    p.Clear();
    p << MakeOscBeginToken(addr_altgrp.c_str())
      << normalized_grp.c_str()
      << MakeOscEndToken();
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("Error setting ALT group for channel " + std::to_string(channel_num));
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    
    // Set ALT input: /ch/N/in/conn/altin <int>
    std::string addr_altin = "/ch/" + ch + "/in/conn/altin";
    p.Clear();
    p << MakeOscBeginToken(addr_altin.c_str())
      << (int32_t)in
      << MakeOscEndToken();
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("Error setting ALT input for channel " + std::to_string(channel_num));
    }
    
    Log("Set channel " + std::to_string(channel_num) + " ALT source to " + normalized_grp + " " + std::to_string(in));
}

void WingOSC::EnableChannelAltSource(int channel_num, bool enable) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    std::string ch = FormatChannelNum(channel_num);
    std::string address = "/ch/" + ch + "/in/set/altsrc";
    
    p << MakeOscBeginToken(address.c_str())
      << (int32_t)(enable ? 1 : 0)
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("Error toggling ALT source for channel " + std::to_string(channel_num));
    } else {
        Log("Channel " + std::to_string(channel_num) + " ALT " + (enable ? "enabled" : "disabled"));
    }
}

void WingOSC::SetUSBOutputSource(int usb_num, const std::string& grp, int in) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    const std::string normalized_grp = NormalizeDeskSourceGroup(grp);
    
    Log("[OSC] SetUSBOutputSource: USB OUTPUT " + std::to_string(usb_num) + 
        " routing to " + normalized_grp + ":" + std::to_string(in));
    
    // Correct USB output routing paths discovered via OSC queries:
    // /io/out/USB/[1-48]/grp - source group (LCL, AUX, A, B, C, SC, USB, CRD, MOD, REC, AES)
    // /io/out/USB/[1-48]/in  - input number within that group (1-40)
    
    // Set USB output source group
    std::string addr_grp = "/io/out/USB/" + std::to_string(usb_num) + "/grp";
    p.Clear();
    p << MakeOscBeginToken(addr_grp.c_str())
      << normalized_grp.c_str()
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set USB " + std::to_string(usb_num) + " group");
        return;
    }
    Log("[OSC] " + addr_grp + " = " + normalized_grp);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    
    // Set USB output input number
    std::string addr_in = "/io/out/USB/" + std::to_string(usb_num) + "/in";
    p.Clear();
    p << MakeOscBeginToken(addr_in.c_str())
      << (int32_t)in
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set USB " + std::to_string(usb_num) + " input");
        return;
    }
    Log("[OSC] " + addr_in + " = " + std::to_string(in));
}

void WingOSC::SetUSBOutputName(int usb_num, const std::string& name) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    Log("[OSC] SetUSBOutputName: USB OUTPUT " + std::to_string(usb_num) + 
        " name = '" + name + "'");
    
    // USB output naming is address-format sensitive on the desk. Use the
    // canonical zero-padded path form.
    std::string addr_name = OscBuilder::UsbPath(usb_num, "out", "name");
    p.Clear();
    p << MakeOscBeginToken(addr_name.c_str())
      << name.c_str()
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set USB output " + std::to_string(usb_num) + " name");
        return;
    }
    Log("[OSC] " + addr_name + " = " + name);
}

void WingOSC::SetUSBInputName(int usb_num, const std::string& name) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    Log("[OSC] SetUSBInputName: USB INPUT " + std::to_string(usb_num) + 
        " name = '" + name + "'");
    
    // Set USB input name: /io/in/USB/[1-48]/name
    std::string addr_name = "/io/in/USB/" + std::to_string(usb_num) + "/name";
    p.Clear();
    p << MakeOscBeginToken(addr_name.c_str())
      << name.c_str()
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set USB " + std::to_string(usb_num) + " name");
        return;
    }
    Log("[OSC] " + addr_name + " = " + name);
}

void WingOSC::UnlockUSBOutputs() {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    // Unlock USB recording outputs: /usbrec/lock 0
    p << MakeOscBeginToken("/usbrec/lock")
      << (int32_t)0  // 0 = unlock, 1 = lock
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("ERROR: Failed to unlock USB outputs");
        return;
    }
    
    Log("[OSC] Sent: /usbrec/lock = 0 (UNLOCKED)");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));  // Give Wing time to process
}

void WingOSC::SetCardOutputSource(int card_num, const std::string& grp, int in) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    const std::string normalized_grp = NormalizeDeskSourceGroup(grp);
    
    Log("[OSC] SetCardOutputSource: CARD OUTPUT " + std::to_string(card_num) + 
        " routing to " + normalized_grp + ":" + std::to_string(in));
    
    // CARD output routing paths (similar to USB):
    // /io/out/CRD/[1-32]/grp - source group (LCL, AUX, A, B, C, SC, USB, CRD, MOD, REC, AES)
    // /io/out/CRD/[1-32]/in  - input number within that group (1-40)
    
    // Set CARD output source group
    std::string addr_grp = "/io/out/CRD/" + std::to_string(card_num) + "/grp";
    p.Clear();
    p << MakeOscBeginToken(addr_grp.c_str())
      << normalized_grp.c_str()
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set CARD " + std::to_string(card_num) + " group");
        return;
    }
    Log("[OSC] " + addr_grp + " = " + normalized_grp);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    
    // Set CARD output input number
    std::string addr_in = "/io/out/CRD/" + std::to_string(card_num) + "/in";
    p.Clear();
    p << MakeOscBeginToken(addr_in.c_str())
      << (int32_t)in
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set CARD " + std::to_string(card_num) + " input");
        return;
    }
    Log("[OSC] " + addr_in + " = " + std::to_string(in));
}

void WingOSC::SetCardOutputName(int card_num, const std::string& name) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    Log("[OSC] SetCardOutputName: CARD OUTPUT " + std::to_string(card_num) + 
        " name = '" + name + "'");
    
    std::string addr_name = OscBuilder::CardPath(card_num, "name");
    p.Clear();
    p << MakeOscBeginToken(addr_name.c_str())
      << name.c_str()
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set CARD output " + std::to_string(card_num) + " name");
        return;
    }
    Log("[OSC] " + addr_name + " = " + name);
}

void WingOSC::SetWLiveRecordTrackCount(int slot, int tracks) {
    if (slot < 1 || slot > 2) {
        return;
    }
    const int clamped_tracks = std::max(2, tracks);
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    const std::string addr = "/cards/wlive/" + std::to_string(slot) + "/cfg/rectracks";
    p << MakeOscBeginToken(addr.c_str()) << std::to_string(clamped_tracks).c_str() << MakeOscEndToken();
    if (SendRawPacket(p.Data(), p.Size())) {
        Log("[OSC] " + addr + " = " + std::to_string(clamped_tracks));
    }
}

void WingOSC::SetRecorderOutputSource(int recorder_num, const std::string& grp, int in) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    const std::string normalized_grp = NormalizeDeskSourceGroup(grp);

    Log("[OSC] SetRecorderOutputSource: RECORDER OUTPUT " + std::to_string(recorder_num) +
        " routing to " + normalized_grp + ":" + std::to_string(in));

    std::string addr_grp = "/io/out/REC/" + std::to_string(recorder_num) + "/grp";
    p.Clear();
    p << MakeOscBeginToken(addr_grp.c_str())
      << normalized_grp.c_str()
      << MakeOscEndToken();

    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set RECORDER " + std::to_string(recorder_num) + " group");
        return;
    }
    Log("[OSC] " + addr_grp + " = " + normalized_grp);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    std::string addr_in = "/io/out/REC/" + std::to_string(recorder_num) + "/in";
    p.Clear();
    p << MakeOscBeginToken(addr_in.c_str())
      << (int32_t)in
      << MakeOscEndToken();

    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set RECORDER " + std::to_string(recorder_num) + " input");
        return;
    }
    Log("[OSC] " + addr_in + " = " + std::to_string(in));
}

void WingOSC::SetRecorderOutputName(int recorder_num, const std::string& name) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);

    Log("[OSC] SetRecorderOutputName: RECORDER OUTPUT " + std::to_string(recorder_num) +
        " name = '" + name + "'");

    std::string addr_name = "/io/out/REC/" + std::to_string(recorder_num) + "/name";
    p.Clear();
    p << MakeOscBeginToken(addr_name.c_str())
      << name.c_str()
      << MakeOscEndToken();

    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set RECORDER output " + std::to_string(recorder_num) + " name");
        return;
    }
    Log("[OSC] " + addr_name + " = " + name);
}

void WingOSC::SetCardInputName(int card_num, const std::string& name) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    Log("[OSC] SetCardInputName: CARD INPUT " + std::to_string(card_num) + 
        " name = '" + name + "'");
    
    // Set CARD input name: /io/in/CRD/[1-32]/name
    std::string addr_name = "/io/in/CRD/" + std::to_string(card_num) + "/name";
    p.Clear();
    p << MakeOscBeginToken(addr_name.c_str())
      << name.c_str()
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set CARD " + std::to_string(card_num) + " input name");
        return;
    }
    Log("[OSC] " + addr_name + " = " + name);
}

void WingOSC::SetSourceInputName(const std::string& group, int input_num, const std::string& name) {
    if (group.empty() || input_num <= 0) {
        return;
    }

    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    const std::string normalized_group = NormalizeDeskSourceGroup(group);
    const std::string addr_name = "/io/in/" + normalized_group + "/" + std::to_string(input_num) + "/name";
    p.Clear();
    p << MakeOscBeginToken(addr_name.c_str())
      << name.c_str()
      << MakeOscEndToken();

    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set " + normalized_group + " input " + std::to_string(input_num) + " name");
        return;
    }
    Log("[OSC] " + addr_name + " = " + name);
}

void WingOSC::RequestMeterValue(const std::string& address_template, int channel_num) {
    std::string address = address_template;
    if (address.find('%') != std::string::npos) {
        char formatted[256];
        snprintf(formatted, sizeof(formatted), address_template.c_str(), channel_num);
        address = formatted;
    }

    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    p << MakeOscBeginToken(address.c_str()) << MakeOscEndToken();
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("Failed meter request: " + address);
        return;
    }

    std::lock_guard<std::mutex> lock(data_mutex_);
    last_meter_address_ = address;
}

double WingOSC::GetLastMeterLinearValue(int value_index) const {
    std::lock_guard<std::mutex> lock(data_mutex_);
    if (value_index < 0 || value_index >= static_cast<int>(last_meter_values_.size())) {
        return 0.0;
    }
    return last_meter_values_[value_index];
}

std::vector<double> WingOSC::GetLastMeterValues() const {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return last_meter_values_;
}

void WingOSC::StartSDRecorder() {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    p << MakeOscBeginToken("/cards/wlive/1/$ctl/control") << "REC" << MakeOscEndToken();
    if (SendRawPacket(p.Data(), p.Size())) {
        Log("WING-LIVE start OSC sent: /cards/wlive/1/$ctl/control = REC");
    }
}

void WingOSC::StopSDRecorder() {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    p << MakeOscBeginToken("/cards/wlive/1/$ctl/control") << "STOP" << MakeOscEndToken();
    if (SendRawPacket(p.Data(), p.Size())) {
        Log("WING-LIVE stop OSC sent: /cards/wlive/1/$ctl/control = STOP");
    }
}

void WingOSC::StartUSBRecorder() {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    p << MakeOscBeginToken("/rec/$action") << "REC" << MakeOscEndToken();
    if (SendRawPacket(p.Data(), p.Size())) {
        Log("USB recorder start OSC sent: /rec/$action = REC");
    }
}

void WingOSC::StopUSBRecorder() {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    p << MakeOscBeginToken("/rec/$action") << "STOP" << MakeOscEndToken();
    if (SendRawPacket(p.Data(), p.Size())) {
        Log("USB recorder stop OSC sent: /rec/$action = STOP");
    }
}

bool WingOSC::GetUSBRecorderStatus(std::string& active_state, std::string& action_state) const {
    const auto values = QueryStringAddressesDirect({"/rec/$actstate", "/rec/$action"});
    bool ok = false;
    auto it = values.find("/rec/$actstate");
    if (it != values.end()) {
        active_state = it->second;
        ok = true;
    }
    it = values.find("/rec/$action");
    if (it != values.end()) {
        action_state = it->second;
        ok = true;
    }
    return ok;
}

bool WingOSC::GetWLiveRecorderStatus(int slot,
                                     std::string& state,
                                     std::string& media_state,
                                     std::string& error_message,
                                     std::string& error_code) const {
    if (slot < 1 || slot > 2) {
        return false;
    }
    const std::string prefix = "/cards/wlive/" + std::to_string(slot) + "/$stat/";
    const auto values = QueryStringAddressesDirect({
        prefix + "state",
        prefix + "sdstate",
        prefix + "errormessage",
        prefix + "errorcode",
    });
    bool ok = false;
    auto it = values.find(prefix + "state");
    if (it != values.end()) {
        state = it->second;
        ok = true;
    }
    it = values.find(prefix + "sdstate");
    if (it != values.end()) {
        media_state = it->second;
        ok = true;
    }
    it = values.find(prefix + "errormessage");
    if (it != values.end()) {
        error_message = it->second;
        ok = true;
    }
    it = values.find(prefix + "errorcode");
    if (it != values.end()) {
        error_code = it->second;
        ok = true;
    }
    return ok;
}

void WingOSC::SetUserControlLed(int layer, int button, bool on) {
    if (layer <= 0 || button <= 0) {
        return;
    }
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    const std::string addr_a = "/$ctl/user/" + std::to_string(layer) + "/" + std::to_string(button) + "/led";
    p << MakeOscBeginToken(addr_a.c_str()) << (int32_t)(on ? 1 : 0) << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());

    p.Clear();
    const std::string addr_b = "/$ctl/user/" + std::to_string(layer) + "/" + std::to_string(button) + "/config/led";
    p << MakeOscBeginToken(addr_b.c_str()) << (int32_t)(on ? 1 : 0) << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());
}

void WingOSC::SetUserControlColor(int layer, int button, int color_index) {
    if (layer <= 0 || button <= 0) {
        return;
    }
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    const std::string addr_a = "/$ctl/user/" + std::to_string(layer) + "/" + std::to_string(button) + "/col";
    p << MakeOscBeginToken(addr_a.c_str()) << (int32_t)color_index << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());

    p.Clear();
    const std::string addr_b = "/$ctl/user/" + std::to_string(layer) + "/" + std::to_string(button) + "/config/col";
    p << MakeOscBeginToken(addr_b.c_str()) << (int32_t)color_index << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());
}

void WingOSC::SetUserControlButtonLed(int layer, int button, bool on, bool lower_row) {
    if (layer <= 0 || button <= 0) {
        return;
    }
    const char* slot = lower_row ? "bd" : "bu";
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    const std::string addr_a = "/$ctl/user/" + std::to_string(layer) + "/" +
                               std::to_string(button) + "/" + slot + "/led";
    p << MakeOscBeginToken(addr_a.c_str()) << (int32_t)(on ? 1 : 0) << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());

    p.Clear();
    const std::string addr_b = "/$ctl/user/" + std::to_string(layer) + "/" +
                               std::to_string(button) + "/" + slot + "/config/led";
    p << MakeOscBeginToken(addr_b.c_str()) << (int32_t)(on ? 1 : 0) << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());
}

void WingOSC::SetActiveUserControlLayer(int layer) {
    if (layer <= 0) {
        return;
    }
    const std::vector<std::string> paths = {
        "/$ctl/user/layer",
        "/$ctl/user/page",
        "/$ctl/user/sel",
        "/$ctl/layer/user"
    };
    for (const auto& path : paths) {
        char buffer[256];
        osc::OutboundPacketStream p(buffer, 256);
        p << MakeOscBeginToken(path.c_str()) << (int32_t)layer << MakeOscEndToken();
        SendRawPacket(p.Data(), p.Size());
    }
}

void WingOSC::SetUserControlRotaryName(int layer, int rotary, const std::string& name) {
    if (layer <= 0 || rotary <= 0) {
        return;
    }
    const std::string path = "/$ctl/user/" + std::to_string(layer) + "/" +
                             std::to_string(rotary) + "/enc/name";
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    p << MakeOscBeginToken(path.c_str()) << name.c_str() << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());
}

void WingOSC::SetUserControlButtonName(int layer, int button, const std::string& name, bool lower_row) {
    if (layer <= 0 || button <= 0) {
        return;
    }
    const char* slot = lower_row ? "bd" : "bu";
    const std::string path = "/$ctl/user/" + std::to_string(layer) + "/" +
                             std::to_string(button) + "/" + slot + "/name";
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    p << MakeOscBeginToken(path.c_str()) << name.c_str() << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());
}

void WingOSC::SetUserControlButtonMidiCCToggle(int layer, int button, int midi_channel, int cc_number, int value, bool lower_row, bool toggle_mode) {
    if (layer <= 0 || button <= 0) {
        return;
    }
    midi_channel = std::max(1, std::min(16, midi_channel));
    cc_number = std::max(0, std::min(127, cc_number));
    value = std::max(0, std::min(127, value));

    const char* slot = lower_row ? "bd" : "bu";
    const std::string base = "/$ctl/user/" + std::to_string(layer) + "/" + std::to_string(button) + "/" + slot;
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);

    p << MakeOscBeginToken((base + "/mode").c_str()) << (toggle_mode ? "MIDICCT" : "MIDICCP") << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());

    p.Clear();
    p << MakeOscBeginToken((base + "/ch").c_str()) << (int32_t)midi_channel << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());

    p.Clear();
    p << MakeOscBeginToken((base + "/cc").c_str()) << (int32_t)cc_number << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());

    p.Clear();
    p << MakeOscBeginToken((base + "/val").c_str()) << (int32_t)value << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());
}

void WingOSC::ClearUserControlButtonCommand(int layer, int button, bool lower_row) {
    if (layer <= 0 || button <= 0) {
        return;
    }
    const char* slot = lower_row ? "bd" : "bu";
    const std::string path = "/$ctl/user/" + std::to_string(layer) + "/" + std::to_string(button) + "/" + slot + "/mode";
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    p << MakeOscBeginToken(path.c_str()) << "OFF" << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());
}

void WingOSC::SetUserControlButtonValue(int layer, int button, int value, bool lower_row) {
    if (layer <= 0 || button <= 0) {
        return;
    }
    value = std::max(0, std::min(127, value));
    const char* slot = lower_row ? "bd" : "bu";
    const std::string path = "/$ctl/user/" + std::to_string(layer) + "/" +
                             std::to_string(button) + "/" + slot + "/val";
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    p << MakeOscBeginToken(path.c_str()) << (int32_t)value << MakeOscEndToken();
    SendRawPacket(p.Data(), p.Size());
}

void WingOSC::QueryUserControlColor(int layer, int button) {
    if (layer <= 0 || button <= 0) {
        return;
    }
    const std::vector<std::string> paths = {
        "/$ctl/user/" + std::to_string(layer) + "/" + std::to_string(button) + "/col",
        "/$ctl/user/" + std::to_string(layer) + "/" + std::to_string(button) + "/config/col"
    };
    for (const auto& path : paths) {
        char buffer[256];
        osc::OutboundPacketStream p(buffer, 256);
        p << MakeOscBeginToken(path.c_str()) << MakeOscEndToken();
        SendRawPacket(p.Data(), p.Size());
    }
}

int WingOSC::GetCachedUserControlColor(int layer, int button, int fallback) const {
    std::lock_guard<std::mutex> lock(data_mutex_);
    auto it = user_control_color_cache_.find({layer, button});
    if (it == user_control_color_cache_.end()) {
        return fallback;
    }
    return it->second;
}

void WingOSC::QueryUserControlRotaryText(int layer, int rotary) {
    if (layer <= 0 || rotary <= 0) {
        return;
    }
    const std::vector<int> layer_candidates = {layer, std::max(0, layer - 1)};
    const std::vector<int> rotary_candidates = {rotary};
    for (int ly : layer_candidates) {
        for (int ry : rotary_candidates) {
            const std::vector<std::string> paths = {
                "/$ctl/user/" + std::to_string(ly) + "/" + std::to_string(ry) + "/enc/name",
                "/$ctl/user/" + std::to_string(ly) + "/" + std::to_string(ry) + "/enc/$fname",
                "/$ctl/user/" + std::to_string(ly) + "/rot/" + std::to_string(ry) + "/name",
                "/$ctl/user/" + std::to_string(ly) + "/rotary/" + std::to_string(ry) + "/name",
                "/$ctl/user/" + std::to_string(ly) + "/enc/" + std::to_string(ry) + "/name",
                "/$ctl/user/" + std::to_string(ly) + "/knob/" + std::to_string(ry) + "/name",
                "/$ctl/user/" + std::to_string(ly) + "/" + std::to_string(ry) + "/name",
                "/$ctl/user/" + std::to_string(ly) + "/rot/" + std::to_string(ry) + "/txt",
                "/$ctl/user/" + std::to_string(ly) + "/rotary/" + std::to_string(ry) + "/txt",
                "/$ctl/user/" + std::to_string(ly) + "/enc/" + std::to_string(ry) + "/txt",
                "/$ctl/user/" + std::to_string(ly) + "/knob/" + std::to_string(ry) + "/txt",
                "/$ctl/user/" + std::to_string(ly) + "/" + std::to_string(ry) + "/txt",
                "/$ctl/user/" + std::to_string(ly) + "/rot/" + std::to_string(ry) + "/label",
                "/$ctl/user/" + std::to_string(ly) + "/rotary/" + std::to_string(ry) + "/label",
                "/$ctl/user/" + std::to_string(ly) + "/enc/" + std::to_string(ry) + "/label",
                "/$ctl/user/" + std::to_string(ly) + "/knob/" + std::to_string(ry) + "/label",
                "/$ctl/user/" + std::to_string(ly) + "/" + std::to_string(ry) + "/label"
            };
            for (const auto& path : paths) {
                char buffer[256];
                osc::OutboundPacketStream p(buffer, 256);
                p << MakeOscBeginToken(path.c_str()) << MakeOscEndToken();
                SendRawPacket(p.Data(), p.Size());
            }
        }
    }
}

void WingOSC::ClearUSBOutput(int usb_num) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    // Clear USB output by setting source group to OFF
    std::string addr_grp = "/io/out/USB/" + std::to_string(usb_num) + "/grp";
    p.Clear();
    p << MakeOscBeginToken(addr_grp.c_str())
      << "OFF"
      << MakeOscEndToken();
    
    SendRawPacket(p.Data(), p.Size());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void WingOSC::ClearCardOutput(int card_num) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    // Clear CARD output by setting source group to OFF
    std::string addr_grp = "/io/out/CRD/" + std::to_string(card_num) + "/grp";
    p.Clear();
    p << MakeOscBeginToken(addr_grp.c_str())
      << "OFF"
      << MakeOscEndToken();
    
    SendRawPacket(p.Data(), p.Size());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void WingOSC::ClearRecorderOutput(int recorder_num) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);

    std::string addr_grp = "/io/out/REC/" + std::to_string(recorder_num) + "/grp";
    p.Clear();
    p << MakeOscBeginToken(addr_grp.c_str())
      << "OFF"
      << MakeOscEndToken();

    SendRawPacket(p.Data(), p.Size());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void WingOSC::SetUSBInputMode(int usb_num, const std::string& mode) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    Log("[OSC] SetUSBInputMode: USB INPUT " + std::to_string(usb_num) + 
        " mode = " + mode + " (ST=stereo, M=mono)");
    
    // Set USB input mode: /io/in/USB/[1-48]/mode
    std::string addr_mode = "/io/in/USB/" + std::to_string(usb_num) + "/mode";
    p.Clear();
    p << MakeOscBeginToken(addr_mode.c_str())
      << mode.c_str()
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set USB " + std::to_string(usb_num) + " mode");
        return;
    }
    Log("[OSC] " + addr_mode + " = " + mode);
}

void WingOSC::SetCardInputMode(int card_num, const std::string& mode) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    Log("[OSC] SetCardInputMode: CARD INPUT " + std::to_string(card_num) + 
        " mode = " + mode + " (ST=stereo, M=mono)");
    
    // Set CARD input mode: /io/in/CRD/[1-32]/mode
    std::string addr_mode = "/io/in/CRD/" + std::to_string(card_num) + "/mode";
    p.Clear();
    p << MakeOscBeginToken(addr_mode.c_str())
      << mode.c_str()
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set CARD " + std::to_string(card_num) + " mode");
        return;
    }
    Log("[OSC] " + addr_mode + " = " + mode);
}

void WingOSC::SetSourceInputMode(const std::string& group, int input_num, const std::string& mode) {
    if (group.empty() || input_num <= 0) {
        return;
    }

    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    const std::string addr_mode = "/io/in/" + group + "/" + std::to_string(input_num) + "/mode";
    p.Clear();
    p << MakeOscBeginToken(addr_mode.c_str())
      << mode.c_str()
      << MakeOscEndToken();

    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("[ERROR] Failed to set " + group + " input " + std::to_string(input_num) + " mode");
        return;
    }
    Log("[OSC] " + addr_mode + " = " + mode);
}

void WingOSC::ClearUSBInput(int usb_num) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
        // Clear USB input by resetting mode and name
    std::string addr_mode = "/io/in/USB/" + std::to_string(usb_num) + "/mode";
    p.Clear();
    p << MakeOscBeginToken(addr_mode.c_str())
      << "M"
      << MakeOscEndToken();
    
    SendRawPacket(p.Data(), p.Size());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

        std::string addr_name = "/io/in/USB/" + std::to_string(usb_num) + "/name";
        p.Clear();
        p << MakeOscBeginToken(addr_name.c_str())
            << ""
            << MakeOscEndToken();

        SendRawPacket(p.Data(), p.Size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void WingOSC::ClearCardInput(int card_num) {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
        // Clear CARD input by resetting mode and name
    std::string addr_mode = "/io/in/CRD/" + std::to_string(card_num) + "/mode";
    p.Clear();
    p << MakeOscBeginToken(addr_mode.c_str())
      << "M"
      << MakeOscEndToken();
    
    SendRawPacket(p.Data(), p.Size());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

        std::string addr_name = "/io/in/CRD/" + std::to_string(card_num) + "/name";
        p.Clear();
        p << MakeOscBeginToken(addr_name.c_str())
            << ""
            << MakeOscEndToken();

        SendRawPacket(p.Data(), p.Size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void WingOSC::LockUSBOutputs() {
    char buffer[256];
    osc::OutboundPacketStream p(buffer, 256);
    
    // Lock USB recording outputs: /usbrec/lock 1
    p << MakeOscBeginToken("/usbrec/lock")
      << (int32_t)1  // 0 = unlock, 1 = lock
      << MakeOscEndToken();
    
    if (!SendRawPacket(p.Data(), p.Size())) {
        Log("ERROR: Failed to lock USB outputs");
        return;
    }
    
    Log("[OSC] Sent: /usbrec/lock = 1 (LOCKED)");
}

void WingOSC::SetAllChannelsAltEnabled(bool enable) {
    Log(std::string("Setting ALL channels ALT to ") + (enable ? "enabled" : "disabled"));
    
    // Loop through all channels we have data for
    std::lock_guard<std::mutex> lock(data_mutex_);
    for (const auto& pair : channel_data_) {
        // Only toggle if ALT is configured (altgrp != "OFF")
        if (pair.second.alt_source_group != "OFF" && !pair.second.alt_source_group.empty()) {
            EnableChannelAltSource(pair.first, enable);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

// Playback allocation algorithm with gap backfilling.
std::vector<PlaybackAllocation> WingOSC::CalculatePlaybackAllocation(const std::vector<SourceSelectionInfo>& channels) {
    Log("Calculating playback allocation for " + std::to_string(channels.size()) + " sources...");
    auto allocations = PlaybackAllocator::BuildSequentialPlaybackAllocation(channels);
    for (const auto& alloc : allocations) {
        if (alloc.is_stereo) {
            Log("Source " + std::to_string(alloc.source_number) + " (stereo) → slot " +
                std::to_string(alloc.usb_start) + "-" + std::to_string(alloc.usb_end));
        } else {
            Log("Source " + std::to_string(alloc.source_number) + " (mono) → slot " +
                std::to_string(alloc.usb_start));
        }
    }
    return allocations;
}

std::vector<PlaybackAllocation> WingOSC::CalculateUSBAllocation(const std::vector<SourceSelectionInfo>& channels) {
    return CalculatePlaybackAllocation(channels);
}

void WingOSC::QueryUserSignalInputs(int count) {
    if (!handshake_complete_ && !PerformHandshake()) {
        Log("Cannot query USR inputs: Wing handshake failed");
        return;
    }
    
    Log("[QUERY] Requesting " + std::to_string(count) + " User Signal Input routing data...");
    Log("[QUERY] Using correct Wing OSC paths: /io/in/USR/N/user/grp and /io/in/USR/N/user/in");
    
    std::vector<std::string> addresses;
    addresses.reserve(static_cast<size_t>(count) * 2);
    for (int i = 1; i <= count; ++i) {
        addresses.push_back("/io/in/USR/" + std::to_string(i) + "/user/grp");
        addresses.push_back("/io/in/USR/" + std::to_string(i) + "/user/in");
    }
    SendQueryBurst(addresses);
    std::this_thread::sleep_for(std::chrono::milliseconds(kAsyncQuerySettlingMs));
    
    // Log how many USR inputs we have routing data for
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        Log("[QUERY] Completed - USR routing data populated for " + 
            std::to_string(usr_routing_data_.size()) + " inputs");
        
        if (usr_routing_data_.size() > 0) {
            // Log the first few as a sanity check
            Log("[QUERY] Sample USR routing data:");
            int logged = 0;
            for (const auto& [usr_num, src_pair] : usr_routing_data_) {
                Log("       USR:" + std::to_string(usr_num) + " → " + src_pair.first + 
                    ":" + std::to_string(src_pair.second));
                if (++logged >= 3) break;
            }
        } else {
            Log("[WARNING] No USR routing data received - queries may have failed!");
        }
    }
}

void WingOSC::QueryUserSignalStereo(int count) {
    if (!handshake_complete_ && !PerformHandshake()) {
        Log("Cannot query USR stereo: Wing handshake failed");
        return;
    }
    
    Log("========== QUERYING USR STEREO STATUS ==========");
    Log("Testing multiple OSC paths for USR stereo status...");
    
    // Try queries to all USRs on all candidate paths
    std::vector<std::string> test_paths = {
        "/io/in/USR/{}/clink",
        "/io/in/USR/{}/link", 
        "/io/in/USR/{}/stereo",
    };
    
    for (const auto& path_template : test_paths) {
        Log("Querying path: " + path_template);
        
        for (int i = 1; i <= count; ++i) {
            std::string path = path_template;
            size_t pos = path.find("{}");
            if (pos != std::string::npos) {
                path.replace(pos, 2, std::to_string(i));
            }
            
            char buffer[256];
            osc::OutboundPacketStream p(buffer, 256);
            p << MakeOscBeginToken(path.c_str()) << MakeOscEndToken();
            SendRawPacket(p.Data(), p.Size());
            
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        // Wait for responses
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    Log("=========== QUERY COMPLETE ===========");
}

std::pair<std::string, int> WingOSC::ResolveRoutingChain(const std::string& grp, int in) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return ResolveRoutingChainLocked(grp, in);
}

std::pair<std::string, int> WingOSC::ResolveRoutingChainLocked(const std::string& grp, int in) const {
    // If source is NOT a User Signal input, return it as-is
    if (grp != "USR") {
        return {grp, in};
    }
    
    // If source IS a User Signal input (USR), look up what it sources from
    auto it = usr_routing_data_.find(in);
    if (it == usr_routing_data_.end()) {
        Log("[NOTICE] USR:" + std::to_string(in) + " - Wing console doesn't expose USR routing via OSC");
        Log("         (Wing firmware limitation: /usr/*/in/conn/* queries return no response)");
        Log("         To fix: reconfigure channels to use direct (non-USR) sources.");
        Log("         Falling back to non-resolved USB routing...");
        return {grp, in};
    }
    
    // Follow the chain: if the USR input sources from another USR, recurse
    const auto& [resolved_grp, resolved_in] = it->second;
    if (resolved_grp == "USR") {
        // Recurse to follow the full chain
        return ResolveRoutingChainLocked(resolved_grp, resolved_in);
    }
    
    return {resolved_grp, resolved_in};
}

bool WingOSC::IsUserSignalStereo(int usr_num) const {
    auto it = usr_stereo_data_.find(usr_num);
    if (it == usr_stereo_data_.end()) {
        return false;  // Default to mono if no stereo data available
    }
    return it->second;
}

void WingOSC::QueryInputSourceNames(const std::set<std::pair<std::string, int>>& sources) {
    if (!handshake_complete_ && !PerformHandshake()) {
        Log("Cannot query source names: Wing handshake failed");
        return;
    }

    std::vector<std::string> addresses;
    addresses.reserve(sources.size());
    for (const auto& src : sources) {
        const std::string& grp = src.first;
        int in = src.second;
        if (in <= 0) {
            continue;
        }

        // Query source labels for directly readable input groups used by the selection popup.
        if (!IsDirectInputQueryGroup(grp)) {
            continue;
        }
        addresses.push_back("/io/in/" + grp + "/" + std::to_string(in) + "/name");
    }
    SendQueryBurst(addresses);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

std::string WingOSC::GetInputSourceName(const std::string& grp, int in) const {
    std::lock_guard<std::mutex> lock(data_mutex_);
    std::string key = grp + ":" + std::to_string(in);
    auto it = input_source_names_.find(key);
    if (it != input_source_names_.end()) {
        return it->second;
    }
    return "";
}

std::string WingOSC::QueryInputSourceNameDirect(const std::string& grp, int in) const {
    if (in <= 0) {
        return "";
    }
    if (!IsDirectInputQueryGroup(grp)) {
        return "";
    }

    std::string name;
    if (QueryInputNameDirectRaw(wing_ip_, wing_port_, grp, in, name)) {
        return name;
    }
    return "";
}

std::string WingOSC::QueryInputModeDirect(const std::string& grp, int in) const {
    if (in <= 0) {
        return "";
    }
    if (!IsDirectInputQueryGroup(grp)) {
        return "";
    }

    std::string mode;
    if (QueryInputModeDirectRaw(wing_ip_, wing_port_, grp, in, mode)) {
        return mode;
    }
    return "";
}

std::string WingOSC::QueryConsoleSourceNameDirect(SourceKind kind, int number) const {
    if (number <= 0) {
        return "";
    }

    std::vector<std::string> candidate_paths;
    char formatted[8];
    snprintf(formatted, sizeof(formatted), "%02d", number);

    switch (kind) {
        case SourceKind::Bus:
            candidate_paths = {
                "/bus/" + std::string(formatted) + "/name",
                "/bus/" + std::to_string(number) + "/name",
                "/bus/" + std::string(formatted) + "/config/name",
                "/bus/" + std::to_string(number) + "/config/name",
            };
            break;
        case SourceKind::Main:
            candidate_paths = {
                "/main/" + std::string(formatted) + "/name",
                "/main/" + std::to_string(number) + "/name",
                "/main/" + std::string(formatted) + "/config/name",
                "/main/" + std::to_string(number) + "/config/name",
            };
            break;
        case SourceKind::Matrix:
            candidate_paths = {
                "/mtx/" + std::string(formatted) + "/name",
                "/mtx/" + std::to_string(number) + "/name",
                "/mtx/" + std::string(formatted) + "/config/name",
                "/mtx/" + std::to_string(number) + "/config/name",
            };
            break;
        case SourceKind::Channel:
            return "";
    }

    for (const auto& path : candidate_paths) {
        std::string name;
        if (QueryStringAddressDirectRaw(wing_ip_, wing_port_, path, name) && !name.empty()) {
            return name;
        }
    }
    return "";
}

void WingOSC::ApplyPlaybackAllocationAsAlt(const std::vector<PlaybackAllocation>& allocations,
                                           const std::vector<SourceSelectionInfo>& channels,
                                           const std::string& output_mode,
                                           bool setup_soundcheck) {
    std::string output_type = (output_mode == "CARD") ? "CARD" : "USB";
    auto kind_label = [](SourceKind kind) {
        switch (kind) {
            case SourceKind::Channel: return "CH";
            case SourceKind::Bus: return "BUS";
            case SourceKind::Main: return "MAIN";
            case SourceKind::Matrix: return "MTX";
        }
        return "SRC";
    };
    auto source_key = [&](SourceKind kind, int number) {
        return std::string(kind_label(kind)) + std::to_string(number);
    };

    bool configure_soundcheck_inputs = false;
    for (const auto& src : channels) {
        if (src.soundcheck_capable) {
            configure_soundcheck_inputs = configure_soundcheck_inputs || setup_soundcheck;
        }
    }

    Log("\n╔═══════════════════════════════════════════════════════════╗");
    if (configure_soundcheck_inputs) {
        Log("║     SOUNDCHECK " + output_type + " MAPPING CONFIGURATION                 ║");
    } else {
        Log("║     RECORDING-ONLY " + output_type + " CONFIGURATION                     ║");
    }
    Log("╚═══════════════════════════════════════════════════════════╝");
    Log("\n[SETUP] ApplyPlaybackAllocationAsAlt called with " + std::to_string(allocations.size()) +
        " allocations and " + std::to_string(channels.size()) + " selected sources");
    Log("[SETUP] Output mode: " + output_type);
    Log("[SETUP] Setup soundcheck: " + std::string(configure_soundcheck_inputs ? "YES" : "NO"));
    
    // CRITICAL: Unlock USB outputs before configuration
    Log("\n[SETUP] Step 1/4: Unlocking USB recording outputs...");
    UnlockUSBOutputs();
    
    // Step 2: Clear full selected output/input bank to avoid stale mappings from previous runs
    Log("\n[SETUP] Step 2/4: Clearing full " + output_type + " bank to remove old mappings...");
    const int max_outputs = (output_type == "CARD") ? 32 : 48;
    const int max_inputs = (output_type == "CARD") ? 32 : 48;

    Log("       Clearing " + std::to_string(max_outputs) + " " + output_type + " outputs...");
    for (int output_num = 1; output_num <= max_outputs; ++output_num) {
        if (output_type == "CARD") {
            ClearCardOutput(output_num);
        } else {
            ClearUSBOutput(output_num);
        }
    }
    Log("       ✓ Outputs cleared");

    if (configure_soundcheck_inputs) {
        Log("       Clearing " + std::to_string(max_inputs) + " " + output_type + " inputs (mode reset)...");
        for (int input_num = 1; input_num <= max_inputs; ++input_num) {
            if (output_type == "CARD") {
                ClearCardInput(input_num);
            } else {
                ClearUSBInput(input_num);
            }
        }
        Log("       ✓ Inputs cleared");
    } else {
        Log("       Skipping input bank reset (record-only sources)");
    }
    
    std::map<std::pair<int, int>, SourceSelectionInfo> source_map;
    for (const auto& ch : channels) {
        source_map[{static_cast<int>(ch.kind), ch.source_number}] = ch;
    }
    
    const auto full_data = GetChannelData();

    int configured_count = 0;
    int skipped_count = 0;
    std::set<int> configured_channels_with_alt;
    std::set<std::pair<int, int>> processed_stereo_sources;
    
    Log("\n[SETUP] Step 3/4: Configuring " + output_type + " output sources and naming...");
    Log("───────────────────────────────────────────────────────────");
    
    for (const auto& alloc : allocations) {
        if (!alloc.allocation_note.empty() && alloc.allocation_note.find("ERROR") != std::string::npos) {
            Log("[SKIP] " + source_key(alloc.source_kind, alloc.source_number) + ": " + alloc.allocation_note);
            skipped_count++;
            continue;
        }
        
        const auto lookup_key = std::make_pair(static_cast<int>(alloc.source_kind), alloc.source_number);
        if (processed_stereo_sources.count(lookup_key)) {
            Log("[SKIP] " + source_key(alloc.source_kind, alloc.source_number) + ": already processed as stereo partner");
            skipped_count++;
            continue;
        }
        
        auto it = source_map.find(lookup_key);
        if (it == source_map.end()) {
            Log("[ERROR] " + source_key(alloc.source_kind, alloc.source_number) + " not found in source data");
            skipped_count++;
            continue;
        }
        const SourceSelectionInfo& selected = it->second;

        ChannelInfo ch_info{};
        bool has_channel_info = false;
        if (alloc.source_kind == SourceKind::Channel) {
            auto ch_it = full_data.find(alloc.source_number);
            if (ch_it == full_data.end()) {
                Log("[ERROR] CH" + std::to_string(alloc.source_number) + " not found in channel data");
                skipped_count++;
                continue;
            }
            ch_info = ch_it->second;
            has_channel_info = true;
        }

        const std::string display_name = !selected.name.empty()
            ? selected.name
            : source_key(alloc.source_kind, alloc.source_number);
        const bool can_setup_soundcheck = configure_soundcheck_inputs && selected.soundcheck_capable && has_channel_info;
        std::string src_grp = (!selected.source_group.empty() && selected.source_input > 0)
            ? selected.source_group
            : (has_channel_info ? ch_info.primary_source_group : selected.source_group);
        int src_in = (selected.source_input > 0)
            ? selected.source_input
            : (has_channel_info ? ch_info.primary_source_input : selected.source_input);
        if (!src_grp.empty() && src_in > 0 && !IsDirectOutputSourceGroup(src_grp)) {
            auto resolved = ResolveRoutingChain(src_grp, src_in);
            src_grp = resolved.first;
            src_in = resolved.second;
        }
        const int planned_primary_stereo_start =
            alloc.is_stereo ? WingConnector::SourceInputModePlan::PlannedStereoPrimaryInputStart(src_in, alloc.usb_start) : src_in;
        
        if (alloc.is_stereo) {
            const int partner_channel = WingConnector::StereoChannelPlan::PartnerChannel(alloc.source_number);
            Log("\n[STEREO] " + source_key(alloc.source_kind, alloc.source_number) + ": " + display_name);
            Log("         Primary source: " + src_grp + ":" + std::to_string(src_in));

            std::string right_src_grp = src_grp;
            int right_src_in = src_in + 1;
            
            Log("           " + output_type + " OUTPUT " + std::to_string(alloc.usb_start) + 
                " (LEFT):  " + src_grp + ":" + std::to_string(src_in) + " → REAPER");
            if (output_type == "CARD") {
                SetCardOutputSource(alloc.usb_start, src_grp, src_in);
            } else {
                SetUSBOutputSource(alloc.usb_start, src_grp, src_in);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            
            Log("           " + output_type + " OUTPUT " + std::to_string(alloc.usb_end) + 
                " (RIGHT): " + right_src_grp + ":" + std::to_string(right_src_in) + " → REAPER");
            if (output_type == "CARD") {
                SetCardOutputSource(alloc.usb_end, right_src_grp, right_src_in);
            } else {
                SetUSBOutputSource(alloc.usb_end, right_src_grp, right_src_in);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            
            // ===== OUTPUT NAMES =====
            Log("\n         [NAMING " + output_type + " OUTPUTS]");
            
            // Name outputs (what Wing console sends)
            std::string out_left_name = WingConnector::PlaybackNaming::StereoOutputLeftName(display_name);
            std::string out_right_name = WingConnector::PlaybackNaming::StereoOutputRightName(display_name);
            std::string in_pair_name = WingConnector::PlaybackNaming::StereoInputName(display_name);
            
            Log("           " + output_type + " OUTPUT " + std::to_string(alloc.usb_start) + 
                " name: '" + out_left_name + "'");
            if (output_type == "CARD") {
                SetCardOutputName(alloc.usb_start, out_left_name);
            } else {
                SetUSBOutputName(alloc.usb_start, out_left_name);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            
            Log("           " + output_type + " OUTPUT " + std::to_string(alloc.usb_end) + 
                " name: '" + out_right_name + "'");
            if (output_type == "CARD") {
                SetCardOutputName(alloc.usb_end, out_right_name);
            } else {
                SetUSBOutputName(alloc.usb_end, out_right_name);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            
            if (can_setup_soundcheck) {
                Log("\n         [NAME CHANNEL]");
                Log("           CH" + std::to_string(alloc.source_number) + " name: '" + display_name + "'");
                SetChannelName(alloc.source_number, display_name);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (partner_channel > 0) {
                    Log("           CH" + std::to_string(partner_channel) + " name: ''");
                    SetChannelName(partner_channel, "");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                if (selected.color_id >= 0) {
                    Log("           CH" + std::to_string(alloc.source_number) + " color: " + std::to_string(selected.color_id));
                    SetChannelColor(alloc.source_number, selected.color_id);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }

                Log("\n         [SET CHANNEL CUSTOMIZATION LINK]");
                Log("           CH" + std::to_string(alloc.source_number) + " customization link: off");
                SetChannelCustomizationLinked(alloc.source_number, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (partner_channel > 0) {
                    Log("           CH" + std::to_string(partner_channel) + " customization link: off");
                    SetChannelCustomizationLinked(partner_channel, false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }

                Log("\n         [RESET ALT SOURCE]");
                SetChannelAltSource(alloc.source_number, "OFF", 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                EnableChannelAltSource(alloc.source_number, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                if (partner_channel > 0) {
                    SetChannelAltSource(partner_channel, "OFF", 1);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    EnableChannelAltSource(partner_channel, false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }

                if (!src_grp.empty() && planned_primary_stereo_start > 0) {
                    Log("\n         [REASSIGN PRIMARY SOURCE]");
                    Log("           CH" + std::to_string(alloc.source_number) + " primary: " +
                        src_grp + ":" + std::to_string(planned_primary_stereo_start));
                    SetChannelPrimarySource(alloc.source_number, src_grp, planned_primary_stereo_start);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    if (partner_channel > 0) {
                        Log("           CH" + std::to_string(partner_channel) + " primary: OFF:1");
                        SetChannelPrimarySource(partner_channel, "OFF", 1);
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }

                    Log("\n         [NAME PRIMARY SOURCE INPUTS]");
                    Log("           " + src_grp + " INPUT " + std::to_string(planned_primary_stereo_start) + " name: '" + out_left_name + "'");
                    SetSourceInputName(src_grp, planned_primary_stereo_start, out_left_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    Log("           " + src_grp + " INPUT " + std::to_string(planned_primary_stereo_start + 1) + " name: '" + out_right_name + "'");
                    SetSourceInputName(src_grp, planned_primary_stereo_start + 1, out_right_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));

                    Log("\n         [SET PRIMARY SOURCE MODE] (Stereo source)");
                    Log("           " + src_grp + " INPUT " + std::to_string(planned_primary_stereo_start) + " mode: ST");
                    SetSourceInputMode(src_grp, planned_primary_stereo_start, "ST");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    Log("           " + src_grp + " INPUT " + std::to_string(planned_primary_stereo_start + 1) + " mode: ST");
                    SetSourceInputMode(src_grp, planned_primary_stereo_start + 1, "ST");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                } else {
                    Log("\n         [SKIP PRIMARY SOURCE PAIRING]");
                    if (src_grp.empty()) {
                        Log("           Primary source group is unavailable; leaving channel primary source unchanged.");
                    } else {
                        Log("           No valid odd-start stereo primary source pair is available for this adopted source.");
                    }
                }

                if (output_type == "USB") {
                    Log("\n         [NAMING USB INPUTS] (For REAPER playback)");
                    Log("           USB INPUT " + std::to_string(alloc.usb_start) + 
                        " name: '" + in_pair_name + "'");
                    SetUSBInputName(alloc.usb_start, in_pair_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    
                    Log("           USB INPUT " + std::to_string(alloc.usb_end) + 
                        " name: '" + in_pair_name + "'");
                    SetUSBInputName(alloc.usb_end, in_pair_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    
                    Log("\n         [SET USB INPUT MODES] (Stereo pair)");
                    Log("           USB INPUT " + std::to_string(alloc.usb_start) + " mode: ST (stereo)");
                    SetUSBInputMode(alloc.usb_start, "ST");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    
                    Log("           USB INPUT " + std::to_string(alloc.usb_end) + " mode: ST (stereo)");
                    SetUSBInputMode(alloc.usb_end, "ST");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                } else if (output_type == "CARD") {
                    Log("\n         [NAMING CARD INPUTS] (For REAPER playback)");
                    Log("           CARD INPUT " + std::to_string(alloc.usb_start) + 
                        " name: '" + in_pair_name + "'");
                    SetCardInputName(alloc.usb_start, in_pair_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    
                    Log("           CARD INPUT " + std::to_string(alloc.usb_end) + 
                        " name: '" + in_pair_name + "'");
                    SetCardInputName(alloc.usb_end, in_pair_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    
                    Log("\n         [SET CARD INPUT MODES] (Stereo pair)");
                    Log("           CARD INPUT " + std::to_string(alloc.usb_start) + " mode: ST (stereo)");
                    SetCardInputMode(alloc.usb_start, "ST");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    
                    Log("           CARD INPUT " + std::to_string(alloc.usb_end) + " mode: ST (stereo)");
                    SetCardInputMode(alloc.usb_end, "ST");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                
                Log("\n         [CONFIGURE ALT SOURCES] (For soundcheck mode toggle)");
                
                Log("           CH" + std::to_string(alloc.source_number) + 
                    " ALT: " + output_type + ":" + std::to_string(alloc.usb_start));
                SetChannelAltSource(alloc.source_number, output_type, alloc.usb_start);
                configured_channels_with_alt.insert(alloc.source_number);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } else {
                Log("\n         [SKIP INPUT/ALT CONFIG] (Recording-only mode)");
            }
            
            processed_stereo_sources.insert(lookup_key);
            
        } else {
            Log("\n[MONO] " + source_key(alloc.source_kind, alloc.source_number) + ": " + display_name);
            Log("       Primary source: " + src_grp + ":" + std::to_string(src_in));
            
            Log("\n       [CONFIGURE " + output_type + " OUTPUT] (Wing console → REAPER via " + output_type + ")");
            
            Log("         " + output_type + " OUTPUT " + std::to_string(alloc.usb_start) + 
                ": " + src_grp + ":" + std::to_string(src_in) + " → REAPER");
            if (output_type == "CARD") {
                SetCardOutputSource(alloc.usb_start, src_grp, src_in);
            } else {
                SetUSBOutputSource(alloc.usb_start, src_grp, src_in);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            
            Log("\n       [NAMING " + output_type + " OUTPUT]");
            
            Log("         " + output_type + " OUTPUT " + std::to_string(alloc.usb_start) + 
                " name: '" + display_name + "'");
            if (output_type == "CARD") {
                SetCardOutputName(alloc.usb_start, display_name);
            } else {
                SetUSBOutputName(alloc.usb_start, display_name);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            
            if (can_setup_soundcheck) {
                Log("\n       [NAME CHANNEL]");
                Log("         CH" + std::to_string(alloc.source_number) + " name: '" + display_name + "'");
                SetChannelName(alloc.source_number, display_name);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (selected.color_id >= 0) {
                    Log("         CH" + std::to_string(alloc.source_number) + " color: " + std::to_string(selected.color_id));
                    SetChannelColor(alloc.source_number, selected.color_id);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }

                Log("\n       [RESET ALT SOURCE]");
                SetChannelAltSource(alloc.source_number, "OFF", 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                EnableChannelAltSource(alloc.source_number, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));

                if (!src_grp.empty() && src_in > 0) {
                    Log("\n       [NAME PRIMARY SOURCE INPUT]");
                    Log("         " + src_grp + " INPUT " + std::to_string(src_in) + " name: '" + display_name + "'");
                    SetSourceInputName(src_grp, src_in, display_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));

                    Log("\n       [SET PRIMARY SOURCE MODE] (Mono)");
                    Log("         " + src_grp + " INPUT " + std::to_string(src_in) + " mode: M");
                    SetSourceInputMode(src_grp, src_in, "M");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }

                if (output_type == "USB") {
                    Log("\n       [NAMING USB INPUT] (For REAPER playback)");
                    Log("         USB INPUT " + std::to_string(alloc.usb_start) + 
                        " name: '" + display_name + "'");
                    SetUSBInputName(alloc.usb_start, display_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    
                    Log("\n       [SET USB INPUT MODE] (Mono)");
                    Log("         USB INPUT " + std::to_string(alloc.usb_start) + " mode: M (mono)");
                    SetUSBInputMode(alloc.usb_start, "M");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                } else if (output_type == "CARD") {
                    Log("\n       [NAMING CARD INPUT] (For REAPER playback)");
                    Log("         CARD INPUT " + std::to_string(alloc.usb_start) + 
                        " name: '" + display_name + "'");
                    SetCardInputName(alloc.usb_start, display_name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    
                    Log("\n       [SET CARD INPUT MODE] (Mono)");
                    Log("         CARD INPUT " + std::to_string(alloc.usb_start) + " mode: M (mono)");
                    SetCardInputMode(alloc.usb_start, "M");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                
                Log("\n       [CONFIGURE ALT SOURCE] (For soundcheck mode toggle)");
                
                Log("         CH" + std::to_string(alloc.source_number) + 
                    " ALT: " + output_type + ":" + std::to_string(alloc.usb_start));
                SetChannelAltSource(alloc.source_number, output_type, alloc.usb_start);
                configured_channels_with_alt.insert(alloc.source_number);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } else {
                Log("\n       [SKIP INPUT/ALT CONFIG] (Recording-only mode)");
            }
        }
        
        configured_count++;
    }
    
    if (configure_soundcheck_inputs) {
        Log("\n[SETUP] Step 4/5: Clearing ALT sources for unselected channels...");
        int cleared_unselected = 0;
        for (const auto& [channel_num, channel_info] : full_data) {
            (void)channel_info;
            if (configured_channels_with_alt.find(channel_num) == configured_channels_with_alt.end()) {
                SetChannelAltSource(channel_num, "OFF", 1);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                EnableChannelAltSource(channel_num, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                cleared_unselected++;
            }
        }
        Log("       ✓ Cleared ALT source on " + std::to_string(cleared_unselected) + " unselected channels");
    } else {
        Log("\n[SETUP] Step 4/5: Skipping ALT source clearing (recording-only mode)");
    }

    Log("\n───────────────────────────────────────────────────────────");
    Log("[SETUP] Step 5/5: Summary");
    Log("       ✓ Configured: " + std::to_string(configured_count) + " channels");
    Log("       ✗ Skipped:    " + std::to_string(skipped_count) + " channels");
    
    Log("\n[COMPLETE] " + output_type + " mapping configuration finished");
    if (output_type == "USB") {
        Log("           USB outputs remain UNLOCKED for manual adjustment if needed.");
        Log("           You can lock them manually on the Wing console if desired.");
    }
    
    Log("\n╔═══════════════════════════════════════════════════════════╗");
    if (configure_soundcheck_inputs) {
        Log("║     SOUNDCHECK READY - READY FOR REAPER TRACK SETUP       ║");
    } else {
        Log("║     RECORDING READY - READY FOR REAPER TRACK SETUP        ║");
    }
    Log("╚═══════════════════════════════════════════════════════════╝\n");
}

void WingOSC::ApplyUSBAllocationAsAlt(const std::vector<PlaybackAllocation>& allocations,
                                      const std::vector<SourceSelectionInfo>& channels,
                                      const std::string& output_mode,
                                      bool setup_soundcheck) {
    ApplyPlaybackAllocationAsAlt(allocations, channels, output_mode, setup_soundcheck);
}

void WingOSC::QueryChannel(int channel_num) {
    if (!handshake_complete_ && !PerformHandshake()) {
        Log("Cannot query channel: Wing handshake failed");
        return;
    }

    const std::string ch = FormatChannelNum(channel_num);
    SendQueryBurst({
        "/ch/" + ch + "/name",
        "/ch/" + ch + "/col",
        "/ch/" + ch + "/icon",
        "/ch/" + ch + "/in/conn/grp",
        "/ch/" + ch + "/in/conn/in",
        "/ch/" + ch + "/in/conn/altgrp",
        "/ch/" + ch + "/in/conn/altin",
        "/ch/" + ch + "/in/set/altsrc",
    });
}

void WingOSC::QueryAllChannels(int count) {
    if (!handshake_complete_ && !PerformHandshake()) {
        Log("Cannot query channels: Wing handshake failed");
        return;
    }

    Log("Querying " + std::to_string(count) + " channels from Wing...");
    
    std::vector<std::string> first_pass_addresses;
    first_pass_addresses.reserve(static_cast<size_t>(count) * 8);
    for (int i = 1; i <= count; ++i) {
        const std::string ch = FormatChannelNum(i);
        first_pass_addresses.push_back("/ch/" + ch + "/name");
        first_pass_addresses.push_back("/ch/" + ch + "/col");
        first_pass_addresses.push_back("/ch/" + ch + "/icon");
        first_pass_addresses.push_back("/ch/" + ch + "/in/conn/grp");
        first_pass_addresses.push_back("/ch/" + ch + "/in/conn/in");
        first_pass_addresses.push_back("/ch/" + ch + "/in/conn/altgrp");
        first_pass_addresses.push_back("/ch/" + ch + "/in/conn/altin");
        first_pass_addresses.push_back("/ch/" + ch + "/in/set/altsrc");
    }
    SendQueryBurst(first_pass_addresses);
    
    // Query USR routing before stereo detection so routed sources can be
    // resolve to the final /io/in/{grp}/{num}/mode endpoint.
    std::this_thread::sleep_for(std::chrono::milliseconds(kAsyncQuerySettlingMs));
    QueryUserSignalInputs(48);

    // Second pass: query source stereo mode via /io/in/{grp}/{num}/mode.
    Log("Second pass: querying source stereo modes via /io/in/{grp}/{num}/mode...");
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::set<std::pair<std::string, int>> mode_sources;
        {
            std::lock_guard<std::mutex> lock(data_mutex_);
            for (const auto& [channel_num, ch_info] : channel_data_) {
                (void)channel_num;
                if (ch_info.primary_source_group.empty() || ch_info.primary_source_group == "OFF" ||
                    ch_info.primary_source_input <= 0) {
                    continue;
                }
                mode_sources.insert(ResolveRoutingChainLocked(ch_info.primary_source_group,
                                                              ch_info.primary_source_input));
            }
        }

        std::vector<std::string> mode_addresses;
        mode_addresses.reserve(mode_sources.size());
        for (const auto& [grp, in] : mode_sources) {
            if (!IsDirectInputQueryGroup(grp) || in <= 0) {
                continue;
            }
            mode_addresses.push_back("/io/in/" + grp + "/" + std::to_string(in) + "/mode");
        }
        SendQueryBurst(mode_addresses);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}

void WingOSC::HandleOscMessage(const std::string& address, const void* data, size_t /* size */) {
    auto* msg = static_cast<const osc::ReceivedMessage*>(data);
    Log("OSC rx: " + address);

    // Capture user control color responses for later reuse (e.g. recording state color restore).
    {
        const std::string prefix = "/$ctl/user/";
        if (address.rfind(prefix, 0) == 0 &&
            (address.find("/name") != std::string::npos ||
             address.find("/txt") != std::string::npos ||
             address.find("/label") != std::string::npos)) {
            auto arg = msg->ArgumentsBegin();
            if (arg != msg->ArgumentsEnd()) {
                if (arg->IsString()) {
                    Log("UserCtl text rx: " + address + " = \"" + std::string(arg->AsString()) + "\"");
                } else if (arg->IsInt32()) {
                    Log("UserCtl text rx(int): " + address + " = " + std::to_string(arg->AsInt32()));
                } else if (arg->IsFloat()) {
                    Log("UserCtl text rx(float): " + address + " = " + std::to_string(arg->AsFloat()));
                }
            }
        }

        if (address.rfind(prefix, 0) == 0 &&
            (address.size() >= 4) &&
            (address.find("/col") != std::string::npos)) {
            // Patterns: /$ctl/user/<layer>/<button>/col and /$ctl/user/<layer>/<button>/config/col
            size_t pos = prefix.size();
            size_t slash1 = address.find('/', pos);
            if (slash1 != std::string::npos) {
                size_t slash2 = address.find('/', slash1 + 1);
                if (slash2 != std::string::npos) {
                    int layer = 0;
                    int button = 0;
                    try {
                        layer = std::stoi(address.substr(pos, slash1 - pos));
                        button = std::stoi(address.substr(slash1 + 1, slash2 - slash1 - 1));
                    } catch (...) {
                        layer = 0;
                        button = 0;
                    }
                    if (layer > 0 && button > 0) {
                        auto arg = msg->ArgumentsBegin();
                        if (arg != msg->ArgumentsEnd() && arg->IsInt32()) {
                            int color = arg->AsInt32();
                            std::lock_guard<std::mutex> lock(data_mutex_);
                            user_control_color_cache_[{layer, button}] = color;
                            Log("UserCtl color learned: layer=" + std::to_string(layer) +
                                " button=" + std::to_string(button) +
                                " color=" + std::to_string(color));
                        }
                    }
                }
            }
        }
    }

    // Meter responses can come back as scalar args or blobs.
    // We parse into linear values [0..1+] and cache for trigger logic.
    if (address.rfind("/meters", 0) == 0 || address == last_meter_address_) {
        std::vector<double> meter_values;
        try {
            auto arg = msg->ArgumentsBegin();
            if (arg != msg->ArgumentsEnd()) {
                if (arg->IsFloat()) {
                    double v = arg->AsFloat();
                    meter_values.push_back(v > 1.5 ? std::pow(10.0, v / 20.0) : std::max(0.0, v));
                } else if (arg->IsInt32()) {
                    double v = static_cast<double>(arg->AsInt32());
                    meter_values.push_back(v > 1.5 ? std::pow(10.0, v / 20.0) : std::max(0.0, v));
                } else if (arg->IsBlob()) {
                    const void* blob_data = nullptr;
                    osc::osc_bundle_element_size_t blob_size = 0;
                    arg->AsBlob(blob_data, blob_size);
                    const auto* bytes = static_cast<const uint8_t*>(blob_data);

                    // Heuristic 1: float array payload.
                    if (blob_size >= 4 && (blob_size % 4) == 0) {
                        for (osc::osc_bundle_element_size_t i = 0; i + 3 < blob_size; i += 4) {
                            uint32_t u = (static_cast<uint32_t>(bytes[i]) << 24) |
                                         (static_cast<uint32_t>(bytes[i + 1]) << 16) |
                                         (static_cast<uint32_t>(bytes[i + 2]) << 8) |
                                         static_cast<uint32_t>(bytes[i + 3]);
                            float f = 0.0f;
                            std::memcpy(&f, &u, sizeof(float));
                            if (std::isfinite(f)) {
                                meter_values.push_back(f > 1.5f ? std::pow(10.0, f / 20.0) : std::max(0.0f, f));
                            }
                        }
                    }

                    // Heuristic 2 fallback: signed 16-bit meter words.
                    if (meter_values.empty() && blob_size >= 2) {
                        for (osc::osc_bundle_element_size_t i = 0; i + 1 < blob_size; i += 2) {
                            int16_t s = static_cast<int16_t>((bytes[i] << 8) | bytes[i + 1]);
                            double norm = static_cast<double>(std::max<int16_t>(0, s)) / 32767.0;
                            meter_values.push_back(norm);
                        }
                    }
                }
            }
        } catch (osc::Exception& e) {
            Log(std::string("Error parsing meter OSC message: ") + e.what());
        }

        if (!meter_values.empty()) {
            std::lock_guard<std::mutex> lock(data_mutex_);
            last_meter_values_ = meter_values;
        }
        return;
    }
    
    // Helper: skip N args and return iterator
    auto skip = [&](osc::ReceivedMessage::const_iterator it, int n) {
        for (int i = 0; i < n && it != msg->ArgumentsEnd(); ++i) ++it;
        return it;
    };
    
    // Try to parse as channel message: /ch/N/param
    if (address.rfind("/ch/", 0) == 0) {
        size_t num_start = 4;
        size_t num_end   = address.find('/', num_start);
        if (num_end == std::string::npos) return;  // bare /ch/N with no param
        
        int channel_num;
        try {
            channel_num = std::stoi(address.substr(num_start, num_end - num_start));
        } catch (...) { return; }
        
        bool should_callback = false;
        ChannelInfo callback_data{};

        try {
            {
                std::lock_guard<std::mutex> lock(data_mutex_);

                // Ensure channel exists in map
                if (channel_data_.find(channel_num) == channel_data_.end()) {
                    channel_data_[channel_num] = ChannelInfo();
                    channel_data_[channel_num].channel_number = channel_num;
                }

                std::string param = address.substr(num_end + 1); // e.g. "name", "col", "icon", "clink", "in/conn/grp"

                if (param == "name") {
                    // Response: ,s  -> first arg is the name string
                    auto arg = msg->ArgumentsBegin();
                    if (arg != msg->ArgumentsEnd() && arg->IsString()) {
                        ParseChannelName(channel_num, arg->AsString());
                    }
                }
                else if (param == "col") {
                    // Response: ,sfi -> third arg (int) is the color index
                    auto arg = skip(msg->ArgumentsBegin(), 2);
                    if (arg != msg->ArgumentsEnd() && arg->IsInt32()) {
                        ParseChannelColor(channel_num, arg->AsInt32());
                    }
                }
                else if (param == "icon") {
                    // Response: ,sfi -> third arg (int) is the icon index
                    auto arg = skip(msg->ArgumentsBegin(), 2);
                    if (arg != msg->ArgumentsEnd() && arg->IsInt32()) {
                        channel_data_[channel_num].icon = std::to_string(arg->AsInt32());
                    }
                }
                // Routing parameters (virtual soundcheck)
                // NOTE: stereo_linked is set exclusively by /io/in/{grp}/{num}/mode responses
                else if (param == "in/conn/grp") {
                    // Primary source group: ,s -> string
                    auto arg = msg->ArgumentsBegin();
                    if (arg != msg->ArgumentsEnd() && arg->IsString()) {
                        channel_data_[channel_num].primary_source_group = arg->AsString();
                        Log("Channel " + std::to_string(channel_num) + " primary source: " + arg->AsString());
                    }
                }
                else if (param == "in/conn/in") {
                    // Primary source input: Wing may return either string-form (e.g. "fi25=")
                    // or numeric in alternate arg slots.
                    int parsed_from_string = -1;
                    int parsed_from_int = -1;

                    auto arg0 = msg->ArgumentsBegin();
                    if (arg0 != msg->ArgumentsEnd() && arg0->IsString()) {
                        std::string in_str = arg0->AsString();
                        int value = 0;
                        bool has_digit = false;
                        for (char c : in_str) {
                            if (std::isdigit(static_cast<unsigned char>(c))) {
                                value = value * 10 + (c - '0');
                                has_digit = true;
                            }
                        }
                        if (has_digit) {
                            parsed_from_string = value;
                        }
                    }

                    auto arg = skip(msg->ArgumentsBegin(), 2);
                    if (arg != msg->ArgumentsEnd() && arg->IsInt32()) {
                        parsed_from_int = arg->AsInt32();
                    }

                    // Use string arg for all groups - it reliably contains the correct input number.
                    // The int arg (arg2) gives wrong values for A-group inputs on this firmware.
                    int parsed_input = (parsed_from_string >= 0) ? parsed_from_string : parsed_from_int;

                    if (parsed_input >= 0) {
                        channel_data_[channel_num].primary_source_input = parsed_input;
                    }
                }
                else if (param == "in/conn/altgrp") {
                    // ALT source group: ,s -> string
                    auto arg = msg->ArgumentsBegin();
                    if (arg != msg->ArgumentsEnd() && arg->IsString()) {
                        channel_data_[channel_num].alt_source_group = arg->AsString();
                        Log("Channel " + std::to_string(channel_num) + " ALT source: " + arg->AsString());
                    }
                }
                else if (param == "in/conn/altin") {
                    // ALT source input: same mixed payload behavior as primary input.
                    int parsed_from_string = -1;
                    int parsed_from_int = -1;

                    auto arg0 = msg->ArgumentsBegin();
                    if (arg0 != msg->ArgumentsEnd() && arg0->IsString()) {
                        std::string in_str = arg0->AsString();
                        int value = 0;
                        bool has_digit = false;
                        for (char c : in_str) {
                            if (std::isdigit(static_cast<unsigned char>(c))) {
                                value = value * 10 + (c - '0');
                                has_digit = true;
                            }
                        }
                        if (has_digit) {
                            parsed_from_string = value;
                        }
                    }

                    auto arg = skip(msg->ArgumentsBegin(), 2);
                    if (arg != msg->ArgumentsEnd() && arg->IsInt32()) {
                        parsed_from_int = arg->AsInt32();
                    }

                    // Use string arg for all groups - it reliably contains the correct input number.
                    int parsed_input = (parsed_from_string >= 0) ? parsed_from_string : parsed_from_int;

                    if (parsed_input >= 0) {
                        channel_data_[channel_num].alt_source_input = parsed_input;
                    }
                }
                else if (param == "in/set/altsrc") {
                    // ALT source enabled: ,sfi -> third arg (int) 0=disabled 1=enabled
                    auto arg = skip(msg->ArgumentsBegin(), 2);
                    if (arg != msg->ArgumentsEnd() && arg->IsInt32()) {
                        channel_data_[channel_num].alt_source_enabled = (arg->AsInt32() != 0);
                        Log("Channel " + std::to_string(channel_num) + " ALT " +
                            (channel_data_[channel_num].alt_source_enabled ? "enabled" : "disabled"));
                    }
                }

                should_callback = (channel_callback_ != nullptr);
                if (should_callback) {
                    callback_data = channel_data_[channel_num];
                }
            }

            if (should_callback) {
                channel_callback_(callback_data);
            }
        }
        catch (osc::Exception& e) {
            Log(std::string("Error parsing OSC message: ") + e.what());
        }
        return;
    }
    
    // Handle /io/in/{grp}/{num}/mode responses: source stereo status.
    // mode = "M" (mono), "ST" (stereo), "MS" (mid-side).
    // Map responses back to channels using both raw and resolved source paths.
    {
        std::vector<ChannelInfo> callbacks;
        const std::string kPrefix = "/io/in/";
        if (address.compare(0, kPrefix.size(), kPrefix) == 0) {
            size_t grp_start = kPrefix.size();
            size_t grp_end = address.find('/', grp_start);
            if (grp_end != std::string::npos) {
                size_t num_start = grp_end + 1;
                size_t num_end = address.find('/', num_start);
                if (num_end != std::string::npos) {
                    std::string io_param = address.substr(num_end + 1);
                    if (io_param == "mode") {
                        std::string src_grp = address.substr(grp_start, grp_end - grp_start);
                        int src_num = 0;
                        try { src_num = std::stoi(address.substr(num_start, num_end - num_start)); }
                        catch (...) {}
                        if (src_num > 0) {
                            auto* msg_local = static_cast<const osc::ReceivedMessage*>(data);
                            auto arg = msg_local->ArgumentsBegin();
                            if (arg != msg_local->ArgumentsEnd() && arg->IsString()) {
                                std::string mode_str = arg->AsString();
                                bool is_stereo = (mode_str == "ST" || mode_str == "MS");
                                std::lock_guard<std::mutex> lock(data_mutex_);
                                for (auto& [ch_num, ch_info] : channel_data_) {
                                    auto resolved = ResolveRoutingChainLocked(ch_info.primary_source_group,
                                                                              ch_info.primary_source_input);
                                    if ((ch_info.primary_source_group == src_grp &&
                                         ch_info.primary_source_input == src_num) ||
                                        (resolved.first == src_grp &&
                                         resolved.second == src_num)) {
                                        ch_info.stereo_linked = is_stereo;
                                        if (channel_callback_) {
                                            callbacks.push_back(ch_info);
                                        }
                                    }
                                }
                            }
                        }
                        for (const auto& ch_info : callbacks) {
                            channel_callback_(ch_info);
                        }
                        return;
                    }
                }
            }
        }
    }

    // Try to parse as input source name message: /io/in/{grp}/{n}/name
    {
        const std::string kPrefix = "/io/in/";
        if (address.rfind(kPrefix, 0) == 0) {
            size_t grp_start = kPrefix.size();
            size_t grp_end = address.find('/', grp_start);
            if (grp_end != std::string::npos) {
                std::string grp = address.substr(grp_start, grp_end - grp_start);
                size_t num_start = grp_end + 1;
                size_t num_end = address.find('/', num_start);
                if (num_end != std::string::npos) {
                    int in_num = 0;
                    try {
                        in_num = std::stoi(address.substr(num_start, num_end - num_start));
                    } catch (...) {
                        in_num = 0;
                    }

                    if (in_num > 0) {
                        std::string param = address.substr(num_end + 1);
                        if (param == "name" &&
                            grp != "USB" &&
                            grp != "CRD" &&
                            grp != "CH") {
                            auto* msg_local = static_cast<const osc::ReceivedMessage*>(data);
                            auto arg = msg_local->ArgumentsBegin();
                            if (arg != msg_local->ArgumentsEnd() && arg->IsString()) {
                                std::lock_guard<std::mutex> lock(data_mutex_);
                                input_source_names_[grp + ":" + std::to_string(in_num)] = arg->AsString();
                            }
                            return;
                        }
                    }
                }
            }
        }
    }

    // Try to parse as User Signal input message: /io/in/USR/N/user/grp or /io/in/USR/N/user/in
    // Correct paths discovered from Wing object model (Patrick-Gilles Maillot manual method)
    if (address.rfind("/io/in/USR/", 0) == 0) {
        size_t num_start = 11;  // After "/io/in/USR/"
        size_t num_end = address.find('/', num_start);
        if (num_end == std::string::npos) return;  // No parameter specified
        
        int usr_num;
        try {
            usr_num = std::stoi(address.substr(num_start, num_end - num_start));
        } catch (...) { return; }
        
        std::lock_guard<std::mutex> lock(data_mutex_);
        
        try {
            std::string param = address.substr(num_end + 1); // e.g. "user/grp", "user/in"
            
            if (param == "user/grp") {
                // USR input source group: ,s -> string
                auto arg = msg->ArgumentsBegin();
                if (arg != msg->ArgumentsEnd() && arg->IsString()) {
                    std::string src_group = arg->AsString();
                    // Initialize or update the USR routing entry
                    if (usr_routing_data_.find(usr_num) == usr_routing_data_.end()) {
                        usr_routing_data_[usr_num] = {src_group, 0};
                    } else {
                        usr_routing_data_[usr_num].first = src_group;
                    }
                    Log("✓ USR:" + std::to_string(usr_num) + " sources from " + src_group + ":?");
                }
            }
            else if (param == "user/in") {
                // USR input source number: ,s -> string (Wing returns it as string, not int)
                auto arg = msg->ArgumentsBegin();
                if (arg != msg->ArgumentsEnd() && arg->IsString()) {
                    std::string in_str = arg->AsString();
                    // Parse the input number from string (format like "fi8=")
                    // Extract digits from the string
                    int src_input = 0;
                    for (char c : in_str) {
                        if (std::isdigit(c)) {
                            src_input = src_input * 10 + (c - '0');
                        }
                    }
                    
                    // Initialize or update the USR routing entry
                    if (usr_routing_data_.find(usr_num) == usr_routing_data_.end()) {
                        usr_routing_data_[usr_num] = {"", src_input};
                    } else {
                        usr_routing_data_[usr_num].second = src_input;
                    }
                    Log("✓ USR:" + std::to_string(usr_num) + " sources from ?:" + std::to_string(src_input));
                }
            }

        }
        catch (osc::Exception& e) {
            Log(std::string("Error parsing USR OSC message: ") + e.what());
        }
        return;
    }
}

void WingOSC::ParseChannelName(int channel_num, const std::string& value) {
    if (!value.empty()) {
        channel_data_[channel_num].name = value;
    }
    Log("Channel " + std::to_string(channel_num) + ": " + channel_data_[channel_num].name);
}

void WingOSC::ParseChannelColor(int channel_num, int value) {
    channel_data_[channel_num].color = value;
    Log("Channel " + std::to_string(channel_num) + " color: " + std::to_string(value));
}

bool WingOSC::GetChannelInfo(int channel_num, ChannelInfo& info) const {
    std::lock_guard<std::mutex> lock(data_mutex_);
    auto it = channel_data_.find(channel_num);
    if (it != channel_data_.end()) {
        info = it->second;
        return true;
    }
    return false;
}

} // namespace WingConnector
