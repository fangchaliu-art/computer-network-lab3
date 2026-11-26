// Lab 3: configuration parsing, validation and display.
#include "config.h"

#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>
#include <unordered_map>

namespace fs = std::filesystem;

namespace lab3 {

void print_help() {
    std::cout << R"HELP(Lab 3 - Reliable UDP file transfer: configuration module

Usage:
  lab3.exe send --file PATH [options]
  lab3.exe receive --output PATH [options]
  lab3.exe --help

Options (use separate option and value, e.g. --window 16):
  --bind-ip IPv4          Local IPv4 address (default: 127.0.0.1)
  --bind-port PORT        Local port (send: 9001, receive: 9002)
  --router-ip IPv4        Router IPv4 address (default: 127.0.0.1)
  --router-port PORT      Router port (default: 9000)
  --file PATH            Input file; required for send mode
  --output PATH          New output file; required for receive mode
  --window N             Fixed window, packets, 1..1024 (default: 16)
  --payload N            Data bytes per packet, 1..14976 (default: 1200)
  --rto-ms N             Retransmission timeout, 1..3600000 (default: 1000)
  --max-retries N        Retransmissions per packet, 0..10000 (default: 20)
  --idle-timeout-ms N    No-progress timeout, >= RTO, <= 86400000
                        (default: 120000)
  --log-level LEVEL      info or debug (default: info)
  --check                Validate and print config; do not start transfer
  --connection-test      Establish/close an empty test session; no file transfer
  --help, -h             Show help

Both peers must use the same window and payload settings for now.
Loss and delay are configured in the teacher's Router.exe.
Router server address must match the receiver's bind address and port.
--check performs no socket operations and never creates output files.
)HELP";
}

namespace {
//把字符串转换成整数，并检查范围
std::uint32_t number(const std::string& key, const std::string& value,
                     std::uint32_t minimum, std::uint32_t maximum) {
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() ||
        result < minimum || result > maximum) {
        throw ConfigError(key + " must be an integer in [" +
                          std::to_string(minimum) + ", " +
                          std::to_string(maximum) + "]");
    }
    return result;
}
// 检查 IPv4 地址格式
std::string ipv4(const std::string& key, const std::string& value) {
    std::size_t begin = 0;
    for (int part = 0; part < 4; ++part) {
        const auto end = value.find('.', begin);
        //检查点号数量是否正确
        if ((part < 3 && end == std::string::npos) ||
            (part == 3 && end != std::string::npos)) {
            throw ConfigError(key + " must be a numeric IPv4 address");
        }
        //截取当前段
        const auto octet = value.substr(begin, end == std::string::npos
                                                 ? end : end - begin);
        if (octet.size() > 1 && octet.front() == '0') {
            throw ConfigError(key + " must not contain leading-zero octets");
        }
        number(key + " octet", octet, 0, 255);
        if (part < 3) begin = end + 1;
    }
    return value;
}

} // namespace

//建立默认配置，确定工作模式
Config parse_config(const std::vector<std::string>& args) {
    if (args.empty()) throw ConfigError("first argument must be send or receive");
    Config config;
    //检查第一个参数
    if (args.front() == "send") {
        config.mode = Mode::Send;
    } else if (args.front() == "receive") {
        config.mode = Mode::Receive;
        config.bind_port = 9002;
    } else {
        throw ConfigError("first argument must be send or receive");
    }

    const std::vector<std::string> value_options = {
        "--bind-ip", "--bind-port", "--router-ip", "--router-port",
        "--file", "--output", "--window", "--payload", "--rto-ms",
        "--max-retries", "--idle-timeout-ms", "--log-level"
    };
    //逐项识别选项，将带值的参数存入 options
    std::unordered_map<std::string, std::string> options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& key = args[i];
        if (key == "--connection-test") {
            if (config.connection_test) throw ConfigError("duplicate option: --connection-test");
            config.connection_test = true;
            continue;
        }
        if (key == "--check") {
            if (config.check_only) throw ConfigError("duplicate option: --check");
            config.check_only = true;
            continue;
        }
        bool known = false;
        for (const auto& option : value_options) known = known || key == option;
        if (!known) throw ConfigError("unknown option: " + key);
        if (options.count(key)) throw ConfigError("duplicate option: " + key);
        if (i + 1 >= args.size() || args[i + 1].rfind("--", 0) == 0) {
            throw ConfigError("missing value for " + key);
        }
        const auto value = args[++i];
        if (value.empty()) throw ConfigError("empty value for " + key);
        options.emplace(key, value);
    }

    const auto get_number = [&](const char* key, std::uint32_t fallback,
                                std::uint32_t low, std::uint32_t high) {
        const auto it = options.find(key);
        return it == options.end() ? fallback : number(key, it->second, low, high);
    };
    if (options.count("--bind-ip"))
        config.bind_ip = ipv4("--bind-ip", options.at("--bind-ip"));
    if (options.count("--router-ip"))
        config.router_ip = ipv4("--router-ip", options.at("--router-ip"));
    const auto router_first_octet = std::stoi(config.router_ip.substr(0, config.router_ip.find('.')));
    if (router_first_octet == 0 || router_first_octet >= 224)
        throw ConfigError("--router-ip must be a unicast destination address");
    config.bind_port = static_cast<std::uint16_t>(
        get_number("--bind-port", config.bind_port, 1, 65535));
    config.router_port = static_cast<std::uint16_t>(
        get_number("--router-port", config.router_port, 1, 65535));
    // A wildcard bind would also occupy the router's port on this computer.
    if (config.bind_port == config.router_port &&
        (config.bind_ip == config.router_ip || config.bind_ip == "0.0.0.0"))
        throw ConfigError("local and router endpoints must not conflict");

    config.window = get_number("--window", config.window, 1, kMaxWindow);
    config.payload = get_number("--payload", config.payload, 1,
                                kRouterMaxPacketBytes - kHeaderBytes);
    config.rto_ms = get_number("--rto-ms", config.rto_ms, 1, 3600000);
    config.max_retries = get_number("--max-retries", config.max_retries, 0, 10000);
    config.idle_timeout_ms = get_number("--idle-timeout-ms", config.idle_timeout_ms,
                                       1, 86400000);
    if (config.idle_timeout_ms < config.rto_ms)
        throw ConfigError("--idle-timeout-ms must be at least --rto-ms");
    if (options.count("--log-level")) config.log_level = options.at("--log-level");
    if (config.log_level != "info" && config.log_level != "debug")
        throw ConfigError("--log-level must be info or debug");

    if (config.mode == Mode::Send) {
        if (options.count("--output")) throw ConfigError("--output is for receive mode only");
        if (!options.count("--file")) throw ConfigError("send mode requires --file PATH");
        config.file = fs::absolute(fs::u8path(options.at("--file"))).lexically_normal();
        if (!fs::is_regular_file(config.file))
            throw ConfigError("input is not an existing regular file: " + config.file.u8string());
        std::ifstream input(config.file, std::ios::binary);
        if (!input) throw ConfigError("input file cannot be opened for reading");
        config.file_bytes = fs::file_size(config.file);
        const auto packets = config.file_bytes / config.payload +
                             (config.file_bytes % config.payload != 0);
        if (packets > std::numeric_limits<std::uint32_t>::max())
            throw ConfigError("file requires too many packets for this session");
    } else {
        if (options.count("--file")) throw ConfigError("--file is for send mode only");
        if (!options.count("--output")) throw ConfigError("receive mode requires --output PATH");
        config.output = fs::absolute(fs::u8path(options.at("--output"))).lexically_normal();
        if (config.output.filename().empty() || fs::exists(config.output))
            throw ConfigError("output must name a new file (overwrite is disabled)");
        if (!fs::is_directory(config.output.parent_path()))
            throw ConfigError("output parent directory does not exist");
    }
    return config;
}

void print_config(const Config& config) {
    std::cout << "[CONFIG] mode=" << (config.mode == Mode::Send ? "send" : "receive")
              << "\n[CONFIG] local=" << config.bind_ip << ':' << config.bind_port
              << "\n[CONFIG] router=" << config.router_ip << ':' << config.router_port
              << "\n[CONFIG] window_packets=" << config.window
              << " payload_bytes=" << config.payload
              << " max_data_datagram_bytes=" << config.payload + kHeaderBytes
              << "\n[CONFIG] rto_ms=" << config.rto_ms
              << " max_retries=" << config.max_retries
              << " idle_timeout_ms=" << config.idle_timeout_ms
              << "\n[CONFIG] log_level=" << config.log_level << '\n';
    if (config.mode == Mode::Send) {
        const auto packets = config.file_bytes / config.payload +
                             (config.file_bytes % config.payload != 0);
        std::cout << "[CONFIG] input=" << config.file.u8string()
                  << "\n[CONFIG] file_bytes=" << config.file_bytes
                  << " data_packets=" << packets << '\n';
    } else {
        std::cout << "[CONFIG] output=" << config.output.u8string() << '\n';
    }
    std::cout << "[CONFIG] loss and delay: configure Router.exe separately\n";
}

} // namespace lab3
