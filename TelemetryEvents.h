#pragma once
#include <common/mavlink.h>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// Values are owned, never views into the socket's reusable receive buffer.
struct TelemetryEvent {
    enum class Kind {
        RawDatagram,      // Every successful receive, before parsing or filtering.
        Message,          // Valid framing/CRC and accepted sequence; any message type.
        SequenceRejected, // Duplicate or backward/ambiguous sequence.
        Tick,             // Timer notification, independent of incoming traffic.
        ReceiveError      // Fatal gateway error; Failed() is also set.
    };
    enum class Rejection { None, Duplicate, OutOfOrder };
    Kind kind = Kind::Tick;
    Rejection rejection = Rejection::None;
    // Receive-completion time for packet events; creation time for ticks/errors.
    std::chrono::steady_clock::time_point received_at;
    std::string sender_address;
    std::uint16_t sender_port = 0;
    std::vector<std::uint8_t> bytes; // RawDatagram only, including empty payloads.
    mavlink_message_t message{}; // Message and sequence events only.
    std::uint8_t expected_sequence = 0;
    unsigned missing_messages = 0; // Forward gap estimate on accepted Message events.
    std::string error; // ReceiveError only.
};
