#pragma once
#include <QColor>
#include <QString>

namespace ArcMeta {
namespace Style {

/**
 * @brief 物理 UI 常量引擎 (StyleLibrary)
 * 解决硬编码问题，实现全应用视觉契约。
 */

// 核心品牌色
const QColor PrimaryBlue   = QColor("#3498db");
const QColor SuccessGreen   = QColor("#2ecc71");
const QColor WarningOrange  = QColor("#f39c12");
const QColor ErrorRed       = QColor("#e81123");
const QColor BrandOrange    = QColor("#FF551C");
const QColor AccentCyan     = QColor("#41F2F2");

// 面板背景色
const QColor BackgroundDeep   = QColor("#1E1E1E");
const QColor BackgroundHeader = QColor("#252526");
const QColor BackgroundHover  = QColor("#2A2A2A");
const QColor BackgroundSelected = QColor("#282828");
const QColor BorderColor      = QColor("#333333");
const QColor BorderDark       = QColor("#444444");

// 文字颜色
const QColor TextMain       = QColor("#EEEEEE");
const QColor TextDim        = QColor("#B0B0B0");
const QColor TextDark       = QColor("#AAAAAA");
const QColor TextMuted      = QColor("#888888");

// 布局常量
const int StandardIconSize  = 18;
const int ToolBtnIconSize   = 18;
const int RowHeight         = 28;
const int TreeIndentation   = 20;
const int TitleBarHeight    = 32;
const int StatusBarHeight   = 28;

// 交互反馈
const QColor HoverBackground = QColor(255, 255, 255, 25);
const QColor PressedBackground = QColor(255, 255, 255, 50);

// QSS Helper
inline QString qssColor(const QColor& color) { return color.name(); }

} // namespace Style
} // namespace ArcMeta
