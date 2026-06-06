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

static std::string generateUniquePath(const std::string& dest) {
    fs::path p(dest);
    std::string stem = p.stem().string();
    std::string ext = p.extension().string();
    fs::path parent = p.parent_path();
    for (int i = 1; i <= 999; ++i) {
        std::ostringstream ss;
        ss << stem << "_" << std::setw(3) << std::setfill('0') << i << ext;
        fs::path candidate = parent / ss.str();
        if (!fs::exists(candidate)) return candidate.string();
    }
    return {};
}

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

struct AsyncContext {
    HANDLE hDst;
    char* buffers[2];
    DWORD sizes[2];
    HANDLE hReady;
    HANDLE hFree[2];
    volatile bool error;
};

static DWORD WINAPI writerThread(LPVOID param) {
    AsyncContext* ctx = static_cast<AsyncContext*>(param);

    while (true) {
        WaitForSingleObject(ctx->hReady, INFINITE);

        int idx = -1;
        for (int i = 0; i < 2; ++i) {
            DWORD s = ctx->sizes[i];
            if (s != 0 && s != (DWORD)-1) {
                idx = i;
                break;
            }
        }

        if (idx == -1) {
            DWORD s0 = ctx->sizes[0];
            DWORD s1 = ctx->sizes[1];
            if ((s0 == 0 || s0 == (DWORD)-1) && (s1 == 0 || s1 == (DWORD)-1)) break;
            continue;
        }

        DWORD size = ctx->sizes[idx];
        ctx->sizes[idx] = (DWORD)-1;

        DWORD written;
        if (!WriteFile(ctx->hDst, ctx->buffers[idx], size, &written, nullptr) ||
            written != size) {
            ctx->error = true;
            SetEvent(ctx->hFree[0]);
            SetEvent(ctx->hFree[1]);
            break;
        }
        SetEvent(ctx->hFree[idx]);
    }
    return 0;
}

CopyResult FileUtils::copyWithHash(const std::string& src, const std::string& dest, OnExists onExists) {
    fs::path destFsPath(dest);
    fs::create_directories(destFsPath.parent_path());

    std::string actualDest = dest;
    if (fs::exists(dest)) {
        switch (onExists) {
            case OnExists::Skip:
                return {true, "", dest, true};
            case OnExists::Error:
                return {false, "", dest, false};
            case OnExists::Suffix: {
                auto unique = generateUniquePath(dest);
                if (unique.empty()) return {false, "", dest, false};
                actualDest = unique;
                break;
            }
            case OnExists::Overwrite:
            default:
                break;
        }
    }

    HANDLE hSrc = CreateFileA(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hSrc == INVALID_HANDLE_VALUE) return {false, "", actualDest, false};

    HANDLE hDst = CreateFileA(actualDest.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hDst == INVALID_HANDLE_VALUE) {
        CloseHandle(hSrc);
        return {false, "", actualDest, false};
    }

    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    bool cryptoOk = CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
                    CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash);
    if (!cryptoOk) {
        CloseHandle(hDst);
        CloseHandle(hSrc);
        return {false, "", actualDest, false};
    }

    char* buffers[2];
    buffers[0] = static_cast<char*>(VirtualAlloc(nullptr, COPY_BUF_SIZE, MEM_COMMIT, PAGE_READWRITE));
    buffers[1] = static_cast<char*>(VirtualAlloc(nullptr, COPY_BUF_SIZE, MEM_COMMIT, PAGE_READWRITE));
    if (!buffers[0] || !buffers[1]) {
        if (buffers[0]) VirtualFree(buffers[0], 0, MEM_RELEASE);
        if (buffers[1]) VirtualFree(buffers[1], 0, MEM_RELEASE);
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        CloseHandle(hDst);
        CloseHandle(hSrc);
        return {false, "", actualDest, false};
    }

    AsyncContext ctx;
    ctx.hDst = hDst;
    ctx.buffers[0] = buffers[0];
    ctx.buffers[1] = buffers[1];
    ctx.sizes[0] = 0;
    ctx.sizes[1] = 0;
    ctx.hReady = CreateSemaphoreA(nullptr, 0, 10, nullptr);
    ctx.hFree[0] = CreateEventA(nullptr, FALSE, TRUE, nullptr);
    ctx.hFree[1] = CreateEventA(nullptr, FALSE, TRUE, nullptr);
    ctx.error = false;

    HANDLE hThread = CreateThread(nullptr, 0, writerThread, &ctx, 0, nullptr);
    if (!hThread) {
        CloseHandle(ctx.hReady);
        CloseHandle(ctx.hFree[0]);
        CloseHandle(ctx.hFree[1]);
        VirtualFree(buffers[0], 0, MEM_RELEASE);
        VirtualFree(buffers[1], 0, MEM_RELEASE);
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        CloseHandle(hDst);
        CloseHandle(hSrc);
        return {false, "", actualDest, false};
    }

    bool success = true;
    int cur = 0;

    while (true) {
        WaitForSingleObject(ctx.hFree[cur], INFINITE);
        if (ctx.error) { success = false; break; }

        DWORD bytesRead;
        if (!ReadFile(hSrc, buffers[cur], COPY_BUF_SIZE, &bytesRead, nullptr)) {
            success = false;
            break;
        }
        if (bytesRead == 0) break;

        CryptHashData(hHash, reinterpret_cast<BYTE*>(buffers[cur]), bytesRead, 0);
        ctx.sizes[cur] = bytesRead;
        ReleaseSemaphore(ctx.hReady, 1, nullptr);

        cur ^= 1;

        if (bytesRead < COPY_BUF_SIZE) break;
    }

    if (success) {
        WaitForSingleObject(ctx.hFree[cur], INFINITE);
        if (ctx.error) { success = false; }
    }

    if (!success || ctx.error) {
        ctx.sizes[0] = 0;
        ctx.sizes[1] = 0;
        ReleaseSemaphore(ctx.hReady, 1, nullptr);
    } else {
        ctx.sizes[cur] = 0;
        ReleaseSemaphore(ctx.hReady, 1, nullptr);
    }

    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    CloseHandle(ctx.hReady);
    CloseHandle(ctx.hFree[0]);
    CloseHandle(ctx.hFree[1]);

    VirtualFree(buffers[0], 0, MEM_RELEASE);
    VirtualFree(buffers[1], 0, MEM_RELEASE);

    std::string hash;
    if (success) {
        FlushFileBuffers(hDst);
        hash = hashToString(hHash);
        FILETIME ftCreate, ftAccess, ftWrite;
        if (GetFileTime(hSrc, &ftCreate, &ftAccess, &ftWrite))
            SetFileTime(hDst, &ftCreate, &ftAccess, &ftWrite);
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    CloseHandle(hDst);
    CloseHandle(hSrc);

    if (!success) {
        DeleteFileA(actualDest.c_str());
        return {false, "", actualDest, false};
    }

    return {true, hash, actualDest, false};
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
