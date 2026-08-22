// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 SwiftSQL Contributors

#include "core/Secret.h"

#include <wx/base64.h>
#include <wx/buffer.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>

#include <vector>

#if defined(__WXMSW__)
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#endif

namespace core {

#if defined(__WXMSW__)
namespace {

const BYTE kEntropy[] = {
    0x53, 0x77, 0x69, 0x66, 0x74, 0x53, 0x51, 0x4c,
    0x9a, 0x3f, 0xe1, 0x74, 0x2b, 0xc8, 0x05, 0xd6,
    0x1f, 0x40, 0x88, 0xa2, 0x6e, 0xb3, 0x77, 0x0c
};

constexpr const wchar_t* kAesPrefix = L"aesgcm:v1:";

DATA_BLOB EntropyBlob()
{
    return DATA_BLOB{ static_cast<DWORD>(sizeof kEntropy),
                      const_cast<BYTE*>(kEntropy) };
}

wxString UserDataDir()
{
    wxString dir = wxStandardPaths::Get().GetUserDataDir();
    if (!wxFileName::DirExists(dir))
        wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    return dir;
}

wxString MasterKeyFile()
{
    return UserDataDir() + wxFileName::GetPathSeparator() + L"secret.key";
}

wxString B64(const BYTE* data, size_t len)
{
    return len ? wxBase64Encode(data, len) : wxString();
}

std::vector<BYTE> Unb64(const wxString& token)
{
    wxMemoryBuffer b = wxBase64Decode(token);
    const BYTE* data = static_cast<const BYTE*>(b.GetData());
    return data ? std::vector<BYTE>(data, data + b.GetDataLen()) : std::vector<BYTE>();
}

bool ProtectBytes(const std::vector<BYTE>& plain, wxString& token)
{
    if (plain.empty()) return false;
    DATA_BLOB in{ static_cast<DWORD>(plain.size()),
                  const_cast<BYTE*>(plain.data()) };
    DATA_BLOB entropy = EntropyBlob();
    DATA_BLOB out{ 0, nullptr };
    if (!CryptProtectData(&in, L"SwiftSQL AES key", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    token = B64(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool UnprotectBytes(const wxString& token, std::vector<BYTE>& plain, bool legacyFallback)
{
    std::vector<BYTE> blob = Unb64(token);
    if (blob.empty()) return false;
    DATA_BLOB in{ static_cast<DWORD>(blob.size()), blob.data() };
    DATA_BLOB out{ 0, nullptr };
    DATA_BLOB entropy = EntropyBlob();

    bool ok = CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                                 CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok && legacyFallback)
        ok = CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                                CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) return false;

    plain.assign(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool RandomBytes(std::vector<BYTE>& out, size_t len)
{
    out.assign(len, 0);
    return BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::vector<BYTE> MasterKey()
{
    wxString protectedKey;
    const wxString path = MasterKeyFile();
    if (wxFileName::FileExists(path)) {
        wxFile f(path);
        f.ReadAll(&protectedKey, wxConvUTF8);
        protectedKey.Trim(true).Trim(false);
    }

    std::vector<BYTE> key;
    if (!protectedKey.IsEmpty() && UnprotectBytes(protectedKey, key, false) && key.size() == 32)
        return key;

    if (!RandomBytes(key, 32)) return {};
    if (!ProtectBytes(key, protectedKey)) {
        SecureZeroMemory(key.data(), key.size());
        return {};
    }

    wxFile f(path, wxFile::write);
    if (!f.IsOpened() || !f.Write(protectedKey, wxConvUTF8)) {
        SecureZeroMemory(key.data(), key.size());
        return {};
    }
    return key;
}

struct BCryptAlg {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~BCryptAlg() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};

struct BCryptKey {
    BCRYPT_KEY_HANDLE h = nullptr;
    ~BCryptKey() { if (h) BCryptDestroyKey(h); }
};

bool OpenAes(BCRYPT_ALG_HANDLE& alg, DWORD& objLen)
{
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return false;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0) != 0)
        return false;
    DWORD got = 0;
    return BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
                             sizeof(objLen), &got, 0) == 0;
}

bool AesGcmEncrypt(const std::vector<BYTE>& key, const BYTE* plain, ULONG plainLen,
                   std::vector<BYTE>& nonce, std::vector<BYTE>& tag,
                   std::vector<BYTE>& cipher)
{
    if (key.size() != 32) return false;
    if (!RandomBytes(nonce, 12)) return false;
    tag.assign(16, 0);
    cipher.assign(plainLen, 0);

    BCryptAlg alg;
    DWORD objLen = 0;
    if (!OpenAes(alg.h, objLen)) return false;
    std::vector<BYTE> obj(objLen);
    BCryptKey hkey;
    if (BCryptGenerateSymmetricKey(alg.h, &hkey.h, obj.data(), objLen,
                                   const_cast<BYTE*>(key.data()),
                                   static_cast<ULONG>(key.size()), 0) != 0)
        return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = nonce.data();
    auth.cbNonce = static_cast<ULONG>(nonce.size());
    auth.pbTag = tag.data();
    auth.cbTag = static_cast<ULONG>(tag.size());

    ULONG outLen = 0;
    return BCryptEncrypt(hkey.h, const_cast<BYTE*>(plain), plainLen, &auth, nullptr, 0,
                         cipher.data(), static_cast<ULONG>(cipher.size()), &outLen, 0) == 0 &&
           outLen == plainLen;
}

bool AesGcmDecrypt(const std::vector<BYTE>& key, const std::vector<BYTE>& nonce,
                   const std::vector<BYTE>& tag, const std::vector<BYTE>& cipher,
                   std::vector<BYTE>& plain)
{
    if (key.size() != 32 || nonce.empty() || tag.empty()) return false;
    plain.assign(cipher.size(), 0);

    BCryptAlg alg;
    DWORD objLen = 0;
    if (!OpenAes(alg.h, objLen)) return false;
    std::vector<BYTE> obj(objLen);
    BCryptKey hkey;
    if (BCryptGenerateSymmetricKey(alg.h, &hkey.h, obj.data(), objLen,
                                   const_cast<BYTE*>(key.data()),
                                   static_cast<ULONG>(key.size()), 0) != 0)
        return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce = const_cast<BYTE*>(nonce.data());
    auth.cbNonce = static_cast<ULONG>(nonce.size());
    auth.pbTag = const_cast<BYTE*>(tag.data());
    auth.cbTag = static_cast<ULONG>(tag.size());

    ULONG outLen = 0;
    return BCryptDecrypt(hkey.h, const_cast<BYTE*>(cipher.data()),
                         static_cast<ULONG>(cipher.size()), &auth, nullptr, 0,
                         plain.data(), static_cast<ULONG>(plain.size()), &outLen, 0) == 0 &&
           outLen == plain.size();
}

bool SplitAesToken(const wxString& token, wxString& nonce, wxString& tag, wxString& cipher)
{
    if (!token.StartsWith(kAesPrefix)) return false;
    wxString rest = token.Mid(wxString(kAesPrefix).length());
    const int a = rest.Find(':');
    if (a == wxNOT_FOUND) return false;
    nonce = rest.Left(a);
    rest = rest.Mid(a + 1);
    const int b = rest.Find(':');
    if (b == wxNOT_FOUND) return false;
    tag = rest.Left(b);
    cipher = rest.Mid(b + 1);
    return !nonce.IsEmpty() && !tag.IsEmpty();
}

wxString DecryptLegacyDpapi(const wxString& token)
{
    std::vector<BYTE> bytes;
    if (!UnprotectBytes(token, bytes, true)) return wxString();
    wxString plain = wxString::FromUTF8(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
    return plain;
}

} // namespace

bool SecretIsSecure() { return true; }

wxString EncryptSecret(const wxString& plain)
{
    if (plain.IsEmpty()) return wxString();

    const std::vector<BYTE> key = MasterKey();
    if (key.empty()) return wxString();

    const wxScopedCharBuffer utf8 = plain.utf8_str();
    std::vector<BYTE> nonce, tag, cipher;
    const bool ok = AesGcmEncrypt(key, reinterpret_cast<const BYTE*>(utf8.data()),
                                  static_cast<ULONG>(utf8.length()), nonce, tag, cipher);
    if (utf8.length())
        SecureZeroMemory(const_cast<char*>(utf8.data()), utf8.length());
    if (!ok) return wxString();

    return wxString(kAesPrefix) + B64(nonce.data(), nonce.size()) + L":" +
           B64(tag.data(), tag.size()) + L":" + B64(cipher.data(), cipher.size());
}

wxString DecryptSecret(const wxString& token)
{
    if (token.IsEmpty()) return wxString();

    wxString nonceText, tagText, cipherText;
    if (SplitAesToken(token, nonceText, tagText, cipherText)) {
        const std::vector<BYTE> key = MasterKey();
        std::vector<BYTE> plain;
        if (!key.empty() &&
            AesGcmDecrypt(key, Unb64(nonceText), Unb64(tagText), Unb64(cipherText), plain)) {
            wxString out = wxString::FromUTF8(reinterpret_cast<char*>(plain.data()), plain.size());
            if (!plain.empty()) SecureZeroMemory(plain.data(), plain.size());
            return out;
        }
        return wxString();
    }

    return DecryptLegacyDpapi(token);
}

#else  // non-Windows: base64 only until a platform keychain is wired up.

bool SecretIsSecure() { return false; }

wxString EncryptSecret(const wxString& plain)
{
    if (plain.IsEmpty()) return wxString();
    const wxScopedCharBuffer utf8 = plain.utf8_str();
    return L"b64:" + wxBase64Encode(utf8.data(), utf8.length());
}

wxString DecryptSecret(const wxString& token)
{
    if (!token.StartsWith(L"b64:")) return wxString();
    wxMemoryBuffer blob = wxBase64Decode(token.Mid(4));
    return wxString::FromUTF8(static_cast<char*>(blob.GetData()), blob.GetDataLen());
}

#endif

} // namespace core
