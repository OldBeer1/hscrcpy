// SPDX-License-Identifier: Apache-2.0
// Password-material decoding follows @deveco/deveco-cli 1.3.0-stable (MIT).
// The original MIT notice is distributed in licenses/DevEco-CLI-LICENSE.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import { spawnSync, spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { pathToFileURL, fileURLToPath } from 'node:url';

export const BUNDLE = 'com.hscrcpy.audiocapture';
function fail(message) { throw new Error(message); }
function under(root, relative) {
  const resolved = path.resolve(root, relative);
  const rel = path.relative(path.resolve(root), resolved);
  if (path.isAbsolute(rel) || rel === '..' || rel.startsWith('..' + path.sep)) fail('Path escapes package directory.');
  return resolved;
}
export function loadKit(root) {
  root = path.resolve(root);
  const layout = JSON.parse(fs.readFileSync(under(root, 'package-layout.json'), 'utf8'));
  const kit = { root, layout };
  for (const name of ['clt', 'node', 'cli', 'javaHome', 'unsignedHap', 'project']) kit[name] = under(root, layout[name]);
  kit.hdc = path.join(kit.clt, 'sdk/default/openharmony/toolchains/hdc.exe');
  kit.java = path.join(kit.javaHome, 'bin/java.exe');
  kit.signerBridge = under(root, kit.layout.signerBridge || 'deployment/SensitiveSigner.java');
  kit.jar = path.join(kit.clt, 'sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar');
  const require = createRequire(kit.cli);
  kit.json5 = require('json5');
  kit.Zip = require('adm-zip');
  kit.env = { ...process.env, DEVECO_CLI_CLT_PATH: kit.clt, JAVA_HOME: kit.javaHome,
    DEVECO_CLI_DISABLE_UPDATE: 'all' };
  delete kit.env.DEVECO_CLI_STUDIO_PATH;
  delete kit.env.DEVECO_HOME;
  delete kit.env.DEVECO_PATH;
  // This affects child processes only. Never write global environment settings.
  kit.env.PATH = [path.dirname(kit.node), path.dirname(kit.hdc), path.dirname(kit.java),
    process.env.PATH || ''].join(path.delimiter);
  return kit;
}
function execute(kit, exe, args, options = {}) {
  const result = spawnSync(exe, args, { encoding: 'utf8', windowsHide: true,
    timeout: options.timeout || 30000, maxBuffer: 8 * 1024 * 1024, input: options.input,
    cwd: options.cwd || kit.root, env: kit.env, stdio: options.inherit ? 'inherit' : 'pipe' });
  if (result.error) fail('Cannot run tool: ' + result.error.message);
  if (result.status !== 0) {
    let detail = String(result.stderr || result.stdout || '').slice(-3500);
    for (const secret of options.secrets || []) if (secret) detail = detail.split(secret).join('[redacted]');
    fail('Tool failed (exit ' + result.status + '). ' + detail);
  }
  return String(result.stdout || '');
}
function hdc(kit, args, timeout = 20000) { return execute(kit, kit.hdc, args, { timeout }); }
export function parseTargets(text) {
  const targets = [];
  for (const line of text.split(/\r?\n/)) {
    const fields = line.trim().split(/\s+/);
    if (fields.length >= 3 && /^(USB|TCP|UART)$/i.test(fields[1]) &&
      /^[A-Za-z0-9_.:-]+$/.test(fields[0])) {
      targets.push({ serial: fields[0], transport: fields[1], status: fields[2] });
    }
  }
  return targets;
}
function oneDevice(kit) {
  const targets = parseTargets(hdc(kit, ['list', 'targets', '-v']));
  const connected = targets.filter(t => /^Connected$/i.test(t.status));
  if (connected.length > 1) fail('检测到多部手机。请只连接本次要配置的手机，再重试。');
  if (!connected.length) fail('没有已连接的手机。请解锁、开启 USB 调试并允许连接；仍不在线时使用“恢复连接”。');
  const device = connected[0];
  device.api = Number(hdc(kit, ['-t', device.serial, 'shell', 'param', 'get', 'const.ohos.apiversion']).trim());
  const udidText = hdc(kit, ['-t', device.serial, 'shell', 'bm', 'get', '-u']);
  const matches = udidText.match(/[A-Fa-f0-9]{64,128}/g) || [];
  if (matches.length !== 1) fail('Cannot obtain the phone UDID. Check developer mode and USB authorization.');
  device.udid = matches[0].toUpperCase();
  return device;
}
function installed(kit, device) {
  const output = hdc(kit, ['-t', device.serial, 'shell', 'bm', 'dump', '-n', BUNDLE]);
  return output.includes('"bundleName": "' + BUNDLE + '"') ||
    new RegExp('"bundleName"\\s*:\\s*"' + BUNDLE.replaceAll('.', '\\.') + '"').test(output);
}
export function extractProfileJson(bytes) {
  if (bytes.length > 5 * 1024 * 1024) fail('Profile is too large.');
  const text = bytes.toString('utf8');
  const start = text.indexOf('{');
  if (start < 0) fail('Profile has no JSON payload.');
  let depth = 0, quoted = false, escaped = false;
  for (let i = start; i < text.length; i++) {
    const ch = text[i];
    if (quoted) {
      if (escaped) escaped = false;
      else if (ch === '\\') escaped = true;
      else if (ch === '"') quoted = false;
    } else if (ch === '"') quoted = true;
    else if (ch === '{') depth++;
    else if (ch === '}' && --depth === 0) return JSON.parse(text.slice(start, i + 1));
  }
  fail('Profile JSON is incomplete.');
}
export function validateProfile(profile, device, now = Date.now()) {
  if (profile['bundle-info']?.['bundle-name'] !== BUNDLE) fail('Signing profile belongs to another application.');
  const expiry = profile.validity?.['not-after'];
  if (typeof expiry !== 'number' || expiry * 1000 <= now) fail('Signing profile has expired or has no valid expiry.');
  const ids = profile['debug-info']?.['device-ids'];
  if (!Array.isArray(ids) || !ids.some(id => typeof id === 'string' && id.toUpperCase() === device.udid)) {
    fail('This phone is not included in the debug profile. Generate a new profile for this phone.');
  }
  return new Date(expiry * 1000).toISOString();
}
function apiNumber(value) {
  const n = Number(value);
  if (!Number.isSafeInteger(n) || n <= 0) fail('Invalid minimum SDK version in HAP.');
  return n >= 1000000 ? n % 100000 : n;
}
export function hapInfo(kit, filename) {
  const zip = new kit.Zip(filename);
  const entry = zip.getEntry('module.json');
  if (!entry || entry.header.size > 1024 * 1024) fail('HAP module.json is absent or too large.');
  const module = JSON.parse(entry.getData().toString('utf8'));
  if (module.app?.bundleName !== BUNDLE) fail('HAP belongs to another application.');
  if (!zip.getEntry('libs/arm64-v8a/libaudio_capture.so')) fail('HAP is missing the ARM64 audio library.');
  return { bundle: module.app.bundleName, minApi: apiNumber(module.app.minAPIVersion), version: module.app.versionName };
}
function validateApi(info, device) {
  if (!Number.isInteger(device.api) || device.api < info.minApi) fail('Phone API is lower than the HAP minimum API.');
}
function readSingle(directory) {
  const files = fs.readdirSync(directory).filter(name => name !== '.DS_Store');
  if (files.length !== 1 || !fs.statSync(path.join(directory, files[0])).isFile()) fail('Invalid signing password material.');
  return fs.readFileSync(path.join(directory, files[0]));
}
function decryptBlock(key, bytes) {
  if (bytes.length < 32 || bytes.readUInt32BE(0) !== bytes.length - 16) fail('Invalid encrypted signing password.');
  const decipher = crypto.createDecipheriv('aes-128-gcm', key, bytes.subarray(4, 16));
  decipher.setAuthTag(bytes.subarray(bytes.length - 16));
  return Buffer.concat([decipher.update(bytes.subarray(16, bytes.length - 16)), decipher.final()]);
}
export function decryptPassword(encoded, storeFile) {
  const material = path.join(path.dirname(storeFile), 'material');
  if (!/^[a-f0-9]+$/i.test(encoded || '') || !fs.existsSync(material)) fail('Cannot decode signing password material.');
  const ac = readSingle(path.join(material, 'ac'));
  const pieces = [0, 1, 2].map(i => readSingle(path.join(material, 'fd', String(i))));
  pieces.push(Buffer.from([49,243,9,115,214,175,91,184,211,190,177,88,101,131,192,119]));
  if (ac.length !== 16 || pieces.some(p => p.length !== 16)) fail('Invalid signing key material length.');
  const xor = Buffer.alloc(16);
  for (let i = 0; i < 16; i++) for (const part of pieces) xor[i] ^= part[i];
  const key = crypto.pbkdf2Sync(Buffer.from(xor.toString('utf8'), 'utf8'), ac, 10000, 16, 'sha256');
  const master = decryptBlock(key, readSingle(path.join(material, 'ce')));
  if (master.length !== 16) fail('Invalid signing master key.');
  return decryptBlock(master, Buffer.from(encoded, 'hex')).toString('utf8');
}
function copyNewTree(source, destination) {
  fs.mkdirSync(destination, { recursive: true });
  for (const entry of fs.readdirSync(source, { withFileTypes: true })) {
    if (entry.isSymbolicLink()) fail('Project template contains a symbolic link.');
    const from = path.join(source, entry.name), to = path.join(destination, entry.name);
    if (entry.isDirectory()) copyNewTree(from, to);
    else if (entry.isFile()) fs.copyFileSync(from, to, fs.constants.COPYFILE_EXCL);
  }
}
function workspace(kit, device) {
  const digest = crypto.createHash('sha256').update(device.serial + kit.layout.phoneSha256).digest('hex').slice(0, 20);
  const base = process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData/Local');
  const directory = path.join(base, 'hscrcpy/phone-projects', digest);
  if (!fs.existsSync(directory)) {
    copyNewTree(kit.project, directory);
    const app = path.join(directory, 'audio-companion');
    const profile = path.join(app, 'build-profile.json5');
    fs.copyFileSync(path.join(app, 'build-profile.example.json5'), profile, fs.constants.COPYFILE_EXCL);
    fs.writeFileSync(path.join(directory, 'device.json'), JSON.stringify({
      bundle: BUNDLE, serial: device.serial, api: device.api, udid: device.udid
    }, null, 2), { mode: 0o600 });
  }
  return path.join(directory, 'audio-companion');
}
function runCli(kit, args, cwd, inherit = true) {
  return execute(kit, kit.node, [kit.cli, ...args], { cwd, inherit, timeout: 15 * 60 * 1000 });
}
function verifiedProfile(kit, hap, device, directory) {
  const cert = path.join(directory, 'verified-chain.cer');
  const profile = path.join(directory, 'verified-profile.p7b');
  execute(kit, kit.java, ['-jar', kit.jar, 'verify-app', '-inFile', hap,
    '-outCertChain', cert, '-outProfile', profile], { timeout: 90000 });
  return validateProfile(extractProfileJson(fs.readFileSync(profile)), device);
}
export function signingInvocation(kit, material, minApi, output, keyPassword, storePassword) {
  return {
    args: ['--class-path', kit.jar, kit.signerBridge, kit.jar, 'sign-app', '-mode', 'localSign',
      '-keyAlias', material.keyAlias, '-signAlg', material.signAlg || 'SHA256withECDSA',
      '-appCertFile', material.certpath, '-profileFile', material.profile,
      '-keystoreFile', material.storeFile, '-inFile', kit.unsignedHap, '-outFile', output,
      '-compatibleVersion', String(minApi), '-signCode', '1'],
    input: Buffer.from(Buffer.from(keyPassword, 'utf8').toString('base64') + '\n' +
      Buffer.from(storePassword, 'utf8').toString('base64') + '\n', 'utf8')
  };
}
export function signWithConfig(kit, device, configFile, output) {
  const config = kit.json5.parse(fs.readFileSync(configFile, 'utf8'));
  const product = config.app?.products?.find(p => p.name === 'default');
  const material = config.app?.signingConfigs?.find(s => s.name === product?.signingConfig)?.material;
  if (!material) fail('The default product has no signing configuration.');
  for (const field of ['certpath', 'profile', 'storeFile']) if (!fs.statSync(material[field]).isFile()) fail('Signing material is missing.');
  const expiry = validateProfile(extractProfileJson(fs.readFileSync(material.profile)), device);
  const info = hapInfo(kit, kit.unsignedHap);
  validateApi(info, device);
  if (crypto.createHash('sha256').update(fs.readFileSync(kit.unsignedHap)).digest('hex') !== kit.layout.phoneSha256) {
    fail('Unsigned HAP checksum does not match this toolkit.');
  }
  const keyPassword = decryptPassword(material.keyPassword, material.storeFile);
  const storePassword = decryptPassword(material.storePassword, material.storeFile);
  fs.mkdirSync(path.dirname(output), { recursive: true });
  if (fs.existsSync(output)) fail('Output already exists; existing signed package is preserved.');
  const invocation = signingInvocation(kit, material, info.minApi, output, keyPassword, storePassword);
  try {
    execute(kit, kit.java, invocation.args,
      { timeout: 120000, input: invocation.input, secrets: [keyPassword, storePassword] });
  } finally { invocation.input.fill(0); }
  verifiedProfile(kit, output, device, path.dirname(output));
  console.log('Signed package verified. Profile expires: ' + expiry);
  return output;
}
function install(kit, device, signedHap) {
  validateApi(hapInfo(kit, signedHap), device);
  const verifyDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'hscrcpy-verify-'));
  verifiedProfile(kit, signedHap, device, verifyDirectory);
  const output = hdc(kit, ['-t', device.serial, 'install', '-r', path.resolve(signedHap)], 90000);
  if (!/install bundle successfully/i.test(output)) {
    fail('Installation was not confirmed. Existing applications were not uninstalled. ' + output.slice(-1800));
  }
  if (!installed(kit, device)) fail('Phone did not report the expected installed bundle.');
  console.log('Audio companion installed and verified: ' + BUNDLE);
}
function parseOptions(args) {
  const options = {};
  for (let i = 0; i < args.length; i++) {
    const key = args[i];
    if (['--sign-only', '--no-login', '--renew'].includes(key)) options[key] = true;
    else if (['--root', '--config', '--out', '--hap'].includes(key) && i + 1 < args.length) options[key] = args[++i];
    else fail('Unknown option: ' + key);
  }
  return options;
}
export async function main(args = process.argv.slice(2)) {
  const action = args.shift() || 'check', options = parseOptions(args);
  const kit = loadKit(options['--root'] || path.dirname(path.dirname(fileURLToPath(import.meta.url))));
  if (action === 'recover') {
    console.log('Restarting the local HDC service. Existing debug connections will be rebuilt.');
    hdc(kit, ['start', '-r']);
    const deadline = Date.now() + 20000;
    let status = '';
    do {
      status = hdc(kit, ['list', 'targets', '-v']);
      if (parseTargets(status).some(target => /^Connected$/i.test(target.status))) {
        console.log(status); return;
      }
      await new Promise(resolve => setTimeout(resolve, 1000));
    } while (Date.now() < deadline);
    fail('调试服务已重启，但手机仍未连接。请检查数据线、解锁状态和手机上的调试授权。');
  }
  const device = oneDevice(kit);
  validateApi(hapInfo(kit, kit.unsignedHap), device);
  const present = installed(kit, device);
  if (action === 'device-info') {
    console.log(JSON.stringify({ serial: device.serial, api: device.api, installed: present })); return;
  }
  console.log('Phone connected (ending ' + device.serial.slice(-4) + '), API ' + device.api +
    '. Audio companion: ' + (present ? 'installed' : 'not installed'));
  if (action === 'check') return;
  if (action === 'install') {
    if (!options['--hap']) fail('Choose a signed HAP first.');
    install(kit, device, options['--hap']); return;
  }
  if (action === 'sign-existing') {
    if (!options['--config'] || !options['--out']) fail('Provide --config and --out.');
    signWithConfig(kit, device, path.resolve(options['--config']), path.resolve(options['--out'])); return;
  }
  if (action !== 'setup') fail('Unknown action: ' + action);
  if (present && !options['--sign-only'] && !options['--renew']) {
    console.log('手机端已安装，可直接使用；本次没有替换手机上的应用。'); return;
  }
  const project = workspace(kit, device);
  const status = runCli(kit, ['auth', 'status'], project, false);
  if (/not logged in/i.test(status)) {
    if (options['--no-login']) fail('Huawei account authorization is required. Run Setup interactively.');
    console.log('请在浏览器完成华为开发者账号登录和授权。');
    runCli(kit, ['auth', 'login'], project);
  }
  runCli(kit, ['signature', 'generate', '--product', 'default'], project);
  const outputDirectory = path.join(path.dirname(project), 'signed', String(Date.now()));
  const signedHap = signWithConfig(kit, device, path.join(project, 'build-profile.json5'),
    path.join(outputDirectory, 'audio-capture-signed.hap'));
  if (!options['--sign-only']) install(kit, device, signedHap);
  console.log('个人签名包（仅供已授权设备）：' + signedHap);
  console.log('签名材料和账号状态属于私密资料，不要放进公开运行包或 GitHub。');
}
if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  main().catch(error => { console.error('手机准备未完成：' + error.message); process.exitCode = 1; });
}
