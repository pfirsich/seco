#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace seco {
class CommandOutput {
public:
    CommandOutput(int sock);
    ~CommandOutput();

    void write(std::string_view str) const;
    void close(char exitCode);

private:
    int socket_;
};

class Listener {
public:
    using OutputFunc = void(std::string_view);
    using HandlerFunc = char(const std::vector<std::string>& command, CommandOutput&);

    static constexpr auto maxCommandLength = 512;

    template <typename Func>
    Listener(std::string_view path, std::string_view id, Func&& handler)
        : path_(path)
        , id_(id)
        , handler_(std::forward<Func>(handler))
    {
    }

    ~Listener() = default;

    bool start();
    void stop();

    const std::string& getId() const;

private:
    std::thread thread_;
    std::string path_;
    std::string id_;
    std::atomic<bool> running_ { false };
    std::function<HandlerFunc> handler_;
};

std::optional<char> control(std::string_view path, std::string_view id,
    const std::vector<std::string>& command, std::function<Listener::OutputFunc> outputCallback);
}
