#ifndef PASSWORD_UTILS_HPP
#define PASSWORD_UTILS_HPP

#include <string>

namespace PasswordUtils
{

// scrypt 哈希密码 (N=16384 r=8 p=1)，格式：salt_hex$key_hex
std::string hashPassword(const std::string& password);

// 验证密码（兼容旧 SHA-256 格式）
bool verifyPassword(const std::string& password, const std::string& stored);

} // namespace PasswordUtils

#endif
