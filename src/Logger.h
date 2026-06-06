#pragma once
#include <string>
#include <fstream>
#include <mutex>

class Logger {
public:
    Logger() = default;
    ~Logger();

    bool init(const std::string& logDir, int maxLogs);
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);
    void cleanupOldLogs();

private:
    std::string getTimestamp();
    void write(const std::string& level, const std::string& message);

    std::string m_logFilePath;
    std::string m_logDir;
    int m_maxLogs = 30;
    std::ofstream m_stream;
    std::mutex m_mutex;
};
