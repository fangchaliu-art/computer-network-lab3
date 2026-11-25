#include "config.h"

#include <iostream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace lab3 {

int run(const Config& config) {
    //打印最终采用的配置打印
    print_config(config);
    if (config.check_only) {
        std::cout << "[CHECK] configuration valid; no socket or output file created\n";
        return 0;
    }
    std::cerr << "[NOT_IMPLEMENTED] configuration is ready; connection and transfer "
                 "modules are not implemented yet. Use --check to validate only.\n";
    return 4;
}

} 

std::vector<std::string> command_line_args(int argc, char* argv[]) {
    std::vector<std::string> args;
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int count = 0;
    auto* wide_args = CommandLineToArgvW(GetCommandLineW(), &count);
    //获取失败
    if (!wide_args) throw std::runtime_error("cannot read Windows command line");
    try {
        //从 1 开始，跳过第 0 个参数程序名
        for (int i = 1; i < count; ++i) {
            const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                                wide_args[i], -1, nullptr, 0, nullptr, nullptr);
            if (!size) throw std::runtime_error("cannot encode command line as UTF-8");
            //分配空间
            std::string value(static_cast<std::size_t>(size), '\0');
            //第二次调用才真正转换,指定了输出缓冲区value.data()
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_args[i], -1,
                                     value.data(), size, nullptr, nullptr))
                throw std::runtime_error("cannot encode command line as UTF-8");
            //移除转换结果里额外包含的末尾空字符
            value.pop_back();
            //把转换后的参数加入列表
            args.push_back(value);
        }
    } catch (...) {
        LocalFree(wide_args);
        throw;
    }
    LocalFree(wide_args);
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    return args;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    try {
        //负责整理参数，返回字符串列表
        const auto args = command_line_args(argc, argv);
        if (args.empty()) {
            lab3::print_help();
            return 0;
        }
        for (const auto& arg : args) {
            if (arg == "--help" || arg == "-h") {
                lab3::print_help();
                return 0;
            }
        }
        return lab3::run(lab3::parse_config(args));
    } catch (const lab3::ConfigError& error) {
        std::cerr << "[CONFIG_ERROR] " << error.what() << "\nUse --help for usage.\n";
        return 2;
    } catch (const fs::filesystem_error& error) {
        std::cerr << "[FILESYSTEM_ERROR] " << error.what() << '\n';
        return 3;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 3;
    }
}
