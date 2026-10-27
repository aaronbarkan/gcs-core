#include "TelemetryGateway.h"
#include "TelemetryLogger.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

using Kind = TelemetryEvent::Kind;
using namespace std::chrono;

void Check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
struct TempFile {
    char path[32] = "/tmp/gcs-logger-XXXXXX";
    TempFile() {
        const int fd = mkstemp(path);
        if (fd == -1) throw std::runtime_error("mkstemp failed");
        close(fd);
    }
    ~TempFile() { std::remove(path); }
};
std::string Read(const char* path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct Received {
    std::mutex mutex;
    std::condition_variable ready;
    unsigned raw = 0, ticks = 0;
    void Add(TelemetrySubscriptions::Event event) {
        std::lock_guard<std::mutex> lock(mutex);
        if (event->kind == Kind::RawDatagram) ++raw;
        if (event->kind == Kind::Tick) ++ticks;
        ready.notify_one();
    }
    void Wait(unsigned datagrams, unsigned timer_ticks = 0) {
        std::unique_lock<std::mutex> lock(mutex);
        Check(ready.wait_for(lock, seconds(3), [&] {
            return raw >= datagrams && ticks >= timer_ticks;
        }), "logger event timeout");
    }
};
void LogEvents(const char* path) {
    Received received;
    TelemetryGateway gateway(0);
    TelemetryLogger logger(gateway, path);
    Check(Read(path).empty(), "previous log was not truncated");
    auto observer = gateway.Subscribe([&](TelemetrySubscriptions::Event e) { received.Add(e); });
    gateway.Start();
    boost::asio::io_context io;
    boost::asio::ip::udp::socket socket(io, {boost::asio::ip::udp::v4(), 0});
    const boost::asio::ip::udp::endpoint target(boost::asio::ip::address_v4::loopback(), gateway.Port());
    for (unsigned seq : {10, 10, 9, 12}) {
        mavlink_status_t status{};
        status.current_tx_seq = seq;
        mavlink_message_t message{};
        mavlink_msg_global_position_int_pack_status(1, 1, &status, &message,
            0, 320000000, 348000000, 100000, 50000, -300, 400, 0, 9000);
        std::uint8_t bytes[MAVLINK_MAX_PACKET_LEN];
        const auto size = mavlink_msg_to_send_buffer(bytes, &message);
        socket.send_to(boost::asio::buffer(bytes, size), target);
    }
    const std::uint8_t malformed[] = {0, '\n', 255};
    socket.send_to(boost::asio::buffer(malformed), target);
    socket.send_to(boost::asio::buffer(malformed, 0), target);
    received.Wait(6, 1);
    gateway.Stop();
    Check(!logger.Failed(), "logger failed writing events");
    const auto contents = Read(path);
    const std::regex header(R"(^<(11|12|14|15)>1 [0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z - gcs-core [0-9]+ (RAW_DATAGRAM|MESSAGE|SEQUENCE_REJECTED|TICK|RECEIVE_ERROR) -($| .*))");
    std::istringstream lines(contents);
    std::string line;
    unsigned raw = 0, accepted = 0, rejected = 0;
    for (; std::getline(lines, line);) {
        Check(std::regex_match(line, header), "invalid syslog header or multiline event");
        if (line.find(" RAW_DATAGRAM ") != std::string::npos) ++raw;
        if (line.find(" MESSAGE ") != std::string::npos) ++accepted;
        if (line.find(" SEQUENCE_REJECTED ") != std::string::npos) ++rejected;
    }
    Check(raw == 6 && accepted == 2 && rejected == 2, "wrong logged event counts");
    Check(contents.find(" RAW_DATAGRAM ") < contents.find(" MESSAGE "), "raw logged after message");
    Check(contents.find("bytes=3 hex=000aff") != std::string::npos, "malformed datagram not preserved");
    Check(contents.find("bytes=0 hex=\n") != std::string::npos, "empty datagram not logged");
    Check(contents.find("reason=duplicate") != std::string::npos &&
          contents.find("reason=out_of_order") != std::string::npos, "missing rejection reasons");
    Check(contents.find("seq=12 expected=11 missing=1") != std::string::npos, "missing gap count");
    Check(contents.find("lat_e7=320000000 lon_e7=348000000") != std::string::npos,
          "missing position fields");
}

int main() {
    try {
        TempFile file;
        { std::ofstream old(file.path); old << "old run\n"; }
        LogEvents(file.path);
        Received received;
        TelemetryGateway gateway(0);
        {
            TelemetryLogger logger(gateway, file.path);
            Check(Read(file.path).empty(), "restart did not truncate log");
            gateway.Start();
        }
        const auto after_unsubscribe = Read(file.path);
        auto observer = gateway.Subscribe([&](TelemetrySubscriptions::Event e) { received.Add(e); });
        received.Wait(0, 2);
        Check(Read(file.path) == after_unsubscribe, "logger wrote after destruction");
        bool threw = false;
        try { TelemetryLogger invalid(gateway, std::string(file.path) + "/child"); }
        catch (const std::runtime_error&) { threw = true; }
        Check(threw, "file-open failure not reported");
        gateway.Stop();
#ifdef __linux__
        TelemetryGateway full_gateway(0);
        TelemetryLogger full(full_gateway, "/dev/full");
        Received errors;
        auto notice = full_gateway.Subscribe([&](TelemetrySubscriptions::Event e) { errors.Add(e); });
        full_gateway.Start();
        errors.Wait(0, 1);
        Check(full.Failed(), "file-write failure not reported");
        full_gateway.Stop();
#endif
        std::cout << "Syslog format, datagrams, messages, rejections, truncation and lifetime passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
