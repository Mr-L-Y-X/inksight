"use client";

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Loader2, RotateCcw, RotateCw, X } from "lucide-react";
import { Button } from "@/components/ui/button";

type PhotoCropDialogProps = {
  file: File;
  /** 目标画面尺寸（当前屏幕像素），决定裁切框比例 */
  targetWidth: number;
  targetHeight: number;
  busy?: boolean;
  tr: (zh: string, en: string) => string;
  onCancel: () => void;
  onConfirm: (blob: Blob) => void | Promise<void>;
};

const MAX_VIEW_WIDTH = 520;
const MIN_VIEW_WIDTH = 220;
const MAX_ZOOM = 4;
const MAX_EXPORT_WIDTH = 1600;
const MIN_EXPORT_WIDTH = 768;

/**
 * 按目标比例裁切照片：画布即成品画面，拖拽平移、滚轮/滑杆缩放、左右旋转。
 * 导出时按目标分辨率的 2 倍（上限 1600px 宽）重绘，保证下发到墨水屏不糊。
 */
export function PhotoCropDialog({
  file,
  targetWidth,
  targetHeight,
  busy = false,
  tr,
  onCancel,
  onConfirm,
}: PhotoCropDialogProps) {
  const [image, setImage] = useState<HTMLImageElement | null>(null);
  const [rotation, setRotation] = useState(0);
  const [zoom, setZoom] = useState(1);
  const [offset, setOffset] = useState({ x: 0, y: 0 });
  const [view, setView] = useState({ w: 0, h: 0 });
  const [loadFailed, setLoadFailed] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [exporting, setExporting] = useState(false);

  const wrapRef = useRef<HTMLDivElement | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const dragRef = useRef<{ x: number; y: number; ox: number; oy: number } | null>(null);
  const zoomRef = useRef(1);

  useEffect(() => {
    zoomRef.current = zoom;
  }, [zoom]);

  // 读取本地文件
  useEffect(() => {
    const url = URL.createObjectURL(file);
    const img = new Image();
    img.onload = () => {
      setImage(img);
      setRotation(0);
      setZoom(1);
      setOffset({ x: 0, y: 0 });
      setLoadFailed(false);
      setError(null);
    };
    img.onerror = () => setLoadFailed(true);
    img.src = url;
    return () => URL.revokeObjectURL(url);
  }, [file]);

  // 视口按容器宽度自适应，高度由目标比例决定
  useEffect(() => {
    const el = wrapRef.current;
    if (!el) return;
    const update = () => {
      const w = Math.max(MIN_VIEW_WIDTH, Math.min(MAX_VIEW_WIDTH, el.clientWidth || MAX_VIEW_WIDTH));
      setView({ w, h: Math.max(1, Math.round((w * targetHeight) / targetWidth)) });
    };
    update();
    if (typeof ResizeObserver === "undefined") return;
    const ro = new ResizeObserver(update);
    ro.observe(el);
    return () => ro.disconnect();
  }, [targetWidth, targetHeight]);

  const rotatedDims = useMemo(() => {
    if (!image) return { w: 1, h: 1 };
    const turned = rotation % 180 !== 0;
    return {
      w: turned ? image.naturalHeight : image.naturalWidth,
      h: turned ? image.naturalWidth : image.naturalHeight,
    };
  }, [image, rotation]);

  // zoom=1 时恰好铺满视口（cover）
  const coverScale = useMemo(() => {
    if (!image || !view.w) return 1;
    return Math.max(view.w / rotatedDims.w, view.h / rotatedDims.h);
  }, [image, view, rotatedDims]);

  const clampOffset = useCallback(
    (next: { x: number; y: number }, nextZoom: number) => {
      if (!image || !view.w) return next;
      const scale = coverScale * nextZoom;
      const maxX = Math.max(0, (rotatedDims.w * scale - view.w) / 2);
      const maxY = Math.max(0, (rotatedDims.h * scale - view.h) / 2);
      return {
        x: Math.max(-maxX, Math.min(maxX, next.x)),
        y: Math.max(-maxY, Math.min(maxY, next.y)),
      };
    },
    [image, view, coverScale, rotatedDims],
  );

  // 绘制预览
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || !image || !view.w) return;
    const dpr = Math.min(2, typeof window === "undefined" ? 1 : window.devicePixelRatio || 1);
    canvas.width = Math.round(view.w * dpr);
    canvas.height = Math.round(view.h * dpr);
    canvas.style.width = `${view.w}px`;
    canvas.style.height = `${view.h}px`;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.fillStyle = "#ffffff";
    ctx.fillRect(0, 0, view.w, view.h);
    const scale = coverScale * zoom;
    ctx.save();
    ctx.translate(view.w / 2 + offset.x, view.h / 2 + offset.y);
    ctx.rotate((rotation * Math.PI) / 180);
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(
      image,
      (-image.naturalWidth * scale) / 2,
      (-image.naturalHeight * scale) / 2,
      image.naturalWidth * scale,
      image.naturalHeight * scale,
    );
    ctx.restore();
    ctx.strokeStyle = "rgba(0,0,0,0.16)";
    ctx.lineWidth = 1;
    for (const ratio of [1 / 3, 2 / 3]) {
      const gx = Math.round(view.w * ratio) + 0.5;
      const gy = Math.round(view.h * ratio) + 0.5;
      ctx.beginPath();
      ctx.moveTo(gx, 0);
      ctx.lineTo(gx, view.h);
      ctx.stroke();
      ctx.beginPath();
      ctx.moveTo(0, gy);
      ctx.lineTo(view.w, gy);
      ctx.stroke();
    }
  }, [image, view, coverScale, zoom, offset, rotation]);

  // 滚轮缩放（React 的 onWheel 是被动监听，没法 preventDefault）
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const factor = e.deltaY > 0 ? 0.9 : 1.1;
      const next = Math.min(MAX_ZOOM, Math.max(1, zoomRef.current * factor));
      if (next === zoomRef.current) return;
      zoomRef.current = next;
      setZoom(next);
      setOffset((prev) => clampOffset(prev, next));
    };
    canvas.addEventListener("wheel", onWheel, { passive: false });
    return () => canvas.removeEventListener("wheel", onWheel);
  }, [clampOffset]);

  const applyZoom = useCallback(
    (next: number) => {
      const value = Math.min(MAX_ZOOM, Math.max(1, next));
      zoomRef.current = value;
      setZoom(value);
      setOffset((prev) => clampOffset(prev, value));
    },
    [clampOffset],
  );

  const rotate = (direction: 1 | -1) => {
    setRotation((prev) => (prev + direction * 90 + 360) % 360);
    zoomRef.current = 1;
    setZoom(1);
    setOffset({ x: 0, y: 0 });
  };

  const handleConfirm = () => {
    if (!image || !view.w || exporting || busy) return;
    setExporting(true);
    try {
      const exportWidth = Math.min(MAX_EXPORT_WIDTH, Math.max(MIN_EXPORT_WIDTH, targetWidth * 2));
      const k = exportWidth / view.w;
      const exportHeight = Math.max(1, Math.round(view.h * k));
      const canvas = document.createElement("canvas");
      canvas.width = exportWidth;
      canvas.height = exportHeight;
      const ctx = canvas.getContext("2d");
      if (!ctx) {
        setExporting(false);
        return;
      }
      ctx.fillStyle = "#ffffff";
      ctx.fillRect(0, 0, exportWidth, exportHeight);
      const scale = coverScale * zoom * k;
      ctx.translate(exportWidth / 2 + offset.x * k, exportHeight / 2 + offset.y * k);
      ctx.rotate((rotation * Math.PI) / 180);
      ctx.imageSmoothingEnabled = true;
      ctx.imageSmoothingQuality = "high";
      ctx.drawImage(
        image,
        (-image.naturalWidth * scale) / 2,
        (-image.naturalHeight * scale) / 2,
        image.naturalWidth * scale,
        image.naturalHeight * scale,
      );
      canvas.toBlob(
        (blob) => {
          setExporting(false);
          if (blob) void onConfirm(blob);
        },
        "image/jpeg",
        0.92,
      );
    } catch {
      setExporting(false);
      setError(tr("导出失败，请重试", "Export failed, please retry"));
    }
  };

  const working = busy || exporting;

  return (
    <div className="fixed inset-0 z-[60] flex items-center justify-center">
      <div
        className="absolute inset-0 bg-black/50"
        onClick={() => {
          if (!working) onCancel();
        }}
      />
      <div className="relative w-[min(600px,calc(100vw-24px))] rounded-sm border border-ink/15 bg-white shadow-xl">
        <div className="flex items-center justify-between border-b border-ink/10 px-4 py-3">
          <div className="text-sm font-semibold text-ink">{tr("裁切照片", "Crop Photo")}</div>
          <div className="flex items-center gap-3">
            <span className="text-[11px] text-ink-light">
              {targetWidth} × {targetHeight}
            </span>
            <button
              className="text-ink-light transition-colors hover:text-ink disabled:opacity-40"
              onClick={onCancel}
              disabled={working}
              title={tr("关闭", "Close")}
            >
              <X size={16} />
            </button>
          </div>
        </div>

        <div className="px-4 pt-4">
          <div ref={wrapRef} className="w-full">
            <div className="flex justify-center">
              <div className="relative overflow-hidden rounded-sm border border-ink/20 bg-paper-dark">
                {image ? (
                  <canvas
                    ref={canvasRef}
                    className="block cursor-move touch-none"
                    onPointerDown={(e) => {
                      if (working) return;
                      e.currentTarget.setPointerCapture(e.pointerId);
                      dragRef.current = { x: e.clientX, y: e.clientY, ox: offset.x, oy: offset.y };
                    }}
                    onPointerMove={(e) => {
                      const drag = dragRef.current;
                      if (!drag) return;
                      setOffset(
                        clampOffset(
                          { x: drag.ox + (e.clientX - drag.x), y: drag.oy + (e.clientY - drag.y) },
                          zoom,
                        ),
                      );
                    }}
                    onPointerUp={(e) => {
                      dragRef.current = null;
                      if (e.currentTarget.hasPointerCapture(e.pointerId)) {
                        e.currentTarget.releasePointerCapture(e.pointerId);
                      }
                    }}
                    onPointerCancel={() => {
                      dragRef.current = null;
                    }}
                  />
                ) : (
                  <div className="flex h-40 w-full items-center justify-center text-xs text-ink-light">
                    {loadFailed ? tr("无法读取这张图片", "Could not read this image") : <Loader2 size={18} className="animate-spin" />}
                  </div>
                )}
                {working ? (
                  <div className="absolute inset-0 flex items-center justify-center bg-white/70">
                    <Loader2 size={22} className="animate-spin text-ink" />
                  </div>
                ) : null}
              </div>
            </div>
          </div>
          <p className="mt-2 text-center text-[11px] text-ink-light">
            {tr("拖拽调整位置，滚轮或滑杆缩放", "Drag to reposition, scroll or use the slider to zoom")}
          </p>
        </div>

        <div className="flex items-center gap-3 px-4 pb-2 pt-3">
          <button
            className="rounded-sm border border-ink/20 p-1.5 text-ink transition-colors hover:bg-ink hover:text-white disabled:opacity-40"
            onClick={() => rotate(-1)}
            disabled={!image || working}
            title={tr("向左旋转", "Rotate left")}
          >
            <RotateCcw size={16} />
          </button>
          <button
            className="rounded-sm border border-ink/20 p-1.5 text-ink transition-colors hover:bg-ink hover:text-white disabled:opacity-40"
            onClick={() => rotate(1)}
            disabled={!image || working}
            title={tr("向右旋转", "Rotate right")}
          >
            <RotateCw size={16} />
          </button>
          <input
            type="range"
            min={1}
            max={MAX_ZOOM}
            step={0.01}
            value={zoom}
            onChange={(e) => applyZoom(Number(e.target.value))}
            disabled={!image || working}
            className="h-1 flex-1 cursor-pointer accent-ink"
            aria-label={tr("缩放", "Zoom")}
          />
          <button
            className="text-xs text-ink-light transition-colors hover:text-ink disabled:opacity-40"
            onClick={() => {
              zoomRef.current = 1;
              setZoom(1);
              setOffset({ x: 0, y: 0 });
              setRotation(0);
            }}
            disabled={!image || working}
          >
            {tr("重置", "Reset")}
          </button>
        </div>

        {error ? <div className="px-4 pb-1 text-xs text-red-600">{error}</div> : null}

        <div className="flex justify-end gap-2 px-4 pb-4 pt-2">
          <Button variant="outline" onClick={onCancel} disabled={working}>
            {tr("取消", "Cancel")}
          </Button>
          <Button onClick={handleConfirm} disabled={!image || working}>
            {working ? tr("处理中...", "Working...") : tr("使用这张", "Use This")}
          </Button>
        </div>
      </div>
    </div>
  );
}
