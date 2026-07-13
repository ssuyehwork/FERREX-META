#pragma once
#include "IScanResultView.h"

class QThreadPool;

namespace FERREX {

class JustifiedView;

class JustifiedResultView : public IScanResultView {
    Q_OBJECT
public:
    explicit JustifiedResultView(QWidget* parent = nullptr);
    ~JustifiedResultView() override;

    QWidget* getWidget() override;
    QAbstractItemView* getBaseView() override;
    void setModel(QAbstractItemModel* model) override;
    void setIconSize(int size) override;
    void refreshLayout() override;

private:
    JustifiedView* m_justifiedView = nullptr;
    QThreadPool* m_thumbPool = nullptr;
};

} // namespace FERREX
