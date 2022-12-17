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
#include <ctime>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>


#include "json.hpp"
#include "enum.h"

using nlohmann::json;

using namespace std::literals;

#define CONCAT_IMPL(a,b) a##b
#define CONCAT(a,b) CONCAT_IMPL(a,b)


constexpr int vcc_port = 274;
constexpr size_t buf_size = 1 << 10;

int nfds_total = 1;
int pollfds = 1024;

pollfd *fds;

struct user {
    std::string username, password;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(user, username, password)
};

struct user_list {
    std::vector<user> users;
    user_list(const char *path = "./users.json") {
        std::ifstream file{path};
        auto data = json::parse(file);
        std::transform(data.begin(), data.end(), std::back_inserter(users), [] (json &json_obj) {
            return json_obj.get<user>();
        });
        for (auto &&i : users) {
            std::cout << i.username << " " << i.password << "\n";
        }
        // std::string line;
        // while (std::getline(file, line)) {
        //     std::istringstream line_stream{line};
        //     std::string username, password;
        //     line_stream >> username >> password;
        //     user new_element {
        //         .username = std::move(username),
        //         .password = std::move(password)
        //     };
        //     std::cout << new_element.username << " " << new_element.password << "\n";
        //     users.emplace_back(std::move(new_element));
        // }
    }
    user *find_username(std::string_view username) {
        auto find_result = std::find_if(users.begin(), users.end(), [username] (user &i) {
            return i.username == username;
        });
        return find_result == users.end() ? nullptr : find_result.base();
    }
};

BETTER_ENUM(message_type, unsigned char, NORMAL, LOGIN, INVALID);

struct message {
    user *send_user;
    std::string username;
    std::string data;
    message_type type;
    time_t time;
    std::string to_json() const {
        std::string type_str = type._to_string();
        std::transform(type_str.begin(), type_str.end(), type_str.begin(), [] (char c) {
            return std::tolower(c);
        });
        json json_obj {
            {"user", send_user->username},
            {"data", data},
            {"type", type_str},
            {"time", time}
        };
        return json_obj.dump();
    }
    std::string to_lisp_list() const {
        // ugly but work
        std::stringstream formatter;
        formatter << "(message (user '" << send_user->username << ") (data \"" << data << "\"))";
        return formatter.str();
    }
    static std::optional<message> from_json(const std::string &content, user *connect_user) noexcept {
        // { "type": "message", "user": "username", "data": "message" }
        try {
            auto json_content = json::parse(content);
            const auto username = json_content["user"].get<std::string>();
            const auto data = json_content["data"].get<std::string>();
            const auto type = json_content["type"].get<std::string>();

            return message {
                .send_user = connect_user ? connect_user : nullptr,
                .username = std::move(username),
                .data = std::move(data),
                .type = message_type::_from_string_nocase(type.c_str()),
                .time = std::time(NULL)
            };
        } catch (const std::exception &exc) {
            std::cerr << exc.what() << "\n";
            return std::nullopt;
        }
    }

    #define regex_search_nullopt(name)               \
        if (!std::regex_search(content, match_result, CONCAT(name,_regex))) {    \
            return std::nullopt;                                        \
        }                                                               \
        std::string name = match_result[1]

    static std::optional<message> from_lisp_list(const std::string &content, user *connect_user) noexcept {
        // (message (user 'username) (data "message"))
        // TODO: use a better way instead of regex to parse the lisp list
        try {
            static std::regex username_regex{R"regex(\(user '(.+?(?=\))))regex"};
            static std::regex data_regex{R"regex(\(data "(.*?(?=")))regex"};
            static std::regex type_regex{R"regex(\((.+?(?=(\(| ))))regex"};
            std::smatch match_result;

            regex_search_nullopt(username);
            regex_search_nullopt(data);
            regex_search_nullopt(type);

            return message {
                .send_user = connect_user ? connect_user : nullptr,
                .username = std::move(username),
                .data = std::move(data),
                .type = message_type::_from_string_nocase(type.c_str()),
                .time = std::time(NULL)
            };
        } catch (const std::exception &exc) {
            std::cerr << exc.what() << "\n";
            return std::nullopt;
        }
    }

    #undef regex_search_nullopt
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

    void close() {
        shutdown(fd, SHUT_RDWR);
        ::close(fd);
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

    void remove(connection &curr_connection) {
        curr_connection.close();
        
        for (int i = 1; i < nfds_total; i++) {
            if (fds[i].fd == curr_connection.fd) {
                fds[i].fd = -1;
                break;
            }
        }
        connections.erase(
            std::remove(connections.begin(), connections.end(), curr_connection), connections.end()
        );
    }
    
};

template <typename Func>
struct err_handler_impl {
    const char *name;
    Func func;
    err_handler_impl(const char *name_, Func func_) : name(name_), func(std::move(func_)) {}
    template <typename ...Args>
    auto operator()(Args &&...args) {
        auto i = func(std::forward<Args>(args)...);
        if (i < 0) {
            std::perror(name);
            std::exit(1);
        }
        return i;
    }
};

#define err_handler(func) err_handler_impl(#func "()", func)

struct socket_connection {
    user_list &users;
    message_list &messages;
    connection_list &connections;
    int fd;
    socket_connection(user_list &users_, message_list &messages_, connection_list &connections_) : users(users_), messages(messages_), connections(connections_) {
        struct sockaddr_in addr;
        int i, enable = 1;

        fd = err_handler(socket)(AF_INET, SOCK_STREAM, 0);

        std::memset(&addr, 0, sizeof(struct sockaddr_in));

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(vcc_port);

        err_handler(setsockopt)(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));

#ifdef SO_REUSEPORT
        err_handler(setsockopt)(fd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int));
#endif
        err_handler(bind)(fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));
        err_handler(listen)(fd, SOMAXCONN);

        if (!(fds = new pollfd[pollfds])) {
            return;
        }

        std::memset(fds, 0, sizeof(struct pollfd) * pollfds);

        for (i = 1; i < pollfds; i++) 
            fds[i].fd = -1;

        fds[0].fd = fd;
        fds[0].events = POLLIN;
        
    }
    void broadcast_message(connection &except_connection, const message &msg) {
        auto data_sent = msg.to_json(); // or msg.to_lisp_list()
        data_sent += "\n";
        for (auto &&i : connections.connections) {
            if (i == except_connection) continue;
            if (!i.connect_user) continue;
            write(i.fd, data_sent.data(), data_sent.size());
        }
    }
    void poll_loop(std::function<void(connection &)> func) {
        for (;;) {
            int n = poll(fds, nfds_total, -1);

            if (n == -1) {
                return;
            }
            if (fds[0].revents & POLLIN) {
                socklen_t size = sizeof(struct sockaddr_in);
                struct sockaddr usr_addr;
                int usr_fd;

                usr_fd = err_handler(accept)(fd, (struct sockaddr *) &usr_addr, &size);
                fds[nfds_total].fd = usr_fd;
                fds[nfds_total].events = POLLIN;

                connections.connections.push_back(connection {
                    .fd = usr_fd,
                    .connect_user = nullptr
                });
                std::cout << "fd: " << usr_fd << std::endl;

                nfds_total++;
            }
            for (int i = 1; i < nfds_total; i++) {
                if (fds[i].fd == -1) 
                    continue;

                if (fds[i].revents & POLLIN) {
                    auto fd_result = connections.find_fd(fds[i].fd);
                    if (!fd_result) {
                        std::clog << "can't find connection of fd " << fd_result << "\n";
                        return;
                    }
                    func(*fd_result);
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
            connections.remove(curr_connection);
            return std::nullopt;
        }

        std::string line{buf.get(), static_cast<size_t>(size - 1)};
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());

        if (line.empty())  return std::nullopt;

        std::optional<message> new_message_optional;
        if (line[0] == '(') {
            new_message_optional = message::from_lisp_list(line, curr_connection.connect_user);
            std::cout << "LISP\n";
        } else {
            new_message_optional = message::from_json(line, curr_connection.connect_user);
            std::cout << "JSON\n";
        }
        return new_message_optional;
    }
};

int handle_request(connection &curr_connection, socket_connection &sock) {
    // err if return value is 1
    auto &users = sock.users;
    auto &messages = sock.messages;
    auto new_message_optional = sock.recv(curr_connection);
    if (!new_message_optional.has_value()) {
        std::cerr << "error\n" << std::endl;
        return 1;
    }
    const auto new_message = new_message_optional.value();

    std::cout << new_message.type._to_string() << " " << (new_message.send_user ? new_message.send_user->username : new_message.username) << " " << new_message.data << std::endl;
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
    sock.poll_loop([&] (connection &curr_connection) {
        handle_request(curr_connection, sock);
    });
}