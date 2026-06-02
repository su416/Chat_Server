                    // 测试模块：PasswordUtils（scrypt 密码哈希）
// 覆盖：哈希+验证、错误密码、篡改哈希、随机盐特性、哈希长度

#include <gtest/gtest.h>
#include "password_utils.hpp"

TEST(PasswordUtilsTest, HashAndVerifySuccess)
{
    std::string hash = PasswordUtils::hashPassword("mypassword");
    EXPECT_TRUE(PasswordUtils::verifyPassword("mypassword", hash));
}

TEST(PasswordUtilsTest, WrongPasswordFails)
{
    std::string hash = PasswordUtils::hashPassword("correct");
    EXPECT_FALSE(PasswordUtils::verifyPassword("wrong", hash));
}

TEST(PasswordUtilsTest, TamperedHashFails)
{
    std::string hash = PasswordUtils::hashPassword("secret");
    // 篡改哈希中间一个字符
    hash[10] = (hash[10] == 'a') ? 'b' : 'a';
      EXPECT_FALSE(PasswordUtils::verifyPassword("secret", hash));
}
  
TEST(PasswordUtilsTest, SamePasswordDifferentHashes)
{
    // scrypt 每次生成随机盐 → 相同密码产生不同哈希
    std::string h1 = PasswordUtils::hashPassword("same");
    std::string h2 = PasswordUtils::hashPassword("same");
    EXPECT_NE(h1, h2);

    // 但都能验证通过
    EXPECT_TRUE(PasswordUtils::verifyPassword("same", h1));
    EXPECT_TRUE(PasswordUtils::verifyPassword("same", h2));
}

TEST(PasswordUtilsTest, HashContainsSeparator)
{
    std::string hash = PasswordUtils::hashPassword("test");
    size_t pos = hash.find('$');
    EXPECT_NE(pos, std::string::npos);
    // 盐 32 hex + '$' + 密钥 64 hex
    EXPECT_EQ(pos, 32u);
    EXPECT_EQ(hash.size(), 32u + 1 + 64u);
}

TEST(PasswordUtilsTest, InvalidFormatReturnsFalse)
{
    EXPECT_FALSE(PasswordUtils::verifyPassword("x", "no_dollar_sign"));
    EXPECT_FALSE(PasswordUtils::verifyPassword("x", ""));
}

TEST(PasswordUtilsTest, CorruptedHexSaltRejected)
{
    // 把合法哈希的盐替换为非法 hex 字符 "gg"
    std::string hash = PasswordUtils::hashPassword("test");
    hash.replace(0, 2, "gg");  // 前 2 字节变成非法字符
    EXPECT_FALSE(PasswordUtils::verifyPassword("test", hash));
}

TEST(PasswordUtilsTest, CorruptedHexKeyRejected)
{
    // 把合法哈希的密钥替换为非法 hex 字符 "zz"
    std::string hash = PasswordUtils::hashPassword("test");
    size_t dollar = hash.find('$');
    hash.replace(dollar + 1, 2, "zz");
    EXPECT_FALSE(PasswordUtils::verifyPassword("test", hash));
}
