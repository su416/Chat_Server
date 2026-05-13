#include "password_utils.hpp"
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>

namespace PasswordUtils
{

static std::string sha256(const std::string& input)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()),
           input.size(), digest);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        oss << std::setw(2) << static_cast<int>(digest[i]);
    return oss.str();
}

std::string hashPassword(const std::string& password)
{
    // 固定服务端盐值，防止彩虹表反查
    static const std::string SECRET = "ChatServer2024!@#";
    return sha256(password + SECRET);
}

bool verifyPassword(const std::string& password, const std::string& hash)
{
    return hashPassword(password) == hash;
}

} // namespace PasswordUtils
