// Exercise report creation and incoming peer-log attachment without the game.
#include "mgmp_peerlog.h"
#include "mgmp_net.h"
#include "mgmp_log.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdio>

namespace mgmp {
NetRole net_role() { return NetRole::Host; }
bool net_active() { return true; }
uint8_t net_self() { return 0; }
uint8_t net_peer_count() { return 2; }
bool net_peer_ids(uint8_t* ids, uint8_t cap) {
    if (cap < 2) return false;
    ids[0] = 0; ids[1] = 1; return true;
}
bool net_send_choice(const ChoiceMsg&) { return true; }
bool net_send_logfile(const LogFileMsg&) { return true; }
}

static std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
        ("mgmp_peerlog_test_" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(dir);
    mgmp::log_init((dir / "local.log").c_str(), false);
    mgmp::log_set_turn(7);
    mgmp::log_line("APPLY", "test recent action");
    mgmp::log_line("LOCKSTEP", "DIFF cat 42 dead 1 | 0");
    mgmp::peerlog_halt_report("turn 7 hash mismatch (state only)", 42, 7);
    const fs::path report = fs::u8path(mgmp::peerlog_report_path());
    auto contents = read(report / "report.txt");
    bool ok = !report.empty() && contents.find("test recent action") != std::string::npos &&
        contents.find("DIFF cat 42") != std::string::npos &&
        contents.find("peer 1 pending") != std::string::npos &&
        contents.find("peer 2") == std::string::npos &&
        read(report / "local.log").find("test recent action") != std::string::npos;
    mgmp::LogFileMsg message{};
    uint8_t payload[] = "peer evidence";
    message.data = payload; message.size = sizeof(payload) - 1;
    strcpy_s(message.reason, "halt"); strcpy_s(message.name, "client.log");
    mgmp::peerlog_on_message(message, 1);
    contents = read(report / "report.txt");
    ok = ok && read(report / "peer_1.log") == "peer evidence" &&
        contents.find("peer 1 received") != std::string::npos;
    mgmp::log_shutdown();
    fs::remove_all(dir);
    if (!ok) std::fputs("HALT report integration check failed\n", stderr);
    return ok ? 0 : 1;
}
