'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const cp = require('node:child_process');
const assert = require('node:assert/strict');

const root = path.resolve(__dirname, '..');
const out = fs.mkdtempSync(path.join(os.tmpdir(), 'video-frame-resize-'));
const vcvars = 'C:/VS2026/BuildTools/VC/Auxiliary/Build/vcvars64.bat';
const ffmpeg = path.join(root, 'ddnet-libs/ffmpeg');
const ffmpegLib = path.join(ffmpeg, 'windows/lib64');
const exe = path.join(out, 'video_frame_resize_test.exe');
const source = path.join(__dirname, 'video_frame_resize_test.cpp');

assert(fs.existsSync(vcvars), 'VS2026 vcvars64.bat not found');
assert(fs.existsSync(path.join(ffmpegLib, 'swscale.lib')) && fs.existsSync(path.join(ffmpegLib, 'avutil.lib')), 'FFmpeg import libraries missing');

const windowsPath = value => path.resolve(value).replaceAll('/', '\\');
const command = `call ${windowsPath(vcvars)} >nul && cl.exe /nologo /std:c++20 /EHsc /W4 /utf-8 /I${windowsPath(path.join(root, 'src'))} /I${windowsPath(path.join(ffmpeg, 'include'))} ${windowsPath(source)} /link /LIBPATH:${windowsPath(ffmpegLib)} swscale.lib avutil.lib /OUT:${windowsPath(exe)}`;
const build = cp.spawnSync('cmd.exe', ['/d', '/c', command], { cwd: out, encoding: 'utf8', timeout: 180000, maxBuffer: 4 * 1024 * 1024 });
if(build.error)
	throw build.error;
assert.equal(build.status, 0, `C++ video-frame fixture failed to build:\n${build.stdout || ''}\n${build.stderr || ''}`);

const run = cp.spawnSync(exe, [], { cwd: out, encoding: 'utf8', timeout: 30000, maxBuffer: 2 * 1024 * 1024, env: { ...process.env, PATH: [ffmpegLib, path.join(root, 'build-vs2026'), process.env.PATH].join(path.delimiter) } });
if(run.error)
	throw run.error;
assert.equal(run.status, 0, `${run.stdout || ''}\n${run.stderr || ''}`);
process.stdout.write(run.stdout || '');
console.log(`Evidence: ${out}`);
