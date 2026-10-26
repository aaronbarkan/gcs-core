#include "TelemetryGateway.h"
#include "TelemetryProcessor.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

TelemetryGateway::TelemetryGateway(StateStore& store)
    : store_(store), socket_(io_, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 14550)) {
    socket_.non_blocking(true);
}
TelemetryGateway::~TelemetryGateway() { Stop(); }
void TelemetryGateway::Start() {
    running_ = true;
    worker_ = std::thread(&TelemetryGateway::Run, this);
}
void TelemetryGateway::Stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}
void TelemetryGateway::Run() {
    try {
        unsigned char buffer[65536];
        mavlink_message_t message{};
        mavlink_status_t status{};
        TelemetryProcessor processor;
        bool was_live = false;
        while (running_) {
            // Check even when traffic is continuous but contains no valid positions.
            const auto before = store_.GetSnapshot();
            if (was_live && before.stale) {
                std::cout << "LINK STALE - no position received for >2 seconds" << std::endl;
                was_live = false;
            }
            boost::asio::ip::udp::endpoint sender;
            boost::system::error_code error;
            const auto count = socket_.receive_from(boost::asio::buffer(buffer), sender, 0, error);
            if (error && error != boost::asio::error::would_block &&
                error != boost::asio::error::try_again) {
                throw boost::system::system_error(error);
            }
            if (!error) {
                for (std::size_t i = 0; i < count; ++i) {
                    if (mavlink_parse_char(MAVLINK_COMM_0, buffer[i], &message, &status) &&
                        message.sysid == 1 && message.compid == 1 &&
                        message.msgid == MAVLINK_MSG_ID_GLOBAL_POSITION_INT) {
                        mavlink_global_position_int_t position{};
                        mavlink_msg_global_position_int_decode(&message, &position);
                        TelemetryState value;
                        if (!processor.Convert(position, value)) continue;
                        store_.Update(value);
                        const auto snapshot = store_.GetSnapshot();
                        std::ostringstream line;
                        line << "TELEMETRY rev=" << snapshot.revision
                             << std::fixed << std::setprecision(7)
                             << " lat=" << value.latitude_deg
                             << " lon=" << value.longitude_deg
                             << std::setprecision(2)
                             << " alt_msl_m=" << value.altitude_msl_m
                             << " rel_alt_m=" << value.relative_alt_m
                             << " speed_mps=" << value.speed_mps
                             << " hdg_deg=" << value.heading_deg;
                        std::cout << line.str() << std::endl;
                        if (!was_live) {
                            std::cout << "LINK LIVE" << std::endl;
                            was_live = true;
                        }
                    }
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    } catch (const std::exception& error) {
        std::cout << "ERROR UDP receiver: " << error.what() << std::endl;
        failed_ = true;
        running_ = false;
    }
}
