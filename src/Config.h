#pragma once
#include <string>
#include <map>
#include <vector>

class Config {
public:
    Config() = default;
    bool load(const std::string& path);

    std::vector<std::string> backupSections() const;

    std::string sourcePath(const std::string& section = "") const;
    std::string destPath(const std::string& section = "") const;
    std::string nameTemplate(const std::string& section = "") const;
    std::string dateFormat(const std::string& section = "") const;
    std::string fileExtension(const std::string& section = "") const;
    bool debug() const;

    std::string mailServer() const;
    int mailPort() const;
    std::string senderName() const;
    std::string senderEmail() const;
    std::vector<std::string> recipients() const;
    bool mailAuth() const;
    std::string mailUsername() const;
    std::string mailPassword() const;

    std::string logPath() const;
    int maxLogs() const;

private:
    std::string getValue(const std::string& section, const std::string& key, const std::string& defaultVal = "") const;

    std::map<std::string, std::map<std::string, std::string>> m_data;
};
