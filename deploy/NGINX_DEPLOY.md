# InkSight 前端 Nginx 反向代理部署说明

## 一句话结论

不动你现有的「前端 3000」Nginx 配置。你只需要在它已有的 `server {}` 块里
**追加一段 `/api` 反向代理**，把 API 请求转发到后端即可：

```
浏览器 → Nginx（前端站点，端口不变）
          ├─ /      → 前端（本来就是它管的，不动）
          └─ /api/  → 后端（新增这一段）
```

后端端口被占用时，换端口 + 改 `proxy_pass` 一行即可，前端**零改动、不重新 build**。

---

## 一、前端 8080 写死的位置（全部以环境变量为准）

| 文件 | 写死内容 | 说明 |
|---|---|---|
| `webapp/next.config.ts` | `http://127.0.0.1:8080` | rewrite fallback |
| `webapp/app/api/_proxy.ts` | `8080` + `8000` | 图片/上传/增删改代理 fallback |
| `webapp/app/api/firmware/_shared.ts` | `http://127.0.0.1:8080` | 固件接口 fallback |
| `webapp/app/api/uploads/route.ts` | `8080` + `8000` | 上传接口 fallback |

> 结论：这些**不用改**。它们都是兜底默认值，生产环境设了
> `INKSIGHT_BACKEND_API_BASE` 后一律读环境变量，兜底不生效。
> 本地 `npm run dev` 反而靠这些兜底才能零配置跑起来。

---

## 二、部署步骤

### 1. 把 `deploy/nginx.conf` 的片段粘进前端站点

在宝塔找到前端站点的配置文件（网站 → 设置 → 配置文件），
把 `deploy/nginx.conf` 里「追加从这里开始…到这里结束」中间的部分，
粘进该站点的 `server {}` 块内。

只需改一处：

- `proxy_pass http://127.0.0.1:3010;` → 后端 Python 项目实际端口

`proxy_pass` 后面**不要加斜杠**。后端路由本身就是 `app.include_router(router, prefix="/api")`，
即真实路径是 `/api/render`、`/api/device/{mac}/heartbeat`；写成 `http://127.0.0.1:3010/;`
会把 `/api` 前缀吃掉，全部 404。

### 2. 配前端环境变量（可选，视场景）

```bash
cp deploy/webapp.env webapp/.env.production
# 改 INKSIGHT_BACKEND_API_BASE 为后端实际端口
npm run build
```

> 说明：走 Nginx 反代后，浏览器只跟前端站点打交道，前端页面本身并不直连
> 8080。`INKSIGHT_BACKEND_API_BASE` 只影响 Next 服务端自身上游转发的场景
> （上传图片、固件代理等 Next 端内部 fetch），配不配取决于这些功能是否走
> Next 服务端。若都走浏览器 → Nginx 直接到后端，可不配。

### 3. 配后端环境变量

```bash
cp deploy/backend.env backend/.env
# 填 ADMIN_TOKEN、API Key，改域名相关两项
```

### 4. 重启生效

```bash
nginx -s reload
```

---

## 三、端口被占用时怎么办（核心诉求）

假设 8080 被别的服务占了：

1. 宝塔里把 Python 项目端口从 8080 改成空闲的，比如 **9090**；
2. 改前端站点 Nginx 里 `proxy_pass http://127.0.0.1:9090;`；
3. 若配了 `webapp/.env.production`，同步改 `INKSIGHT_BACKEND_API_BASE` 并重新 build；
4. 重启后端进程 + `nginx -s reload`。

前端页面代码一行不用动。

---

## 四、注意事项

- **CORS**：走 Nginx 反代后浏览器与前端站点同源，不再跨域，后端
  `INKSIGHT_CORS_ORIGINS` 一般无需额外配置。
- **流式/上传**：追加片段里已关 `proxy_buffering`（SSE 流式渲染、语音、LLM
  流式输出不卡顿），并把 `client_max_body_size` 放宽到 32m。
- **域名**：前端站点 `server_name`、后端 `INKSIGHT_ALLOWED_HOSTS`、
  `INKSIGHT_CORS_ORIGINS` 三处要保持一致，用你的真实部署域名。

---

## 五、唯一需要单独放行的接口

`/api/` 全部转给后端之后，前端 32 个 Next API 路由里有 **31 个**在后端有同名实现
（纯粹是转发 + 透传 Authorization/Cookie/X-Device-Token），唯一的例外是：

```
GET /api/firmware/download?url=<github release 地址>
```

这是 **Next 自己实现**的（校验必须是 GitHub 域名 → 再拉取转发），后端只有
`/api/firmware/download/{version}`，签名对不上。网页刷机页 `/flash` 就是靠它下载
`.bin` 的，被后端接走就会 404。

所以要在 `location /api/` **前面**加一个精确匹配把它抢回来：

```nginx
location = /api/firmware/download {
    proxy_pass http://127.0.0.1:3000;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
}
```

`location = ` 是精确匹配，优先级高于 `location /api/` 这个前缀匹配，位置写在哪都行。
