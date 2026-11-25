// The Ambrosia runner: a small Switch homebrew program that lets a PC on the same network run Ambrosia
// builds on the console with nobody at it (scripts/switch-runner.py drives it; BUILD.md, "Test
// runs from the PC", has the whole flow).
//
// It listens on a TCP port and takes one request per connection. A job request brings an Ambrosia
// build (uploaded, or kept from an earlier upload with the same SHA-256), the game's options and
// the files to send back. The runner writes the job down, names the build as the next program for
// the homebrew loader to start (envSetNextLoad) and exits. The game, started with
// --ambrosia-runner <this program's path> in front of its options, names the runner as the next
// program in turn, so when the game exits the loader starts the runner again, which finds the job
// it wrote, waits for the PC to fetch the results, and then for the next job.
//
// It writes only under sdmc:/switch/ambrosia/, reads only there and the newest of Atmosphère's crash
// reports (the crash-report request), takes only requests that carry the token in
// sdmc:/switch/ambrosia/runner/token.txt, from private (local network) addresses, and runs whatever
// build it is sent.

#include <switch.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
    constexpr int protocolVersion = 1;
    constexpr uint16_t port = 28300;
    constexpr const char* allowedRoot = "sdmc:/switch/ambrosia/";
    constexpr const char* workDirectory = "sdmc:/switch/ambrosia/runner";
    constexpr const char* cacheDirectory = "sdmc:/switch/ambrosia/runner/cache";
    constexpr const char* recentPath = "sdmc:/switch/ambrosia/runner/cache/recent.txt";
    constexpr const char* tokenPath = "sdmc:/switch/ambrosia/runner/token.txt";
    constexpr const char* jobPath = "sdmc:/switch/ambrosia/runner/job.txt";
    constexpr const char* defaultSelfPath = "sdmc:/switch/ambrosia-runner/ambrosia-runner.nro";
    // The argument the game passes back when it names the runner as the next program.
    constexpr const char* afterJobArgument = "--after-job";
    // How long one read or write on a connection may wait.
    constexpr int ioTimeoutMs = 30000;
    constexpr uint64_t maxNroSize = 512ull << 20;
    // Builds kept in the cache, most recently used first.
    constexpr size_t cacheKeep = 8;
    // nx-hbloader's buffers for the next program's path and argument string.
    constexpr size_t maxNextPath = 511;
    constexpr size_t maxNextArgv = 2047;

    // What is shown on screen: a few fixed lines and the latest events.
    struct Screen {
        std::string address = "not connected";
        std::string state = "starting";
        std::string token = "not read";
        std::string warning;
        std::deque<std::string> events;
        bool dirty = true;
    } screen;

    void event(const char* format, ...) __attribute__((format(printf, 1, 2)));

    void event(const char* format, ...)
    {
        char text[300];
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(text, sizeof(text), format, arguments);
        va_end(arguments);
        std::time_t now = std::time(nullptr);
        char stamp[16];
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&now));
        screen.events.push_back(std::string(stamp) + "  " + text);
        while (screen.events.size() > 22) {
            screen.events.pop_front();
        }
        screen.dirty = true;
    }

    void setState(const std::string& state)
    {
        screen.state = state;
        screen.dirty = true;
    }

    // Prints text word-wrapped to the console's 80 columns, the first line after its label and
    // the rest under the first.
    void printWrapped(const std::string& label, const std::string& text)
    {
        const size_t width = 79 - label.size();
        size_t start = 0;
        bool first = true;
        while (start < text.size() || first) {
            size_t end = std::min(text.size(), start + width);
            if (end < text.size()) {
                size_t space = text.rfind(' ', end);
                if (space != std::string::npos && space > start) {
                    end = space;
                }
            }
            std::printf("%s%s\n", first ? label.c_str() : std::string(label.size(), ' ').c_str(), text.substr(start, end - start).c_str());
            first = false;
            start = text.find_first_not_of(' ', end);
            if (start == std::string::npos) {
                break;
            }
        }
    }

    void draw()
    {
        if (!screen.dirty) {
            return;
        }
        screen.dirty = false;
        std::printf("\x1b[2J\x1b[1;1H");
        std::printf("Ambrosia runner (protocol %d)\n\n", protocolVersion);
        printWrapped("  Address  ", screen.address);
        printWrapped("  State    ", screen.state);
        printWrapped("  Token    ", screen.token);
        if (!screen.warning.empty()) {
            std::printf("\n");
            printWrapped("  ", screen.warning);
        }
        std::printf("\n");
        printWrapped("  ", "Keeps the console awake. Press + to stop and return to the homebrew menu.");
        printWrapped("  ", "Runs whatever Ambrosia build it is sent: leave it running only on a trusted network.");
        std::printf("\n");
        for (const std::string& line : screen.events) {
            printWrapped("  ", line);
        }
        consoleUpdate(nullptr);
    }

    bool startsWith(const std::string& text, const std::string& prefix)
    {
        return text.compare(0, prefix.size(), prefix) == 0;
    }

    // A path the runner may read or write: under sdmc:/switch/ambrosia/, with no "." or ".."
    // part and nothing that is not printable.
    bool allowedPath(const std::string& path)
    {
        if (!startsWith(path, allowedRoot) || path.size() > 400) {
            return false;
        }
        for (char c : path) {
            if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == '"' || c == 0x7f) {
                return false;
            }
        }
        size_t start = std::strlen("sdmc:/");
        while (start <= path.size()) {
            size_t end = path.find('/', start);
            std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (part == "." || part == "..") {
                return false;
            }
            if (part.empty() && end != std::string::npos) {
                return false;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return true;
    }

    bool isHex64(const std::string& text)
    {
        return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    std::string hex(const uint8_t* bytes, size_t count)
    {
        static const char digits[] = "0123456789abcdef";
        std::string text;
        for (size_t i = 0; i < count; i++) {
            text += digits[bytes[i] >> 4];
            text += digits[bytes[i] & 15];
        }
        return text;
    }

    bool constantTimeEqual(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) {
            return false;
        }
        unsigned char difference = 0;
        for (size_t i = 0; i < a.size(); i++) {
            difference = static_cast<unsigned char>(difference | (a[i] ^ b[i]));
        }
        return difference == 0;
    }

    std::string trim(const std::string& text)
    {
        size_t begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            return {};
        }
        size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(begin, end - begin + 1);
    }

    // A file, closed when it goes out of scope. The writers close theirs themselves once the
    // writing is done, with fclose(file.release()), as they check that the close succeeded.
    struct CloseFile {
        void operator()(FILE* file) const
        {
            std::fclose(file);
        }
    };

    using File = std::unique_ptr<FILE, CloseFile>;

    std::optional<std::string> readTextFile(const char* path)
    {
        File file(std::fopen(path, "rb"));
        if (!file) {
            return std::nullopt;
        }
        std::string text;
        char buffer[4096];
        size_t count;
        while ((count = std::fread(buffer, 1, sizeof(buffer), file.get())) > 0) {
            text.append(buffer, count);
        }
        return text;
    }

    bool writeTextFile(const char* path, const std::string& text)
    {
        File file(std::fopen(path, "wb"));
        if (!file) {
            return false;
        }
        bool ok = std::fwrite(text.data(), 1, text.size(), file.get()) == text.size();
        return std::fclose(file.release()) == 0 && ok;
    }

    std::vector<std::string> lines(const std::string& text)
    {
        std::vector<std::string> result;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            std::string line = text.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            result.push_back(line);
            start = end + 1;
        }
        return result;
    }

    // The token from the SD card; empty when it is missing or too short to trust.
    std::string readToken()
    {
        std::optional<std::string> text = readTextFile(tokenPath);
        if (!text) {
            screen.token = std::string("MISSING: put one in ") + tokenPath + " (all requests are refused)";
            return {};
        }
        std::string token = trim(*text);
        if (token.size() < 16 || token.find_first_of(" \t\r\n") != std::string::npos) {
            screen.token = std::string("TOO SHORT or has spaces: ") + tokenPath + " needs 16 or more characters (all requests are refused)";
            return {};
        }
        screen.token = std::string("read from ") + tokenPath;
        return token;
    }

    // Private IPv4 ranges (and the console's own loopback): the local network.
    bool localAddress(in_addr address)
    {
        uint32_t ip = ntohl(address.s_addr);
        return (ip >> 24) == 10 || (ip >> 20) == (172u << 4 | 1u) || (ip >> 16) == (192u << 8 | 168u) || (ip >> 16) == (169u << 8 | 254u) || (ip >> 24) == 127;
    }

    // One connection, with reads and writes that give up after ioTimeoutMs without progress. The
    // socket is non-blocking and retried after short sleeps, since Ryujinx (where the runner is
    // tried out) has no working blocking socket calls and no poll on accepted sockets.
    class Connection {
    public:
        explicit Connection(int socket)
            : fd(socket)
        {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        }

        ~Connection()
        {
            close(fd);
        }

        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        std::optional<std::string> readLine()
        {
            std::string line;
            while (true) {
                size_t newline = pending.find('\n');
                if (newline != std::string::npos) {
                    line = pending.substr(0, newline);
                    pending.erase(0, newline + 1);
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    return line;
                }
                if (pending.size() > 8192 || !fill()) {
                    return std::nullopt;
                }
            }
        }

        // Reads count bytes into the file, hashing them; false on a short read or write.
        bool readToFile(FILE* file, uint64_t count, Sha256Context& hash)
        {
            while (count > 0) {
                if (pending.empty() && !fill()) {
                    return false;
                }
                size_t take = static_cast<size_t>(std::min<uint64_t>(count, pending.size()));
                sha256ContextUpdate(&hash, pending.data(), take);
                if (std::fwrite(pending.data(), 1, take, file) != take) {
                    return false;
                }
                pending.erase(0, take);
                count -= take;
            }
            return true;
        }

        bool send(const void* data, size_t size)
        {
            const char* bytes = static_cast<const char*>(data);
            u64 lastProgress = armGetSystemTick();
            while (size > 0) {
                ssize_t sent = ::send(fd, bytes, size, 0);
                if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !expired(lastProgress)) {
                    svcSleepThread(2'000'000);
                    continue;
                }
                if (sent <= 0) {
                    return false;
                }
                lastProgress = armGetSystemTick();
                bytes += sent;
                size -= static_cast<size_t>(sent);
            }
            return true;
        }

        bool send(const std::string& text)
        {
            return send(text.data(), text.size());
        }

    private:
        static bool expired(u64 since)
        {
            return armTicksToNs(armGetSystemTick() - since) > static_cast<u64>(ioTimeoutMs) * 1'000'000;
        }

        bool fill()
        {
            char buffer[64 * 1024];
            u64 started = armGetSystemTick();
            ssize_t got;
            // Ryujinx returns 0 rather than EAGAIN when no data has come yet, so 0 is retried too;
            // a PC that really closed the connection is given up on at the deadline instead.
            while ((got = read(fd, buffer, sizeof(buffer))) <= 0 && (got == 0 || errno == EAGAIN || errno == EWOULDBLOCK) && !expired(started)) {
                svcSleepThread(2'000'000);
            }
            if (got <= 0) {
                error = got == 0 ? std::string("closed by the PC") : "recv: " + std::to_string(errno) + " " + std::strerror(errno);
                return false;
            }
            pending.append(buffer, static_cast<size_t>(got));
            return true;
        }

        int fd;
        std::string pending;
        std::string error = "no error";

    public:
        const std::string& lastError() const
        {
            return error;
        }
    };

    // A job, as received and as written to job.txt for after the game.
    struct Job {
        std::string nroPath;
        std::string args;
        std::vector<std::string> results;
        // Files that were in the result folders before the game ran: path, size and time.
        std::vector<std::string> before;
    };

    std::string fileStamp(const std::string& path, const struct stat& info)
    {
        return path + "\t" + std::to_string(info.st_size) + "\t" + std::to_string(static_cast<long long>(info.st_mtime));
    }

    // The regular files directly in a folder, with their stamps.
    std::vector<std::pair<std::string, std::string>> listFolder(const std::string& folder)
    {
        std::vector<std::pair<std::string, std::string>> files;
        DIR* directory = opendir(folder.c_str());
        if (!directory) {
            return files;
        }
        while (dirent* entry = readdir(directory)) {
            std::string path = folder + entry->d_name;
            struct stat info {};
            if (entry->d_name[0] != '.' && stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
                files.emplace_back(path, fileStamp(path, info));
            }
        }
        closedir(directory);
        std::sort(files.begin(), files.end());
        return files;
    }

    std::string serialize(const Job& job)
    {
        std::string text = "nro " + job.nroPath + "\nargs " + job.args + "\n";
        for (const std::string& result : job.results) {
            text += "result " + result + "\n";
        }
        for (const std::string& stamp : job.before) {
            text += "before " + stamp + "\n";
        }
        return text;
    }

    std::optional<Job> loadJob()
    {
        std::optional<std::string> text = readTextFile(jobPath);
        if (!text) {
            return std::nullopt;
        }
        Job job;
        for (const std::string& line : lines(*text)) {
            if (startsWith(line, "nro ")) {
                job.nroPath = line.substr(4);
            } else if (startsWith(line, "args ")) {
                job.args = line.substr(5);
            } else if (startsWith(line, "result ")) {
                job.results.push_back(line.substr(7));
            } else if (startsWith(line, "before ")) {
                job.before.push_back(line.substr(7));
            }
        }
        return job;
    }

    // The cache's builds, most recently used first.
    std::vector<std::string> recentBuilds()
    {
        std::vector<std::string> recent;
        if (std::optional<std::string> text = readTextFile(recentPath)) {
            for (const std::string& line : lines(*text)) {
                if (isHex64(line)) {
                    recent.push_back(line);
                }
            }
        }
        return recent;
    }

    std::string cachedPath(const std::string& sha)
    {
        return std::string(cacheDirectory) + "/" + sha + ".nro";
    }

    // Puts sha first in the cache's order and removes the builds past cacheKeep.
    void touchCache(const std::string& sha)
    {
        std::vector<std::string> recent = recentBuilds();
        recent.erase(std::remove(recent.begin(), recent.end(), sha), recent.end());
        recent.insert(recent.begin(), sha);
        while (recent.size() > cacheKeep) {
            std::remove(cachedPath(recent.back()).c_str());
            recent.pop_back();
        }
        std::string text;
        for (const std::string& entry : recent) {
            text += entry + "\n";
        }
        writeTextFile(recentPath, text);
    }

    bool hasCached(const std::string& sha, uint64_t size)
    {
        struct stat info {};
        return stat(cachedPath(sha).c_str(), &info) == 0 && static_cast<uint64_t>(info.st_size) == size;
    }

    // An IPv4 address in network order as text.
    std::string addressName(uint32_t networkOrder)
    {
        uint32_t ip = ntohl(networkOrder);
        return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255);
    }

    std::string peerName(const sockaddr_in& peer)
    {
        return addressName(peer.sin_addr.s_addr);
    }

    // What the runner can do, decided at start.
    struct Setup {
        std::string selfPath;
        std::string token;
        bool canRun = false;
        std::string cannotRun;
        // Started by the loader after the game (which passes afterJobArgument), rather than from
        // the homebrew menu with a job left over from a game that never came back.
        bool returnedFromGame = false;
        // Different each time the runner starts, so the PC can tell the runner that started a
        // game from the one that comes back after it.
        std::string session;
    };

    // Receives a job after its "job" line. True when the game is to be started.
    bool receiveJob(Connection& connection, const Setup& setup, Job& job, const std::string& peer)
    {
        std::string sha;
        uint64_t size = 0;
        std::string path;
        while (true) {
            std::optional<std::string> line = connection.readLine();
            if (!line) {
                event("%s: the job was cut off", peer.c_str());
                return false;
            }
            if (*line == "end") {
                break;
            }
            if (startsWith(*line, "nro-sha256 ")) {
                sha = line->substr(11);
            } else if (startsWith(*line, "nro-size ")) {
                size = std::strtoull(line->c_str() + 9, nullptr, 10);
            } else if (startsWith(*line, "nro-path ")) {
                path = line->substr(9);
            } else if (startsWith(*line, "args ")) {
                job.args = line->substr(5);
            } else if (startsWith(*line, "result ")) {
                job.results.push_back(line->substr(7));
            } else {
                connection.send("error unknown job line: " + line->substr(0, 40) + "\n");
                return false;
            }
        }
        auto refuse = [&](const std::string& why) {
            event("%s: job refused: %s", peer.c_str(), why.c_str());
            connection.send("error " + why + "\n");
            return false;
        };
        if (!setup.canRun) {
            return refuse(setup.cannotRun);
        }
        for (const std::string& result : job.results) {
            if (!allowedPath(result)) {
                return refuse("result path not under " + std::string(allowedRoot) + ": " + result);
            }
        }
        if (job.args.find('\n') != std::string::npos) {
            return refuse("options with a line break");
        }
        if (!path.empty()) {
            struct stat info {};
            if (!allowedPath(path) || path.size() < 5 || path.compare(path.size() - 4, 4, ".nro") != 0) {
                return refuse("nro-path must be an .nro under " + std::string(allowedRoot));
            }
            if (stat(path.c_str(), &info) != 0) {
                return refuse("no such file: " + path);
            }
            job.nroPath = path;
        } else {
            if (!isHex64(sha) || size == 0 || size > maxNroSize) {
                return refuse("a job needs nro-path, or nro-sha256 and nro-size");
            }
            job.nroPath = cachedPath(sha);
            if (hasCached(sha, size)) {
                connection.send("have-nro\n");
                event("%s: build %.12s already on the card", peer.c_str(), sha.c_str());
            } else {
                connection.send("send-nro\n");
                setState("receiving a build from " + peer);
                draw();
                std::string partial = job.nroPath + ".part";
                File file(std::fopen(partial.c_str(), "wb"));
                if (!file) {
                    return refuse("cannot write " + partial);
                }
                std::setvbuf(file.get(), nullptr, _IOFBF, 256 * 1024);
                Sha256Context hash;
                sha256ContextCreate(&hash);
                u64 started = armGetSystemTick();
                bool ok = connection.readToFile(file.get(), size, hash);
                ok = std::fclose(file.release()) == 0 && ok;
                uint8_t digest[SHA256_HASH_SIZE];
                sha256ContextGetHash(&hash, digest);
                if (!ok) {
                    std::remove(partial.c_str());
                    return refuse("the build was cut off");
                }
                if (hex(digest, sizeof(digest)) != sha) {
                    std::remove(partial.c_str());
                    return refuse("the build's SHA-256 does not match");
                }
                std::remove(job.nroPath.c_str());
                if (std::rename(partial.c_str(), job.nroPath.c_str()) != 0) {
                    return refuse("cannot rename " + partial);
                }
                double seconds = static_cast<double>(armTicksToNs(armGetSystemTick() - started)) / 1e9;
                event("%s: received build %.12s, %.1f MB in %.1f s", peer.c_str(), sha.c_str(), static_cast<double>(size) / 1048576.0, seconds);
            }
            touchCache(sha);
        }
        std::string next = "\"" + job.nroPath + "\" --ambrosia-runner \"" + setup.selfPath + "\" " + job.args;
        if (next.size() > maxNextArgv || job.nroPath.size() > maxNextPath) {
            return refuse("the options are too long for the homebrew loader (2047 characters in all)");
        }
        for (const std::string& result : job.results) {
            if (result.back() == '/') {
                for (const auto& file : listFolder(result)) {
                    job.before.push_back(file.second);
                }
            }
        }
        // The game's log is written afresh each start; the old one would be sent if it never ran.
        std::remove("sdmc:/switch/ambrosia/log.txt");
        if (!writeTextFile(jobPath, serialize(job))) {
            return refuse(std::string("cannot write ") + jobPath);
        }
        Result rc = envSetNextLoad(job.nroPath.c_str(), next.c_str());
        if (R_FAILED(rc)) {
            std::remove(jobPath);
            return refuse("envSetNextLoad failed: " + std::to_string(rc));
        }
        connection.send("launching " + setup.session + "\n");
        event("%s: starting %s %s", peer.c_str(), job.nroPath.c_str(), job.args.c_str());
        return true;
    }

    // The console's mode and clock rates in MHz, as "mode=docked cpu=1020 gpu=768 mem=1600":
    // whether a run was handheld or docked, and whether sys-clk had changed the clocks.
    std::string consoleInfo()
    {
        std::string text = std::string("mode=") + (appletGetOperationMode() == AppletOperationMode_Handheld ? "handheld" : "docked");
        const std::pair<const char*, PcvModuleId> clocks[] = { { "cpu", PcvModuleId_CpuBus }, { "gpu", PcvModuleId_GPU }, { "mem", PcvModuleId_EMC } };
        // clkrst is the clock service from 8.0.0 on; older systems report the clocks as unknown.
        bool ready = hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize());
        for (const auto& [name, module] : clocks) {
            u32 hz = 0;
            bool known = false;
            ClkrstSession session;
            if (ready && R_SUCCEEDED(clkrstOpenSession(&session, module, 3))) {
                known = R_SUCCEEDED(clkrstGetClockRate(&session, &hz));
                clkrstCloseSession(&session);
            }
            text += std::string(" ") + name + "=" + (known ? std::to_string(hz / 1'000'000) : std::string("unknown"));
        }
        if (ready) {
            clkrstExit();
        }
        return text;
    }

    // Sends the job's result files; clears the job when the PC acknowledges them.
    // Sends a file's bytes after a header line built from its size; false when the connection
    // fails partway, or when the file cannot be read, in which case nothing is sent.
    bool sendFile(Connection& connection, const std::string& path, const std::function<std::string(const struct stat&)>& header, bool& opened)
    {
        struct stat info {};
        File file(stat(path.c_str(), &info) == 0 ? std::fopen(path.c_str(), "rb") : nullptr);
        opened = file != nullptr;
        if (!file) {
            return false;
        }
        bool ok = connection.send(header(info));
        std::vector<char> buffer(256 * 1024);
        size_t count;
        while (ok && (count = std::fread(buffer.data(), 1, buffer.size(), file.get())) > 0) {
            ok = connection.send(buffer.data(), count);
        }
        return ok;
    }

    void sendResults(Connection& connection, const std::optional<Job>& job, bool returned, const std::string& peer)
    {
        if (!job) {
            connection.send("error no results: no job has run since the last fetch\n");
            return;
        }
        std::string header = std::string("results\nreturned ") + (returned ? "yes" : "no") + "\nconsole " + consoleInfo() + "\n";
        if (!connection.send(header)) {
            return;
        }
        int sent = 0;
        for (const std::string& result : job->results) {
            std::vector<std::string> paths;
            if (result.back() == '/') {
                for (const auto& file : listFolder(result)) {
                    if (std::find(job->before.begin(), job->before.end(), file.second) == job->before.end()) {
                        paths.push_back(file.first);
                    }
                }
            } else {
                paths.push_back(result);
            }
            for (const std::string& path : paths) {
                bool opened = false;
                bool ok = sendFile(connection, path, [&](const struct stat& info) { return "file " + path + " " + std::to_string(info.st_size) + "\n"; }, opened);
                if (!opened) {
                    if (!connection.send("missing " + path + "\n")) {
                        return;
                    }
                    continue;
                }
                if (!ok) {
                    event("%s: sending %s failed", peer.c_str(), path.c_str());
                    return;
                }
                sent++;
            }
        }
        if (!connection.send("end\n")) {
            return;
        }
        std::optional<std::string> ack = connection.readLine();
        if (ack && *ack == "ack") {
            std::remove(jobPath);
            connection.send("cleared\n");
            event("%s: fetched %d result files", peer.c_str(), sent);
        }
    }

    // Sends the newest of Atmosphère's crash reports, which it writes when a program crashes (the
    // runner's own folder rule is for writing; this is the one place outside it that is read).
    void sendCrashReport(Connection& connection, const std::string& peer)
    {
        const char* folder = "sdmc:/atmosphere/crash_reports/";
        std::string newest;
        time_t newestTime = 0;
        if (DIR* directory = opendir(folder)) {
            while (dirent* entry = readdir(directory)) {
                std::string path = std::string(folder) + entry->d_name;
                struct stat info {};
                if (entry->d_name[0] != '.' && stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && (newest.empty() || info.st_mtime > newestTime || (info.st_mtime == newestTime && path > newest))) {
                    newest = path;
                    newestTime = info.st_mtime;
                }
            }
            closedir(directory);
        }
        bool opened = false;
        bool ok = !newest.empty() && sendFile(connection, newest, [&](const struct stat& info) { return "file " + newest + " " + std::to_string(info.st_size) + " " + std::to_string(static_cast<long long>(info.st_mtime)) + "\n"; }, opened);
        if (!opened) {
            connection.send("none\nend\n");
            return;
        }
        if (ok) {
            connection.send("end\n");
            event("%s: sent crash report %s", peer.c_str(), newest.c_str());
        }
    }

    // Handles one connection. True when a job is to be started.
    bool handle(int socket, const sockaddr_in& peerAddress, const Setup& setup, Job& launch)
    {
        Connection connection(socket);
        std::string peer = peerName(peerAddress);
        if (!localAddress(peerAddress.sin_addr)) {
            event("%s: refused, not a local network address", peer.c_str());
            return false;
        }
        std::optional<std::string> hello = connection.readLine();
        std::optional<std::string> token = hello ? connection.readLine() : std::nullopt;
        std::optional<std::string> command = token ? connection.readLine() : std::nullopt;
        if (!command || *hello != "ambrosia-runner " + std::to_string(protocolVersion)) {
            connection.send("error expected protocol ambrosia-runner " + std::to_string(protocolVersion) + "\n");
            std::string got = hello ? hello->substr(0, 24) : "nothing";
            std::replace_if(got.begin(), got.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e; }, '?');
            event("%s: refused, not the runner's protocol (%s, %s; %s)", peer.c_str(), got.c_str(), command ? "complete" : "cut off", connection.lastError().c_str());
            return false;
        }
        if (setup.token.empty() || !startsWith(*token, "token ") || !constantTimeEqual(token->substr(6), setup.token)) {
            // A pause, so tokens cannot be guessed quickly.
            svcSleepThread(1'000'000'000);
            connection.send("error wrong token\n");
            event("%s: refused, wrong token", peer.c_str());
            return false;
        }
        std::optional<Job> pending = loadJob();
        if (*command == "status") {
            connection.send(std::string("state ") + (pending ? "results" : "idle") + " session " + setup.session + "\n");
            return false;
        }
        if (*command == "results") {
            sendResults(connection, pending, setup.returnedFromGame, peer);
            return false;
        }
        if (*command == "crash-report") {
            sendCrashReport(connection, peer);
            return false;
        }
        if (*command == "job") {
            if (pending) {
                // The PC asked for a new job without fetching the last one's results.
                std::remove(jobPath);
                event("%s: dropped the unfetched results of the last job", peer.c_str());
            }
            return receiveJob(connection, setup, launch, peer);
        }
        connection.send("error unknown command\n");
        return false;
    }

    std::string addressText()
    {
        u32 address = 0;
        bool known = R_SUCCEEDED(nifmInitialize(NifmServiceType_User)) && R_SUCCEEDED(nifmGetCurrentIpAddress(&address));
        nifmExit();
        if (!known || address == 0) {
            return "no network (connect the console to the network, then restart the runner)";
        }
        return addressName(address) + ":" + std::to_string(port);
    }
}

int main(int argc, char** argv)
{
    consoleInit(nullptr);
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Setup setup;
    setup.session = std::to_string(randomGet64());
    for (int i = 1; i < argc; i++) {
        setup.returnedFromGame = setup.returnedFromGame || std::strcmp(argv[i], afterJobArgument) == 0;
    }
    // The homebrew menu passes the program's own path first, which the game is told to return to.
    setup.selfPath = argc > 0 && startsWith(argv[0], "sdmc:/") && std::strstr(argv[0], ".nro") ? argv[0] : defaultSelfPath;

    mkdir("sdmc:/switch/ambrosia", 0777);
    mkdir(workDirectory, 0777);
    mkdir(cacheDirectory, 0777);
    setup.token = readToken();

    AppletType type = appletGetAppletType();
    if (type != AppletType_Application && type != AppletType_SystemApplication) {
        setup.cannotRun = "started from the album, where Ambrosia has too little memory: start the runner by holding R while starting a game";
    } else if (!envHasNextLoad()) {
        setup.cannotRun = "the loader that started the runner cannot start another program (envHasNextLoad is false)";
    } else if (setup.token.empty()) {
        setup.cannotRun = "no token";
    } else {
        setup.canRun = true;
    }
    if (!setup.canRun) {
        screen.warning = "Jobs are refused: " + setup.cannotRun + ".";
    }

    // Keeps the console from dimming and sleeping while it waits (the game does the same while
    // it runs a job).
    appletSetAutoSleepDisabled(true);
    appletSetMediaPlaybackState(true);

    SocketInitConfig config = *socketGetDefaultInitConfig();
    config.tcp_tx_buf_size = 256 * 1024;
    config.tcp_rx_buf_size = 256 * 1024;
    config.tcp_tx_buf_max_size = 1024 * 1024;
    config.tcp_rx_buf_max_size = 1024 * 1024;
    Result sockets = socketInitialize(&config);
    int listener = -1;
    // Opens the port; tried again every two seconds while it fails, as the port may still be
    // held for a while after the last program that had it open exits.
    u64 lastListenTry = 0;
    bool listenFailureShown = false;
    auto openListener = [&]() {
        lastListenTry = armGetSystemTick();
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) {
            if (!listenFailureShown) {
                event("cannot open a socket for port %u yet (%s); trying again every 2 s", port, std::strerror(errno));
                listenFailureShown = true;
            }
            return;
        }
        int yes = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listener, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 || listen(listener, 2) != 0) {
            if (!listenFailureShown) {
                event("cannot listen on port %u yet (%s); trying again every 2 s", port, std::strerror(errno));
                listenFailureShown = true;
            }
            close(listener);
            listener = -1;
        } else if (listenFailureShown) {
            event("listening on port %u", port);
            listenFailureShown = false;
        }
    };
    if (R_SUCCEEDED(sockets)) {
        screen.address = addressText();
        openListener();
    } else {
        event("socketInitialize failed: 0x%x", sockets);
    }

    std::optional<Job> pending = loadJob();
    if (pending) {
        if (setup.returnedFromGame) {
            event("the game returned; results of %s are ready", pending->args.c_str());
        } else {
            event("the last job's game did not return to the runner (it crashed or was closed); its results are ready");
        }
    }
    auto idleState = [&]() {
        setState(loadJob() ? "results ready, waiting for the PC to fetch them" : "waiting for a job");
    };
    idleState();

    bool launch = false;
    Job job;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }
        draw();
        if (listener < 0) {
            if (R_SUCCEEDED(sockets) && armTicksToNs(armGetSystemTick() - lastListenTry) > 2'000'000'000) {
                openListener();
            }
            svcSleepThread(50'000'000);
            continue;
        }
        pollfd entry { listener, POLLIN, 0 };
        if (poll(&entry, 1, 50) <= 0 || !(entry.revents & POLLIN)) {
            continue;
        }
        sockaddr_in peer {};
        socklen_t length = sizeof(peer);
        int client = accept(listener, reinterpret_cast<sockaddr*>(&peer), &length);
        if (client < 0) {
            continue;
        }
        job = Job {};
        if (handle(client, peer, setup, job)) {
            launch = true;
            break;
        }
        idleState();
    }

    if (listener >= 0) {
        close(listener);
    }
    if (launch) {
        setState("starting the game");
        draw();
    } else {
        // Back to the homebrew menu: the system's own sleep and dimming again.
        appletSetMediaPlaybackState(false);
        appletSetAutoSleepDisabled(false);
    }
    socketExit();
    consoleExit(nullptr);
    return 0;
}
