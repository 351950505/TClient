'use strict'

/**
 * static files (404.html, sw.js, conf.js)
 */
const ASSET_URL = 'https://geekertao.github.io/gh-proxy/'
// 前缀，如果自定义路由为example.com/gh/*，将PREFIX改为 '/gh/'，注意，少一个杠都会错！
const PREFIX = '/'
// 分支文件使用jsDelivr镜像的开关，0为关闭，默认关闭
const Config = {
    jsdelivr: 0,
    // 是否把 GitHub 签名资源跳转（release-assets / objects 等）重写为代理链接返回给客户端。
    // 0（默认）：Worker 内部跟随跳转并直接流式返回，1 次往返，与旧版行为一致。
    // 1：返回 302 让客户端再请求一次代理，URL 保持可见（便于浏览器扩展统一接管）。
    rewriteAssetRedirect: 0
}

const whiteList = [] // 白名单，路径里面有包含字符的才会通过，e.g. ['/username/']

/** @type {ResponseInit} */
const PREFLIGHT_INIT = {
    status: 204,
    headers: new Headers({
        'access-control-allow-origin': '*',
        'access-control-allow-methods': 'GET,POST,PUT,PATCH,TRACE,DELETE,HEAD,OPTIONS',
        'access-control-max-age': '1728000',
    }),
}


const exp1 = /^(?:https?:\/\/)?github\.com\/.+?\/.+?\/(?:releases|archive)\/.*$/i
const exp2 = /^(?:https?:\/\/)?github\.com\/.+?\/.+?\/(?:blob|raw)\/.*$/i
const exp3 = /^(?:https?:\/\/)?github\.com\/.+?\/.+?\/(?:info|git-).*$/i
const exp4 = /^(?:https?:\/\/)?raw\.(?:githubusercontent|github)\.com\/.+?\/.+?\/.+?\/.+$/i
const exp5 = /^(?:https?:\/\/)?gist\.(?:githubusercontent|github)\.com\/.+?\/.+?\/.+$/i
const exp6 = /^(?:https?:\/\/)?github\.com\/.+?\/.+?\/tags.*$/i
const exp7 = /^(?:https?:\/\/)?api\.github\.com\/.*$/i
// —— GitHub 资源 CDN（签名直链 / 归档分流）——
// release-assets：新版 release 资产（302 目标，带 sig/jwt 签名）
// objects：旧版资产 CDN 主机名
// github-releases：更早的资产主机名
// codeload：源码归档（archive/tar.gz/zip）分流主机
// media：Git LFS 媒体文件
const exp8 = /^(?:https?:\/\/)?(?:release-assets|objects|github-releases)\.githubusercontent\.com\/.*$/i
const exp9 = /^(?:https?:\/\/)?codeload\.github\.com\/.*$/i
const exp10 = /^(?:https?:\/\/)?media\.githubusercontent\.com\/.*$/i

// 可作为「入口」由用户直接粘贴的链接
// —— release 资产稳定通道（2026-11-08）——
// github.com 的 releases/download 入口大范围 504（含全部镜像的回源），
// 唯一稳定的官方入口是 api.github.com（octet-stream 302 到 release-assets Azure blob）。
// 命中 release 下载路径时改走 api 解析 asset id，不再 fetch github.com。
const expReleaseDl = /^https?:\/\/github\.com\/([^\/]+)\/([^\/]+)\/releases\/(?:download\/([^\/?#]+)|latest\/download)\/([^\/?#]+)/i
const GH_TOKEN = typeof GITHUB_TOKEN !== 'undefined' ? GITHUB_TOKEN : ''

function ghHeaders(accept) {
    const h = {'User-Agent': 'tclient-mirror', 'Accept': accept}
    if (GH_TOKEN) h['Authorization'] = 'Bearer ' + GH_TOKEN
    return h
}

async function resolveRelease(req, path) {
    const m = path.search(expReleaseDl) === 0 ? path.match(expReleaseDl) : null
    if (!m) return null
    const [, owner, repo, tag, asset] = m
    const apiUrl = tag
        ? 'https://api.github.com/repos/' + owner + '/' + repo + '/releases/tags/' + tag
        : 'https://api.github.com/repos/' + owner + '/' + repo + '/releases/latest'
    const meta = await fetch(apiUrl, {headers: ghHeaders('application/vnd.github+json')})
    if (!meta.ok) return makeRes('github api ' + meta.status + ' resolving release', 502)
    const data = await meta.json()
    const found = (data.assets || []).find(a => a.name === asset)
    if (!found || !found.url) return makeRes('asset not found: ' + asset, 404)
    // octet-stream：302 到 release-assets 签名直链；manual 拿 location 后
    // 带客户端原始头（含 Range 断点续传）回源，避免 redirect 重发丢头。
    const first = await fetch(found.url, {headers: ghHeaders('application/octet-stream'), redirect: 'manual'})
    let target = first.status >= 300 && first.status < 400 ? first.headers.get('location') : null
    let res
    if (target) {
        const dlHdr = new Headers(req.headers)
        dlHdr.delete('host'); dlHdr.delete('referer'); dlHdr.delete('cookie')
        res = await fetch(target, {headers: dlHdr, redirect: 'follow'})
    } else {
        res = first
    }
    // fetch() 返回的 Response headers 不可变，复制进新 Response 再统一加 CORS
    const outHdr = new Headers(res.headers)
    outHdr.set('access-control-expose-headers', '*')
    outHdr.set('access-control-allow-origin', '*')
    outHdr.delete('content-security-policy')
    outHdr.delete('content-security-policy-report-only')
    return new Response(res.body, {status: res.status, headers: outHdr})
}

const ENTRY_EXPS = [exp1, exp2, exp3, exp4, exp5, exp6, exp7]
// 仅作为「跳转目标 / 直链」的 CDN 链接
const ASSET_EXPS = [exp8, exp9, exp10]

/**
 * @param {any} body
 * @param {number} status
 * @param {Object<string, string>} headers
 */
function makeRes(body, status = 200, headers = {}) {
    headers['access-control-allow-origin'] = '*'
    return new Response(body, {status, headers})
}


/**
 * @param {string} urlStr
 */
function newUrl(urlStr) {
    try {
        return new URL(urlStr)
    } catch (err) {
        return null
    }
}


addEventListener('fetch', e => {
    const ret = fetchHandler(e)
        .catch(err => makeRes('cfworker error:\n' + err.stack, 502))
    e.respondWith(ret)
})


function checkUrl(u) {
    for (let i of [...ENTRY_EXPS, ...ASSET_EXPS]) {
        if (u.search(i) === 0) {
            return true
        }
    }
    return false
}

// 只判断「入口」类链接（用于决定 302 是否重写为代理地址）
function isEntryUrl(u) {
    for (let i of ENTRY_EXPS) {
        if (u.search(i) === 0) {
            return true
        }
    }
    return false
}

/**
 * @param {FetchEvent} e
 */
async function fetchHandler(e) {
    const req = e.request
    const urlStr = req.url
    const urlObj = new URL(urlStr)
    let path = urlObj.searchParams.get('q')
    if (path) {
        return Response.redirect('https://' + urlObj.host + PREFIX + path, 301)
    }
    // cfworker 会把路径中的 `//` 合并成 `/`
    path = urlObj.href.substr(urlObj.origin.length + PREFIX.length).replace(/^https?:\/+/, 'https://')
    if (path.search(expReleaseDl) === 0) {
        return await resolveRelease(req, path)
    }
    if (path.search(exp7) === 0) {
        return httpHandler(req, path)
    } else if (path.search(exp1) === 0 || path.search(exp5) === 0 || path.search(exp6) === 0 || path.search(exp3) === 0 || path.search(exp4) === 0
        || path.search(exp8) === 0 || path.search(exp9) === 0 || path.search(exp10) === 0) {
        return httpHandler(req, path)
    } else if (path.search(exp2) === 0) {
        if (Config.jsdelivr) {
            const newUrl = path.replace('/blob/', '@').replace(/^(?:https?:\/\/)?github\.com/, 'https://cdn.jsdelivr.net/gh')
            return Response.redirect(newUrl, 302)
        } else {
            path = path.replace('/blob/', '/raw/')
            return httpHandler(req, path)
        }
    } else if (path.search(exp4) === 0) {
        const newUrl = path.replace(/(?<=com\/.+?\/.+?)\/(.+?\/)/, '@$1').replace(/^(?:https?:\/\/)?raw\.(?:githubusercontent|github)\.com/, 'https://cdn.jsdelivr.net/gh')
        return Response.redirect(newUrl, 302)
    } else {
        return fetch(ASSET_URL + path)
    }
}


/**
 * @param {Request} req
 * @param {string} pathname
 */
function httpHandler(req, pathname) {
    const reqHdrRaw = req.headers

    // preflight
    if (req.method === 'OPTIONS' &&
        reqHdrRaw.has('access-control-request-headers')
    ) {
        return new Response(null, PREFLIGHT_INIT)
    }

    const reqHdrNew = new Headers(reqHdrRaw)

    let urlStr = pathname
    let flag = !Boolean(whiteList.length)
    for (let i of whiteList) {
        if (urlStr.includes(i)) {
            flag = true
            break
        }
    }
    if (!flag) {
        return new Response("blocked", {status: 403})
    }
    if (urlStr.search(/^https?:\/\//) !== 0) {
        urlStr = 'https://' + urlStr
    }
    const urlObj = newUrl(urlStr)

    /** @type {RequestInit} */
    const reqInit = {
        method: req.method,
        headers: reqHdrNew,
        redirect: 'manual',
        body: req.body
    }
    return proxy(urlObj, reqInit)
}


/**
 *
 * @param {URL} urlObj
 * @param {RequestInit} reqInit
 */
async function proxy(urlObj, reqInit) {
    const res = await fetch(urlObj.href, reqInit)
    const resHdrOld = res.headers
    const resHdrNew = new Headers(resHdrOld)

    const status = res.status

    if (resHdrNew.has('location')) {
        let _location = resHdrNew.get('location')
        // 入口链接（github.com / api.github.com 等）仍按旧逻辑重写为代理地址；
        // 签名 CDN 跳转默认在 Worker 内部跟随，避免多一次往返、也避免签名暴露在地址栏。
        if (isEntryUrl(_location) || (Config.rewriteAssetRedirect && checkUrl(_location)))
            resHdrNew.set('location', PREFIX + _location)
        else {
            reqInit.redirect = 'follow'
            return proxy(newUrl(_location), reqInit)
        }
    }
    resHdrNew.set('access-control-expose-headers', '*')
    resHdrNew.set('access-control-allow-origin', '*')

    resHdrNew.delete('content-security-policy')
    resHdrNew.delete('content-security-policy-report-only')
    resHdrNew.delete('clear-site-data')

    return new Response(res.body, {
        status,
        headers: resHdrNew,
    })
}

