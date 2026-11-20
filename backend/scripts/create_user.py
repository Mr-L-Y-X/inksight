"""直接创建可登录账号（不走 WebApp 注册，也不需要跑后端进程）。

服务器上用面板的 venv python 跑（只用标准库，不依赖项目包）：

    cd /www/wwwroot/inksight/backend
    python scripts/create_user.py demo demo1234 --email demo@example.com
    python scripts/create_user.py admin admin1234 --email admin@example.com --role root

参数：
    <username> <password>   必填
    --email  x@y.com        建议填，登录/找回密码要用
    --role   user|root      默认 user；root 是管理员（webapp 无后台页面，只能调 API）
    --update                用户已存在时改密码/角色，而不是报错退出
    --db     /path/to.db    默认 backend/inksight.db，一般不用改

密码哈希格式与 core/config_store.py 的 _hash_password 一致：
    pbkdf2_hmac("sha256", password, salt, 200_000) → "salt_hex:dk_hex"
"""

from __future__ import annotations

import argparse
import hashlib
import os
import secrets
import sqlite3
import sys
from datetime import datetime

DB_PATH = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "inksight.db"))
HASH_ROUNDS = 200_000
FREE_QUOTA = 50


def hash_password(password: str) -> str:
    salt = secrets.token_bytes(16)
    dk = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, HASH_ROUNDS)
    return salt.hex() + ":" + dk.hex()


def main() -> int:
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("username")
    ap.add_argument("password")
    ap.add_argument("--email", default="")
    ap.add_argument("--role", default="user", choices=["user", "root"])
    ap.add_argument("--update", action="store_true", help="已存在时更新密码与角色")
    ap.add_argument("--db", default=DB_PATH)
    args = ap.parse_args()

    db_path = os.path.normpath(args.db)
    if not os.path.isfile(db_path):
        print(f"找不到数据库 {db_path}")
        print("请先在面板启动一次后端（启动时会自动建表），再运行本脚本。")
        return 1

    username = args.username.strip()
    if len(username) < 2:
        print("用户名至少 2 个字符")
        return 2
    if len(args.password) < 4:
        print("密码至少 4 位")
        return 2

    now = datetime.now().isoformat()
    pw_hash = hash_password(args.password)

    conn = sqlite3.connect(db_path)
    try:
        cur = conn.cursor()
        cur.execute("SELECT id FROM users WHERE username = ?", (username,))
        row = cur.fetchone()

        if row:
            if not args.update:
                print(f"用户 {username} 已存在（id={row[0]}）。改密码请加 --update")
                return 1
            cur.execute(
                "UPDATE users SET password_hash = ?, role = ? WHERE id = ?",
                (pw_hash, args.role, row[0]),
            )
            if args.email:
                cur.execute("UPDATE users SET email = ? WHERE id = ?", (args.email, row[0]))
            user_id = row[0]
            action = "已更新"
        else:
            try:
                cur.execute(
                    "INSERT INTO users (username, password_hash, email, role, invite_code, created_at)"
                    " VALUES (?, ?, ?, ?, '', ?)",
                    (username, pw_hash, args.email or None, args.role, now),
                )
            except sqlite3.IntegrityError as exc:
                print(f"创建失败（用户名/邮箱已占用）：{exc}")
                return 1
            user_id = cur.lastrowid
            cur.execute(
                "INSERT OR IGNORE INTO api_quotas (user_id, total_calls_made, free_quota_remaining)"
                " VALUES (?, 0, ?)",
                (user_id, FREE_QUOTA),
            )
            action = "已创建"

        conn.commit()
    finally:
        conn.close()

    print(f"{action}：{username}（id={user_id}, role={args.role}）")
    if args.email:
        print(f"  邮箱：{args.email}")
    print("现在可以用它登录 /zh/login ，或让设备绑定到这个账号。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
