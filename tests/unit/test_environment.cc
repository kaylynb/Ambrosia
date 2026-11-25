#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace {
    struct IsolatedUserData {
        IsolatedUserData()
        {
            directory = std::filesystem::temp_directory_path() / ("ambrosia-test-" + std::to_string(getpid()) + "-userdata");
            std::error_code error;
            std::filesystem::remove_all(directory, error);
            std::filesystem::create_directories(directory, error);
#ifdef _WIN32
            _putenv_s("XDG_DATA_HOME", directory.string().c_str());
#else
            setenv("XDG_DATA_HOME", directory.string().c_str(), 1);
#endif
        }

        ~IsolatedUserData()
        {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }

        std::filesystem::path directory;
    };

    const IsolatedUserData isolatedUserData;
}
