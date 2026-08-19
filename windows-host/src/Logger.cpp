#include "Logger.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <windows.h>

namespace remcote {
namespace {

std::mutex g_mutex;
std::ofstream g_file;
Logger::Sink g_sink;
bool g_shuttingDown = false;
size_t g_inFlightSinks = 0;
std::condition_variable g_sinkIdle;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_lastRateLimited;
std::string g_filePath;
size_t g_fileBytes = 0;
constexpr size_t kMaxLogBytes = 5 * 1024 * 1024;

const char* LevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Info: return "INFO";
    case LogLevel::Warning: return "WARN";
    case LogLevel::Error: return "ERROR";
    case LogLevel::Debug: return "DEBUG";
    }
    return "INFO";
}

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm local{};
    localtime_s(&local, &time);

    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << milliseconds.count();
    return out.str();
}

void RotateFileLocked() {
    if (!g_file.is_open() || g_filePath.empty()) return;
    g_file.close();

    const std::filesystem::path current(g_filePath);
    const std::filesystem::path previous = current.parent_path() / "remcote-host.previous.log";
    std::error_code error;
    std::filesystem::remove(previous, error);
    if (error) {
        OutputDebugStringA("[ReMCote] Could not remove the previous diagnostic log; rotation skipped.\n");
        error.clear();
        g_file.open(g_filePath, std::ios::out | std::ios::app);
        return;
    }
    error.clear();
    std::filesystem::rename(current, previous, error);
    if (error) {
        OutputDebugStringA("[ReMCote] Could not rotate the diagnostic log; preserving the current file.\n");
        error.clear();
        g_file.open(g_filePath, std::ios::out | std::ios::app);
        return;
    }
    g_file.open(g_filePath, std::ios::out | std::ios::trunc);
    g_fileBytes = 0;
}

} // namespace

void Logger::Initialize(const std::string& filePath, Sink sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open()) g_file.close();
    g_filePath = filePath;
    g_fileBytes = 0;
    std::error_code error;
    if (std::filesystem::exists(g_filePath, error)) {
        g_fileBytes = static_cast<size_t>(std::filesystem::file_size(g_filePath, error));
        if (!error && g_fileBytes >= kMaxLogBytes) {
            const std::filesystem::path current(g_filePath);
            const std::filesystem::path previous = current.parent_path() / "remcote-host.previous.log";
            std::filesystem::remove(previous, error);
            if (!error) {
                std::filesystem::rename(current, previous, error);
                if (!error) g_fileBytes = 0;
            }
            if (error) {
                OutputDebugStringA("[ReMCote] Could not archive an oversized diagnostic log; preserving it.\n");
                error.clear();
            }
        }
    }
    g_file.open(filePath, std::ios::out | std::ios::app);
    g_sink = std::move(sink);
    g_shuttingDown = false;
}

void Logger::Shutdown() {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_shuttingDown = true;
    g_sink = {};
    g_sinkIdle.wait(lock, [] { return g_inFlightSinks == 0; });
    if (g_file.is_open()) {
        g_file.flush();
        g_file.close();
    }
}

void Logger::Log(LogLevel level, const std::string& message) {
    const std::string line = "[" + Timestamp() + "] [" + LevelName(level) + "] " + message;

    Logger::Sink sink;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_shuttingDown) return;
        if (g_file.is_open()) {
            g_file << line << '\n';
            g_file.flush();
            g_fileBytes += line.size() + 1;
            if (g_fileBytes >= kMaxLogBytes) RotateFileLocked();
        }
        sink = g_sink;
        if (sink) ++g_inFlightSinks;
    }

    OutputDebugStringA((line + "\n").c_str());
    if (sink) {
        try {
            sink(line);
        } catch (...) {
            OutputDebugStringA("[ReMCote] Diagnostic UI sink failed; continuing without that log entry.\n");
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        if (--g_inFlightSinks == 0) g_sinkIdle.notify_all();
    }
}

void Logger::Logf(LogLevel level, const char* format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    buffer[sizeof(buffer) - 1] = '\0';
    va_end(args);
    Log(level, buffer);
}

void Logger::WarningRateLimited(const std::string& key,
                                const std::string& message,
                                unsigned int intervalMs) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto found = g_lastRateLimited.find(key);
        if (found != g_lastRateLimited.end() &&
            now - found->second < std::chrono::milliseconds(intervalMs)) {
            return;
        }
        g_lastRateLimited[key] = now;
    }
    Log(LogLevel::Warning, message);
}

std::string Logger::RedactUrl(const std::string& url) {
    std::string redacted = url;
    const size_t query = redacted.find('?');
    const size_t fragment = redacted.find('#');
    size_t sensitiveStart = std::string::npos;
    if (query != std::string::npos) sensitiveStart = query;
    if (fragment != std::string::npos &&
        (sensitiveStart == std::string::npos || fragment < sensitiveStart)) {
        sensitiveStart = fragment;
    }
    if (sensitiveStart != std::string::npos) redacted.erase(sensitiveStart);
    const size_t scheme = redacted.find("://");
    const size_t at = redacted.find('@', scheme == std::string::npos ? 0 : scheme + 3);
    if (at != std::string::npos) {
        const size_t start = scheme == std::string::npos ? 0 : scheme + 3;
        redacted.replace(start, at - start + 1, "***@");
    }
    return redacted;
}

} // namespace remcote