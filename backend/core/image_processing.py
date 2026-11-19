"""Reusable image fitting and e-ink quantization helpers."""
from __future__ import annotations

from contextvars import ContextVar
from typing import Tuple

from PIL import Image, ImageEnhance, ImageFilter, ImageOps

from . import native_dither

# 渲染期生效的图像调色参数（亮度, 对比度, 饱和度），100 = 不调整。
# 用 ContextVar 而不是函数签名透传：只有图片块需要它，而渲染器有 6 层调用栈。
# ContextVar 在 asyncio task 之间天然隔离，不会串设备。
_DEFAULT_ADJUST: Tuple[int, int, int] = (100, 100, 100)
_image_adjust: ContextVar[Tuple[int, int, int]] = ContextVar(
    "inksight_image_adjust", default=_DEFAULT_ADJUST
)


def _clamp(value, default: int = 100) -> int:
    try:
        v = int(float(value))
    except (TypeError, ValueError):
        return default
    return max(0, min(200, v))


def set_image_adjust(brightness=100, contrast=100, saturation=100) -> None:
    """设置本次渲染的调色参数。在 build_image 入口调用一次即可。"""
    _image_adjust.set((_clamp(brightness), _clamp(contrast), _clamp(saturation)))


def reset_image_adjust() -> None:
    _image_adjust.set(_DEFAULT_ADJUST)


def get_image_adjust() -> Tuple[int, int, int]:
    return _image_adjust.get()


def _enhance(img: Image.Image, *, brightness: int, contrast: int, saturation: int) -> Image.Image:
    """按 0-200 的百分比调节亮度/对比度/饱和度；100 为原值。

    顺序遵循常规调色流程：饱和度 -> 亮度 -> 对比度。
    saturation 对已转灰度的图（L/1 模式）无效，会被自动跳过。
    """
    out = img
    if saturation != 100 and out.mode in ("RGB", "RGBA"):
        out = ImageEnhance.Color(out).enhance(saturation / 100.0)
    if brightness != 100:
        out = ImageEnhance.Brightness(out).enhance(brightness / 100.0)
    if contrast != 100:
        out = ImageEnhance.Contrast(out).enhance(contrast / 100.0)
    return out


def _aligned_offset(container: int, content: int, align: str) -> int:
    if align in ("left", "top", "start"):
        return 0
    if align in ("right", "bottom", "end"):
        return container - content
    return (container - content) // 2


def fit_image_to_box(
    src: Image.Image,
    width: int,
    height: int,
    *,
    fit: str = "fill",
    align_x: str = "center",
    align_y: str = "center",
) -> Image.Image:
    """Fit an image into an RGB box using JSON image-block semantics."""
    src_rgba = ImageOps.exif_transpose(src).convert("RGBA")
    fit_mode = str(fit or "fill").lower()
    if fit_mode in ("fill", "stretch"):
        base = Image.new("RGBA", (width, height), (255, 255, 255, 255))
        base.alpha_composite(src_rgba.resize((width, height), Image.LANCZOS))
        return base.convert("RGB")

    src_w = max(1, src_rgba.size[0])
    src_h = max(1, src_rgba.size[1])
    scale_x = width / src_w
    scale_y = height / src_h
    scale = min(scale_x, scale_y) if fit_mode == "contain" else max(scale_x, scale_y)
    resized_w = max(1, int(round(src_w * scale)))
    resized_h = max(1, int(round(src_h * scale)))
    resized = src_rgba.resize((resized_w, resized_h), Image.LANCZOS)
    base = Image.new("RGBA", (width, height), (255, 255, 255, 255))
    paste_x = _aligned_offset(width, resized_w, align_x)
    paste_y = _aligned_offset(height, resized_h, align_y)
    base.alpha_composite(resized, (paste_x, paste_y))
    return base.convert("RGB")


def enhance_photo_for_eink(rgb: Image.Image) -> Image.Image:
    """Conservative photo preparation before e-ink quantization."""
    img = ImageOps.autocontrast(rgb.convert("RGB"), cutoff=1)
    img = ImageEnhance.Contrast(img).enhance(1.12)
    img = ImageEnhance.Sharpness(img).enhance(1.25)
    return img.filter(ImageFilter.UnsharpMask(radius=0.8, percent=80, threshold=3))


def quantize_image_for_eink(
    rgb: Image.Image,
    *,
    colors: int,
    photo_enhance: bool = False,
) -> Image.Image:
    """Quantize RGB image data for 2-, 3-, or 4-color e-ink output with Atkinson dithering."""
    prepared = enhance_photo_for_eink(rgb) if photo_enhance else rgb.convert("RGB")

    # 设备配置里的调色参数（亮度/对比度/饱和度），由 build_image 注入到 ContextVar。
    brightness, contrast, saturation = _image_adjust.get()

    if colors < 3:
        # 先 autocontrast 铺满动态范围，再叠加用户微调 —— 顺序反了会被 autocontrast 抹平。
        gray = ImageOps.autocontrast(prepared.convert("L"), cutoff=1)
        if (brightness, contrast) != (100, 100):
            gray = _enhance(gray, brightness=brightness, contrast=contrast, saturation=100)
        return native_dither.atkinson_bw(gray)

    if (brightness, contrast, saturation) != (100, 100, 100):
        prepared = _enhance(
            prepared, brightness=brightness, contrast=contrast, saturation=saturation
        )
    return native_dither.atkinson_palette(prepared, 3 if colors == 3 else 4)


def convert_image_block(
    src: Image.Image,
    width: int,
    height: int,
    colors: int,
    *,
    fit: str = "fill",
    align_x: str = "center",
    align_y: str = "center",
    photo_enhance: bool = False,
) -> Image.Image:
    """Fit and quantize an image for a JSON image block."""
    fitted = fit_image_to_box(src, width, height, fit=fit, align_x=align_x, align_y=align_y)
    return quantize_image_for_eink(
        fitted,
        colors=colors,
        photo_enhance=photo_enhance,
    )
