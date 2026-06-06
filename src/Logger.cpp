#include "Logger.h"
#include <windows.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <vector>

namespace fs = std::filesystem;

Logger::~Logger() {
    if (m_stream.is_open()) {
        m_stream.close();
    }
}

bool Logger::init(const std::string& logDir, int maxLogs) {
    m_logDir = logDir;
    m_maxLogs = maxLogs;

    fs::create_directories(m_logDir);

    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);

    std::ostringstream ss;
    ss << "SQLBackup_"
       << std::put_time(&tm, "%Y%m%d_%H%M%S")
       << ".log";

    m_logFilePath = (fs::path(m_logDir) / ss.str()).string();

    m_stream.open(m_logFilePath, std::ios::out | std::ios::app);
    if (!m_stream.is_open()) return false;

    info("=== SQLBackup started ===");
    return true;
}

std::string Logger::getTimestamp() {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void Logger::write(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_stream.is_open()) return;

    m_stream << "[" << getTimestamp() << "] [" << level << "] " << message << std::endl;
    m_stream.flush();
}

void Logger::info(const std::string& message) {
    write("INFO", message);
}

void Logger::warn(const std::string& message) {
    write("WARN", message);
}

void Logger::error(const std::string& message) {
    write("ERROR", message);
}

void Logger::cleanupOldLogs() {
    if (!fs::exists(m_logDir)) return;

    std::vector<fs::directory_entry> logFiles;
    for (auto& entry : fs::directory_iterator(m_logDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".log") {
            logFiles.push_back(entry);
        }
    }

    if (logFiles.size() <= static_cast<size_t>(m_maxLogs)) return;

    std::sort(logFiles.begin(), logFiles.end(),
        [](const fs::directory_entry& a, const fs::directory_entry& b) {
            return fs::last_write_time(a) < fs::last_write_time(b);
        });

    size_t toRemove = logFiles.size() - static_cast<size_t>(m_maxLogs);
    for (size_t i = 0; i < toRemove; ++i) {
        fs::remove(logFiles[i].path());
        info("Removed old log file: " + logFiles[i].path().filename().string());
    }
}
