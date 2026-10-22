// GENERATED from pdf2epub.mjs by build-browser.mjs. Edit the .mjs file instead.
(function () {
"use strict";
// pdf2epub.mjs: browser (and Node) port of pdf2epub.py.
//
// Turns a PDF into a very plain EPUB for CrossPoint e-readers: drops running headers,
// footers, page numbers and watermarks, rejoins paragraphs and hyphenated words, finds
// headings, code and list items, and writes a minimal EPUB 3 with an NCX.
//
// The PDF is read with pdf.js and the EPUB is zipped with JSZip; both are passed in, so
// this file has no imports and works the same in a browser page and in Node:
//
//   const { epub, title, summary } = await convertPdf(pdfjsLib, JSZip, pdfBytes, { title: "..." });
//
// Like the Python tool it also rebuilds ruled tables cell by cell and keeps images, turned
// gray and shrunk to fit the 480x800 screen.

const VERSION = "1.0.0-js";

const SCREEN_W = 480, SCREEN_H = 800;

const LIGATURES = { "ﬀ": "ff", "ﬁ": "fi", "ﬂ": "fl", "ﬃ": "ffi", "ﬄ": "ffl", "ﬅ": "st", "ﬆ": "st" };
const ZERO_WIDTH_RE = /[​‌‍⁠﻿]/g;
const XML_BAD_RE = /[\x00-\x08\x0b\x0c\x0e-\x1f￾￿]/g;

// Python's \w and [^\W\d_] for Unicode text.
const W = "[\\p{L}\\p{N}\\p{M}_]";
const NW = "[^\\p{L}\\p{N}\\p{M}_]";
const L = "\\p{L}";
const ROMAN = "[ivxlcdm]+";

const PAGE_NUM_RE = new RegExp(
  `^(?:${NW}|_)*(?:page|p\\.|pg\\.?|seite|página|pagina)?\\s*(?:\\d{1,4}|${ROMAN})` +
  `(?:\\s*(?:of|/|de|von)\\s*\\d{1,4})?(?:${NW}|_)*$`, "iu");
const CHAPTER_RE = /^(chapter|chap\.|part|book|section|prologue|epilogue|preface|foreword|introduction|afterword|appendix|contents|table of contents|acknowledg(e)?ments)(?![\p{L}\p{N}_])/iu;
const COMPOUND_PREFIXES = new Set(["self", "half", "ill", "well", "all", "non", "quasi", "twenty", "thirty",
  "forty", "fifty", "sixty", "seventy", "eighty", "ninety", "great", "step"]);
const VOWEL_PREFIXES = new Set(["co", "re", "pre", "de", "anti", "semi", "multi"]);
const CHAPTER_NUM_RE = /^(chapter|chap\.|part|book)\s+([0-9]+|[ivxlc]+|one|two|three|four|five|six|seven|eight|nine|ten)(?![\p{L}\p{N}_])/iu;
const BACK_MATTER_RE = /^(appendix|index|glossary|bibliography|references|notes|about the author|afterword|epilogue|colophon|acknowledg)/i;
const HYPHENS = "-‐‑­";
const DASHES = "–—";
const TERMINAL = ".!?:;\"'”’)]…";
const LIST_MARK_RE = /^(?:[•◦▪●‣⁃]|\(?\d{1,3}[.)]|\(?[a-z][.)])\s/;
const BULLET_CHARS = "•◦▪▫■□●○‣⁃∙·–-*";
const DOMAIN_RE = new RegExp(
  `^${NW}*(?:https?://)?(?:www\\.)?[\\p{L}\\p{N}_-]+(?:\\.[\\p{L}\\p{N}_-]+)*\\.` +
  `(?:com|net|org|io|info|ru|to|me|co|cc|xyz|club|site|online|pdf)(?![\\p{L}\\p{N}_])/?${NW}*$`, "iu");

// Times-Roman advance widths (1/1000 em) for ASCII 32..126, used to guess whether the next
// line's first word would have fit on the previous line.
const TIMES_W = [250, 333, 408, 500, 500, 833, 778, 180, 333, 333, 500, 564, 250, 333, 250, 278, 500, 500, 500, 500,
  500, 500, 500, 500, 500, 500, 278, 278, 564, 564, 564, 444, 921, 722, 667, 667, 722, 611, 556, 722, 722, 333, 389,
  722, 611, 889, 722, 722, 556, 722, 667, 556, 611, 722, 722, 944, 722, 722, 611, 333, 278, 333, 469, 500, 333, 444,
  500, 444, 500, 444, 333, 500, 500, 278, 278, 500, 278, 778, 500, 500, 500, 500, 333, 389, 278, 500, 500, 722, 500,
  500, 444, 480, 200, 480, 541];

// --------------------------------------------------------------------------- small helpers

function pyRound(x, nd = 0) {
  // Python's round(): halves go to the even neighbour.
  const m = 10 ** nd;
  const v = x * m;
  const r = Math.round(v);
  if (Math.abs(v % 1) === 0.5 && r % 2 !== 0) return (r - 1) / m;
  return r / m;
}

class Counter extends Map {
  add(k, n = 1) { this.set(k, (this.get(k) || 0) + n); return this; }
  count(k) { return this.get(k) || 0; }
  total() { let t = 0; for (const v of this.values()) t += v; return t; }
  mostCommon(n) {
    // stable: ties keep insertion order, like collections.Counter
    const e = [...this.entries()].map((kv, i) => [kv, i]);
    e.sort((a, b) => b[0][1] - a[0][1] || a[1] - b[1]);
    const out = e.map(x => x[0]);
    return n === undefined ? out : out.slice(0, n);
  }
}

function weightedMode(pairs) {
  const c = new Counter();
  for (const [v, w] of pairs) c.add(v, w);
  const top = c.mostCommon(1);
  return top.length ? top[0][0] : null;
}

function median(a) {
  const s = [...a].sort((x, y) => x - y);
  const n = s.length;
  return n % 2 ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
}

const isAlpha = c => !!c && /^\p{L}+$/u.test(c);
const isLower = c => !!c && c !== c.toUpperCase() && c === c.toLowerCase();
const isUpper = c => !!c && c !== c.toLowerCase() && c === c.toUpperCase();
const isDigit = c => !!c && /^\d$/.test(c);
const rstrip = s => s.replace(/\s+$/u, "");
const lstrip = s => s.replace(/^\s+/u, "");
const strip = s => s.trim();
const escapeHtml = (s, quote = true) => {
  s = s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  return quote ? s.replace(/"/g, "&quot;").replace(/'/g, "&#x27;") : s;
};

function cleanText(t) {
  for (const [k, v] of Object.entries(LIGATURES)) t = t.split(k).join(v);
  return t.replace(ZERO_WIDTH_RE, "").replace(XML_BAD_RE, "").replace(/ /g, " ").replace(/\t/g, " ");
}

function normKey(t) {
  t = t.toLowerCase();
  t = t.replace(/\d+/g, "#");
  t = t.replace(new RegExp(`(?<![\\p{L}\\p{N}_])${ROMAN}(?![\\p{L}\\p{N}_])`, "gu"), "#");
  t = t.replace(new RegExp(`(?:${NW}|_)+`, "gu"), " ");
  return t.trim();
}

const boldName = n => /bold|black|heavy|semibold|demi|medi(?:um)?\b|-medi/i.test(n);
const italicName = n => /ital|oblique/i.test(n);
const monoName = n => /mono|courier|consol|code|menlo|typewriter/i.test(n);

function seg(text, bold = false, italic = false, mono = false) { return { text, bold, italic, mono }; }

function newLine(page, x0, y0, x1, y1, segs, size, font, bold, italic) {
  return { page, x0, y0, x1, y1, segs, size, font, bold, italic, col: 0, kind: "body", startsPara: false, level: 0 };
}
const lineText = l => l.segs.map(s => s.text).join("");

// --------------------------------------------------------------------------- extraction

function fontInfo(page, fontName) {
  let f = null;
  try { f = page.commonObjs.get(fontName); } catch (e) { /* not loaded */ }
  const name = (f && f.name) || fontName || "";
  return {
    name,
    bold: !!(f && (f.bold || f.black)) || boldName(name),
    italic: !!(f && f.italic) || italicName(name),
    mono: !!(f && f.isMonospace) || monoName(name),
  };
}

function mul(m, n) {
  return [m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3],
    m[1] * n[2] + m[3] * n[3], m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]];
}

// Filled and stroked shapes on the page, as top-left based rectangles (like PyMuPDF's
// get_drawings rects). Used for drawn bullets.
function drawings(opList, OPS, view) {
  const out = [];
  const stack = [];
  let ctm = [1, 0, 0, 1, 0, 0];
  let pending = null;
  const H = view[3];
  const toPage = ([x, y]) => [ctm[0] * x + ctm[2] * y + ctm[4] - view[0], H - (ctm[1] * x + ctm[3] * y + ctm[5])];
  out.images = [];
  for (let i = 0; i < opList.fnArray.length; i++) {
    const fn = opList.fnArray[i], args = opList.argsArray[i];
    if (fn === OPS.save) stack.push(ctm);
    else if (fn === OPS.restore) ctm = stack.pop() || [1, 0, 0, 1, 0, 0];
    else if (fn === OPS.transform) ctm = mul(ctm, args);
    else if (fn === OPS.paintImageXObject || fn === OPS.paintInlineImageXObject) {
      // images are drawn into the unit square of the current transform
      const c = [[0, 0], [1, 0], [0, 1], [1, 1]].map(toPage);
      const xs = c.map(p => p[0]), ys = c.map(p => p[1]);
      const box = { x0: Math.min(...xs), x1: Math.max(...xs), y0: Math.min(...ys), y1: Math.max(...ys) };
      if (fn === OPS.paintImageXObject) out.images.push({ ...box, id: args[0], w: args[1], h: args[2] });
      else if (args[0]) out.images.push({ ...box, inline: args[0], w: args[0].width, h: args[0].height });
    } else if (fn === OPS.constructPath) {
      // Bounding box from the path's own points (pdf.js' minMax is unreliable for curves).
      const [pathOps, coords] = args;
      const local = [];
      const segs = []; // straight pieces, for table rules
      let k = 0, at = null, start = null;
      for (const op of pathOps || []) {
        if (op === OPS.rectangle) {
          const [x, y, w, h] = coords.slice(k, k + 4);
          local.push([x, y], [x + w, y + h]);
          segs.push([[x, y], [x + w, y]], [[x + w, y], [x + w, y + h]], [[x + w, y + h], [x, y + h]], [[x, y + h], [x, y]]);
          at = start = [x, y];
          k += 4;
        } else {
          const n = op === OPS.moveTo || op === OPS.lineTo ? 2 : op === OPS.curveTo ? 6
            : op === OPS.curveTo2 || op === OPS.curveTo3 ? 4 : 0;
          for (let q = 0; q < n; q += 2) local.push([coords[k + q], coords[k + q + 1]]);
          const end = n ? [coords[k + n - 2], coords[k + n - 1]] : null;
          if (op === OPS.moveTo) start = end;
          else if (op === OPS.lineTo && at) segs.push([at, end]);
          else if (op === OPS.closePath && at && start) segs.push([at, start]);
          if (end) at = end;
          else if (op === OPS.closePath) at = start;
          k += n;
        }
      }
      if (!local.length) { pending = null; continue; }
      const pts = local.map(toPage);
      const xs = pts.map(p => p[0]), ys = pts.map(p => p[1]);
      pending = { x0: Math.min(...xs), x1: Math.max(...xs), y0: Math.min(...ys), y1: Math.max(...ys),
        segs: segs.map(([a, b]) => [toPage(a), toPage(b)]) };
    } else if (pending) {
      const fill = fn === OPS.fill || fn === OPS.eoFill || fn === OPS.fillStroke || fn === OPS.eoFillStroke ||
        fn === OPS.closeFillStroke || fn === OPS.closeEOFillStroke;
      const stroke = fn === OPS.stroke || fn === OPS.closeStroke || fn === OPS.fillStroke || fn === OPS.eoFillStroke ||
        fn === OPS.closeFillStroke || fn === OPS.closeEOFillStroke;
      if (fill || stroke) {
        pending.fill = fill;
        pending.width = pending.x1 - pending.x0;
        pending.height = pending.y1 - pending.y0;
        out.push(pending);
      }
      if (fn !== OPS.constructPath) pending = null;
    }
  }
  return out;
}

async function extractPage(pdf, OPS, pno, pageNo, opts = {}) {
  const page = await pdf.getPage(pno + 1);
  const view = page.view;
  const width = view[2] - view[0], height = view[3] - view[1];
  const opList = await page.getOperatorList(); // also loads the fonts, so real names are known
  const tc = await page.getTextContent();
  const pg = { number: pageNo, width, height, twoCol: false, lines: [] };

  const raw = [];
  const words = []; // word boxes, for rebuilding table cells
  let cur = null;
  const finish = () => {
    if (cur && cur.segs.length && strip(lineText(cur))) {
      const vis = cur.segs.filter(s => strip(s.text));
      cur.size = weightedMode(cur._sizes.map(([sz, n]) => [sz, n]));
      cur.font = weightedMode(cur._sizes.map(([, n, f]) => [f, n]));
      cur.bold = vis.every(s => s.bold);
      cur.italic = vis.every(s => s.italic);
      delete cur._sizes; delete cur._base;
      // a monospace line that starts with spaces: the spaces are not drawn, keep them anyway
      if (cur._cw && cur.segs.length && cur.segs[0].mono) cur.segs[0].text = lstrip(cur.segs[0].text);
      raw.push(cur);
    }
    cur = null;
  };
  for (const it of tc.items) {
    if (it.str === undefined) continue; // marked-content markers
    const [a, b, c, d, e, f] = it.transform;
    const size = Math.hypot(c, d) || Math.hypot(a, b);
    if (!it.str) { if (it.hasEOL) finish(); continue; }
    // rotated or mirrored text: watermarks, margin notes
    if (Math.abs(b) > 0.05 * Math.abs(a) || a < 0) { if (it.hasEOL) finish(); continue; }
    const st = tc.styles[it.fontName] || {};
    const asc = st.ascent || 0.8, desc = st.descent || -0.2;
    const x0 = e - view[0];
    const base = view[3] - f;
    const x1 = x0 + it.width;
    const y0 = base - asc * size, y1 = base - desc * size;
    if (x1 < 0 || x0 > width || y1 < 0 || y0 > height) { if (it.hasEOL) finish(); continue; }
    const fi = fontInfo(page, it.fontName);
    const t = cleanText(it.str);
    if (t) {
      // word positions, spread over the item by character count
      const chars = [...t], cw = it.width / (chars.length || 1);
      for (const m of t.matchAll(/\S+/gu)) {
        const a = [...t.slice(0, m.index)].length, n = [...m[0]].length;
        words.push({ x0: x0 + a * cw, x1: x0 + (a + n) * cw, y0, y1, text: m[0], bold: fi.bold, mono: fi.mono });
      }
      // Same visual line: continues to the right (accents may step back a little) and overlaps
      // vertically, so superscripts and footnote marks stay on their line.
      let sameLine = false;
      if (cur) {
        const gap = x0 - cur.x1;
        const overlap = Math.min(cur.y1, y1) - Math.max(cur.y0, y0);
        // aligned code ("key":     value) can leave wide gaps inside one line
        const lastSeg = cur.segs[cur.segs.length - 1];
        const maxGap = (fi.mono && lastSeg && lastSeg.mono ? 10 : 2) * size;
        sameLine = gap > -Math.max(2, 0.6 * size) && gap < maxGap &&
          (Math.abs(cur._base - base) < Math.max(1.5, 0.25 * size) || overlap > 0.5 * Math.min(cur.y1 - cur.y0, y1 - y0));
      }
      if (!sameLine) {
        finish();
        if (!strip(t)) { if (it.hasEOL) finish(); continue; }
        cur = newLine(pageNo, x0, y0, x1, y1, [], 0, "", false, false);
        cur._sizes = []; cur._base = base;
      } else {
        const gap = x0 - cur.x1;
        const lt = lineText(cur);
        const prevMono = cur.segs.length && cur.segs[cur.segs.length - 1].mono;
        if (fi.mono && prevMono && !strip(t)) { if (it.hasEOL) finish(); continue; } // rebuilt from positions below
        if (fi.mono && prevMono && gap > 0.15 * size) {
          // monospace: keep the number of spaces, so code columns line up
          const cw = it.width / [...t].length || 0.6 * size;
          cur.segs.push(seg(" ".repeat(Math.max(1, Math.round(gap / cw))), false, false, true));
        } else if (gap > 0.15 * size && !lt.endsWith(" ") && !t.startsWith(" ")) {
          const last = cur.segs[cur.segs.length - 1];
          cur.segs.push(seg(" ", last.bold, last.italic));
        }
      }
      cur.segs.push(seg(t, fi.bold, fi.italic, fi.mono));
      // pdf.js turns wide gaps into space items; they must not bridge columns or table cells,
      // so only visible text extends the line's box.
      if (strip(t)) {
        if (fi.mono && !cur._cw) cur._cw = it.width / [...t].length;
        cur._sizes.push([pyRound(size * 2) / 2, strip(t).length || 1, fi.name]);
        cur.x0 = Math.min(cur.x0, x0); cur.x1 = Math.max(cur.x1, x1);
        cur.y0 = Math.min(cur.y0, y0); cur.y1 = Math.max(cur.y1, y1);
      }
    }
    if (it.hasEOL) finish();
  }
  finish();
  for (const l of raw) if (!l._sizes) { /* already finalised */ }
  const shapes = drawings(opList, OPS, view);
  pg.tables = findTables(shapes, words, pageNo, width, height);
  let kept = raw.filter(l => l.size);
  if (pg.tables.length) {
    // text inside a table is rebuilt cell by cell; drop the flowing copy
    const inside = l => {
      const cx = (l.x0 + l.x1) / 2, cy = (l.y0 + l.y1) / 2;
      return pg.tables.some(t => t.x0 - 2 <= cx && cx <= t.x1 + 2 && t.y0 - 2 <= cy && cy <= t.y1 + 2);
    };
    kept = kept.filter(l => !inside(l));
  }
  pg.lines = mergeSameBaseline(kept);
  markDrawnBullets(shapes, pg.lines);
  if (opts.images) {
    for (const im of shapes.images) {
      // skip decorations, rules and tiny ornaments
      if (!(im.w >= 64 && im.h >= 64 && im.x1 - im.x0 > 40 && im.y1 - im.y0 > 40)) continue;
      if (im.x1 < 0 || im.x0 > width || im.y1 < 0 || im.y0 > height) continue;
      let jpg = null;
      try { jpg = await imageToJpeg(page, im, opts.encodeJpeg); } catch (e) { jpg = null; }
      if (!jpg) continue;
      const l = newLine(pageNo, im.x0, im.y0, im.x1, im.y1, [], 0, "", false, false);
      l.kind = "image"; l.image = jpg;
      pg.lines.push(l);
    }
  }
  page.cleanup();
  return pg;
}

// --------------------------------------------------------------------------- tables

// Tables drawn with rules or shaded rows, found the way PyMuPDF's find_tables(strategy="lines")
// and pdfplumber do it: straight edges are snapped and joined, their crossings found, and
// the smallest rectangles between crossings become cells. Each table becomes a Line(kind "table").
const SNAP = 3, JOIN = 3, ISECT = 3, MIN_EDGE = 3;

function tableEdges(shapes, width, height) {
  const hs = [], vs = [];
  const cl = (v, hi) => Math.min(Math.max(v, 0), hi);
  for (const sh of shapes) {
    for (let [[ax, ay], [bx, by]] of sh.segs || []) {
      // only the part on the page counts
      if (Math.max(ax, bx) < 0 || Math.min(ax, bx) > width || Math.max(ay, by) < 0 || Math.min(ay, by) > height) continue;
      ax = cl(ax, width); bx = cl(bx, width); ay = cl(ay, height); by = cl(by, height);
      if (Math.abs(ay - by) < 0.5 && Math.abs(ax - bx) >= MIN_EDGE) hs.push({ a: Math.min(ax, bx), b: Math.max(ax, bx), at: (ay + by) / 2 });
      else if (Math.abs(ax - bx) < 0.5 && Math.abs(ay - by) >= MIN_EDGE) vs.push({ a: Math.min(ay, by), b: Math.max(ay, by), at: (ax + bx) / 2 });
    }
  }
  const snapJoin = edges => {
    // snap: edges whose positions are within SNAP of each other share their average
    edges.sort((p, q) => p.at - q.at);
    const out = [];
    let group = [];
    const close = () => {
      if (!group.length) return;
      const avg = group.reduce((n, e) => n + e.at, 0) / group.length;
      for (const e of group) e.at = avg;
      group.sort((p, q) => p.a - q.a);
      // join: collinear pieces that touch or nearly touch become one edge
      let cur = null;
      for (const e of group) {
        if (cur && e.a <= cur.b + JOIN) cur.b = Math.max(cur.b, e.b);
        else { if (cur) out.push(cur); cur = { ...e }; }
      }
      if (cur) out.push(cur);
      group = [];
    };
    for (const e of edges) {
      if (group.length && e.at - group[0].at > SNAP) close();
      group.push(e);
    }
    close();
    return out.filter(e => e.b - e.a >= MIN_EDGE);
  };
  return [snapJoin(hs), snapJoin(vs)];
}

function tableCells(hs, vs) {
  // crossings, keyed by point, with the edges that meet there
  const pts = new Map();
  for (const [vi, v] of vs.entries()) {
    for (const [hi, h] of hs.entries()) {
      if (v.at >= h.a - ISECT && v.at <= h.b + ISECT && h.at >= v.a - ISECT && h.at <= v.b + ISECT) {
        const k = v.at + "," + h.at;
        if (!pts.has(k)) pts.set(k, { x: v.at, y: h.at, v: new Set(), h: new Set() });
        pts.get(k).v.add(vi); pts.get(k).h.add(hi);
      }
    }
  }
  const list = [...pts.values()].sort((p, q) => p.y - q.y || p.x - q.x);
  const share = (A, B) => { for (const x of A) if (B.has(x)) return true; return false; };
  const cells = [];
  for (let i = 0; i < list.length; i++) {
    const p = list[i];
    const below = list.filter(q => q.x === p.x && q.y > p.y).sort((a, b) => a.y - b.y);
    const right = list.filter(q => q.y === p.y && q.x > p.x).sort((a, b) => a.x - b.x);
    let found = null;
    for (const b of below) {
      if (!share(p.v, b.v)) continue;
      for (const r of right) {
        if (!share(p.h, r.h)) continue;
        const br = pts.get(r.x + "," + b.y);
        if (br && share(br.h, b.h) && share(br.v, r.v)) { found = { x0: p.x, y0: p.y, x1: r.x, y1: b.y }; break; }
      }
      if (found) break;
    }
    if (found) cells.push(found);
  }
  return cells;
}

function groupCells(cells) {
  // cells that share a corner belong to the same table
  const corners = c => [[c.x0, c.y0], [c.x1, c.y0], [c.x0, c.y1], [c.x1, c.y1]].map(([x, y]) => x + "," + y);
  const tables = [];
  const left = cells.slice();
  while (left.length) {
    const t = [left.shift()];
    const seen = new Set(corners(t[0]));
    let grew = true;
    while (grew) {
      grew = false;
      for (let i = 0; i < left.length; i++) {
        if (corners(left[i]).some(k => seen.has(k))) {
          for (const k of corners(left[i])) seen.add(k);
          t.push(left.splice(i, 1)[0]);
          i--; grew = true;
        }
      }
    }
    if (t.length > 1) tables.push(t);
  }
  return tables;
}

function cellText(words, r) {
  const ws = words.filter(w => {
    const cx = (w.x0 + w.x1) / 2, cy = (w.y0 + w.y1) / 2;
    return cx >= r.x0 && cx <= r.x1 && cy >= r.y0 && cy <= r.y1;
  });
  // reading order: by line, then left to right
  ws.sort((a, b) => a.y0 - b.y0);
  const lines = [];
  for (const w of ws) {
    const ln = lines.find(l => Math.min(l.y1, w.y1) - Math.max(l.y0, w.y0) > 0.5 * Math.min(l.y1 - l.y0, w.y1 - w.y0));
    if (ln) { ln.ws.push(w); ln.y0 = Math.min(ln.y0, w.y0); ln.y1 = Math.max(ln.y1, w.y1); }
    else lines.push({ y0: w.y0, y1: w.y1, ws: [w] });
  }
  lines.sort((a, b) => a.y0 - b.y0);
  const out = [];
  for (const l of lines) {
    for (const w of l.ws.sort((a, b) => a.x0 - b.x0)) {
      const t = w.text;
      const last = out[out.length - 1];
      if (last && last.endsWith("-") && last.length > 1 && /\p{L}/u.test(last[last.length - 2]) && /^\p{Ll}/u.test(t)) {
        out[out.length - 1] = last.slice(0, -1) + t; // word split across lines inside the cell
      } else out.push(t);
    }
  }
  const vis = ws.filter(w => strip(w.text));
  return [strip(out.join(" ")), vis.length > 0 && vis.every(w => w.bold)];
}

function findTables(shapes, words, pageNo, width, height) {
  // only try on pages with several rules or cell fills
  if (shapes.filter(sh => Math.max(sh.width, sh.height) > 30).length < 4) return [];
  const [hs, vs] = tableEdges(shapes, width, height);
  if (hs.length < 2 || vs.length < 2) return [];
  const result = [];
  for (const cells of groupCells(tableCells(hs, vs))) {
    const x0 = Math.min(...cells.map(c => c.x0)), y0 = Math.min(...cells.map(c => c.y0));
    const x1 = Math.max(...cells.map(c => c.x1)), y1 = Math.max(...cells.map(c => c.y1));
    // rows: cells with the same top edge
    const tops = [...new Set(cells.map(c => c.y0))].sort((a, b) => a - b);
    const colXs = [...new Set(cells.map(c => c.x0))].sort((a, b) => a - b);
    const rows = tops.map(y => {
      const rc = cells.filter(c => c.y0 === y);
      return { cells: colXs.map(x => rc.find(c => c.x0 === x) || null),
        y0: y, y1: Math.max(...rc.map(c => c.y1)) };
    });
    if (rows.length < 2) continue;
    // column edges from the row that has the most separate cells
    let best = rows[0];
    for (const r of rows) if (r.cells.filter(Boolean).length > best.cells.filter(Boolean).length) best = r;
    const bc = best.cells.filter(Boolean);
    if (bc.length < 2) continue;
    const xs = bc.map(c => c.x0).concat([bc[bc.length - 1].x1]);
    const inTable = words.filter(w => {
      const cx = (w.x0 + w.x1) / 2, cy = (w.y0 + w.y1) / 2;
      return cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1;
    });
    // a shaded code listing is not a table
    const nChars = inTable.reduce((n, w) => n + w.text.length, 0);
    if (nChars && inTable.filter(w => w.mono).reduce((n, w) => n + w.text.length, 0) > 0.5 * nChars) continue;
    let caption = null, header = 0;
    const outRows = [];
    for (const r of rows) {
      const rw = inTable.filter(w => r.y0 - 1 <= (w.y0 + w.y1) / 2 && (w.y0 + w.y1) / 2 <= r.y1 + 1);
      if (!rw.length) continue;
      // a row whose words straddle a column edge spans the whole width (title, note)
      const spans = rw.some(w => xs.slice(1, -1).some(e => w.x0 < e - 1 && w.x1 > e + 1));
      if (spans) {
        const [text] = cellText(inTable, { x0: xs[0], y0: r.y0, x1: xs[xs.length - 1], y1: r.y1 });
        if (!outRows.length && caption === null) caption = text;
        else outRows.push([text]);
        continue;
      }
      const row = [];
      for (let c = 0; c + 1 < xs.length; c++) row.push(cellText(inTable, { x0: xs[c], y0: r.y0, x1: xs[c + 1], y1: r.y1 }));
      if (!row.some(([t]) => t)) continue;
      if (!outRows.length && row.filter(([t]) => t).every(([, b]) => b)) header = 1;
      outRows.push(row.map(([t]) => t));
    }
    if (outRows.length < 2) continue;
    const l = newLine(pageNo, x0, y0, x1, y1, [], 0, "", false, false);
    l.kind = "table";
    l.table = { caption, rows: outRows, header, cols: xs.length - 1 };
    result.push(l);
  }
  return result;
}

function tableHtml(t) {
  // CrossPoint gives every cell the reader's paragraph indent unless the first word
  // is laid out with preserved whitespace; the "ni" span does that and nothing else.
  const e = x => {
    if (!x) return "";
    const i = x.indexOf(" ");
    const first = i < 0 ? x : x.slice(0, i), rest = i < 0 ? "" : x.slice(i + 1);
    return `<span class="ni">${escapeHtml(first, false)}</span>` + (rest ? " " + escapeHtml(rest, false) : "");
  };
  const out = [];
  if (t.caption) out.push(`<p class="caption"><b>${e(t.caption)}</b></p>`);
  out.push("<table>");
  t.rows.forEach((row, i) => {
    const tag = i < t.header ? "th" : "td";
    if (row.length === 1 && t.cols > 1) out.push(`<tr><${tag} colspan="${t.cols}">${e(row[0])}</${tag}></tr>`);
    else out.push("<tr>" + row.map(c => `<${tag}>${e(c)}</${tag}>`).join("") + "</tr>");
  });
  out.push("</table>");
  return out.join("\n");
}

// --------------------------------------------------------------------------- images

// pdf.js hands decoded images over once the operator list is done; wait a little if not yet.
function pdfObject(page, id) {
  const objs = id.startsWith("g_") ? page.commonObjs : page.objs;
  if (objs.has(id)) return Promise.resolve(objs.get(id));
  return new Promise(resolve => {
    const t = setTimeout(() => resolve(null), 5000);
    objs.get(id, v => { clearTimeout(t); resolve(v); });
  });
}

// Image pixels as 8-bit gray, transparent parts on white. Returns null for formats we can't read.
function toGray(img) {
  const { width: w, height: h, kind, data } = img;
  if (!w || !h || !data) return null;
  const g = new Uint8Array(w * h);
  if (kind === 1) { // 1 bit per pixel, rows padded to whole bytes, 1 = white
    const rb = (w + 7) >> 3;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      g[y * w + x] = data[y * rb + (x >> 3)] & (128 >> (x & 7)) ? 255 : 0;
    }
  } else if (kind === 2) { // RGB
    for (let i = 0, j = 0; i < g.length; i++, j += 3) g[i] = (299 * data[j] + 587 * data[j + 1] + 114 * data[j + 2]) / 1000;
  } else if (kind === 3) { // RGBA
    for (let i = 0, j = 0; i < g.length; i++, j += 4) {
      const a = data[j + 3] / 255;
      const v = (299 * data[j] + 587 * data[j + 1] + 114 * data[j + 2]) / 1000;
      g[i] = v * a + 255 * (1 - a);
    }
  } else return null;
  return { w, h, g };
}

// Shrink to fit the screen by averaging pixel areas.
function fitGray({ w, h, g }, maxW = SCREEN_W, maxH = SCREEN_H) {
  const scale = Math.min(1, maxW / w, maxH / h);
  if (scale >= 1) return { w, h, g };
  const W2 = Math.max(1, Math.round(w * scale)), H2 = Math.max(1, Math.round(h * scale));
  const out = new Uint8Array(W2 * H2);
  const fx = w / W2, fy = h / H2;
  for (let y = 0; y < H2; y++) {
    const sy0 = Math.floor(y * fy), sy1 = Math.max(sy0 + 1, Math.floor((y + 1) * fy));
    for (let x = 0; x < W2; x++) {
      const sx0 = Math.floor(x * fx), sx1 = Math.max(sx0 + 1, Math.floor((x + 1) * fx));
      let sum = 0;
      for (let yy = sy0; yy < sy1; yy++) for (let xx = sx0; xx < sx1; xx++) sum += g[yy * w + xx];
      out[y * W2 + x] = sum / ((sy1 - sy0) * (sx1 - sx0));
    }
  }
  return { w: W2, h: H2, g: out };
}

// Gray pixels to JPEG (quality 80) with a canvas. Node callers pass their own encodeJpeg(g, w, h).
async function encodeJpegBrowser(g, w, h) {
  const rgba = new Uint8ClampedArray(w * h * 4);
  for (let i = 0, j = 0; i < g.length; i++, j += 4) { rgba[j] = rgba[j + 1] = rgba[j + 2] = g[i]; rgba[j + 3] = 255; }
  const imgData = new ImageData(rgba, w, h);
  let blob;
  if (typeof OffscreenCanvas !== "undefined") {
    const c = new OffscreenCanvas(w, h);
    c.getContext("2d").putImageData(imgData, 0, 0);
    blob = await c.convertToBlob({ type: "image/jpeg", quality: 0.8 });
  } else {
    const c = document.createElement("canvas");
    c.width = w; c.height = h;
    c.getContext("2d").putImageData(imgData, 0, 0);
    blob = await new Promise(r => c.toBlob(r, "image/jpeg", 0.8));
  }
  return blob ? new Uint8Array(await blob.arrayBuffer()) : null;
}

async function imageToJpeg(page, im, encode) {
  const obj = im.inline || await pdfObject(page, im.id);
  if (!obj) return null;
  let gray = null;
  if (obj.bitmap && typeof OffscreenCanvas !== "undefined") {
    const c = new OffscreenCanvas(obj.bitmap.width, obj.bitmap.height);
    const ctx = c.getContext("2d");
    ctx.drawImage(obj.bitmap, 0, 0);
    const d = ctx.getImageData(0, 0, c.width, c.height);
    gray = toGray({ width: c.width, height: c.height, kind: 3, data: d.data });
  } else {
    gray = toGray(obj);
  }
  if (!gray) return null;
  const f = fitGray(gray);
  return (encode || encodeJpegBrowser)(f.g, f.w, f.h);
}

function markDrawnBullets(shapes, lines) {
  for (const r of shapes) {
    if (!r.fill) continue;
    if (!(r.width >= 1.5 && r.width <= 9 && r.height >= 1.5 && r.height <= 9 && Math.abs(r.width - r.height) < 1.5)) continue;
    const cy = (r.y0 + r.y1) / 2;
    for (const l of lines) {
      if (l.kind !== "image" && l.y0 <= cy && cy <= l.y1 && l.x0 - r.x1 >= 0 && l.x0 - r.x1 < 25) {
        if (!l.startsPara) {
          l.startsPara = true;
          l.segs.unshift(seg("• "));
        }
        break;
      }
    }
  }
}

function mergeSameBaseline(lines) {
  const text = lines.filter(l => l.kind !== "image");
  const imgs = lines.filter(l => l.kind === "image");
  text.sort((a, b) => pyRound(a.y1) - pyRound(b.y1) || a.x0 - b.x0);
  const out = [];
  for (const l of text) {
    if (out.length) {
      const p = out[out.length - 1];
      const sameRow = Math.abs(p.y1 - l.y1) < Math.max(1.5, 0.25 * l.size) && Math.abs(p.size - l.size) < 0.6;
      const gap = l.x0 - p.x1;
      if (sameRow && gap > -1 && gap < 2 * l.size) {
        const pt = lineText(p);
        if (gap > 0.15 * l.size && !pt.endsWith(" ") && !lineText(l).startsWith(" ")) {
          const last = p.segs[p.segs.length - 1];
          p.segs.push(seg(" ", last.bold, last.italic));
        }
        p.segs.push(...l.segs);
        p.x1 = Math.max(p.x1, l.x1);
        p.y0 = Math.min(p.y0, l.y0); p.y1 = Math.max(p.y1, l.y1);
        const vis = p.segs.filter(s => strip(s.text));
        p.bold = vis.every(s => s.bold);
        p.italic = vis.every(s => s.italic);
        continue;
      }
    }
    out.push(l);
  }
  return out.concat(imgs);
}

// --------------------------------------------------------------------------- stats

function bodyStats(pages) {
  const pairsSize = [], pairsFont = [];
  for (const pg of pages) for (const l of pg.lines) {
    if (l.kind === "image") continue;
    const n = strip(lineText(l)).length;
    pairsSize.push([l.size, n]);
    pairsFont.push([l.font, n]);
  }
  if (!pairsSize.length) {
    throw new Error("No text found in the PDF. It is probably a scan: run OCR on it first, then convert the result.");
  }
  const size = weightedMode(pairsSize);
  const font = weightedMode(pairsFont);
  const gaps = [];
  for (const pg of pages) {
    const body = pg.lines.filter(l => l.kind !== "image" && Math.abs(l.size - size) < 0.6)
      .sort((a, b) => Math.floor(a.x0 / (pg.width / 2)) - Math.floor(b.x0 / (pg.width / 2)) || a.y0 - b.y0);
    for (let i = 0; i + 1 < body.length; i++) {
      const d = body[i + 1].y1 - body[i].y1;
      if (d > 0.8 * size && d < 2.5 * size) gaps.push(pyRound(d, 1));
    }
  }
  const leading = gaps.length ? median(gaps) : size * 1.2;
  return { bodySize: size, bodyFont: font, leading, blockStyle: false, indentStyle: true };
}

// --------------------------------------------------------------------------- headers / footers

function groupRows(lines) {
  const rows = [];
  for (const l of [...lines].sort((a, b) => a.y1 - b.y1)) {
    if (rows.length && Math.abs(rows[rows.length - 1][0].y1 - l.y1) < 2.5) rows[rows.length - 1].push(l);
    else rows.push([l]);
  }
  return rows;
}

function stripHeadersFooters(pages, st, margin, extraPatterns) {
  const removed = new Counter();
  const nPages = pages.length;
  const repMin = nPages >= 6 ? 3 : 2;

  if (extraPatterns && extraPatterns.length) {
    for (const pg of pages) {
      pg.lines = pg.lines.filter(l => {
        if (l.kind !== "image" && extraPatterns.some(p => p.test(lineText(l)))) {
          removed.add("[pattern] " + strip(lineText(l)).slice(0, 60));
          return false;
        }
        return true;
      });
    }
  }

  const seen = new Counter();
  for (const pg of pages) for (const l of pg.lines) {
    if (l.kind !== "image" && DOMAIN_RE.test(strip(lineText(l)))) seen.add(strip(lineText(l)).toLowerCase());
  }
  const marks = new Set([...seen].filter(([, c]) => c >= 3).map(([t]) => t));
  if (marks.size) {
    for (const pg of pages) {
      pg.lines = pg.lines.filter(l => {
        if (l.kind !== "image" && marks.has(strip(lineText(l)).toLowerCase())) {
          removed.add("[watermark] " + strip(lineText(l)).slice(0, 60));
          return false;
        }
        return true;
      });
    }
  }

  const cands = []; // [pg, line, where, isolated]
  for (const pg of pages) {
    const text = pg.lines.filter(l => l.kind !== "image").sort((a, b) => a.y0 - b.y0);
    if (!text.length) continue;
    const topLim = pg.height * margin, botLim = pg.height * (1 - margin);
    const rows = groupRows(text);
    for (const [where, seq] of [["top", rows], ["bottom", [...rows].reverse()]]) {
      for (let i = 0; i < Math.min(2, seq.length); i++) {
        const row = seq[i];
        const y = where === "top" ? row[0].y0 : row[0].y1;
        if ((where === "top" && y > topLim) || (where === "bottom" && y < botLim)) break;
        const nxt = i + 1 < seq.length ? seq[i + 1] : null;
        let gap;
        if (!nxt) gap = 99;
        else if (where === "top") gap = (nxt[0].y1 - row[0].y1) / st.leading;
        else gap = (row[0].y1 - nxt[0].y1) / st.leading;
        const isolated = gap > 1.4;
        for (const l of row) cands.push([pg, l, where, isolated]);
        const rowStyled = row.every(l => Math.abs(l.size - st.bodySize) > 0.6 || l.font !== st.bodyFont);
        if (!isolated && !rowStyled) break;
      }
    }
  }

  const keyPos = new Map();
  const bands = new Map();
  for (const [, l, where, iso] of cands) {
    const k = where + "\u0000" + normKey(lineText(l));
    if (!keyPos.has(k)) keyPos.set(k, []);
    keyPos.get(k).push(where === "top" ? l.y0 : l.y1);
    const yb = pyRound((where === "top" ? l.y0 : l.y1) / 3);
    const bk = where + "\u0000" + yb;
    const b = bands.get(bk) || [0, 0];
    b[1] += 1;
    if (iso) b[0] += 1;
    bands.set(bk, b);
  }
  const inHeaderBand = (where, y) => {
    const yb = pyRound(y / 3);
    let iso = 0, tot = 0;
    for (const d of [-1, 0, 1]) {
      const [i, t] = bands.get(where + "\u0000" + (yb + d)) || [0, 0];
      iso += i; tot += t;
    }
    return tot >= Math.max(2, 0.3 * nPages) && iso >= 0.7 * tot;
  };

  const drop = new Set();
  for (const [, l, where, iso] of cands) {
    const t = strip(lineText(l));
    const y = where === "top" ? l.y0 : l.y1;
    const key = normKey(t);
    const styled = Math.abs(l.size - st.bodySize) > 0.6 || l.font !== st.bodyFont;
    let reason = null;
    if (PAGE_NUM_RE.test(t)) reason = "page number";
    else if (t.length < 120) {
      const ys = keyPos.get(where + "\u0000" + key) || [];
      const samePos = ys.filter(v => Math.abs(v - y) < 4).length;
      if (key && samePos >= repMin && (iso || styled)) reason = "repeated";
      else if (iso && inHeaderBand(where, y) && l.size <= st.bodySize * 1.1) reason = "header band";
    }
    if (reason) {
      drop.add(l);
      removed.add(`[${where} ${reason}] ` + t.replace(/\d+/g, "#").slice(0, 60));
    }
  }
  for (const [pg, l, where] of cands) {
    if (drop.has(l)) continue;
    const styled = Math.abs(l.size - st.bodySize) > 0.6 || l.font !== st.bodyFont;
    if (!styled) continue;
    for (const o of pg.lines) {
      if (drop.has(o) && o.kind !== "image" && Math.abs(o.size - l.size) < 0.6 && o.font === l.font &&
          Math.abs(o.y1 - l.y1) < 1.6 * Math.max(l.size, 1)) {
        drop.add(l);
        removed.add(`[${where} wrapped header] ` + strip(lineText(l)).replace(/\d+/g, "#").slice(0, 60));
        break;
      }
    }
  }
  for (const pg of pages) pg.lines = pg.lines.filter(l => !drop.has(l));
  return removed;
}

// --------------------------------------------------------------------------- layout

function assignColumns(pg) {
  const text = pg.lines.filter(l => l.kind !== "image");
  const mid = pg.width / 2;
  const narrow = text.filter(l => (l.x1 - l.x0) < pg.width * 0.48);
  const left = narrow.filter(l => l.x1 < mid + 5);
  const right = narrow.filter(l => l.x0 > mid - 5);
  const twoCol = text.length >= 10 && left.length >= 0.3 * text.length && right.length >= 0.3 * text.length;
  pg.twoCol = twoCol;
  for (const l of pg.lines) l.col = twoCol && l.x0 > mid - 5 ? 1 : 0;
  if (twoCol) {
    const spanners = pg.lines.filter(l => (l.x1 - l.x0) >= pg.width * 0.55).map(l => l.y0).sort((a, b) => a - b);
    const band = l => (l.x1 - l.x0) < pg.width * 0.55
      ? spanners.filter(y => y <= l.y0 - 0.5).length
      : spanners.filter(y => y < l.y0 - 0.5).length;
    pg.lines.sort((a, b) => band(a) - band(b) || a.col - b.col || a.y0 - b.y0 || a.x0 - b.x0);
  } else {
    pg.lines.sort((a, b) => a.y0 - b.y0 || a.x0 - b.x0);
  }
}

function columnEdges(pg, st) {
  const edges = new Map();
  for (const col of new Set(pg.lines.map(l => l.col))) {
    let body = pg.lines.filter(l => l.col === col && l.kind === "body" && Math.abs(l.size - st.bodySize) < 1 &&
      !(pg.twoCol && (l.x1 - l.x0) >= pg.width * 0.55));
    if (!body.length) body = pg.lines.filter(l => l.col === col && l.kind !== "image");
    if (!body.length) continue;
    const xs = new Counter();
    for (const l of body) xs.add(pyRound(l.x0));
    const [mode, top] = xs.mostCommon(1)[0];
    const left = Math.min(mode, ...[...xs].filter(([x, c]) => x < mode && c >= 0.4 * top).map(([x]) => x));
    const xr = new Counter();
    for (const l of body) xr.add(pyRound(l.x1));
    const topR = xr.mostCommon(1)[0][1];
    let right;
    if (topR >= 3) {
      right = Math.max(...[...xr].filter(([, c]) => c >= Math.max(2, 0.2 * topR)).map(([x]) => x));
    } else {
      const rv = body.map(l => l.x1).sort((a, b) => a - b);
      right = rv[Math.floor(0.9 * (rv.length - 1))];
    }
    edges.set(col, [left, right]);
  }
  return edges;
}

function markHeadings(pages, st) {
  const hasBig = pages.some(pg => pg.lines.some(l => l.kind !== "image" && l.size >= st.bodySize * 1.15 &&
    strip(lineText(l)).length > 1 && strip(lineText(l)).length <= 160));
  for (const pg of pages) for (const l of pg.lines) {
    const vis = l.segs.filter(s => strip(s.text));
    if (l.kind === "body" && vis.length && vis.every(s => s.mono)) l.kind = "code";
  }
  for (const pg of pages) {
    const lines = pg.lines;
    for (let i = 0; i < lines.length; i++) {
      const l = lines[i];
      if (l.kind === "image" || l.kind === "code") continue;
      const t = strip(lineText(l));
      const big = l.size >= st.bodySize * 1.15;
      if (big && [...t].length === 1 && isAlpha(t)) {
        const beside = lines.filter(o => o !== l && o.kind === "body" && o.x0 >= l.x1 - 1 &&
          o.x0 - l.x1 < 3 * st.bodySize && l.y0 - 2 <= o.y0 && o.y0 <= l.y1);
        if (beside.length) {
          const target = beside.reduce((m, o) => (o.y0 < m.y0 ? o : m));
          target.segs.unshift(seg(t, target.segs[0].bold, target.segs[0].italic));
          for (const o of beside) o.x0 = l.x0;
          target.startsPara = true;
          l.kind = "dropcap";
          continue;
        }
      }
      if (big && t.length <= 160 && new RegExp(W, "u").test(t)) { l.kind = "heading"; continue; }
      if (t.length > 80) continue;
      const prev = i ? lines[i - 1] : null;
      const nxt = i + 1 < lines.length ? lines[i + 1] : null;
      const aloneBefore = !prev || prev.kind === "heading" || (l.y0 - prev.y1) > 0.9 * st.leading;
      const aloneAfter = !nxt || (nxt.y0 - l.y1) > 0.9 * st.leading;
      if (aloneBefore && aloneAfter && (
        (l.bold && !/[.,;]$/.test(t) && t.length <= 60) || (CHAPTER_RE.test(t) && t.length <= 60 && !hasBig))) {
        l.kind = "heading";
      }
    }
  }
  const sizes = [...new Set(pages.flatMap(pg => pg.lines.filter(l => l.kind === "heading").map(l => pyRound(l.size))))]
    .sort((a, b) => b - a);
  for (const pg of pages) for (const l of pg.lines) {
    if (l.kind !== "heading") continue;
    if (pyRound(l.size) > pyRound(st.bodySize)) l.level = sizes.indexOf(pyRound(l.size)) + 1;
    else {
      const bigLevels = sizes.filter(s => s > pyRound(st.bodySize));
      l.level = CHAPTER_RE.test(strip(lineText(l))) && !bigLevels.length ? 1 : bigLevels.length + 1;
    }
  }
  const used = [...new Set(pages.flatMap(pg => pg.lines.filter(l => l.kind === "heading").map(l => l.level)))]
    .sort((a, b) => a - b);
  const remap = new Map(used.map((lv, i) => [lv, Math.min(i + 1, 3)]));
  const heads = pages.flatMap(pg => pg.lines.filter(l => l.kind === "heading"));
  for (const l of heads) l.level = remap.get(l.level);
  const tops = heads.filter(l => l.level === 1);
  const numbered = tops.filter(l => CHAPTER_NUM_RE.test(strip(lineText(l))));
  if (numbered.length >= 2 && numbered.length < tops.length) {
    let inChapter = false, prev = null;
    for (const l of heads) {
      const t = strip(lineText(l));
      const wrapped = prev && prev.page === l.page && Math.abs(prev.size - l.size) < 0.6 &&
        l.y0 - prev.y1 >= 0 && l.y0 - prev.y1 < 1.5 * l.size;
      if (wrapped) l.level = prev.level;
      else if (l.level === 1 && (CHAPTER_NUM_RE.test(t) || BACK_MATTER_RE.test(t))) inChapter = CHAPTER_NUM_RE.test(t);
      else if (inChapter) l.level = Math.min(l.level + 1, 4);
      prev = l;
    }
  }
}

// --------------------------------------------------------------------------- paragraphs

class Dehyphenator {
  constructor(pages) {
    const words = new Counter(), hyph = new Counter();
    for (const pg of pages) for (const l of pg.lines) {
      if (l.kind === "image") continue;
      let toks = lineText(l).toLowerCase().split(/\s+/u).filter(Boolean).slice(1);
      if (toks.length && HYPHENS.includes(toks[toks.length - 1].slice(-1))) toks = toks.slice(0, -1);
      const t = toks.join(" ");
      for (const m of t.matchAll(/\p{L}+(?:-\p{L}+)+/gu)) hyph.add(m[0]);
      for (const m of t.matchAll(/\p{L}+/gu)) words.add(m[0]);
    }
    this.words = words; this.hyph = hyph;
    const lines = pages.flatMap(pg => pg.lines.filter(l => l.kind === "body"));
    const endRe = new RegExp(`\\p{L}[${HYPHENS}]$`, "u");
    const ends = lines.filter(l => endRe.test(rstrip(lineText(l)))).length;
    this.autoHyphenated = lines.length < 200 || ends > 0.01 * lines.length;
    this.joined = 0; this.kept = 0;
  }

  keepHyphen(left, right) {
    const a = left.match(/(\p{L}+)$/u);
    const b = right.match(/^(\p{L}+)/u);
    if (!a || !b) return true;
    const lw = a[1].toLowerCase(), rw = b[1].toLowerCase();
    const joined = this.words.count(lw + rw), hyphened = this.hyph.count(lw + "-" + rw);
    if (hyphened > joined) return true;
    if (joined) return false;
    if (!this.autoHyphenated) return true;
    if ((/\p{L}-\p{L}+$/u.test(left) || /^\p{L}+-\p{L}/u.test(right)) && this.words.count(lw) && this.words.count(rw)) return true;
    if (COMPOUND_PREFIXES.has(lw) && this.words.count(rw)) return true;
    if (VOWEL_PREFIXES.has(lw) && rw.slice(0, 1) === lw.slice(-1)) return true;
    return false;
  }
}

function firstWordWidth(l, size) {
  const t = strip(lineText(l));
  const w = t ? t.split(" ")[0] : "";
  let total = 0;
  for (const ch of w) {
    const c = ch.codePointAt(0);
    total += c >= 32 && c <= 126 ? TIMES_W[c - 32] : 500;
  }
  return total / 1000 * size * 1.1;
}

function stripSegs(segs) {
  segs = segs.map(s => seg(s.text, s.bold, s.italic));
  while (segs.length && !strip(segs[0].text)) segs.shift();
  while (segs.length && !strip(segs[segs.length - 1].text)) segs.pop();
  if (segs.length) {
    segs[0].text = lstrip(segs[0].text);
    segs[segs.length - 1].text = rstrip(segs[segs.length - 1].text);
  }
  return segs;
}

function joinInto(cur, segs, dehy) {
  if (!segs.length) return;
  const last = cur.segs[cur.segs.length - 1];
  const lt = last.text, nt = segs[0].text;
  const lc = lt.slice(-1), n0 = nt.slice(0, 1);
  if (lt.endsWith("­")) {
    last.text = lt.slice(0, -1);
    dehy.joined++;
  } else if (lc && HYPHENS.includes(lc) && lt.length >= 2 && isAlpha(lt.slice(-2, -1)) && isAlpha(n0)) {
    const caps = /[A-Z]{2}$/.test(lt.slice(0, -1)) && isUpper(n0);
    if ((isLower(n0) || caps) && !dehy.keepHyphen(lt.slice(0, -1), nt)) {
      last.text = lt.slice(0, -1);
      dehy.joined++;
    } else {
      dehy.kept++;
      if (lc !== "-") last.text = lt.slice(0, -1) + "-";
    }
  } else if (lc && HYPHENS.includes(lc) && isDigit(n0)) {
    // "IEEE-" + "754"
  } else if ((lc && DASHES.includes(lc)) || lt.endsWith("--") || (n0 && DASHES.includes(n0))) {
    // em/en dash at a break: no space
  } else {
    last.text = lt + " ";
  }
  cur.segs.push(...segs);
}

function unifyEdges(pages, edgesByPage) {
  const lefts = new Map(), rights = new Map();
  const keyOf = (pg, col) => `${pg.number % 2}|${pg.twoCol}|${col}`;
  for (const pg of pages) for (const [col, [left, right]] of edgesByPage.get(pg.number)) {
    const k = keyOf(pg, col);
    if (!lefts.has(k)) { lefts.set(k, new Counter()); rights.set(k, new Counter()); }
    lefts.get(k).add(pyRound(left));
    rights.get(k).add(pyRound(right));
  }
  for (const pg of pages) {
    const edges = edgesByPage.get(pg.number);
    for (const [col, [left0, right0]] of [...edges]) {
      let left = left0, right = right0;
      const k = keyOf(pg, col);
      if (!lefts.has(k) || lefts.get(k).total() < 5) continue;
      const dl = lefts.get(k).mostCommon(1)[0][0];
      const dr = rights.get(k).mostCommon(1)[0][0];
      const body = pg.lines.filter(l => l.col === col && l.kind === "body");
      if (!body.length) continue;
      if (Math.min(...body.map(l => l.x0)) >= dl - 2) left = dl;
      if (Math.max(...body.map(l => l.x1)) <= dr + 2) right = dr;
      edges.set(col, [left, right]);
    }
  }
}

function paragraphBreak(a, b, st, edgesByPage) {
  const ea = edgesByPage.get(a.page).get(a.col) || [a.x0, a.x1];
  const eb = edgesByPage.get(b.page).get(b.col) || [b.x0, b.x1];
  const size = st.bodySize;
  const at = lineText(a), bt = lineText(b);
  if (Math.abs(a.size - b.size) > 1.0 || (a.bold !== b.bold && at.length < 60)) return [true, false];
  const sameFlow = a.page === b.page && a.col === b.col && b.y0 > a.y0;
  if (sameFlow) {
    const gap = (b.y1 - a.y1) / st.leading;
    if (gap > 3.5) return [true, true];
    if (gap > 1.35) return [true, false];
  }
  const bl = lstrip(bt);
  if (bl && BULLET_CHARS.slice(0, -3).includes(bl[0]) && bl.slice(1, 2) === " ") return [true, false];
  const ind = b.x0 - eb[0];
  let prevInd = st.indentStyle ? 0 : Math.max(0, a.x0 - ea[0]);
  if (LIST_MARK_RE.test(lstrip(at))) prevInd = Math.max(prevInd, ind);
  if (0.6 * size < ind - prevInd && ind < 6 * size) return [true, false];
  if (ind < -0.5 * b.size && b.x0 < a.x0 - 0.5 * b.size) return [true, false];
  const room = ea[1] - a.x1;
  if (room > firstWordWidth(b, b.size) + 1.5 * b.size) return [true, false];
  if (st.blockStyle && !sameFlow && TERMINAL.includes(rstrip(at).slice(-1) || "\u0001") &&
      /^["'“‘(]*[A-Z0-9]/.test(bl)) return [true, false];
  if (a.x0 - ea[0] > 6 * size && b.x0 - eb[0] > 6 * size) return [true, false];
  return [false, false];
}

function buildBlocks(pages, st, dehy) {
  const blocks = [];
  let cur = null, prev = null;
  const flush = () => {
    if (cur && cur.segs.length && strip(cur.segs.map(s => s.text).join(""))) blocks.push(cur);
    cur = null;
  };
  const edgesByPage = new Map();
  for (const pg of pages) edgesByPage.set(pg.number, columnEdges(pg, st));
  unifyEdges(pages, edgesByPage);

  let indented = 0, total = 0;
  for (const pg of pages) {
    const e = edgesByPage.get(pg.number);
    for (const l of pg.lines) {
      if (l.kind === "body" && e.has(l.col)) {
        total++;
        const d = l.x0 - e.get(l.col)[0];
        if (d > 0.6 * st.bodySize && d < 6 * st.bodySize) indented++;
      }
    }
  }
  st.blockStyle = total > 50 && indented < 0.01 * total;
  const starts = [];
  for (const pg of pages) {
    const e = edgesByPage.get(pg.number);
    const body = pg.lines.filter(l => l.kind === "body");
    for (let i = 0; i + 1 < body.length; i++) {
      const a = body[i], b = body[i + 1];
      if (a.col !== b.col || !e.has(a.col) || b.y0 <= a.y0) continue;
      const [left, right] = e.get(a.col);
      if (b.startsPara || LIST_MARK_RE.test(lstrip(lineText(b))) || Math.abs(a.x0 - left) > 2) continue;
      const gap = (b.y1 - a.y1) / st.leading;
      if (((right - a.x1) > 0.25 * (right - left) && gap < 1.35) || (gap >= 1.35 && gap < 2.6)) {
        starts.push(b.x0 - left > 0.6 * st.bodySize);
      }
    }
  }
  if (starts.length >= 10) st.indentStyle = starts.filter(Boolean).length >= 0.6 * starts.length;

  for (const pg of pages) {
    let run = [];
    const close = () => {
      if (run.length) { const m = Math.min(...run.map(l => l.x0)); for (const l of run) l._indentFrom = m; }
      run = [];
    };
    for (const l of pg.lines) {
      if (l.kind === "code") run.push(l);
      else if (l.kind !== "dropcap") close();
    }
    close();
  }

  for (const pg of pages) {
    for (const l of pg.lines) {
      if (l.kind === "dropcap") continue;
      if (l.kind === "image") {
        flush();
        blocks.push({ kind: "img", segs: [], image: l.image, level: 1, page: pg.number });
        prev = null;
        continue;
      }
      if (l.kind === "table") {
        flush();
        const last = blocks[blocks.length - 1];
        if (last && last.kind === "table" && last.page === pg.number - 1 &&
            last.table.cols === l.table.cols && !l.table.caption) {
          // the same table continued on the next page
          const same = l.table.rows.length && last.table.rows.length &&
            l.table.rows[0].join("\u0000") === last.table.rows[0].join("\u0000");
          const rows = last.table.header && same ? l.table.rows.slice(l.table.header) : l.table.rows;
          last.table.rows.push(...rows);
          last.page = pg.number;
        } else {
          blocks.push({ kind: "table", segs: [], table: l.table, level: 1, page: pg.number });
        }
        prev = null;
        continue;
      }
      if (l.kind === "code") {
        flush();
        const segs = l.segs.map(x => seg(x.text, x.bold, x.italic, x.mono));
        // indentation from the line's position within its run of code lines
        if (l._indentFrom !== undefined && l._cw && segs.length) {
          const n = Math.round((l.x0 - l._indentFrom) / l._cw);
          if (n > 0) segs[0].text = " ".repeat(n) + segs[0].text;
        }
        if (segs.length) segs[segs.length - 1].text = rstrip(segs[segs.length - 1].text);
        blocks.push({ kind: "code", segs, level: 1, page: pg.number });
        prev = l;
        continue;
      }
      if (l.kind === "heading") {
        const lastB = blocks[blocks.length - 1];
        if (lastB && lastB.kind === "h" && lastB.level === l.level && cur === null && prev && prev.kind === "heading" &&
            prev.page === l.page && (l.y0 - prev.y1) < 1.5 * l.size) {
          if (!HYPHENS.includes(lastB.segs[lastB.segs.length - 1].text.slice(-1) || "\u0001")) lastB.segs.push(seg(" "));
          lastB.segs.push(...stripSegs(l.segs));
        } else {
          flush();
          blocks.push({ kind: "h", segs: stripSegs(l.segs), level: l.level, page: pg.number });
        }
        prev = l;
        continue;
      }
      const segs = stripSegs(l.segs);
      if (cur === null || prev === null || prev.kind !== "body" || l.startsPara) {
        flush();
        cur = { kind: "p", segs, level: 1, page: pg.number };
        prev = l;
        continue;
      }
      const [newPara, scene] = paragraphBreak(prev, l, st, edgesByPage);
      if (newPara) {
        flush();
        if (scene) blocks.push({ kind: "sep", segs: [], level: 1, page: pg.number });
        cur = { kind: "p", segs, level: 1, page: pg.number };
      } else {
        joinInto(cur, segs, dehy);
      }
      prev = l;
    }
  }
  flush();
  return blocks;
}

// --------------------------------------------------------------------------- epub

function segsToHtml(segs, styles = true) {
  const merged = [];
  for (const s of segs) {
    const m = merged[merged.length - 1];
    if (m && m.bold === s.bold && m.italic === s.italic) m.text += s.text;
    else merged.push(seg(s.text, s.bold, s.italic));
  }
  return merged.map(s => {
    let t = escapeHtml(s.text.replace(/ {2,}/g, " "), false);
    if (styles && strip(s.text)) {
      const lead = t.slice(0, t.length - lstrip(t).length);
      const trail = t.slice(rstrip(t).length);
      let core = strip(t);
      if (s.italic) core = `<i>${core}</i>`;
      if (s.bold) core = `<b>${core}</b>`;
      t = lead + core + trail;
    }
    return t;
  }).join("");
}

const plain = segs => segs.map(s => s.text).join("").replace(/\s+/gu, " ").trim();

const CSS = `p { margin: 0; text-indent: 1.2em; }
h1, h2, h3 { text-align: center; }
p.sep { text-align: center; text-indent: 0; margin: 1em 0; }
td, th { text-align: left; vertical-align: top; }
span.ni { white-space: pre-wrap; }
pre { text-align: left; font-family: monospace; font-size: 0.8em; margin: 0.5em 0; }
`;

function xhtmlDoc(title, body, lang, css) {
  const link = css ? '<link rel="stylesheet" type="text/css" href="style.css"/>' : "";
  return '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n' +
    `<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="${lang}" xml:lang="${lang}">\n` +
    `<head><title>${escapeHtml(title)}</title>${link}</head>\n<body>\n${body}\n</body>\n</html>\n`;
}

function chapterLevel(blocks) {
  const counts = new Counter();
  for (const b of blocks) if (b.kind === "h") counts.add(b.level);
  const lvls = [...counts.keys()].sort((a, b) => a - b);
  for (const lv of lvls) if (counts.get(lv) >= 2) return lv;
  return lvls.length ? lvls[0] : null;
}

function splitChapters(blocks, maxBytes, top) {
  const chapters = [[]];
  for (const b of blocks) {
    const last = chapters[chapters.length - 1];
    if (b.kind === "h" && top !== null && b.level <= top && last.length && !last.every(x => x.kind === "h")) {
      chapters.push([]);
    }
    chapters[chapters.length - 1].push(b);
  }
  const out = [];
  for (const ch of chapters) {
    let part = [], size = 0;
    for (const b of ch) {
      let est = 40 + b.segs.reduce((n, s) => n + s.text.length, 0) * 1.1;
      if (b.kind === "table") est += b.table.rows.reduce((n, r) => n + r.reduce((m, c) => m + c.length + 12, 0), 0);
      if (part.length && size + est > maxBytes && (b.kind === "p" || b.kind === "sep" || b.kind === "img")) {
        out.push(part);
        part = []; size = 0;
      }
      part.push(b);
      size += est;
    }
    if (part.length) out.push(part);
  }
  return out.filter(p => p.length);
}

function navXhtml(toc, title, lang) {
  const out = [];
  let depth = 0;
  for (const [lv, label, href] of toc) {
    if (lv > depth) out.push("<ol>".repeat(lv - depth));
    else { out.push("</li>"); out.push("</ol></li>".repeat(depth - lv)); }
    out.push(`<li><a href="${href}">${escapeHtml(label)}</a>`);
    depth = lv;
  }
  out.push("</li>" + "</ol></li>".repeat(depth - 1) + "</ol>");
  const body = `<nav epub:type="toc" id="toc"><h1>Contents</h1>\n${out.join("")}\n</nav>`;
  return xhtmlDoc(title, body, lang, false);
}

function tocNcx(toc, title, bookId) {
  const out = [];
  let depth = 0, n = 0;
  for (const [lv, label, href] of toc) {
    n++;
    if (lv <= depth) out.push("</navPoint>".repeat(depth - lv + 1));
    out.push(`<navPoint id="np${n}" playOrder="${n}"><navLabel><text>${escapeHtml(label)}</text></navLabel><content src="${href}"/>`);
    depth = lv;
  }
  out.push("</navPoint>".repeat(depth));
  return `<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
<head><meta name="dtb:uid" content="${bookId}"/><meta name="dtb:depth" content="4"/>
<meta name="dtb:totalPageCount" content="0"/><meta name="dtb:maxPageNumber" content="0"/></head>
<docTitle><text>${escapeHtml(title)}</text></docTitle>
<navMap>${out.join("")}</navMap>
</ncx>
`;
}

function uuid4() {
  if (globalThis.crypto && globalThis.crypto.randomUUID) return globalThis.crypto.randomUUID();
  return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, c => {
    const r = Math.random() * 16 | 0;
    return (c === "x" ? r : (r & 3 | 8)).toString(16);
  });
}

async function writeEpub(JSZip, blocks, meta, opts, cover) {
  const { lang, css, maxKb, styles } = opts;
  const top = chapterLevel(blocks);
  const chapters = splitChapters(blocks, maxKb * 1024, top);
  const bookId = "urn:uuid:" + uuid4();
  const modified = new Date().toISOString().replace(/\.\d+Z$/, "Z");
  const { title, author } = meta;
  const files = [];
  const images = [];
  let toc = [];
  chapters.forEach((ch, idx) => {
    const i = idx + 1;
    const name = `ch${String(i).padStart(3, "0")}.xhtml`;
    const body = [];
    let docTitle = null;
    const hstack = [];
    ch.forEach((b, j) => {
      if (b.kind === "h") {
        const hid = `h${i}_${j}`;
        const lv = Math.min(b.level, 4);
        body.push(`<h${lv} id="${hid}">${segsToHtml(b.segs, false)}</h${lv}>`);
        const label = plain(b.segs);
        const lastT = toc[toc.length - 1];
        if (toc.length && hstack.length && hstack[hstack.length - 1] === j - 1 && lastT[1] && lastT[0] < lv) {
          toc[toc.length - 1] = [lastT[0], `${lastT[1]}: ${label}`, lastT[2]];
        } else {
          toc.push([lv, label, `${name}#${hid}`]);
        }
        hstack.push(j);
        docTitle = docTitle || label;
      } else if (b.kind === "p") {
        body.push(`<p>${segsToHtml(b.segs, styles)}</p>`);
      } else if (b.kind === "sep") {
        body.push('<p class="sep">* * *</p>');
      } else if (b.kind === "img") {
        const iname = `images/img${String(images.length + 1).padStart(4, "0")}.jpg`;
        images.push([iname, b.image]);
        body.push(`<p class="sep"><img src="${iname}" alt=""/></p>`);
      } else if (b.kind === "table") {
        body.push(tableHtml(b.table));
      } else if (b.kind === "code") {
        const line = escapeHtml(b.segs.map(x => x.text).join(""), false);
        if (j > 0 && ch[j - 1].kind === "code" && body.length && body[body.length - 1].endsWith("</pre>")) {
          body[body.length - 1] = body[body.length - 1].slice(0, -6) + "\n" + line + "</pre>";
        } else {
          body.push("<pre>" + line + "</pre>");
        }
      }
    });
    files.push([name, xhtmlDoc(docTitle || title, body.join("\n"), lang, css)]);
  });
  if (!toc.length) toc.push([1, title, files[0][0]]);
  const shift = (top || 1) - 1;
  let last = 0;
  toc = toc.map(([lv, label, href]) => {
    lv = Math.min(Math.max(1, lv - shift), last + 1);
    last = lv;
    return [lv, label || "Untitled", href];
  });

  const manifest = ['<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
    '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'];
  if (css) manifest.push('<item id="css" href="style.css" media-type="text/css"/>');
  const spine = [];
  files.forEach(([name], k) => {
    manifest.push(`<item id="c${k + 1}" href="${name}" media-type="application/xhtml+xml"/>`);
    spine.push(`<itemref idref="c${k + 1}"/>`);
  });
  images.forEach(([iname], k) => manifest.push(`<item id="img${k + 1}" href="${iname}" media-type="image/jpeg"/>`));
  let coverMeta = "";
  if (cover) {
    manifest.push('<item id="cover-image" href="images/cover.jpg" media-type="image/jpeg" properties="cover-image"/>');
    coverMeta = '<meta name="cover" content="cover-image"/>';
  }
  const opf = `<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid" xml:lang="${lang}">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="bookid">${bookId}</dc:identifier>
<dc:title>${escapeHtml(title)}</dc:title>
<dc:creator>${escapeHtml(author)}</dc:creator>
<dc:language>${lang}</dc:language>
<meta property="dcterms:modified">${modified}</meta>
${coverMeta}
</metadata>
<manifest>
${manifest.join("\n")}
</manifest>
<spine toc="ncx">
${spine.join("\n")}
</spine>
</package>
`;
  const container = `<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
`;
  const z = new JSZip();
  z.file("mimetype", "application/epub+zip", { compression: "STORE" });
  const def = { compression: "DEFLATE" };
  z.file("META-INF/container.xml", container, def);
  z.file("OEBPS/content.opf", opf, def);
  z.file("OEBPS/nav.xhtml", navXhtml(toc, title, lang), def);
  z.file("OEBPS/toc.ncx", tocNcx(toc, title, bookId), def);
  if (css) z.file("OEBPS/style.css", CSS, def);
  for (const [name, data] of files) z.file("OEBPS/" + name, data, def);
  for (const [iname, data] of images) z.file("OEBPS/" + iname, data, { compression: "STORE" });
  if (cover) z.file("OEBPS/images/cover.jpg", cover, { compression: "STORE" });
  const epub = await z.generateAsync({ type: "uint8array", mimeType: "application/epub+zip" });
  return { epub, nFiles: files.length, nToc: toc.length, nImages: images.length };
}

// --------------------------------------------------------------------------- driver

function parsePages(spec, n) {
  if (!spec) return [...Array(n).keys()];
  const sel = [];
  for (let part of String(spec).split(",")) {
    part = part.trim();
    if (part.includes("-")) {
      const [a, b] = part.split("-", 2);
      const lo = a ? parseInt(a, 10) : 1, hi = b ? parseInt(b, 10) : n;
      for (let p = lo - 1; p < Math.min(hi, n); p++) sel.push(p);
    } else if (part) sel.push(parseInt(part, 10) - 1);
  }
  return sel.filter(p => p >= 0 && p < n);
}

// Renders page 1 to a grayscale JPEG that fits 480x800. Browser only (needs a canvas).
async function renderCoverBrowser(pdf) {
  if (typeof document === "undefined") return null;
  const page = await pdf.getPage(1);
  const v1 = page.getViewport({ scale: 1 });
  const zoom = Math.min(SCREEN_W / v1.width, SCREEN_H / v1.height);
  const vp = page.getViewport({ scale: zoom });
  const canvas = document.createElement("canvas");
  canvas.width = Math.floor(vp.width); canvas.height = Math.floor(vp.height);
  const ctx = canvas.getContext("2d");
  ctx.fillStyle = "#fff"; ctx.fillRect(0, 0, canvas.width, canvas.height);
  await page.render({ canvasContext: ctx, viewport: vp }).promise;
  const img = ctx.getImageData(0, 0, canvas.width, canvas.height);
  const d = img.data;
  for (let i = 0; i < d.length; i += 4) {
    const g = 0.299 * d[i] + 0.587 * d[i + 1] + 0.114 * d[i + 2];
    d[i] = d[i + 1] = d[i + 2] = g;
  }
  ctx.putImageData(img, 0, 0);
  const blob = await new Promise(r => canvas.toBlob(r, "image/jpeg", 0.8));
  return blob ? new Uint8Array(await blob.arrayBuffer()) : null;
}

/**
 * Convert PDF bytes to EPUB bytes.
 * opts: title, author, lang ("en"), pages ("5-200"), margin (0.12), strip ([regex strings]),
 *       keepHeaders, noHeadings, cover (true; browser only), css (true), styles (true), maxKb (80),
 *       fileName (used for the default title), onProgress(done, total).
 */
async function convertPdf(pdfjsLib, JSZip, data, opts = {}) {
  const o = Object.assign({ lang: "en", margin: 0.12, strip: [], keepHeaders: false, noHeadings: false,
    cover: true, images: true, css: true, styles: true, maxKb: 80 }, opts);
  // plain pixel arrays for images (not ImageBitmaps), so they can be turned gray and resized here
  const pdf = await pdfjsLib.getDocument({ data, verbosity: 0, isEvalSupported: false, fontExtraProperties: true,
    isOffscreenCanvasSupported: false, isImageDecoderSupported: false }).promise;
  try {
    let md = {};
    try { md = (await pdf.getMetadata()).info || {}; } catch (e) { /* no metadata */ }
    const baseName = (o.fileName || "Untitled").replace(/\.pdf$/i, "");
    const title = o.title || strip(md.Title || "") || baseName;
    const author = o.author || strip(md.Author || "") || "Unknown";
    const sel = parsePages(o.pages, pdf.numPages);
    const pages = [];
    for (let k = 0; k < sel.length; k++) {
      pages.push(await extractPage(pdf, pdfjsLib.OPS, sel[k], sel[k] + 1, { images: o.images, encodeJpeg: o.encodeJpeg }));
      if (o.onProgress) o.onProgress(k + 1, sel.length);
    }
    const st = bodyStats(pages);
    const patterns = (o.strip || []).filter(Boolean).map(p => new RegExp(p, "iu"));
    let removed = new Counter();
    if (!o.keepHeaders) removed = stripHeadersFooters(pages, st, o.margin, patterns);
    for (const pg of pages) assignColumns(pg);
    if (!o.noHeadings) markHeadings(pages, st);
    for (const pg of pages) {
      for (const t of pg.tables || []) {
        let pos = pg.lines.findIndex(l => l.y0 > t.y0 && l.col === 0);
        if (pos < 0) pos = pg.lines.length;
        pg.lines.splice(pos, 0, t);
      }
    }
    const dehy = new Dehyphenator(pages);
    const blocks = buildBlocks(pages, st, dehy);
    let cover = null;
    if (o.cover) { try { cover = await renderCoverBrowser(pdf); } catch (e) { cover = null; } }
    const { epub, nFiles, nToc, nImages } = await writeEpub(JSZip, blocks, { title, author },
      { lang: o.lang, css: o.css, maxKb: o.maxKb, styles: o.styles }, cover);
    const nPar = blocks.filter(b => b.kind === "p").length;
    const summary = { pages: pages.length, paragraphs: nPar, tocEntries: nToc, files: nFiles,
      images: nImages, tables: blocks.filter(b => b.kind === "table").length,
      headerFooterLinesRemoved: removed.total(), hyphenationsRejoined: dehy.joined, hyphensKept: dehy.kept };
    const report = {
      body: `${st.bodySize}pt '${st.bodyFont}', line spacing ${st.leading.toFixed(1)}pt`,
      removed: removed.mostCommon(40),
      headings: blocks.filter(b => b.kind === "h").map(b => ({ level: b.level, page: b.page, text: plain(b.segs) })),
    };
    return { epub, title, author, summary, report };
  } finally {
    pdf.destroy();
  }
}

// For tests.

window.Pdf2Epub = { convertPdf, parsePages, VERSION };
})();
