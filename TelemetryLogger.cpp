#include "TelemetryLogger.h"
#include "TelemetryGateway.h"
#include <ctime>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void Hex(std::ostream& out, const std::uint8_t* bytes, std::size_t size) {
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i)
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    out << std::dec;
}

// Keep each event on one line, including error text from external libraries.
void Escaped(std::ostream& out, const std::string& text) {
    for (unsigned char c : text) {
        if (c == '\\') out << "\\\\";
        else if (c >= 32 && c <= 126) out << c;
        else {
            out << "\\x";
            Hex(out, &c, 1);
        }
    }
}
}

TelemetryLogger::TelemetryLogger(TelemetryGateway& gateway, const std::string& path)
    : file_(path, std::ios::out | std::ios::trunc) {
    if (!file_) throw std::runtime_error("cannot open telemetry log: " + path);
    file_.exceptions(std::ios::failbit | std::ios::badbit);
    subscription_ = gateway.Subscribe([this](TelemetrySubscriptions::Event event) {
        if (failed_) return;
        try {
            Log(*event);
        } catch (const std::ios_base::failure& error) {
            failed_ = true;
            std::cerr << "Telemetry log write failed: " << error.what() << std::endl;
        }
    });
}

TelemetryLogger::~TelemetryLogger() {
    subscription_.Reset();
}

void TelemetryLogger::Log(const TelemetryEvent& event) {
    using Kind = TelemetryEvent::Kind;
    const char* id = "TICK";
    int severity = 7; // Debug; facility 1 (user-level messages).
    switch (event.kind) {
    case Kind::RawDatagram: id = "RAW_DATAGRAM"; break;
    case Kind::Message: id = "MESSAGE"; severity = 6; break;
    case Kind::SequenceRejected: id = "SEQUENCE_REJECTED"; severity = 4; break;
    case Kind::ReceiveError: id = "RECEIVE_ERROR"; severity = 3; break;
    case Kind::Tick: break;
    }

    // Syslog uses wall time at logging; the event's steady clock has no calendar epoch.
    const auto now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    file_ << '<' << 8 + severity << ">1 " << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ")
          << " - gcs-core " << getpid() << ' ' << id << " -";

    if (event.kind == Kind::RawDatagram || event.kind == Kind::Message ||
        event.kind == Kind::SequenceRejected) {
        file_ << " sender=";
        Escaped(file_, event.sender_address);
        file_ << ':' << event.sender_port;
    }
    if (event.kind == Kind::RawDatagram) {
        file_ << " bytes=" << event.bytes.size() << " hex=";
        Hex(file_, event.bytes.data(), event.bytes.size());
    } else if (event.kind == Kind::Message || event.kind == Kind::SequenceRejected) {
        const auto& message = event.message;
        file_ << " sysid=" << unsigned(message.sysid) << " compid=" << unsigned(message.compid)
              << " msgid=" << message.msgid << " seq=" << unsigned(message.seq)
              << " expected=" << unsigned(event.expected_sequence);
        if (event.kind == Kind::SequenceRejected)
            file_ << " reason=" << (event.rejection == TelemetryEvent::Rejection::Duplicate
                                        ? "duplicate" : "out_of_order");
        else
            file_ << " missing=" << event.missing_messages;
        file_ << " payload=";
        Hex(file_, reinterpret_cast<const std::uint8_t*>(_MAV_PAYLOAD(&message)), message.len);
        if (message.msgid == MAVLINK_MSG_ID_GLOBAL_POSITION_INT) {
            mavlink_global_position_int_t position{};
            mavlink_msg_global_position_int_decode(&message, &position);
            file_ << " lat_e7=" << position.lat << " lon_e7=" << position.lon
                  << " alt_mm=" << position.alt << " relative_alt_mm=" << position.relative_alt
                  << " vx_cms=" << position.vx << " vy_cms=" << position.vy
                  << " vz_cms=" << position.vz << " heading_cdeg=" << position.hdg;
        }
    } else if (event.kind == Kind::ReceiveError) {
        file_ << " error=";
        Escaped(file_, event.error);
    }
    file_ << std::endl;
}
