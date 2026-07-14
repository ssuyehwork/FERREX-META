# 磁盘驱动器左键取消激活后重启强制恢复缺陷分析 —— Analysis_Modification_Plan-197.md

## 1. 任务背景
在 FERREX-META 中，顶部盘符按钮区提供了多盘联动、快速切换磁盘筛选的交互控制功能。用户通过左键单击某个盘符（如“C: (系统)”），可以改变该盘符在 `m_config.activeDrives` 中的激活选中状态。

然而，目前系统被反馈存在一个严重的持久化控制缺陷：
* 当用户通过左键点击将 C 盘（或其它特定默认盘符）设为**取消激活**（即不再是橙色）后，在程序运行期虽然能够正常过滤。
* 但**只要重启主程序，C 盘就会强行触发激活并自动变成橙色**，即上一次取消激活的状态无法被持久化保留，每次重启都会无视配置被强行重置覆盖。

本方案将进行代码级的故障重构，并提供不带任何歧义、零空子的修改方案。

---

## 2. 问题定位

### 2.1 缺陷根因代码分析
该缺陷的全部逻辑直接存在于 `src/ui/ScanDialog.cpp` 的 `ScanDialog::initializeDrives` 函数中。
在异步获取系统可用驱动器列表后，UI 线程执行了以下初始化回调分支：

```cpp
QMetaObject::invokeMethod(weakThis.data(), [weakThis, drives]() {
    if (!weakThis) return;
    weakThis->m_cachedDriveInfos = drives;
    
    if (weakThis->m_config.activeDrives.isEmpty()) {
        for (const auto& info : drives) {
            if (info.hasMedia && info.isNtfs) {
                weakThis->m_config.activeDrives.insert(info.letter);
            }
        }
    } else {
        // ⚠️ 缺陷代码位置：硬编码强制重置覆盖
        if (!weakThis->m_config.activeDrives.contains("C:")) {
            for (const auto& info : drives) {
                if (info.letter == "C:") {
                    weakThis->m_config.activeDrives.insert("C:");
                    break;
                }
            }
        }
    }
    weakThis->m_config.save();
    ...
```

**代码执行机理拆解**：
1. **冷启动判断**：如果配置文件不存在，或者被清除，`activeDrives` 集合全空，执行 `if` 分支，自动将所有 NTFS 分区作为默认值激活。这符合冷启动初始设计。
2. **热重启霸道覆盖 (缺陷根源)**：如果配置文件已存在（即 `activeDrives` 非空，保存了用户之前的配置，例如只有 `G:`，而不含 `C:`），代码强制转入 `else` 分支。
3. **强制干涉检测**：在 `else` 中，硬编码逻辑强行判定：**如果 `activeDrives` 中没有 `"C:"`，就必须强制在 `drives` 中把 `"C:"` 塞进 `activeDrives` 中**。
4. **覆盖持久化**：由于强制加入了 `"C:"`，随后调用的 `weakThis->m_config.save()` 会立刻将这个被篡改的脏配置重写到硬盘 JSON 文件中。
5. **导致结果**：用户主动取消激活 C 盘的记录在每次重启程序时，都会在初始化阶段被无条件强行抹杀。

---

## 3. 强制对照表

| 编号 | 用户原话 / 我的理解 | 方案对应点 | 是否一致 |
|------|---------------------|------------|----------|
| 1    | C盘本来是未被单击左键激活的，但在重启主程序之后，却自动被触发激活状态 | 详见“2.1 缺陷根因代码分析”部分，准确定位了 `else` 下强插 `"C:"` 的漏洞 | ✅ |
| 2    | 只要重新启动主程序之后，C盘就会自动显示为橙色，完全没有被持久化 | 详见“4.2 解决方案实现步骤”部分，彻底拔除了越权恢复逻辑 | ✅ |
| 3    | 针对该“C 盘无法被正常“取消激活”持久化”问题，请给出相应的修改方案 | 详见“4.2 解决方案实现步骤”部分，提供 100% 杜绝脑补、无空子的代码修改方案 | ✅ |

---

## 4. 详细解决方案

为了保障持久化设置的 100% 忠实度与一致性，本方案将对 `ScanDialog::initializeDrives` 的数据加载分支进行物理净化。

### 4.1 逻辑重构方针
* **冷启动**：若 `activeDrives` 全空，保持原有默认全激活的兜底保障，确保首次打开即用（对应用户原话：“在没有历史配置时，默认将所有带介质的 NTFS 分区作为默认初始筛选”）。
* **热重启**：若 `activeDrives` 不为空，**100% 信任当前配置文件中的盘符集合**，不掺入任何外界越权的“强插 C 盘”逻辑，直接跳过处理，不执行 `else` 篡改动作。

### 4.2 解决方案实现步骤 (修改点精准对照)

在 `src/ui/ScanDialog.cpp` 中执行以下精准替换（删除霸道的 `else` 修复代码，不留任何歧义和腦补空子）：

#### 替换前的代码（100% 原版漏洞逻辑）：
```cpp
<<<<<<< SEARCH
            if (weakThis->m_config.activeDrives.isEmpty()) {
                for (const auto& info : drives) {
                    if (info.hasMedia && info.isNtfs) {
                        weakThis->m_config.activeDrives.insert(info.letter);
                    }
                }
            } else {
                if (!weakThis->m_config.activeDrives.contains("C:")) {
                    for (const auto& info : drives) {
                        if (info.letter == "C:") {
                            weakThis->m_config.activeDrives.insert("C:");
                            break;
                        }
                    }
                }
            }
            weakThis->m_config.save();
=======
            if (weakThis->m_config.activeDrives.isEmpty()) {
                for (const auto& info : drives) {
                    if (info.hasMedia && info.isNtfs) {
                        weakThis->m_config.activeDrives.insert(info.letter);
                    }
                }
                weakThis->m_config.save();
            }
>>>>>>> REPLACE
```

#### 修改效果物理判定：
* **冷启动状态下**：`activeDrives` 是空的，它会将当前电脑扫描到的所有有媒体且格式为 NTFS 的驱动器（当然包含C盘）加入其中，并写入本地配置文件保存，保证初次使用体验。
* **热重启状态下**：`activeDrives` 已经包含用户上一次保存过的任何盘符子集（例如只有 `"G:"`）。由于 `isEmpty()` 为 false，它将**直接跳过这整个判断段**，不进行任何修改、不调用多余的 `save()`，保证历史配置完好无损地恢复到界面。

该方案逻辑纯净、物理链路直接，100% 杜绝了 C 盘自动变橙、无法持久化的逻辑缺陷，不留任何歧义脑补空间。

---

## 5. 修改边界声明【红线】

**本次方案涉及范围：**
* [ ] 模块/文件：`src/ui/ScanDialog.cpp` 的 `ScanDialog::initializeDrives` 方法中的异步调用初始化模块。

**明确禁止越界修改的范围：**
* [ ] 严禁修改 `ConfigManager.cpp` 中磁盘序列化的实现。
* [ ] 严禁修改任何 `MftReader.cpp` 或 `ScanController.cpp` 等底层数据层检索逻辑。

---

## 6. 实现准则与预警【核心】

1. **零多余磁盘 I/O 预警**：将 `weakThis->m_config.save()` 移入 `if` 段内部。这意味着在普通重启恢复配置时，主线程和回调线程不会高频次去覆写磁盘 JSON，极大地优化了启动速度并保护了硬盘寿命。
2. **开箱即用保障**：此修改仅删除了越权的 `else` 条件分支，未引入任何新变量、新宏定义或第三方关联库，不存在编译期“未定义的标识符”或多线程不安全隐患。

---

## 7. Memories.md 合规检查

| 组件 / 模式 | Memories.md 规范要求 | 本方案是否符合 |
|-------------|----------------------|----------------|
| **磁盘初始化** | 物理修复磁盘根目录名称为空 | ✅ 本方案在 `initializeDrives` 对齐配置加载，不干涉根目录名称扫描，完全合规 |
| **评级显示** | 移除悬停高亮，星级停用 | ✅ 无任何交互冲突 |

---

## 8. 待确认事项
* 本方案在设计与持久化一致性上已达到最简与极致。直接实施上述精准 Search-Replace 即可实现无瑕疵的配置状态恢复。
