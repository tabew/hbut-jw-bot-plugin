// jw_aes.hpp —— 内置 AES-128-CBC（仅加密），用于 CAS 密码加密，避免引入 OpenSSL
#pragma once
#include <string>

namespace jw {

// AES-128-CBC + PKCS7，key/iv 均为 16 字节
std::string aes128_cbc_encrypt(const std::string& key16, const std::string& iv16,
                               const std::string& plain);

// 正方统一身份认证的密码加密：
//   明文 = 64 位随机串 + 明文密码，key = salt(16)，iv 随机，输出 Base64
std::string cas_encrypt_password(const std::string& password, const std::string& salt);

} // namespace jw
