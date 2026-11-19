"""本地联调用：把某台设备直接绑定到一个本地 owner 账号，跳过 WebApp 注册流程。

用法（在 backend 目录下）：
    .venv/Scripts/python.exe scripts/bind_local_device.py AA:BB:CC:DD:EE:FF
    .venv/Scripts/python.exe scripts/bind_local_device.py AA:BB:CC:DD:EE:FF --user demo

--user 省略时用占位账号 local-dev（不可登录，仅用于免登录联调）。
绑定后设备请求 /api/render 就不再返回"配对码"提示图，而是走真实渲染。
"""

from __future__ import annotations

import asyncio
import os
import sqlite3
import sys
from datetime import datetime, timezone

DB_PATH = os.path.join(os.path.dirname(__file__), "..", "inksight.db")
DEFAULT_USER = "local-dev"


def main() -> int:
    args = [a for a in sys.argv[1:]]
    user_name = DEFAULT_USER
    if "--user" in args:
        idx = args.index("--user")
        if idx + 1 >= len(args):
            print("--user 后需要跟用户名")
            return 2
        user_name = args[idx + 1]
        del args[idx:idx + 2]

    if len(args) != 1:
        print("用法: python scripts/bind_local_device.py <MAC> [--user 用户名]")
        return 2
    mac = args[0].strip().upper()
    if not os.path.isfile(DB_PATH):
        print(f"找不到数据库 {DB_PATH}，请先启动一次后端让它建表")
        return 1

    now = datetime.now(timezone.utc).isoformat()
    conn = sqlite3.connect(DB_PATH)
    try:
        cur = conn.cursor()
        cur.execute("SELECT id FROM users WHERE username = ?", (user_name,))
        row = cur.fetchone()
        if row:
            user_id = row[0]
        elif user_name == DEFAULT_USER:
            # password_hash 占位即可：本地联调不走 WebApp 登录
            cur.execute(
                "INSERT INTO users (username, password_hash, role, invite_code, created_at)"
                " VALUES (?, ?, 'user', '', ?)",
                (user_name, "!local-dev-no-login", now),
            )
            user_id = cur.lastrowid
        else:
            print(f"用户 {user_name} 不存在，请先在 /login 页面注册")
            return 1

        cur.execute(
            "SELECT id FROM device_memberships WHERE mac = ? AND user_id = ?",
            (mac, user_id),
        )
        existing = cur.fetchone()
        if existing:
            cur.execute(
                "UPDATE device_memberships SET role = 'owner', status = 'active', updated_at = ? WHERE id = ?",
                (now, existing[0]),
            )
        else:
            cur.execute(
                "INSERT INTO device_memberships (mac, user_id, role, status, nickname, created_at, updated_at)"
                " VALUES (?, ?, 'owner', 'active', '', ?, ?)",
                (mac, user_id, now, now),
            )
        conn.commit()
    finally:
        conn.close()

    print(f"已绑定 {mac} -> owner={user_name} (user_id={user_id})")
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(asyncio.to_thread(main)))
