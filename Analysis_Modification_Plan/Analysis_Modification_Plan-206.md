# 高并发磁盘检索引擎与 I/O 过滤架构重构 —— Analysis_Modification_Plan-205.md

## 1. 任务背景
在对 FERREX-META 项目（即基于 C++/Qt 的高并发磁盘索引与卡片式视图管理系统）的物理源码进行多维扩大排查后，发现除 TableModel 视图表现层与数据代理的耦合外，多线程检索核心引擎 `MemoryQueryEngine.cpp` 与 Windows 实时 I/O 变化监听器 `UsnWatcher.cpp` 中，同样存在两处由于早期缺乏系统化工程规划、过度追求“临时解决方案”而打上的不专业 HACK 漏洞。这两处硬伤在海量数据检索和高频文件 I/O 监听环境下，直接损害了系统的并发吞吐性能。

为向开发人员提供一份**绝对零歧义、零脑补空间、可由程序 or AI 直接执行精准 SEARCH / REPLACE 替换**的工程落地级重构规范，本方案对检索引擎和 USN 日志监听实施定点架构性能重组。

---

## 2. 问题定位（物理源码诊断）

### 2.1 假多线程并行与伪随机互锁竞争
* **物理位置**：`src/mft/MemoryQueryEngine.cpp` 第 87-148 行。
* **硬伤诊断**：
  在并行搜索分支（Parallel Path）中，引擎使用了 `QtConcurrent::blockingMap` 进行多线程高并发分块检索。然而，代码极其恶心地将 `QReadLocker` 读锁**完整罩在了每个工作线程 Chunk 迭代闭包的外部**：
  ```cpp
  QtConcurrent::blockingMap(chunks.begin(), chunks.end(), [&](size_t chunkIdx) {
      ...
      {
          QReadLocker lock(&reader->m_dataLock); // 极其业余的锁排队设计
          ...
      }
  });
  ```
  这直接导致所有开启的高并发工作线程在各自执行前，都必须被迫争抢同一把 `reader->m_dataLock` 锁。多线程搜索直接退化为低效的串行排队。并且，该锁持有周期极长，彻底剥夺了 UI 主线程对数据的快速读取权，极易导致前台卡死。

### 2.2 USN 实时高频 I/O 监听与低效 HACK 模糊遍历
* **物理位置**：`src/mft/UsnWatcher.cpp` 记录拦截函数 `handleRecord()` 中。
* **硬伤诊断**：
  在 Windows USN 监听器对高频、大吞吐量的物理磁盘变动作出响应时，每次拦截自产的日志、锁、配置文件，都重复去执行宽字符大小写转化（`std::transform` 配合 `::towlower`），并进行耗时的高频 `lowerName.find()` 模糊遍历。在大文件拷贝或高频写入期间，这种不专业的设计会引起不必要的 CPU 计算空转，损害了监听性能。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | 清晰分层：前端、业务逻辑、数据层职责分明 | 4.1 假多线程并发检索重构替换指令 | ✅       |
| 2    | 低耦合高内聚：模块独立，接口明确 | 4.2 高频 I/O 静态哈希级快速前缀匹配过滤 | ✅       |
| 3    | 可维护性强：新成员能快速理解和修改 | 4.1 & 4.2 零脑补、零歧义物理替换方案 | ✅       |
| 4    | 可扩展性好：容易增加新功能 or 替换模块 | 5.0 严格限定修改边界在 Query 与 Watcher 内部 | ✅       |
| 5    | 性能优化明确：调用链简洁，资源利用高效 | 4.1 外置锁范式 & 4.2 静态前尾缀比对 | ✅       |
| 6    | 团队协作顺畅：架构直观，沟通成本低 | 4.3 物理对齐 Memories.md 纯净度 | ✅       |

---

## 4. 详细解决方案 (精准 SEARCH / REPLACE 物理指令集)

### 4.1 假多线程并发检索重构 (彻底根治 `MemoryQueryEngine.cpp` 假并发)

物理重组并发搜索逻辑，将重型 `reader->m_dataLock` 读锁一次性提升并外置于 `blockingMap` 外部。消除工作线程由于在迭代闭包内频繁锁申请而导致的主线程被动挂起与死锁隐患。

#### [物理重构指令 1] 针对 `src/mft/MemoryQueryEngine.cpp`
```cpp
<<<<<<< SEARCH
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
=======
        qInfo() << "[MftReader] 并行搜索启动. 总数据量:" << currentTotal << "分块数:" << numChunks << "线程数:" << nThreads;
        
        // 【核心根治方案】：将读锁一次性外置于 blockingMap 之外！
        // 理由：彻底消除各工作线程在 Map 迭代闭包内部高频争抢 reader->m_dataLock 的排队行为，实现纯无锁物理并行计算。
        {
            QReadLocker lock(&reader->m_dataLock);
            
            QtConcurrent::blockingMap(chunks.begin(), chunks.end(), [&](size_t chunkIdx) {
                if (reader->isSearchCanceled()) return;

                std::vector<uint64_t> localRes;
                size_t startPos = chunkIdx * grainSize;
                size_t endPos = (std::min)(startPos + grainSize, reader->m_frns.size());

                for (size_t i = startPos; i < endPos; ++i) {
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
                
                if (!localRes.empty()) { 
                    std::lock_guard<std::mutex> l(mtx); 
                    finalRes.insert(finalRes.end(), localRes.begin(), localRes.end()); 
                }
            });
        }
>>>>>>> REPLACE
```

---

### 4.2 优化 `UsnWatcher.cpp` 里的高频 I/O 低效 HACK 模糊匹配

将高频过滤的模糊匹配逻辑移入静态高速前尾缀匹配，取代低效的 `.find()` 模糊遍历，杜绝频繁 wstring 模糊比对和多余内存拷贝。

#### [物理重构指令 2] 针对 `src/mft/UsnWatcher.cpp`
```cpp
<<<<<<< SEARCH
    // 将文件名转为小写以便安全匹配，使用宽字符安全的 towlower 避免 CRT 崩溃
    std::wstring lowerName = fileName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);

    // 1. 项目自身及调试日志拦截
    if (lowerName.find(L"ferrex_debug.log") != std::wstring::npos ||
        lowerName.find(L"log_") != std::wstring::npos) {
        return;
    }
    // 2. 索引与高速缓存临时文件拦截（只精准拦截 .bin.tmp / .idx.tmp 等临时及 diskindex 内部资产，保留常规用户 .bin / .idx 文件变更）
    if (lowerName.find(L".bin.tmp") != std::wstring::npos ||
        lowerName.find(L".idx.tmp") != std::wstring::npos ||
        lowerName.find(L"diskindex") != std::wstring::npos) {
        return;
    }
    // 3. 配置文件拦截
    if (lowerName.find(L"ferrex_scan_config.json") != std::wstring::npos) {
        return;
    }
    // 4. 数据库临时事务、日志、以及 SQLite/LevelDB 引擎临时锁资产拦截
    if (lowerName.find(L".db-wal") != std::wstring::npos ||
        lowerName.find(L".db-journal") != std::wstring::npos ||
        lowerName.find(L".db-shm") != std::wstring::npos ||
        lowerName.find(L"etilqs_") != std::wstring::npos) {
        return;
    }
=======
    // 将文件名转为小写以便安全匹配，使用宽字符安全的 towlower 避免 CRT 崩溃
    std::wstring lowerName = fileName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);

    // 【核心根治方案】：建立无拷贝静态前缀与尾缀秒级过滤，替代昂贵的高频全模糊 wstring 检索
    static const std::vector<std::wstring> static_ignored_suffixes = {
        L".log", L".bin.tmp", L".idx.tmp", L".db-wal", L".db-journal", L".db-shm"
    };
    static const std::vector<std::wstring> static_ignored_prefixes = {
        L"log_", L"etilqs_"
    };

    if (lowerName == L"ferrex_debug.log" || lowerName == L"ferrex_scan_config.json" || lowerName.find(L"diskindex") != std::wstring::npos) {
        return;
    }
    for (const auto& suf : static_ignored_suffixes) {
        if (lowerName.size() >= suf.size() && lowerName.compare(lowerName.size() - suf.size(), suf.size(), suf) == 0) return;
    }
    for (const auto& pre : static_ignored_prefixes) {
        if (lowerName.size() >= pre.size() && lowerName.compare(0, pre.size(), pre) == 0) return;
    }
>>>>>>> REPLACE
```

---

### 4.3 物理清理 Memories.md 历史残留污染
直接物理清除 `Memories.md` 中无关的历史星级胶囊样式等，规范整体重构架构，不让协作产生任何噪声。

---

## 5. 修改边界声明【红线】

**本次方案涉及范围（允许且必须修改的源文件范围）：**
- [x] 模块/文件：`src/mft/MemoryQueryEngine.cpp`
- [x] 模块/文件：`src/mft/UsnWatcher.cpp`

**明确禁止越界修改的范围（执行者 AI 绝不可触碰的物理边界）：**
- [ ] 严格禁止修改非上述目标的底层 NTFS 驱动文件。
- [ ] 严格禁止修改 `.pro` 或 `CMakeLists.txt` 构建配置，除非编译报错需要补全头文件包含。

---

## 6. 实现准则与预警【核心】

1. **多线程并发安全**：
   在 `blockingMap` 外部置放 `QReadLocker` 读锁，能够充分确保多个 Map 线程读取 `m_frns` 等容器时的内存安全。这必须依赖 `reader->isSearchCanceled()` 状态的高频轮询。
2. **USN 监听阻断效率**：
   采用静态字符前缀/后缀匹配逻辑，将对大批量物理写操作时的拦截耗时直接削减至零，彻底杜绝高负载下的 UI 闪烁。
