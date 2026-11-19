"""ctypes bridge for native e-ink dithering.

优先加载编译好的 native 库（libeink_dither.dll / .so）；
本地没装 C++ 编译器时自动回退到等价的纯 Python 实现（较慢但结果一致）。
"""
from __future__ import annotations

import ctypes
import logging
from pathlib import Path

from PIL import Image

from .config import EINK_4COLOR_PALETTE

import platform

logger = logging.getLogger(__name__)

_EXT = ".dll" if platform.system() == "Windows" else ".so"
_LIB_PATH = Path(__file__).resolve().parent / "native" / f"libeink_dither{_EXT}"
_LIB: ctypes.CDLL | None = None
_BUILD_HINT = "run `python3 backend/scripts/build_native_dither.py` from the repository root"
_NATIVE_UNAVAILABLE = False

# Atkinson 误差扩散核：(dx, dy, 权重/8)
_ATK_KERNEL = ((1, 0, 1), (2, 0, 1), (-1, 1, 1), (0, 1, 1), (1, 1, 1), (0, 2, 1))
_PALETTE_RGB = ((0, 0, 0), (255, 255, 255), (232, 176, 0), (200, 0, 0))


def _load_lib() -> ctypes.CDLL:
    global _LIB
    if _LIB is not None:
        return _LIB
    if not _LIB_PATH.exists():
        raise RuntimeError(f"native dithering library not found at {_LIB_PATH}; {_BUILD_HINT}")
    try:
        lib = ctypes.CDLL(str(_LIB_PATH))
        lib.inksight_atkinson_bw.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_int,
        ]
        lib.inksight_atkinson_bw.restype = ctypes.c_int
        lib.inksight_atkinson_palette.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_int,
        ]
        lib.inksight_atkinson_palette.restype = ctypes.c_int
        _LIB = lib
        return lib
    except OSError as exc:
        raise RuntimeError(f"failed to load native dithering library at {_LIB_PATH}: {exc}") from exc


def _try_lib() -> ctypes.CDLL | None:
    """取 native 库；不可用时只警告一次，之后静默走 Python 实现。"""
    global _NATIVE_UNAVAILABLE
    if _NATIVE_UNAVAILABLE:
        return None
    try:
        return _load_lib()
    except RuntimeError as exc:
        _NATIVE_UNAVAILABLE = True
        logger.warning("[DITHER] %s —— 回退到纯 Python 实现（结果相同，速度较慢）", exc)
        return None


def _atkinson_bw_py(gray: Image.Image) -> Image.Image:
    """纯 Python 版 Atkinson 黑白抖色，与 eink_dither.cpp 的 inksight_atkinson_bw 等价。"""
    src = gray.convert("L")
    w, h = src.size
    data = [float(v) for v in src.tobytes()]
    out = bytearray(w * h)
    for y in range(h):
        row = y * w
        row1 = row + w
        row2 = row + 2 * w
        has1 = y + 1 < h
        has2 = y + 2 < h
        for x in range(w):
            pos = row + x
            old = data[pos]
            if old >= 128.0:
                out[pos] = 255
                err = (old - 255.0) * 0.125
            else:
                err = old * 0.125
            if x + 1 < w:
                v = data[pos + 1] + err
                data[pos + 1] = 255.0 if v > 255.0 else (0.0 if v < 0.0 else v)
            if x + 2 < w:
                v = data[pos + 2] + err
                data[pos + 2] = 255.0 if v > 255.0 else (0.0 if v < 0.0 else v)
            if has1:
                if x > 0:
                    i = row1 + x - 1
                    v = data[i] + err
                    data[i] = 255.0 if v > 255.0 else (0.0 if v < 0.0 else v)
                i = row1 + x
                v = data[i] + err
                data[i] = 255.0 if v > 255.0 else (0.0 if v < 0.0 else v)
                if x + 1 < w:
                    i = row1 + x + 1
                    v = data[i] + err
                    data[i] = 255.0 if v > 255.0 else (0.0 if v < 0.0 else v)
            if has2:
                i = row2 + x
                v = data[i] + err
                data[i] = 255.0 if v > 255.0 else (0.0 if v < 0.0 else v)
    return Image.frombytes("L", (w, h), bytes(out)).convert("1", dither=Image.Dither.NONE)


def _atkinson_palette_py(rgb: Image.Image, colors: int) -> Image.Image:
    """纯 Python 版 Atkinson 调色板抖色，与 inksight_atkinson_palette 等价。"""
    src = rgb.convert("RGB")
    w, h = src.size
    raw = src.tobytes()
    data = [float(v) for v in raw]
    out = bytearray(w * h)
    allowed = (0, 1, 3) if colors == 3 else (0, 1, 2, 3)
    pal = _PALETTE_RGB
    for y in range(h):
        row = y * w
        row1 = row + w
        row2 = row + 2 * w
        has1 = y + 1 < h
        has2 = y + 2 < h
        for x in range(w):
            pos = row + x
            base = pos * 3
            r, g, b = data[base], data[base + 1], data[base + 2]
            best = allowed[0]
            best_dist = 1e30
            for idx in allowed:
                pr, pg, pb = pal[idx]
                dr = r - pr
                dg = g - pg
                db = b - pb
                dist = dr * dr + dg * dg + db * db
                if dist < best_dist:
                    best_dist = dist
                    best = idx
            out[pos] = best
            pr, pg, pb = pal[best]
            er = (r - pr) * 0.125
            eg = (g - pg) * 0.125
            eb = (b - pb) * 0.125
            for dx, dy, _weight in _ATK_KERNEL:
                nx = x + dx
                ny = y + dy
                if nx < 0 or nx >= w or ny >= h:
                    continue
                nbase = (ny * w + nx) * 3
                v = data[nbase] + er
                data[nbase] = 255.0 if v > 255.0 else (0.0 if v < 0.0 else v)
                v = data[nbase + 1] + eg
                data[nbase + 1] = 255.0 if v > 255.0 else (0.0 if v < 0.0 else v)
                v = data[nbase + 2] + eb
                data[nbase + 2] = 255.0 if v > 255.0 else (0.0 if v < 0.0 else v)
    result = Image.frombytes("P", (w, h), bytes(out))
    result.putpalette(EINK_4COLOR_PALETTE + [0] * (768 - len(EINK_4COLOR_PALETTE)))
    return result


def atkinson_bw(gray: Image.Image) -> Image.Image:
    lib = _try_lib()
    if lib is None:
        return _atkinson_bw_py(gray)
    src = gray.convert("L")
    w, h = src.size
    in_buf = src.tobytes()
    out_buf = ctypes.create_string_buffer(w * h)
    err_buf = ctypes.create_string_buffer(256)
    status = lib.inksight_atkinson_bw(
        in_buf,
        w,
        h,
        out_buf,
        err_buf,
        len(err_buf),
    )
    if status != 0:
        raise RuntimeError(f"native black/white Atkinson dithering failed: {err_buf.value.decode('utf-8', errors='replace')}")
    return Image.frombytes("L", (w, h), out_buf.raw).convert("1", dither=Image.Dither.NONE)


def atkinson_palette(rgb: Image.Image, colors: int) -> Image.Image:
    if colors not in (3, 4):
        raise ValueError("native palette Atkinson dithering supports only 3 or 4 colors")
    lib = _try_lib()
    if lib is None:
        return _atkinson_palette_py(rgb, colors)
    src = rgb.convert("RGB")
    w, h = src.size
    in_buf = src.tobytes()
    out_buf = ctypes.create_string_buffer(w * h)
    err_buf = ctypes.create_string_buffer(256)
    status = lib.inksight_atkinson_palette(
        in_buf,
        w,
        h,
        int(colors),
        out_buf,
        err_buf,
        len(err_buf),
    )
    if status != 0:
        raise RuntimeError(f"native palette Atkinson dithering failed: {err_buf.value.decode('utf-8', errors='replace')}")
    out = Image.frombytes("P", (w, h), out_buf.raw)
    out.putpalette(EINK_4COLOR_PALETTE + [0] * (768 - len(EINK_4COLOR_PALETTE)))
    return out
