#include "SkillSession.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
extern char** environ;

namespace {
    void Check(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
    bool WaitFor(const std::filesystem::path& path) {
        for (int i = 0; i < 300; ++i) {
            if (std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) > 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
}

int main(int argc, char** argv) {
    try {
        Check(argc >= 2, "Pass a test output directory");
        const auto executable = std::filesystem::read_symlink("/proc/self/exe");
        if (std::string(argv[1]) == "--spawn-parent") {
            LaunchSkillController(executable, true);
            return 0;
        }
        if (std::string(argv[1]) == "--controller") {
            const auto directory = std::filesystem::path(std::getenv("KILOBYTE_CONTROLLER_TEST"));
            Check(argc == 3 && std::string(argv[2]) == "--debug", "Spawn arguments lost");
            Check(getsid(0) == getpid(), "Controller stayed in inference session");
            Check(fcntl(100, F_GETFD) == -1, "Controller inherited inference descriptors");
            std::ofstream(directory / "ready") << getpid();
            Check(WaitFor(directory / "release"), "Controller test release timed out");
            std::ofstream(directory / "done") << "ok";
            return 0;
        }
        const auto root = std::filesystem::path(argv[1]);
        // Each run retains its files and has a distinct identity, including retries.
        const auto directory = root / std::to_string(getpid());
        std::filesystem::create_directories(directory);
        setenv("KILOBYTE_CONTROLLER_TEST", directory.c_str(), 1);
        const int file = open((directory / "inherited-descriptor").c_str(), O_CREAT | O_RDWR, 0600);
        Check(file >= 0 && dup2(file, 100) == 100, "Cannot prepare inherited descriptor");
        close(file);
        auto path = executable.string(); char option[] = "--spawn-parent";
        char* arguments[] = {path.data(), option, nullptr};
        pid_t parent = -1;
        Check(posix_spawn(&parent, path.c_str(), nullptr, nullptr, arguments, environ) == 0, "Cannot spawn test parent");
        close(100);
        int status = 0;
        Check(waitpid(parent, &status, 0) == parent && WIFEXITED(status) && WEXITSTATUS(status) == 0, "Spawn parent failed");
        Check(WaitFor(directory / "ready"), "Controller did not start");
        pid_t controller = -1; std::ifstream(directory / "ready") >> controller;
        Check(controller > 0 && kill(controller, 0) == 0, "Controller died with inference parent");
        std::ofstream(directory / "release") << "close";
        Check(WaitFor(directory / "done"), "Controller did not finish cleanly");

        // An unused local port makes connection failure deterministic without a
        // running RLBot server. A held singleton lock must bypass even that connect.
        const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(socket >= 0 && bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Cannot reserve test port");
        socklen_t size = sizeof(address);
        Check(getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) == 0, "Cannot read test port");
        setenv("RLBOT_SERVER_IP", "127.0.0.1", 1);
        setenv("RLBOT_SERVER_PORT", std::to_string(ntohs(address.sin_port)).c_str(), 1);
        // Bound but not listening: no unrelated process can take this test port.
        Check(RunSkillController(directory, "SubparNova/KilobyteReduced", false) == 1, "Unexpected observer connection");
        std::filesystem::path lockPath;
        for (const auto& item : std::filesystem::directory_iterator(directory))
            if (item.path().extension() == ".lock") lockPath = item.path();
        Check(!lockPath.empty(), "Missing controller lock");
        const int lease = open(lockPath.c_str(), O_RDWR);
        Check(lease >= 0 && flock(lease, LOCK_EX | LOCK_NB) == 0, "Failed startup retained singleton lock");
        Check(RunSkillController(directory, "SubparNova/KilobyteReduced", false) == 0, "Duplicate controller was not suppressed");
        close(lease);
        Check(RunSkillController(directory, "SubparNova/KilobyteReduced", false) == 1, "Released singleton could not reopen");
        close(socket);
        std::cout << "PASS: Linux controller detachment, arguments, descriptor isolation and singleton lifecycle\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
