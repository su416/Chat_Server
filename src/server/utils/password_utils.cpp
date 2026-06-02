#include "password_utils.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <sstream>
#include <iomanip>

namespace PasswordUtils
{

namespace
{

static std::string toHex(const unsigned char* data, size_t len)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i)
        oss << std::setw(2) << static_cast<int>(data[i]);
    return oss.str();
}

static int hexValue(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;  // 非法字符
}

static size_t fromHex(const std::string& hex, unsigned char* out, size_t maxLen)
{
    size_t n = 0;
    for (size_t i = 0; i + 1 < hex.size() && n < maxLen; i += 2, ++n)
    {
        int hi = hexValue(hex[i]);
        int lo = hexValue(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return 0;  // 非法字符 → 校验失败
        out[n] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return n;
}

constexpr uint64_t SCRYPT_N = 16384;
constexpr uint64_t SCRYPT_R = 8;
constexpr uint64_t SCRYPT_P = 1;
constexpr size_t   SALT_LEN = 16;
constexpr size_t   KEY_LEN  = 32;

} // anonymous namespace

std::string hashPassword(const std::string& password)
{
    unsigned char salt[SALT_LEN];
    RAND_bytes(salt, sizeof(salt));

    unsigned char key[KEY_LEN];
    EVP_PBE_scrypt(password.c_str(), password.size(),
                   salt, sizeof(salt),
                   SCRYPT_N, SCRYPT_R, SCRYPT_P,
                   0,
                   key, sizeof(key));

    return toHex(salt, SALT_LEN) + "$" + toHex(key, KEY_LEN);
}

bool verifyPassword(const std::string& password, const std::string& stored)
{
    size_t pos = stored.find('$');
    if (pos == std::string::npos) return false;

    unsigned char salt[SALT_LEN];
    fromHex(stored.substr(0, pos), salt, SALT_LEN);

    unsigned char expected[KEY_LEN];
    fromHex(stored.substr(pos + 1), expected, KEY_LEN);

    unsigned char actual[KEY_LEN];
    EVP_PBE_scrypt(password.c_str(), password.size(),
                   salt, sizeof(salt),
                   SCRYPT_N, SCRYPT_R, SCRYPT_P,
                   0,
                   actual, sizeof(actual));

    return CRYPTO_memcmp(actual, expected, KEY_LEN) == 0;
}

} // namespace PasswordUtils
