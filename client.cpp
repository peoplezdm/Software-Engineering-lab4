#include <chrono>
#include <clocale>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
using SocketType = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using SocketType = int;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

#include "des_operator.h"

void init_console_utf8() {
    std::setlocale(LC_ALL, "");
}

bool socket_startup() {
#ifdef _WIN32
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
    return true;
#endif
}

void socket_cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

void close_socket(SocketType sock) {
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

void shutdown_socket(SocketType sock) {
#ifdef _WIN32
    shutdown(sock, SD_BOTH);
#else
    shutdown(sock, SHUT_RDWR);
#endif
}

unsigned long get_process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

SocketType sock = INVALID_SOCKET;
const std::uint64_t kDesKey = 0x133457799BBCDFF1ULL;
DesOperator g_des(kDesKey);

std::string generate_default_username() {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    unsigned long pid = get_process_id();
    return "User" + std::to_string(pid) + "_" + std::to_string(now_ms % 1000000);
}

bool send_all(SocketType s, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = static_cast<int>(send(s, data + sent, len - sent, 0));
        if (n <= 0) {
            return false;
        }
        sent += n;
    }
    return true;
}

bool recv_all(SocketType s, char* data, int len) {
    int received = 0;
    while (received < len) {
        int n = static_cast<int>(recv(s, data + received, len - received, 0));
        if (n <= 0) {
            return false;
        }
        received += n;
    }
    return true;
}

bool send_encrypted(const std::string& plaintext) {
    std::vector<std::uint8_t> cipher = g_des.encrypt(plaintext);
    std::uint32_t net_len = htonl(static_cast<std::uint32_t>(cipher.size()));

    if (!send_all(sock, reinterpret_cast<const char*>(&net_len), sizeof(net_len))) {
        return false;
    }
    return cipher.empty() || send_all(sock, reinterpret_cast<const char*>(cipher.data()), static_cast<int>(cipher.size()));
}

std::string trim_right_spaces(const std::string& s) {
    std::size_t end = s.find_last_not_of(' ');
    if (end == std::string::npos) {
        return "";
    }
    return s.substr(0, end + 1);
}

std::string log_safe_text(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch == '\t') out += "\\t";
        else out.push_back(ch);
    }
    return out;
}

std::string format_decrypted_packet(const std::string& plaintext) {
    std::string normalized = plaintext;
    if (!normalized.empty() && normalized.back() == '\n') {
        normalized.pop_back();
    }
    if (normalized.size() < 96) {
        return log_safe_text(normalized);
    }

    std::string header = trim_right_spaces(normalized.substr(0, 32));
    std::string sender = trim_right_spaces(normalized.substr(32, 32));
    std::string target = trim_right_spaces(normalized.substr(64, 32));
    std::string body = normalized.substr(96);
    return "header=" + log_safe_text(header) + ", sender=" + log_safe_text(sender) +
           ", target=" + log_safe_text(target) + ", body=" + log_safe_text(body);
}

bool recv_encrypted(std::string& plaintext) {
    std::uint32_t net_len = 0;
    if (!recv_all(sock, reinterpret_cast<char*>(&net_len), sizeof(net_len))) {
        return false;
    }

    std::uint32_t len = ntohl(net_len);
    if (len == 0 || (len % 8) != 0) {
        return false;
    }

    std::vector<std::uint8_t> cipher(len);
    if (!recv_all(sock, reinterpret_cast<char*>(cipher.data()), static_cast<int>(cipher.size()))) {
        return false;
    }

    std::string cipher_hex = DesOperator::bytesToHex(cipher);
    if (cipher_hex.size() > 96) {
        cipher_hex = cipher_hex.substr(0, 96) + "...";
    }

    try {
        plaintext = g_des.decrypt(cipher);
    } catch (...) {
        return false;
    }

    std::cout << "[加密调试] 收包 " << len << "B\n"
              << "  密文(hex): " << cipher_hex << "\n"
              << "  解密后: " << format_decrypted_packet(plaintext) << "\n";
    return true;
}

std::string pack(const std::string& h, const std::string& s,
                 const std::string& t, const std::string& b) {
    auto pad = [](std::string x, size_t n) {
        x.resize(n, ' ');
        return x;
    };
    return pad(h, 32) + pad(s, 32) + pad(t, 32) + b + "\n";
}

void receive_thread_impl() {
    while (true) {
        std::string msg;
        if (!recv_encrypted(msg)) {
            std::cout << "与服务器断开连接" << std::endl;
            return;
        }

        if (!msg.empty() && msg.back() == '\n') {
            msg.pop_back();
        }

        auto trim = [](std::string s) {
            while (!s.empty() && s.back() == ' ') s.pop_back();
            return s;
        };

        if (msg.size() < 32) continue;
        std::string header = trim(msg.substr(0, 32));
        std::string body = (msg.size() > 96) ? msg.substr(96) : "";

        if (header == "用户列表") {
            std::cout << "===== 在线用户 =====\n" << body << "==================\n";
        } else if (header == "群聊列表") {
            std::cout << "===== 已加入群聊 =====\n" << body << "====================\n";
        } else {
            std::cout << body;
        }
    }
}

#ifdef _WIN32
DWORD WINAPI receive_thread_proc(LPVOID) {
    receive_thread_impl();
    return 0;
}
#else
void* receive_thread_proc(void*) {
    receive_thread_impl();
    return nullptr;
}
#endif

int main() {
    init_console_utf8();

    std::string name;
    std::cout << "请输入用户名(留白将随机生成): ";
    std::getline(std::cin, name);
    if (name.empty()) {
        name = generate_default_username();
        std::cout << "随机生成用户名: " << name << std::endl;
    }

    if (!socket_startup()) {
        std::cerr << "socket 初始化失败\n";
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "创建 socket 失败\n";
        socket_cleanup();
        return 1;
    }

    std::string ip = "127.0.0.1";
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8888);

#ifdef _WIN32
    unsigned long ip_addr = inet_addr(ip.c_str());
    if (ip_addr == INADDR_NONE) {
        std::cerr << "服务器IP无效\n";
        close_socket(sock);
        socket_cleanup();
        return 1;
    }
    server_addr.sin_addr.s_addr = ip_addr;
#else
    if (inet_pton(AF_INET, ip.c_str(), &server_addr.sin_addr) != 1) {
        std::cerr << "服务器IP无效\n";
        close_socket(sock);
        socket_cleanup();
        return 1;
    }
#endif

    if (connect(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == SOCKET_ERROR) {
        std::cerr << "连接服务器失败\n";
        close_socket(sock);
        socket_cleanup();
        return 1;
    }

    send_encrypted(pack("LOGIN", name, "", ""));

#ifdef _WIN32
    HANDLE recv_handle = CreateThread(nullptr, 0, receive_thread_proc, nullptr, 0, nullptr);
    if (recv_handle == nullptr) {
        std::cerr << "创建接收线程失败\n";
        close_socket(sock);
        socket_cleanup();
        return 1;
    }
#else
    pthread_t recv_tid;
    if (pthread_create(&recv_tid, nullptr, receive_thread_proc, nullptr) != 0) {
        std::cerr << "创建接收线程失败\n";
        close_socket(sock);
        socket_cleanup();
        return 1;
    }
#endif

    while (true) {
        std::string line;
        std::getline(std::cin, line);

        if (line == "/quit") {
            send_encrypted(pack("LOGOUT", name, "", ""));
            break;
        } else if (line.rfind("/pmsg ", 0) == 0) {
            size_t sp = line.find(' ', 6);
            if (sp == std::string::npos) {
                std::cout << "格式错误, 输入 help 查看指令\n";
                continue;
            }
            send_encrypted(pack("PMSG", name, line.substr(6, sp - 6), line.substr(sp + 1)));
        } else if (line.rfind("/gcreate", 0) == 0) {
            size_t sp1 = line.find(' ', 8);
            size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : line.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) {
                std::cout << "格式错误: /gcreate <群名> <密码>\n";
                continue;
            }
            send_encrypted(pack("CREATE_GROUP", name, line.substr(sp1 + 1, sp2 - sp1 - 1), line.substr(sp2 + 1)));
        } else if (line.rfind("/join", 0) == 0) {
            size_t sp1 = line.find(' ', 5);
            size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : line.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) {
                std::cout << "格式错误: /join <群名> <密码>\n";
                continue;
            }
            send_encrypted(pack("JOIN_GROUP", name, line.substr(sp1 + 1, sp2 - sp1 - 1), line.substr(sp2 + 1)));
        } else if (line.rfind("/gmsg ", 0) == 0) {
            size_t sp = line.find(' ', 6);
            if (sp == std::string::npos) {
                std::cout << "格式错误: /gmsg <群名> <消息>\n";
                continue;
            }
            send_encrypted(pack("GROUP_MSG", name, line.substr(6, sp - 6), line.substr(sp + 1)));
        } else if (line.rfind("/leave ", 0) == 0) {
            send_encrypted(pack("LEAVE_GROUP", name, line.substr(7), ""));
        } else if (line == "/users") {
            send_encrypted(pack("LIST_USERS", name, "", ""));
        } else if (line == "/mygroups") {
            send_encrypted(pack("LIST_GROUPS", name, "", ""));
        } else if (line == "help") {
            std::cout << "===== 可用命令 =====\n"
                      << "/quit                     : 退出客户端\n"
                      << "/pmsg <用户> <消息>       : 私聊某个用户\n"
                      << "/gcreate <群名> <密码>    : 创建群聊\n"
                      << "/join <群名> <密码>       : 加入群聊\n"
                      << "/gmsg <群名> <消息>       : 群聊消息\n"
                      << "/leave <群名>             : 退出群聊\n"
                      << "/users                    : 显示所有在线用户\n"
                      << "/mygroups                 : 显示已加入群聊\n"
                      << "help                      : 显示帮助信息\n"
                      << "====================\n";
        } else {
            std::cout << "未知命令, 输入 help 查看所有指令\n";
        }
    }

    shutdown_socket(sock);
    close_socket(sock);

#ifdef _WIN32
    WaitForSingleObject(recv_handle, INFINITE);
    CloseHandle(recv_handle);
#else
    pthread_join(recv_tid, nullptr);
#endif

    socket_cleanup();
    return 0;
}
