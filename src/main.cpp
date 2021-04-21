#include <atomic>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include <docopt.h>

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "seco.hpp"

using namespace std::literals;

const auto usage = R"(seco test

Usage:
    server-control start [--id=<id>]
    server-control control [--id=<id>] status
    server-control control [--id=<id>] get-var <name>
    server-control control [--id=<id>] set-var <name> <value>
    server-control control [--id=<id>] print <message>
    server-control control [--id=<id>] long
    server-control control [--id=<id>] exit

Options:
    --id=<id>       Server instance ID. May also be the server's pid.
                    If ommited and there is only one server instance running, it will be chosen.
)";

char executeControlCommand(const std::vector<std::string>& command, seco::CommandOutput& output)
{
    static std::unordered_map<std::string, std::string> variables;

    std::map<std::string, docopt::value> args;
    try {
        args = docopt::docopt_parse(usage, command);
    } catch (const std::runtime_error& exc) {
        return 1;
    }

    if (!args.at("control").asBool()) {
        return 1;
    }

    if (args.at("status").asBool()) {
        output.write("Everything seems fine\n");
        return 0;
    } else if (args.at("get-var").asBool()) {
        const auto it = variables.find(args.at("<name>").asString());
        if (it == variables.end()) {
            output.write("Variable not found\n");
            return 1;
        }
        output.write(it->second + "\n");
        return 0;
    } else if (args.at("set-var").asBool()) {
        variables[args.at("<name>").asString()] = args.at("<value>").asString();
        return 0;
    } else if (args.at("print").asBool()) {
        std::cout << args.at("<message>").asString() << std::endl;
        return 0;
    } else if (args.at("long").asBool()) {
        for (size_t i = 0; i < 5; ++i) {
            output.write("Processing..\n");
            sleep(1);
        }
        output.write("Done.\n");
        return 0;
    } else if (args.at("exit").asBool()) {
        output.close(0);
        std::exit(0);
    } else {
        output.write("Unknown command\n");
        return 1;
    }
}

std::atomic<bool> running { true };

void signalHandler(int signal)
{
    if (signal == SIGINT) {
        running.store(false);
    }
}

int main(int argc, char** argv)
{
    const auto args = docopt::docopt(usage, { argv + 1, argv + argc }, true);
    const auto path = "/tmp/server-control";
    if (args.at("start").asBool()) {
        const auto id = args.at("--id") ? args.at("--id").asString() : "";
        seco::Listener secoListener(path, id, executeControlCommand);
        std::cout << "pid: " << ::getpid() << ", id: " << secoListener.getId() << std::endl;
        if (!secoListener.start()) {
            std::cout << "Error starting control listener" << std::endl;
        }

        ::signal(SIGINT, signalHandler);

        // You probably want to do this in your application, because SIGPIPE will be received if
        // the control process was terminated while the server is still writing.
        ::signal(SIGPIPE, SIG_IGN);

        while (running.load()) {
            ::sleep(1);
        }

        std::cout << "Stopping listener" << std::endl;
        secoListener.stop();
    } else if (args.at("control").asBool()) {
        const std::vector<std::string> cmd { argv + 1, argv + argc };
        const auto id = args.at("--id") ? args.at("--id").asString() : "";
        const auto res
            = seco::control(path, id, cmd, [](std::string_view str) { std::cout << str; });
        if (!res) {
            std::cerr << "Error executing control command" << std::endl;
            return 1;
        }
        return *res;
    }
}
