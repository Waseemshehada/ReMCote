#pragma once

#include <functional>
#include <string>

namespace remcote {

enum class LogLevel { Info, Warning, Error, Debug };

class Logger {
public:
    using Sink = std::function<void(const std::string& line)>;

    static void Initialize(const std::string& filePath, Sink sink = {});
    static void Shutdown();

    static void Log(LogLevel level, const std::string& message);
    static void Logf(LogLevel level, const char* format, ...);
    static void WarningRateLimited(const std::string& key,
                                   const std::string& message,
                                   unsigned int intervalMs = 5000);
    static std::string RedactUrl(const std::string& url);

    static void Info(const std::string& message) { Log(LogLevel::Info, message); }
    static void Warning(const std::string& message) { Log(LogLevel::Warning, message); }
    static void Error(const std::string& message) { Log(LogLevel::Error, message); }
    static void Debug(const std::string& message) { Log(LogLevel::Debug, message); }

    template <typename... Args>
    static void Infof(const char* format, Args... args) {
        Logf(LogLevel::Info, format, args...);
    }

    template <typename... Args>
    static void Warningf(const char* format, Args... args) {
        Logf(LogLevel::Warning, format, args...);
    }

    template <typename... Args>
    static void Errorf(const char* format, Args... args) {
        Logf(LogLevel::Error, format, args...);
    }

    template <typename... Args>
    static void Debugf(const char* format, Args... args) {
        Logf(LogLevel::Debug, format, args...);
    }

private:
    Logger() = delete;
};

} // namespace remcote