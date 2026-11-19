"use client";

import { useCallback, useEffect, useState } from "react";

/**
 * 把一份选择结果记到 localStorage，刷新后自动恢复。
 *
 * 关键点：**首帧一律用默认值**。SSR 阶段没有 localStorage，若在 useState 的
 * 初始化函数里去读，客户端 hydrate 的首次结果会和服务端 HTML 对不上。
 * 所以先渲染默认值，等挂载后的 effect 再读回缓存覆盖一次——刷新时会有一次
 * 极短的状态切换，对这几个只有几像素大小的按钮没有可见影响。
 *
 * 只在用户主动修改时写入，避免「首次渲染的默认值把上次的选择冲掉」。
 */
export function usePersistedState<T>(key: string, defaultValue: T) {
  const [value, setValue] = useState<T>(defaultValue);

  useEffect(() => {
    const raw = readStorage(key);
    if (raw === null) return;
    try {
      setValue(JSON.parse(raw) as T);
    } catch {
      // 缓存损坏就当没有，继续用默认值
    }
  }, [key]);

  const update = useCallback(
    (next: T) => {
      setValue(next);
      writeStorage(key, JSON.stringify(next));
    },
    [key],
  );

  return [value, update] as const;
}

function readStorage(key: string): string | null {
  if (typeof window === "undefined") return null;
  try {
    return window.localStorage.getItem(key);
  } catch {
    return null;
  }
}

function writeStorage(key: string, value: string): void {
  if (typeof window === "undefined") return;
  try {
    window.localStorage.setItem(key, value);
  } catch {
    // 隐私模式 / 配额写满时静默失败，不影响当前会话
  }
}
