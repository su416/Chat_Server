#ifndef PASSWORD_UTILS_HPP
#define PASSWORD_UTILS_HPP

#include <string>

namespace PasswordUtils
{

// 对密码做 SHA-256(明文 + 服务端盐值)，返回 64 位十六进制字符串
std::string hashPassword(const std::string& password);

// 验证密码：hashPassword(password) == storedHash
bool verifyPassword(const std::string& password, const std::string& hash);

} // namespace PasswordUtils

#endif
