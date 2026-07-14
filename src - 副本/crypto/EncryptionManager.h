#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <memory>

namespace FERREX {

class DecryptedFileHandle {
public:
    DecryptedFileHandle(HANDLE hFile, const std::wstring& path) 
        : m_hFile(hFile), m_path(path) {}
    ~DecryptedFileHandle() {
        if (m_hFile != INVALID_HANDLE_VALUE) CloseHandle(m_hFile);
    }
    std::wstring path() const { return m_path; }
    bool isValid() const { return m_hFile != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_hFile;
    std::wstring m_path;
};

class EncryptionManager {
public:
    static EncryptionManager& instance();

    bool encryptFile(const std::wstring& srcPath, const std::wstring& destPath, const std::string& password);

    std::shared_ptr<DecryptedFileHandle> decryptToTemp(const std::wstring& amencPath, const std::string& password);

private:
    EncryptionManager();
    ~EncryptionManager();

    bool deriveKey(const std::string& password, const std::vector<BYTE>& salt, std::vector<BYTE>& key);
    std::vector<BYTE> generateRandom(size_t size);

    BCRYPT_ALG_HANDLE m_aesAlg = NULL;
};

} 
