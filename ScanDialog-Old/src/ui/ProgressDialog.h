#pragma once

#include "FramelessDialog.h"
#include <QProgressBar>
#include <QLabel>
#include <QVBoxLayout>

namespace ArcMeta {

class ProgressDialog : public FramelessDialog {
    Q_OBJECT

public:
    explicit ProgressDialog(const QString& title, QWidget* parent = nullptr)
        : FramelessDialog(title, parent) {
        setFixedSize(400, 150);
        
        auto* layout = new QVBoxLayout(m_contentArea);
        layout->setContentsMargins(20, 20, 20, 20);
        layout->setSpacing(10);

        m_statusLabel = new QLabel("正在准备...", this);
        m_statusLabel->setStyleSheet("color: #EEE; font-size: 13px;");
        layout->addWidget(m_statusLabel);

        m_progressBar = new QProgressBar(this);
        m_progressBar->setFixedHeight(8);
        m_progressBar->setTextVisible(false);
        m_progressBar->setStyleSheet(
            "QProgressBar { background: #2D2D2D; border: none; border-radius: 4px; }"
            "QProgressBar::chunk { background: #378ADD; border-radius: 4px; }"
        );
        layout->addWidget(m_progressBar);
        
        layout->addStretch();
    }

    void setStatus(const QString& text) {
        m_statusLabel->setText(text);
    }

    void setRange(int min, int max) {
        m_progressBar->setRange(min, max);
    }

    void setValue(int value) {
        m_progressBar->setValue(value);
    }

private:
    QLabel* m_statusLabel = nullptr;
    QProgressBar* m_progressBar = nullptr;
};

} // namespace ArcMeta
