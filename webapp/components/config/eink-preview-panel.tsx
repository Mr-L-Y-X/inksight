"use client";

import Image from "next/image";
import type { ReactNode } from "react";
import { Eye, Loader2 } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { getDisplayWidth } from "@/components/ui/screen-size-select";

export function EInkPreviewPanel({
  tr,
  previewModeLabel,
  previewLoading,
  previewStatusText,
  previewImg,
  previewCacheHit,
  previewLlmStatus,
  canApplyToScreen,
  applyToScreenLoading,
  onRegenerate,
  onApplyToScreen,
  rightActions,
  screenW = 400,
  screenH = 300,
}: {
  tr: (zh: string, en: string) => string;
  previewModeLabel: string;
  previewLoading: boolean;
  previewStatusText: string;
  previewImg: string | null;
  previewCacheHit: boolean | null;
  previewLlmStatus: string | null;
  canApplyToScreen: boolean;
  applyToScreenLoading: boolean;
  onRegenerate: () => void;
  onApplyToScreen: () => void;
  rightActions?: ReactNode;
  screenW?: number;
  screenH?: number;
}) {
  // 预览按**物理尺寸**显示而不是按像素 1:1——3.98" 是 768×552（238 DPI），
  // 像素比 5.83"（648×480，138 DPI）多，但物理上更小。
  // dispW 已在 screen-size-select 里统一放大 25%（观感比 CSS 名义尺寸小），这里直接用。
  //
  // 只靠 CSS 约束：width = 实物宽度，max-width: 100% 让它在右栏放不下时自动缩，
  // aspect-ratio 保证高度跟着按比例走。**不要用 JS 量宽度**——首帧量到的值会被
  // 布局稳定后的真实值甩在后面，缩放系数就错了。
  const rawW = getDisplayWidth(screenW, screenH);

  return (
    <Card>
      <CardHeader className="pb-2">
        <CardTitle className="flex items-baseline justify-between gap-3 flex-wrap">
          <span className="text-base font-semibold text-ink">{tr("水墨屏预览", "E-Ink Preview")}</span>
          <span className="text-base font-semibold text-ink">{previewModeLabel}</span>
        </CardTitle>
      </CardHeader>
      {/* 内联 padding:0 —— CardContent 自带 p-6，靠 class 覆盖会被样式顺序吃掉 */}
      <CardContent className="p-0" style={{ padding: 0 }}>
        {/* 屏幕本体：宽度=实物宽度，图片直接铺满，不留内边距 */}
        <div className="flex justify-center">
          <div
            className="relative border border-ink/15 rounded-sm bg-paper flex flex-col items-center justify-center overflow-hidden"
            style={{
              width: `${rawW}px`,
              maxWidth: "100%",
              aspectRatio: `${screenW} / ${screenH}`,
            }}
          >
            {previewLoading ? (
              <div className="text-center px-3">
                <Loader2 size={28} className="animate-spin mx-auto text-ink-light mb-2" />
                <p className="text-[13px] text-ink-light">
                  {previewStatusText || tr("预览生成中...", "Generating preview...")}
                </p>
              </div>
            ) : previewImg ? (
              <Image src={previewImg} alt="Preview" fill unoptimized className="object-contain" />
            ) : (
              <div className="text-center px-3">
                <Eye size={28} className="mx-auto text-ink-light mb-2" />
                <p className="text-[13px] text-ink-light">
                  {tr("点击任意模式的「预览」查看效果", "Click Preview on any mode to view output")}
                </p>
              </div>
            )}
          </div>
        </div>

        {previewImg && !previewLoading && previewLlmStatus ? (
          <p className="pt-2 px-3 text-[13px] text-ink-light text-center">{previewLlmStatus}</p>
        ) : null}

        <p className="pt-2 text-[13px] text-ink-light text-center">
          {tr("按实物比例显示", "Shown at physical scale")}
        </p>

        <div className="mt-3 border-t border-ink/10 p-3 bg-white">
          {previewImg && !previewLoading && previewCacheHit === true ? (
            <div className="mb-2 text-xs text-amber-700 bg-amber-50 border border-amber-200 rounded-sm px-2 py-1.5">
              {tr(
                "当前预览为历史缓存。如需查看最新效果，请点击“重新生成预览”。",
                'Current preview is from cache. Click "Regenerate Preview" to fetch latest output.',
              )}
            </div>
          ) : null}
          <div className="flex gap-2 items-center">
            <Button
              variant="outline"
              size="sm"
              onClick={onRegenerate}
              disabled={!previewModeLabel || previewLoading}
              className="bg-white text-ink border-ink/20 hover:bg-ink hover:text-white active:bg-ink active:text-white disabled:bg-white disabled:text-ink/50"
            >
              {tr("重新生成预览", "Regenerate Preview")}
            </Button>
            <Button
              variant="outline"
              size="sm"
              onClick={onApplyToScreen}
              disabled={!canApplyToScreen || applyToScreenLoading || previewLoading}
              className="bg-white text-ink border-ink/20 hover:bg-ink hover:text-white active:bg-ink active:text-white disabled:bg-white disabled:text-ink/50"
            >
              {applyToScreenLoading ? <Loader2 size={14} className="animate-spin mr-1" /> : null}
              {tr("应用到墨水屏", "Apply to E-Ink")}
            </Button>
            {rightActions ? <div className="ml-auto flex items-center gap-2">{rightActions}</div> : null}
          </div>
        </div>
      </CardContent>
    </Card>
  );
}
