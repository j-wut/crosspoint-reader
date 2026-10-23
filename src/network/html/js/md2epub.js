// md2epub.js: turns a Markdown file into a plain EPUB for CrossPoint, in the browser.
//
// Markdown is parsed with marked (served by the reader as /js/marked.min.js) and the EPUB is
// zipped with JSZip; both are passed in, so this file has no imports:
//
//   const refs = Md2Epub.imageRefs(marked, text);            // image paths the text uses
//   const { epub, title, summary } = await Md2Epub.convertMarkdown(marked, JSZip, text, {
//     fileName: "notes.md",
//     resolveImage: async href => ({ data: Uint8Array }) | null,
//   });
//
// Headings split the book into chapters and fill the table of contents. Lists, tables, code
// blocks, block quotes and images are kept. Links keep only their text, and raw HTML keeps
// only its text (plus <br> and <img>), because the reader cannot follow or lay them out.
(function () {
"use strict";

const MAX_CHAPTER_BYTES = 80 * 1024;
const XML_BAD_RE = /[\x00-\x08\x0b\x0c\x0e-\x1f￾￿]/g;
const XML_ENTITIES = new Set(["amp", "lt", "gt", "quot", "apos"]);

// The reader ignores most CSS; these rules only fix table cell indents and code layout.
const CSS = `td, th { text-align: left; vertical-align: top; }
span.ni { white-space: pre-wrap; }
pre { text-align: left; font-family: monospace; font-size: 0.8em; margin: 0.5em 0; }
`;

const esc = s => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");

// XHTML only knows the five XML entities, so named HTML entities (&nbsp;, &copy;) become characters.
const FALLBACK_ENTITIES = { nbsp: " ", copy: "©", reg: "®", trade: "™", mdash: "—",
  ndash: "–", hellip: "…", lsquo: "‘", rsquo: "’", ldquo: "“", rdquo: "”",
  times: "×", deg: "°", middot: "·", bull: "•", larr: "←", rarr: "→" };
let entityEl = null;
function decodeEntity(name) {
  if (typeof document !== "undefined") {
    entityEl = entityEl || document.createElement("textarea");
    entityEl.innerHTML = `&${name};`;
    const v = entityEl.value;
    return v === `&${name};` ? null : v;
  }
  return FALLBACK_ENTITIES[name] || null;
}
function fixEntities(html) {
  return html.replace(/&([A-Za-z][A-Za-z0-9]{1,31});/g, (m, name) => {
    if (XML_ENTITIES.has(name)) return m;
    const v = decodeEntity(name);
    return v === null ? "&amp;" + name + ";" : esc(v);
  });
}

// Leading YAML front matter: only title, author and lang are used.
function frontMatter(text) {
  const m = /^---[ \t]*\r?\n([\s\S]*?)\r?\n(?:---|\.\.\.)[ \t]*(?:\r?\n|$)/.exec(text);
  if (!m) return { meta: {}, body: text };
  const meta = {};
  for (const line of m[1].split(/\r?\n/)) {
    const kv = /^(title|author|lang|language)\s*:\s*(.+?)\s*$/i.exec(line);
    if (kv) meta[kv[1].toLowerCase().replace("language", "lang")] = kv[2].replace(/^(["'])(.*)\1$/, "$2");
  }
  return { meta, body: text.slice(m[0].length) };
}

function plainText(tokens) {
  let out = "";
  for (const t of tokens || []) {
    if (t.tokens) out += plainText(t.tokens);
    else if (t.type === "html") out += t.text.replace(/<[^>]*>/g, "");
    else if (t.type !== "image") out += t.text || "";
  }
  return out.replace(/\s+/g, " ").trim();
}

const IMG_TAG_RE = /<img\b[^>]*>/gi;
const attr = (tag, name) => {
  const m = new RegExp(`\\b${name}\\s*=\\s*(?:"([^"]*)"|'([^']*)'|([^\\s>]+))`, "i").exec(tag);
  return m ? (m[1] ?? m[2] ?? m[3]) : "";
};

function imageRefs(marked, text) {
  const refs = new Set();
  marked.walkTokens(marked.lexer(frontMatter(text.replace(XML_BAD_RE, "")).body), t => {
    if (t.type === "image" && t.href) refs.add(t.href);
    if (t.type === "html") for (const tag of t.text.match(IMG_TAG_RE) || []) if (attr(tag, "src")) refs.add(attr(tag, "src"));
  });
  return [...refs];
}

function imageType(d) {
  if (d.length > 3 && d[0] === 0xff && d[1] === 0xd8) return ["jpg", "image/jpeg"];
  if (d.length > 8 && d[0] === 0x89 && d[1] === 0x50 && d[2] === 0x4e && d[3] === 0x47) return ["png", "image/png"];
  if (d.length > 6 && d[0] === 0x47 && d[1] === 0x49 && d[2] === 0x46) return ["gif", "image/gif"];
  if (d.length > 12 && d[0] === 0x52 && d[8] === 0x57 && d[9] === 0x45 && d[10] === 0x42 && d[11] === 0x50) {
    return ["webp", "image/webp"];
  }
  return null;
}

function makeRenderer(images) {
  const imageHtml = (href, alt) => {
    const img = images.get(href);
    if (img) return `<img src="${img.path}" alt="${esc(alt || "")}"/>`;
    return alt ? `<i>[${esc(alt)}]</i>` : "";
  };
  // Raw HTML: keep its text, line breaks and images; drop the tags.
  const rawHtml = html => {
    let out = html.replace(/<!--[\s\S]*?-->/g, "").replace(/<br\s*\/?>/gi, "\u0001")
      .replace(IMG_TAG_RE, tag => "\u0002" + encodeURIComponent(attr(tag, "src")) + "\u0003" + encodeURIComponent(attr(tag, "alt")) + "\u0004")
      .replace(/<\/?[A-Za-z][^>]*>/g, "");
    out = out.replace(/&(?!#?\w+;)/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    return out.replace(/\u0001/g, "<br/>")
      .replace(/\u0002([^\u0003]*)\u0003([^\u0004]*)\u0004/g, (m, s, a) => imageHtml(decodeURIComponent(s), decodeURIComponent(a)));
  };
  return {
    heading(t) {
      const d = Math.min(t.depth, 6);
      const id = t._id ? ` id="${t._id}"` : "";
      return `<h${d}${id}>${this.parser.parseInline(t.tokens)}</h${d}>\n`;
    },
    code(t) { return `<pre>${esc(t.text.replace(/\n+$/, ""))}</pre>\n`; },
    html(t) {
      const inner = rawHtml(t.text);
      if (!t.block) return inner;
      return inner.trim() ? `<p>${inner.trim()}</p>\n` : "";
    },
    hr() { return "<hr/>\n"; },
    br() { return "<br/>"; },
    link(t) { return this.parser.parseInline(t.tokens); },
    image(t) { return imageHtml(t.href, t.text); },
    checkbox(t) { return t.checked ? "[x] " : "[ ] "; },
    // No thead/tbody: the reader's table layout only knows table, tr, th and td.
    table(t) {
      const row = cells => "<tr>" + cells.map(c => this.tablecell(c)).join("") + "</tr>\n";
      return "<table>\n" + row(t.header) + t.rows.map(row).join("") + "</table>\n";
    },
    // The reader gives table cells the paragraph indent unless the first word sits in a
    // pre-wrap span. Only the first word: pre-wrap makes every space its own layout word.
    tablecell(c) {
      const tag = c.header ? "th" : "td";
      const inner = this.parser.parseInline(c.tokens).replace(/^((?:<[^>]+>)*)([^<\s]+)/, '$1<span class="ni">$2</span>');
      return `<${tag}>${inner}</${tag}>`;
    },
  };
}

// The EPUB writer below mirrors writeEpub in pdf2epub.js; a fix for how the reader lays out
// EPUBs (span.ni, pre, NCX) likely belongs in both.

// The chapter level is the highest heading level used at least twice (so a lone title
// heading does not make the whole book one chapter). The level below it also starts new
// chapters (Part > Chapter) when that still leaves about 2 KB of text per chapter.
function chapterLevel(tokens) {
  const counts = new Map();
  for (const t of tokens) if (t.type === "heading") counts.set(t.depth, (counts.get(t.depth) || 0) + 1);
  const levels = [...counts.keys()].sort((a, b) => a - b);
  const top = levels.find(lv => counts.get(lv) >= 2) ?? levels[0] ?? null;
  let split = top;
  if (top !== null && counts.has(top + 1)) {
    const bytes = tokens.reduce((n, t) => n + (t.raw || "").length, 0);
    const n = levels.filter(lv => lv <= top + 1).reduce((s, lv) => s + counts.get(lv), 0);
    if (bytes / n >= 2048) split = top + 1;
  }
  return { top, split };
}

function splitChapters(tokens, top) {
  const chapters = [[]];
  for (const t of tokens) {
    const last = chapters[chapters.length - 1];
    if (t.type === "heading" && top !== null && t.depth <= top && last.some(x => x.type !== "heading" && x.type !== "space")) {
      chapters.push([]);
    }
    chapters[chapters.length - 1].push(t);
  }
  // Long chapters are cut between blocks so each file stays small enough to lay out quickly.
  const out = [];
  for (const ch of chapters) {
    let part = [], size = 0;
    for (const t of ch) {
      const n = (t.raw || "").length * 1.2 + 20;
      if (part.length && size + n > MAX_CHAPTER_BYTES && t.type !== "heading" && t.type !== "space") {
        out.push(part);
        part = []; size = 0;
      }
      part.push(t);
      size += n;
    }
    if (part.length) out.push(part);
  }
  return out;
}

function xhtmlDoc(title, body, lang, css) {
  const link = css ? '<link rel="stylesheet" type="text/css" href="style.css"/>' : "";
  return '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n' +
    `<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" lang="${lang}" xml:lang="${lang}">\n` +
    `<head><title>${esc(title)}</title>${link}</head>\n<body>\n${body}\n</body>\n</html>\n`;
}

function navXhtml(toc, title, lang) {
  const out = [];
  let depth = 0;
  for (const [lv, label, href] of toc) {
    if (lv > depth) out.push("<ol>".repeat(lv - depth));
    else { out.push("</li>"); out.push("</ol></li>".repeat(depth - lv)); }
    out.push(`<li><a href="${href}">${esc(label)}</a>`);
    depth = lv;
  }
  out.push("</li>" + "</ol></li>".repeat(depth - 1) + "</ol>");
  return xhtmlDoc(title, `<nav epub:type="toc" id="toc"><h1>Contents</h1>\n${out.join("")}\n</nav>`, lang, false);
}

function tocNcx(toc, title, bookId) {
  const out = [];
  let depth = 0, n = 0;
  for (const [lv, label, href] of toc) {
    n++;
    if (lv <= depth) out.push("</navPoint>".repeat(depth - lv + 1));
    out.push(`<navPoint id="np${n}" playOrder="${n}"><navLabel><text>${esc(label)}</text></navLabel><content src="${href}"/>`);
    depth = lv;
  }
  out.push("</navPoint>".repeat(depth));
  return `<?xml version="1.0" encoding="utf-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
<head><meta name="dtb:uid" content="${bookId}"/><meta name="dtb:depth" content="3"/>
<meta name="dtb:totalPageCount" content="0"/><meta name="dtb:maxPageNumber" content="0"/></head>
<docTitle><text>${esc(title)}</text></docTitle>
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

async function convertMarkdown(marked, JSZip, text, opts = {}) {
  const fileName = opts.fileName || "book.md";
  const { meta, body } = frontMatter(String(text).replace(/^﻿/, "").replace(XML_BAD_RE, ""));
  const md = new marked.Marked({ gfm: true });
  const tokens = md.lexer(body);

  // Images are fetched before rendering, because the renderer is synchronous.
  // They load in parallel, so slow web images cost one timeout, not one each.
  const images = new Map();
  let missingImages = 0;
  const refs = imageRefs(marked, body);
  const loaded = await Promise.all(refs.map(async href => {
    try {
      const r = opts.resolveImage ? await opts.resolveImage(href) : null;
      return r && r.data ? new Uint8Array(r.data) : null;
    } catch (e) {
      return null;
    }
  }));
  refs.forEach((href, i) => {
    const data = loaded[i];
    const type = data && imageType(data);
    if (!type) { missingImages++; return; }
    images.set(href, { path: `images/img${String(images.size + 1).padStart(4, "0")}.${type[0]}`, type: type[1], data });
  });
  md.use({ renderer: makeRenderer(images) });

  const headings = tokens.filter(t => t.type === "heading");
  const h1s = headings.filter(t => t.depth === 1);
  const baseName = fileName.replace(/^.*[\\/]/, "").replace(/\.(md|markdown)$/i, "");
  const title = meta.title || (h1s.length === 1 ? plainText(h1s[0].tokens) : "") || baseName || "Untitled";
  const author = meta.author || "Unknown";
  const lang = /^[A-Za-z]{2,3}(-[A-Za-z0-9]{1,8})*$/.test(meta.lang || "") ? meta.lang : "en";

  if (!tokens.some(t => t.type !== "space")) throw new Error("the Markdown file has no text");
  const { top, split } = chapterLevel(tokens);
  const chapters = splitChapters(tokens, split);
  const tocDepth = (top || 1) + 2;
  let toc = [];
  const files = [];
  chapters.forEach((ch, idx) => {
    const name = `ch${String(idx + 1).padStart(3, "0")}.xhtml`;
    let docTitle = null;
    ch.forEach((t, j) => {
      if (t.type !== "heading") return;
      t._id = `h${idx + 1}_${j}`;
      const label = plainText(t.tokens) || "Untitled";
      docTitle = docTitle || label;
      if (t.depth <= tocDepth) toc.push([t.depth, label, `${name}#${t._id}`]);
    });
    const html = fixEntities(md.parser(ch));
    files.push([name, xhtmlDoc(docTitle || title, html, lang, true)]);
    if (opts.onProgress) opts.onProgress(idx + 1, chapters.length);
  });
  if (!toc.length) toc.push([1, title, files[0][0]]);
  // TOC levels start at 1 and never skip a level.
  const shift = (top || 1) - 1;
  let last = 0;
  toc = toc.map(([lv, label, href]) => {
    lv = Math.min(Math.max(1, lv - shift), last + 1);
    last = lv;
    return [lv, label, href];
  });

  const bookId = "urn:uuid:" + uuid4();
  const modified = new Date().toISOString().replace(/\.\d+Z$/, "Z");
  const manifest = ['<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
    '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>',
    '<item id="css" href="style.css" media-type="text/css"/>'];
  const spine = [];
  files.forEach(([name], k) => {
    manifest.push(`<item id="c${k + 1}" href="${name}" media-type="application/xhtml+xml"/>`);
    spine.push(`<itemref idref="c${k + 1}"/>`);
  });
  let k = 0;
  for (const img of images.values()) manifest.push(`<item id="img${++k}" href="${img.path}" media-type="${img.type}"/>`);
  const opf = `<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid" xml:lang="${lang}">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="bookid">${bookId}</dc:identifier>
<dc:title>${esc(title)}</dc:title>
<dc:creator>${esc(author)}</dc:creator>
<dc:language>${lang}</dc:language>
<meta property="dcterms:modified">${modified}</meta>
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
  z.file("OEBPS/style.css", CSS, def);
  for (const [name, data] of files) z.file("OEBPS/" + name, data, def);
  for (const img of images.values()) z.file("OEBPS/" + img.path, img.data, { compression: "STORE" });
  const epub = await z.generateAsync({ type: "uint8array", mimeType: "application/epub+zip" });
  return {
    epub, title,
    summary: { chapters: files.length, headings: headings.length, images: images.size, missingImages },
  };
}

const api = { convertMarkdown, imageRefs };
if (typeof module !== "undefined" && module.exports) module.exports = api;
else globalThis.Md2Epub = api;
})();
