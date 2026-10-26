/* The FermentDial theme (webui/src/shared/theme.css), trimmed to what a
 * one-page sensor needs. Same tokens, same hero/card/pill vocabulary, so a pill
 * and a Dial look like siblings in the same browser tab.
 *
 * Kept in a header as C string literals: the pill is C, so no R"()" raw strings,
 * which means HTML attributes use single quotes and the CSS avoids them.
 */
#pragma once

#define PILL_THEME_CSS \
":root{--bg:#0d1b1e;--sunken:#091418;--face:#102126;--card:#132428;--panel2:#1b3540;" \
"--border:#1e3840;--text:#d0e8f0;--muted:#6a9aaa;--accent:#b0d8f8;--primary:#356f89;" \
"--ok:#36c87a;--heat:#e36018;--cool:#b0d8f8;--gold:#ffd178;--fault:#e44840;" \
"--radius:.625rem;--font-sans:system-ui,-apple-system,'Segoe UI',Roboto,Arial,sans-serif;" \
"--font-mono:ui-monospace,'SF Mono',Menlo,Consolas,monospace}" \
"html{background:var(--bg)}*{box-sizing:border-box}" \
"body{margin:0 auto;padding:20px 16px;max-width:52em;font:15px/1.45 var(--font-sans);" \
"background:var(--bg);color:var(--text);-webkit-font-smoothing:antialiased}" \
"a{color:#79d4ff;text-decoration:none}a:hover{text-decoration:underline}" \
"code{font-family:var(--font-mono);font-size:13px}" \
"h1{font-size:21px;margin:0;color:var(--accent);font-weight:700;letter-spacing:-.02em}" \
"h2{font-size:16px;margin:0;font-weight:600}" \
"header{display:flex;justify-content:space-between;align-items:center;gap:12px;" \
"flex-wrap:wrap;margin-bottom:14px}" \
"input,button,select{font:inherit;border:1px solid var(--border);" \
"border-radius:calc(var(--radius) - 2px);padding:9px 12px}" \
"input,select{width:100%;background:var(--face);color:var(--text)}" \
"input::placeholder{color:var(--muted);opacity:.75}" \
"button{background:var(--primary);color:#fff;border-color:#2a5a70;font-weight:600;cursor:pointer}" \
"button.secondary{background:var(--panel2);color:var(--accent);border-color:#2a4a58}" \
".statusbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap}" \
".pill{border:1px solid var(--border);border-radius:999px;padding:5px 11px;" \
"background:var(--face);color:var(--accent);font-size:12px;font-weight:500}" \
".hero{border:1px solid var(--border);border-top:3px solid var(--ok);border-radius:var(--radius);" \
"padding:20px;background:var(--sunken);overflow:hidden}" \
"body[data-state=active] .hero{border-top-color:var(--heat)}" \
"body[data-state=dry] .hero{border-top-color:var(--cool)}" \
"body[data-state=fault] .hero{border-top-color:var(--fault)}" \
".heroTop{display:flex;justify-content:space-between;gap:12px;color:var(--muted);font-size:12px;" \
"font-weight:600;letter-spacing:.04em;text-transform:uppercase}" \
".readout{display:flex;align-items:flex-end;justify-content:space-between;gap:14px;margin-top:12px}" \
".num{font-size:74px;line-height:.9;font-weight:700;letter-spacing:-.03em;" \
"font-variant-numeric:tabular-nums}" \
".unit{font-size:24px;color:var(--accent);margin-left:8px;font-weight:700}" \
".state{font-size:23px;font-weight:700;margin-top:8px;color:var(--ok)}" \
"body[data-state=active] .state{color:var(--heat)}body[data-state=dry] .state{color:var(--cool)}" \
"body[data-state=fault] .state{color:var(--fault)}" \
".sub{margin-top:6px;color:var(--muted)}" \
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-top:12px}" \
".card{border:1px solid var(--border);border-radius:var(--radius);background:var(--card);" \
"padding:14px;min-width:0}" \
".label{color:var(--muted);font-size:11px;font-weight:600;letter-spacing:.04em;text-transform:uppercase}" \
".value{font-size:23px;font-weight:700;margin-top:4px;letter-spacing:-.01em;" \
"font-variant-numeric:tabular-nums}" \
".panel{border:1px solid var(--border);border-radius:var(--radius);background:var(--card);" \
"padding:14px;margin-top:12px}" \
".panelHead{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:12px}" \
".panelHead span{color:var(--muted);font-size:11px;font-weight:600;letter-spacing:.04em;" \
"text-transform:uppercase}" \
".field{margin:.7em 0}.hint{color:var(--muted);font-size:13px}" \
".btns{display:flex;gap:8px;flex-wrap:wrap;margin-top:.8em}"

/* Every page starts the same way. Pass a refresh meta (or "") to keep a
 * dashboard honest without JS. */
#define PILL_REFRESH_15 "<meta http-equiv=refresh content='15'>"
#define PILL_PAGE_HEAD(title, refresh_meta) \
    "<!doctype html><meta charset=utf-8>" \
    "<meta name=viewport content='width=device-width,initial-scale=1'>" \
    "<meta name=color-scheme content=dark>" \
    refresh_meta "<title>" title "</title><style>" PILL_THEME_CSS "</style>"
