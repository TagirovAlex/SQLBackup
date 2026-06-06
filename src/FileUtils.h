#pragma once
#include <string>
#include <optional>
#include <cstdint>

struct BackupFile {
    std::string fullPath;
    std::string fileName;
    uint64_t fileSize = 0;
};

struct BackupResult {
    std::string originalName;
    std::string renamedName;
    std::string sourceFullPath;
    std::string destFullPath;
    uint64_t fileSize = 0;
    bool success = false;
};

class FileUtils {
public:
    static std::optional<BackupFile> findNewestFile(const std::string& directory, const std::string& extension);
    static std::string generateFileName(const std::string& nameTemplate, const std::string& dateFormat);
    static std::optional<std::string> renameFile(const std::string& oldPath, const std::string& newName);
    static bool copyFile(const std::string& src, const std::string& dest);
    static bool verifyFiles(const std::string& file1, const std::string& file2);
    static bool deleteFile(const std::string& path);
    static std::string computeSha256(const std::string& filePath);
};
