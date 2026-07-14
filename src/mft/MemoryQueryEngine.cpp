#include "MemoryQueryEngine.h"
#include <QElapsedTimer>
#include <QReadLocker>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrent>
#include <thread>
#include <algorithm>
#include <numeric>
#include <mutex>
#include <windows.h>
#include <shlwapi.h>

namespace FERREX {

std::vector<uint64_t> MemoryQueryEngine::search(MftReader* reader, const QString& query, bool useRegex, bool caseSensitive, 
                                                const QStringList& extensionList, bool includeHidden, bool includeSystem,
                                                bool includeDollar) {
    if (!reader) return {};

    QElapsedTimer timer;
    timer.start();
    
    {
        QReadLocker lock(&reader->m_dataLock);
        if (!reader->m_isInitialized) return {};
    }

    bool hasQuery = !query.isEmpty();
    bool hasExt = !extensionList.isEmpty();
    
    QRegularExpression re;
    QByteArray queryUtf8;
    if (hasQuery) {
        if (useRegex) {
            re = QRegularExpression(query, caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
        } else {
            queryUtf8 = query.toUtf8();
        }
    }

    std::vector<QByteArray> processedExtBytes;
    if (hasExt) {
        for (const QString& ex : extensionList) {
            QString normalized = ex.toLower();
            if (normalized.startsWith('.')) normalized = normalized.mid(1);
            processedExtBytes.push_back(normalized.toUtf8());
        }
    }

    std::mutex mtx;
    std::vector<uint64_t> finalRes;
    finalRes.reserve(reader->m_frns.size() / 16);

    unsigned int nThreads = std::thread::hardware_concurrency();
    if (nThreads == 0) nThreads = 2; 
    const size_t idealGrain = (reader->m_frns.size() / (nThreads * 4));
    const size_t grainSize = (std::max)(static_cast<size_t>(10000), idealGrain); 

    if (hasQuery && !useRegex && !caseSensitive && !hasExt) {
        qInfo() << "[MftReader] 进入前缀搜索分支 (Fast Path)";
        QReadLocker lock(&reader->m_dataLock);
        
        auto it_start = std::lower_bound(reader->m_sorted_indices.begin(), reader->m_sorted_indices.end(), queryUtf8.constData(), 
            [reader](uint32_t idx, const char* q) {
                const char* name = reinterpret_cast<const char*>(reader->m_string_pool.data() + reader->m_name_offsets[idx]);
                return _strnicmp(name, q, strlen(q)) < 0;
            });
        
        size_t counter = 0;
        for (auto it = it_start; it != reader->m_sorted_indices.end(); ++it) {
            if ((counter++ & 4095) == 0 && reader->isSearchCanceled()) {
                qInfo() << "[MftReader] 前缀搜索分支检测到取消信号，提前终止";
                return {};
            }

            uint32_t i = *it;
            if (i >= reader->m_frns.size() || reader->m_frns[i] == 0) continue;
            
            const char* p = reinterpret_cast<const char*>(reader->m_string_pool.data() + reader->m_name_offsets[i]);
            if (_strnicmp(p, queryUtf8.constData(), queryUtf8.size()) != 0) break; 

            if (!includeDollar && p[0] == '$') continue;

            size_t dIdx = static_cast<size_t>(reader->m_parent_frns[i] >> 48);
            if (dIdx >= 32 || !(reader->m_drive_active_mask.load(std::memory_order_relaxed) & (1 << dIdx))) continue;
            
            uint32_t at = reader->m_attributes[i];
            if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) continue;

            finalRes.push_back(MftReader::makeKey(dIdx, reader->m_frns[i]));
            if (finalRes.size() > 200000) break; 
        }
        qInfo() << "[MftReader] 前缀搜索完成. 命中:" << finalRes.size() << "耗时:" << timer.elapsed() << "ms";
    } else {
        qInfo() << "[MftReader] 进入全量/并行搜索分支 (Parallel Path). Regex:" << useRegex << "HasExt:" << hasExt;
        size_t currentTotal = 0;
        { QReadLocker lock(&reader->m_dataLock); currentTotal = reader->m_frns.size(); }

        size_t numChunks = (currentTotal + grainSize - 1) / grainSize;
        std::vector<size_t> chunks(numChunks);
        std::iota(chunks.begin(), chunks.end(), 0);

        qInfo() << "[MftReader] 并行搜索启动. 总数据量:" << currentTotal << "分块数:" << numChunks << "线程数:" << nThreads;
        QtConcurrent::blockingMap(chunks.begin(), chunks.end(), [&](size_t chunkIdx) {
            // 分块执行开始前，首先检测外部取消信号
            if (reader->isSearchCanceled()) return; // 瞬间拦截，不参与任何耗时逻辑

            std::vector<uint64_t> localRes;
            size_t startPos = chunkIdx * grainSize;
            
            {
                QReadLocker lock(&reader->m_dataLock);
                size_t endPos = (std::min)(startPos + grainSize, reader->m_frns.size());

                for (size_t i = startPos; i < endPos; ++i) {
                    // 每执行一定步长检测一次，保证百万级扫描极速响应取消
                    if ((i & 4095) == 0 && reader->isSearchCanceled()) return;

                    if (reader->m_frns[i] == 0) continue;
                    
                    size_t dIdx = static_cast<size_t>(reader->m_parent_frns[i] >> 48);
                    if (dIdx >= 32 || !(reader->m_drive_active_mask.load(std::memory_order_relaxed) & (1 << dIdx))) continue;

                    uint32_t at = reader->m_attributes[i];
                    if (!includeHidden && (at & FILE_ATTRIBUTE_HIDDEN)) continue;
                    if (!includeSystem && (at & FILE_ATTRIBUTE_SYSTEM)) continue;

                    const char* p = reinterpret_cast<const char*>(reader->m_string_pool.data() + reader->m_name_offsets[i]);
                    if (!includeDollar && p[0] == '$') continue;

                    if (!hasQuery && !hasExt) {
                        localRes.push_back(MftReader::makeKey(dIdx, reader->m_frns[i]));
                        continue;
                    }

                    if (hasExt) {
                        bool extMatch = false;
                        const char* ext = reinterpret_cast<const char*>(reader->m_string_pool.data() + reader->m_ext_offsets[i]);
                        for (const auto& ex : processedExtBytes) {
                            if (_stricmp(ext, ex.constData()) == 0) {
                                extMatch = true; break;
                            }
                        }
                        if (!extMatch) continue;
                    }

                    if (!hasQuery) {
                        localRes.push_back(MftReader::makeKey(dIdx, reader->m_frns[i]));
                    } else {
                        bool match = false;
                        if (useRegex) match = re.match(QString::fromUtf8(p)).hasMatch();
                        else {
                            if (caseSensitive) match = (strstr(p, queryUtf8.constData()) != nullptr);
                            else match = (StrStrIA(p, queryUtf8.constData()) != nullptr);
                        }
                        if (match) localRes.push_back(MftReader::makeKey(dIdx, reader->m_frns[i]));
                    }
                }
            }
            if (!localRes.empty()) { std::lock_guard<std::mutex> l(mtx); finalRes.insert(finalRes.end(), localRes.begin(), localRes.end()); }
        });
    }
    return finalRes;
}

} // namespace FERREX
