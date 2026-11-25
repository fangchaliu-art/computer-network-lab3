#ifndef LAB3_CONFIG_H
#define LAB3_CONFIG_H

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace lab3 {

inline constexpr std::uint32_t kHeaderBytes = 24;
inline constexpr std::uint32_t kRouterMaxPacketBytes = 15000;
// An implementation resource limit, rather than a restriction of SR itself.
inline constexpr std::uint32_t kMaxWindow = 1024;

enum class Mode { Send, Receive };

struct Config {
    //当前程序是发送端还是接收端
    Mode mode = Mode::Send;
    //程序自己使用的地址和端口
    std::string bind_ip = "127.0.0.1";
    std::uint16_t bind_port = 9001;
    //报文要发往的路由程序地址和端口
    std::string router_ip = "127.0.0.1";
    std::uint16_t router_port = 9000;
    //发送端读取哪个文件
    std::filesystem::path file;
    //接收端把文件保存在哪里
    std::filesystem::path output;
    //一次最多允许多少个数据包处于发送窗口内
    std::uint32_t window = 16;
    //每个数据包最多放多少字节的文件内容
    std::uint32_t payload = 1200;
    //发出数据包后，等待确认多久才判定超时
    std::uint32_t rto_ms = 1000;
    //首次发送之外，最多允许重传多少次
    std::uint32_t max_retries = 20;
    //多长时间没有进展就结束等待
    std::uint32_t idle_timeout_ms = 120000;
    std::string log_level = "info";
    //是否只检查配置
    bool check_only = false;
    std::uintmax_t file_bytes = 0;
};

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void print_help();
//接收一组命令行参数，解析后返回一个 Config 配置对象
Config parse_config(const std::vector<std::string>& args);
//接收一个配置对象，将它的内容打印出来
void print_config(const Config& config);

} 

#endif // LAB3_CONFIG_H
