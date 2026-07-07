#pragma once

#include "ActionCommand.h"
#include <deque>
#include <memory>
#include <QObject>
#include <QMutex>
#include <QMutexLocker>

namespace ArcMeta {

/**
 * @brief 撤销栈管理器 (Undo Stack Manager)
 * 采用双栈结构，支持全局 Ctrl+Z / Ctrl+Y
 */
class UndoManager : public QObject {
    Q_OBJECT
public:
    static UndoManager& instance() {
        static UndoManager inst;
        return inst;
    }

    void pushCommand(std::unique_ptr<ActionCommand> command) {
        QMutexLocker lock(&m_mutex);
        m_undoStack.push_back(std::move(command));
        if (m_undoStack.size() > 50) {
            m_undoStack.pop_front();
        }
        m_redoStack.clear(); // 执行新操作时清空重做栈
        emit canUndoChanged(!m_undoStack.empty());
        emit canRedoChanged(false);
    }

    void undo() {
        QMutexLocker lock(&m_mutex);
        if (m_undoStack.empty()) return;

        auto command = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        
        command->undo();
        m_redoStack.push_back(std::move(command));
        
        emit canUndoChanged(!m_undoStack.empty());
        emit canRedoChanged(true);
    }

    void redo() {
        QMutexLocker lock(&m_mutex);
        if (m_redoStack.empty()) return;

        auto command = std::move(m_redoStack.back());
        m_redoStack.pop_back();
        
        command->redo();
        m_undoStack.push_back(std::move(command));
        
        emit canUndoChanged(true);
        emit canRedoChanged(!m_redoStack.empty());
    }

    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }

    /**
     * @brief 2026-06-xx 按照分析计划 #8：当文件被永久删除时，清理受影响的指令
     */
    void removeCommandsForPath(const QString& path) {
        QMutexLocker lock(&m_mutex);
        auto cleaner = [&](std::deque<std::unique_ptr<ActionCommand>>& stack) {
            for (auto it = stack.begin(); it != stack.end(); ) {
                // 这里需要一种方式判定 Command 是否涉及该路径
                // 暂时通过 description 或其他元数据判定（在 BasicCommands 中补全）
                // 简化处理：由于 std::unique_ptr 不能简单判定内容，我们在 ActionCommand 中增加接口
                if ((*it)->affectsPath(path)) {
                    it = stack.erase(it);
                } else {
                    ++it;
                }
            }
        };
        cleaner(m_undoStack);
        cleaner(m_redoStack);
        emit canUndoChanged(!m_undoStack.empty());
        emit canRedoChanged(!m_redoStack.empty());
    }

signals:
    void canUndoChanged(bool canUndo);
    void canRedoChanged(bool canRedo);

private:
    UndoManager() = default;
    ~UndoManager() = default;

    std::deque<std::unique_ptr<ActionCommand>> m_undoStack;
    std::deque<std::unique_ptr<ActionCommand>> m_redoStack;
    QMutex m_mutex;
};

} // namespace ArcMeta
