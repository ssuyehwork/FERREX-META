#pragma once
#include "IScanResultView.h"
#include "JustifiedView.h"

namespace FERREX {

class JustifiedResultView : public IScanResultView {
    Q_OBJECT
public:
    explicit JustifiedResultView(QWidget* parent = nullptr);
    ~JustifiedResultView() override;

    QWidget* getWidget() override;
    void setModel(QAbstractItemModel* model) override;
    void selectRows(const QItemSelection& selection) override;
    void clearSelection() override;
    void refreshLayout() override;
    void forceFetchAllResults() override;

    JustifiedView* getJustifiedView() const { return m_justifiedView; }

private:
    JustifiedView* m_justifiedView = nullptr;
};

} // namespace FERREX
