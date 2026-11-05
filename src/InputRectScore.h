#pragma once
// 候选窗锚点矩形可信度评分（纯函数，TextService.cpp 与单测共用）。
//
// inputRect 三级取数（真插入符/TSF 组字起点/last-good）+ IMM32 救援层用
// 同一把尺子给候选锚点打分：高度 ≤1px 的 CUAS 幽灵点直接判废（-1000，
// 任何奖励不得翻正），其余按宽度/高度/是否在前景窗内/是否前景窗左上角
// 小标记加减分。
#include <windows.h>

namespace Ime {

inline int absoluteDiff(int lhs, int rhs) {
	return lhs > rhs ? lhs - rhs : rhs - lhs;
}

// rect：候选锚点矩形；foregroundRect：前景窗口矩形（可空=无从判断）；
// preferSelection：selection 锚点相对 composition 起点的 +1 先手。
inline int inputRectScore(const RECT& rect, const RECT* foregroundRect,
                          bool preferSelection) {
	int score = preferSelection ? 1 : 0;
	const int width = rect.right - rect.left;
	const int height = rect.bottom - rect.top;

	// 高度 0~1px 的"矩形"不可能是一行文字：CUAS 对纯 IMM 宿主（wezterm 类）
	// 代理出的幽灵点恒定在屏幕右下（实测全屏时 (1913,931,1914,931)，不随
	// 光标/pane 动），旧的 -30 罚分会被"在前景窗内 +40"翻正，短路 IMM32
	// 救援层，把候选窗钉死在屏幕右下。真锚点最矮也是终端光标格（约 20px）。
	// 直接判废，任何奖励不得翻正。
	if (height <= 1) {
		return -1000;
	}

	if (width < 0) {
		score -= 100;
	} else {
		if (width == 0) {
			score -= 10;
		}
		if (height < 8) {
			score -= 10;
		}
	}

	if (!foregroundRect) {
		return score;
	}

	const bool insideForeground =
	    rect.left >= foregroundRect->left && rect.left <= foregroundRect->right &&
	    rect.top >= foregroundRect->top && rect.top <= foregroundRect->bottom;
	if (insideForeground) {
		score += 40;
	} else {
		score -= 20;
	}

	const bool tinyTopLeftMarker =
	    absoluteDiff(rect.left, foregroundRect->left) <= 48 &&
	    absoluteDiff(rect.top, foregroundRect->top) <= 48 &&
	    width <= 4 &&
	    height > 0 && height <= 48;
	if (tinyTopLeftMarker) {
		score -= 80;
	}

	return score;
}

} // namespace Ime
