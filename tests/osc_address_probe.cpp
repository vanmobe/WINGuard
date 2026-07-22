#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

namespace {

struct ProbeReply {
    std::string address;
    std::string rendered;
};

struct QueryRequest {
    std::string address;
    bool use_blob = false;
    std::vector<uint8_t> blob;
};

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
                          int recv_timeout_ms) {
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
                        const QueryRequest& request,
                        uint16_t reply_port) {
    char buffer[256];
    osc::OutboundPacketStream packet(buffer, sizeof(buffer));
    const std::string routed_address =
        (reply_port > 0) ? ("/%" + std::to_string(reply_port) + request.address) : request.address;
    packet << osc::BeginMessage(routed_address.c_str());
    if (request.use_blob) {
        packet << osc::Blob(request.blob.data(), static_cast<osc::osc_bundle_element_size_t>(request.blob.size()));
    }
    packet << osc::EndMessage;

    auto bytes_sent = sendto(sock, packet.Data(), static_cast<int>(packet.Size()), 0,
                             reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
#if defined(_WIN32)
    return bytes_sent != SOCKET_ERROR;
#else
    return bytes_sent >= 0;
#endif
}

std::string RenderReply(const osc::ReceivedMessage& msg) {
    std::ostringstream out;
    bool first = true;
    for (auto arg = msg.ArgumentsBegin(); arg != msg.ArgumentsEnd(); ++arg) {
        if (!first) {
            out << ", ";
        }
        first = false;
        if (arg->IsString()) {
            out << '"' << arg->AsString() << '"';
        } else if (arg->IsInt32()) {
            out << arg->AsInt32();
        } else if (arg->IsFloat()) {
            out << std::fixed << std::setprecision(3) << arg->AsFloat();
        } else if (arg->IsBool()) {
            out << (arg->AsBool() ? "true" : "false");
        } else if (arg->IsBlob()) {
            const void* blob_data = nullptr;
            osc::osc_bundle_element_size_t blob_size = 0;
            arg->AsBlob(blob_data, blob_size);
            out << "blob[" << blob_size << "]";
        } else {
            out << "<unhandled>";
        }
    }
    if (first) {
        out << "<empty>";
    }
    return out.str();
}

std::map<std::string, ProbeReply> QueryOscAddresses(const std::string& wing_ip,
                                                    uint16_t wing_port,
                                                    const std::vector<QueryRequest>& addresses,
                                                    int total_timeout_ms,
                                                    int idle_timeout_ms) {
    std::map<std::string, ProbeReply> replies;
    if (addresses.empty()) {
        return replies;
    }

    NativeSocket sock = kInvalidSocket;
    sockaddr_in dest{};
    if (!CreateUdpQuerySocket(wing_ip, wing_port, sock, dest, 20)) {
        return replies;
    }

    uint16_t local_port = 0;
    sockaddr_in local_addr{};
    socklen_t local_addr_len = sizeof(local_addr);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&local_addr), &local_addr_len) == 0) {
        local_port = ntohs(local_addr.sin_port);
    }

    for (const auto& address : addresses) {
        SendOscQueryPacket(sock, dest, address, local_port);
    }

    const auto started = std::chrono::steady_clock::now();
    auto last_reply = started;
    while (true) {
        char recv_buffer[2048];
        sockaddr_in from{};
#if defined(_WIN32)
        int from_len = sizeof(from);
        int received = recvfrom(sock, recv_buffer, sizeof(recv_buffer), 0,
                                reinterpret_cast<sockaddr*>(&from), &from_len);
        const bool got_packet = (received != SOCKET_ERROR);
#else
        socklen_t from_len = sizeof(from);
        ssize_t received = recvfrom(sock, recv_buffer, sizeof(recv_buffer), 0,
                                    reinterpret_cast<sockaddr*>(&from), &from_len);
        const bool got_packet = (received >= 0);
#endif

        if (got_packet) {
            try {
                osc::ReceivedPacket packet(recv_buffer, static_cast<int>(received));
                if (packet.IsMessage()) {
                    osc::ReceivedMessage msg(packet);
                    ProbeReply reply;
                    reply.address = msg.AddressPattern();
                    reply.rendered = RenderReply(msg);
                    replies[reply.address] = reply;
                    last_reply = std::chrono::steady_clock::now();
                    if (static_cast<int>(replies.size()) >= static_cast<int>(addresses.size())) {
                        break;
                    }
                    continue;
                }
            } catch (...) {
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const auto total_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        const auto idle_elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reply).count();
        if (total_elapsed >= total_timeout_ms) {
            break;
        }
        if (!replies.empty() && idle_elapsed >= idle_timeout_ms) {
            break;
        }
    }

    CloseNativeSocket(sock);
    return replies;
}

std::string LoadWingIpFromConfig() {
    std::vector<std::string> candidate_paths;
    if (const char* home = std::getenv("HOME")) {
        candidate_paths.push_back(std::string(home) + "/.wingconnector/config.json");
    }
#if defined(_WIN32)
    if (const char* profile = std::getenv("USERPROFILE")) {
        candidate_paths.push_back(std::string(profile) + "/.wingconnector/config.json");
    }
#endif
    candidate_paths.push_back("install/config.json");

    for (const auto& path : candidate_paths) {
        std::ifstream in(path);
        if (!in.is_open()) {
            continue;
        }

        nlohmann::json config;
        in >> config;
        const std::string ip = config.value("wing_ip", std::string());
        if (!ip.empty()) {
            return ip;
        }
    }

    return "192.168.1.100";
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    if (argc < 2) {
        std::cerr << "Usage: osc_address_probe <osc-address> [<osc-address> ...]\n"
                     "       prefix with blob: to send a node query blob request\n";
        return 1;
    }

    std::vector<QueryRequest> addresses;
    std::vector<std::string> ordered_addresses;
    for (int i = 1; i < argc; ++i) {
        std::string value = argv[i];
        QueryRequest request;
        if (value.rfind("blob:", 0) == 0) {
            request.address = value.substr(5);
            request.use_blob = true;
            request.blob = {0xdd};
        } else {
            request.address = value;
        }
        ordered_addresses.push_back(request.address);
        addresses.push_back(request);
    }

    const std::string wing_ip = LoadWingIpFromConfig();
    const auto replies = QueryOscAddresses(wing_ip, 2223, addresses, 300, 60);
    for (const auto& address : ordered_addresses) {
        auto it = replies.find(address);
        if (it == replies.end()) {
            std::cout << address << " -> <no reply>\n";
        } else {
            std::cout << it->second.address << " -> " << it->second.rendered << "\n";
        }
    }

#if defined(_WIN32)
    WSACleanup();
#endif
    return 0;
}
