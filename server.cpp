#include <algorithm>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <list>
#include <sstream>
#include <string>
#include <vector>
#include <clocale>

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

std::string get_time_string() {
    auto t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string pack(const std::string& header, const std::string& sender,
                 const std::string& target, const std::string& body) {
    auto pad = [](std::string x, size_t n) {
        x.resize(n, ' ');
        return x;
    };
    return pad(header, 32) + pad(sender, 32) + pad(target, 32) + body + "\n";
}

struct Client {
    std::string name;
    SocketType socket;
    std::vector<std::string> groups;
};

struct Group {
    std::string name;
    std::string password;
    std::vector<Client*> members;
};

std::list<Client> clients;
std::vector<Group> groups;

#ifdef _WIN32
CRITICAL_SECTION g_mtx;
#else
pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
#endif

struct MutexGuard {
    MutexGuard() {
#ifdef _WIN32
    EnterCriticalSection(&g_mtx);
#else
    pthread_mutex_lock(&g_mtx);
#endif
    }
    ~MutexGuard() {
#ifdef _WIN32
    LeaveCriticalSection(&g_mtx);
#else
    pthread_mutex_unlock(&g_mtx);
#endif
    }
};

const std::uint64_t kDesKey = 0x133457799BBCDFF1ULL;
DesOperator g_des(kDesKey);

bool send_all(SocketType sock, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = static_cast<int>(send(sock, data + sent, len - sent, 0));
        if (n <= 0) {
            return false;
        }
        sent += n;
    }
    return true;
}

bool recv_all(SocketType sock, char* data, int len) {
    int received = 0;
    while (received < len) {
        int n = static_cast<int>(recv(sock, data + received, len - received, 0));
        if (n <= 0) {
            return false;
        }
        received += n;
    }
    return true;
}

bool send_encrypted(SocketType sock, const std::string& plaintext) {
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

bool recv_encrypted(SocketType sock, std::string& plaintext) {
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

void send_message(SocketType sock, const std::string& msg) {
    send_encrypted(sock, msg);
}

Client* find_client(const std::string& name) {
    for (auto& c : clients) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

Group* find_group(const std::string& name) {
    for (auto& g : groups) {
        if (g.name == name) {
            return &g;
        }
    }
    return nullptr;
}

void print_server_status() {
    std::cout << "\n======= 服务器状态 =======\n";
    std::cout << "在线用户(" << clients.size() << "):\n";
    for (auto& c : clients) {
        std::cout << "  " << (c.name.empty() ? "(未登录)" : c.name) << "\n";
    }

    std::cout << "当前群聊(" << groups.size() << "):\n";
    for (auto& g : groups) {
        std::cout << "  群聊: " << g.name << " (" << g.members.size() << "人)\n";
        for (auto* m : g.members) {
            std::cout << "    - " << m->name << "\n";
        }
    }
    std::cout << "========================\n";
}

void handle_client(Client& client) {
    try {
        while (true) {
            std::string line;
            if (!recv_encrypted(client.socket, line)) {
                break;
            }

            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
            }
            if (line.size() < 96) {
                continue;
            }

            auto trim = [](std::string s) {
                while (!s.empty() && s.back() == ' ') s.pop_back();
                return s;
            };

            std::string header = trim(line.substr(0, 32));
            std::string sender = trim(line.substr(32, 32));
            std::string target = trim(line.substr(64, 32));
            std::string body = (line.size() > 96) ? line.substr(96) : "";

            MutexGuard lock;

            if (header == "LOGIN") {
                client.name = sender;
                for (auto& c : clients) {
                    if (&c != &client && !c.name.empty()) {
                        send_message(c.socket, pack("SYSTEM", "系统", c.name, "[" + get_time_string() + "]" + sender + " 已上线"));
                    }
                }
                send_message(client.socket, pack("SYSTEM", "系统", sender, "[" + get_time_string() + "]欢迎 " + sender + " 进入聊天室"));
                print_server_status();
            } else if (header == "PMSG") {
                Client* dest = find_client(target);
                if (dest) {
                    send_message(dest->socket, pack("PMSG", sender, target, "[私聊][" + get_time_string() + "][" + sender + "]" + body));
                }
            } else if (header == "CREATE_GROUP") {
                Group* existing = find_group(target);
                if (!existing) {
                    groups.push_back({target, body, {&client}});
                    client.groups.push_back(target);
                    send_message(client.socket, pack("SYSTEM", "系统", sender, "[" + get_time_string() + "]群聊 " + target + " 创建成功"));
                } else {
                    send_message(client.socket, pack("SYSTEM", "系统", sender, "群聊名已存在"));
                }
                print_server_status();
            } else if (header == "JOIN_GROUP") {
                Group* g = find_group(target);
                if (g && g->password == body) {
                    if (std::find(g->members.begin(), g->members.end(), &client) == g->members.end()) {
                        g->members.push_back(&client);
                        client.groups.push_back(target);
                        for (auto* m : g->members) {
                            if (m != &client) {
                                send_message(m->socket, pack("SYSTEM", "系统", m->name, "[" + get_time_string() + "]" + sender + " 加入了群聊 " + target));
                            }
                        }
                        send_message(client.socket, pack("SYSTEM", "系统", sender, "[" + get_time_string() + "]你已成功加入群聊 " + target));
                    }
                } else {
                    send_message(client.socket, pack("SYSTEM", "系统", sender, "密码错误或群聊不存在"));
                }
                print_server_status();
            } else if (header == "GROUP_MSG") {
                Group* g = find_group(target);
                if (g) {
                    bool is_member = std::find(g->members.begin(), g->members.end(), &client) != g->members.end();
                    if (is_member) {
                        for (auto* m : g->members) {
                            if (m != &client) {
                                send_message(m->socket, pack("GROUP_MSG", sender, target, "[群聊" + g->name + "][" + get_time_string() + "][" + sender + "]" + body));
                            }
                        }
                    } else {
                        send_message(client.socket, pack("SYSTEM", "系统", sender, "你不是群聊 " + target + " 的成员"));
                    }
                } else {
                    send_message(client.socket, pack("SYSTEM", "系统", sender, "群聊 " + target + " 不存在"));
                }
            } else if (header == "LEAVE_GROUP") {
                Group* g = find_group(target);
                if (g) {
                    g->members.erase(std::remove(g->members.begin(), g->members.end(), &client), g->members.end());
                    client.groups.erase(std::remove(client.groups.begin(), client.groups.end(), target), client.groups.end());
                    for (auto* m : g->members) {
                        send_message(m->socket, pack("SYSTEM", "系统", m->name, "[" + get_time_string() + "]" + sender + " 已退出群聊 " + target));
                    }
                    send_message(client.socket, pack("SYSTEM", "系统", sender, "[" + get_time_string() + "]你已成功退出群聊 " + target));
                }
                print_server_status();
            } else if (header == "LIST_USERS") {
                std::string user_list;
                for (auto& c : clients) {
                    if (!c.name.empty()) {
                        user_list += c.name + "\n";
                    }
                }
                send_message(client.socket, pack("用户列表", "系统", sender, user_list));
            } else if (header == "LIST_GROUPS") {
                std::string group_list;
                for (auto& g : groups) {
                    if (std::find(g.members.begin(), g.members.end(), &client) != g.members.end()) {
                        group_list += g.name + "\n";
                    }
                }
                send_message(client.socket, pack("群聊列表", "系统", sender, group_list));
            } else if (header == "LOGOUT") {
                break;
            }
        }
    } catch (...) {
        std::cerr << "[ERROR] 客户端线程异常: " << client.name << "\n";
    }

    MutexGuard lock;
    std::string logout_name = client.name;
    SocketType sock_to_close = client.socket;

    for (auto& g : groups) {
        g.members.erase(std::remove(g.members.begin(), g.members.end(), &client), g.members.end());
    }
    clients.remove_if([&client](const Client& c) { return c.socket == client.socket; });

    for (auto& c : clients) {
        if (!c.name.empty()) {
            send_message(c.socket, pack("SYSTEM", "系统", c.name, "[" + get_time_string() + "]" + logout_name + " 已下线"));
        }
    }
    print_server_status();
    close_socket(sock_to_close);
}

int main() {
    init_console_utf8();

#ifdef _WIN32
    InitializeCriticalSection(&g_mtx);
#endif

    if (!socket_startup()) {
        std::cerr << "socket 初始化失败\n";
        return 1;
    }

    SocketType server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {
        std::cerr << "创建 socket 失败\n";
        socket_cleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8888);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "绑定失败\n";
        close_socket(server_fd);
        socket_cleanup();
        return 1;
    }
    if (listen(server_fd, 100) == SOCKET_ERROR) {
        std::cerr << "监听失败\n";
        close_socket(server_fd);
        socket_cleanup();
        return 1;
    }

    std::cout << "服务器运行在端口 8888...\n";
    print_server_status();

    while (true) {
        SocketType client_sock = accept(server_fd, nullptr, nullptr);
        if (client_sock == INVALID_SOCKET) {
            continue;
        }

        Client* client_ptr = nullptr;
        {
            MutexGuard lock;
            clients.push_back({"", client_sock, {}});
            client_ptr = &clients.back();
        }

#ifdef _WIN32
        struct ThreadArg {
            Client* ptr;
        };
        ThreadArg* arg = new ThreadArg{client_ptr};
        HANDLE h = CreateThread(
            nullptr,
            0,
            [](LPVOID p) -> DWORD {
                ThreadArg* a = reinterpret_cast<ThreadArg*>(p);
                Client* cp = a->ptr;
                delete a;
                handle_client(*cp);
                return 0;
            },
            arg,
            0,
            nullptr);
        if (h != nullptr) {
            CloseHandle(h);
        } else {
            delete arg;
            close_socket(client_sock);
        }
#else
        struct ThreadArg {
            Client* ptr;
        };
        ThreadArg* arg = new ThreadArg{client_ptr};
        pthread_t tid;
        if (pthread_create(&tid, nullptr,
                           [](void* p) -> void* {
                               ThreadArg* a = reinterpret_cast<ThreadArg*>(p);
                               Client* cp = a->ptr;
                               delete a;
                               handle_client(*cp);
                               return nullptr;
                           },
                           arg) == 0) {
            pthread_detach(tid);
        } else {
            delete arg;
            close_socket(client_sock);
        }
#endif
    }

    close_socket(server_fd);
    socket_cleanup();

#ifdef _WIN32
    DeleteCriticalSection(&g_mtx);
#endif

    return 0;
}
