#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
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

volatile std::sig_atomic_t g_stop = 0;

void HandleSignal(int) {
    g_stop = 1;
}

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
        } else if (arg->IsInt64()) {
            out << arg->AsInt64();
        } else if (arg->IsDouble()) {
            out << std::fixed << std::setprecision(3) << arg->AsDouble();
        } else if (arg->IsNil()) {
            out << "nil";
        } else if (arg->IsBlob()) {
            const void* blob_data = nullptr;
            osc::osc_bundle_element_size_t blob_size = 0;
            arg->AsBlob(blob_data, blob_size);
            const auto* bytes = static_cast<const uint8_t*>(blob_data);
            out << "blob[" << blob_size << "] ";
            for (osc::osc_bundle_element_size_t i = 0; i < blob_size; ++i) {
                if (i > 0) {
                    out << ' ';
                }
                out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]) << std::dec;
            }
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
        if (!address.address.empty()) {
            SendOscQueryPacket(sock, dest, address, local_port);
        }
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

std::string FormatChannel(int channel_number) {
    char formatted[8];
    std::snprintf(formatted, sizeof(formatted), "%02d", channel_number);
    return formatted;
}

std::vector<std::string> BuildCandidateAddresses(int channel_number,
                                                 const std::string& source_group,
                                                 int source_input,
                                                 std::vector<QueryRequest>& node_requests_out) {
    const std::string ch = FormatChannel(channel_number);
    std::vector<std::string> addresses{
        "/ch/" + ch + "/name",
        "/ch/" + ch + "/col",
        "/ch/" + ch + "/icon",
        "/ch/" + ch + "/clink",
        "/ch/" + ch + "/$name",
        "/ch/" + ch + "/$col",
        "/ch/" + ch + "/$icon",
        "/ch/" + ch + "/config/name",
        "/ch/" + ch + "/config/col",
        "/ch/" + ch + "/config/color",
        "/ch/" + ch + "/config/icon",
        "/ch/" + ch + "/config/source",
        "/ch/" + ch + "/config/link",
        "/ch/" + ch + "/in/conn/grp",
        "/ch/" + ch + "/in/conn/in",
        "/ch/" + ch + "/in/conn/altgrp",
        "/ch/" + ch + "/in/conn/altin",
        "/ch/" + ch + "/in/set/altsrc",
    };

    if (!source_group.empty() && source_input > 0) {
        addresses.push_back("/io/in/" + source_group + "/" + std::to_string(source_input) + "/name");
        addresses.push_back("/io/in/" + source_group + "/" + std::to_string(source_input) + "/col");
        addresses.push_back("/io/in/" + source_group + "/" + std::to_string(source_input) + "/icon");
        addresses.push_back("/io/in/" + source_group + "/" + std::to_string(source_input) + "/config/name");
        addresses.push_back("/io/in/" + source_group + "/" + std::to_string(source_input) + "/config/col");
        addresses.push_back("/io/in/" + source_group + "/" + std::to_string(source_input) + "/config/icon");
        addresses.push_back("/io/in/" + source_group + "/" + std::to_string(source_input) + "/mode");
    }

    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());

    const std::vector<std::string> node_candidates{
        "/ch/" + ch,
        "/ch/" + ch + "/config",
        "/ch/" + ch + "/in",
        "/ch/" + ch + "/in/conn",
        "/ch/" + ch + "/icon",
    };
    for (const auto& candidate : node_candidates) {
        node_requests_out.push_back(QueryRequest{candidate, true, {0xdd}});
    }

    return addresses;
}

int ParseEmbeddedInt(const std::string& value) {
    int parsed = 0;
    bool has_digit = false;
    for (char c : value) {
        if (std::isdigit(static_cast<unsigned char>(c))) {
            parsed = parsed * 10 + (c - '0');
            has_digit = true;
        } else if (has_digit) {
            break;
        }
    }
    return has_digit ? parsed : 0;
}

void ExtractCurrentSource(const std::map<std::string, ProbeReply>& replies,
                          int channel_number,
                          std::string& source_group_out,
                          int& source_input_out) {
    const std::string ch = FormatChannel(channel_number);
    const std::string group_path = "/ch/" + ch + "/in/conn/grp";
    const std::string input_path = "/ch/" + ch + "/in/conn/in";

    auto group_it = replies.find(group_path);
    if (group_it != replies.end()) {
        source_group_out = group_it->second.rendered;
        source_group_out.erase(std::remove(source_group_out.begin(), source_group_out.end(), '"'),
                               source_group_out.end());
    }

    auto input_it = replies.find(input_path);
    if (input_it != replies.end()) {
        source_input_out = ParseEmbeddedInt(input_it->second.rendered);
    }
}

std::string TimestampNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &time);
#else
    localtime_r(&time, &local_tm);
#endif
    std::ostringstream out;
    out << std::put_time(&local_tm, "%H:%M:%S");
    return out.str();
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

    std::signal(SIGINT, HandleSignal);

    int channel_number = 1;
    if (argc >= 2) {
        channel_number = std::atoi(argv[1]);
    }
    if (channel_number <= 0) {
        std::cerr << "Usage: wing_osc_probe <channel-number>\n";
        return 1;
    }

    const std::string wing_ip = LoadWingIpFromConfig();
    std::cout << "Polling WING at " << wing_ip << ":2223 for CH" << channel_number << "\n";
    std::cout << "Press Ctrl+C to stop.\n" << std::flush;

    std::map<std::string, std::string> last_seen;
    std::string source_group;
    int source_input = 0;

    while (!g_stop) {
        std::vector<QueryRequest> requests;
        std::vector<QueryRequest> node_requests;
        const auto plain_addresses = BuildCandidateAddresses(channel_number, source_group, source_input, node_requests);
        requests.reserve(plain_addresses.size() + node_requests.size());
        for (const auto& address : plain_addresses) {
            requests.push_back(QueryRequest{address, false, {}});
        }
        requests.insert(requests.end(), node_requests.begin(), node_requests.end());

        const auto replies = QueryOscAddresses(wing_ip, 2223, requests, 300, 60);
        ExtractCurrentSource(replies, channel_number, source_group, source_input);

        for (const auto& [address, reply] : replies) {
            auto previous = last_seen.find(address);
            if (previous == last_seen.end() || previous->second != reply.rendered) {
                std::cout << "[" << TimestampNow() << "] " << address << " -> " << reply.rendered << "\n";
                std::cout.flush();
                last_seen[address] = reply.rendered;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(350));
    }

#if defined(_WIN32)
    WSACleanup();
#endif
    return 0;
}
