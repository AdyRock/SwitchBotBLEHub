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

const char* version = "Hello! SwitchBot BLE Hub V2.11";

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

static bool TryGetJsonByteArrayField( const uint8_t* data, size_t len, const char* key, String& value )
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

	int pos = colonPos + 1;
	while ( pos < body.length() && ( body[ pos ] == ' ' || body[ pos ] == '\t' || body[ pos ] == '\r' || body[ pos ] == '\n' ) )
	{
		pos++;
	}

	if ( pos >= body.length() )
	{
		return false;
	}

	if ( body[ pos ] == '"' )
	{
		String stringValue;
		if ( !TryGetJsonStringField( data, len, key, stringValue ) )
		{
			return false;
		}

		stringValue.trim();
		if ( stringValue.length() == 0 )
		{
			return false;
		}

		if ( stringValue[ 0 ] == '[' )
		{
			value = stringValue;
		}
		else
		{
			value = "[" + stringValue + "]";
		}
		return true;
	}

	if ( body[ pos ] != '[' )
	{
		return false;
	}

	int endPos = pos;
	while ( endPos < body.length() && body[ endPos ] != ']' )
	{
		endPos++;
	}
	if ( endPos >= body.length() )
	{
		return false;
	}

	String list = body.substring( pos, endPos + 1 );
	String normalized = "[";
	int listPos = 1;
	bool added = false;
	while ( listPos < list.length() )
	{
		while ( listPos < list.length() && ( list[ listPos ] == ' ' || list[ listPos ] == '\t' || list[ listPos ] == ',' ) )
		{
			listPos++;
		}

		if ( listPos >= list.length() || list[ listPos ] == ']' )
		{
			break;
		}

		int valueStart = listPos;
		while ( listPos < list.length() && list[ listPos ] >= '0' && list[ listPos ] <= '9' )
		{
			listPos++;
		}

		if ( valueStart == listPos )
		{
			return false;
		}

		long byteValue = list.substring( valueStart, listPos ).toInt();
		if ( byteValue < 0 || byteValue > 255 )
		{
			return false;
		}

		if ( added )
		{
			normalized += ",";
		}
		normalized += String( byteValue );
		added = true;
	}

	if ( !added )
	{
		return false;
	}

	normalized += "]";
	value = normalized;
	return true;
}

static bool ParseByteListString( const String& value, uint8_t* outData, uint8_t maxLen, uint8_t& outLen )
{
	outLen = 0;
	if ( outData == nullptr || maxLen == 0 )
	{
		return false;
	}

	String list = value;
	list.trim();
	if ( list.length() < 3 || list[ 0 ] != '[' || list[ list.length() - 1 ] != ']' )
	{
		return false;
	}

	int pos = 1;
	while ( pos < list.length() - 1 )
	{
		while ( pos < list.length() - 1 && ( list[ pos ] == ' ' || list[ pos ] == '\t' || list[ pos ] == ',' ) )
		{
			pos++;
		}

		if ( pos >= list.length() - 1 )
		{
			break;
		}

		int start = pos;
		while ( pos < list.length() - 1 && list[ pos ] >= '0' && list[ pos ] <= '9' )
		{
			pos++;
		}

		if ( start == pos )
		{
			return false;
		}

		if ( outLen >= maxLen )
		{
			return false;
		}

		long byteValue = list.substring( start, pos ).toInt();
		if ( byteValue < 0 || byteValue > 255 )
		{
			return false;
		}

		outData[ outLen++ ] = ( uint8_t )byteValue;
	}

	return ( outLen > 0 );
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
        <p class="ver">Version 2.11</p>
        <div class="links">
          <a class="btn" href="/update">Update firmware</a>
					<a class="btn" href="/api/v1/stats/page">View runtime stats</a>
          <a class="btn" href="/api/v1/devices">View registered devices (JSON)</a>
          <a class="btn" href="/api/v1/devices/table">View registered devices (table)</a>
					<a class="btn" href="/api/v1/homey/page">View Homey registration and activity</a>
        </div>
      </div>
    </div>
  </body>
</html>
)HTMLEOF";

static const char STATS_HTML[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
	<head>
		<meta charset="UTF-8">
		<meta name="viewport" content="width=device-width, initial-scale=1">
		<title>SwitchBot BLE Hub Stats</title>
		<style>
			body { margin: 0; background: #1e1e1e; color: #d4d4d4; font-family: sans-serif; }
			.wrap { max-width: 980px; margin: 1.2rem auto; padding: 0 1rem; }
			.top { display: flex; align-items: center; justify-content: space-between; gap: 0.8rem; margin-bottom: 0.9rem; }
			.title { color: #9cdcfe; font-size: 1.45rem; font-weight: 700; }
			.home { text-decoration: none; background: #3c3c3c; color: #fff; border-radius: 6px; padding: 0.45rem 0.7rem; font-weight: 600; }
			.home:hover { background: #555; }
			.meta { color: #9aa0a6; font-size: 0.85rem; margin-bottom: 0.9rem; }
			.blocks { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 0.85rem; }
			.block { background: #252526; border: 1px solid #3c3c3c; border-radius: 10px; padding: 0.85rem; }
			.block h2 { margin: 0 0 0.6rem; color: #9cdcfe; font-size: 1.05rem; }
			.stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 0.55rem; }
			.stat { background: #1f1f1f; border: 1px solid #3a3a3a; border-radius: 6px; padding: 0.45rem 0.5rem; }
			.stat.clickable { cursor: pointer; border-color: #007acc; }
			.stat.clickable:hover { background: #2a3440; }
			.k { font-size: 0.75rem; color: #9aa0a6; }
			.v { font-size: 1rem; color: #d4d4d4; font-weight: 600; margin-top: 0.2rem; }
			#heapHistoryModal { position: fixed; inset: 0; background: rgba(0,0,0,0.6); display: none; align-items: center; justify-content: center; z-index: 20; }
			#heapHistoryDialog { width: min(940px, 95vw); background: #252526; border: 1px solid #3c3c3c; border-radius: 10px; padding: 0.9rem; }
			#heapHistoryHeader { display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.7rem; }
			#heapHistoryTitle { color: #9cdcfe; font-weight: 700; }
			#heapHistoryClose { background: #3c3c3c; color: #fff; border: none; border-radius: 5px; padding: 0.35rem 0.65rem; cursor: pointer; }
			#heapHistoryClose:hover { background: #555; }
			#heapHistoryCanvas { width: 100%; height: 320px; background: #1e1e1e; border: 1px solid #3c3c3c; border-radius: 6px; display: block; }
			#heapHistoryMeta { margin-top: 0.55rem; font-size: 0.8rem; color: #bbb; }
			.unknown-wrap { overflow-x: auto; }
			.unknown-table { width: 100%; border-collapse: collapse; font-size: 0.85rem; }
			.unknown-table th, .unknown-table td { border-bottom: 1px solid #3c3c3c; padding: 0.45rem 0.5rem; text-align: left; }
			.unknown-table th { color: #9cdcfe; font-weight: 600; }
			.unknown-empty { color: #9aa0a6; font-size: 0.85rem; }
			.section-head { display: flex; align-items: center; justify-content: space-between; gap: 0.6rem; margin: 0 0 0.6rem; }
			.section-head h2 { margin: 0; }
			.clear-btn { background: #3c3c3c; color: #fff; border: 1px solid #555; border-radius: 6px; padding: 0.3rem 0.55rem; font-size: 0.78rem; cursor: pointer; }
			.clear-btn:hover { background: #555; }
		</style>
	</head>
	<body>
		<div class="wrap">
			<div class="top">
				<div class="title">Runtime Stats</div>
				<a class="home" href="/">Home</a>
			</div>
			<div id="meta" class="meta">Loading...</div>
			<div class="blocks">
				<section class="block">
					<h2>BLE Stats</h2>
					<div class="stats" id="bleStats"></div>
				</section>
				<section class="block">
					<h2>Memory and System</h2>
					<div class="stats" id="memStats"></div>
				</section>
				<section class="block">
					<div class="section-head">
						<h2 id="unknownDevicesTitle">Unknown Devices</h2>
						<button class="clear-btn" type="button" onclick="clearUnknownDevices()">Clear</button>
					</div>
					<div class="unknown-wrap" id="unknownTypes"></div>
				</section>
			</div>
			<div id="heapHistoryModal" onclick="if(event.target===this) closeFreeHeapHistory()">
				<div id="heapHistoryDialog">
					<div id="heapHistoryHeader">
						<div id="heapHistoryTitle">Free Heap History</div>
						<button id="heapHistoryClose" onclick="closeFreeHeapHistory()">Close</button>
					</div>
					<canvas id="heapHistoryCanvas"></canvas>
					<div id="heapHistoryMeta"></div>
				</div>
			</div>
		</div>
		<script>
			var lastStats = null;
			var statsClockOffsetMs = 0;
			var nextStatsServerMs = 0;
			var lastChartHistory = null;
			var chartConfig = null;
			var statsRefreshTimer = null;
			var statsFetchInFlight = false;
			var statsErrorText = "";
			var statsErrorUntilMs = 0;

			function escAttr(s) {
				return String(s || "").replace(/&/g, "&amp;").replace(/"/g, "&quot;");
			}

			function mkStat(k, v, clickFn, hint) {
				var cls = clickFn ? "stat clickable" : "stat";
				var clickAttr = clickFn ? " onclick=\"" + clickFn + "()\"" : "";
				var titleAttr = hint ? ' title="' + escAttr(hint) + '"' : "";
				return '<div class="' + cls + '"' + clickAttr + titleAttr + '><div class="k">' + k + '</div><div class="v">' + v + '</div></div>';
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

			function updateStatsMeta() {
				if (statsErrorText && Date.now() < statsErrorUntilMs) {
					document.getElementById("meta").textContent = "Failed to load stats: " + statsErrorText;
					return;
				}
				document.getElementById("meta").textContent = "Updated: " + new Date().toLocaleTimeString() + " | Next update: " + getStatsRemainingSeconds() + "s";
			}

			function render(s) {
				var ble = "";
				ble += mkStat("Registered devices", s.registeredDevices || 0, null, "Number of known SwitchBot devices currently registered (max 50).");
				ble += mkStat("Adverts/min", s.advertsSeenPerMinute || 0, "openAdvertsHistory", "Total BLE advertisements seen per minute. Click to view recent history.");
				ble += mkStat("UUID match/min", s.matchedServiceDataPerMinute || 0, null, "Advertisements per minute matching supported SwitchBot service UUIDs.");
				ble += mkStat("Empty payload/min", s.matchedEmptyPayloadPerMinute || 0, null, "Matched advertisements per minute that had no service payload data.");
				ble += mkStat("Bad data/min", s.badDataRejectedPerMinute || 0, null, "Matched advertisements per minute from known device types that failed data validation (wrong payload size).");
				ble += mkStat("Matches/min", s.updatesPerMinute || 0, "openMatchesHistory", "Matched advertisements per minute accepted by AddDevice (new, changed, or unchanged match). Click to view recent history.");
				ble += mkStat("Actual updates/min", s.actualDataUpdatesPerMinute || 0, "openActualUpdatesHistory", "New devices or existing devices whose stored data changed per minute. Click to view recent history.");
				ble += mkStat("No-update minutes", s.noUpdateMinutes || 0, null, "Consecutive minutes with zero accepted device updates.");
				document.getElementById("bleStats").innerHTML = ble;

				var mem = "";
				mem += mkStat("Free heap", s.freeHeap || 0, "openFreeHeapHistory", "Current free heap memory in bytes. Click to view recent history.");
				mem += mkStat("Largest block", s.largestHeapBlock || 0, null, "Size in bytes of the largest currently available contiguous heap block.");
				mem += mkStat("CPU usage", (s.cpuUsage || 0) + "%", null, "Estimated CPU busy percentage sampled over the last stats interval.");
				mem += mkStat("Uptime (D, HH:MM:SS)", formatUptime(s.uptimeMs || 0), null, "Time since the hub last booted.");
				document.getElementById("memStats").innerHTML = mem;
				renderUnknownTypes(s.unknownTypes || []);

				updateStatsMeta();
			}

			function typeHex(v) {
				var n = Number(v) & 0xFF;
				var h = n.toString(16).toUpperCase();
				if (h.length < 2) h = "0" + h;
				return "0x" + h;
			}

			function typeChar(v) {
				var n = Number(v) & 0xFF;
				if (n >= 32 && n <= 126) return String.fromCharCode(n);
				return "-";
			}

			var unknownTypesData = [];
			var unknownTypesSortCol = 'count';
			var unknownTypesSortAsc = false;

			function sortAndRenderUnknownTypes() {
				var host = document.getElementById("unknownTypes");
				if (!unknownTypesData || !unknownTypesData.length) {
					host.innerHTML = '<div class="unknown-empty">No unknown types seen yet.</div>';
					return;
				}
				var col = unknownTypesSortCol;
				var asc = unknownTypesSortAsc;
				var sorted = unknownTypesData.slice().sort(function(a, b) {
					if (col === 'count') {
						var diff = (Number(a.count || 0)) - (Number(b.count || 0));
						return asc ? diff : -diff;
					} else if (col === 'mac') {
						var ma = (a.mac || '').toUpperCase();
						var mb = (b.mac || '').toUpperCase();
						var diff = ma < mb ? -1 : ma > mb ? 1 : 0;
						return asc ? diff : -diff;
					} else {
						var ta = (Number(a.type || 0) & 0xFF);
						var tb = (Number(b.type || 0) & 0xFF);
						var sa = a.subtype || '';
						var sb = b.subtype || '';
						var keyA = typeHex(ta) + '|' + sa;
						var keyB = typeHex(tb) + '|' + sb;
						var diff = keyA < keyB ? -1 : keyA > keyB ? 1 : 0;
						return asc ? diff : -diff;
					}
				});
				function thStyle(col2) {
					return ' style="cursor:pointer;user-select:none;" onclick="unknownTypesSortBy(\'' + col2 + '\')"';
				}
				function sortArrow(col2) {
					if (unknownTypesSortCol !== col2) return '';
					return unknownTypesSortAsc ? ' &#9650;' : ' &#9660;';
				}
				var h = '';
				h += '<table class="unknown-table">';
				h += '<thead><tr>';
				h += '<th' + thStyle('type') + '>Type (Hex)' + sortArrow('type') + '</th>';
				h += '<th>ASCII</th>';
				h += '<th' + thStyle('type') + '>Sub-type (Hex)' + sortArrow('type') + '</th>';
				h += '<th' + thStyle('mac') + '>MAC' + sortArrow('mac') + '</th>';
				h += '<th' + thStyle('count') + '>BLE Updates' + sortArrow('count') + '</th>';
				h += '</tr></thead><tbody>';
				for (var i = 0; i < sorted.length; i++) {
					var t = Number(sorted[i].type || 0) & 0xFF;
					var c = Number(sorted[i].count || 0);
					var sub = sorted[i].subtype ? sorted[i].subtype : '-';
					var mac = sorted[i].mac ? sorted[i].mac : '-';
					h += '<tr><td>' + typeHex(t) + '</td><td>' + typeChar(t) + '</td><td>' + sub + '</td><td>' + mac + '</td><td>' + c + '</td></tr>';
				}
				h += '</tbody></table>';
				host.innerHTML = h;
			}

			function unknownTypesSortBy(col) {
				if (unknownTypesSortCol === col) {
					unknownTypesSortAsc = !unknownTypesSortAsc;
				} else {
					unknownTypesSortCol = col;
					unknownTypesSortAsc = col === 'type';
				}
				sortAndRenderUnknownTypes();
			}

			function renderUnknownTypes(types) {
				unknownTypesData = types || [];
				var titleEl = document.getElementById('unknownDevicesTitle');
				if (titleEl) titleEl.textContent = 'Unknown Devices (' + unknownTypesData.length + ')';
				sortAndRenderUnknownTypes();
			}

			function clearUnknownDevices() {
				fetch('/api/v1/stats/unknown-types/clear', { method: 'POST' })
					.then(function(r) {
						if (!r.ok) throw new Error('HTTP ' + r.status);
						return r.json();
					})
					.then(function() {
						renderUnknownTypes([]);
						loadStats();
					})
					.catch(function(e) {
						statsErrorText = String(e);
						statsErrorUntilMs = Date.now() + 15000;
						updateStatsMeta();
					});
			}

			function closeFreeHeapHistory() {
				document.getElementById("heapHistoryModal").style.display = "none";
			}

			function openFreeHeapHistory() {
				openHistoryChart("Free Heap History", "/api/v1/stats/free-heap-history", "#4ec94e", "bytes");
			}

			function openAdvertsHistory() {
				openHistoryChart("Adverts/min History", "/api/v1/stats/adverts-history", "#4ec9ff", "per min");
			}

			function openMatchesHistory() {
				openHistoryChart("Matches/min History", "/api/v1/stats/matches-history", "#d7ba7d", "per min");
			}

			function openActualUpdatesHistory() {
				openHistoryChart("Actual updates/min History", "/api/v1/stats/actual-updates-history", "#ce9178", "per min");
			}

			function openHistoryChart(title, url, lineColor, units) {
				chartConfig = { title: title, url: url, lineColor: lineColor, units: units || "" };
				document.getElementById("heapHistoryTitle").textContent = title;
				document.getElementById("heapHistoryModal").style.display = "flex";
				loadHistoryChart();
			}

			function drawHistoryChart(values, intervalMs) {
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

				ctx.strokeStyle = (chartConfig && chartConfig.lineColor) ? chartConfig.lineColor : "#4ec94e";
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
				var units = (chartConfig && chartConfig.units) ? (" " + chartConfig.units) : "";
				meta.textContent = "Samples: " + values.length + " | Window: ~" + spanHours + "h | Min: " + minV + units + " | Max: " + maxV + units + " | Latest: " + latest + units;
			}

			function loadHistoryChart() {
				var meta = document.getElementById("heapHistoryMeta");
				meta.textContent = "Loading...";
				if (!chartConfig || !chartConfig.url) {
					meta.textContent = "No chart configured";
					return;
				}
				fetch(chartConfig.url, { headers: { Accept: "application/json" } })
					.then(function(r) { return r.json(); })
					.then(function(data) {
						lastChartHistory = data;
						drawHistoryChart(data.values || [], data.intervalMs || 15000);
					})
					.catch(function(e) {
						meta.textContent = "Failed to load history: " + e;
						drawHistoryChart([], 15000);
					});
			}

			window.addEventListener("resize", function() {
				if (document.getElementById("heapHistoryModal").style.display === "flex" && lastChartHistory) {
					drawHistoryChart(lastChartHistory.values || [], lastChartHistory.intervalMs || 15000);
				}
			});

			function isHistoryChartOpen() {
				return document.getElementById("heapHistoryModal").style.display === "flex";
			}

			function loadStats() {
				if (statsFetchInFlight) return;
				statsFetchInFlight = true;
				fetch("/api/v1/stats", { headers: { Accept: "application/json" } })
					.then(function(r) { return r.json(); })
					.then(function(s) {
						statsErrorText = "";
						statsErrorUntilMs = 0;
						lastStats = s;
						statsClockOffsetMs = Date.now() - (s.uptimeMs || 0);
						nextStatsServerMs = (s.lastStatsAtMs || 0) + (s.statsIntervalMs || 15000);
						render(s);
						if (isHistoryChartOpen() && chartConfig && chartConfig.url) {
							loadHistoryChart();
						}

						var serverNowMs = Date.now() - statsClockOffsetMs;
						var msUntilNext = Math.max(250, nextStatsServerMs - serverNowMs + 50);
						if (statsRefreshTimer) clearTimeout(statsRefreshTimer);
						statsRefreshTimer = setTimeout(loadStats, msUntilNext);
					})
					.catch(function(e) {
						statsErrorText = String(e);
						statsErrorUntilMs = Date.now() + 15000;
						updateStatsMeta();
						if (statsRefreshTimer) clearTimeout(statsRefreshTimer);
						statsRefreshTimer = setTimeout(loadStats, 5000);
					})
					.finally(function() {
						statsFetchInFlight = false;
					});
			}

			loadStats();
			setInterval(function() { if (lastStats) render(lastStats); }, 1000);
		</script>
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
			.top { display: flex; align-items: center; justify-content: space-between; margin-bottom: 0.8rem; }
			h1 { color: #9cdcfe; font-family: sans-serif; margin: 0; }
			.home { text-decoration: none; background: #3c3c3c; color: #fff; border-radius: 6px; padding: 0.45rem 0.7rem; font-weight: 600; font-family: sans-serif; }
			.home:hover { background: #555; }
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
		<div class="top">
			<h1>Registered Devices</h1>
			<a class="home" href="/">Home</a>
		</div>
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
			.top { display: flex; align-items: center; justify-content: space-between; gap: 0.8rem; margin-bottom: 0.8rem; }
			h1 { color: #9cdcfe; margin: 0; }
			.home { text-decoration: none; background: #3c3c3c; color: #fff; border-radius: 6px; padding: 0.45rem 0.7rem; font-weight: 600; }
			.home:hover { background: #555; }
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
		<div class="top">
			<h1>Registered Devices</h1>
			<a class="home" href="/">Home</a>
		</div>
		<span id="ts"></span><span id="live">&#9679; connecting...</span>
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
			var lastHeapHistory = null;

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
						drawFreeHeapHistory(data.values || [], data.intervalMs || 15000);
					})
					.catch(function(e) {
						meta.textContent = "Failed to load history: " + e;
						drawFreeHeapHistory([], 15000);
					});
			}

			window.addEventListener("resize", function() {
				if (document.getElementById("heapHistoryModal").style.display === "flex" && lastHeapHistory) {
					drawFreeHeapHistory(lastHeapHistory.values || [], lastHeapHistory.intervalMs || 15000);
				}
			});


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

			load();

      var es = new EventSource("/api/v1/events");
			es.addEventListener("ble", function() { load(); });
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

static const char HOMEY_MONITOR_HTML[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
	<head>
		<meta charset="UTF-8">
		<meta name="viewport" content="width=device-width, initial-scale=1">
		<title>Homey Monitor</title>
		<style>
			body { margin: 0; background: #1e1e1e; color: #d4d4d4; font-family: sans-serif; }
			.wrap { width: 100%; max-width: 100vw; box-sizing: border-box; margin: 1.2rem auto; padding: 0 1rem; }
			.top { display: flex; align-items: center; justify-content: space-between; gap: 0.8rem; margin-bottom: 0.9rem; }
			.title { color: #9cdcfe; font-size: 1.45rem; font-weight: 700; }
			.home { text-decoration: none; background: #3c3c3c; color: #fff; border-radius: 6px; padding: 0.45rem 0.7rem; font-weight: 600; }
			.home:hover { background: #555; }
			.grid { display: grid; grid-template-columns: 1fr; gap: .9rem; }
			.card { background: #252526; border: 1px solid #3c3c3c; border-radius: 10px; padding: .85rem; min-width: 0; }
			.card h2 { margin: 0 0 .7rem; color: #9cdcfe; font-size: 1.05rem; }
			.table-wrap { overflow-x: auto; width: 100%; }
			table { width: 100%; table-layout: fixed; border-collapse: collapse; font-size: .86rem; }
			th, td { border-bottom: 1px solid #3c3c3c; padding: .45rem .5rem; text-align: left; vertical-align: top; overflow: hidden; }
			th { color: #9cdcfe; font-weight: 600; }
			td.wrap-cell { white-space: pre-wrap; word-break: break-all; }
			td.nowrap-cell { white-space: nowrap; }
			.empty { color: #9aa0a6; font-size: .86rem; }
			.meta { color: #9aa0a6; font-size: .82rem; margin-top: .35rem; }
			code { color: #d7ba7d; }
		</style>
	</head>
	<body>
		<div class="wrap">
			<div class="top">
				<div class="title">Homey Registration and Activity</div>
				<a class="home" href="/">Home</a>
			</div>
			<div class="grid">
				<section class="card">
					<h2>Registered Homey Devices</h2>
					<div id="registered"></div>
					<div id="meta" class="meta">Loading...</div>
				</section>
				<section class="card">
					<h2>Last 10 Push Updates</h2>
					<div id="pushUpdates"></div>
				</section>
				<section class="card">
					<h2>Last 10 Received BLE Commands</h2>
					<div id="bleCommands"></div>
				</section>
			</div>
		</div>
		<script>
			var monitorClockOffsetMs = 0;
			var monitorRefreshTimer = null;
			var monitorFetchInFlight = false;
			var monitorLastUpdatedText = "-";
			var lastBleCommandSeq = -1;
			var bleSeqTimer = null;
			var lastPushUpdateSeq = -1;
			var pushSeqTimer = null;
			var pushData = [];
			var cmdData = [];

			function esc(s) {
				return String(s || "")
					.replace(/&/g, "&amp;")
					.replace(/</g, "&lt;")
					.replace(/>/g, "&gt;")
					.replace(/\"/g, "&quot;")
					.replace(/'/g, "&#39;");
			}

			function fmtAge(ms) {
				var n = Number(ms || 0);
				if (!n) return "-";
				var serverNowMs = Date.now() - monitorClockOffsetMs;
				var ageMs = serverNowMs - n;
				if (ageMs < 0) ageMs = 0;
				var s = Math.floor(ageMs / 1000);
				if (s < 60) return s + "s ago";
				var m = Math.floor(s / 60); s = s % 60;
				if (m < 60) return m + "m " + s + "s ago";
				var h = Math.floor(m / 60); m = m % 60;
				return h + "h " + m + "m ago";
			}

			function updateMonitorMeta(statusPrefix) {
				var prefix = statusPrefix || "Updated: " + monitorLastUpdatedText;
				document.getElementById('meta').textContent = prefix;
			}

			function renderRegistered(list) {
				var host = document.getElementById("registered");
				if (!list || !list.length) {
					host.innerHTML = '<div class="empty">No Homey callbacks currently registered.</div>';
					return;
				}
				var h = '<div class="table-wrap"><table><thead><tr><th>IP</th><th>Callback URI</th></tr></thead><tbody>';
				for (var i = 0; i < list.length; i++) {
					var r = list[i] || {};
					h += '<tr><td>' + esc(r.ip || '-') + '</td><td><code>' + esc(r.uri || '') + '</code></td></tr>';
				}
				h += '</tbody></table></div>';
				host.innerHTML = h;
			}

			function renderPush(list) {
				var host = document.getElementById("pushUpdates");
				if (!list || !list.length) {
					host.innerHTML = '<div class="empty">No push updates sent yet.</div>';
					return;
				}
				var h = '<div class="table-wrap"><table style="table-layout:fixed"><colgroup>'
					+ '<col style="width:7em"><col style="width:9em"><col><col style="width:5em"><col style="width:5em">'
					+ '</colgroup><thead><tr><th>At</th><th>IP</th><th>Payload</th><th>Bytes</th><th>HTTP</th></tr></thead><tbody>';
				for (var i = 0; i < list.length; i++) {
					var e = list[i] || {};
					h += '<tr><td class="nowrap-cell">' + fmtAge(e.atMs) + '</td><td class="nowrap-cell">' + esc(e.ip || '-') + '</td><td class="wrap-cell"><code>' + esc(e.payload || '') + '</code></td><td>' + Number(e.bytes || 0) + '</td><td>' + Number(e.httpCode || 0) + '</td></tr>';
				}
				h += '</tbody></table></div>';
				host.innerHTML = h;
			}

			function renderCmd(list) {
				var host = document.getElementById("bleCommands");
				if (!list || !list.length) {
					host.innerHTML = '<div class="empty">No BLE commands received yet.</div>';
					return;
				}
				var h = '<div class="table-wrap"><table style="table-layout:fixed"><colgroup>'
					+ '<col style="width:7em"><col style="width:9em"><col style="width:12em"><col><col style="width:8em">'
					+ '</colgroup><thead><tr><th>At</th><th>Source IP</th><th>Address</th><th>Data</th><th>Result</th></tr></thead><tbody>';
				for (var i = 0; i < list.length; i++) {
					var e = list[i] || {};
					h += '<tr><td class="nowrap-cell">' + fmtAge(e.atMs) + '</td><td class="nowrap-cell">' + esc(e.sourceIp || '-') + '</td><td class="nowrap-cell"><code>' + esc(e.address || '') + '</code></td><td class="wrap-cell"><code>' + esc(e.data || '') + '</code></td><td class="nowrap-cell">' + esc(e.result || '') + '</td></tr>';
				}
				h += '</tbody></table></div>';
				host.innerHTML = h;
			}

			function load() {
				if (monitorFetchInFlight) return;
				monitorFetchInFlight = true;
				fetch('/api/v1/homey/monitor', { headers: { Accept: 'application/json' } })
					.then(function(r) { if (!r.ok) throw new Error('HTTP ' + r.status); return r.json(); })
					.then(function(d) {
						pushData = d.pushUpdates || [];
						cmdData = d.bleCommands || [];
						renderRegistered(d.registered || []);
						renderPush(pushData);
						renderCmd(cmdData);

						if (d.bleCommandSeq !== undefined) lastBleCommandSeq = Number(d.bleCommandSeq);
						if (d.pushUpdateSeq !== undefined) lastPushUpdateSeq = Number(d.pushUpdateSeq);

						monitorClockOffsetMs = Date.now() - Number(d.uptimeMs || 0);
						monitorLastUpdatedText = new Date().toLocaleTimeString();
						updateMonitorMeta();
					})
					.catch(function(e) {
						document.getElementById('registered').innerHTML = '<div class="empty">Error loading data: ' + esc(e) + '</div>';
						updateMonitorMeta('Retrying...');
						if (monitorRefreshTimer) clearTimeout(monitorRefreshTimer);
						monitorRefreshTimer = setTimeout(load, 5000);
					})
					.finally(function() {
						monitorFetchInFlight = false;
					});
			}

			function pollBleSeq() {
				fetch('/api/v1/homey/bleseq')
					.then(function(r) { return r.json(); })
					.then(function(d) {
						var seq = Number(d.seq);
						if (lastBleCommandSeq >= 0 && seq !== lastBleCommandSeq) {
							if (monitorRefreshTimer) clearTimeout(monitorRefreshTimer);
							load();
						}
					})
					.catch(function() {})
					.finally(function() {
						bleSeqTimer = setTimeout(pollBleSeq, 500);
					});
			}

			function pollPushSeq() {
				fetch('/api/v1/homey/pushseq')
					.then(function(r) { return r.json(); })
					.then(function(d) {
						var seq = Number(d.seq);
						if (lastPushUpdateSeq >= 0 && seq !== lastPushUpdateSeq) {
							if (monitorRefreshTimer) clearTimeout(monitorRefreshTimer);
							load();
						}
					})
					.catch(function() {})
					.finally(function() {
						pushSeqTimer = setTimeout(pollPushSeq, 5000);
					});
			}

			load();
			pollBleSeq();
			pollPushSeq();
			setInterval(function() {
				updateMonitorMeta();
				if (pushData.length) renderPush(pushData);
				if (cmdData.length) renderCmd(cmdData);
			}, 1000);
		</script>
	</body>
</html>
)HTMLEOF";

BLE_Device BLE_Devices;
ClientCallbacks OurCallbacks;
SemaphoreHandle_t callbackMutex = nullptr;
SemaphoreHandle_t historyMutex = nullptr;

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

static inline void lockHistory()
{
	if ( historyMutex != nullptr )
	{
		xSemaphoreTake( historyMutex, portMAX_DELAY );
	}
}

static inline void unlockHistory()
{
	if ( historyMutex != nullptr )
	{
		xSemaphoreGive( historyMutex );
	}
}

CommandQ BLECommandQ;
AsyncWebServer server( 80 );
DNSServer dns;
AsyncUDP udp;
AsyncEventSource bleEvents( "/api/v1/events" );

const IPAddress DISCOVERY_MULTICAST_IP( 239, 1, 2, 3 );
const uint16_t DISCOVERY_PORT = 1234;

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
int32_t NumActualDataUpdates = 0;
int32_t NumAdvertsSeen = 0;
int32_t NumMatchedServiceData = 0;
int32_t NumMatchedEmptyPayload = 0;
int32_t NumMatchedRejected = 0;
int32_t NumBadDataRejected = 0;
int32_t NumUpdatesAt0 = 0;
int32_t NumZeroUpdateIntervals = 0;
volatile int32_t LastUpdatesPerMinute = 0;
volatile int32_t LastActualDataUpdatesPerMinute = 0;
volatile int32_t LastAdvertsSeenPerMinute = 0;
volatile int32_t LastMatchedServiceDataPerMinute = 0;
volatile int32_t LastMatchedEmptyPayloadPerMinute = 0;
volatile int32_t LastMatchedRejectedPerMinute = 0;
volatile int32_t LastBadDataRejectedPerMinute = 0;
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
unsigned long nextStatsSample = 0;
const uint32_t STATS_SAMPLE_MS = 15000;
const uint32_t STATS_PER_MINUTE_SCALE = 60000 / STATS_SAMPLE_MS;
const uint16_t FREE_HEAP_HISTORY_MAX = 1000;
const uint8_t UNKNOWN_TYPE_MAX = 100;
const uint8_t HOMEY_HISTORY_MAX = 10;

struct PUSH_UPDATE_LOG_ENTRY
{
	uint32_t atMs;
	char payload[ 512 ];
	char ip[ 64 ];
	int bytes;
	int httpCode;
};

struct BLE_COMMAND_LOG_ENTRY
{
	uint32_t atMs;
	char sourceIp[ 64 ];
	char address[ 18 ];
	char data[ 80 ];
	char result[ 32 ];
};

PUSH_UPDATE_LOG_ENTRY PushUpdateHistory[ HOMEY_HISTORY_MAX ];
uint8_t PushUpdateHistoryStart = 0;
uint8_t PushUpdateHistoryCount = 0;
uint32_t PushUpdateSeq = 0;

BLE_COMMAND_LOG_ENTRY BleCommandHistory[ HOMEY_HISTORY_MAX ];
uint8_t BleCommandHistoryStart = 0;
uint8_t BleCommandHistoryCount = 0;
uint32_t BleCommandSeq = 0;

uint32_t FreeHeapHistory[ FREE_HEAP_HISTORY_MAX ];
uint16_t FreeHeapHistoryStart = 0;
uint16_t FreeHeapHistoryCount = 0;
int32_t AdvertsPerMinuteHistory[ FREE_HEAP_HISTORY_MAX ];
int32_t MatchesPerMinuteHistory[ FREE_HEAP_HISTORY_MAX ];
int32_t ActualUpdatesPerMinuteHistory[ FREE_HEAP_HISTORY_MAX ];
uint16_t BleRateHistoryStart = 0;
uint16_t BleRateHistoryCount = 0;
struct UNKNOWN_TYPE_ENTRY
{
	uint8_t type;
	uint8_t subtypeB0;
	uint8_t subtypeB1;
	uint8_t subtypeB2;
	bool hasSubtype;
	char mac[ 18 ];
	uint32_t count;
};
UNKNOWN_TYPE_ENTRY UnknownTypes[ UNKNOWN_TYPE_MAX ];
uint8_t UnknownTypeCount = 0;

static void ExtractIpFromUri( const char* uri, char* outIp, int outSize )
{
	if ( outIp == nullptr || outSize <= 0 )
	{
		return;
	}

	outIp[ 0 ] = 0;
	if ( uri == nullptr || uri[ 0 ] == 0 )
	{
		return;
	}

	const char* host = strstr( uri, "://" );
	if ( host != nullptr )
	{
		host += 3;
	}
	else
	{
		host = uri;
	}

	int i = 0;
	while ( host[ i ] != 0 && host[ i ] != '/' && host[ i ] != ':' && i < outSize - 1 )
	{
		outIp[ i ] = host[ i ];
		i++;
	}
	outIp[ i ] = 0;
}

static String JsonEscape( const char* s )
{
	if ( s == nullptr )
	{
		return "";
	}

	String out;
	out.reserve( strlen( s ) + 8 );
	for ( const char* p = s; *p != 0; p++ )
	{
		const uint8_t ch = ( uint8_t )*p;
		switch ( ch )
		{
			case '\\':
				out += "\\\\";
				break;
			case '"':
				out += "\\\"";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				if ( ch < 0x20 )
				{
					char esc[ 7 ];
					snprintf( esc, sizeof( esc ), "\\u%04X", ( unsigned int )ch );
					out += esc;
				}
				else
				{
					out += ( char )ch;
				}
				break;
		}
	}

	return out;
}

static bool IsHexDigitChar( char c )
{
	return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
}

static bool IsValidMacAddress( const char* mac )
{
	if ( mac == nullptr )
	{
		return false;
	}

	for ( int i = 0; i < 17; i++ )
	{
		char c = mac[ i ];
		if ( c == '\0' )
		{
			return false;
		}
		if ( ( i % 3 ) == 2 )
		{
			if ( c != ':' )
			{
				return false;
			}
		}
		else if ( !IsHexDigitChar( c ) )
		{
			return false;
		}
	}

	return mac[ 17 ] == '\0';
}

static void RecordPushUpdate( const char* target, const char* payload, int bytes, int httpCode )
{
	char ip[ 64 ] = {0};
	ExtractIpFromUri( target, ip, sizeof( ip ) );

	lockHistory();
	uint8_t idx;
	if ( PushUpdateHistoryCount < HOMEY_HISTORY_MAX )
	{
		idx = ( PushUpdateHistoryStart + PushUpdateHistoryCount ) % HOMEY_HISTORY_MAX;
		PushUpdateHistoryCount++;
	}
	else
	{
		idx = PushUpdateHistoryStart;
		PushUpdateHistoryStart = ( PushUpdateHistoryStart + 1 ) % HOMEY_HISTORY_MAX;
	}

	PushUpdateHistory[ idx ].atMs = millis();
	PushUpdateHistory[ idx ].bytes = bytes;
	PushUpdateHistory[ idx ].httpCode = httpCode;
	PushUpdateHistory[ idx ].payload[ 0 ] = 0;
	if ( payload != nullptr && bytes > 0 )
	{
		int copyLen = bytes;
		if ( copyLen > ( int )sizeof( PushUpdateHistory[ idx ].payload ) - 1 )
		{
			copyLen = ( int )sizeof( PushUpdateHistory[ idx ].payload ) - 1;
		}
		memcpy( PushUpdateHistory[ idx ].payload, payload, copyLen );
		PushUpdateHistory[ idx ].payload[ copyLen ] = 0;
	}
	strncpy( PushUpdateHistory[ idx ].ip, ip, sizeof( PushUpdateHistory[ idx ].ip ) - 1 );
	PushUpdateHistory[ idx ].ip[ sizeof( PushUpdateHistory[ idx ].ip ) - 1 ] = 0;
	PushUpdateSeq++;
	unlockHistory();
}

static void RecordBleCommandRequest( const char* sourceIp, const char* address, const char* data, const char* result )
{
	lockHistory();
	uint8_t idx;
	if ( BleCommandHistoryCount < HOMEY_HISTORY_MAX )
	{
		idx = ( BleCommandHistoryStart + BleCommandHistoryCount ) % HOMEY_HISTORY_MAX;
		BleCommandHistoryCount++;
	}
	else
	{
		idx = BleCommandHistoryStart;
		BleCommandHistoryStart = ( BleCommandHistoryStart + 1 ) % HOMEY_HISTORY_MAX;
	}

	BleCommandHistory[ idx ].atMs = millis();
	strncpy( BleCommandHistory[ idx ].sourceIp, ( sourceIp != nullptr ? sourceIp : "" ), sizeof( BleCommandHistory[ idx ].sourceIp ) - 1 );
	BleCommandHistory[ idx ].sourceIp[ sizeof( BleCommandHistory[ idx ].sourceIp ) - 1 ] = 0;
	strncpy( BleCommandHistory[ idx ].address, ( address != nullptr ? address : "" ), sizeof( BleCommandHistory[ idx ].address ) - 1 );
	BleCommandHistory[ idx ].address[ sizeof( BleCommandHistory[ idx ].address ) - 1 ] = 0;
	strncpy( BleCommandHistory[ idx ].data, ( data != nullptr ? data : "" ), sizeof( BleCommandHistory[ idx ].data ) - 1 );
	BleCommandHistory[ idx ].data[ sizeof( BleCommandHistory[ idx ].data ) - 1 ] = 0;
	strncpy( BleCommandHistory[ idx ].result, ( result != nullptr ? result : "" ), sizeof( BleCommandHistory[ idx ].result ) - 1 );
	BleCommandHistory[ idx ].result[ sizeof( BleCommandHistory[ idx ].result ) - 1 ] = 0;
	BleCommandSeq++;
	unlockHistory();
}

static void FormatBleCommandData( const BLE_COMMAND* command, char* out, size_t outSize )
{
	if ( out == nullptr || outSize == 0 )
	{
		return;
	}

	out[ 0 ] = 0;
	if ( command == nullptr || command->DataLen <= 0 )
	{
		return;
	}

	int bytes = snprintf( out, outSize, "[" );
	for ( int i = 0; i < command->DataLen && bytes > 0 && bytes < ( int )outSize - 2; i++ )
	{
		bytes += snprintf( out + bytes, outSize - bytes, "%u", command->Data[ i ] );
		if ( i + 1 < command->DataLen )
		{
			bytes += snprintf( out + bytes, outSize - bytes, "," );
		}
	}

	if ( bytes > 0 && bytes < ( int )outSize - 1 )
	{
		snprintf( out + bytes, outSize - bytes, "]" );
	}
	else
	{
		out[ outSize - 1 ] = 0;
	}
}

static void UpdateBleCommandResult( const char* sourceIp, const char* address, const char* data, const char* result )
{
	bool updated = false;
	lockHistory();
	for ( int i = BleCommandHistoryCount - 1; i >= 0; i-- )
	{
		uint8_t idx = ( BleCommandHistoryStart + i ) % HOMEY_HISTORY_MAX;
		if ( strcmp( BleCommandHistory[ idx ].sourceIp, ( sourceIp != nullptr ? sourceIp : "" ) ) != 0 )
		{
			continue;
		}
		if ( strcmp( BleCommandHistory[ idx ].address, ( address != nullptr ? address : "" ) ) != 0 )
		{
			continue;
		}
		if ( strcmp( BleCommandHistory[ idx ].data, ( data != nullptr ? data : "" ) ) != 0 )
		{
			continue;
		}

		BleCommandHistory[ idx ].atMs = millis();
		strncpy( BleCommandHistory[ idx ].result, ( result != nullptr ? result : "" ), sizeof( BleCommandHistory[ idx ].result ) - 1 );
		BleCommandHistory[ idx ].result[ sizeof( BleCommandHistory[ idx ].result ) - 1 ] = 0;
		BleCommandSeq++;
		updated = true;
		break;
	}
	unlockHistory();

	if ( !updated )
	{
		RecordBleCommandRequest( sourceIp, address, data, result );
	}
}

static void RecordUnknownType( uint8_t type, const char* mac, bool hasSubtype = false, uint8_t b0 = 0, uint8_t b1 = 0, uint8_t b2 = 0 )
{
	char safeMac[ 18 ];
	if ( IsValidMacAddress( mac ) )
	{
		strncpy( safeMac, mac, sizeof( safeMac ) - 1 );
		safeMac[ sizeof( safeMac ) - 1 ] = '\0';
	}
	else
	{
		strncpy( safeMac, "??:??:??:??:??:??", sizeof( safeMac ) - 1 );
		safeMac[ sizeof( safeMac ) - 1 ] = '\0';
	}

	for ( uint8_t i = 0; i < UnknownTypeCount; i++ )
	{
		if ( UnknownTypes[ i ].mac[ 0 ] != '\0' &&
			 strncmp( UnknownTypes[ i ].mac, safeMac, sizeof( UnknownTypes[ i ].mac ) ) == 0 )
		{
			// Update type info in case it changes and increment count
			UnknownTypes[ i ].type = type;
			UnknownTypes[ i ].hasSubtype = hasSubtype;
			UnknownTypes[ i ].subtypeB0 = b0;
			UnknownTypes[ i ].subtypeB1 = b1;
			UnknownTypes[ i ].subtypeB2 = b2;
			UnknownTypes[ i ].count++;
			return;
		}
	}

	if ( UnknownTypeCount < UNKNOWN_TYPE_MAX )
	{
		UnknownTypes[ UnknownTypeCount ].type = type;
		UnknownTypes[ UnknownTypeCount ].hasSubtype = hasSubtype;
		UnknownTypes[ UnknownTypeCount ].subtypeB0 = b0;
		UnknownTypes[ UnknownTypeCount ].subtypeB1 = b1;
		UnknownTypes[ UnknownTypeCount ].subtypeB2 = b2;
		strncpy( UnknownTypes[ UnknownTypeCount ].mac, safeMac, sizeof( UnknownTypes[ UnknownTypeCount ].mac ) - 1 );
		UnknownTypes[ UnknownTypeCount ].mac[ sizeof( UnknownTypes[ UnknownTypeCount ].mac ) - 1 ] = '\0';
		UnknownTypes[ UnknownTypeCount ].count = 1;
		UnknownTypeCount++;
	}
}

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

static void RecordBleRateHistory( int32_t advertsPerMinute, int32_t matchesPerMinute, int32_t actualUpdatesPerMinute )
{
	if ( BleRateHistoryCount < FREE_HEAP_HISTORY_MAX )
	{
		const uint16_t idx = ( BleRateHistoryStart + BleRateHistoryCount ) % FREE_HEAP_HISTORY_MAX;
		AdvertsPerMinuteHistory[ idx ] = advertsPerMinute;
		MatchesPerMinuteHistory[ idx ] = matchesPerMinute;
		ActualUpdatesPerMinuteHistory[ idx ] = actualUpdatesPerMinute;
		BleRateHistoryCount++;
	}
	else
	{
		AdvertsPerMinuteHistory[ BleRateHistoryStart ] = advertsPerMinute;
		MatchesPerMinuteHistory[ BleRateHistoryStart ] = matchesPerMinute;
		ActualUpdatesPerMinuteHistory[ BleRateHistoryStart ] = actualUpdatesPerMinute;
		BleRateHistoryStart = ( BleRateHistoryStart + 1 ) % FREE_HEAP_HISTORY_MAX;
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

static void SendDiscoveryAnnouncement( const IPAddress* targetIp = nullptr, uint16_t targetPort = 0 )
{
	char message[ 64 ];
	int length = snprintf( message, sizeof( message ), "SwitchBot BLE Hub! %s", macAddress );
	if ( length <= 0 )
	{
		return;
	}

	if ( targetIp != nullptr && targetPort != 0 )
	{
		size_t sent = udp.writeTo( ( const uint8_t* )message, ( size_t )length, *targetIp, targetPort );
		Serial.printf( "UDP unicast discovery reply to %s:%u (%u bytes)\n", targetIp->toString().c_str(), targetPort, ( unsigned int )sent );
	}

	size_t multicastSent = udp.writeTo( ( const uint8_t* )message, ( size_t )length, DISCOVERY_MULTICAST_IP, DISCOVERY_PORT );
	Serial.printf( "UDP multicast discovery announcement to %s:%u (%u bytes)\n", DISCOVERY_MULTICAST_IP.toString().c_str(), DISCOVERY_PORT, ( unsigned int )multicastSent );
}

static void HandleDiscoveryPacket( AsyncUDPPacket& packet, const char* listenerName )
{
	String packetText;
	packetText.reserve( packet.length() + 1 );
	for ( size_t i = 0; i < packet.length(); i++ )
	{
		char c = ( char )packet.data()[ i ];
		if ( c == '\0' )
		{
			break;
		}
		packetText += c;
	}
	packetText.trim();

	String packetLower = packetText;
	packetLower.toLowerCase();
	if ( ( packetText == "Are you there SwitchBot?" ) ||
		 ( packetText == "Are you there SwitchBot" ) ||
		 ( packetLower.indexOf( "are you there switchbot" ) >= 0 ) )
	{
		Serial.printf( "Received discovery on %s: '%s' from %s:%u\n",
			listenerName,
			packetText.c_str(),
			packet.remoteIP().toString().c_str(),
			packet.remotePort() );
		IPAddress remoteIp = packet.remoteIP();
		uint16_t remotePort = packet.remotePort();
		SendDiscoveryAnnouncement( &remoteIp, remotePort );
		sendBroadcast = millis();
	}
	else
	{
		Serial.printf( "Received other UDP packet on %s from %s:%u len=%u text='%s'\n",
			listenerName,
			packet.remoteIP().toString().c_str(),
			packet.remotePort(),
			( unsigned int )packet.length(),
			packetText.c_str() );
	}
}

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
			std::string deviceAddress = advertisedDevice->getAddress().toString();
			const char* deviceAddressCStr = deviceAddress.c_str();
			const std::string& serviceData = advertisedDevice->getServiceData();
			if ( serviceData.length() == 0 )
			{
				NumMatchedEmptyPayload++;
				return;
			}
			const uint8_t* serviceDataBuf = ( const uint8_t* )serviceData.data();
			NumMatchedServiceData++;
			bool dataUpdated = false;
			bool failedValidation = false;
			bool unknownType = false;
			if ( BLE_Devices.AddDevice( deviceAddressCStr, advertisedDevice->getRSSI(), advertisedDevice->getAddress().getType(), ( uint8_t* )serviceData.data(), serviceData.length(), ( uint8_t* )advertisedDevice->getManufacturerData().data(), advertisedDevice->getManufacturerData().length(), &dataUpdated, &failedValidation, &unknownType ) )
			{
				// Serial.printf( "Updated device: %s\n", advertisedDevice->getAddress().toString().c_str() );
				NumUpdates++;
				if ( dataUpdated )
				{
					NumActualDataUpdates++;
				}
				pendingSSEUpdate = true;
			}
			else
			{
				if ( failedValidation && unknownType )
				{
					if ( serviceDataBuf[ 0 ] == 0 && serviceData.length() >= 7 )
					{
						RecordUnknownType( serviceDataBuf[ 0 ], deviceAddressCStr, true, serviceDataBuf[ 4 ], serviceDataBuf[ 5 ], serviceDataBuf[ 6 ] );
					}
					else
					{
						RecordUnknownType( serviceDataBuf[ 0 ], deviceAddressCStr );
					}
					NumMatchedRejected++;
				}
				else if ( failedValidation )
				{
					NumBadDataRejected++;
				}
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
	historyMutex = xSemaphoreCreateMutex();
	if ( historyMutex == nullptr )
	{
		Serial.println( "Failed to create history mutex" );
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

	server.on( "/api/v1/stats/page", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		request->send( 200, "text/html", STATS_HTML );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/homey/page", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		request->send( 200, "text/html", HOMEY_MONITOR_HTML );
		digitalWrite( led, 0 );
	} );

	sendBroadcast = millis();
	nextStatsSample = millis();
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
		    if ( request->method() != HTTP_POST )
		    {
			    return;
		    }

		    digitalWrite( led, 1 );

		    // Body can arrive in multiple chunks. Buffer it and parse only once complete.
		    if ( index == 0 )
		    {
			    if ( request->_tempObject != nullptr )
			    {
				    delete ( String* )request->_tempObject;
				    request->_tempObject = nullptr;
			    }
			    String* body = new String();
			    if ( body != nullptr )
			    {
				    body->reserve( total );
				    request->_tempObject = body;
			    }
		    }

		    if ( request->_tempObject == nullptr )
		    {
			    request->send( 500, "text/plain", "Internal Error" );
			    digitalWrite( led, 0 );
			    return;
		    }

		    String* body = ( String* )request->_tempObject;
		    for ( size_t i = 0; i < len; i++ )
		    {
			    *body += ( char )data[ i ];
		    }

		    if ( ( index + len ) < total )
		    {
			    digitalWrite( led, 0 );
			    return;
		    }

		    String payload = *body;
		    delete body;
		    request->_tempObject = nullptr;

		    const uint8_t* payloadData = ( const uint8_t* )payload.c_str();
		    size_t payloadLen = payload.length();

		    if ( request->url() == "/api/v1/callback/add" )
		    {
			    Serial.println( "Received request for /api/v1/callback/add" );
			    String uri;
			    if ( TryGetJsonStringField( payloadData, payloadLen, "uri", uri ) && uri.length() > 0 )
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
			    if ( TryGetJsonStringField( payloadData, payloadLen, "uri", uri ) && uri.length() > 0 )
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
				String sourceIP = IPAddress( request->client()->getRemoteAddress() ).toString();
			    String clientAddress;
			    String dataToWrite;
			    uint8_t commandData[ sizeof( BLE_COMMAND::Data ) ];
			    uint8_t commandDataLen = 0;
			    bool hasAddress = TryGetJsonStringField( payloadData, payloadLen, "address", clientAddress );
			    bool hasData = TryGetJsonByteArrayField( payloadData, payloadLen, "data", dataToWrite );
			    bool parsedData = hasData && ParseByteListString( dataToWrite, commandData, sizeof( commandData ), commandDataLen );
			    bool hasNonEmptyAddress = ( clientAddress.length() > 0 );

			    if ( hasAddress && hasData && parsedData && hasNonEmptyAddress )
			    {
				    // Check that we have seen that device
				    int deviceIdx = BLE_Devices.FindDevice( clientAddress.c_str() );
				    if ( deviceIdx >= 0 )
				    {
					    Serial.printf( "Received request to write device %s with %s (%u) from %s\n", clientAddress.c_str(), dataToWrite.c_str(), ( unsigned int )dataToWrite.length(), sourceIP.c_str() );

					    if ( BLECommandQ.Find( clientAddress.c_str(), commandData, commandDataLen ) )
					    {
						    // Same command already queued
						    request->send( 200, "text/plain", "OK" );
						    Serial.println( "Command already in the Q" );
							RecordBleCommandRequest( sourceIP.c_str(), clientAddress.c_str(), dataToWrite.c_str(), "already-queued" );
					    }
					    else if ( BLECommandQ.Push( clientAddress.c_str(), commandData, commandDataLen, sourceIP.c_str() ) )
					    {
						    request->send( 200, "text/plain", "OK" );
							RecordBleCommandRequest( sourceIP.c_str(), clientAddress.c_str(), dataToWrite.c_str(), "queued" );
					    }
					    else
					    {
						    String out = "{\"message\":\"Too Many Requests\",\"error\":\"QueueFull\",\"address\":\"" + JsonEscape( clientAddress.c_str() ) + "\"}";
						    request->send( 429, "application/json", out );
						    Serial.println( "I have too much in my command Q" );
							RecordBleCommandRequest( sourceIP.c_str(), clientAddress.c_str(), dataToWrite.c_str(), "queue-full" );
					    }
				    }
				    else
				    {
					    String out = "{\"message\":\"Unknown device\",\"error\":\"UnknownDevice\",\"address\":\"" + JsonEscape( clientAddress.c_str() ) + "\"}";
					    request->send( 422, "application/json", out );
					    Serial.printf( "Received request to write device %s but I have not seen that device)\n", clientAddress.c_str() );
							RecordBleCommandRequest( sourceIP.c_str(), clientAddress.c_str(), dataToWrite.c_str(), "unknown-device" );
				    }
			    }
			    else
			    {
				    String details = "";
				    if ( !hasAddress )
				    {
					    details += "missing-address";
				    }
				    else if ( !hasNonEmptyAddress )
				    {
					    details += "empty-address";
				    }

				    if ( !hasData )
				    {
					    if ( details.length() > 0 )
					    {
						    details += ",";
					    }
					    details += "missing-data";
				    }
				    else if ( !parsedData )
				    {
					    if ( details.length() > 0 )
					    {
						    details += ",";
					    }
					    details += "invalid-data";
				    }

				    String out = "{\"message\":\"Bad Request\",\"error\":\"InvalidPayload\",\"details\":\"" + JsonEscape( details.c_str() ) + "\",\"expected\":{\"address\":\"AA:BB:CC:DD:EE:FF\",\"data\":[87,15,71,1,5,0,255,85]}}";
				    request->send( 400, "application/json", out );
							RecordBleCommandRequest( sourceIP.c_str(), clientAddress.c_str(), dataToWrite.c_str(), "bad-request" );
			    }
		    }

		    digitalWrite( led, 0 );
	    } );

	server.on( "/api/v1/devices", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		Serial.println( "Received request for devices" );

		// Decide whether the caller wants raw JSON (e.g. API clients that send
		// Accept: application/json, or no Accept header at all) or a pretty HTML
		// page (browser). Browsers always include text/html in their Accept header.
		bool wantJson = true;
		if ( request->hasHeader( "Accept" ) )
		{
			String accept = request->getHeader( "Accept" )->value();
			// Only serve the HTML page if the caller explicitly wants text/html
			// and has NOT asked for application/json
			wantJson = ( accept.indexOf( "text/html" ) < 0 ||
						 accept.indexOf( "application/json" ) >= 0 );
		}

		if ( wantJson )
		{
			char* buf = ( char* )malloc( 16384 );
			if ( buf )
			{
				BLE_Devices.AllToJson( buf, 16384, false, macAddress );
				Serial.println( buf );
				String json( buf );
				free( buf );
				request->send( 200, "application/json", json );
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
		out += "\"intervalMs\":" + String( STATS_SAMPLE_MS );
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

	server.on( "/api/v1/stats/adverts-history", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		String out;
		out.reserve( 14000 );
		out = "{";
		out += "\"intervalMs\":" + String( STATS_SAMPLE_MS );
		out += ",\"maxPoints\":" + String( FREE_HEAP_HISTORY_MAX );
		out += ",\"count\":" + String( BleRateHistoryCount );
		out += ",\"values\":[";
		for ( uint16_t i = 0; i < BleRateHistoryCount; i++ )
		{
			const uint16_t idx = ( BleRateHistoryStart + i ) % FREE_HEAP_HISTORY_MAX;
			out += String( AdvertsPerMinuteHistory[ idx ] );
			if ( i + 1 < BleRateHistoryCount )
			{
				out += ",";
			}
		}
		out += "]}";

		request->send( 200, "application/json", out );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/stats/matches-history", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		String out;
		out.reserve( 14000 );
		out = "{";
		out += "\"intervalMs\":" + String( STATS_SAMPLE_MS );
		out += ",\"maxPoints\":" + String( FREE_HEAP_HISTORY_MAX );
		out += ",\"count\":" + String( BleRateHistoryCount );
		out += ",\"values\":[";
		for ( uint16_t i = 0; i < BleRateHistoryCount; i++ )
		{
			const uint16_t idx = ( BleRateHistoryStart + i ) % FREE_HEAP_HISTORY_MAX;
			out += String( MatchesPerMinuteHistory[ idx ] );
			if ( i + 1 < BleRateHistoryCount )
			{
				out += ",";
			}
		}
		out += "]}";

		request->send( 200, "application/json", out );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/stats/actual-updates-history", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		String out;
		out.reserve( 14000 );
		out = "{";
		out += "\"intervalMs\":" + String( STATS_SAMPLE_MS );
		out += ",\"maxPoints\":" + String( FREE_HEAP_HISTORY_MAX );
		out += ",\"count\":" + String( BleRateHistoryCount );
		out += ",\"values\":[";
		for ( uint16_t i = 0; i < BleRateHistoryCount; i++ )
		{
			const uint16_t idx = ( BleRateHistoryStart + i ) % FREE_HEAP_HISTORY_MAX;
			out += String( ActualUpdatesPerMinuteHistory[ idx ] );
			if ( i + 1 < BleRateHistoryCount )
			{
				out += ",";
			}
		}
		out += "]}";

		request->send( 200, "application/json", out );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/stats/unknown-types/clear", HTTP_POST, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		lockHistory();
		UnknownTypeCount = 0;
		for ( uint8_t i = 0; i < UNKNOWN_TYPE_MAX; i++ )
		{
			UnknownTypes[ i ].type = 0;
			UnknownTypes[ i ].subtypeB0 = 0;
			UnknownTypes[ i ].subtypeB1 = 0;
			UnknownTypes[ i ].subtypeB2 = 0;
			UnknownTypes[ i ].hasSubtype = false;
			UnknownTypes[ i ].mac[ 0 ] = '\0';
			UnknownTypes[ i ].count = 0;
		}
		unlockHistory();

		request->send( 200, "application/json", "{\"ok\":true,\"message\":\"Unknown devices cleared\"}" );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/homey/monitor", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		String out;
		out.reserve( 6000 );
		out = "{";

		out += "\"registered\":[";
		char uriBuf[ 256 ];
		uint8_t callbackIndex = 0;
		bool callbackAdded = false;
		while ( true )
		{
			lockCallbacks();
			bool gotCallback = OurCallbacks.Get( callbackIndex, uriBuf, sizeof( uriBuf ) );
			unlockCallbacks();
			if ( !gotCallback )
			{
				break;
			}

			char ipBuf[ 64 ] = {0};
			ExtractIpFromUri( uriBuf, ipBuf, sizeof( ipBuf ) );

			if ( callbackAdded ) out += ",";
			out += "{\"uri\":\"" + JsonEscape( uriBuf ) + "\",\"ip\":\"" + JsonEscape( ipBuf ) + "\"}";
			callbackAdded = true;
			callbackIndex++;
		}
		out += "],";

		out += "\"pushUpdates\":[";
		lockHistory();
		for ( int i = PushUpdateHistoryCount - 1; i >= 0; i-- )
		{
			uint8_t idx = ( PushUpdateHistoryStart + i ) % HOMEY_HISTORY_MAX;
			if ( i != PushUpdateHistoryCount - 1 ) out += ",";
			out += "{\"atMs\":" + String( PushUpdateHistory[ idx ].atMs );
			out += ",\"payload\":\"" + JsonEscape( PushUpdateHistory[ idx ].payload ) + "\"";
			out += ",\"ip\":\"" + JsonEscape( PushUpdateHistory[ idx ].ip ) + "\"";
			out += ",\"bytes\":" + String( PushUpdateHistory[ idx ].bytes );
			out += ",\"httpCode\":" + String( PushUpdateHistory[ idx ].httpCode ) + "}";
		}
		unlockHistory();
		out += "],";

		out += "\"bleCommands\":[";
		lockHistory();
		for ( int i = BleCommandHistoryCount - 1; i >= 0; i-- )
		{
			uint8_t idx = ( BleCommandHistoryStart + i ) % HOMEY_HISTORY_MAX;
			if ( i != BleCommandHistoryCount - 1 ) out += ",";
			out += "{\"atMs\":" + String( BleCommandHistory[ idx ].atMs );
			out += ",\"sourceIp\":\"" + JsonEscape( BleCommandHistory[ idx ].sourceIp ) + "\"";
			out += ",\"address\":\"" + JsonEscape( BleCommandHistory[ idx ].address ) + "\"";
			out += ",\"data\":\"" + JsonEscape( BleCommandHistory[ idx ].data ) + "\"";
			out += ",\"result\":\"" + JsonEscape( BleCommandHistory[ idx ].result ) + "\"}";
		}
		unlockHistory();
		out += "],";

		uint32_t nowMs = millis();
		out += "\"uptimeMs\":" + String( nowMs );
		out += ",\"nextUpdateAtMs\":" + String( LastStatsAt + STATS_SAMPLE_MS );
		out += ",\"bleCommandSeq\":" + String( BleCommandSeq );
		out += ",\"pushUpdateSeq\":" + String( PushUpdateSeq );

		out += "}";

		request->send( 200, "application/json", out );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/homey/pushseq", HTTP_GET, []( AsyncWebServerRequest* request ) {
		request->send( 200, "application/json", "{\"seq\":" + String( PushUpdateSeq ) + "}" );
	} );

	server.on( "/api/v1/homey/bleseq", HTTP_GET, []( AsyncWebServerRequest* request ) {
		request->send( 200, "application/json", "{\"seq\":" + String( BleCommandSeq ) + "}" );
	} );

		server.on( "/api/v1/stats", HTTP_GET, []( AsyncWebServerRequest* request ) {
			digitalWrite( led, 1 );

			static char statsBuf[ 10240 ];
			int pos = 0;
			int rem = ( int )sizeof( statsBuf );
#define STATS_APPEND( ... ) \
			do { int _n = snprintf( statsBuf + pos, rem, __VA_ARGS__ ); if ( _n > 0 ) { pos += _n; rem -= _n; } } while ( 0 )

			STATS_APPEND( "{" );
			STATS_APPEND( "\"uptimeMs\":%lu", ( unsigned long )millis() );
			STATS_APPEND( ",\"advertsSeenPerMinute\":%ld", ( long )LastAdvertsSeenPerMinute );
			STATS_APPEND( ",\"matchedServiceDataPerMinute\":%ld", ( long )LastMatchedServiceDataPerMinute );
			STATS_APPEND( ",\"matchedEmptyPayloadPerMinute\":%ld", ( long )LastMatchedEmptyPayloadPerMinute );
			STATS_APPEND( ",\"matchedRejectedPerMinute\":%ld", ( long )LastMatchedRejectedPerMinute );
			STATS_APPEND( ",\"badDataRejectedPerMinute\":%ld", ( long )LastBadDataRejectedPerMinute );
			STATS_APPEND( ",\"updatesPerMinute\":%ld", ( long )LastUpdatesPerMinute );
			STATS_APPEND( ",\"actualDataUpdatesPerMinute\":%ld", ( long )LastActualDataUpdatesPerMinute );
			STATS_APPEND( ",\"currentMinuteUpdates\":%ld", ( long )NumUpdates );
			STATS_APPEND( ",\"noUpdateMinutes\":%lu", ( unsigned long )NumUpdatesAt0 );
			STATS_APPEND( ",\"freeHeap\":%lu", ( unsigned long )LastFreeHeap );
			STATS_APPEND( ",\"largestHeapBlock\":%lu", ( unsigned long )LastLargestHeapBlock );
			STATS_APPEND( ",\"lastStatsAtMs\":%lu", ( unsigned long )LastStatsAt );
			STATS_APPEND( ",\"statsIntervalMs\":%lu", ( unsigned long )STATS_SAMPLE_MS );
			STATS_APPEND( ",\"cpuUsage\":%u", ( unsigned )LastCpuUsagePercent );
			STATS_APPEND( ",\"registeredDevices\":%u", ( unsigned )BLE_Devices.GetNumberOfDevices() );
			STATS_APPEND( ",\"unknownTypes\":[" );
			for ( uint8_t i = 0; i < UnknownTypeCount; i++ )
			{
				STATS_APPEND( "%s{\"type\":%u", i > 0 ? "," : "", ( unsigned )UnknownTypes[ i ].type );
				if ( UnknownTypes[ i ].hasSubtype )
				{
					STATS_APPEND( ",\"subtype\":\"%02X%02X%02X\"", UnknownTypes[ i ].subtypeB0, UnknownTypes[ i ].subtypeB1, UnknownTypes[ i ].subtypeB2 );
				}
				if ( UnknownTypes[ i ].mac[ 0 ] != '\0' )
				{
					STATS_APPEND( ",\"mac\":\"%s\"", UnknownTypes[ i ].mac );
				}
				STATS_APPEND( ",\"count\":%lu}", ( unsigned long )UnknownTypes[ i ].count );
			}
			STATS_APPEND( "]}" );
#undef STATS_APPEND

			request->send( 200, "application/json", statsBuf );

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
	pBLEScan->setMaxResults( 0 ); // disable result storage - callbacks fire for all devices; use direct address connect for BLE commands
	pBLEScan->start( 0, false, true );

	Serial.println( "Application started" );

	uint8_t mac[ 6 ];
	WiFi.macAddress( mac );
	sprintf( macAddress, "%0.2x:%0.2x:%0.2x:%0.2x:%0.2x:%0.2x", mac[ 5 ], mac[ 4 ], mac[ 3 ], mac[ 2 ], mac[ 1 ], mac[ 0 ] );

	LastFreeHeap = esp_get_free_heap_size();
	LastLargestHeapBlock = heap_caps_get_largest_free_block( MALLOC_CAP_8BIT );
	LastStatsAt = millis();
	RecordFreeHeapHistory( LastFreeHeap );
	RecordBleRateHistory( LastAdvertsSeenPerMinute, LastUpdatesPerMinute, LastActualDataUpdatesPerMinute );
	SampleCpuUsage();

	if ( udp.listenMulticast( IPAddress( 239, 1, 2, 3 ), 1234 ) )
	{
		Serial.print( "UDP Listening on IP: " );
		Serial.println( WiFi.localIP() );
		udp.onPacket( []( AsyncUDPPacket packet ) {
			Serial.println( "Received multicast packet" );
			HandleDiscoveryPacket( packet, "multicast" ); } );
	}
	else
	{
		Serial.printf( "Failed to join multicast 39.1.2.3:1234\n" );
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
			SendDiscoveryAnnouncement();
			sendBroadcast = millis() + 15000;
		}

		if ( millis() >= nextStatsSample )
		{
			nextStatsSample = millis() + STATS_SAMPLE_MS;

			LastAdvertsSeenPerMinute = NumAdvertsSeen * ( int32_t )STATS_PER_MINUTE_SCALE;
			LastMatchedServiceDataPerMinute = NumMatchedServiceData * ( int32_t )STATS_PER_MINUTE_SCALE;
			LastMatchedEmptyPayloadPerMinute = NumMatchedEmptyPayload * ( int32_t )STATS_PER_MINUTE_SCALE;
			LastMatchedRejectedPerMinute = NumMatchedRejected * ( int32_t )STATS_PER_MINUTE_SCALE;
			LastBadDataRejectedPerMinute = NumBadDataRejected * ( int32_t )STATS_PER_MINUTE_SCALE;
			LastUpdatesPerMinute = NumUpdates * ( int32_t )STATS_PER_MINUTE_SCALE;
			LastActualDataUpdatesPerMinute = NumActualDataUpdates * ( int32_t )STATS_PER_MINUTE_SCALE;

			if ( LastUpdatesPerMinute == 0 )
			{
				NumZeroUpdateIntervals++;
				NumUpdatesAt0 = NumZeroUpdateIntervals / ( int32_t )STATS_PER_MINUTE_SCALE;
			}
			else
			{
				NumZeroUpdateIntervals = 0;
				NumUpdatesAt0 = 0;
			}

			if (NumUpdatesAt0 > 3)
			{
				Serial.println( "No BLE updates for 3 minutes, rebooting" );
				RebootRequired = true;
			}

			Serial.printf( "BLE adverts %i/min, UUID matches %i/min, empty payload %i/min, rejected %i/min, bad data %i/min, updates %i/min\n", LastAdvertsSeenPerMinute, LastMatchedServiceDataPerMinute, LastMatchedEmptyPayloadPerMinute, LastMatchedRejectedPerMinute, LastBadDataRejectedPerMinute, LastUpdatesPerMinute );
			NumAdvertsSeen = 0;
			NumMatchedServiceData = 0;
			NumMatchedEmptyPayload = 0;
			NumMatchedRejected = 0;
			NumBadDataRejected = 0;
			NumUpdates = 0;
			NumActualDataUpdates = 0;

			// Report heap available
			uint32_t freeHeap = esp_get_free_heap_size();
			uint32_t largestHeapBlock = heap_caps_get_largest_free_block( MALLOC_CAP_8BIT );
			LastFreeHeap = freeHeap;
			LastLargestHeapBlock = largestHeapBlock;
			LastStatsAt = millis();
			pendingSSEStats = true;
			RecordFreeHeapHistory( freeHeap );
			RecordBleRateHistory( LastAdvertsSeenPerMinute, LastUpdatesPerMinute, LastActualDataUpdatesPerMinute );
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
			int httpCode = SendDeviceChange( addresBuf, deviceBuf, bytes );
			RecordPushUpdate( addresBuf, deviceBuf, bytes, httpCode );
			if ( httpCode == -1 )
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
	char commandDataText[ 80 ];
	FormatBleCommandData( BLECommand, commandDataText, sizeof( commandDataText ) );
	const char* finalResult = "error-device-not-found";

	Serial.printf( "Sending command to BLE device: %s\n", BLECommand->Address );

	// Look up the BLE address type stored when the device was first seen during scanning.
	// This avoids a separate scan just to resolve the address type.
	uint8_t addrType = BLE_Devices.GetDeviceAddressType( BLECommand->Address );
	NimBLEAddress bleAddress( std::string( BLECommand->Address ), addrType );
	Serial.printf( "Using stored address type %u for %s\n", addrType, BLECommand->Address );

	pBLEScan->stop();
	delay( 100 );

	{
		NimBLEClient* pBLEClient = NimBLEDevice::createClient();
		pBLEClient->setConnectTimeout( 5 * 1000 );
		pBLEClient->setConnectionParams( 32, 160, 0, 500 );

		bool complete = false;
		int retries = 5;

		while ( !complete && ( retries-- > 0 ) )
		{
			if ( pBLEClient->connect( bleAddress ) )
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
						finalResult = "completed";

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
									finalResult = "error-notify-subscribe";
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
								finalResult = "completed-notify";

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
							else
							{
								finalResult = "error-notify-timeout";
							}

							Serial.println( "Unsubscribe from notification" );
							rn->unsubscribe();
						}
						complete = true;
					}
					else
					{
						Serial.println( "Failed to get characteristic" );
						finalResult = "error-characteristic";
					}
				}
				else
				{
					Serial.println( "Failed to get service" );
					finalResult = "error-service";
				}

				pBLEClient->disconnect();
				Serial.println( "Disconnected device" );
			}
			else
			{
				Serial.println( "Failed to connected to device" );
				finalResult = "error-connect";
			}
		}

		NimBLEDevice::deleteClient( pBLEClient );
	}

	UpdateBleCommandResult( BLECommand->ReplyTo, BLECommand->Address, commandDataText, finalResult );

	Serial.println( "Restarting BLE scan" );
	pBLEScan->start( 0, true, false );
	BLESending = millis() + 1000;
}
