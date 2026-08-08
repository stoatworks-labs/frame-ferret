#pragma once

namespace ferret {

/// The operator page, served at "/".
///
/// One raw string literal, no build step, exactly like WebLinked — and with
/// WebLinked's scar: a single stray byte in here compiles cleanly, serves 200,
/// renders the HTML, and silently kills every line of JavaScript after it. The
/// page then sits on "connecting" with a dead preview and nothing in any log.
/// A NUL also makes grep treat this file as binary and return nothing, which
/// sends the hunt in the wrong direction entirely.
///
/// `tests/test_web_assets.cpp` guards it: no NUL or control bytes, balanced
/// script tags, and every endpoint the page calls is one the control API
/// actually serves. If you edit this string, run that test.
inline constexpr const char* kControlPageHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Frame Ferret</title>
<style>
  :root {
    --bg: #14161a; --panel: #1c1f26; --line: #2a2f3a; --ink: #e6e9ef;
    --dim: #8b93a4; --accent: #c9a227; --live: #3fb950; --black: #6e7681;
    --bad: #f85149;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; background: var(--bg); color: var(--ink);
    font: 14px/1.5 ui-sans-serif, -apple-system, "Segoe UI", sans-serif;
  }
  header {
    display: flex; align-items: baseline; gap: 16px; flex-wrap: wrap;
    padding: 16px 20px; border-bottom: 1px solid var(--line);
  }
  h1 { font-size: 17px; margin: 0; letter-spacing: .2px; }
  h1 span { color: var(--accent); }
  .pill {
    font-size: 12px; padding: 2px 9px; border-radius: 999px;
    border: 1px solid var(--line); color: var(--dim);
  }
  .pill.live { color: var(--live); border-color: #234d2e; }
  .pill.muted { color: var(--accent); border-color: #4a3d12; }
  main { padding: 20px; display: grid; gap: 20px; max-width: 1200px; }
  section {
    background: var(--panel); border: 1px solid var(--line);
    border-radius: 10px; padding: 16px;
  }
  h2 {
    font-size: 12px; text-transform: uppercase; letter-spacing: .08em;
    color: var(--dim); margin: 0 0 12px;
  }
  .scroll { overflow-x: auto; }
  table { border-collapse: collapse; font-size: 13px; }
  th, td {
    border: 1px solid var(--line); padding: 7px 10px; text-align: left;
    white-space: nowrap;
  }
  th { color: var(--dim); font-weight: 500; }
  td.cell { text-align: center; cursor: pointer; width: 90px; }
  td.cell:hover { background: #232833; }
  td.cell.on { background: #1d3524; color: var(--live); font-weight: 600; }
  td.rowhead { font-weight: 500; }
  .reason { color: var(--black); font-size: 12px; white-space: normal; }
  .reason.bad { color: var(--bad); }
  .stats { display: flex; gap: 22px; flex-wrap: wrap; font-size: 13px; }
  .stat b { display: block; font-size: 19px; font-weight: 600; }
  .stat span { color: var(--dim); font-size: 11px; text-transform: uppercase; }
  img#preview {
    max-width: 100%; border: 1px solid var(--line); border-radius: 6px;
    background: #000; display: block;
  }
  button {
    background: #232833; color: var(--ink); border: 1px solid var(--line);
    border-radius: 6px; padding: 6px 14px; cursor: pointer; font-size: 13px;
  }
  button:hover { border-color: var(--accent); }
  ul.fail { margin: 0; padding-left: 18px; color: var(--dim); font-size: 13px; }
  ul.fail code { color: var(--ink); }
  .empty { color: var(--dim); font-size: 13px; }
</style>
</head>
<body>
<header>
  <h1>Frame <span>Ferret</span></h1>
  <span class="pill" id="status">connecting</span>
  <span class="pill" id="ratePill">&mdash;</span>
  <button id="muteBtn">Mute</button>
</header>

<main>
  <section>
    <h2>Crosspoint</h2>
    <div class="scroll"><table id="grid"></table></div>
  </section>

  <section>
    <h2>Preview</h2>
    <img id="preview" alt="preview" width="480">
    <p class="empty" id="previewNote"></p>
  </section>

  <section>
    <h2>Counters</h2>
    <div class="stats" id="stats"></div>
  </section>

  <section id="failSection" hidden>
    <h2>Unavailable nodes</h2>
    <ul class="fail" id="failures"></ul>
  </section>
</main>

<script>
"use strict";

var previewId = null;

function h(text) {
  return String(text == null ? "" : text).replace(/[&<>"]/g, function (c) {
    return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
  });
}

function drawGrid(state) {
  var sources = state.sources || [];
  var sinks = state.sinks || [];
  var rows = [];

  var head = "<tr><th>Output &darr; &nbsp; Source &rarr;</th>";
  head += '<th style="text-align:center">none</th>';
  sources.forEach(function (s) {
    var label = h(s.label || s.id);
    if (!s.available) label += " ⚠";
    head += '<th style="text-align:center">' + label + "</th>";
  });
  head += "<th>State</th></tr>";
  rows.push(head);

  sinks.forEach(function (sink) {
    var row = '<tr><td class="rowhead">' + h(sink.label || sink.id);
    if (!sink.available) row += ' <span class="reason bad">⚠</span>';
    row += "</td>";

    var routed = sink.routedFrom || "";
    row +=
      '<td class="cell' + (routed === "" ? " on" : "") +
      '" data-sink="' + h(sink.id) + '" data-source="">&mdash;</td>';

    sources.forEach(function (src) {
      var on = routed === src.id;
      row +=
        '<td class="cell' + (on ? " on" : "") +
        '" data-sink="' + h(sink.id) + '" data-source="' + h(src.id) + '">' +
        (on ? "●" : "") + "</td>";
    });

    var note = sink.reason
      ? '<span class="reason bad">' + h(sink.reason) + "</span>"
      : '<span class="reason">' + h(sink.action || "") +
        (sink.action === "convert" ? " → " + h(sink.targetFormat) : "") +
        "</span>";
    row += "<td>" + note + "</td></tr>";
    rows.push(row);
  });

  if (sinks.length === 0) {
    rows.push('<tr><td class="empty">No sinks configured.</td></tr>');
  }
  document.getElementById("grid").innerHTML = rows.join("");
}

function drawStats(c) {
  var items = [
    ["measured fps", (c.measuredFps || 0).toFixed(2)],
    ["ticks", c.ticks || 0],
    ["frames", c.framesDelivered || 0],
    ["black", c.blackDelivered || 0],
    ["conversions", c.conversions || 0],
    ["late ticks", c.lateTicks || 0]
  ];
  document.getElementById("stats").innerHTML = items
    .map(function (it) {
      return '<div class="stat"><b>' + h(it[1]) + "</b><span>" + h(it[0]) +
        "</span></div>";
    })
    .join("");
}

function drawFailures(failures) {
  var section = document.getElementById("failSection");
  if (!failures || failures.length === 0) {
    section.hidden = true;
    return;
  }
  section.hidden = false;
  document.getElementById("failures").innerHTML = failures
    .map(function (f) {
      return "<li><code>" + h(f.id) + "</code> &mdash; " + h(f.reason) + "</li>";
    })
    .join("");
}

function refresh() {
  fetch("/api/state")
    .then(function (r) { return r.json(); })
    .then(function (state) {
      var status = document.getElementById("status");
      if (state.muted) {
        status.textContent = "muted";
        status.className = "pill muted";
      } else if (state.running) {
        status.textContent = "running";
        status.className = "pill live";
      } else {
        status.textContent = "stopped";
        status.className = "pill";
      }
      document.getElementById("muteBtn").textContent =
        state.muted ? "Unmute" : "Mute";
      document.getElementById("ratePill").textContent = state.rate + " fps";

      drawGrid(state);
      drawStats(state.counters || {});
      drawFailures(state.failures);

      var withPreview = (state.sinks || []).filter(function (s) {
        return s.hasPreview;
      });
      previewId = withPreview.length ? withPreview[0].id : null;
      document.getElementById("previewNote").textContent = previewId
        ? ""
        : "No preview sink configured.";
    })
    .catch(function () {
      var status = document.getElementById("status");
      status.textContent = "disconnected";
      status.className = "pill";
    });
}

function refreshPreview() {
  if (!previewId) return;
  var img = document.getElementById("preview");
  img.src = "/preview/" + encodeURIComponent(previewId) + ".bmp?t=" + Date.now();
}

document.getElementById("grid").addEventListener("click", function (event) {
  var cell = event.target.closest("td.cell");
  if (!cell) return;
  fetch("/api/route", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      sink: cell.getAttribute("data-sink"),
      source: cell.getAttribute("data-source")
    })
  }).then(refresh);
});

document.getElementById("muteBtn").addEventListener("click", function () {
  fetch("/api/mute", { method: "POST" }).then(refresh);
});

refresh();
setInterval(refresh, 1000);
setInterval(refreshPreview, 200);
</script>
</body>
</html>
)HTML";

}  // namespace ferret
