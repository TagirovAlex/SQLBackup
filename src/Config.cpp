#include "Config.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r");
    return s.substr(start, end - start + 1);
}

static std::string toLower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool Config::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string currentSection;
    std::string line;

    while (std::getline(file, line)) {
        std::string trimmed = trim(line);

        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;

        if (trimmed[0] == '[') {
            auto end = trimmed.find(']');
            if (end != std::string::npos) {
                currentSection = trim(trimmed.substr(1, end - 1));
            }
            continue;
        }

        auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));

        if (key.empty()) continue;

        m_data[currentSection][key] = value;
    }

    return true;
}

std::string Config::getValue(const std::string& section, const std::string& key, const std::string& defaultVal) const {
    auto secIt = m_data.find(toLower(section));
    if (secIt == m_data.end()) {
        secIt = m_data.find(section);
        if (secIt == m_data.end()) return defaultVal;
    }
    auto keyIt = secIt->second.find(toLower(key));
    if (keyIt == secIt->second.end()) {
        keyIt = secIt->second.find(key);
        if (keyIt == secIt->second.end()) return defaultVal;
    }
    return keyIt->second;
}

std::string Config::sourcePath() const { return getValue("General", "SourcePath"); }
std::string Config::destPath() const { return getValue("General", "DestPath"); }
std::string Config::nameTemplate() const { return getValue("General", "NameTemplate"); }
std::string Config::dateFormat() const { return getValue("General", "DateFormat", "%d.%m.%Y"); }
std::string Config::fileExtension() const { return getValue("General", "FileExtension", ".bak"); }
bool Config::debug() const {
    auto v = getValue("General", "Debug", "false");
    std::string low = toLower(v);
    return low == "true" || low == "yes" || low == "1";
}

std::string Config::mailServer() const { return getValue("Mail", "Server"); }
int Config::mailPort() const {
    auto v = getValue("Mail", "Port", "25");
    return std::stoi(v);
}
std::string Config::senderName() const { return getValue("Mail", "SenderName"); }
std::string Config::senderEmail() const { return getValue("Mail", "SenderEmail"); }
std::vector<std::string> Config::recipients() const {
    std::vector<std::string> result;
    std::string raw = getValue("Mail", "Recipients");
    std::stringstream ss(raw);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::string t = trim(item);
        if (!t.empty()) result.push_back(t);
    }
    return result;
}
bool Config::mailAuth() const {
    auto v = getValue("Mail", "Auth", "false");
    std::string low = toLower(v);
    return low == "true" || low == "yes" || low == "1";
}
std::string Config::mailUsername() const { return getValue("Mail", "Username"); }
std::string Config::mailPassword() const { return getValue("Mail", "Password"); }

std::string Config::logPath() const { return getValue("Log", "LogPath", "logs"); }
int Config::maxLogs() const {
    auto v = getValue("Log", "MaxLogs", "30");
    return std::max(1, std::stoi(v));
}
