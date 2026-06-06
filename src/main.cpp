#include "Config.h"
#include "Logger.h"
#include "FileUtils.h"
#include "Mailer.h"
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <ctime>
#include <iomanip>

namespace fs = std::filesystem;

static std::string buildHtmlTemplate(
    const std::string& filename,
    const std::string& sourcePath,
    const std::string& destPath,
    uint64_t fileSize
) {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream dateStr;
    dateStr << std::put_time(&tm, "%d.%m.%Y %H:%M:%S");

    std::ostringstream sizeStr;
    if (fileSize > 1073741824) {
        sizeStr << std::fixed << std::setprecision(2) << (static_cast<double>(fileSize) / 1073741824.0) << " GB";
    } else if (fileSize > 1048576) {
        sizeStr << std::fixed << std::setprecision(2) << (static_cast<double>(fileSize) / 1048576.0) << " MB";
    } else if (fileSize > 1024) {
        sizeStr << std::fixed << std::setprecision(2) << (static_cast<double>(fileSize) / 1024.0) << " KB";
    } else {
        sizeStr << fileSize << " B";
    }

    std::string sourceUrl = "file:///" + sourcePath;
    std::string destUrl = "file:///" + destPath;
    std::replace(sourceUrl.begin(), sourceUrl.end(), '\\', '/');
    std::replace(destUrl.begin(), destUrl.end(), '\\', '/');

    std::ostringstream html;
    html << "<!DOCTYPE html>\n"
         << "<html>\n<head>\n"
         << "<meta charset=\"utf-8\">\n"
         << "<style>\n"
         << "body { font-family: 'Segoe UI', Arial, sans-serif; color: #333; padding: 20px; }\n"
         << "table { border-collapse: collapse; margin: 15px 0; }\n"
         << "td, th { padding: 8px 12px; border: 1px solid #ddd; text-align: left; }\n"
         << "th { background-color: #f5f5f5; font-weight: 600; }\n"
         << ".success { color: #28a745; font-weight: bold; }\n"
         << "a { color: #007bff; text-decoration: none; }\n"
         << "a:hover { text-decoration: underline; }\n"
         << "</style>\n</head>\n<body>\n"
         << "<h2>Backup Copy Report</h2>\n"
         << "<p class=\"success\">&#10004; Backup file successfully processed</p>\n"
         << "<table>\n"
         << "<tr><th>Parameter</th><th>Value</th></tr>\n"
         << "<tr><td>File Name</td><td><b>" << filename << "</b></td></tr>\n"
         << "<tr><td>File Size</td><td>" << sizeStr.str() << "</td></tr>\n"
         << "<tr><td>Source Path</td><td><a href=\"" << sourceUrl << "\">" << sourcePath << "</a></td></tr>\n"
         << "<tr><td>Destination Path</td><td><a href=\"" << destUrl << "\">" << destPath << "</a></td></tr>\n"
         << "<tr><td>Copy Date</td><td>" << dateStr.str() << "</td></tr>\n"
         << "</table>\n"
         << "<p>This is an automated message from SQL Backup Utility.</p>\n"
         << "</body>\n</html>";

    return html.str();
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

    if (config.sourcePath().empty()) {
        logger.error("SourcePath is not configured");
        return 1;
    }

    auto backupFile = FileUtils::findNewestFile(config.sourcePath(), config.fileExtension());
    if (!backupFile.has_value()) {
        logger.error("No backup files found in: " + config.sourcePath());
        return 1;
    }

    logger.info("Found backup file: " + backupFile->fileName);

    std::string newFileName = FileUtils::generateFileName(config.nameTemplate(), config.dateFormat());
    std::string ext = fs::path(backupFile->fileName).extension().string();
    newFileName += ext;

    logger.info("Renaming to: " + newFileName);

    auto renamedPath = FileUtils::renameFile(backupFile->fullPath, newFileName);
    if (!renamedPath.has_value()) {
        logger.error("Failed to rename file to: " + newFileName);
        return 1;
    }

    logger.info("File renamed to: " + renamedPath.value());

    std::string destPath = (fs::path(config.destPath()) / newFileName).string();
    logger.info("Copying to: " + destPath + " (with inline SHA-256)");

    auto copyResult = FileUtils::copyWithHash(renamedPath.value(), destPath);
    if (!copyResult.success) {
        logger.error("Failed to copy file to: " + destPath);
        return 1;
    }

    logger.info("File copied successfully. Verifying destination...");

    auto destHash = FileUtils::computeSha256(destPath);
    if (destHash != copyResult.sourceHash) {
        logger.error("Verification failed: SHA-256 mismatch");
        logger.error("  Source hash: " + copyResult.sourceHash);
        logger.error("  Dest   hash: " + destHash);
        return 1;
    }

    logger.info("Verification passed. Source SHA-256: " + copyResult.sourceHash);

    if (config.debug()) {
        logger.info("Debug mode: source file will NOT be deleted: " + renamedPath.value());
    } else {
        logger.info("Removing source file...");
        if (!FileUtils::deleteFile(renamedPath.value())) {
            logger.warn("Failed to remove source file: " + renamedPath.value());
        } else {
            logger.info("Source file removed: " + renamedPath.value());
        }
    }

    logger.cleanupOldLogs();

    auto recipients = config.recipients();
    if (!recipients.empty() && !config.mailServer().empty()) {
        logger.info("Sending email notification to " + std::to_string(recipients.size()) + " recipient(s)");

        std::string htmlBody = buildHtmlTemplate(
            newFileName,
            renamedPath.value(),
            destPath,
            backupFile->fileSize
        );

        Mailer mailer;
        if (!mailer.connect(config.mailServer(), config.mailPort())) {
            logger.error("Failed to connect to mail server: " + config.mailServer());
            return 1;
        }

        std::string subject = "Backup Copy: " + newFileName;
        if (!mailer.sendMail(
            config.senderName(),
            config.senderEmail(),
            recipients,
            subject,
            htmlBody,
            config.mailAuth(),
            config.mailUsername(),
            config.mailPassword()
        )) {
            logger.error("Failed to send email notification");
            mailer.disconnect();
            return 1;
        }

        mailer.disconnect();
        logger.info("Email notification sent successfully");
    } else {
        logger.info("Email notification skipped (no recipients or mail server configured)");
    }

    logger.info("=== SQLBackup completed successfully ===");
    return 0;
}
