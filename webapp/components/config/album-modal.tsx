"use client";

import { useRef, useState } from "react";
import { Loader2, Plus, X } from "lucide-react";
import { Button } from "@/components/ui/button";
import { PhotoCropDialog } from "@/components/config/photo-crop-dialog";

type AlbumModalProps = {
  action: "preview" | "apply";
  initialUrls: string[];
  screenWidth: number;
  screenHeight: number;
  tr: (zh: string, en: string) => string;
  upload: (file: File) => Promise<string>;
  onCancel: () => void;
  onConfirm: (urls: string[]) => void | Promise<void>;
};

const MAX_PHOTOS = 6;

/**
 * 相册图片管理：选图后先按当前屏幕比例裁切（可旋转），再上传。
 * 与相框不同的是，相册成品无状态栏/页脚，所以裁切框比例 = 整屏比例。
 */
export function AlbumModal({
  action,
  initialUrls,
  screenWidth,
  screenHeight,
  tr,
  upload,
  onCancel,
  onConfirm,
}: AlbumModalProps) {
  const [urls, setUrls] = useState<string[]>(initialUrls);
  const [queue, setQueue] = useState<File[]>([]);
  const [active, setActive] = useState<File | null>(null);
  const [uploading, setUploading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const fileRef = useRef<HTMLInputElement | null>(null);

  const remain = MAX_PHOTOS - urls.length;

  const pickFiles = (files: File[]) => {
    if (files.length === 0) return;
    const picked = files
      .filter((f) => f.type.startsWith("image/"))
      .slice(0, Math.max(0, remain));
    if (picked.length === 0) {
      setError(
        remain <= 0
          ? tr("已达 6 张上限，请先移除一张", "Limit of 6 photos reached, remove one first")
          : tr("请选择图片文件", "Please choose image files"),
      );
      return;
    }
    setError(null);
    setActive(picked[0]);
    setQueue(picked.slice(1));
  };

  const handleCropConfirm = async (blob: Blob) => {
    if (!active) return;
    setUploading(true);
    setError(null);
    try {
      const baseName = active.name.replace(/\.[^.]+$/, "") || "photo";
      const file = new File([blob], `${baseName}.jpg`, { type: "image/jpeg" });
      const url = await upload(file);
      setUrls((prev) => (prev.length >= MAX_PHOTOS ? prev : [...prev, url]));
      if (queue.length > 0) {
        setActive(queue[0]);
        setQueue((prev) => prev.slice(1));
      } else {
        setActive(null);
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : tr("上传失败", "Upload failed"));
    } finally {
      setUploading(false);
    }
  };

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center">
      <div
        className="absolute inset-0 bg-black/40"
        onClick={() => {
          if (!active) onCancel();
        }}
      />
      <div className="relative w-[min(540px,calc(100vw-32px))] rounded-sm border border-ink/15 bg-white shadow-xl">
        <div className="flex items-center justify-between border-b border-ink/10 px-4 py-3">
          <div className="text-sm font-semibold text-ink">{tr("相册图片", "Album Photos")}</div>
          <button
            className="text-ink-light hover:text-ink"
            onClick={onCancel}
            disabled={Boolean(active)}
            title={tr("关闭", "Close")}
          >
            ✕
          </button>
        </div>

        <div className="space-y-3 px-4 py-4">
          <div className="text-xs text-ink-light">
            {tr(
              `选图后先按当前屏幕比例（${screenWidth}×${screenHeight}）拖拽裁切，可旋转后再上传，至多 6 张。`,
              `Crop each photo to your screen ratio (${screenWidth}×${screenHeight}) before uploading, up to 6.`,
            )}
          </div>
          <div className="rounded border border-amber-300 bg-amber-50 px-3 py-2 text-xs text-amber-900">
            {tr(
              "照片会全屏显示，不含时间、天气和页脚。预览或下发只是临时生效，保留请点击“保存到设备”。",
              "Photos fill the whole screen with no status bar or footer. Previewing is temporary — use \"Save to Device\" to keep them.",
            )}
          </div>

          <div className="grid grid-cols-3 gap-2">
            {urls.map((url, i) => (
              <div
                key={`${url}-${i}`}
                className="group relative overflow-hidden rounded border border-ink/15 bg-paper-dark"
                style={{ aspectRatio: `${screenWidth} / ${screenHeight}` }}
              >
                {/* eslint-disable-next-line @next/next/no-img-element */}
                <img src={url} alt={`album ${i + 1}`} className="h-full w-full object-cover" />
                <button
                  className="absolute right-1 top-1 flex h-5 w-5 items-center justify-center rounded-full bg-black/60 text-xs text-white opacity-0 transition-opacity group-hover:opacity-100"
                  onClick={() => setUrls((prev) => prev.filter((_, idx) => idx !== i))}
                  title={tr("移除", "Remove")}
                >
                  ✕
                </button>
                <div className="absolute bottom-0 left-0 right-0 bg-black/40 py-0.5 text-center text-[10px] text-white">
                  {i + 1}
                </div>
              </div>
            ))}
            {remain > 0 ? (
              <button
                className="flex flex-col items-center justify-center rounded border-2 border-dashed border-ink/20 text-ink-light transition-colors hover:border-ink/40 hover:text-ink"
                style={{ aspectRatio: `${screenWidth} / ${screenHeight}` }}
                onClick={() => fileRef.current?.click()}
                disabled={uploading || Boolean(active)}
              >
                {uploading && !active ? (
                  <Loader2 size={20} className="animate-spin" />
                ) : (
                  <>
                    <Plus size={20} className="leading-none" />
                    <span className="mt-0.5 text-[10px]">{tr("添加照片", "Add")}</span>
                  </>
                )}
              </button>
            ) : null}
          </div>

          {error ? <div className="text-xs text-red-600">{error}</div> : null}

          <div className="flex justify-end gap-2 pt-2">
            <Button variant="outline" onClick={onCancel} disabled={uploading || Boolean(active)}>
              {tr("取消", "Cancel")}
            </Button>
            <Button
              disabled={urls.length === 0 || uploading || Boolean(active)}
              onClick={() => onConfirm(urls)}
            >
              {action === "apply" ? tr("确认并加入轮播", "Confirm & Add to Rotation") : tr("确认并预览", "Confirm & Preview")}
            </Button>
          </div>
        </div>

        <input
          ref={fileRef}
          type="file"
          accept="image/*"
          multiple
          className="hidden"
          onChange={(e) => {
            // 必须先把 FileList 拷成数组：清空 input.value 会连带清空这个 live 的 FileList
            const files = e.target.files ? Array.from(e.target.files) : [];
            e.target.value = "";
            pickFiles(files);
          }}
        />
      </div>

      {active ? (
        <PhotoCropDialog
          file={active}
          targetWidth={screenWidth}
          targetHeight={screenHeight}
          busy={uploading}
          tr={tr}
          onCancel={() => {
            if (uploading) return;
            setActive(null);
            setQueue([]);
          }}
          onConfirm={handleCropConfirm}
        />
      ) : null}
    </div>
  );
}
