# github1/github2.me3.dpdns.org 镜像 Worker

gh-proxy 改造版（基于 git23 增强版）。两个 Worker 同源部署：
- github1.me3.dpdns.org → Worker "github1"
  - 旧客户端兼容别名：fast1.me3.dpdns.org（v1.7.8.60 等版本硬编码，勿删）
- github2.me3.dpdns.org → Worker "git23"
  - 旧客户端兼容别名：fast2.me3.dpdns.org（v1.7.8.60 等版本硬编码，勿删）
- Secret `GITHUB_TOKEN`（GitHub PAT，api 匿名 60/h/IP 提额到 5000/h）
- 部署走 CF API PUT scripts/{name}（保留 workers custom domain 绑定）；源码更新改本文件后上传。

关键改造（2026-11-08）：release 下载路径（releases/download/<tag>/<asset> 与
releases/latest/download/<asset>）不再 fetch github.com（该入口 504 时全部镜像陪葬），
改走 api.github.com：
1. GET /repos/{owner}/{repo}/releases/tags/<tag>（latest 路径用 /releases/latest）
2. assets[] 按 name 找 asset 的 url 字段
3. GET 该 url（Accept: application/octet-stream）302 → release-assets Azure blob，
   Worker 内部跟随（客户端只见 200/206，永不收 302），透传 Range 断点续传。
