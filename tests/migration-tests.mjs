// SPDX-License-Identifier: Apache-2.0
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { parseTargets, extractProfileJson, validateProfile, decryptPassword, hapInfo, loadKit, signingInvocation } from '../deployment/phone-tools.mjs';
const repo = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const require = createRequire(path.join(repo, '.tools/deveco-cli/node_modules/@deveco/deveco-cli/package.json'));
const Zip = require('adm-zip');
function scratch(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'hscrcpy-migration-test-'));
  t.after(() => {
    const base = path.resolve(os.tmpdir()) + path.sep;
    if (!path.resolve(directory).startsWith(base) || !path.basename(directory).startsWith('hscrcpy-migration-test-')) {
      throw new Error('Unsafe test cleanup path.');
    }
    fs.rmSync(directory, { recursive: true });
  });
  return directory;
}
const udid = 'AB'.repeat(32);
function profile() { return {
  'bundle-info': {'bundle-name': 'com.hscrcpy.audiocapture'},
  validity: {'not-after': 2000000000},
  'debug-info': {'device-ids': [udid]}
}; }
test('HDC detailed states distinguish offline, connected and unrelated output', () => {
  assert.deepEqual(parseTargets('[Empty]\r\n'), []);
  const rows = parseTargets('noise\nSERIAL USB Offline unknown hdc\r\n192.168.1.2:5555 TCP Connected host hdc\nbad&name USB Connected host hdc');
  assert.equal(rows.length, 2);
  assert.equal(rows[0].status, 'Offline');
  assert.equal(rows[1].serial, '192.168.1.2:5555');
});
test('Profile parser handles binary prefix, nested JSON and braces inside strings', () => {
  const data = profile(); data.note = 'a{b}"quoted';
  assert.deepEqual(extractProfileJson(Buffer.from('\u0000\u0001header' + JSON.stringify(data) + 'trailer')), data);
  assert.throws(() => extractProfileJson(Buffer.from('{"x":')), /incomplete/);
  assert.throws(() => extractProfileJson(Buffer.alloc(6 * 1024 * 1024)), /large/);
});
test('A valid signature profile must match bundle, device and expiry', () => {
  assert.equal(validateProfile(profile(), {udid}, 1900000000000), '2033-05-18T03:33:20.000Z');
  const wrongBundle = profile(); wrongBundle['bundle-info']['bundle-name'] = 'other.app';
  assert.throws(() => validateProfile(wrongBundle, {udid}), /another/);
  assert.throws(() => validateProfile(profile(), {udid: 'CD'.repeat(32)}), /not included/);
  assert.throws(() => validateProfile(profile(), {udid}, 2100000000000), /expired/);
  const noExpiry = profile(); delete noExpiry.validity;
  assert.throws(() => validateProfile(noExpiry, {udid}), /expiry/);
});
function encrypt(key, plain, iv) {
  const cipher = crypto.createCipheriv('aes-128-gcm', key, iv);
  const content = Buffer.concat([cipher.update(plain), cipher.final(), cipher.getAuthTag()]);
  const head = Buffer.alloc(4); head.writeUInt32BE(content.length);
  return Buffer.concat([head, iv, content]);
}
test('Vendor password format authenticates ciphertext and rejects ambiguous material', t => {
  const directory = scratch(t), material = path.join(directory, 'material');
  const ac = Buffer.from('00112233445566778899aabbccddeeff', 'hex');
  const pieces = [Buffer.alloc(16, 11), Buffer.alloc(16, 23), Buffer.alloc(16, 47)];
  const constant = Buffer.from([49,243,9,115,214,175,91,184,211,190,177,88,101,131,192,119]);
  const xor = Buffer.alloc(16);
  for (let i = 0; i < 16; i++) xor[i] = pieces[0][i] ^ pieces[1][i] ^ pieces[2][i] ^ constant[i];
  const wrapping = crypto.pbkdf2Sync(Buffer.from(xor.toString('utf8')), ac, 10000, 16, 'sha256');
  const master = Buffer.alloc(16, 83);
  for (const [relative, value] of [
    ['ac', ac], ['fd/0', pieces[0]], ['fd/1', pieces[1]], ['fd/2', pieces[2]],
    ['ce', encrypt(wrapping, master, Buffer.alloc(12, 61))]
  ]) {
    fs.mkdirSync(path.join(material, relative), {recursive: true});
    fs.writeFileSync(path.join(material, relative, 'fixture'), value);
  }
  const secret = 'test-only password with spaces';
  const encrypted = encrypt(master, Buffer.from(secret), Buffer.alloc(12, 71));
  assert.equal(decryptPassword(encrypted.toString('hex'), path.join(directory, 'fixture.p12')), secret);
  const changed = Buffer.from(encrypted); changed[20] ^= 1;
  assert.throws(() => decryptPassword(changed.toString('hex'), path.join(directory, 'fixture.p12')));
  const extra = Buffer.concat([encrypted, Buffer.from([0])]);
  assert.throws(() => decryptPassword(extra.toString('hex'), path.join(directory, 'fixture.p12')), /Invalid encrypted/);
  fs.writeFileSync(path.join(material, 'ac', 'unexpected'), 'unrelated');
  assert.throws(() => decryptPassword(encrypted.toString('hex'), path.join(directory, 'fixture.p12')), /Invalid signing/);
});
test('HAP admission rejects another bundle, absent native library and invalid SDK value', t => {
  const directory = scratch(t);
  function write(name, bundle, api, native = true) {
    const zip = new Zip();
    zip.addFile('module.json', Buffer.from(JSON.stringify({app: {bundleName:bundle,minAPIVersion:api,versionName:'1.0.0'}})));
    if (native) zip.addFile('libs/arm64-v8a/libaudio_capture.so', Buffer.from('test fixture'));
    const filename = path.join(directory, name); zip.writeZip(filename); return filename;
  }
  const kit = {Zip};
  assert.equal(hapInfo(kit, write('good.hap','com.hscrcpy.audiocapture',50000012)).minApi, 12);
  assert.throws(() => hapInfo(kit, write('wrong.hap','other.app',12)), /another application/);
  assert.throws(() => hapInfo(kit, write('missing.hap','com.hscrcpy.audiocapture',12,false)), /ARM64/);
  assert.throws(() => hapInfo(kit, write('invalid.hap','com.hscrcpy.audiocapture',0)), /Invalid minimum/);
});
test('Relocated kit contains only the unsigned phone payload and no personal signing config', {skip: !process.env.HSCRCPY_KIT_TEST_ROOT}, () => {
  const root = process.env.HSCRCPY_KIT_TEST_ROOT;
  const kit = loadKit(root);
  assert.equal(hapInfo(kit, kit.unsignedHap).minApi, 12);
  assert.equal(crypto.createHash('sha256').update(fs.readFileSync(kit.unsignedHap)).digest('hex'), kit.layout.phoneSha256);
  const manifest = JSON.parse(fs.readFileSync(path.join(root, 'files.sha256.json'), 'utf8'));
  const haps = manifest.filter(f => f.path.endsWith('.hap')).map(f => f.path);
  assert.deepEqual(haps, ['phone/audio-capture-unsigned.hap']);
  assert.equal(manifest.some(f => /(^|\/)(\.ohos|TraceLogData|private|\.local)(\/|$)/i.test(f.path)), false);
  assert.equal(manifest.some(f => /\.(p12|pfx|p7b)$/i.test(f.path)), false);
  const cfg = kit.json5.parse(fs.readFileSync(path.join(kit.project,'audio-companion/build-profile.example.json5'),'utf8'));
  assert.deepEqual(cfg.app.signingConfigs, []);
});

test('Signing passwords use private input rather than process arguments', () => {
  const kit = {jar:'C:/工具/sign tool.jar', signerBridge:'C:/工具/SensitiveSigner.java', unsignedHap:'C:/包/unsigned.hap'};
  const material = {keyAlias:'alias',certpath:'cert.cer',profile:'profile.p7b',storeFile:'store.p12'};
  const first = 'private secret\nwith spaces', second = '另一个口令';
  const call = signingInvocation(kit, material, 12, 'out.hap', first, second);
  assert.equal(call.args.some(arg => arg.includes(first) || arg.includes(second)), false);
  assert.equal(call.args.includes('-keyPwd'), false);
  assert.equal(call.args.includes('-keystorePwd'), false);
  const lines = call.input.toString('utf8').trim().split('\n');
  assert.equal(Buffer.from(lines[0], 'base64').toString('utf8'), first);
  assert.equal(Buffer.from(lines[1], 'base64').toString('utf8'), second);
  call.input.fill(0);
});
