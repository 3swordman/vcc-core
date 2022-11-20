#include <vector>
#include <string>
#include <string_view>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>
#include <algorithm>
#include <optional>
#include <exception>
#include <regex>
#include <memory>
#include <functional>

#include <cstring>
#include <cstdlib>
#include <cstdio>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>


#include "json.hpp"

using json = nlohmann::json;

using namespace std::literals;

#define unlikely

#ifdef NDEBUG
#define DEBUG_MODE 0
#else
#define DEBUG_MODE 1
#endif


constexpr int vcc_port = 46;
constexpr size_t buf_size = 1 << 10;

int nfds_total = 1;
int fd;
int pollfds = 1024;
int next_fd;
pollfd *fds;

struct user {
    std::string username, password;
};

struct user_list {
    std::vector<user> users;
    user_list(const char *path = "./users") {
        std::ifstream file{path};
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream line_stream{line};
            std::string username, password;
            line_stream >> username >> password;
            user new_element {
                .username = std::move(username),
                .password = std::move(password)
            };
            if constexpr (DEBUG_MODE) {
                std::cout << new_element.username << " " << new_element.password << "\n";
            }
            users.emplace_back(std::move(new_element));
        }
    }
    user *find_username(std::string_view username) {
        for (auto &i : users) {
            if (i.username == username) {
                return &i;
            }

        }
        return nullptr;
    }
};

enum class message_type {
    NORMAL = 1,
    LOGIN = 2,
    INVALID = 0
};

message_type type_string_to_enum(std::string_view str) {
    if (str == "message") {
        return message_type::NORMAL;
    } else if (str == "login") {
        return message_type::LOGIN;
    } else {
        return message_type::INVALID;
    }
}

std::string_view type_enum_to_string(message_type type) {
    if (type == message_type::NORMAL) {
        return "message";
    } else if (type == message_type::LOGIN) {
        return "login";
    } else {
        return "invalid";
    }
}

struct message {
    user *send_user;
    std::string username;
    std::string data;
    message_type type;
    std::string to_json() const {
        json json_obj {
            {"user", send_user->username},
            {"data", data},
            {"type", type_enum_to_string(type)}
        };
        return json_obj.dump();
    }
    std::string to_lisp_list() const {
        // ugly but work
        std::stringstream formatter;
        formatter << "(message (user '" << send_user->username << ") (data \"" << data << "\"))";
        return formatter.str();
    }
};

struct message_list {
    std::vector<message> messages;
};

struct connection {
    int fd;
    user *connect_user;

    bool operator==(const connection &other) const {
        return fd == other.fd && connect_user == other.connect_user;
    }
};

struct connection_list {
    std::vector<connection> connections;
    connection *find_fd(int fd) {
        for (auto &i : connections) {
            if (i.fd == fd) {
                return &i;
            }
        }
        return nullptr;
    }
};


std::optional<message> json_to_message(const std::string &content, user *connect_user) noexcept {
    // { "type": "message", "user": "username", "data": "message" }
    try {
        auto json_content = json::parse(content);
        const auto username = json_content["user"].get<std::string>();
        const auto data = json_content["data"].get<std::string>();
        const auto type_string = json_content["type"].get<std::string>();
        message_type type = type_string_to_enum(type_string);

        message msg;
        msg.send_user = connect_user ? connect_user : nullptr;
        msg.username = username;
        msg.data = std::move(data);
        msg.type = type;

        return msg;
    } catch (const std::exception &exc) {
        std::cerr << exc.what() << "\n";
        return std::nullopt;
    }
}

std::optional<message> lisp_list_to_message(const std::string &content, user *connect_user) noexcept {
    // (message (user 'username) (data "message"))
    // TODO: use a better way instead of regex to parse the lisp list
    try {
        static std::regex username_regex{R"regex(\(user '.+?(?=\)))regex"};
        static std::regex data_regex{R"regex(\(data ".*?(?="))regex"};
        static std::regex type_regex{R"regex(\(.+?(?=(\(| )))regex"};
        std::smatch match_result;

        if (!std::regex_search(content, match_result, username_regex)) {
            return std::nullopt;
        }
        std::string username = match_result[0];
        username = username.substr(7);

        if (!std::regex_search(content, match_result, data_regex)) {
            return std::nullopt;
        }
        std::string data = match_result[0];
        data = data.substr(7);

        if (!std::regex_search(content, match_result, type_regex)) {
            return std::nullopt;
        }
        std::string type_string = match_result[0];
        data = data.substr(1);
        message_type type = type_string_to_enum(type_string);

        message msg;
        msg.send_user = connect_user ? connect_user : nullptr;
        msg.username = username;
        msg.data = std::move(data);
        msg.type = type;
        return msg;
    } catch (const std::exception &exc) {
        std::cerr << exc.what() << "\n";
        return std::nullopt;
    }
}

void err_handler(const char *name, int i) {
    if (i < 0) {
        std::perror(name);
        std::exit(1);
    }
}

struct socket_connection {
    user_list &users;
    message_list &messages;
    connection_list &connections;
    int fd;
    socket_connection(user_list &users_, message_list &messages_, connection_list &connections_) : users(users_), messages(messages_), connections(connections_) {
        struct sockaddr_in addr;
        int i, enable = 1;

        err_handler("socket()", (fd = socket(AF_INET, SOCK_STREAM, 0)));
        ::fd = fd;

        std::memset(&addr, 0, sizeof(struct sockaddr_in));

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(vcc_port);

        err_handler("setsockopt()", setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)));

#ifdef SO_REUSEPORT
        err_handler("setsockopt()", setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)));
#endif
        err_handler("bind()", bind(fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in)));
        err_handler("listen()", listen(fd, SOMAXCONN));

        if (!(fds = new pollfd[pollfds])) {
            return;
        }

        std::memset(fds, 0, sizeof(struct pollfd) * pollfds);

        for (i = 0; i < pollfds; i++) 
            fds[i].fd = -1;

        fds[0].fd = fd;
        fds[0].events = POLLIN;

        next_fd = 1;
        
    }
    void broadcast_message(connection &except_connection, const message &msg) {
        auto data_sent = msg.to_lisp_list(); // or msg.to_json()
        data_sent += "\n";
        for (auto &&i : connections.connections) {
            if (i == except_connection) continue;
            if (!i.connect_user) continue;
            write(i.fd, data_sent.data(), data_sent.size());
        }
    }
    void poll_loop(std::function<void(int)> func) {
        for (;;) {
            int n = poll(fds, nfds_total, -1);

            if (n == -1) {
                return;
            }
            if (fds[0].revents & POLLIN) {
                socklen_t size = sizeof(struct sockaddr_in);
                struct sockaddr usr_addr;
                int usr_fd;

                err_handler("accept()", (usr_fd = accept(fd, (struct sockaddr *) &usr_addr, &size)));
                fds[next_fd].fd = usr_fd;
                fds[next_fd].events = POLLIN;

                connections.connections.push_back(connection {
                    .fd = usr_fd,
                    .connect_user = nullptr
                });
                std::cout << "fd: " << usr_fd << std::endl;

                next_fd++;
                nfds_total++;
            }
            for (int i = 1; i < nfds_total; i++) {
                if (fds[i].fd == -1) 
                    continue;

                if (fds[i].revents & POLLIN) {
                    func(fds[i].fd);
                }
                fds[i].revents = 0;
            }

        }
    }
    std::optional<message> recv(connection &curr_connection) {
        auto buf = std::make_unique<char[]>(buf_size);
        int fd = curr_connection.fd;
        std::memset(buf.get(), 0, buf_size);
        int size = read(fd, buf.get(), buf_size);
        
        if (size < 0 || size == 0) {
            connections.connections.erase(
                std::remove(connections.connections.begin(), connections.connections.end(), curr_connection), connections.connections.end()
            );
            
            for (int i = 1; i < nfds_total; i++) {
                if (fds[i].fd == fd) {
                    fds[i].fd = -1;
                    break;
                }
            }
            return std::nullopt;
        }

        std::string line{buf.get(), static_cast<size_t>(size - 1)};
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

        if (line.empty())  return std::nullopt;

        std::optional<message> new_message_optional;
        if (line[0] == '(') {
            new_message_optional = lisp_list_to_message(line, curr_connection.connect_user);
            std::cout << "LISP\n";
        } else {
            new_message_optional = json_to_message(line, curr_connection.connect_user);
            std::cout << "JSON\n";
        }
        return new_message_optional;
    }
};

int handle_request(connection &curr_connection, user_list &users, message_list &messages, socket_connection &sock) {
    // err if return value is 1
    auto new_message_optional = sock.recv(curr_connection);
    if (!new_message_optional.has_value()) {
        std::cerr << "error\n" << std::endl;
        return 1;
    }
    const auto new_message = new_message_optional.value();

    std::cout << type_enum_to_string(new_message.type) << " " << (new_message.send_user ? new_message.send_user->username : new_message.username) << " " << new_message.data << std::endl;
    switch (new_message.type) {
        case message_type::NORMAL:
            if (!curr_connection.connect_user) break;
            sock.broadcast_message(curr_connection, new_message);
            messages.messages.emplace_back(std::move(new_message));
            break;
        case message_type::LOGIN: {
            const auto login_user = users.find_username(new_message.username);
            if (!login_user) {
                return 1;
            }
            if (login_user->password == new_message.data) {
                curr_connection.connect_user = login_user;
            }
            break;
        }
        case message_type::INVALID:
            break;
    }
    return 0;

}

int main() {
    user_list users;
    message_list messages;
    connection_list connections;
    socket_connection sock{users, messages, connections};
    sock.poll_loop([&] (int fd) {
        const auto curr_connection = connections.find_fd(fd);
        handle_request(*curr_connection, users, messages, sock);
    });
    for (auto &&i : messages.messages) {
        std::cout << i.send_user->username << " " << i.data << "\n";
    }
}