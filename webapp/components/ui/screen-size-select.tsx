"use client";

/**
 * 屏幕尺寸选项。
 *
 * `dispW` 是预览卡片在网页上的**显示宽度（CSS px）**，按物理尺寸等比换算，
 * 再把结果放大 25%（CSS 名义 1px = 1/96 英寸，但实际屏幕上偏小，见下）：
 *
 *   物理宽(in) = w / ( sqrt(w²+h²) / 对角线英寸 )
 *   dispW     = 物理宽 × 120   // CSS 名义 1px = 1/96 英寸，再放大 25% 抵消观感偏小
 *
 * 各尺寸像素密度差异很大，所以「像素多」不等于「屏大」：
 *   2.9"  296×128  → 111 DPI，物理宽 2.66in → 显示 320px
 *   3.98" 768×552  → 238 DPI，物理宽 3.23in → 显示 388px
 *   4.2"  400×300  → 119 DPI，物理宽 3.36in → 显示 404px
 *   5.83" 648×480  → 138 DPI，物理宽 4.69in → 显示 563px
 *
 * 3.98" 像素最多但物理上比 5.83" 小，显示宽度必须按物理尺寸走。
 * 想回到严格 1:1 就把系数 120 改成 96。
 */
export const SCREEN_OPTIONS = [
  { w: 400, h: 300, dispW: 404, zh: '4.2"', en: '4.2"' },
  { w: 296, h: 128, dispW: 320, zh: '2.9"', en: '2.9"' },
  { w: 768, h: 552, dispW: 388, zh: '3.98"', en: '3.98"' },
  { w: 648, h: 480, dispW: 563, zh: '5.83"', en: '5.83"' },
] as const;

/** 尺寸表里最大的显示宽度（5.83" = 563px），未知尺寸的兜底上限。 */
export const MAX_DISP_W = 563;

/** 按像素尺寸反查推荐显示宽度；未知尺寸回落到像素 1:1，并夹到 450px。 */
export function getDisplayWidth(w: number, h: number): number {
  const hit = SCREEN_OPTIONS.find((o) => o.w === w && o.h === h);
  return hit ? hit.dispW : Math.min(w, MAX_DISP_W);
}

interface ScreenSizeSelectProps {
  width: number;
  height: number;
  onChange: (w: number, h: number) => void;
  tr: (zh: string, en: string) => string;
}

export function ScreenSizeSelect({ width, height, onChange, tr }: ScreenSizeSelectProps) {
  return (
    <div className="flex" title={tr("屏幕尺寸", "Screen size")}>
      {SCREEN_OPTIONS.map((o, i) => {
        const active = o.w === width && o.h === height;
        return (
          <button
            key={o.w + "x" + o.h}
            type="button"
            onClick={() => onChange(o.w, o.h)}
            className={`px-2 py-1 text-xs border border-ink/20 transition-colors ${
              i === 0 ? "rounded-l-sm" : ""
            }${i === SCREEN_OPTIONS.length - 1 ? "rounded-r-sm" : ""
            }${i > 0 ? " -ml-px" : ""
            } ${active
              ? "bg-ink text-white border-ink z-10 relative"
              : "bg-white text-ink hover:bg-ink/5"
            }`}
          >
            {tr(o.zh, o.en)}
          </button>
        );
      })}
    </div>
  );
}
