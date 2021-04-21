#include "seco.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
std::string errorString()
{
    const auto err = errno;
    return std::string(strerror(err)) + " (" + std::to_string(err) + ")";
}

bool checkWrite(int fd, const void* data, size_t num)
{
    if (::write(fd, data, num) == -1) {
        std::cerr << "Error writing socket: " << errorString() << std::endl;
        return false;
    }
    return true;
}
}

CommandOutput::CommandOutput(int sock)
    : socket_(sock)
{
}

CommandOutput::~CommandOutput()
{
    if (socket_ != -1) {
        ::close(socket_);
        socket_ = -1;
    }
}

void CommandOutput::write(std::string_view str) const
{
    if (socket_ == -1)
        return;
    for (size_t i = 0; i < str.size() / 0xff + 1; ++i) {
        const auto start = i * 0xff;
        const auto size = std::min(str.size() - start, 0xfful);
        const auto cSize = static_cast<uint8_t>(size);
        checkWrite(socket_, &cSize, 1);
        checkWrite(socket_, str.data() + start, size);
    }
}

void CommandOutput::close(char exitCode)
{
    if (socket_ == -1)
        return;
    const char null = 0;
    checkWrite(socket_, &null, 1);
    checkWrite(socket_, &exitCode, 1);
    ::close(socket_);
    socket_ = -1;
}

namespace {
sockaddr_un getAddress(const std::string& path)
{
    sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    return addr;
}
}

bool ControlListener::start()
{
    const auto sock = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock == -1) {
        std::cerr << "Could not open socket: " << errorString() << std::endl;
        return false;
    }

    const auto sockPath = path_ + '/' + std::to_string(::getpid());
    std::cout << "sockPath: " << sockPath << std::endl;
    const auto addr = getAddress(sockPath);

    ::unlink(sockPath.c_str());
    ::mkdir(path_.c_str(), 0600);
    if (::bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == -1) {
        std::cerr << "Could not bind '" << sockPath << "': " << errorString() << std::endl;
        return false;
    }

    const auto idPath = path_ + '/' + id_;
    std::cout << "Symlink: " << idPath << std::endl;
    ::unlink(idPath.c_str());
    if (::symlink(sockPath.c_str(), idPath.c_str()) == -1) {
        std::cerr << "Could not create symlink: " << errorString() << std::endl;
        // don't return
    }

    const auto maxConnectionRequests = 2;
    if (::listen(sock, maxConnectionRequests) == -1) {
        std::cerr << "Could not listen: " << errorString() << std::endl;
        return false;
    }

    running_.store(true);
    thread_ = std::thread { [this, sock]() {
        pollfd fds[1];
        ::memset(fds, 0, sizeof(fds));
        fds[0].fd = sock;
        fds[0].events = POLLIN;

        while (running_.load()) {
            // We are using a nonblocking accept socket and poll, because otherwise we could be
            // stuck in ::accept forever, after we call stop().
            const auto numFds = ::poll(fds, 1, 500);
            if (numFds == -1) {
                std::cerr << "Error in poll: " << errorString() << std::endl;
                continue;
            } else if (numFds == 0) {
                // timeout
                continue;
            }
            // num > 1. Since we are only polling 1 fd, we know it's the accept socket

            const auto fd = ::accept(sock, nullptr, nullptr);
            if (fd == -1) {
                std::cerr << "Could not accept connection: " << errorString() << std::endl;
                continue;
            }

            std::string buf(maxCommandLength, '\0');
            const auto num = ::read(fd, buf.data(), maxCommandLength);
            buf.resize(num);

            std::vector<std::string> args;
            size_t i = 0;
            while (i < buf.size()) {
                const auto end = buf.find('\0', i);
                args.emplace_back(buf.substr(i, end - i));
                i = end + 1;
            }

            CommandOutput output(fd);
            const auto res = handler_(args, output);
            output.close(res);

            ::close(fd);
        }
        ::close(sock);
    } };

    return true;
}

void ControlListener::stop()
{
    if (!running_.load()) {
        return;
    }
    running_.store(false);
    thread_.join();
}

const std::string& ControlListener::getId() const
{
    return id_;
}

namespace {
std::optional<std::vector<std::string>> listDir(const std::string& path, unsigned char type)
{
    auto dir = ::opendir(path.c_str());
    if (!dir) {
        return std::nullopt;
    }

    std::vector<std::string> items;
    ::dirent* entry;
    errno = 0;
    while ((entry = ::readdir(dir))) {
        if (entry->d_type == type) {
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
    const auto items = listDir(std::string(path), DT_SOCK);
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

[[maybe_unused]] std::string toHexStream(const uint8_t* data, size_t size)
{
    static constexpr std::array<char, 16> hexDigits { '0', '1', '2', '3', '4', '5', '6', '7', '8',
        '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    std::string str;
    str.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        str.append(1, hexDigits[(data[i] & 0xf0) >> 4]);
        str.append(1, hexDigits[(data[i] & 0x0f) >> 0]);
    }
    return str;
}
}

std::optional<char> control(std::string_view path, std::string_view id,
    const std::vector<std::string>& command,
    std::function<ControlListener::OutputFunc> outputCallback)
{
    const auto sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        std::cerr << "Error creating socket: " << errorString() << std::endl;
        return std::nullopt;
    }

    std::string realId(id);
    if (realId.empty()) {
        const auto guess = guessId(path);
        if (!guess) {
            std::cerr << "Could not guess id" << std::endl;
            return std::nullopt;
        }
        realId = *guess;
    }
    const auto sockPath = std::string(path) + '/' + std::string(realId);
    const auto addr = getAddress(sockPath);

    if (::connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == -1) {
        std::cerr << "Could not connect: " << errorString() << std::endl;
        return std::nullopt;
    }

    std::string wbuf;
    for (const auto& arg : command) {
        wbuf.append(arg);
        wbuf.append(1, '\0');
    }
    if (::write(sock, wbuf.data(), wbuf.size()) == -1) {
        std::cerr << "Error writing command: " << errorString() << std::endl;
        return std::nullopt;
    }

    while (true) {
        std::string buffer;
        std::string rbuf(512, '\0');
        int num = 0;
        while ((num = ::read(sock, rbuf.data(), rbuf.size())) > 0) {
            buffer.append(rbuf.data(), num);

            while (true) {
                const auto size = static_cast<uint8_t>(buffer[0]);
                if (size == 0) {
                    if (buffer.size() >= 2) {
                        ::close(sock);
                        return buffer[1];
                    } else {
                        break;
                    }
                } else {
                    if (buffer.size() >= size + 1) {
                        outputCallback(buffer.substr(1, size));
                        buffer = buffer.substr(size + 1);
                    } else {
                        break;
                    }
                }
            }
        }

        if (num < 0) {
            std::cerr << "Error reading string: " << errorString() << std::endl;
            ::close(sock);
            return std::nullopt;
        }
    }

    // We should never get here
    ::close(sock);
    return std::nullopt;
}
