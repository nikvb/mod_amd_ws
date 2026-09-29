#!/usr/bin/env python3
"""Build one self-contained HTML documentation page from README.md and docs/*.md.

    python3 tools/make-docs-html.py [out.html]      (default: build/mod_amd_ws.html)

Needs the Python 'markdown' package (tables, fenced code, toc extensions).
"""
import html
import os
import re
import sys
import datetime

import markdown

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (file, anchor, nav title); README is split at its own H2 sections
PAGES = [
    ("docs/performance.md", "performance", "Performance report"),
    ("docs/protocol.md", "protocol", "Wire protocol"),
    ("docs/architecture.md", "architecture", "Architecture"),
    ("docs/installer.md", "installer", "Installer"),
    ("test/README.md", "tests", "Tests"),
    ("bench/README.md", "benchmark", "Benchmark"),
    ("CHANGELOG.md", "changelog", "Changelog"),
]
LINKMAP = {
    "docs/performance.md": "#performance", "performance.md": "#performance",
    "docs/protocol.md": "#protocol", "protocol.md": "#protocol",
    "docs/architecture.md": "#architecture", "architecture.md": "#architecture",
    "docs/installer.md": "#installer", "installer.md": "#installer",
    "test/README.md": "#tests", "bench/README.md": "#benchmark", "../docs/performance.md": "#performance",
    "CHANGELOG.md": "#changelog",
}
GH = "https://github.com/nikvb/mod_amd_ws/blob/main/"


def slug(s):
    s = re.sub(r"<[^>]+>", "", s)
    s = re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")
    return s or "s"


def fix_links(md_text, base, prefix=None):
    def rep(m):
        text, target = m.group(1), m.group(2)
        path, _, frag = target.partition("#")
        if target.startswith("#") and prefix:
            return "[%s](#%s-%s)" % (text, prefix, target[1:])
        if target.startswith(("http://", "https://", "#", "mailto:")):
            return m.group(0)
        if path in LINKMAP:
            page = LINKMAP[path][1:]
            return "[%s](#%s)" % (text, page + "-" + frag if frag else page)
        # other repo files -> GitHub
        full = os.path.normpath(os.path.join(os.path.dirname(base), path))
        return "[%s](%s%s%s)" % (text, GH, full, "#" + frag if frag else "")
    return re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", rep, md_text)


def render(md_text, prefix):
    md = markdown.Markdown(extensions=["tables", "fenced_code", "toc", "sane_lists"],
                           extension_configs={"toc": {"slugify": lambda v, sep: prefix + "-" + slug(v)}})
    body = md.convert(md_text)
    return body, md.toc_tokens


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "mod_amd_ws.html")
    version = re.search(r'#define MOD_AMD_WS_VERSION "([^"]+)"', open(os.path.join(ROOT, "src/mod_amd_ws.c")).read()).group(1)

    sections = []   # (anchor, title, html, subs)
    readme = fix_links(open(os.path.join(ROOT, "README.md")).read(), "README.md")
    head, *parts = re.split(r"(?m)^## ", readme)
    intro = re.sub(r"(?m)^# .*\n", "", head)
    ih, _ = render(intro, "overview")
    sections.append(("overview", "Overview", ih, []))
    for p in parts:
        title, _, rest = p.partition("\n")
        a = slug(title)
        body, toc = render(rest, a)
        subs = [(t["id"], t["name"]) for t in toc]
        sections.append((a, title.strip(), body, subs))
    for f, a, title in PAGES:
        text = fix_links(open(os.path.join(ROOT, f)).read(), f, a)
        text = re.sub(r"(?m)^# .*\n", "", text, count=1)
        body, toc = render(text, a)
        subs = []
        for t in toc:
            subs.append((t["id"], t["name"]))
        sections.append((a, title, body, subs))

    nav = []
    for a, title, _, subs in sections:
        nav.append('<li><a href="#%s">%s</a>' % (a, html.escape(title)))
        if subs and a in ("performance", "dialplan", "installer", "protocol"):
            nav.append("<ul>" + "".join('<li><a href="#%s">%s</a></li>' % (i, n) for i, n in subs[:9]) + "</ul>")
        nav.append("</li>")
    content = []
    for a, title, body, _ in sections:
        content.append('<section id="%s"><h2>%s</h2>%s</section>' % (a, html.escape(title), body))

    page = TEMPLATE.replace("@VERSION@", version).replace("@DATE@", datetime.date.today().isoformat()) \
        .replace("@NAV@", "\n".join(nav)).replace("@CONTENT@", "\n".join(content))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "w").write(page)
    print("wrote", out, len(page), "bytes")


TEMPLATE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>mod_amd_ws documentation</title>
<meta name="description" content="mod_amd_ws: answering machine detection for FreeSWITCH over WebSocket. Installation, dialplan, configuration, protocol and native mod_amd comparison.">
<style>
:root{--bg:#fbfbfa;--panel:#ffffff;--text:#1d1f23;--muted:#5d636d;--line:#e3e4e8;--accent:#0b6bcb;--accent-soft:#e8f1fb;
--code-bg:#f3f4f6;--code-text:#1d1f23;--good:#1a7f37;--warn:#9a6700;--nav-w:290px;
--mono:ui-monospace,SFMono-Regular,"JetBrains Mono",Menlo,Consolas,monospace;
--sans:-apple-system,BlinkMacSystemFont,"Segoe UI",Inter,Roboto,Helvetica,Arial,sans-serif}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#0f1115;--panel:#161a20;--text:#e6e8eb;--muted:#9aa2ad;
--line:#2a2f37;--accent:#5aa8ff;--accent-soft:#16263a;--code-bg:#1c2129;--code-text:#e6e8eb}}
:root[data-theme="dark"]{--bg:#0f1115;--panel:#161a20;--text:#e6e8eb;--muted:#9aa2ad;--line:#2a2f37;--accent:#5aa8ff;
--accent-soft:#16263a;--code-bg:#1c2129;--code-text:#e6e8eb}
*{box-sizing:border-box}
html{scroll-behavior:smooth;scroll-padding-top:16px}
html,body{overflow-x:hidden}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.6 var(--sans)}
a{color:var(--accent);text-decoration:none}a:hover{text-decoration:underline}
header.top{position:sticky;top:0;z-index:5;display:flex;align-items:center;gap:12px;padding:10px 20px;
background:var(--panel);border-bottom:1px solid var(--line)}
header.top .brand{font-weight:700;font-size:16px}header.top .ver{color:var(--muted);font-size:13px}
header.top .sp{flex:1}
header.top button,header.top a.btn{font:13px var(--sans);color:var(--text);background:transparent;border:1px solid var(--line);
border-radius:6px;padding:5px 10px;cursor:pointer}
#menu{display:none}
.wrap{display:flex;max-width:1400px;margin:0 auto}
nav.side{position:sticky;top:53px;align-self:flex-start;width:var(--nav-w);flex:0 0 var(--nav-w);height:calc(100vh - 53px);
overflow:auto;padding:18px 12px 40px 20px;border-right:1px solid var(--line);font-size:14px}
nav.side ul{list-style:none;margin:0;padding:0}nav.side li{margin:2px 0}
nav.side ul ul{margin:2px 0 6px 12px;font-size:13px}
nav.side a{display:block;color:var(--muted);padding:3px 8px;border-radius:5px}
nav.side a:hover{background:var(--accent-soft);color:var(--text);text-decoration:none}
nav.side a.on{color:var(--accent);background:var(--accent-soft);font-weight:600}
main{flex:1;min-width:0;padding:24px 40px 80px;max-width:1000px}
.hero{padding:10px 0 18px;border-bottom:1px solid var(--line);margin-bottom:10px}
.hero h1{font-size:30px;margin:0 0 6px;letter-spacing:-.01em}.hero p{margin:0;color:var(--muted);font-size:16px}
.install{margin:18px 0 6px;background:var(--code-bg);border:1px solid var(--line);border-radius:8px;padding:12px 14px;position:relative}
.install .lbl{font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin-bottom:4px}
section{padding-top:22px;border-bottom:1px solid var(--line);padding-bottom:18px}
section>h2{font-size:24px;margin:8px 0 12px;letter-spacing:-.01em}
h3{font-size:18px;margin:26px 0 8px}h4{font-size:15px;margin:20px 0 6px}
p,li{max-width:78ch}
code{font-family:var(--mono);font-size:.88em;background:var(--code-bg);padding:.1em .35em;border-radius:4px;color:var(--code-text)}
pre{position:relative;background:var(--code-bg);border:1px solid var(--line);border-radius:8px;padding:12px 14px;overflow:auto;
font-size:13px;line-height:1.5}
pre code{background:none;padding:0;font-size:inherit}
button.copy{position:absolute;top:6px;right:6px;font:12px var(--sans);color:var(--muted);background:var(--panel);
border:1px solid var(--line);border-radius:5px;padding:2px 8px;cursor:pointer;opacity:.85}
button.copy:hover{opacity:1;color:var(--text)}
.tbl{overflow-x:auto;margin:10px 0 16px;border:1px solid var(--line);border-radius:8px}
table{border-collapse:collapse;width:100%;font-size:14px;background:var(--panel)}
th,td{text-align:left;vertical-align:top;padding:7px 10px;border-bottom:1px solid var(--line)}
th{background:var(--code-bg);font-weight:600;white-space:nowrap}
tr:last-child td{border-bottom:none}
blockquote{margin:12px 0;padding:8px 14px;border-left:3px solid var(--accent);background:var(--accent-soft);border-radius:0 6px 6px 0}
footer{color:var(--muted);font-size:13px;padding:24px 0}
@media (max-width:600px){header.top .ver{display:none}header.top{gap:8px;padding:10px 12px}}
@media (max-width:900px){
 #menu{display:inline-block}
 nav.side{position:fixed;left:0;top:53px;z-index:4;background:var(--panel);transform:translateX(-105%);transition:transform .2s;
  box-shadow:0 10px 30px rgba(0,0,0,.2);width:min(86vw,320px)}
 nav.side.open{transform:none}
 main{padding:16px 16px 60px}
 .hero h1{font-size:24px}
}
@media print{header.top,nav.side,button.copy{display:none}main{max-width:none;padding:0}}
</style>
</head>
<body>
<header class="top">
 <button id="menu" aria-label="Contents">&#9776;</button>
 <span class="brand">mod_amd_ws</span><span class="ver">v@VERSION@ · FreeSWITCH</span>
 <span class="sp"></span>
 <a class="btn" href="https://github.com/nikvb/mod_amd_ws">GitHub</a>
 <button id="theme" aria-label="Toggle theme">Theme</button>
</header>
<div class="wrap">
<nav class="side" id="side"><ul>
@NAV@
</ul></nav>
<main>
<div class="hero">
 <h1>mod_amd_ws</h1>
 <p>Answering machine detection for FreeSWITCH over WebSocket (amdy.io), with the native mod_amd detector built in.</p>
 <div class="install"><div class="lbl">Install on the FreeSWITCH server</div>
<pre><code>curl -fsSL https://raw.githubusercontent.com/nikvb/mod_amd_ws/main/install.sh | sudo bash -s -- -y</code></pre></div>
</div>
@CONTENT@
<footer>mod_amd_ws v@VERSION@ · generated @DATE@ from the repository documentation · <a href="https://github.com/nikvb/mod_amd_ws">github.com/nikvb/mod_amd_ws</a> · MPL 1.1</footer>
</main>
</div>
<script>
(function(){
 // wrap tables for horizontal scroll
 document.querySelectorAll('main table').forEach(function(t){var d=document.createElement('div');d.className='tbl';t.parentNode.insertBefore(d,t);d.appendChild(t);});
 // drop header rows that are entirely empty (key/value tables)
 document.querySelectorAll('main thead tr').forEach(function(r){if(!r.textContent.trim())r.parentNode.remove();});
 // copy buttons
 document.querySelectorAll('pre').forEach(function(p){var b=document.createElement('button');b.className='copy';b.textContent='Copy';
  b.onclick=function(){var t=p.querySelector('code')?p.querySelector('code').innerText:p.innerText;
   (navigator.clipboard?navigator.clipboard.writeText(t):Promise.reject()).then(function(){b.textContent='Copied';setTimeout(function(){b.textContent='Copy'},1200)},function(){b.textContent='Select + copy'});};
  p.appendChild(b);});
 // theme
 var root=document.documentElement,KEY='amdws-theme';
 try{var s=localStorage.getItem(KEY);if(s)root.setAttribute('data-theme',s);}catch(e){}
 document.getElementById('theme').onclick=function(){var dark=root.getAttribute('data-theme')?root.getAttribute('data-theme')==='dark':matchMedia('(prefers-color-scheme: dark)').matches;
  var n=dark?'light':'dark';root.setAttribute('data-theme',n);try{localStorage.setItem(KEY,n)}catch(e){}};
 // mobile menu
 var side=document.getElementById('side');document.getElementById('menu').onclick=function(){side.classList.toggle('open')};
 side.addEventListener('click',function(e){if(e.target.tagName==='A')side.classList.remove('open')});
 // scrollspy
 var links={};document.querySelectorAll('nav.side > ul > li > a').forEach(function(a){links[a.getAttribute('href').slice(1)]=a});
 var io=new IntersectionObserver(function(es){es.forEach(function(e){if(e.isIntersecting){Object.values(links).forEach(function(a){a.classList.remove('on')});
  var a=links[e.target.id];if(a)a.classList.add('on');}})},{rootMargin:'-10% 0px -80% 0px'});
 document.querySelectorAll('main section').forEach(function(s){io.observe(s)});
})();
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
