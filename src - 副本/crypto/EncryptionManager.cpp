#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "EncryptionManager.h"
#include <windows.h>
#include <bcrypt.h>
#include <vector>
#include <fstream>
#include <filesystem>
#include <QString>

#pragma comment(lib, "bcrypt.lib")

namespace FERREX {

EncryptionManager& EncryptionManager::instance() {
    static EncryptionManager inst;
    return inst;
}

EncryptionManager::EncryptionManager() {
    BCryptOpenAlgorithmProvider(&m_aesAlg, BCRYPT_AES_ALGORITHM, NULL, 0);
    BCryptSetProperty(m_aesAlg, BCRYPT_CHAINING_MODE, (PBYTE)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
}

EncryptionManager::~EncryptionManager() {
    if (m_aesAlg) BCryptCloseAlgorithmProvider(m_aesAlg, 0);
}

bool EncryptionManager::deriveKey(const std::string& password, const std::vector<BYTE>& salt, std::vector<BYTE>& key) {
    BCRYPT_ALG_HANDLE hPbkdf2 = NULL;
    if (BCryptOpenAlgorithmProvider(&hPbkdf2, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return false;

    key.resize(32);
    NTSTATUS status = BCryptDeriveKeyPBKDF2(hPbkdf2, (PUCHAR)password.c_str(), (ULONG)password.length(),
                                           (PUCHAR)salt.data(), (ULONG)salt.size(), 10000, 
                                           key.data(), (ULONG)key.size(), 0);
    
    BCryptCloseAlgorithmProvider(hPbkdf2, 0);
    return status == 0;
}

std::vector<BYTE> EncryptionManager::generateRandom(size_t size) {
    std::vector<BYTE> buffer(size);
    BCryptGenRandom(NULL, buffer.data(), (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return buffer;
}

bool EncryptionManager::encryptFile(const std::wstring& srcPath, const std::wstring& destPath, const std::string& password) {
    std::ifstream is(QString::fromStdWString(srcPath).toStdString(), std::ios::binary);
    if (!is) return false;
    std::ofstream os(QString::fromStdWString(destPath).toStdString(), std::ios::binary);
    if (!is || !os) return false;

    std::vector<BYTE> salt = generateRandom(16);
    std::vector<BYTE> iv = generateRandom(16);
    std::vector<BYTE> key;
    if (!deriveKey(password, salt, key)) return false;

    os.write((char*)salt.data(), salt.size());
    os.write((char*)iv.data(), iv.size());

    BCRYPT_KEY_HANDLE hKey = NULL;
    BCryptGenerateSymmetricKey(m_aesAlg, &hKey, NULL, 0, key.data(), (ULONG)key.size(), 0);

    const size_t CHUNK_SIZE = 64 * 1024;
    std::vector<BYTE> buffer(CHUNK_SIZE);
    std::vector<BYTE> cipherBuffer(CHUNK_SIZE + 16); 

    while (is.read((char*)buffer.data(), CHUNK_SIZE) || is.gcount() > 0) {
        DWORD readBytes = (DWORD)is.gcount();
        DWORD cipherLen = 0;
        bool isLast = is.eof();

        BCryptEncrypt(hKey, buffer.data(), readBytes, NULL, iv.data(), (ULONG)iv.size(), 
                      cipherBuffer.data(), (ULONG)cipherBuffer.size(), &cipherLen, 
                      isLast ? BCRYPT_BLOCK_PADDING : 0);
        
        os.write((char*)cipherBuffer.data(), cipherLen);
    }

    BCryptDestroyKey(hKey);
    is.close();
    os.close();
    return true;
}

std::shared_ptr<DecryptedFileHandle> EncryptionManager::decryptToTemp(const std::wstring& amencPath, const std::string& password) {
    std::ifstream is(QString::fromStdWString(amencPath).toStdString(), std::ios::binary);
    if (!is) return nullptr;

    std::vector<BYTE> salt(16);
    std::vector<BYTE> iv(16);
    is.read((char*)salt.data(), 16);
    is.read((char*)iv.data(), 16);

    std::vector<BYTE> key;
    if (!deriveKey(password, salt, key)) return nullptr;

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring amTempDir = std::wstring(tempPath) + L"amtemp\\";
    CreateDirectoryW(amTempDir.c_str(), NULL);

    std::wstring outPath = amTempDir + std::filesystem::path(amencPath).stem().wstring();
    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    
    if (hFile == INVALID_HANDLE_VALUE) return nullptr;

    BCRYPT_KEY_HANDLE hKey = NULL;
    BCryptGenerateSymmetricKey(m_aesAlg, &hKey, NULL, 0, key.data(), (ULONG)key.size(), 0);

    const size_t CHUNK_SIZE = 64 * 1024;
    std::vector<BYTE> buffer(CHUNK_SIZE + 16);
    std::vector<BYTE> plainBuffer(CHUNK_SIZE + 16);

    while (is.read((char*)buffer.data(), CHUNK_SIZE) || is.gcount() > 0) {
        DWORD readBytes = (DWORD)is.gcount();
        DWORD plainLen = 0;
        bool isLast = is.eof();

        BCryptDecrypt(hKey, buffer.data(), readBytes, NULL, iv.data(), (ULONG)iv.size(),
                      plainBuffer.data(), (ULONG)plainBuffer.size(), &plainLen,
                      isLast ? BCRYPT_BLOCK_PADDING : 0);
        
        DWORD written = 0;
        WriteFile(hFile, plainBuffer.data(), plainLen, &written, NULL);
    }

    BCryptDestroyKey(hKey);
    is.close();

    SetFilePointer(hFile, 0, NULL, FILE_BEGIN);

    return std::make_shared<DecryptedFileHandle>(hFile, outPath);
}

} 
