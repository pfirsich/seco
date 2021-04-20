#include <atomic>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <dirent.h>
#include <docopt.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std::literals;

const auto usage = R"(Server Control

Usage:
    server-control start [--id=<id>]
    server-control control [--id=<id>] show-id
    server-control control [--id=<id>] exit
    server-control control [--id=<id>] status
    server-control control [--id=<id>] get-var <name>
    server-control control [--id=<id>] set-var <name> <value>
    server-control control [--id=<id>] print <message>

Options:
    --id=<id>       Server instance ID
    --pid=<pid>     Server instance PID
)";

class ControlLister {
public:
    using HandlerFunc = std::pair<char, std::string>(const std::vector<std::string>&);

    ControlLister(std::string path, std::string id, std::function<HandlerFunc> func)
        : path_(std::move(path))
        , id_(std::move(id))
        , handler_(std::move(func))
    {
    }

    ~ControlLister()
    {
    }

    bool start()
    {
        const auto pid = ::getpid();
        const auto fifoPath = path_ + '/' + std::to_string(pid);
        std::cout << "fifoPath: " << fifoPath << std::endl;

        // TODO: mkdir -p before!
        if (::mkfifo(fifoPath.c_str(), 0600) != 0) {
            std::cout << "mkfifo failed" << std::endl;
            return false;
        }

        running_.store(true);
        thread_ = std::thread { [this, fifoPath]() {
            while (running_.load()) {
                std::cout << "open fdr" << std::endl;
                const int fdr = ::open(fifoPath.c_str(), O_RDONLY);
                if (fdr == -1) {
                    // Just try again
                    continue;
                }
                std::string rbuf(1024, '\0');
                std::cout << "read" << std::endl;
                const auto rnum = ::read(fdr, rbuf.data(), rbuf.size());
                std::cout << "rnum: " << rnum << std::endl;
                ::close(fdr);

                std::string wbuf;
                if (rnum > 0) {
                    rbuf.resize(rnum);
                    std::cout << "close fdr" << std::endl;

                    std::vector<std::string> args;
                    size_t i = 0;
                    while (i < rbuf.size()) {
                        const auto end = rbuf.find('\0', i);
                        args.emplace_back(rbuf.substr(i, end - i));
                        std::cout << "arg: " << args.back() << std::endl;
                        i = end + 1;
                    }
                    const auto res = handler_(args);
                    std::cout << "res.code: " << static_cast<int>(res.first)
                              << ", res.output: " << res.second << std::endl;

                    wbuf.append(1, res.first);
                    wbuf.append(res.second);
                }

                std::cout << "open fdw" << std::endl;
                const int fdw = ::open(fifoPath.c_str(), O_WRONLY);
                if (fdw == -1) {
                    continue;
                }
                std::cout << "write" << std::endl;
                ::write(fdw, wbuf.data(), wbuf.size());
                std::cout << "close" << std::endl;
                ::close(fdw);
            }
        } };

        return true;
    }

    void stop()
    {
        if (!running_.load()) {
            return;
        }
        running_.store(false);
        thread_.join();
    }

    const std::string& getId() const
    {
        return id_;
    }

private:
    static void cleanup()
    {
    }

    std::string path_;
    std::string id_;
    std::thread thread_;
    std::function<HandlerFunc> handler_;
    std::atomic<bool> running_ { false };
};

std::atomic<bool> running { true };

void signalHandler(int signal)
{
    if (signal == SIGINT)
        running.store(false);
}

std::optional<std::pair<char, std::string>> control(
    std::string_view path, std::string_view id, const std::vector<std::string>& command)
{
    const auto fifoPath = std::string(path) + '/' + std::string(id);
    std::cout << "fifoPath: " << fifoPath << std::endl;
    std::cout << "open fdw" << std::endl;
    const auto fdw = ::open(fifoPath.c_str(), O_WRONLY);
    std::cout << "fdw: " << fdw << std::endl;
    if (fdw == -1) {
        return std::nullopt;
    }
    std::string wbuf;
    for (const auto& arg : command) {
        wbuf.append(arg);
        wbuf.append(1, '\0');
    }
    std::cout << "write" << std::endl;
    ::write(fdw, wbuf.data(), wbuf.size());
    std::cout << "close fdw" << std::endl;
    ::close(fdw);

    std::cout << "open fdr" << std::endl;
    const auto fdr = ::open(fifoPath.c_str(), O_RDONLY);
    std::string rbuf(1024, '\0');
    std::cout << "read fdr" << std::endl;
    ::read(fdr, rbuf.data(), rbuf.size());
    std::cout << "close fdr" << std::endl;
    ::close(fdr);

    const auto resultCode = rbuf[0];
    return std::pair<char, std::string>(resultCode, rbuf.substr(1));
}

std::optional<std::vector<std::string>> listDir(const std::string& path)
{
    auto dir = ::opendir(path.c_str());
    if (!dir) {
        return std::nullopt;
    }

    std::vector<std::string> items;
    ::dirent* entry;
    errno = 0;
    while ((entry = ::readdir(dir))) {
        if (entry->d_type == DT_FIFO) {
            items.emplace_back(entry->d_name);
        }
    }
    if (errno) {
        return std::nullopt;
    }

    // We ignore the closedir errors, because readdir did not fail, so we got what we wanted
    ::closedir(dir);

    return items;
}

std::optional<int> parseInteger(std::string_view str)
{
    try {
        size_t pos = 0;
        const auto num = std::stol(std::string(str), &pos);
        if (pos != str.size()) {
            return std::nullopt;
        }
        return num;
    } catch (const std::exception& exc) {
        return std::nullopt;
    }
}

std::optional<std::string> guessId(std::string_view path)
{
    const auto items = listDir(std::string(path));
    if (!items) {
        return std::nullopt;
    }
    std::string id;
    for (const auto& item : *items) {
        const auto pid = parseInteger(item);
        if (!pid) {
            continue;
        }
        if (::kill(*pid, 0) == 0) {
            // process exists
            if (!id.empty()) {
                // Return nullopt if there are multiple
                return std::nullopt;
            }
            id = item;
        }
    }
    return id;
}

std::optional<std::pair<char, std::string>> control(
    std::string_view path, const std::vector<std::string>& command)
{
    const auto id = guessId(path);
    if (!id) {
        return std::nullopt;
    }
    return control(path, *id, command);
}

std::pair<char, std::string> executeControlCommand(const std::vector<std::string>& command)
{
    static std::unordered_map<std::string, std::string> variables;
    using Result = std::pair<char, std::string>;
    try {
        const auto args = docopt::docopt_parse(usage, command);
        if (!args.at("control").asBool()) {
            return Result(1, "");
        }
        if (args.at("show-id").asBool()) {
            return Result(0, "I don't know");
        } else if (args.at("exit").asBool()) {
            std::exit(0);
        } else if (args.at("status").asBool()) {
            return Result(0, "Everything seems fine");
        } else if (args.at("get-var").asBool()) {
            const auto it = variables.find(args.at("<name>").asString());
            if (it == variables.end())
                return Result(1, "Variable not found");
            return Result(0, it->second);
        } else if (args.at("set-var").asBool()) {
            variables[args.at("<name>").asString()] = args.at("<value>").asString();
        } else if (args.at("print").asBool()) {
            std::cout << args.at("<message>").asString() << std::endl;
            return Result(0, "");
        }
        return Result(1, "Unknown command");
    } catch (const std::runtime_error& exc) {
        return Result(1, "");
    }
}

int main(int argc, char** argv)
{
    const auto args = docopt::docopt(usage, { argv + 1, argv + argc }, true);
    for (const auto& [k, v] : args) {
        std::cout << k << ": " << v << std::endl;
    }
    const auto path = "/tmp/server-control";
    if (args.at("start").asBool()) {
        ControlLister controlListener(
            path, args.at("--id") ? args.at("--id").asString() : "", executeControlCommand);
        std::cout << "Id: " << controlListener.getId() << std::endl;
        if (!controlListener.start()) {
            std::cout << "Error starting control listener" << std::endl;
        }

        ::signal(SIGINT, signalHandler);

        while (running.load()) {
            ::sleep(1);
        }
        controlListener.stop();
    } else if (args.at("control").asBool()) {
        const std::vector<std::string> cmd { argv + 1, argv + argc };
        const auto res
            = args.at("--id") ? control(path, args.at("id").asString(), cmd) : control(path, cmd);
        if (!res) {
            return 1;
        }
        std::cout << res->second << std::endl;
        return res->first;
    }
}
