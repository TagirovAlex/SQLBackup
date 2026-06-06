#include "FileUtils.h"
#include <windows.h>
#include <wincrypt.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <algorithm>
#include <vector>

namespace fs = std::filesystem;

std::optional<BackupFile> FileUtils::findNewestFile(const std::string& directory, const std::string& extension) {
    if (!fs::exists(directory)) return std::nullopt;

    std::vector<BackupFile> files;

    for (auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        auto ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string extLower = extension;
        std::transform(extLower.begin(), extLower.end(), extLower.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != extLower) continue;

        BackupFile bf;
        bf.fullPath = path.string();
        bf.fileName = path.filename().string();
        bf.fileSize = static_cast<uint64_t>(fs::file_size(path));
        files.push_back(bf);
    }

    if (files.empty()) return std::nullopt;

    auto newest = std::max_element(files.begin(), files.end(),
        [](const BackupFile& a, const BackupFile& b) {
            return fs::last_write_time(a.fullPath) < fs::last_write_time(b.fullPath);
        });

    return *newest;
}

std::string FileUtils::generateFileName(const std::string& nameTemplate, const std::string& dateFormat) {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);

    std::vector<char> buf(256);
    size_t len = std::strftime(buf.data(), buf.size(), dateFormat.c_str(), &tm);

    std::string dateStr(buf.data(), len);
    std::string result = nameTemplate;
    size_t pos = result.find("{date}");
    while (pos != std::string::npos) {
        result.replace(pos, 6, dateStr);
        pos = result.find("{date}", pos + dateStr.length());
    }
    return result;
}

std::optional<std::string> FileUtils::renameFile(const std::string& oldPath, const std::string& newName) {
    fs::path oldFsPath(oldPath);
    fs::path parentDir = oldFsPath.parent_path();
    fs::path newFsPath = parentDir / newName;

    if (fs::exists(newFsPath)) {
        return std::nullopt;
    }

    fs::rename(oldPath, newFsPath);
    return newFsPath.string();
}

bool FileUtils::copyFile(const std::string& src, const std::string& dest) {
    fs::path destPath(dest);
    fs::create_directories(destPath.parent_path());

    return CopyFileExA(
        src.c_str(),
        dest.c_str(),
        nullptr,
        nullptr,
        nullptr,
        0
    ) != 0;
}

std::string FileUtils::computeSha256(const std::string& filePath) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;

    if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "";
    }

    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }

    char buffer[65536];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        CryptHashData(hHash, reinterpret_cast<BYTE*>(buffer), static_cast<DWORD>(file.gcount()), 0);
    }
    file.close();

    BYTE hash[32];
    DWORD hashLen = 32;
    CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0);

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    std::ostringstream ss;
    for (DWORD i = 0; i < hashLen; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return ss.str();
}

bool FileUtils::verifyFiles(const std::string& file1, const std::string& file2) {
    auto hash1 = computeSha256(file1);
    if (hash1.empty()) return false;

    auto hash2 = computeSha256(file2);
    if (hash2.empty()) return false;

    return hash1 == hash2;
}

bool FileUtils::deleteFile(const std::string& path) {
    return fs::remove(path);
}
