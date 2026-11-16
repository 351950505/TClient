// Compiler discovery and separate-TU launch plans for isolated offline fixtures.
'use strict';
const fs = require('node:fs');
const path = require('node:path');
const cp = require('node:child_process');

function discoverCompiler({ platform = process.platform, env = process.env } = {}) {
  if (platform !== 'win32') return { platform, cc: env.CC || 'cc', cxx: env.CXX || 'c++', env };
  const programFiles = env['ProgramFiles(x86)'] || 'C:/Program Files (x86)';
  const vswhere = path.join(programFiles, 'Microsoft Visual Studio/Installer/vswhere.exe');
  const installation = cp.spawnSync(vswhere, ['-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], {
    env, encoding: 'utf8', windowsHide: true, timeout: 30000,
  });
  if (installation.error || installation.status !== 0 || !installation.stdout?.trim()) {
    throw new Error(`MSVC Build Tools discovery via vswhere failed: ${installation.error?.message || installation.stderr?.trim() || 'no installation found'}`);
  }
  const directory = file => { try { return fs.statSync(file).isDirectory(); } catch { return false; } };
  const latest = (parent, valid) => fs.readdirSync(parent).filter(v => /^\d+(?:\.\d+)+$/.test(v) && valid(v))
    .sort((a, b) => a.localeCompare(b, undefined, { numeric: true })).pop();
  const tools = path.join(installation.stdout.trim(), 'VC/Tools/MSVC');
  const toolVersion = latest(tools, v => fs.existsSync(path.join(tools, v, 'bin/Hostx64/x64/cl.exe')) && directory(path.join(tools, v, 'include')) && directory(path.join(tools, v, 'lib/x64')));
  if (!toolVersion) throw new Error(`No complete MSVC x64 toolchain in ${tools}`);
  const msvc = path.join(tools, toolVersion);
  const kits = path.join(programFiles, 'Windows Kits/10');
  const version = latest(path.join(kits, 'Include'), v => /^10\./.test(v) &&
    ['ucrt', 'shared', 'um', 'winrt'].every(n => directory(path.join(kits, 'Include', v, n))) &&
    ['ucrt', 'um'].every(n => directory(path.join(kits, 'Lib', v, n, 'x64'))));
  if (!version) throw new Error(`No complete Windows SDK x64 installation in ${kits}`);
  const includes = [path.join(msvc, 'include'), ...['ucrt', 'shared', 'um', 'winrt'].map(n => path.join(kits, 'Include', version, n))];
  const libraries = [path.join(msvc, 'lib/x64'), ...['ucrt', 'um'].map(n => path.join(kits, 'Lib', version, n, 'x64'))];
  const compiler = path.join(msvc, 'bin/Hostx64/x64/cl.exe');
  return { platform, cc: compiler, cxx: compiler, env: { ...env, INCLUDE: includes.join(';'), LIB: libraries.join(';'), VSLANG: '1033' } };
}

// Defines belong to one source only: a simulated Windows provider must not
// enable windows.h in the real lyric_parser.cpp on Linux or macOS.
function compileCommands(toolchain, { out, binaryName, sources, includes = [], warnings = 3 }) {
  const windows = toolchain.platform === 'win32';
  const binary = path.join(out, binaryName + (windows ? '.exe' : ''));
  const objects = [];
  const commands = sources.map((source, index) => {
    const c = source.language === 'c';
    const object = path.join(out, `fixture-${index}.${windows ? 'obj' : 'o'}`);
    objects.push(object);
    const defines = source.defines || [];
    const args = windows
      ? ['/nologo', '/c', c ? '/TC' : '/TP', ...(c ? [] : ['/std:c++17', '/EHsc']), `/W${warnings}`, '/utf-8',
          ...includes.map(p => `/I${p}`), ...defines.map(d => `/D${d}`), source.file, `/Fo${object}`]
      : [...(c ? [] : ['-std=c++17', '-pthread']), '-c', ...includes.flatMap(p => ['-I', p]),
          ...defines.map(d => `-D${d}`), source.file, '-o', object];
    return { executable: c ? toolchain.cc : toolchain.cxx, args, env: toolchain.env };
  });
  commands.push({ executable: toolchain.cxx, args: windows ? ['/nologo', ...objects, `/Fe${binary}`] : ['-pthread', ...objects, '-o', binary], env: toolchain.env });
  return { binary, commands };
}

module.exports = { discoverCompiler, compileCommands };
