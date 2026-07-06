#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ScchCache.h"
#include <windows.h>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <iostream>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace FERREX-META {

static const uint32_t CRC_POLY = 0xEDB88320;
static uint32_t crc_table[256];
static bool crc_table_initialized = false;

static void init_crc_table() {
    if (crc_table_initialized) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1) c = CRC_POLY ^ (c >> 1);
            else c >>= 1;
        }
        crc_table[i] = c;
    }
    crc_table_initialized = true;
}

uint32_t ScchCache::computeCrc32(const uint8_t* data, size_t len) {
    init_crc_table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

bool ScchCache::appendBatch(const std::string& binPath, const std::string& idxPath, 
                            uint64_t volumeSerial, uint64_t nextUsn, const std::vector<Record>& records) {
    if (records.empty()) {
        std::fstream idxFile(idxPath, std::ios::binary | std::ios::in | std::ios::out);
        if (idxFile) {
            IdxHeader h;
            idxFile.read(reinterpret_cast<char*>(&h), sizeof(IdxHeader));
            h.next_usn = nextUsn;
            idxFile.seekp(0);
            idxFile.write(reinterpret_cast<const char*>(&h), sizeof(IdxHeader));
        }
        return true;
    }

    std::ofstream binFile(binPath, std::ios::binary | std::ios::app | std::ios::ate);
    if (!binFile.is_open()) {
        binFile.open(binPath, std::ios::binary);
        if (!binFile.is_open()) return false;
        BinHeader h; h.magic = BIN_MAGIC_VAL; h.version = STORAGE_VERSION; h.volume_serial = volumeSerial; h.record_count = 0;
        binFile.write(reinterpret_cast<const char*>(&h), sizeof(BinHeader));
    }

    std::vector<ScchIndexEntry> newEntries;
    newEntries.reserve(records.size());

    for (const auto& r : records) {
        uint64_t pos = static_cast<uint64_t>(binFile.tellp());
        newEntries.push_back({ r.frn, pos });

        uint16_t nl = static_cast<uint16_t>(r.name.size());
        std::vector<uint8_t> buf; buf.reserve(8+8+2+nl+4+8);
        auto p = [&](const void* d, size_t l) { buf.insert(buf.end(), (const uint8_t*)d, (const uint8_t*)d + l); };
        p(&r.frn, 8); p(&r.parentFrn, 8); p(&nl, 2); p(r.name.data(), nl); p(&r.attributes, 4); p(&r.timestamp, 8);
        uint32_t crc = computeCrc32(buf.data(), buf.size());

        binFile.write(reinterpret_cast<const char*>(&r.frn), 8);
        binFile.write(reinterpret_cast<const char*>(&r.parentFrn), 8);
        binFile.write(reinterpret_cast<const char*>(&nl), 2);
        binFile.write(r.name.data(), nl);
        binFile.write(reinterpret_cast<const char*>(&r.attributes), 4);
        binFile.write(reinterpret_cast<const char*>(&r.timestamp), 8);
        binFile.write(reinterpret_cast<const char*>(&crc), 4);
    }
    binFile.flush(); binFile.close();

    {
        std::fstream f(binPath, std::ios::binary | std::ios::in | std::ios::out);
        if (f) {
            BinHeader h; f.read(reinterpret_cast<char*>(&h), sizeof(BinHeader));
            h.record_count += records.size(); f.seekp(0); f.write(reinterpret_cast<const char*>(&h), sizeof(BinHeader));
        }
    }

    std::fstream idxFile(idxPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!idxFile.is_open()) {
        idxFile.open(idxPath, std::ios::binary | std::ios::out);
        IdxHeader h = { IDX_MAGIC_VAL, STORAGE_VERSION, volumeSerial, nextUsn, 0, 0 };
        idxFile.write(reinterpret_cast<const char*>(&h), sizeof(IdxHeader));
        idxFile.close();
        idxFile.open(idxPath, std::ios::binary | std::ios::in | std::ios::out);
    }

    if (idxFile) {
        IdxHeader h; idxFile.seekg(0); idxFile.read(reinterpret_cast<char*>(&h), sizeof(IdxHeader));
        if (h.volume_serial == volumeSerial) {
            idxFile.seekp(0, std::ios::end);
            for (const auto& e : newEntries) idxFile.write(reinterpret_cast<const char*>(&e), sizeof(ScchIndexEntry));
            h.delta_count += records.size(); h.next_usn = nextUsn;
            idxFile.seekp(0); idxFile.write(reinterpret_cast<const char*>(&h), sizeof(IdxHeader));
        }
        idxFile.close();
    }

    {
        std::ifstream f(idxPath, std::ios::binary);
        if (f) {
            IdxHeader h; f.read(reinterpret_cast<char*>(&h), sizeof(IdxHeader));
            if (h.delta_count > 5000) performCompaction(binPath, idxPath, volumeSerial, nextUsn);
        }
    }
    return true;
}

bool ScchCache::loadIndex(const std::string& idxPath, uint64_t volumeSerial, uint64_t& nextUsn,
                         std::vector<ScchIndexEntry>& mainIndex, std::vector<ScchIndexEntry>& deltaLayer) {
    std::ifstream f(idxPath, std::ios::binary);
    if (!f) return false;
    IdxHeader h; f.read(reinterpret_cast<char*>(&h), sizeof(IdxHeader));
    if (h.magic != IDX_MAGIC_VAL || h.volume_serial != volumeSerial) return false;
    nextUsn = h.next_usn;
    mainIndex.resize(static_cast<size_t>(h.main_count));
    f.read(reinterpret_cast<char*>(mainIndex.data()), h.main_count * sizeof(ScchIndexEntry));
    deltaLayer.resize(static_cast<size_t>(h.delta_count));
    f.read(reinterpret_cast<char*>(deltaLayer.data()), h.delta_count * sizeof(ScchIndexEntry));
    return true;
}

bool ScchCache::readRecords(const std::string& binPath, const std::vector<ScchIndexEntry>& entries, std::vector<Record>& records) {
    std::ifstream f(binPath, std::ios::binary);
    if (!f) return false;
    records.reserve(records.size() + entries.size());
    for (const auto& ie : entries) {
        f.seekg(static_cast<std::streamoff>(ie.offset));
        Record r; uint16_t nl;
        f.read(reinterpret_cast<char*>(&r.frn), 8); f.read(reinterpret_cast<char*>(&r.parentFrn), 8);
        f.read(reinterpret_cast<char*>(&nl), 2); r.name.resize(nl); f.read(&r.name[0], nl);
        f.read(reinterpret_cast<char*>(&r.attributes), 4); f.read(reinterpret_cast<char*>(&r.timestamp), 8);
        records.push_back(std::move(r));
    }
    return true;
}

bool ScchCache::rebuildIndexFromBin(const std::string& binPath, uint64_t volumeSerial,
                                    std::vector<ScchIndexEntry>& mainIndex) {
    std::ifstream f(binPath, std::ios::binary);
    if (!f) return false;
    BinHeader h; f.read(reinterpret_cast<char*>(&h), sizeof(BinHeader));
    if (h.magic != BIN_MAGIC_VAL || h.volume_serial != volumeSerial) return false;
    mainIndex.reserve(static_cast<size_t>(h.record_count));
    for (uint64_t i = 0; i < h.record_count; ++i) {
        uint64_t off = static_cast<uint64_t>(f.tellg());
        uint64_t frn; uint16_t nl;
        f.read(reinterpret_cast<char*>(&frn), 8); f.seekg(8, std::ios::cur);
        f.read(reinterpret_cast<char*>(&nl), 2); f.seekg(nl + 4 + 8 + 4, std::ios::cur);
        mainIndex.push_back({ frn, off });
    }
    std::sort(mainIndex.begin(), mainIndex.end(), [](const ScchIndexEntry& a, const ScchIndexEntry& b) { return a.frn < b.frn; });
    return true;
}

bool ScchCache::performCompaction(const std::string& binPath, const std::string& idxPath, 
                                  uint64_t volumeSerial, uint64_t nextUsn) {
    std::vector<ScchIndexEntry> main, delta; uint64_t u;
    if (!loadIndex(idxPath, volumeSerial, u, main, delta)) {
        if (!rebuildIndexFromBin(binPath, volumeSerial, main)) return false;
    }
    std::unordered_map<uint64_t, uint64_t> m;
    for (const auto& e : main) m[e.frn] = e.offset;
    for (const auto& e : delta) m[e.frn] = e.offset;
    std::vector<ScchIndexEntry> newMain; newMain.reserve(m.size());
    for (const auto& pair : m) newMain.push_back({ pair.first, pair.second });
    std::sort(newMain.begin(), newMain.end(), [](const ScchIndexEntry& a, const ScchIndexEntry& b) { return a.frn < b.frn; });
    
    std::string tmpIdx = idxPath + ".tmp";
    {
        std::ofstream f(tmpIdx, std::ios::binary);
        IdxHeader h = { IDX_MAGIC_VAL, STORAGE_VERSION, volumeSerial, nextUsn, static_cast<uint64_t>(newMain.size()), 0 };
        f.write(reinterpret_cast<const char*>(&h), sizeof(IdxHeader));
        f.write(reinterpret_cast<const char*>(newMain.data()), newMain.size() * sizeof(ScchIndexEntry));
    }
    DeleteFileA(idxPath.c_str()); std::rename(tmpIdx.c_str(), idxPath.c_str());
    return true;
}

} // namespace FERREX-META
