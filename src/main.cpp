#include "Config.h"
#include "Logger.h"
#include "FileUtils.h"
#include "Mailer.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <chrono>

namespace fs = std::filesystem;

static void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.length(), to);
        pos += to.length();
    }
}

static std::string formatDuration(double seconds) {
    std::ostringstream ss;
    if (seconds < 1.0)
        ss << std::fixed << std::setprecision(1) << (seconds * 1000.0) << " мс";
    else if (seconds < 60.0)
        ss << std::fixed << std::setprecision(1) << seconds << " сек";
    else {
        int mins = static_cast<int>(seconds) / 60;
        int secs = static_cast<int>(seconds) % 60;
        ss << mins << " мин " << secs << " сек";
    }
    return ss.str();
}

static std::string formatFileSize(uint64_t fileSize) {
    std::ostringstream ss;
    if (fileSize > 1073741824)
        ss << std::fixed << std::setprecision(2) << (static_cast<double>(fileSize) / 1073741824.0) << " ГБ";
    else if (fileSize > 1048576)
        ss << std::fixed << std::setprecision(2) << (static_cast<double>(fileSize) / 1048576.0) << " МБ";
    else if (fileSize > 1024)
        ss << std::fixed << std::setprecision(2) << (static_cast<double>(fileSize) / 1024.0) << " КБ";
    else
        ss << fileSize << " Б";
    return ss.str();
}

static std::string readAndFillTemplate(
    const std::string& templatePath,
    const std::string& filename,
    const std::string& sourcePath,
    const std::string& destPath,
    uint64_t fileSize,
    const std::string& label,
    double copyDurationSec,
    const std::string& statusJob,
    const std::string& serverName,
    const std::string& errorTable = ""
) {
    std::ifstream file(templatePath);
    if (!file.is_open()) return {};

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream dateStr;
    dateStr << std::put_time(&tm, "%d.%m.%Y %H:%M:%S");

    std::string sourceUrl = "file:///" + sourcePath;
    std::string destUrl = "file:///" + destPath;
    std::replace(sourceUrl.begin(), sourceUrl.end(), '\\', '/');
    std::replace(destUrl.begin(), destUrl.end(), '\\', '/');

    replaceAll(content, "{LABEL}", label);
    replaceAll(content, "{FILENAME}", filename);
    replaceAll(content, "{FILESIZE}", formatFileSize(fileSize));
    replaceAll(content, "{SOURCEPATH}", sourcePath);
    replaceAll(content, "{DESTPATH}", destPath);
    replaceAll(content, "{SOURCEURL}", sourceUrl);
    replaceAll(content, "{DESTURL}", destUrl);
    replaceAll(content, "{COPYDATE}", dateStr.str());
    replaceAll(content, "{COPYDURATION}", formatDuration(copyDurationSec));
    replaceAll(content, "{STATUSJOB}", statusJob);
    replaceAll(content, "{STATUSCLASS}", statusJob == "Ошибка" ? "error" : "success");
    replaceAll(content, "{SERVERNAME}", serverName);
    replaceAll(content, "{ERRORTABLE}", errorTable);

    return content;
}

static std::string getExecutableDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    fs::path p(path);
    return p.parent_path().string();
}

static void printUsage(const char* progName) {
    std::cerr << "Usage: " << progName << " [-c <config_path>]" << std::endl;
    std::cerr << "  -c, --config <path>  Path to configuration file (default: config.ini)" << std::endl;
    std::cerr << "  --help               Show this help message" << std::endl;
}

int main(int argc, char* argv[]) {
    std::string configPath;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                configPath = argv[++i];
            } else {
                std::cerr << "Error: --config requires a path argument" << std::endl;
                return 1;
            }
        } else if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    if (configPath.empty()) {
        configPath = (fs::path(getExecutableDir()) / "config.ini").string();
    }

    Config config;
    if (!config.load(configPath)) {
        std::cerr << "Failed to load config file: " << configPath << std::endl;
        return 1;
    }

    Logger logger;
    if (!logger.init(config.logPath(), config.maxLogs())) {
        std::cerr << "Failed to initialize logger at: " << config.logPath() << std::endl;
        return 1;
    }

    logger.info("Configuration loaded from: " + configPath);

    auto sections = config.backupSections();
    if (sections.empty()) sections.push_back("");

    int successCount = 0;
    int failCount = 0;

    for (const auto& section : sections) {
        std::string label = section.empty() ? "General" : section;
        logger.info("--- Processing section: " + label + " ---");

        bool sectionOk = true;
        std::string errorMessage;
        std::string newFileName;
        std::string renamedPath;
        std::string actualDest;
        std::string destPath;
        uint64_t fileSize = 0;
        double copyDurationSec = 0;

        std::string srcPath = config.sourcePath(section);
        if (srcPath.empty()) {
            errorMessage = "Не указан путь к исходной папке (SourcePath)";
            logger.error(errorMessage);
            sectionOk = false;
        }

        std::optional<BackupFile> backupFile;
        if (sectionOk) {
            backupFile = FileUtils::findNewestFile(srcPath, config.fileExtension(section));
            if (!backupFile.has_value()) {
                errorMessage = "Не найден файл резервной копии в папке: " + srcPath;
                logger.error(errorMessage);
                sectionOk = false;
            }
        }

        if (sectionOk) {
            logger.info("Found backup file: " + backupFile->fileName);
            fileSize = backupFile->fileSize;

            newFileName = FileUtils::generateFileName(config.nameTemplate(section), config.dateFormat(section));
            newFileName += fs::path(backupFile->fileName).extension().string();

            logger.info("Renaming to: " + newFileName);

            auto renamed = FileUtils::renameFile(backupFile->fullPath, newFileName);
            if (!renamed.has_value()) {
                errorMessage = "Не удалось переименовать файл — файл с именем \"" + newFileName + "\" уже существует";
                logger.error(errorMessage);
                sectionOk = false;
            } else {
                renamedPath = renamed.value();
                logger.info("File renamed to: " + renamedPath);
            }
        }

        if (sectionOk) {
            destPath = (fs::path(config.destPath(section)) / newFileName).string();
            auto onExists = static_cast<OnExists>(config.onExists());
            logger.info("Copying to: " + destPath + " (with inline SHA-256)");

            auto copyStart = std::chrono::steady_clock::now();
            auto copyResult = FileUtils::copyWithHash(renamedPath, destPath, onExists);

            if (copyResult.skipped) {
                logger.warn("Destination file already exists, skipped for: " + copyResult.destPath);
                actualDest = copyResult.destPath;
            } else if (!copyResult.success) {
                errorMessage = "Ошибка копирования — не удалось записать файл назначения (проверьте место на диске и права доступа)";
                logger.error("Failed to copy file to: " + copyResult.destPath);
                sectionOk = false;
            } else {
                actualDest = copyResult.destPath;
                logger.info("File copied successfully. Verifying destination...");

                auto destHash = FileUtils::computeSha256(actualDest);
                if (destHash != copyResult.sourceHash) {
                    errorMessage = "Ошибка верификации — контрольная сумма SHA-256 не совпадает, файл назначения повреждён";
                    logger.error("Verification failed: SHA-256 mismatch");
                    logger.error("  Source hash: " + copyResult.sourceHash);
                    logger.error("  Dest   hash: " + destHash);
                    sectionOk = false;
                } else {
                    copyDurationSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - copyStart).count();
                    logger.info("Verification passed. Source SHA-256: " + copyResult.sourceHash);

                    if (config.debug()) {
                        logger.info("Debug mode: source file will NOT be deleted: " + renamedPath);
                    } else {
                        logger.info("Removing source file...");
                        if (!FileUtils::deleteFile(renamedPath)) {
                            logger.warn("Failed to remove source file: " + renamedPath);
                        } else {
                            logger.info("Source file removed: " + renamedPath);
                        }
                    }
                }
            }
        }

        std::string sourceForMail = renamedPath.empty() ? (backupFile ? backupFile->fullPath : "") : renamedPath;
        std::string statusJob = sectionOk ? "Успешно" : "Ошибка";

        auto recipients = config.recipients();
        if (!recipients.empty() && !config.mailServer().empty()) {
            logger.info("Sending email notification to " + std::to_string(recipients.size()) + " recipient(s)");

            std::string tmplPath = config.mailTemplatePath();
            if (!fs::path(tmplPath).is_absolute())
                tmplPath = (fs::path(getExecutableDir()) / tmplPath).string();

            std::string errorTable;
            if (!errorMessage.empty()) {
                errorTable = "<table class=\"err-box\" cellpadding=\"0\" cellspacing=\"0\"><tr><td>"
                    + errorMessage + "</td></tr></table>";
            }
            std::string htmlBody = readAndFillTemplate(
                tmplPath, newFileName, sourceForMail,
                actualDest.empty() ? destPath : actualDest,
                fileSize, label, copyDurationSec, statusJob, config.serverName(), errorTable
            );

            Mailer mailer;
            bool mailOk = true;
            if (htmlBody.empty()) {
                logger.error("Failed to read mail template: " + tmplPath);
                mailOk = false;
            } else if (!mailer.connect(config.mailServer(), config.mailPort())) {
                logger.error("Failed to connect to mail server: " + config.mailServer());
                mailOk = false;
            } else {
                std::string subject = config.mailSubject();
                replaceAll(subject, "{LABEL}", label);
                replaceAll(subject, "{FILENAME}", newFileName);
                replaceAll(subject, "{FILESIZE}", formatFileSize(fileSize));
                replaceAll(subject, "{COPYDATE}", []() {
                    auto now = std::time(nullptr);
                    auto tm = *std::localtime(&now);
                    std::ostringstream ss;
                    ss << std::put_time(&tm, "%d.%m.%Y %H:%M:%S");
                    return ss.str();
                }());
                replaceAll(subject, "{COPYDURATION}", formatDuration(copyDurationSec));
                replaceAll(subject, "{STATUSJOB}", statusJob);
                replaceAll(subject, "{SERVERNAME}", config.serverName());
                replaceAll(subject, "{ERRORMESSAGE}", errorMessage);

                if (!mailer.sendMail(
                    config.senderName(), config.senderEmail(), recipients,
                    subject, htmlBody, config.mailAuth(),
                    config.mailUsername(), config.mailPassword()
                )) {
                    logger.error("Failed to send email notification");
                    mailOk = false;
                }
                mailer.disconnect();
            }

            if (mailOk)
                logger.info("Email notification sent successfully");
        } else {
            logger.info("Email notification skipped (no recipients or mail server configured)");
        }

        if (sectionOk) successCount++;
        else failCount++;
    }

    logger.cleanupOldLogs();

    logger.info("=== SQLBackup completed: " + std::to_string(successCount) + " succeeded, " +
                 std::to_string(failCount) + " failed ===");

    if (successCount == 0) {
        logger.error("No backups were processed successfully");
        return 1;
    }

    return failCount > 0 ? 1 : 0;
}
