#include "Mailer.h"
#include <sstream>
#include <cstring>
#include <vector>
#include <chrono>
#include <random>

#pragma comment(lib, "ws2_32.lib")

Mailer::~Mailer() {
    disconnect();
}

bool Mailer::connect(const std::string& server, int port) {
    m_server = server;
    m_port = port;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }

    struct addrinfo hints = {}, * addr = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string portStr = std::to_string(port);
    if (getaddrinfo(server.c_str(), portStr.c_str(), &hints, &addr) != 0 || addr == nullptr) {
        WSACleanup();
        return false;
    }

    m_socket = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
    if (m_socket == INVALID_SOCKET) {
        freeaddrinfo(addr);
        WSACleanup();
        return false;
    }

    int timeoutMs = 10000;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
    setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    if (::connect(m_socket, addr->ai_addr, static_cast<int>(addr->ai_addrlen)) == SOCKET_ERROR) {
        freeaddrinfo(addr);
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    freeaddrinfo(addr);

    std::string response;
    if (!readResponse(response)) {
        disconnect();
        return false;
    }

    return true;
}

void Mailer::disconnect() {
    if (m_socket != INVALID_SOCKET) {
        sendCommand("QUIT\r\n", "221");
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    WSACleanup();
}

bool Mailer::readResponse(std::string& response) {
    response.clear();
    char buffer[4096];
    int bytes;

    while (true) {
        bytes = recv(m_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) return false;

        buffer[bytes] = '\0';
        response += buffer;

        size_t len = response.size();
        if (len >= 5) {
            size_t end = len;
            if (end >= 2 && response[end - 2] == '\r' && response[end - 1] == '\n') {
                end -= 2;
            }
            size_t start = 0;
            if (end >= 2) {
                size_t prevCRLF = response.rfind("\r\n", end - 2);
                if (prevCRLF != std::string::npos) {
                    start = prevCRLF + 2;
                }
            }
            if (start + 3 <= end && response[start + 3] == ' ') {
                break;
            }
        }
    }

    return true;
}

bool Mailer::sendCommand(const std::string& cmd, const std::string& expectedCode, std::string* outResponse) {
    if (send(m_socket, cmd.c_str(), static_cast<int>(cmd.length()), 0) == SOCKET_ERROR) {
        return false;
    }

    std::string response;
    if (!readResponse(response)) return false;

    if (outResponse) *outResponse = response;

    if (!expectedCode.empty()) {
        if (response.size() < 3) return false;
        return response.substr(0, 3) == expectedCode;
    }

    return true;
}

std::string Mailer::base64Encode(const std::string& input) {
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    size_t i = 0;
    const unsigned char* data = reinterpret_cast<const unsigned char*>(input.c_str());
    size_t len = input.length();

    while (i < len) {
        unsigned char a = (i < len) ? data[i++] : 0;
        unsigned char b = (i < len) ? data[i++] : 0;
        unsigned char c = (i < len) ? data[i++] : 0;

        result += b64[a >> 2];
        result += b64[((a & 0x03) << 4) | (b >> 4)];
        result += (i - 1 < len) ? b64[((b & 0x0F) << 2) | (c >> 6)] : '=';
        result += (i - 2 < len) ? b64[c & 0x3F] : '=';
    }

    return result;
}

std::string Mailer::buildMessageId() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 999999);

    std::ostringstream ss;
    ss << "<SQLBackup." << ms << "." << dis(gen) << "@" << m_server << ">";
    return ss.str();
}

bool Mailer::sendMail(
    const std::string& senderName,
    const std::string& senderEmail,
    const std::vector<std::string>& recipients,
    const std::string& subject,
    const std::string& htmlBody,
    bool useAuth,
    const std::string& username,
    const std::string& password
) {
    if (m_socket == INVALID_SOCKET) return false;
    if (recipients.empty()) return false;

    if (!sendCommand("EHLO SQLBackup\r\n", "250")) {
        sendCommand("HELO SQLBackup\r\n", "250");
    }

    if (useAuth) {
        if (!sendCommand("AUTH LOGIN\r\n", "334")) return false;
        if (!sendCommand(base64Encode(username) + "\r\n", "334")) return false;
        if (!sendCommand(base64Encode(password) + "\r\n", "235")) return false;
    }

    std::string mailFrom = "MAIL FROM:<" + senderEmail + ">\r\n";
    if (!sendCommand(mailFrom, "250")) return false;

    for (const auto& rcpt : recipients) {
        std::string rcptCmd = "RCPT TO:<" + rcpt + ">\r\n";
        if (!sendCommand(rcptCmd, "250")) return false;
    }

    if (!sendCommand("DATA\r\n", "354")) return false;

    std::string messageId = buildMessageId();

    auto needsEncoding = [](const std::string& s) {
        for (unsigned char c : s)
            if (c > 127) return true;
        return false;
    };

    auto containsAt = [](const std::string& s) {
        return s.find('@') != std::string::npos;
    };

    std::ostringstream msg;
    msg << "From: ";
    if (senderName.empty() || containsAt(senderName) || senderName == senderEmail) {
        msg << "<" << senderEmail << ">";
    } else if (needsEncoding(senderName)) {
        msg << "=?utf-8?B?" << base64Encode(senderName) << "?= <" << senderEmail << ">";
    } else {
        msg << senderName << " <" << senderEmail << ">";
    }
    msg << "\r\n";
    msg << "To: ";
    for (size_t i = 0; i < recipients.size(); ++i) {
        if (i > 0) msg << ", ";
        msg << recipients[i];
    }
    msg << "\r\n";
    msg << "Subject: " << subject << "\r\n";
    msg << "Message-ID: " << messageId << "\r\n";
    msg << "MIME-Version: 1.0\r\n";
    msg << "Content-Type: text/html; charset=\"utf-8\"\r\n";
    msg << "Content-Transfer-Encoding: 8bit\r\n";
    msg << "\r\n";
    msg << htmlBody << "\r\n";
    msg << ".\r\n";

    if (!sendCommand(msg.str(), "250")) return false;

    return true;
}
