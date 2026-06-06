#include "FileUtils.h"
#include <windows.h>
#include <wincrypt.h>
#include <filesystem>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <algorithm>
#include <vector>

namespace fs = std::filesystem;

static const size_t COPY_BUF_SIZE = 4 * 1024 * 1024;

static std::string hashToString(HCRYPTHASH hHash) {
    BYTE hash[32];
    DWORD hashLen = 32;
    CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0);
    std::ostringstream ss;
    for (DWORD i = 0; i < hashLen; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    return ss.str();
}

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

CopyResult FileUtils::copyWithHash(const std::string& src, const std::string& dest) {
    fs::path destPath(dest);
    fs::create_directories(destPath.parent_path());

    HANDLE hSrc = CreateFileA(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hSrc == INVALID_HANDLE_VALUE) return {};

    HANDLE hDst = CreateFileA(dest.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hDst == INVALID_HANDLE_VALUE) {
        CloseHandle(hSrc);
        return {};
    }

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    bool cryptoOk = CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
                    CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash);
    if (!cryptoOk) {
        CloseHandle(hDst);
        CloseHandle(hSrc);
        return {};
    }

    char* buffer = static_cast<char*>(VirtualAlloc(nullptr, COPY_BUF_SIZE, MEM_COMMIT, PAGE_READWRITE));
    if (!buffer) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        CloseHandle(hDst);
        CloseHandle(hSrc);
        return {};
    }

    bool success = true;
    DWORD bytesRead;

    while (success) {
        if (!ReadFile(hSrc, buffer, COPY_BUF_SIZE, &bytesRead, nullptr)) {
            success = false;
            break;
        }
        if (bytesRead == 0) break;

        DWORD bytesWritten;
        if (!WriteFile(hDst, buffer, bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead) {
            success = false;
            break;
        }

        CryptHashData(hHash, reinterpret_cast<BYTE*>(buffer), bytesRead, 0);
    }

    VirtualFree(buffer, 0, MEM_RELEASE);

    std::string hash;
    if (success) {
        hash = hashToString(hHash);
    }

    FlushFileBuffers(hDst);

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    CloseHandle(hDst);
    CloseHandle(hSrc);

    if (!success) {
        DeleteFileA(dest.c_str());
        return {};
    }

    return {true, hash};
}

std::string FileUtils::computeSha256(const std::string& filePath) {
    HANDLE hFile = CreateFileA(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return "";

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        if (hProv) CryptReleaseContext(hProv, 0);
        CloseHandle(hFile);
        return "";
    }

    char buffer[65536];
    DWORD bytesRead;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        CryptHashData(hHash, reinterpret_cast<BYTE*>(buffer), bytesRead, 0);
    }

    std::string hash = hashToString(hHash);

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    CloseHandle(hFile);
    return hash;
}

bool FileUtils::deleteFile(const std::string& path) {
    return fs::remove(path);
}
