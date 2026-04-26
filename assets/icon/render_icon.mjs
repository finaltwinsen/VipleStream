// VipleStream icon renderer — rasterises viplestream_icon.svg into:
//
//   Windows:
//     moonlight-qt/app/moonlight.ico       (multi-size ICO)
//   Android:
//     moonlight-android/app/src/main/res/mipmap-<density>/ic_launcher.png
//     moonlight-android/app/src/main/res/mipmap-<density>/ic_launcher_foreground.png
//
// The foreground variant has the ink ground + folio dot stripped so
// it layers correctly on top of Android's adaptive-icon background
// drawable.
//
// Run from the repo root with sharp installed (no persistent install
// needed — npm install sharp in a scratch dir works).
//
//   node assets/icon/render_icon.mjs
//
// Re-run whenever viplestream_icon.svg changes.

import fs from 'node:fs/promises';
import path from 'node:path';
import sharp from 'sharp';

// REPO root: prefer $REPO env, then --repo=<path> arg, otherwise
// assume the script lives under <repo>/assets/icon/ and walk up
// two dirs.  fileURLToPath handles the Windows drive-letter quirk
// in import.meta.url.
import { fileURLToPath } from 'node:url';
function resolveRepo() {
    if (process.env.REPO) return process.env.REPO;
    const arg = process.argv.find(a => a.startsWith('--repo='));
    if (arg) return arg.slice('--repo='.length);
    return path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
}
const REPO = resolveRepo();
const SRC  = path.join(REPO, 'assets/icon/viplestream_icon.svg');

// Load once.
const srcSvg = await fs.readFile(SRC, 'utf8');

// For the adaptive-icon foreground we want just the V + play triangle
// on a transparent canvas — the ink ground + orange stripe move into
// the adaptive-icon background drawable. Strip the ink rect, orange
// stripe, and folio dot from the source SVG.
const fgSvg = srcSvg
    .replace(/<rect width="512" height="512" fill="#0D0F0B"\/>/, '')
    .replace(/<rect x="0" y="180" width="512" height="160" fill="#FF7A2E"\/>/, '')
    .replace(/<circle cx="470" cy="46" r="8" fill="#FF7A2E"\/>/, '')
    // Android adaptive-icon foreground lives inside a 108dp canvas
    // but content should sit inside the centre 66dp safe zone. Add
    // an outer transparent frame so the V ends up roughly 66/108 of
    // the way across the canvas.
    .replace(
        /viewBox="0 0 512 512"/,
        'viewBox="-128 -128 768 768"'
    );

async function png(svg, size, dst) {
    await fs.mkdir(path.dirname(dst), { recursive: true });
    await sharp(Buffer.from(svg)).resize(size, size).png().toFile(dst);
    console.log(`  ${size}px  →  ${path.relative(REPO, dst)}`);
}

// ─── Windows .ico ──────────────────────────────────────────────────
// sharp doesn't write .ico directly, so we render each size to PNG
// and concatenate them into a minimal ICO file by hand.
console.log('Windows ICO:');
const icoSizes = [16, 24, 32, 48, 64, 128, 256];
const pngs = await Promise.all(icoSizes.map(async s => {
    const buf = await sharp(Buffer.from(srcSvg)).resize(s, s).png().toBuffer();
    return { size: s, buf };
}));

// Build ICONDIR (6 bytes) + 16 bytes per entry + PNG payloads.
const headerSize = 6 + 16 * pngs.length;
let offset = headerSize;
const dirEntries = [];
for (const { size, buf } of pngs) {
    const e = Buffer.alloc(16);
    e.writeUInt8(size === 256 ? 0 : size, 0);      // width (0 == 256)
    e.writeUInt8(size === 256 ? 0 : size, 1);      // height
    e.writeUInt8(0, 2);                            // palette (0 for PNG)
    e.writeUInt8(0, 3);                            // reserved
    e.writeUInt16LE(1, 4);                         // planes
    e.writeUInt16LE(32, 6);                        // bit depth
    e.writeUInt32LE(buf.length, 8);                // size in bytes
    e.writeUInt32LE(offset, 12);                   // offset
    offset += buf.length;
    dirEntries.push(e);
}
const header = Buffer.alloc(6);
header.writeUInt16LE(0, 0);                        // reserved
header.writeUInt16LE(1, 2);                        // image type (ICO=1)
header.writeUInt16LE(pngs.length, 4);              // image count
const ico = Buffer.concat([header, ...dirEntries, ...pngs.map(p => p.buf)]);

const icoOut = path.join(REPO, 'moonlight-qt/app/moonlight.ico');
await fs.writeFile(icoOut, ico);
console.log(`  .ico    →  ${path.relative(REPO, icoOut)}  (${pngs.length} sizes, ${ico.length} B)`);

// ─── Android mipmap PNGs ────────────────────────────────────────────
console.log('Android mipmap (launcher):');
const ANDROID = path.join(REPO, 'moonlight-android/app/src/main/res');
// Launcher icon (legacy + pre-API-26 fallback): full masked icon
for (const [density, size] of [
    ['mdpi', 48], ['hdpi', 72], ['xhdpi', 96],
    ['xxhdpi', 144], ['xxxhdpi', 192],
]) {
    await png(srcSvg, size, path.join(ANDROID, `mipmap-${density}`, 'ic_launcher.png'));
}

console.log('Android mipmap (adaptive foreground):');
// Adaptive-icon foreground: 108dp canvas with content in 66dp safe area.
// We fed the renderer a viewBox that already includes the outer
// transparent frame, so raster at 108/96 of the launcher size.
for (const [density, size] of [
    ['mdpi', 108], ['hdpi', 162], ['xhdpi', 216],
    ['xxhdpi', 324], ['xxxhdpi', 432],
]) {
    await png(fgSvg, size, path.join(ANDROID, `mipmap-${density}`, 'ic_launcher_foreground.png'));
}

// ─── Discord Rich Presence assets ──────────────────────────────────
// 1024×1024 PNG for upload to Discord developer portal.
// Upload as BOTH:
//   1) Application Icon (General Information → App Icon)
//   2) Rich Presence Art Asset named "icon" (Rich Presence → Art Assets)
// The second is what `largeImageKey = "icon"` in
// richpresencemanager.cpp references.
console.log('Discord asset:');
const DIST = path.join(REPO, 'assets/icon/dist');
await png(srcSvg, 1024, path.join(DIST, 'discord_icon_1024.png'));

console.log('\nDone. Remember to rebuild moonlight-qt (RC_ICONS embeds the .ico)');
console.log('and the Android APK so the new mipmap pngs ship.');
console.log('For Discord: upload assets/icon/dist/discord_icon_1024.png as BOTH');
console.log('the App Icon AND the Rich Presence asset "icon".');
