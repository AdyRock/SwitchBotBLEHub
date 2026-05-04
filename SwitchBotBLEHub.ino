/*
    <SwitchBotBLEHub:- Turn a ESP32 Arduio compatible board into a hub>
    Copyright (C) <2020>  <Adrian Rockall>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <AsyncUDP.h>            // https://github.com/espressif/arduino-esp32/tree/master/libraries/AsyncUDP
#include <ESPAsyncWebServer.h>   // https://github.com/ESP32Async/ESPAsyncWebServer
#include <ESPAsyncWiFiManager.h> // https://github.com/alanswx/ESPAsyncWiFiManager
#include <HTTPClient.h>
#include <NimBLEDevice.h> // https://github.com/h2zero/NimBLE-Arduino/blob/master/docs/New_user_guide.md
#include <WiFi.h>
#include <WiFiClient.h>
// #include <ElegantOTA.h>           // https://github.com/ayushsharma82/ElegantOTA
#include <ESPAsyncHTTPUpdateServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "BLE_Device.h"
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

const char* version = "Hello! SwitchBot BLE Hub V2.10";

static bool TryGetJsonStringField( const uint8_t* data, size_t len, const char* key, String& value )
{
	value = "";
	if ( data == nullptr || len == 0 || key == nullptr || key[ 0 ] == '\0' )
	{
		return false;
	}

	String body;
	body.reserve( len );
	for ( size_t i = 0; i < len; i++ )
	{
		body += ( char )data[ i ];
	}

	const String keyToken = String( "\"" ) + key + "\"";
	int keyPos = body.indexOf( keyToken );
	if ( keyPos < 0 )
	{
		return false;
	}

	int colonPos = body.indexOf( ':', keyPos + keyToken.length() );
	if ( colonPos < 0 )
	{
		return false;
	}

	int quoteStart = body.indexOf( '"', colonPos + 1 );
	if ( quoteStart < 0 )
	{
		return false;
	}

	int quoteEnd = quoteStart + 1;
	while ( quoteEnd < body.length() )
	{
		if ( body[ quoteEnd ] == '"' && body[ quoteEnd - 1 ] != '\\' )
		{
			break;
		}
		quoteEnd++;
	}
	if ( quoteEnd >= body.length() )
	{
		return false;
	}

	value = body.substring( quoteStart + 1, quoteEnd );
	value.replace( "\\\"", "\"" );
	value.replace( "\\\\", "\\" );
	return true;
}

static const char HOME_HTML[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>SwitchBot BLE Hub</title>
    <style>
      body { margin: 0; background: #1e1e1e; color: #d4d4d4; font-family: sans-serif; }
      .wrap { max-width: 760px; margin: 2rem auto; padding: 0 1rem; }
      .card { background: #252526; border: 1px solid #3c3c3c; border-radius: 10px; padding: 1.25rem 1.1rem; }
      h1 { margin: 0 0 .25rem; color: #9cdcfe; font-size: 1.55rem; }
      .ver { margin: 0 0 1.1rem; color: #a0a0a0; font-size: .95rem; }
      .links { display: grid; gap: .7rem; }
      a.btn { display: block; text-decoration: none; background: #007acc; color: #fff; padding: .7rem .8rem; border-radius: 6px; font-weight: 600; }
      a.btn:hover { background: #005f9e; }
    </style>
  </head>
  <body>
    <div class="wrap">
      <div class="card">
        <h1>SwitchBot BLE Hub</h1>
        <p class="ver">Version 2.10</p>
        <div class="links">
          <a class="btn" href="/update">Update firmware</a>
          <a class="btn" href="/api/v1/devices">View registered devices (JSON)</a>
          <a class="btn" href="/api/v1/devices/table">View registered devices (table)</a>
        </div>
      </div>
    </div>
  </body>
</html>
)HTMLEOF";

static const char DEVICES_JSON_HTML[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Registered Devices</title>
    <style>
      body { font-family: monospace; background: #1e1e1e; color: #d4d4d4; margin: 1rem 2rem; }
      h1 { color: #9cdcfe; font-family: sans-serif; }
      pre {
        background: #252526;
        border: 1px solid #3c3c3c;
        border-radius: 6px;
        padding: 1rem;
        white-space: pre-wrap;
        word-break: break-all;
        font-size: 0.9rem;
      }
      .k { color: #9cdcfe; }
      .s { color: #ce9178; }
      .n { color: #b5cea8; }
      .b { color: #569cd6; }
    </style>
  </head>
  <body>
    <h1>Registered Devices</h1>
    <pre id="out"></pre>
    <script>
      function esc(s) {
        return s.replace(/&/g, "&amp;").replace(/</g, "&lt;");
      }
      function colorize(json) {
        return json.replace(/(\x22(\\u[a-zA-Z0-9]{4}|\\[^u]|[^\\\x22])*\x22\s*:?|\b(true|false|null)\b|-?\d+(?:\.\d*)?(?:[eE][+\-]?\d+)?)/g, function(m) {
          var c = "n";
          if (/^\x22/.test(m)) {
            c = /:$/.test(m) ? "k" : "s";
          } else if (/true|false/.test(m) || /null/.test(m)) {
            c = "b";
          }
          return "<span class=\"" + c + "\">" + m + "</span>";
        });
      }
      fetch("/api/v1/devices", { headers: { Accept: "application/json" } })
        .then(function(r) { return r.json(); })
        .then(function(raw) {
          document.getElementById("out").innerHTML = colorize(esc(JSON.stringify(raw, null, 2)));
        })
        .catch(function(e) {
          document.getElementById("out").textContent = "Error: " + e;
        });
    </script>
  </body>
</html>
)HTMLEOF";

static const char DEVICES_TABLE_HTML[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Devices</title>
    <style>
      body { font-family: sans-serif; background: #1e1e1e; color: #d4d4d4; margin: 1rem 2rem; }
      h2 { color: #9cdcfe; margin-top: 1.8rem; margin-bottom: 0.4rem; border-bottom: 1px solid #3c3c3c; padding-bottom: 4px; }
      table { border-collapse: collapse; margin-bottom: 0.5rem; font-size: 0.85rem; }
      th { background: #007acc; color: #fff; padding: 6px 10px; text-align: left; white-space: nowrap; cursor: pointer; -webkit-user-select: none; user-select: none; }
      th:hover { background: #005f9e; }
      td { padding: 5px 10px; border-bottom: 1px solid #3c3c3c; white-space: nowrap; }
      tr:nth-child(even) { background: #252526; }
      tr:hover td { background: #2d2d2d; }
      #stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(170px, 1fr)); gap: 0.6rem; margin: 1rem 0; }
      .stat { background: #252526; border: 1px solid #3c3c3c; border-radius: 6px; padding: 0.5rem 0.6rem; }
	.stat.clickable { cursor: pointer; border-color: #007acc; }
	.stat.clickable:hover { background: #2a3440; }
      .stat .k { font-size: 0.75rem; color: #9aa0a6; }
      .stat .v { font-size: 1rem; color: #d4d4d4; font-weight: 600; }
      #refresh { margin-bottom: 1rem; padding: 6px 14px; background: #007acc; color: #fff; border: none; border-radius: 4px; cursor: pointer; font-size: 0.9rem; }
      #refresh:hover { background: #005f9e; }
      h1 { color: #9cdcfe; }
      #ts { margin-left: 1rem; font-size: 0.8rem; color: #888; }
      #live { margin-left: 1rem; font-size: 0.8rem; color: #555; }
			#rssiGuide { margin: 0.5rem 0 1rem; font-size: 0.8rem; color: #bbb; display: flex; flex-wrap: wrap; gap: 0.45rem; align-items: center; }
			.q { padding: 0.15rem 0.45rem; border-radius: 999px; font-size: 0.72rem; font-weight: 600; border: 1px solid transparent; }
			.q-excellent { color: #9ef59e; border-color: #2f8f2f; }
			.q-good { color: #9cdcfe; border-color: #2f6f8f; }
			.q-fair { color: #f8d37a; border-color: #8f7a2f; }
			.q-acceptable { color: #f0b57a; border-color: #8f5f2f; }
			.q-poor { color: #f29a9a; border-color: #8f2f2f; }
			#heapHistoryModal { position: fixed; inset: 0; background: rgba(0,0,0,0.6); display: none; align-items: center; justify-content: center; z-index: 20; }
			#heapHistoryDialog { width: min(940px, 95vw); background: #252526; border: 1px solid #3c3c3c; border-radius: 10px; padding: 0.9rem; }
			#heapHistoryHeader { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.7rem; }
			#heapHistoryTitle { color: #9cdcfe; font-weight: 700; }
			#heapHistoryClose { background: #3c3c3c; color: #fff; border: none; border-radius: 5px; padding: 0.35rem 0.65rem; cursor: pointer; }
			#heapHistoryClose:hover { background: #555; }
			#heapHistoryCanvas { width: 100%; height: 320px; background: #1e1e1e; border: 1px solid #3c3c3c; border-radius: 6px; display: block; }
			#heapHistoryMeta { margin-top: 0.55rem; font-size: 0.8rem; color: #bbb; }
    </style>
  </head>
  <body>
    <h1>Registered Devices</h1>
    <button id="refresh" onclick="refreshAll()">&#8635; Refresh</button>
    <span id="ts"></span><span id="live">&#9679; connecting...</span>
    <div id="stats"></div>
		<div id="rssiGuide">
			<strong>RSSI guide:</strong>
			<span class="q q-excellent">Excellent (&gt;= -55 dBm)</span>
			<span class="q q-good">Good (-56 to -67 dBm)</span>
			<span class="q q-fair">Fair (-68 to -75 dBm)</span>
			<span class="q q-acceptable">Acceptable (-76 to -85 dBm)</span>
			<span class="q q-poor">Poor (&lt;= -86 dBm)</span>
		</div>
    <div id="tbl"></div>
		<div id="heapHistoryModal" onclick="if(event.target===this) closeFreeHeapHistory()">
			<div id="heapHistoryDialog">
				<div id="heapHistoryHeader">
					<div id="heapHistoryTitle">Free Heap History (last 1000 samples)</div>
					<button id="heapHistoryClose" onclick="closeFreeHeapHistory()">Close</button>
				</div>
				<canvas id="heapHistoryCanvas"></canvas>
				<div id="heapHistoryMeta"></div>
			</div>
		</div>
    <script>
      var groups = {};
      var sortPrefs = {};
      var statsTimer = null;
      var lastStats = null;
      var statsClockOffsetMs = 0;
      var nextStatsServerMs = 0;
      var waitingStatsRefresh = false;
			var lastHeapHistory = null;

			function mkStat(k, v, clickFn) {
				var cls = clickFn ? "stat clickable" : "stat";
				var clickAttr = clickFn ? " onclick=\"" + clickFn + "()\"" : "";
				return "<div class=\"" + cls + "\"" + clickAttr + "><div class=k>" + k + "</div><div class=v>" + v + "</div></div>";
      }

			function formatUptime(ms) {
				var totalSec = Math.floor((ms || 0) / 1000);
				var days = Math.floor(totalSec / 86400);
				var rem = totalSec % 86400;
				var hh = Math.floor(rem / 3600);
				rem = rem % 3600;
				var mm = Math.floor(rem / 60);
				var ss = rem % 60;
				function pad2(n) { return n < 10 ? "0" + n : "" + n; }
				return days + ", " + pad2(hh) + ":" + pad2(mm) + ":" + pad2(ss);
			}

      function getStatsRemainingSeconds() {
        if (nextStatsServerMs <= 0) return "--";
        var serverNowMs = Date.now() - statsClockOffsetMs;
        var rem = Math.ceil((nextStatsServerMs - serverNowMs) / 1000);
        if (rem < 0) rem = 0;
        return rem;
      }

      function renderStats(s) {
        var h = "";
				var serverNowMs = Date.now() - statsClockOffsetMs;
        h += mkStat("Next stats refresh", getStatsRemainingSeconds() + "s");
				h += mkStat("Adverts/min", s.advertsSeenPerMinute || 0);
				h += mkStat("Service UUID match/min", s.matchedServiceDataPerMinute || 0);
				h += mkStat("Matched empty payload/min", s.matchedEmptyPayloadPerMinute || 0);
				h += mkStat("Matched rejected/min", s.matchedRejectedPerMinute || 0);
				h += mkStat("Updates/min", s.updatesPerMinute);
				h += mkStat("Free heap", s.freeHeap, "openFreeHeapHistory");
				h += mkStat("Largest block", s.largestHeapBlock);
				h += mkStat("CPU usage", s.cpuUsage + "%");
			h += mkStat("Uptime (D, HH:MM:SS)", formatUptime(serverNowMs));
        document.getElementById("stats").innerHTML = h;
      }

			function closeFreeHeapHistory() {
				document.getElementById("heapHistoryModal").style.display = "none";
			}

			function openFreeHeapHistory() {
				document.getElementById("heapHistoryModal").style.display = "flex";
				loadFreeHeapHistory();
			}

			function drawFreeHeapHistory(values, intervalMs) {
				var canvas = document.getElementById("heapHistoryCanvas");
				var meta = document.getElementById("heapHistoryMeta");
				var ratio = window.devicePixelRatio || 1;
				var cssW = canvas.clientWidth;
				var cssH = canvas.clientHeight;
				canvas.width = Math.max(1, Math.floor(cssW * ratio));
				canvas.height = Math.max(1, Math.floor(cssH * ratio));
				var ctx = canvas.getContext("2d");
				ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
				ctx.clearRect(0, 0, cssW, cssH);

				if (!values || !values.length) {
					ctx.fillStyle = "#9aa0a6";
					ctx.font = "14px sans-serif";
					ctx.fillText("No history samples yet", 16, 28);
					meta.textContent = "No data available";
					return;
				}

				var minV = values[0], maxV = values[0];
				for (var i = 1; i < values.length; i++) {
					if (values[i] < minV) minV = values[i];
					if (values[i] > maxV) maxV = values[i];
				}
				if (maxV === minV) {
					minV = minV - 1;
					maxV = maxV + 1;
				}

				var padL = 44, padR = 16, padT = 12, padB = 26;
				var plotW = Math.max(1, cssW - padL - padR);
				var plotH = Math.max(1, cssH - padT - padB);

				ctx.strokeStyle = "#333";
				ctx.lineWidth = 1;
				for (var gy = 0; gy <= 4; gy++) {
					var y = padT + (plotH * gy / 4);
					ctx.beginPath();
					ctx.moveTo(padL, y);
					ctx.lineTo(padL + plotW, y);
					ctx.stroke();
				}

				ctx.strokeStyle = "#4ec94e";
				ctx.lineWidth = 2;
				ctx.beginPath();
				for (var j = 0; j < values.length; j++) {
					var x = padL + (plotW * (values.length === 1 ? 0 : j / (values.length - 1)));
					var t = (values[j] - minV) / (maxV - minV);
					var y2 = padT + (1 - t) * plotH;
					if (j === 0) ctx.moveTo(x, y2); else ctx.lineTo(x, y2);
				}
				ctx.stroke();

				ctx.fillStyle = "#9aa0a6";
				ctx.font = "12px sans-serif";
				ctx.fillText(String(maxV), 6, padT + 4);
				ctx.fillText(String(minV), 6, padT + plotH + 4);

				var spanMs = intervalMs * (values.length - 1);
				var spanHours = (spanMs / 3600000).toFixed(1);
				var latest = values[values.length - 1];
				meta.textContent = "Samples: " + values.length + " | Window: ~" + spanHours + "h | Min: " + minV + " | Max: " + maxV + " | Latest: " + latest;
			}

			function loadFreeHeapHistory() {
				var meta = document.getElementById("heapHistoryMeta");
				meta.textContent = "Loading...";
				fetch("/api/v1/stats/free-heap-history", { headers: { Accept: "application/json" } })
					.then(function(r) { return r.json(); })
					.then(function(data) {
						lastHeapHistory = data;
						drawFreeHeapHistory(data.values || [], data.intervalMs || 60000);
					})
					.catch(function(e) {
						meta.textContent = "Failed to load history: " + e;
						drawFreeHeapHistory([], 60000);
					});
			}

			window.addEventListener("resize", function() {
				if (document.getElementById("heapHistoryModal").style.display === "flex" && lastHeapHistory) {
					drawFreeHeapHistory(lastHeapHistory.values || [], lastHeapHistory.intervalMs || 60000);
				}
			});

      function startStatsTimer() {
        if (statsTimer !== null) return;
        statsTimer = setInterval(function() {
          if (lastStats) {
            if (getStatsRemainingSeconds() === 0 && !waitingStatsRefresh) {
              waitingStatsRefresh = true;
              loadStats();
            }
            renderStats(lastStats);
          }
        }, 1000);
      }

      function loadStats() {
        fetch("/api/v1/stats", { headers: { Accept: "application/json" } })
          .then(function(r) { return r.json(); })
          .then(function(s) {
            lastStats = s;
            statsClockOffsetMs = Date.now() - (s.uptimeMs || 0);
            nextStatsServerMs = (s.lastStatsAtMs || 0) + 60000;
            waitingStatsRefresh = false;
            renderStats(s);
          })
          .catch(function() {
            document.getElementById("stats").innerHTML = "";
          });
      }

      function sortRows(rows, k, asc) {
        rows.sort(function(a, b) {
          var av = a[k] === undefined ? "" : a[k];
          var bv = b[k] === undefined ? "" : b[k];
          if (!isNaN(av) && !isNaN(bv)) { av = +av; bv = +bv; }
          return asc ? (av > bv ? 1 : av < bv ? -1 : 0) : (av < bv ? 1 : av > bv ? -1 : 0);
        });
      }

      function flatten(d) {
        var o = { address: d.address, rssi: d.rssi };
        var s = d.serviceData || {};
        Object.keys(s).forEach(function(k) {
          if (k === "model") return;
          var v = s[k];
          if (v !== null && typeof v === "object") {
            Object.keys(v).forEach(function(sk) { o[k + "_" + sk] = v[sk]; });
          } else {
            o[k] = v;
          }
        });
        return o;
      }

      function buildCols(rows) {
        var seen = {};
        rows.forEach(function(r) { Object.keys(r).forEach(function(k) { seen[k] = true; }); });
        var fixed = ["address", "rssi"];
        var rest = Object.keys(seen).filter(function(k) { return fixed.indexOf(k) < 0 && k !== "modelName"; }).sort();
        return fixed.concat(rest);
      }

			function rssiQuality(v) {
				var r = +v;
				if (r >= -55) return { text: "Excellent", cls: "q-excellent" };
				if (r >= -67) return { text: "Good", cls: "q-good" };
				if (r >= -75) return { text: "Fair", cls: "q-fair" };
				if (r >= -85) return { text: "Acceptable", cls: "q-acceptable" };
				return { text: "Poor", cls: "q-poor" };
			}

      function renderGroup(name, g) {
        var cols = buildCols(g.rows);
        var h = '<table id="t_' + name + '">';
        h += "<thead><tr>";
        cols.forEach(function(c, i) {
          var arrow = g.sortCol === i ? (g.sortAsc ? " &#9650;" : " &#9660;") : "";
          h += '<th onclick="sortGroup(\'' + name + '\',' + i + ')">' + c.replace(/_/g, " ") + arrow + "</th>";
        });
        h += "</tr></thead><tbody>";
        g.rows.forEach(function(r) {
          h += "<tr>";
          cols.forEach(function(c) {
            var v = r[c];
            if (v === undefined || v === null) {
              h += '<td style="color:#555">-</td>';
						} else if (c === "rssi") {
							var q = rssiQuality(v);
							h += "<td>" + v + " dBm <span class=\"q " + q.cls + "\">" + q.text + "</span></td>";
            } else if (typeof v === "boolean") {
              h += "<td>" + (v ? "yes" : "no") + "</td>";
            } else {
              h += "<td>" + v + "</td>";
            }
          });
          h += "</tr>";
        });
        h += "</tbody></table>";
        return h;
      }

      function render() {
        var names = Object.keys(groups).sort();
        if (!names.length) {
          document.getElementById("tbl").innerHTML = "<p>No devices found.</p>";
          return;
        }
        var h = "";
        names.forEach(function(name) {
          h += "<h2>" + name + " (" + groups[name].rows.length + ")</h2>";
          h += renderGroup(name, groups[name]);
        });
        document.getElementById("tbl").innerHTML = h;
      }

      function sortGroup(name, i) {
        var g = groups[name];
        if (g.sortCol === i) {
          g.sortAsc = !g.sortAsc;
        } else {
          g.sortCol = i;
          g.sortAsc = true;
        }
        var cols = buildCols(g.rows);
        var k = cols[i];
        sortRows(g.rows, k, g.sortAsc);
        sortPrefs[name] = { sortCol: g.sortCol, sortAsc: g.sortAsc };
        render();
      }

      function load() {
        fetch("/api/v1/devices", { headers: { Accept: "application/json" } })
          .then(function(r) { return r.json(); })
          .then(function(data) {
            groups = {};
            data.forEach(function(d) {
              var f = flatten(d);
              var name = f.modelName || "Unknown";
              if (!groups[name]) {
                groups[name] = { rows: [], sortCol: -1, sortAsc: true };
              }
              groups[name].rows.push(f);
            });
            Object.keys(groups).forEach(function(name) {
              var pref = sortPrefs[name];
              if (!pref) return;
              var g = groups[name];
              var cols = buildCols(g.rows);
              if (pref.sortCol >= 0 && pref.sortCol < cols.length) {
                g.sortCol = pref.sortCol;
                g.sortAsc = pref.sortAsc;
                sortRows(g.rows, cols[g.sortCol], g.sortAsc);
              }
            });
            render();
            document.getElementById("ts").textContent = "Updated: " + new Date().toLocaleTimeString();
          })
          .catch(function(e) {
            document.getElementById("tbl").innerHTML = "<p style=color:red>Error: " + e + "</p>";
          });
      }

      function refreshAll() { load(); loadStats(); }

      refreshAll();
      startStatsTimer();

      var es = new EventSource("/api/v1/events");
      es.addEventListener("ble", function() { load(); });
      es.addEventListener("stats", function() { loadStats(); });
      es.onopen = function() {
        document.getElementById("live").style.color = "#4ec94e";
        document.getElementById("live").textContent = "\u25cf live";
      };
      es.onerror = function() {
        document.getElementById("live").style.color = "#e05252";
        document.getElementById("live").textContent = "\u25cf disconnected";
      };
    </script>
  </body>
</html>
)HTMLEOF";

BLE_Device BLE_Devices;
ClientCallbacks OurCallbacks;
SemaphoreHandle_t callbackMutex = nullptr;

static inline void lockCallbacks()
{
	if ( callbackMutex != nullptr )
	{
		xSemaphoreTake( callbackMutex, portMAX_DELAY );
	}
}

static inline void unlockCallbacks()
{
	if ( callbackMutex != nullptr )
	{
		xSemaphoreGive( callbackMutex );
	}
}

CommandQ BLECommandQ;
AsyncWebServer server( 80 );
DNSServer dns;
AsyncUDP udp;
AsyncEventSource bleEvents( "/api/v1/events" );

ESPAsyncHTTPUpdateServer _updateServer;

unsigned long ota_progress_millis = 0;

const int led = 14;

char macAddress[ 18 ];
unsigned long sendBroadcast = 0;
uint8_t BLENotifyData[ 50 ];
int BLENotifyLength = 0;
uint32_t BLESending = 0;
bool RebootRequired = false;
int32_t NumUpdates = 0;
int32_t NumAdvertsSeen = 0;
int32_t NumMatchedServiceData = 0;
int32_t NumMatchedEmptyPayload = 0;
int32_t NumMatchedRejected = 0;
int32_t NumUpdatesAt0 = 0;
volatile int32_t LastUpdatesPerMinute = 0;
volatile int32_t LastAdvertsSeenPerMinute = 0;
volatile int32_t LastMatchedServiceDataPerMinute = 0;
volatile int32_t LastMatchedEmptyPayloadPerMinute = 0;
volatile int32_t LastMatchedRejectedPerMinute = 0;
volatile uint32_t LastFreeHeap = 0;
volatile uint32_t LastLargestHeapBlock = 0;
volatile uint32_t LastStatsAt = 0;
volatile uint8_t LastCpuUsagePercent = 0;
static uint32_t PrevIdleRunTime = 0;
static uint32_t PrevTotalRunTime = 0;
volatile bool pendingSSEUpdate = false;
volatile bool pendingSSEStats = false;
unsigned long lastSSESend = 0;
unsigned long lastSSEStatsSend = 0;
const uint16_t FREE_HEAP_HISTORY_MAX = 1000;
uint32_t FreeHeapHistory[ FREE_HEAP_HISTORY_MAX ];
uint16_t FreeHeapHistoryStart = 0;
uint16_t FreeHeapHistoryCount = 0;

static void SampleCpuUsage()
{
	const UBaseType_t maxTasks = 32;
	TaskStatus_t* taskBuffer = ( TaskStatus_t* )malloc( sizeof( TaskStatus_t ) * maxTasks );
	if ( !taskBuffer ) return;

	uint32_t totalRunTime = 0;
	UBaseType_t taskCount = uxTaskGetSystemState( taskBuffer, maxTasks, &totalRunTime );

	uint32_t idleRunTime = 0;
	for ( UBaseType_t i = 0; i < taskCount; i++ )
	{
		if ( strncmp( taskBuffer[ i ].pcTaskName, "IDLE", 4 ) == 0 )
		{
			idleRunTime += taskBuffer[ i ].ulRunTimeCounter;
		}
	}
	free( taskBuffer );

	uint32_t deltaIdle = idleRunTime - PrevIdleRunTime;
	uint32_t deltaTotal = ( totalRunTime - PrevTotalRunTime ) * ( uint32_t )portNUM_PROCESSORS;
	if ( deltaTotal > 0 )
	{
		uint32_t idlePct = ( uint32_t )( ( ( uint64_t )deltaIdle * 100 ) / deltaTotal );
		LastCpuUsagePercent = ( uint8_t )( idlePct < 100 ? 100 - idlePct : 0 );
	}
	PrevIdleRunTime = idleRunTime;
	PrevTotalRunTime = totalRunTime;
}

static void RecordFreeHeapHistory( uint32_t freeHeap )
{
	if ( FreeHeapHistoryCount < FREE_HEAP_HISTORY_MAX )
	{
		const uint16_t idx = ( FreeHeapHistoryStart + FreeHeapHistoryCount ) % FREE_HEAP_HISTORY_MAX;
		FreeHeapHistory[ idx ] = freeHeap;
		FreeHeapHistoryCount++;
	}
	else
	{
		FreeHeapHistory[ FreeHeapHistoryStart ] = freeHeap;
		FreeHeapHistoryStart = ( FreeHeapHistoryStart + 1 ) % FREE_HEAP_HISTORY_MAX;
	}
}

// The remote service we wish to connect to.
static BLEUUID serviceUUID( "cba20d00-224d-11e6-9fb8-0002a5d5c51b" );
// The characteristic of the remote service we are interested in.
static BLEUUID charUUID( "cba20002-224d-11e6-9fb8-0002a5d5c51b" );
// The characteristic of the notification service we are interested in.
static BLEUUID notifyUUID( "cba20003-224d-11e6-9fb8-0002a5d5c51b" );

int SendDeviceChange( const char* host, const char* data, int bytes );
void SendChangedDevices();
void WriteToBLEDevice( BLE_COMMAND* BLECommand );

void handleRoot( AsyncWebServerRequest* request )
{
	digitalWrite( led, 1 );
	request->send( 200, "text/html", HOME_HTML );
	digitalWrite( led, 0 );
}

void handleNotFound( AsyncWebServerRequest* request )
{
	digitalWrite( led, 1 );
	String message = "File Not Found\n\n";
	message += "URI: ";
	message += request->url();
	message += "\nMethod: ";
	message += ( request->method() == HTTP_GET ) ? "GET" : "POST";
	message += "\nArguments: ";
	message += ( unsigned long )request->args();
	message += "\n";
	for ( uint8_t i = 0; i < request->args(); i++ )
	{
		message += " " + request->argName( i ) + ": " + request->arg( i ) + "\n";
	}
	request->send( 404, "text/plain", message );
	digitalWrite( led, 0 );
}

void notifyCallback(
    BLERemoteCharacteristic* pBLERemoteCharacteristic,
    uint8_t* pData,
    size_t length,
    bool isNotify )
{
	// Serial.print( "Notify callback for characteristic " );
	// Serial.print( pBLERemoteCharacteristic->getUUID().toString().c_str() );
	// Serial.print( " of data length " );
	// Serial.println( (unsigned long)length );

	if ( length > 50 )
	{
		length = 50;
	}

	memcpy( BLENotifyData, pData, length );
	BLENotifyLength = length;
}

static constexpr uint32_t scanTime = 30 * 1000; // 30 seconds scan time.

/**
   Scan for BLE servers and find the first one that advertises the service we are looking for.
*/
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
	/**
	    Called for each advertising BLE server.
	*/
	void onResult( const BLEAdvertisedDevice* advertisedDevice ) override
	{
		NumAdvertsSeen++;
		// We have found a device, let us now see if it contains the service we are looking for.
		NimBLEUUID id1( ( uint16_t )0x0d00 );
		NimBLEUUID id2( ( uint16_t )0xfd3d );
		NimBLEUUID devicId = advertisedDevice->getServiceDataUUID();
		if ( ( devicId == id1 ) || ( devicId == id2 ) )
		{
			if ( advertisedDevice->getServiceData().length() == 0 )
			{
				NumMatchedEmptyPayload++;
				return;
			}
			NumMatchedServiceData++;
			if ( BLE_Devices.AddDevice( advertisedDevice->getAddress().toString().c_str(), advertisedDevice->getRSSI(), ( uint8_t* )advertisedDevice->getServiceData().data(), advertisedDevice->getServiceData().length(), ( uint8_t* )advertisedDevice->getManufacturerData().data(), advertisedDevice->getManufacturerData().length() ) )
			{
				// Serial.printf( "Updated device: %s\n", advertisedDevice->getAddress().toString().c_str() );
				NumUpdates++;
				pendingSSEUpdate = true;
			}
			else
			{
				NumMatchedRejected++;
			}
			// else
			// {
			// 	Serial.printf( "Ignored device: %s\n", advertisedDevice->getAddress().toString().c_str() );
			// }
		}
	}; // onResult

	void onScanEnd( const NimBLEScanResults& results, int reason ) override
	{
		Serial.printf( "Scan ended reason = %d; restarting scan\n", reason );
		NimBLEDevice::getScan()->start( scanTime, false, true );
	}
}; // MyAdvertisedDeviceCallbacks

void setup()
{
	pinMode( led, OUTPUT );
	digitalWrite( led, 0 );
	Serial.begin( 921600 );
	Serial.println( "Starting Arduino BLE Client application..." );

	callbackMutex = xSemaphoreCreateMutex();
	if ( callbackMutex == nullptr )
	{
		Serial.println( "Failed to create callback mutex" );
	}

	AsyncWiFiManager wifiManager( &server, &dns );
	//    wifiManager.resetSettings();
	wifiManager.autoConnect( "SwitchBot_ESP32" );

	Serial.println( "" );
	Serial.print( "Connected to " );
	Serial.print( "IP address: " );
	Serial.println( WiFi.localIP() );

	// BLEDevice::init( "" );

	server.on( "/", handleRoot );

	server.on( "/api/v1/devices/table", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		Serial.println( "Received request for devices table" );
		request->send( 200, "text/html", DEVICES_TABLE_HTML );
		digitalWrite( led, 0 );
	} );

	sendBroadcast = millis();
	server.onNotFound( []( AsyncWebServerRequest* request ) {
		if ( ( request->url() == "/api/v1/callback/add" ) || ( request->url() == "/api/v1/callback/remove" ) || ( request->url() == "/api/v1/device/write" ) )
			return; // response object already created by onRequestBody

		String url = request->url();
		String clientIP = IPAddress( request->client()->getRemoteAddress() ).toString();
		Serial.printf( "API function %s not found from %s\n", url.c_str(), clientIP.c_str() );

		request->send( 404, "text/plain", "Not found" );
	} );

	server.onRequestBody(
	    []( AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total ) {
		    if ( request->method() == HTTP_POST )
		    {
			    digitalWrite( led, 1 );

			    if ( request->url() == "/api/v1/callback/add" )
			    {
				    Serial.println( "Received request for /api/v1/callback/add" );
				    String uri;
				    if ( TryGetJsonStringField( data, len, "uri", uri ) && uri.length() > 0 )
				    {
					    lockCallbacks();
					    bool addOk = OurCallbacks.Add( uri.c_str(), millis() );
					    unlockCallbacks();
					    if ( addOk )
					    {
						    request->send( 200, "text/plain", "OK" );
					    }
					    else
					    {
						    request->send( 429, "text/plain", "Too Many Requests" );
						    Serial.println( "Callback error 429" );
					    }
				    }
				    else
				    {
					    request->send( 400, "text/plain", "Bad Request" );
					    Serial.println( "Callback error 400" );
				    }
			    }
			    else if ( request->url() == "/api/v1/callback/remove" )
			    {
				    Serial.println( "Received request for /api/v1/callback/remove" );
				    String uri;
				    if ( TryGetJsonStringField( data, len, "uri", uri ) && uri.length() > 0 )
				    {
					    lockCallbacks();
					    bool removeOk = OurCallbacks.Remove( uri.c_str() );
					    unlockCallbacks();
					    if ( removeOk )
					    {
						    request->send( 200, "text/plain", "OK" );
					    }
					    else
					    {
						    request->send( 400, "text/plain", "Bad Request" );
					    }
				    }
				    else
				    {
					    request->send( 400, "text/plain", "Bad Request" );
				    }
			    }
			    else if ( request->url() == "/api/v1/device/write" )
			    {
				    String clientAddress;
				    String dataToWrite;
				    bool hasAddress = TryGetJsonStringField( data, len, "address", clientAddress );
				    bool hasData = TryGetJsonStringField( data, len, "data", dataToWrite );

				    if ( hasAddress && hasData && clientAddress.length() > 0 )
				    {
					    // Check that we have seen that device
					    int deviceIdx = BLE_Devices.FindDevice( clientAddress.c_str() );
					    if ( deviceIdx >= 0 )
					    {
							String sourcIP = IPAddress( request->client()->getRemoteAddress() ).toString();
						    Serial.printf( "Received request to write device %s with %s (%u) from %s\n", clientAddress.c_str(), dataToWrite.c_str(), ( unsigned int )dataToWrite.length(), sourcIP.c_str() );

						    if ( BLECommandQ.Find( clientAddress.c_str(), dataToWrite.c_str() ) )
						    {
							    // Same command already queued
							    request->send( 200, "text/plain", "OK" );
							    Serial.println( "Command already in the Q" );
						    }
						    else if ( BLECommandQ.Push( clientAddress.c_str(), dataToWrite.c_str(), sourcIP.c_str() ) )
						    {
							    request->send( 200, "text/plain", "OK" );
						    }
						    else
						    {
							    request->send( 429, "text/plain", "Too Many Requests" );
							    Serial.println( "I have too much in my command Q" );
						    }
					    }
					    else
					    {
						    request->send( 422, "text/plain", "Unknown device" );
						    Serial.printf( "Received request to write device %s but I have not seen that device)\n", clientAddress.c_str() );
					    }
				    }
				    else
				    {
					    request->send( 400, "text/plain", "Bad Request" );
				    }
			    }

			    digitalWrite( led, 0 );
		    }
	    } );

	server.on( "/api/v1/devices", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		Serial.println( "Received request for devices" );

		// Decide whether the caller wants raw JSON (e.g. API clients that send
		// Accept: application/json) or a pretty HTML page (browser).
		bool wantJson = false;
		if ( request->hasHeader( "Accept" ) )
		{
			String accept = request->getHeader( "Accept" )->value();
			wantJson = ( accept.indexOf( "application/json" ) >= 0 &&
						 accept.indexOf( "text/html" ) < 0 );
		}

		if ( wantJson )
		{
			char* buf = ( char* )malloc( 8192 );
			if ( buf )
			{
				BLE_Devices.AllToJson( buf, 8192, false, macAddress );
				Serial.println( buf );
				request->send( 200, "application/json", buf );
				free( buf );
			}
			else
			{
				Serial.println( "Failed to allocate buf for JSON" );
				RebootRequired = true;
			}
		}
		else
		{
			request->send( 200, "text/html", DEVICES_JSON_HTML );
		}
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/stats/free-heap-history", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		String out;
		out.reserve( 14000 );
		out = "{";
		out += "\"intervalMs\":60000";
		out += ",\"maxPoints\":" + String( FREE_HEAP_HISTORY_MAX );
		out += ",\"count\":" + String( FreeHeapHistoryCount );
		out += ",\"values\":[";
		for ( uint16_t i = 0; i < FreeHeapHistoryCount; i++ )
		{
			const uint16_t idx = ( FreeHeapHistoryStart + i ) % FREE_HEAP_HISTORY_MAX;
			out += String( FreeHeapHistory[ idx ] );
			if ( i + 1 < FreeHeapHistoryCount )
			{
				out += ",";
			}
		}
		out += "]}";

		request->send( 200, "application/json", out );
		digitalWrite( led, 0 );
	} );

		server.on( "/api/v1/stats", HTTP_GET, []( AsyncWebServerRequest* request ) {
			digitalWrite( led, 1 );

			String out = "{";
			out += "\"uptimeMs\":" + String( millis() );
			out += ",\"advertsSeenPerMinute\":" + String( LastAdvertsSeenPerMinute );
			out += ",\"matchedServiceDataPerMinute\":" + String( LastMatchedServiceDataPerMinute );
			out += ",\"matchedEmptyPayloadPerMinute\":" + String( LastMatchedEmptyPayloadPerMinute );
			out += ",\"matchedRejectedPerMinute\":" + String( LastMatchedRejectedPerMinute );
			out += ",\"updatesPerMinute\":" + String( LastUpdatesPerMinute );
			out += ",\"currentMinuteUpdates\":" + String( NumUpdates );
			out += ",\"noUpdateMinutes\":" + String( NumUpdatesAt0 );
			out += ",\"freeHeap\":" + String( LastFreeHeap );
			out += ",\"largestHeapBlock\":" + String( LastLargestHeapBlock );
			out += ",\"lastStatsAtMs\":" + String( LastStatsAt );
			out += ",\"cpuUsage\":" + String( LastCpuUsagePercent );
			out += "}";
			request->send( 200, "application/json", out );

			digitalWrite( led, 0 );
		} );

	server.on( "/api/v1/device", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		String address = request->arg( "address" );
		Serial.printf( "Received request for device: %s\n", address.c_str() );

		int deviceIdx = BLE_Devices.FindDevice( address.c_str() );

		char* buf = ( char* )malloc( 2048 );
		if ( buf )
		{
			BLE_Devices.DeviceToJson( deviceIdx, buf, 2048, macAddress );
			// Serial.println( buf );
			request->send( 200, "application/json", buf );
			free( buf );
		}
		else
		{
			Serial.println( "Failed to allocate buf for JSON" );
			RebootRequired = true;
		}

		digitalWrite( led, 0 );
	} );

	//	server.onNotFound( handleNotFound );

	_updateServer.setup( &server );

	server.addHandler( &bleEvents );
	server.begin();
	Serial.println( "HTTP server started" );

	BLEDevice::init( "" );

	// Retrieve a Scanner and set the callback we want to use to be informed when we
	// have detected a new device.  Specify that we want active scanning and start the
	// scan to run for 5 seconds.
	BLEScan* pBLEScan = BLEDevice::getScan();
	pBLEScan->setScanCallbacks( new MyAdvertisedDeviceCallbacks(), true );
	pBLEScan->setInterval( 510 );
	pBLEScan->setWindow( 200 );
	pBLEScan->setActiveScan( true );
	pBLEScan->setMaxResults( 0xFF );
	pBLEScan->start( 0, false, true );

	Serial.println( "Application started" );

	uint8_t mac[ 6 ];
	WiFi.macAddress( mac );
	sprintf( macAddress, "%0.2x:%0.2x:%0.2x:%0.2x:%0.2x:%0.2x", mac[ 5 ], mac[ 4 ], mac[ 3 ], mac[ 2 ], mac[ 1 ], mac[ 0 ] );

	LastFreeHeap = esp_get_free_heap_size();
	LastLargestHeapBlock = heap_caps_get_largest_free_block( MALLOC_CAP_8BIT );
	LastStatsAt = millis();
	RecordFreeHeapHistory( LastFreeHeap );
	SampleCpuUsage();

	if ( udp.listenMulticast( IPAddress( 239, 1, 2, 3 ), 1234 ) )
	{
		Serial.print( "UDP Listening on IP: " );
		Serial.println( WiFi.localIP() );
		udp.onPacket( []( AsyncUDPPacket packet ) {
			// Serial.println();
			// Serial.print( "UDP Packet: " );
			// Serial.printf( ", Data (len %i): ", packet.length() );
			// Serial.write( packet.data(), packet.length() );
			// Serial.println();
			// Serial.println();
			if ( strncmp( ( char* )packet.data(), "Are you there SwitchBot?", packet.length() ) == 0 )
			{
				Serial.println( "Received: Are you there SwitchBot?" );
				sendBroadcast = millis();
			}
		} );
	}

} // End of setup.

// This is the Arduino main loop function.
void loop()
{
	for ( ;; )
	{
		if ( RebootRequired )
		{
			// Allow watchdog to restart the CPU
			Serial.println( "Waiting for WD to reset system" );

			cli(); // Clear interrupts

			esp_task_wdt_config_t wdt_config;
			wdt_config.timeout_ms = 15;
			wdt_config.idle_core_mask = 0xFFFFFFFF;
			wdt_config.trigger_panic = true;

			esp_task_wdt_init( &wdt_config );
			esp_task_wdt_add( NULL );
			while ( true );
		}

		if ( pendingSSEUpdate && ( millis() - lastSSESend >= 1000 ) )
		{
			pendingSSEUpdate = false;
			lastSSESend = millis();
			bleEvents.send( "update", "ble", millis() );
		}

			if ( pendingSSEStats && ( millis() - lastSSEStatsSend >= 1000 ) )
			{
				pendingSSEStats = false;
				lastSSEStatsSend = millis();
				bleEvents.send( "update", "stats", millis() );
			}

		if ( millis() >= sendBroadcast )
		{
			// Send multicast
			// Serial.printf( "\n***Broadcasting my details: %s, %s***\n", macAddress, WiFi.localIP().toString().c_str() );
			udp.printf( "SwitchBot BLE Hub! %s", macAddress );
			sendBroadcast = millis() + 60000;

			if (NumUpdates == 0)
			{
				NumUpdatesAt0++;
			}
			else
			{
				NumUpdatesAt0 = 0;
			}

			if (NumUpdatesAt0 > 3)
			{
				Serial.println( "No BLE updates for 3 minutes, rebooting" );
				RebootRequired = true;
			}

			LastAdvertsSeenPerMinute = NumAdvertsSeen;
			LastMatchedServiceDataPerMinute = NumMatchedServiceData;
			LastMatchedEmptyPayloadPerMinute = NumMatchedEmptyPayload;
			LastMatchedRejectedPerMinute = NumMatchedRejected;
			LastUpdatesPerMinute = NumUpdates;
			Serial.printf( "BLE adverts %i/min, UUID matches %i/min, empty payload %i/min, rejected %i/min, updates %i/min\n", NumAdvertsSeen, NumMatchedServiceData, NumMatchedEmptyPayload, NumMatchedRejected, NumUpdates );
			NumAdvertsSeen = 0;
			NumMatchedServiceData = 0;
			NumMatchedEmptyPayload = 0;
			NumMatchedRejected = 0;
			NumUpdates = 0;

			// Report heap available
			uint32_t freeHeap = esp_get_free_heap_size();
			uint32_t largestHeapBlock = heap_caps_get_largest_free_block( MALLOC_CAP_8BIT );
			LastFreeHeap = freeHeap;
			LastLargestHeapBlock = largestHeapBlock;
			LastStatsAt = millis();
			pendingSSEStats = true;
			RecordFreeHeapHistory( freeHeap );
			SampleCpuUsage();
			Serial.printf( "\nFree Heap %i, Largest block %i, CPU %i%%\n\n", freeHeap, largestHeapBlock, LastCpuUsagePercent );
			if ( largestHeapBlock < 30000 )
			{
				Serial.println( "Low heap, rebooting" );
				RebootRequired = true;
			}
		}

		if ( millis() >= BLESending )
		{
			// Check if there is a BLE command to send
			BLE_COMMAND BLECommand;
			if ( BLECommandQ.Pop( &BLECommand ) )
			{
				digitalWrite( led, 1 );
				WriteToBLEDevice( &BLECommand );
				digitalWrite( led, 0 );
			}
		}

		if ( BLE_Devices.HasChanged() )
		{
			static char outstr[ 50 ];

			lockCallbacks();
			bool hasCallbacks = OurCallbacks.HasCallbacks();
			unlockCallbacks();
			if ( hasCallbacks )
			{
				SendChangedDevices();
			}
		}

		lockCallbacks();
		OurCallbacks.Check( millis() ); // Check if any of the registered callbacks have timedout
		unlockCallbacks();

		vTaskDelay( pdMS_TO_TICKS( 1 ) ); // yield to let other tasks run and reduce idle CPU burn on core 1
	} // end of endless loop ;-)

} // End of loop

int SendDeviceChange( const char* host, const char* data, int bytes )
{
	// host = "192.168.1.1", ip or dns

	Serial.printf( "Connecting to %s to send %s\n", host, data );

	WiFiClient client;
	HTTPClient http;

	// configure target server and url
	http.begin( client, host ); // HTTP
	http.addHeader( "Content-Type", "application/json" );
	http.setReuse( false );

	// start connection and send HTTP header
	int httpCode = http.POST( ( uint8_t* )data, bytes );

	// httpCode will be negative on error
	if ( httpCode > 0 )
	{
		// HTTP header has been send and Server response header has been handled
		Serial.printf( "[HTTP] POST response code: %d\n", httpCode );
	}
	else
	{
		Serial.printf( "[HTTP] POST failed, code %i, error: %s\n", httpCode, http.errorToString( httpCode ).c_str() );
	}

	http.end();

	return httpCode;
}

void SendChangedDevices()
{
	// This object changed so send to registered callbacks
	char deviceBuf[ 2048 ];
	int bytes = BLE_Devices.AllToJson( deviceBuf, sizeof( deviceBuf ), true, macAddress );
	if ( bytes > 0 )
	{
		char addresBuf[ 256 ];
		uint8_t i = 0;
		lockCallbacks();
		bool gotEntry = OurCallbacks.Get( i, addresBuf, sizeof( addresBuf ) );
		unlockCallbacks();
		while ( gotEntry )
		{
			if ( SendDeviceChange( addresBuf, deviceBuf, bytes ) == -1 )
			{
				// refused connection
				lockCallbacks();
				OurCallbacks.addRefusal( i );
				unlockCallbacks();
			}
			else
			{
				lockCallbacks();
				OurCallbacks.resetRefusal( i );
				unlockCallbacks();
			}

			i++;
			lockCallbacks();
			gotEntry = OurCallbacks.Get( i, addresBuf, sizeof( addresBuf ) );
			unlockCallbacks();
		}
	}
}

void WriteToBLEDevice( BLE_COMMAND* BLECommand )
{
	BLEScan* pBLEScan = BLEDevice::getScan();

	const BLEAddress bleAddress( BLECommand->Address, 0 );
	Serial.printf( "Sending command to BLE device: %s\n", BLECommand->Address );
	uint64_t requestAddress = bleAddress;

	NimBLEScanResults results = pBLEScan->getResults();
	uint8_t numResults = results.getCount();
	const NimBLEAdvertisedDevice* pDevice = nullptr;
	for ( int i = 0; i < numResults; i++ )
	{
		pDevice = results.getDevice( i );

		uint64_t deviceAddress = pDevice->getAddress();
		if ( deviceAddress == requestAddress )
		{
			Serial.println( "Found the Device in the scan" );
			break;
		}

		pDevice = nullptr;
	}

	pBLEScan->stop();
	delay( 100 );

	if ( pDevice != nullptr )
	{
		NimBLEClient* pBLEClient = NimBLEDevice::createClient();
		pBLEClient->setConnectTimeout( 5 * 1000 );
		pBLEClient->setConnectionParams( 32, 160, 0, 500 );

		bool complete = false;
		int retries = 5;

		while ( !complete && ( retries-- > 0 ) )
		{
			if ( pBLEClient->connect( pDevice, false, false, false ) )
			{
				Serial.println( "Device connected" );

				BLERemoteService* rs = pBLEClient->getService( serviceUUID );
				if ( rs != nullptr )
				{
					Serial.println( "Got remote service" );

					BLERemoteCharacteristic* rc = rs->getCharacteristic( charUUID );
					if ( rc != nullptr )
					{
						Serial.println( "Got remote characteristic" );

						BLERemoteCharacteristic* rn = nullptr;
						if ( ( ( BLECommand->Data[ 0 ] == 87 ) && ( BLECommand->Data[ 1 ] == 15 ) && ( BLECommand->Data[ 2 ] == 72 ) && ( BLECommand->Data[ 3 ] == 1 ) ) ||
						     ( BLECommand->Data[ 0 ] == 87 ) && ( BLECommand->Data[ 1 ] == 2 ) )
						{
							rn = rs->getCharacteristic( notifyUUID );

							if ( rn )
							{
								Serial.println( "Registering notification" );
								BLENotifyLength = 0;
								if ( !rn->subscribe( true, notifyCallback ) )
								{
									Serial.println( "Registering notification FAILED!" );
								}
							}
						}

						rc->writeValue( BLECommand->Data, BLECommand->DataLen );
						Serial.println( "Data sent" );

						if ( rn )
						{
							Serial.println( "Waiting for notification" );
							unsigned long endTime = millis() + 2000;
							while ( ( BLENotifyLength == 0 ) && ( millis() < endTime ) );
							if ( BLENotifyLength > 0 )
							{
								Serial.println( "Got notification" );

								char replyBuf[ 300 ];
								int idx = BLE_Devices.FindDevice( BLECommand->Address );
								SWITCHBOT Device;
								if ( BLE_Devices.GetSWDevice( idx, Device ) )
								{
									int bytes = 0;

									if ( Device.model == 'u' )
									{
										bytes = snprintf( replyBuf, sizeof( replyBuf ), "[{\"hubMAC\":\"%s\",\"address\":\"%s\",\"serviceData\":{\"model\":\"u\",\"modelName\":\"WoBulb\"},\"replyData\":[", macAddress, BLECommand->Address );
									}
									else if ( Device.model == 'x' )
									{
										bytes = snprintf( replyBuf, sizeof( replyBuf ), "[{\"hubMAC\":\"%s\",\"address\":\"%s\",\"serviceData\":{\"model\":\"x\",\"modelName\":\"WoBlindTilt\"},\"replyData\":[", macAddress, BLECommand->Address );
									}

									if ( bytes > 0 )
									{
										for ( int i = 0; ( i < BLENotifyLength ) && ( bytes < ( int )sizeof( replyBuf ) - 4 ); i++ )
										{
											bytes += snprintf( replyBuf + bytes, sizeof( replyBuf ) - bytes, "%i,", BLENotifyData[ i ] );
										}

										if ( bytes > 0 )
										{
											bytes--;
											bytes += snprintf( replyBuf + bytes, sizeof( replyBuf ) - bytes, "]}]" );
											char replyAddress[ 300 ];
											lockCallbacks();
											bool foundCallback = OurCallbacks.Find( BLECommand->ReplyTo, replyAddress, sizeof( replyAddress ) );
											unlockCallbacks();
											if ( foundCallback )
											{
												SendDeviceChange( replyAddress, replyBuf, bytes );
											}
											else
											{
												Serial.printf( "Callback URL %s not found (1)\n", BLECommand->ReplyTo );
											}
										}
									}
									else
									{
										Serial.printf( "Don't understand format for model %c\n", Device.model );
									}
								}
								else
								{
									Serial.printf( "Callback URL %s not found (2)\n", BLECommand->ReplyTo );
								}
							}

							Serial.println( "Unsubscribe from notification" );
							rn->unsubscribe();
						}
						complete = true;
					}
					else
					{
						Serial.println( "Failed to get characteristic" );
					}
				}
				else
				{
					Serial.println( "Failed to get service" );
				}

				pBLEClient->disconnect();
				Serial.println( "Disconnected device" );
			}
			else
			{
				Serial.println( "Failed to connected to device" );
			}
		}

		NimBLEDevice::deleteClient( pBLEClient );
	}
	else
	{
		Serial.println( "Device not found (3)" );
	}

	Serial.println( "Restarting BLE scan" );
	pBLEScan->start( 0, true, false );
	BLESending = millis() + 1000;
}
