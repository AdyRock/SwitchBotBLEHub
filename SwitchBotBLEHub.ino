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
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#include "BLE_Device.h"
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

const char* version = "Hello! SwitchBot BLE Hub V2.15";

struct POST_BODY_BUFFER
{
	char* data;
	size_t len;
	size_t cap;
};

static bool IsJsonWhitespace( char c )
{
	return ( c == ' ' || c == '\t' || c == '\r' || c == '\n' );
}

static const char* SkipJsonWhitespace( const char* ptr, const char* end )
{
	while ( ptr < end && IsJsonWhitespace( *ptr ) )
	{
		ptr++;
	}
	return ptr;
}

static void TrimAsciiInPlace( char* text )
{
	if ( text == nullptr )
	{
		return;
	}

	char* start = text;
	while ( *start != '\0' && IsJsonWhitespace( *start ) )
	{
		start++;
	}

	char* end = start + strlen( start );
	while ( end > start && IsJsonWhitespace( end[ -1 ] ) )
	{
		end--;
	}
	*end = '\0';

	if ( start != text )
	{
		memmove( text, start, ( size_t )( end - start ) + 1 );
	}
}

static void FormatIpAddress( const IPAddress& ip, char* outBuf, size_t outSize )
{
	if ( outBuf == nullptr || outSize == 0 )
	{
		return;
	}

	snprintf( outBuf, outSize, "%u.%u.%u.%u", ip[ 0 ], ip[ 1 ], ip[ 2 ], ip[ 3 ] );
}

static inline void IncrementVolatileU32( volatile uint32_t& value )
{
	value = value + 1;
}

static bool ParseMacAddressToNimble( const char* mac, uint8_t addrType, NimBLEAddress& outAddress )
{
	if ( mac == nullptr )
	{
		return false;
	}

	uint8_t bytes[ 6 ];
	const char* ptr = mac;
	for ( int i = 0; i < 6; i++ )
	{
		int hi = HexNibble( *ptr++ );
		int lo = HexNibble( *ptr++ );
		if ( hi < 0 || lo < 0 )
		{
			return false;
		}
		bytes[ i ] = ( uint8_t )( ( hi << 4 ) | lo );
		if ( i < 5 )
		{
			if ( *ptr != ':' )
			{
				return false;
			}
			ptr++;
		}
	}

	if ( *ptr != '\0' )
	{
		return false;
	}

	uint64_t addressValue =
	    ( ( uint64_t )bytes[ 0 ] << 40 ) |
	    ( ( uint64_t )bytes[ 1 ] << 32 ) |
	    ( ( uint64_t )bytes[ 2 ] << 24 ) |
	    ( ( uint64_t )bytes[ 3 ] << 16 ) |
	    ( ( uint64_t )bytes[ 4 ] << 8 ) |
	    ( ( uint64_t )bytes[ 5 ] );
	outAddress = NimBLEAddress( addressValue, addrType );
	return true;
}

static bool TryFindJsonFieldValue( const uint8_t* data, size_t len, const char* key, const char** valueStart, const char** valueEnd )
{
	if ( data == nullptr || len == 0 || key == nullptr || key[ 0 ] == '\0' ||
	     valueStart == nullptr || valueEnd == nullptr )
	{
		return false;
	}

	const char* body = ( const char* )data;
	const char* end = body + len;
	size_t keyLen = strlen( key );

	for ( const char* ptr = body; ptr < end; ptr++ )
	{
		if ( *ptr != '"' )
		{
			continue;
		}

		if ( ( size_t )( end - ptr ) < keyLen + 2 )
		{
			break;
		}

		if ( memcmp( ptr + 1, key, keyLen ) != 0 || ptr[ keyLen + 1 ] != '"' )
		{
			continue;
		}

		const char* colon = ptr + keyLen + 2;
		colon = SkipJsonWhitespace( colon, end );
		if ( colon >= end || *colon != ':' )
		{
			continue;
		}

		colon++;
		colon = SkipJsonWhitespace( colon, end );
		if ( colon >= end )
		{
			return false;
		}

		*valueStart = colon;
		*valueEnd = end;
		return true;
	}

	return false;
}

static bool CopyRequestArgToBuf( AsyncWebServerRequest* request, const char* name, char* outBuf, size_t outSize, bool trimValue = true, bool lowercase = false )
{
	if ( request == nullptr || name == nullptr || outBuf == nullptr || outSize < 2 || !request->hasArg( name ) )
	{
		return false;
	}

	String argValue = request->arg( name );
	if ( trimValue )
	{
		argValue.trim();
	}
	if ( lowercase )
	{
		argValue.toLowerCase();
	}
	if ( argValue.length() == 0 )
	{
		outBuf[ 0 ] = '\0';
		return false;
	}

	strncpy( outBuf, argValue.c_str(), outSize - 1 );
	outBuf[ outSize - 1 ] = '\0';
	return true;
}

// Extract JSON string field into a fixed char buffer (no String allocation)
static bool TryGetJsonStringFieldIntoBuf( const uint8_t* data, size_t len, const char* key, char* outBuf, size_t maxLen )
{
	if ( outBuf == nullptr || maxLen < 2 )
	{
		return false;
	}
	outBuf[ 0 ] = '\0';

	const char* valueStart = nullptr;
	const char* end = nullptr;
	if ( !TryFindJsonFieldValue( data, len, key, &valueStart, &end ) )
	{
		return false;
	}

	if ( valueStart >= end || *valueStart != '"' )
	{
		return false;
	}

	valueStart++;
	size_t outPos = 0;
	for ( const char* ptr = valueStart; ptr < end; ptr++ )
	{
		char c = *ptr;
		if ( c == '"' )
		{
			outBuf[ outPos ] = '\0';
			return ( outPos > 0 );
		}

		if ( c == '\\' )
		{
			ptr++;
			if ( ptr >= end )
			{
				return false;
			}

			switch ( *ptr )
			{
			case '"': c = '"'; break;
			case '\\': c = '\\'; break;
			case '/': c = '/'; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			default: c = *ptr; break;
			}
		}

		if ( outPos + 1 >= maxLen )
		{
			break;
		}
		outBuf[ outPos++ ] = c;
	}

	outBuf[ outPos ] = '\0';
	return false;
}

static int HexNibble( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return 10 + ( c - 'a' );
	if ( c >= 'A' && c <= 'F' ) return 10 + ( c - 'A' );
	return -1;
}

static void UrlDecodeInPlace( char* text )
{
	if ( text == nullptr )
	{
		return;
	}

	char* src = text;
	char* dst = text;
	while ( *src )
	{
		if ( *src == '+' )
		{
			*dst++ = ' ';
			src++;
			continue;
		}

		if ( *src == '%' && src[ 1 ] && src[ 2 ] )
		{
			int hi = HexNibble( src[ 1 ] );
			int lo = HexNibble( src[ 2 ] );
			if ( hi >= 0 && lo >= 0 )
			{
				*dst++ = ( char )( ( hi << 4 ) | lo );
				src += 3;
				continue;
			}
		}

		*dst++ = *src++;
	}
	*dst = '\0';
}

static bool TryExtractUriFromBody( const uint8_t* data, size_t len, char* outBuf, size_t maxLen )
{
	if ( outBuf == nullptr || maxLen < 2 || data == nullptr || len == 0 )
	{
		return false;
	}
	outBuf[ 0 ] = '\0';

	// Preferred format: JSON body {"uri":"..."}
	if ( TryGetJsonStringFieldIntoBuf( data, len, "uri", outBuf, maxLen ) )
	{
		return true;
	}

	size_t copyLen = 0;
	while ( copyLen < len && copyLen + 1 < maxLen && data[ copyLen ] != '\0' )
	{
		outBuf[ copyLen ] = ( char )data[ copyLen ];
		copyLen++;
	}
	outBuf[ copyLen ] = '\0';
	TrimAsciiInPlace( outBuf );
	if ( outBuf[ 0 ] == '\0' )
	{
		return false;
	}

	// Backward-compatible format: uri=http%3A%2F%2F...
	char* uriPos = strstr( outBuf, "uri=" );
	if ( uriPos != nullptr )
	{
		uriPos += 4;
		char* amp = strchr( uriPos, '&' );
		if ( amp != nullptr )
		{
			*amp = '\0';
		}
		TrimAsciiInPlace( uriPos );
		if ( uriPos[ 0 ] != '\0' )
		{
			if ( uriPos != outBuf )
			{
				memmove( outBuf, uriPos, strlen( uriPos ) + 1 );
			}
			UrlDecodeInPlace( outBuf );
			return outBuf[ 0 ] != '\0';
		}
	}

	// Plain-text fallback body: http://...
	UrlDecodeInPlace( outBuf );
	return outBuf[ 0 ] != '\0';
}

static bool ParseNumericByteToken( const char* token, size_t tokenLen, uint8_t& outByte )
{
	if ( token == nullptr || tokenLen == 0 )
	{
		return false;
	}

	while ( tokenLen > 0 && IsJsonWhitespace( *token ) )
	{
		token++;
		tokenLen--;
	}
	while ( tokenLen > 0 && IsJsonWhitespace( token[ tokenLen - 1 ] ) )
	{
		tokenLen--;
	}
	if ( tokenLen == 0 || tokenLen >= 32 )
	{
		return false;
	}

	char tokenBuf[ 32 ];
	memcpy( tokenBuf, token, tokenLen );
	tokenBuf[ tokenLen ] = '\0';

	const char* start = tokenBuf;
	char* end = nullptr;
	double number = strtod( start, &end );
	if ( end == start )
	{
		return false;
	}

	while ( *end == ' ' || *end == '\t' || *end == '\r' || *end == '\n' )
	{
		end++;
	}
	if ( *end != '\0' )
	{
		return false;
	}

	// Allow floating-point values that are effectively integers after transport/serialization.
	double rounded = floor( number + 0.5 );
	if ( fabs( number - rounded ) > 0.01 )
	{
		return false;
	}

	long byteValue = ( long )rounded;
	if ( byteValue < 0 || byteValue > 255 )
	{
		return false;
	}

	outByte = ( uint8_t )byteValue;
	return true;
}

static bool ParseByteListString( const char* value, uint8_t* outData, uint8_t maxLen, uint8_t& outLen )
{
	outLen = 0;
	if ( value == nullptr || outData == nullptr || maxLen == 0 )
	{
		return false;
	}

	size_t valueLen = strlen( value );
	if ( valueLen < 3 || value[ 0 ] != '[' || value[ valueLen - 1 ] != ']' )
	{
		return false;
	}

	const char* ptr = value + 1;
	const char* end = value + valueLen - 1;
	while ( ptr < end )
	{
		while ( ptr < end && ( IsJsonWhitespace( *ptr ) || *ptr == ',' ) )
		{
			ptr++;
		}

		if ( ptr >= end )
		{
			break;
		}

		const char* tokenStart = ptr;
		while ( ptr < end && *ptr != ',' && *ptr != ']' )
		{
			ptr++;
		}

		if ( tokenStart == ptr || outLen >= maxLen )
		{
			return false;
		}

		uint8_t byteValue = 0;
		if ( !ParseNumericByteToken( tokenStart, ( size_t )( ptr - tokenStart ), byteValue ) )
		{
			return false;
		}
		outData[ outLen++ ] = byteValue;
	}

	return ( outLen > 0 );
}

static bool TryParseJsonByteArrayField( const uint8_t* data, size_t len, const char* key, uint8_t* outData, uint8_t maxLen, uint8_t& outLen )
{
	outLen = 0;
	if ( data == nullptr || len == 0 || key == nullptr || key[ 0 ] == '\0' || outData == nullptr || maxLen == 0 )
	{
		return false;
	}

	const char* valueStart = nullptr;
	const char* end = nullptr;
	if ( !TryFindJsonFieldValue( data, len, key, &valueStart, &end ) )
	{
		return false;
	}

	if ( valueStart >= end )
	{
		return false;
	}

	if ( *valueStart == '"' )
	{
		char valueBuf[ 128 ];
		if ( !TryGetJsonStringFieldIntoBuf( data, len, key, valueBuf, sizeof( valueBuf ) ) )
		{
			return false;
		}
		TrimAsciiInPlace( valueBuf );
		if ( valueBuf[ 0 ] == '\0' )
		{
			return false;
		}
		if ( valueBuf[ 0 ] != '[' )
		{
			char wrapped[ sizeof( valueBuf ) ];
			if ( snprintf( wrapped, sizeof( wrapped ), "[%s]", valueBuf ) >= ( int )sizeof( wrapped ) )
			{
				return false;
			}
			return ParseByteListString( wrapped, outData, maxLen, outLen );
		}
		return ParseByteListString( valueBuf, outData, maxLen, outLen );
	}

	if ( *valueStart != '[' )
	{
		return false;
	}

	const char* listEnd = valueStart;
	while ( listEnd < end && *listEnd != ']' )
	{
		listEnd++;
	}
	if ( listEnd >= end )
	{
		return false;
	}

	char listBuf[ 128 ];
	size_t listLen = ( size_t )( listEnd - valueStart + 1 );
	if ( listLen >= sizeof( listBuf ) )
	{
		return false;
	}
	memcpy( listBuf, valueStart, listLen );
	listBuf[ listLen ] = '\0';
	return ParseByteListString( listBuf, outData, maxLen, outLen );
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
        <p class="ver">Version 2.15</p>
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

static const char OTA_UPDATE_HTML[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
	<head>
		<meta charset="UTF-8">
		<meta name="viewport" content="width=device-width, initial-scale=1">
		<title>SwitchBot BLE Hub OTA</title>
		<style>
			body { margin: 0; background: #1e1e1e; color: #d4d4d4; font-family: sans-serif; }
			.wrap { max-width: 760px; margin: 2rem auto; padding: 0 1rem; }
			.card { background: #252526; border: 1px solid #3c3c3c; border-radius: 10px; padding: 1.2rem 1.1rem; }
			h1 { margin: 0 0 .25rem; color: #9cdcfe; font-size: 1.35rem; }
			.hint { color: #a0a0a0; margin: 0 0 1rem; font-size: .95rem; }
			input[type=file] { display: none; }
			.file-row { margin-bottom: .6rem; }
			.file-btn { display: inline-block; background: #007acc; color: #fff; border: 0; border-radius: 6px; padding: .6rem .9rem; font-weight: 600; font-size: 1rem; line-height: 1.2; cursor: pointer; text-decoration: none; }
			.file-btn:hover { background: #005f9e; }
			.file-name { margin-top: .5rem; color: #d4d4d4; font-size: .92rem; word-break: break-all; white-space: normal; }
			input[type=submit] { background: #007acc; color: #fff; border: 0; border-radius: 6px; padding: .6rem .9rem; font-weight: 600; font-size: 1rem; line-height: 1.2; cursor: pointer; }
			input[type=submit]:hover { background: #005f9e; }
			input[type=submit]:disabled { background: #4e5a63; cursor: not-allowed; }
			.progress-wrap { margin-top: .8rem; display: none; }
			.progress-label { font-size: .9rem; color: #c8c8c8; margin-bottom: .3rem; }
			.progress-rail { width: 100%; height: 12px; background: #1b1b1b; border: 1px solid #3a3a3a; border-radius: 8px; overflow: hidden; }
			.progress-fill { width: 0%; height: 100%; background: linear-gradient(90deg, #007acc, #00a3ff); transition: width .12s linear; }
			.status { margin-top: .6rem; color: #d4d4d4; min-height: 1.2rem; font-size: .92rem; }
			.back { display: inline-block; margin-top: .9rem; color: #9cdcfe; text-decoration: none; }
		</style>
	</head>
	<body>
		<div class="wrap">
			<div class="card">
				<h1>Firmware Update</h1>
				<p class="hint">Upload firmware app image only (.ino.bin). Do not use merged images.</p>
				<form id="otaForm" method="POST" action="/ota?name=firmware" enctype="multipart/form-data">
					<div class="file-row">
						<label class="file-btn" for="firmwareFile">Choose Firmware File</label>
						<input id="firmwareFile" type="file" accept=".bin,.bin.gz" name="firmware" required>
						<div id="fileName" class="file-name">No file selected. Example: SwitchBotBLEHub.ino.bin</div>
					</div>
					<input id="uploadBtn" type="submit" value="Update Firmware">
				</form>
				<div id="progressWrap" class="progress-wrap">
					<div id="progressLabel" class="progress-label">Preparing upload...</div>
					<div class="progress-rail"><div id="progressFill" class="progress-fill"></div></div>
				</div>
				<div id="status" class="status"></div>
				<a class="back" href="/">Back to Home</a>
			</div>
		</div>
		<script>
			(function() {
				var form = document.getElementById('otaForm');
				var fileInput = document.getElementById('firmwareFile');
				var uploadBtn = document.getElementById('uploadBtn');
				var fileNameEl = document.getElementById('fileName');
				var progressWrap = document.getElementById('progressWrap');
				var progressFill = document.getElementById('progressFill');
				var progressLabel = document.getElementById('progressLabel');
				var statusEl = document.getElementById('status');

				function setProgress(percent, label) {
					var safe = Math.max(0, Math.min(100, percent || 0));
					progressFill.style.width = safe + '%';
					progressLabel.textContent = label || ('Uploading ' + safe + '%');
				}

				fileInput.addEventListener('change', function() {
					if (fileInput.files && fileInput.files.length > 0) {
						fileNameEl.textContent = fileInput.files[0].name;
					} else {
						fileNameEl.textContent = 'No file selected. Example: SwitchBotBLEHub.ino.bin';
					}
				});

				form.addEventListener('submit', function(ev) {
					ev.preventDefault();
					if (!fileInput.files || !fileInput.files.length) {
						statusEl.textContent = 'Select a firmware file first.';
						return;
					}

					var body = new FormData();
					body.append('firmware', fileInput.files[0]);

					uploadBtn.disabled = true;
					statusEl.textContent = 'Preparing...';
					progressWrap.style.display = 'block';
					setProgress(0, 'Stopping BLE scan...');

					function startUpload() {
						statusEl.textContent = 'Uploading...';
						setProgress(0, 'Starting upload...');
						var uploadComplete = false;
						var xhr = new XMLHttpRequest();
						xhr.open('POST', '/ota?name=firmware', true);
						xhr.upload.onprogress = function(e) {
							if (e.lengthComputable) {
								var pct = Math.round((e.loaded / e.total) * 100);
								setProgress(pct, 'Uploading ' + pct + '%');
							}
						};
						xhr.upload.onload = function() {
							uploadComplete = true;
							progressWrap.style.display = 'none';
							statusEl.textContent = 'Programming... Please wait while the device restarts.';
							setTimeout(function() {
								window.location.href = '/';
							}, 5000);
						};
						xhr.onload = function() {
							if (!uploadComplete) {
								uploadBtn.disabled = false;
								statusEl.textContent = 'Update failed (HTTP ' + xhr.status + ').';
								fetch('/api/v1/ota/cancel', {method: 'POST'}).catch(function(){});
							}
						};
						xhr.onerror = function() {
							if (!uploadComplete) {
								uploadBtn.disabled = false;
								statusEl.textContent = 'Upload failed due to network error.';
								fetch('/api/v1/ota/cancel', {method: 'POST'}).catch(function(){});
							}
						};
						xhr.send(body);
					}

					fetch('/api/v1/ota/prepare', {method: 'POST'})
						.then(function() { startUpload(); })
						.catch(function() { startUpload(); });
				});
			})();
		</script>
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
		<script>
			(function() {
				window.__statsBootstrapLoadedAt = Date.now();

				function getRetryCount() {
					try {
						var v = parseInt(sessionStorage.getItem("stats_bootstrap_retry_count") || "0", 10);
						return isNaN(v) ? 0 : v;
					} catch (e) {
						return 0;
					}
				}

				function setRetryCount(v) {
					try { sessionStorage.setItem("stats_bootstrap_retry_count", String(v)); } catch (e) {}
				}

				function retryPage(reason) {
					var retries = getRetryCount();
					if (retries >= 2) return;
					setRetryCount(retries + 1);
					var sep = window.location.href.indexOf("?") >= 0 ? "&" : "?";
					window.location.replace(window.location.href + sep + "_ts=" + Date.now() + "&r=" + encodeURIComponent(reason || "retry"));
				}

				window.addEventListener("error", function(ev) {
					var msg = String((ev && ev.message) || "");
					if (msg.indexOf("Unexpected token '<'") >= 0) {
						retryPage("syntax");
					}
				}, true);

				setTimeout(function() {
					if (typeof window.getStatsDebug === "function") {
						setRetryCount(0);
						return;
					}
					retryPage("missing");
				}, 1500);
			})();
		</script>
		<style>
			body { margin: 0; background: #1e1e1e; color: #d4d4d4; font-family: sans-serif; }
			.wrap { max-width: 980px; margin: 1.2rem auto; padding: 0 1rem; }
			.top { display: flex; align-items: center; justify-content: space-between; gap: 0.8rem; margin-bottom: 0.9rem; }
			.title { color: #9cdcfe; font-size: 1.45rem; font-weight: 700; }
			.home { text-decoration: none; background: #3c3c3c; color: #fff; border-radius: 6px; padding: 0.45rem 0.7rem; font-weight: 600; }
			.home:hover { background: #555; }
			.meta { color: #9aa0a6; font-size: 0.85rem; margin-bottom: 0.9rem; }
			.controls { display: flex; align-items: center; gap: 0.6rem; margin-bottom: 0.9rem; }
			.scan-btn { background: #2f8f5b; color: #fff; border: 1px solid #3ea96c; border-radius: 6px; padding: 0.38rem 0.7rem; font-size: 0.82rem; font-weight: 600; cursor: pointer; }
			.scan-btn:hover { background: #3ea96c; }
			.scan-btn:disabled { opacity: 0.6; cursor: not-allowed; }
			.scan-status { color: #9aa0a6; font-size: 0.82rem; }
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
			.raw-link { display: inline-block; padding: 0.2rem 0.45rem; border-radius: 4px; background: #2f8f5b; color: #fff; text-decoration: none; font-size: 0.75rem; font-weight: 600; }
			.raw-link:hover { background: #3ea96c; }
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
			<div class="controls">
				<button id="scanToggleBtn" class="scan-btn" type="button" onclick="toggleBleScan()">Start BLE Scan</button>
				<span id="scanStatus" class="scan-status">Scanner status: unknown</span>
			</div>
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
			window.__statsPageLoadedAt = Date.now();
			var lastStats = null;
			var statsClockOffsetMs = 0;
			var nextStatsServerMs = 0;
			var lastChartHistory = null;
			var chartConfig = null;
			var statsRefreshTimer = null;
			var statsFetchInFlight = false;
			var statsLastRequestStartedMs = 0;
			var statsErrorText = "";
			var statsErrorUntilMs = 0;
			var scanControlBusy = false;

			window.getStatsDebug = function() {
				return {
					location: String(window.location),
					loadedAt: window.__statsPageLoadedAt,
					now: Date.now(),
					statsFetchInFlight: statsFetchInFlight,
					statsLastRequestStartedMs: statsLastRequestStartedMs,
					statsErrorText: statsErrorText,
					statsErrorUntilMs: statsErrorUntilMs,
					hasLastStats: !!lastStats,
					nextStatsServerMs: nextStatsServerMs,
					meta: document.getElementById("meta") ? document.getElementById("meta").textContent : null
				};
			};

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

			function updateScanControls(s) {
				var btn = document.getElementById("scanToggleBtn");
				var status = document.getElementById("scanStatus");
				if (!btn || !status) return;

				var scanning = !!(s && s.bleScanning);
				var pausedByUser = !!(s && s.scanPausedByUser);

				if (pausedByUser) {
					status.textContent = "Scanner status: paused by user";
					btn.textContent = "Start BLE Scan";
				} else if (scanning) {
					status.textContent = "Scanner status: running";
					btn.textContent = "Stop BLE Scan";
				} else {
					status.textContent = "Scanner status: not running";
					btn.textContent = "Start BLE Scan";
				}

				btn.disabled = scanControlBusy;
			}

			function toggleBleScan() {
				if (scanControlBusy || !lastStats) return;
				var action = (lastStats.scanPausedByUser || !lastStats.bleScanning) ? "start" : "stop";
				scanControlBusy = true;
				updateScanControls(lastStats);
				fetch('/api/v1/scan/control?action=' + encodeURIComponent(action), { method: 'POST' })
					.then(function(r) {
						if (!r.ok) throw new Error('HTTP ' + r.status);
						return r.json();
					})
					.then(function() {
						resetStatsPolling();
						loadStats();
					})
					.catch(function(e) {
						statsErrorText = String(e);
						statsErrorUntilMs = Date.now() + 10000;
						updateStatsMeta();
					})
					.finally(function() {
						scanControlBusy = false;
						if (lastStats) updateScanControls(lastStats);
					});
			}

			function render(s) {
				var ble = "";
				ble += mkStat("Registered devices", s.registeredDevices || 0, null, "Number of known SwitchBot devices currently registered (max 50).");
				ble += mkStat("Scan mode", (s.scanPausedByUser ? "Manual Pause" : "Auto") + " | " + (s.bleScanning ? "Running" : "Stopped"), null, "Manual Pause means scan was stopped from the UI. Auto means firmware controls scanning health/restarts.");
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
				mem += mkStat("Diag: Scan callbacks", s.diagScanOnResultCalls || 0, null, "Total BLE advertisement callback invocations since boot.");
				mem += mkStat("Diag: Last restart age", (s.lastSuccessfulScanRestartAtMs ? formatUptime(Math.max(0, (s.uptimeMs || 0) - s.lastSuccessfulScanRestartAtMs)) : "never"), null, "Elapsed time since the last scan restart that reported the scanner running.");
				mem += mkStat("Diag: Last mem-recovery age", (s.lastMemoryScanRecoveryAtMs ? formatUptime(Math.max(0, (s.uptimeMs || 0) - s.lastMemoryScanRecoveryAtMs)) : "never"), null, "Elapsed time since the last automatic memory-fragmentation scan recovery cycle.");
				mem += mkStat("Diag: Scan restarts", s.diagScanRestartCount || 0, null, "Total scanner restarts (health + stale recovery).");
				mem += mkStat("Diag: Restart cooldown skips", s.diagScanRestartSuppressedCount || 0, null, "Auto-restart attempts skipped because the scanner restart cooldown window was still active.");
				mem += mkStat("Diag: Stale recoveries", s.diagScanStaleRecoveryCount || 0, null, "Number of stale-scan recovery cycles performed.");
				mem += mkStat("Diag: Mem recoveries", s.diagScanMemoryRecoveryCount || 0, null, "Automatic scan recoveries triggered when largest free heap block drops below the fragmentation threshold.");
				mem += mkStat("Diag: Raw packets", (s.diagRawPacketsBuilt || 0) + "/" + (s.diagRawPacketsSent || 0), null, "Raw packet stream payloads built/sent.");
				mem += mkStat("Diag: Push posts", (s.diagPushPostCount || 0) + " (err " + (s.diagPushPostErrorCount || 0) + ")", null, "Callback POST attempts and failures.");
				mem += mkStat("Diag: API hits", "stats " + (s.diagStatsRequests || 0) + " | devices " + (s.diagDevicesRequests || 0), null, "Total /api/v1/stats and /api/v1/devices requests since boot.");
				document.getElementById("memStats").innerHTML = mem;
				renderUnknownTypes(s.unknownTypes || []);
				updateScanControls(s);

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
				h += '<th>Raw Viewer</th>';
				h += '</tr></thead><tbody>';
				for (var i = 0; i < sorted.length; i++) {
					var t = Number(sorted[i].type || 0) & 0xFF;
					var c = Number(sorted[i].count || 0);
					var sub = sorted[i].subtype ? sorted[i].subtype : '-';
					var mac = sorted[i].mac ? sorted[i].mac : '-';
					var rawLink = '-';
					if (mac !== '-') {
						rawLink = '<a class="raw-link" href="/api/v1/device/viewer?address=' + encodeURIComponent(mac) + '">View Raw</a>';
					}
					h += '<tr><td>' + typeHex(t) + '</td><td>' + typeChar(t) + '</td><td>' + sub + '</td><td>' + mac + '</td><td>' + c + '</td><td>' + rawLink + '</td></tr>';
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

			function formatSpanLabel(ms) {
				var s = Math.round((ms || 0) / 1000);
				if (s < 60) return s + "s";
				var m = Math.round(s / 60);
				if (m < 60) return m + "m";
				var h = Math.round(m / 60);
				if (h < 24) return h + "h";
				var d = Math.round(h / 24);
				return d + "d";
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

				var padL = 44, padR = 16, padT = 12, padB = 40;
				var plotW = Math.max(1, cssW - padL - padR);
				var plotH = Math.max(1, cssH - padT - padB);
				var spanMs = intervalMs * (values.length - 1);
				var xTicks = 4;

				ctx.strokeStyle = "#333";
				ctx.lineWidth = 1;
				for (var gy = 0; gy <= 4; gy++) {
					var y = padT + (plotH * gy / 4);
					ctx.beginPath();
					ctx.moveTo(padL, y);
					ctx.lineTo(padL + plotW, y);
					ctx.stroke();
				}

				ctx.strokeStyle = "#2b2b2b";
				for (var gx = 0; gx <= xTicks; gx++) {
					var xg = padL + (plotW * gx / xTicks);
					ctx.beginPath();
					ctx.moveTo(xg, padT);
					ctx.lineTo(xg, padT + plotH);
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

				ctx.textAlign = "center";
				for (var tx = 0; tx <= xTicks; tx++) {
					var frac = tx / xTicks;
					var x = padL + (plotW * frac);
					var ageMs = Math.round((1 - frac) * spanMs);
					var label = (tx === xTicks) ? "now" : ("-" + formatSpanLabel(ageMs));
					ctx.fillText(label, x, padT + plotH + 18);
				}
				ctx.textAlign = "start";

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

			function fetchJsonWithTimeout(url, timeoutMs) {
				var sep = url.indexOf("?") >= 0 ? "&" : "?";
				var noCacheUrl = url + sep + "_ts=" + Date.now();
				return Promise.race([
					fetch(noCacheUrl, { headers: { Accept: "application/json" } })
						.then(function(r) {
							if (!r.ok) throw new Error("HTTP " + r.status);
							return r.json();
						}),
					new Promise(function(_, reject) {
						setTimeout(function() {
							reject(new Error("Timeout"));
						}, timeoutMs);
					})
				]);
			}

			function finishStatsFetch() {
				statsLastRequestStartedMs = 0;
				statsFetchInFlight = false;
			}

			function scheduleStatsRetry(ms) {
				if (statsRefreshTimer) clearTimeout(statsRefreshTimer);
				statsRefreshTimer = setTimeout(loadStats, ms);
			}

			function loadStats() {
				if (statsFetchInFlight) return;
				statsFetchInFlight = true;
				statsLastRequestStartedMs = Date.now();

				var p;
				try {
					p = fetchJsonWithTimeout("/api/v1/stats", 8000);
				} catch (e) {
					statsErrorText = "Stats fetch start failed: " + String(e);
					statsErrorUntilMs = Date.now() + 15000;
					updateStatsMeta();
					finishStatsFetch();
					scheduleStatsRetry(5000);
					return;
				}

				if (!p || typeof p.then !== "function") {
					statsErrorText = "Stats fetch returned invalid promise";
					statsErrorUntilMs = Date.now() + 15000;
					updateStatsMeta();
					finishStatsFetch();
					scheduleStatsRetry(5000);
					return;
				}

				p
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
						finishStatsFetch();
						scheduleStatsRetry(msUntilNext);
					})
					.catch(function(e) {
						statsErrorText = String(e);
						statsErrorUntilMs = Date.now() + 15000;
						updateStatsMeta();
						finishStatsFetch();
						scheduleStatsRetry(5000);
					});
			}

			function resetStatsPolling() {
				if (statsRefreshTimer) {
					clearTimeout(statsRefreshTimer);
					statsRefreshTimer = null;
				}
				statsLastRequestStartedMs = 0;
				statsFetchInFlight = false;
			}

			loadStats();
			setInterval(function() { if (lastStats) render(lastStats); }, 1000);

			window.addEventListener("pageshow", function() {
				resetStatsPolling();
				loadStats();
			});

			window.addEventListener("pagehide", function() {
				resetStatsPolling();
			});

			document.addEventListener("visibilitychange", function() {
				if (!document.hidden) {
					resetStatsPolling();
					loadStats();
				}
			});

			setInterval(function() {
				if (document.hidden) return;
				var now = Date.now();
				if (statsFetchInFlight && statsLastRequestStartedMs === 0) {
					statsErrorText = "Watchdog reset stale in-flight state";
					statsErrorUntilMs = now + 5000;
					updateStatsMeta();
					resetStatsPolling();
					loadStats();
					return;
				}
				if (statsFetchInFlight && statsLastRequestStartedMs > 0 && (now - statsLastRequestStartedMs) > 12000) {
					statsErrorText = "Watchdog reset stalled stats request";
					statsErrorUntilMs = now + 5000;
					updateStatsMeta();
					resetStatsPolling();
					loadStats();
					return;
				}
				if (!lastStats && !statsFetchInFlight) {
					loadStats();
				}
			}, 2000);
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
			.view-raw-btn { background: #4ec94e; color: #1e1e1e; border: none; border-radius: 4px; padding: 0.25rem 0.5rem; cursor: pointer; font-size: 0.8rem; font-weight: 600; text-decoration: none; display: inline-block; }
			.view-raw-btn:hover { background: #5fd95f; }
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
			var devicesLoadInFlight = false;
			var devicesLoadQueued = false;
			var devicesLoadTimer = null;
			var lastDevicesLoadAt = 0;
			var minDevicesLoadIntervalMs = 1000;

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
				var model = s.model;
				if (typeof model === "string" && model.length > 0) {
					var modelCode = model.charCodeAt(0);
					o.modelType = (modelCode >= 32 && modelCode <= 126)
						? model
						: ("0x" + modelCode.toString(16).toUpperCase().padStart(2, "0"));
				} else if (typeof model === "number") {
					o.modelType = "0x" + model.toString(16).toUpperCase().padStart(2, "0");
				}
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
				var fixed = ["address", "modelType", "rssi"];
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
        h += '<th>Action</th>';
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
          h += '<td><a class="view-raw-btn" href="/api/v1/device/viewer?address=' + encodeURIComponent(r.address) + '">View Raw</a></td>';
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

      function scheduleLoad() {
				if (devicesLoadInFlight) {
					devicesLoadQueued = true;
					return;
				}
				if (devicesLoadTimer !== null) {
					devicesLoadQueued = true;
					return;
				}
				var now = Date.now();
				var wait = Math.max(0, minDevicesLoadIntervalMs - (now - lastDevicesLoadAt));
				devicesLoadTimer = setTimeout(function() {
					devicesLoadTimer = null;
					load();
				}, wait);
			}

      function load() {
				if (devicesLoadInFlight) {
					devicesLoadQueued = true;
					return;
				}
				devicesLoadInFlight = true;
				lastDevicesLoadAt = Date.now();
        fetch("/api/v1/devices", { headers: { Accept: "application/json" } })
					.then(function(r) {
						if (!r.ok) {
							throw new Error("HTTP " + r.status + " " + r.statusText);
						}
						return r.json();
					})
          .then(function(data) {
					var list = Array.isArray(data) ? data : (data && typeof data === "object" ? [data] : []);
            groups = {};
					list.forEach(function(d) {
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
						document.getElementById("ts").textContent = "Error: " + e;
            document.getElementById("tbl").innerHTML = "<p style=color:red>Error: " + e + "</p>";
					})
					.finally(function() {
						devicesLoadInFlight = false;
						if (devicesLoadQueued) {
							devicesLoadQueued = false;
							scheduleLoad();
						}
          });
      }

			scheduleLoad();

      var es = new EventSource("/api/v1/events");
			es.addEventListener("ble", function() { scheduleLoad(); });
      es.onopen = function() {
        document.getElementById("live").style.color = "#4ec94e";
        document.getElementById("live").textContent = "\u25cf live";
      };
      es.onerror = function() {
        document.getElementById("live").style.color = "#e05252";
        document.getElementById("live").textContent = "\u25cf disconnected";
      };
			window.addEventListener("beforeunload", function() {
				es.close();
			});
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
			td.result-cell { white-space: pre-wrap; word-break: break-word; }
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
					+ '<col style="width:7em"><col style="width:9em"><col style="width:12em"><col><col style="width:16em">'
					+ '</colgroup><thead><tr><th>At</th><th>Source IP</th><th>Address</th><th>Data</th><th>Result</th></tr></thead><tbody>';
				for (var i = 0; i < list.length; i++) {
					var e = list[i] || {};
					h += '<tr><td class="nowrap-cell">' + fmtAge(e.atMs) + '</td><td class="nowrap-cell">' + esc(e.sourceIp || '-') + '</td><td class="nowrap-cell"><code>' + esc(e.address || '') + '</code></td><td class="wrap-cell"><code>' + esc(e.data || '') + '</code></td><td class="result-cell" title="' + esc(e.result || '') + '">' + esc(e.result || '') + '</td></tr>';
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

static const char RAW_PACKET_VIEWER_HTML[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
	<head>
		<meta charset="UTF-8">
		<meta name="viewport" content="width=device-width, initial-scale=1">
		<title>Raw BLE Packet Viewer</title>
		<style>
			body { font-family: monospace; background: #1e1e1e; color: #d4d4d4; margin: 1rem 2rem; }
			.top { display: flex; align-items: center; justify-content: space-between; gap: 1rem; margin-bottom: 1.5rem; }
			h1 { color: #9cdcfe; margin: 0; font-size: 1.4rem; }
			.back { text-decoration: none; background: #3c3c3c; color: #fff; border: none; border-radius: 6px; padding: 0.45rem 0.7rem; font-weight: 600; cursor: pointer; font-size: 1rem; }
			.back:hover { background: #555; }
			.clear-btn { background: #5a3030; color: #f29a9a; border: none; border-radius: 6px; padding: 0.45rem 0.7rem; font-weight: 600; cursor: pointer; font-size: 0.9rem; }
			.clear-btn:hover { background: #6e3535; }
			.copy-btn { background: #2d4f2d; color: #9ef59e; border: none; border-radius: 6px; padding: 0.45rem 0.7rem; font-weight: 600; cursor: pointer; font-size: 0.9rem; }
			.copy-btn:hover { background: #356035; }
			.top-right { display: flex; align-items: center; gap: 0.6rem; }
			.info { background: #252526; border: 1px solid #3c3c3c; border-radius: 8px; padding: 1rem; margin-bottom: 1.5rem; }
			.info-row { margin: 0.5rem 0; }
			.info-label { color: #9cdcfe; font-weight: bold; }
			.info-value { color: #d4d4d4; margin-left: 0.5rem; }
			.section { background: #252526; border: 1px solid #3c3c3c; border-radius: 8px; padding: 1rem; margin-bottom: 1.5rem; }
			.section-title { color: #9cdcfe; font-weight: bold; font-size: 1.05rem; margin-bottom: 0.8rem; border-bottom: 2px solid #3c3c3c; padding-bottom: 0.5rem; }
			.history-table { width: 100%; border-collapse: collapse; }
			.history-table th { color: #9cdcfe; text-align: left; padding: 0.4rem 0.6rem; border-bottom: 1px solid #3c3c3c; font-size: 0.85rem; white-space: nowrap; }
			.history-table td { padding: 0.4rem 0.6rem; border-bottom: 1px solid #2a2a2a; font-size: 0.9rem; vertical-align: middle; word-break: break-all; }
			.history-table tr:first-child td { background: #1e2a1e; }
			.history-table tr:hover td { background: #2d2d2d; }
			.time-col { color: #888; white-space: nowrap; width: 6em; }
			.rssi-col { color: #888; white-space: nowrap; width: 5em; }
			.state-col { white-space: nowrap; min-width: 10em; }
			.bits-col { white-space: nowrap; min-width: 14em; }
			.notes-col { min-width: 12em; }
			.byte-value { color: #9ef59e; font-weight: bold; cursor: pointer; }
			.byte-value:hover { text-decoration: underline; opacity: 0.85; }
			.byte-value-changed { color: #000; background: #f8d37a; font-weight: bold; padding: 0.1rem 0.25rem; border-radius: 3px; cursor: pointer; }
			.byte-value-changed:hover { opacity: 0.8; }
			.byte-selected { outline: 2px solid #9cdcfe; border-radius: 3px; }
			.byte-separator { color: #555; }
			.dash-separator { color: #666; padding: 0 0.4rem; }
			.bit-label { color: #666; font-size: 0.78rem; margin-right: 0.35rem; }
			.bit-normal { color: #9ef59e; font-weight: bold; letter-spacing: 0.05rem; }
			.bit-changed { color: #000; background: #f8d37a; font-weight: bold; padding: 0.05rem 0.15rem; border-radius: 2px; letter-spacing: 0; }
			.bit-gap { margin: 0 0.25rem; color: #444; }
			.state-value { color: #9ef59e; font-weight: bold; }
			.state-changed { color: #000; background: #f8d37a; font-weight: bold; padding: 0.1rem 0.25rem; border-radius: 3px; }
			.note-input { width: 100%; min-width: 10em; box-sizing: border-box; padding: 0.25rem 0.35rem; border-radius: 4px; border: 1px solid #444; background: #1a1a1a; color: #d4d4d4; }
			.note-input:focus { outline: 2px solid #9cdcfe; border-color: #9cdcfe; }
			.no-data { color: #888; font-style: italic; }
			.error { color: #f29a9a; }
			.live { color: #4ec94e; font-weight: bold; }
			.empty-row { color: #888; font-style: italic; padding: 1rem; }
		</style>
	</head>
	<body>
		<div class="top">
			<h1>Raw BLE Packet Viewer</h1>
			<div class="top-right">
				<button class="copy-btn" onclick="copyTableToClipboard()">Copy Table</button>
				<button class="clear-btn" onclick="clearHistory()">Clear</button>
				<button class="back" onclick="goBackAndClear()">&#8592; Back to Devices</button>
			</div>
		</div>

		<div class="info">
			<div class="info-row">
				<span class="info-label">Device Address:</span>
				<span class="info-value" id="deviceAddress">-</span>
			</div>
			<div class="info-row">
				<span class="info-label">RSSI:</span>
				<span class="info-value" id="deviceRssi">-</span>
			</div>
			<div class="info-row">
				<span class="info-label">Last Update:</span>
				<span class="info-value" id="lastUpdate">-</span>
			</div>
			<div class="info-row">
				<span class="info-label">Status:</span>
				<span class="info-value" id="status">
					<span class="live">&#9679; Live</span> - Waiting for packets...
				</span>
			</div>
		</div>

		<div class="section">
			<div class="section-title">Packet History <span id="rowCount" style="color:#888;font-size:0.85rem;font-weight:normal"></span></div>
			<table class="history-table">
				<thead>
					<tr>
						<th class="time-col">Time</th>
						<th class="rssi-col">RSSI</th>
						<th>Service Data &nbsp;&#8212;&nbsp; Manufacturer Data</th>
						<th class="bits-col">Bits (click a byte)</th>
						<th class="notes-col">Notes</th>
					</tr>
				</thead>
				<tbody id="historyBody">
					<tr><td colspan="5" class="empty-row">Waiting for packets...</td></tr>
				</tbody>
			</table>
		</div>

		<script>
			var MAX_ROWS = 100;
			var rows = [];
			var rawStream = null;
			var watchKeepaliveTimer = null;
			var reconnectTimer = null;

			function getAddressParam() {
				var params = new URLSearchParams(window.location.search);
				return params.get('address');
			}

			function formatHex(value) {
				return '0x' + ('0' + value.toString(16).toUpperCase()).slice(-2);
			}

			function formatHexList(bytes) {
				if (!bytes || bytes.length === 0) return '-';
				var out = '';
				for (var i = 0; i < bytes.length; i++) {
					if (i > 0) out += ', ';
					out += formatHex(bytes[i]);
				}
				return out;
			}

			function escapeAttr(text) {
				return String(text || '')
					.replace(/&/g, '&amp;')
					.replace(/"/g, '&quot;')
					.replace(/</g, '&lt;')
					.replace(/>/g, '&gt;');
			}

			function arraysEqual(a, b) {
				if (!a && !b) return true;
				if (!a || !b) return false;
				if (a.length !== b.length) return false;
				for (var i = 0; i < a.length; i++) { if (a[i] !== b[i]) return false; }
				return true;
			}

			function formatBytes(bytes, previousBytes, rowIdx, type) {
				if (!bytes || bytes.length === 0) return '<span class="no-data">-</span>';
				var html = '';
				for (var i = 0; i < bytes.length; i++) {
					var changed = previousBytes && i < previousBytes.length && previousBytes[i] !== bytes[i];
					if (i > 0) html += '<span class="byte-separator">, </span>';
					html += '<span class="' + (changed ? 'byte-value-changed' : 'byte-value') +
									'" data-brow="' + rowIdx + '" data-bidx="' + i +
									'" data-btype="' + type + '" data-bval="' + bytes[i] + '">' +
									formatHex(bytes[i]) + '</span>';
				}
				return html;
			}

			function formatBits(val, prevVal, byteIdx, type) {
				var html = '<span class="bit-label">' + type + '[' + byteIdx + ']</span>';
				for (var b = 7; b >= 0; b--) {
					var bit = (val >> b) & 1;
					var prevBit = (prevVal !== null && prevVal !== undefined) ? (prevVal >> b) & 1 : null;
					var changed = prevBit !== null && prevBit !== bit;
					if (b === 3) html += '<span class="bit-gap">&#xb7;</span>';
					html += '<span class="' + (changed ? 'bit-changed' : 'bit-normal') + '">' + bit + '</span>';
				}
				return html;
			}

			function decodeStateInfo(mfgBytes) {
				if (!mfgBytes || mfgBytes.length <= 9) return null;
				var val = mfgBytes[9];
				var mask30 = val & 0x30;
				var mask08 = val & 0x08;
				var bitPair = ((mask30 >> 5) & 1).toString() + ((mask30 >> 4) & 1).toString();
				var stateLabel = 'Locked';
				if (mask30 === 0x30) {
					stateLabel = 'Latched';
				} else if (mask08 === 0x08) {
					stateLabel = 'Unlocked';
				}
				return {
					mask30: mask30,
					mask08: mask08,
					bitPair: bitPair,
					stateLabel: stateLabel,
					text: 'mfg[9]&0x30=' + formatHex(mask30) + ' (' + bitPair + '), mfg[9]&0x08=' + formatHex(mask08) + ' -> ' + stateLabel
				};
			}

			function formatStateBits(mfgBytes, prevMfgBytes) {
				var state = decodeStateInfo(mfgBytes);
				if (!state) return '<span class="no-data">-</span>';
				var prevState = decodeStateInfo(prevMfgBytes);
				var changed = prevState && (prevState.mask30 !== state.mask30 || prevState.mask08 !== state.mask08);
				return '<span class="' + (changed ? 'state-changed' : 'state-value') + '">' + state.text + '</span>';
			}

			function copyTableToClipboard() {
				if (rows.length === 0) return;
				var lines = [];
				lines.push('Time\tRSSI\tService Data\tManufacturer Data\tState\tNotes');
				for (var i = 0; i < rows.length; i++) {
					var r = rows[i];
					var state = decodeStateInfo(r.manufacturerData);
					lines.push([
						r.time,
						r.rssi,
						formatHexList(r.serviceData),
						formatHexList(r.manufacturerData),
						state ? state.text : '-',
						(r.note || '').replace(/\r?\n/g, ' ')
					].join('\t'));
				}
				var output = lines.join('\n');

				function showCopied(msg) {
					document.getElementById('status').innerHTML = '<span class="live">&#9679; Live</span> - ' + msg;
				}

				if (navigator.clipboard && navigator.clipboard.writeText) {
					navigator.clipboard.writeText(output)
						.then(function() { showCopied('Copied ' + rows.length + ' rows to clipboard'); })
						.catch(function() { fallbackCopy(output, showCopied); });
				} else {
					fallbackCopy(output, showCopied);
				}
			}

			function fallbackCopy(text, onDone) {
				var ta = document.createElement('textarea');
				ta.value = text;
				ta.setAttribute('readonly', '');
				ta.style.position = 'absolute';
				ta.style.left = '-9999px';
				document.body.appendChild(ta);
				ta.select();
				try {
					document.execCommand('copy');
					onDone('Copied ' + rows.length + ' rows to clipboard');
				} catch (e) {
					onDone('Copy failed');
				}
				document.body.removeChild(ta);
			}

			function renderRows() {
				var tbody = document.getElementById('historyBody');
				if (rows.length === 0) {
					tbody.innerHTML = '<tr><td colspan="5" class="empty-row">Waiting for packets...</td></tr>';
					document.getElementById('rowCount').textContent = '';
					return;
				}
				document.getElementById('rowCount').textContent = '(' + rows.length + ')';
				var html = '';
				for (var i = 0; i < rows.length; i++) {
					var r = rows[i];
					var prev = i + 1 < rows.length ? rows[i + 1] : null;
					var prevSvc = prev ? prev.serviceData : null;
					var prevMfg = prev ? prev.manufacturerData : null;
					var svcHtml = formatBytes(r.serviceData, prevSvc, i, 'svc');
					var mfgHtml = formatBytes(r.manufacturerData, prevMfg, i, 'mfg');
					html += '<tr>';
					html += '<td class="time-col">' + r.time + '</td>';
					html += '<td class="rssi-col">' + r.rssi + '</td>';
					html += '<td>' + svcHtml + '<span class="dash-separator">&#8212;</span>' + mfgHtml + '</td>';
					html += '<td class="bits-col" id="bits-' + i + '"></td>';
					html += '<td class="notes-col"><input class="note-input" type="text" data-note-row="' + i + '" value="' + escapeAttr(r.note) + '" placeholder="e.g. Locked, Latched"></td>';
					html += '</tr>';
				}
				tbody.innerHTML = html;
			}

			document.getElementById('historyBody').addEventListener('click', function(e) {
				var t = e.target;
				var rowIdx = t.getAttribute('data-brow');
				if (rowIdx === null) return;
				rowIdx = parseInt(rowIdx);
				var byteIdx = parseInt(t.getAttribute('data-bidx'));
				var type = t.getAttribute('data-btype');
				var val = parseInt(t.getAttribute('data-bval'));

				// Highlight selected byte, clear previous
				var prevSel = document.querySelectorAll('.byte-selected');
				for (var s = 0; s < prevSel.length; s++) prevSel[s].classList.remove('byte-selected');
				t.classList.add('byte-selected');

				var prevRow = rows[rowIdx + 1];
				var prevBytes = prevRow ? (type === 'svc' ? prevRow.serviceData : prevRow.manufacturerData) : null;
				var prevVal = (prevBytes && byteIdx < prevBytes.length) ? prevBytes[byteIdx] : null;

				var bitsCell = document.getElementById('bits-' + rowIdx);
				if (bitsCell) bitsCell.innerHTML = formatBits(val, prevVal, byteIdx, type);
			});

			document.getElementById('historyBody').addEventListener('input', function(e) {
				var t = e.target;
				var noteRow = t.getAttribute('data-note-row');
				if (noteRow === null) return;
				noteRow = parseInt(noteRow);
				if (!isNaN(noteRow) && rows[noteRow]) rows[noteRow].note = t.value;
			});

			function addPacket(data) {
				var svc = data.serviceData ? data.serviceData.slice() : [];
				var mfg = data.manufacturerData ? data.manufacturerData.slice() : [];
				if (rows.length > 0 && arraysEqual(svc, rows[0].serviceData) && arraysEqual(mfg, rows[0].manufacturerData)) {
					document.getElementById('deviceRssi').textContent = data.rssi + ' dBm';
					document.getElementById('lastUpdate').textContent = new Date().toLocaleTimeString();
					return;
				}
				rows.unshift({ serviceData: svc, manufacturerData: mfg, rssi: data.rssi, time: new Date().toLocaleTimeString(), note: '' });
				if (rows.length > MAX_ROWS) rows.pop();
				document.getElementById('deviceAddress').textContent = data.address || '-';
				document.getElementById('deviceRssi').textContent = data.rssi + ' dBm';
				document.getElementById('lastUpdate').textContent = rows[0].time;
				renderRows();
			}

			function clearHistory() {
				rows = [];
				renderRows();
			}

			function goBackAndClear() {
				if (rawStream) {
					rawStream.close();
					rawStream = null;
				}
				if (watchKeepaliveTimer !== null) {
					clearInterval(watchKeepaliveTimer);
					watchKeepaliveTimer = null;
				}
				if (reconnectTimer !== null) {
					clearTimeout(reconnectTimer);
					reconnectTimer = null;
				}
				fetch('/api/v1/device/watch?address=').catch(function(){});
				window.location.href = '/api/v1/devices/table';
			}

			function refreshWatch(address) {
				fetch('/api/v1/device/watch?address=' + encodeURIComponent(address))
					.catch(function(e) { console.log('Error setting watch:', e); });
			}

      function startLiveStream() {
        var address = getAddressParam();
        if (!address) {
          document.getElementById('status').innerHTML = '<span class="error">Error: No device address specified</span>';
          return;
        }

				refreshWatch(address);
				if (watchKeepaliveTimer === null) {
					watchKeepaliveTimer = setInterval(function() { refreshWatch(address); }, 30000);
				}

				if (rawStream) {
					rawStream.close();
					rawStream = null;
				}

				rawStream = new EventSource('/api/v1/device/raw-stream');

				rawStream.addEventListener('packet', function(e) {
          try {
            addPacket(JSON.parse(e.data));
          } catch (err) {
            console.log('Error parsing packet data:', err);
          }
        });

				rawStream.onerror = function() {
          document.getElementById('status').innerHTML = '<span class="error">Connection lost - reconnecting...</span>';
					if (rawStream) {
						rawStream.close();
						rawStream = null;
					}
					if (reconnectTimer === null) {
						reconnectTimer = setTimeout(function() {
							reconnectTimer = null;
							startLiveStream();
						}, 3000);
					}
        };
      }

			window.addEventListener("beforeunload", function() {
				if (rawStream) {
					rawStream.close();
					rawStream = null;
				}
				if (watchKeepaliveTimer !== null) {
					clearInterval(watchKeepaliveTimer);
					watchKeepaliveTimer = null;
				}
				if (reconnectTimer !== null) {
					clearTimeout(reconnectTimer);
					reconnectTimer = null;
				}
				fetch('/api/v1/device/watch?address=').catch(function(){});
			});

      startLiveStream();
    </script>
  </body>
</html>
)HTMLEOF";

BLE_Device BLE_Devices;
ClientCallbacks OurCallbacks;
SemaphoreHandle_t callbackMutex = nullptr;
SemaphoreHandle_t historyMutex = nullptr;

// Hot MAC address for real-time raw packet streaming
char hotMAC[ 18 ] = "";
volatile uint32_t hotMACLeaseExpiresAtMs = 0;
const uint32_t HOT_MAC_LEASE_MS = 120000;
SemaphoreHandle_t hotMACMutex = nullptr;
AsyncEventSource rawPacketEvents( "/api/v1/device/raw-stream" );

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
volatile bool BLECommandConnectInProgress = false;
bool RebootRequired = false;
bool otaInProgress = false;
volatile bool ScanPausedByUser = false;

// Single shared response buffer used by all HTTP GET handlers.
// request->send() copies the data before returning so reuse is safe.
static char sharedRespBuf[ 16384 ];
int32_t NumUpdates = 0;
int32_t NumActualDataUpdates = 0;
int32_t NumAdvertsSeen = 0;
int32_t NumMatchedServiceData = 0;
int32_t NumMatchedEmptyPayload = 0;
int32_t NumMatchedRejected = 0;
int32_t NumBadDataRejected = 0;
int32_t NumUpdatesAt0 = 0;
int32_t NumZeroUpdateIntervals = 0;
int32_t NumZeroAdvertIntervals = 0;
volatile int32_t LastUpdatesPerMinute = 0;
volatile int32_t LastActualDataUpdatesPerMinute = 0;
volatile int32_t LastAdvertsSeenPerMinute = 0;
volatile int32_t LastMatchedServiceDataPerMinute = 0;
volatile int32_t LastMatchedEmptyPayloadPerMinute = 0;
volatile int32_t LastMatchedRejectedPerMinute = 0;
volatile int32_t LastBadDataRejectedPerMinute = 0;
volatile uint32_t LastAdvertSeenAtMs = 0;
volatile uint32_t LastForcedScanRecoveryAtMs = 0;
volatile uint32_t LastScanRestartAttemptAtMs = 0;
volatile uint32_t LastSuccessfulScanRestartAtMs = 0;
volatile uint32_t LastMemoryScanRecoveryAtMs = 0;
volatile uint32_t LastFreeHeap = 0;
volatile uint32_t LastLargestHeapBlock = 0;
volatile uint32_t LastStatsAt = 0;
volatile uint8_t LastCpuUsagePercent = 0;
static uint32_t PrevIdleRunTime = 0;
static uint32_t PrevTotalRunTime = 0;
static constexpr UBaseType_t CPU_USAGE_MAX_TASKS = 32;
static TaskStatus_t CpuTaskBuffer[ CPU_USAGE_MAX_TASKS ];
volatile bool pendingSSEUpdate = false;
volatile bool pendingSSEStats = false;
volatile uint32_t DiagStatsRequests = 0;
volatile uint32_t DiagDevicesRequests = 0;
volatile uint32_t DiagRawPacketsBuilt = 0;
volatile uint32_t DiagRawPacketsSent = 0;
volatile uint32_t DiagSseBleSent = 0;
volatile uint32_t DiagSseStatsSent = 0;
volatile uint32_t DiagScanOnResultCalls = 0;
volatile uint32_t DiagScanRestartCount = 0;
volatile uint32_t DiagScanRestartSuppressedCount = 0;
volatile uint32_t DiagScanStaleRecoveryCount = 0;
volatile uint32_t DiagScanMemoryRecoveryCount = 0;
volatile uint32_t DiagDiscoveryPacketsRx = 0;
volatile uint32_t DiagDiscoveryQueryRx = 0;
volatile uint32_t DiagDiscoveryAnnouncementsTx = 0;
volatile uint32_t DiagPushPostCount = 0;
volatile uint32_t DiagPushPostErrorCount = 0;
unsigned long lastSSESend = 0;
unsigned long lastSSEStatsSend = 0;
unsigned long nextStatsSample = 0;
const uint32_t STATS_SAMPLE_MS = 15000;
const uint32_t STATS_PER_MINUTE_SCALE = 60000 / STATS_SAMPLE_MS;
const uint32_t SCAN_RESTART_COOLDOWN_MS = 2000;
const uint32_t SCAN_MEMORY_RECOVERY_COOLDOWN_MS = 120000;
const uint32_t SCAN_MEMORY_RECOVERY_LARGEST_BLOCK_THRESHOLD = 17000;
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
	char result[ 96 ];
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

// Write JSON-escaped string (no surrounding quotes) directly into dst buffer.
// Returns number of chars written (excluding null terminator).
static int JsonEscapeInto( char* dst, int dstMax, const char* src )
{
	if ( !src || dstMax <= 1 )
		return 0;
	int n = 0;
	for ( const char* p = src; *p != '\0'; p++ )
	{
		uint8_t ch = ( uint8_t )*p;
		int need;
		switch ( ch )
		{
		case '\\':
		case '"':
		case '\n':
		case '\r':
		case '\t':
			need = 2;
			break;
		default:
			need = ( ch < 0x20 ) ? 6 : 1;
			break;
		}
		if ( n + need >= dstMax )
			break;
		switch ( ch )
		{
		case '\\':
			dst[ n++ ] = '\\';
			dst[ n++ ] = '\\';
			break;
		case '"':
			dst[ n++ ] = '\\';
			dst[ n++ ] = '"';
			break;
		case '\n':
			dst[ n++ ] = '\\';
			dst[ n++ ] = 'n';
			break;
		case '\r':
			dst[ n++ ] = '\\';
			dst[ n++ ] = 'r';
			break;
		case '\t':
			dst[ n++ ] = '\\';
			dst[ n++ ] = 't';
			break;
		default:
			if ( ch < 0x20 )
			{
				snprintf( dst + n, 7, "\\u%04X", ( unsigned )ch );
				n += 6;
			}
			else
				dst[ n++ ] = ( char )ch;
			break;
		}
	}
	dst[ n ] = '\0';
	return n;
}

// Safe append into a fixed JSON response buffer.
// Keeps rem in [1..bufSize] so callers cannot underflow and corrupt memory on truncation.
static void RespBufAppendf( char* buf, int bufSize, int& pos, int& rem, const char* fmt, ... )
{
	if ( !buf || bufSize <= 0 || !fmt )
		return;

	if ( rem <= 1 )
	{
		if ( pos < 0 )
			pos = 0;
		if ( pos >= bufSize )
			pos = bufSize - 1;
		buf[ pos ] = '\0';
		rem = 1;
		return;
	}

	va_list args;
	va_start( args, fmt );
	int written = vsnprintf( buf + pos, ( size_t )rem, fmt, args );
	va_end( args );

	if ( written < 0 )
	{
		buf[ pos ] = '\0';
		rem = 1;
		return;
	}

	if ( written >= rem )
	{
		pos += rem - 1;
		if ( pos >= bufSize )
			pos = bufSize - 1;
		rem = 1;
	}
	else
	{
		pos += written;
		rem -= written;
	}
}

static bool IsHexDigitChar( char c )
{
	return ( c >= '0' && c <= '9' ) || ( c >= 'a' && c <= 'f' ) || ( c >= 'A' && c <= 'F' );
}

static bool StrEqualsIgnoreCase( const char* a, const char* b )
{
	if ( a == nullptr || b == nullptr )
	{
		return false;
	}

	while ( *a != '\0' && *b != '\0' )
	{
		if ( tolower( ( unsigned char )*a ) != tolower( ( unsigned char )*b ) )
		{
			return false;
		}
		a++;
		b++;
	}

	return ( *a == '\0' && *b == '\0' );
}

static bool ExtractSwitchBotAdvData( const BLEAdvertisedDevice* advertisedDevice,
	                                 const uint8_t** serviceData,
	                                 uint8_t* serviceDataLen,
	                                 const uint8_t** manufacturerData,
	                                 uint8_t* manufacturerDataLen )
{
	if ( advertisedDevice == nullptr || serviceData == nullptr || serviceDataLen == nullptr ||
	     manufacturerData == nullptr || manufacturerDataLen == nullptr )
	{
		return false;
	}

	*serviceData = nullptr;
	*serviceDataLen = 0;
	*manufacturerData = nullptr;
	*manufacturerDataLen = 0;

	const std::vector<uint8_t>& payload = advertisedDevice->getPayload();
	for ( size_t i = 0; i < payload.size(); )
	{
		uint8_t fieldLen = payload[ i ];
		if ( fieldLen == 0 )
		{
			break;
		}

		size_t fieldStart = i + 1;
		size_t fieldEnd = fieldStart + fieldLen;
		if ( fieldEnd > payload.size() )
		{
			break;
		}

		uint8_t type = payload[ fieldStart ];
		const uint8_t* value = &payload[ fieldStart + 1 ];
		size_t valueLen = fieldLen - 1;

		if ( type == BLE_HS_ADV_TYPE_SVC_DATA_UUID16 && valueLen >= 2 )
		{
			uint16_t uuid16 = ( ( uint16_t )value[ 1 ] << 8 ) | value[ 0 ];
			if ( uuid16 == 0x0d00 || uuid16 == 0xfd3d )
			{
				*serviceData = value + 2;
				*serviceDataLen = ( uint8_t )( valueLen - 2 );
			}
		}
		else if ( type == BLE_HS_ADV_TYPE_MFG_DATA )
		{
			*manufacturerData = value;
			*manufacturerDataLen = ( uint8_t )valueLen;
		}

		i = fieldEnd;
	}

	return ( *serviceData != nullptr );
}

static void FormatBleAddress( const NimBLEAddress& address, char* outMac, size_t outMacSize )
{
	if ( outMac == nullptr || outMacSize == 0 )
	{
		return;
	}

	const uint8_t* addr = address.getVal();
	if ( addr == nullptr )
	{
		outMac[ 0 ] = '\0';
		return;
	}

	snprintf( outMac,
	          outMacSize,
	          "%02X:%02X:%02X:%02X:%02X:%02X",
	          addr[ 5 ],
	          addr[ 4 ],
	          addr[ 3 ],
	          addr[ 2 ],
	          addr[ 1 ],
	          addr[ 0 ] );
}

static bool StartBleScanTracked( BLEScan* pBLEScan, bool trackRestart )
{
	if ( pBLEScan == nullptr )
	{
		return false;
	}

	pBLEScan->start( 0, false, true );
	bool isRunning = pBLEScan->isScanning();
	if ( trackRestart && isRunning )
	{
		LastSuccessfulScanRestartAtMs = millis();
	}

	return isRunning;
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
	char ip[ 64 ] = { 0 };
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
	uint32_t totalRunTime = 0;
	UBaseType_t taskCount = uxTaskGetSystemState( CpuTaskBuffer, CPU_USAGE_MAX_TASKS, &totalRunTime );

	uint32_t idleRunTime = 0;
	for ( UBaseType_t i = 0; i < taskCount; i++ )
	{
		if ( strncmp( CpuTaskBuffer[ i ].pcTaskName, "IDLE", 4 ) == 0 )
		{
			idleRunTime += CpuTaskBuffer[ i ].ulRunTimeCounter;
		}
	}

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
		IncrementVolatileU32( DiagDiscoveryAnnouncementsTx );
		char ipBuf[ 16 ];
		FormatIpAddress( *targetIp, ipBuf, sizeof( ipBuf ) );
		Serial.printf( "UDP unicast discovery reply to %s:%u (%u bytes)\n", ipBuf, targetPort, ( unsigned int )sent );
	}

	size_t multicastSent = udp.writeTo( ( const uint8_t* )message, ( size_t )length, DISCOVERY_MULTICAST_IP, DISCOVERY_PORT );
	IncrementVolatileU32( DiagDiscoveryAnnouncementsTx );
	char multicastIpBuf[ 16 ];
	FormatIpAddress( DISCOVERY_MULTICAST_IP, multicastIpBuf, sizeof( multicastIpBuf ) );
	Serial.printf( "UDP multicast discovery announcement to %s:%u (%u bytes)\n", multicastIpBuf, DISCOVERY_PORT, ( unsigned int )multicastSent );
}

static void HandleDiscoveryPacket( AsyncUDPPacket& packet, const char* listenerName )
{
	IncrementVolatileU32( DiagDiscoveryPacketsRx );

	// Build packet text directly into buffer
	char packetText[ 256 ];
	size_t textLen = 0;
	for ( size_t i = 0; i < packet.length() && textLen < sizeof( packetText ) - 1; i++ )
	{
		char c = ( char )packet.data()[ i ];
		if ( c == '\0' )
			break;
		packetText[ textLen++ ] = c;
	}
	packetText[ textLen ] = '\0';

	// Trim trailing whitespace
	while ( textLen > 0 && ( packetText[ textLen - 1 ] == ' ' || packetText[ textLen - 1 ] == '\t' ||
	                         packetText[ textLen - 1 ] == '\r' || packetText[ textLen - 1 ] == '\n' ) )
	{
		packetText[ --textLen ] = '\0';
	}

	// Check for SwitchBot discovery query (case-insensitive substring search)
	bool isDiscoveryQuery = ( strcmp( packetText, "Are you there SwitchBot?" ) == 0 ) ||
	                       ( strcmp( packetText, "Are you there SwitchBot" ) == 0 );

	// Case-insensitive substring search using strcasestr (if available) or manual search
	if ( !isDiscoveryQuery )
	{
		char packetLower[ 256 ];
		for ( size_t i = 0; i <= textLen; i++ )
		{
			packetLower[ i ] = tolower( packetText[ i ] );
		}
		isDiscoveryQuery = ( strstr( packetLower, "are you there switchbot" ) != nullptr );
	}

	if ( isDiscoveryQuery )
	{
		IncrementVolatileU32( DiagDiscoveryQueryRx );
		char remoteIpBuf[ 16 ];
		FormatIpAddress( packet.remoteIP(), remoteIpBuf, sizeof( remoteIpBuf ) );
		Serial.printf( "Received discovery on %s: '%s' from %s:%u\n",
		               listenerName,
		               packetText,
		               remoteIpBuf,
		               packet.remotePort() );
		IPAddress remoteIp = packet.remoteIP();
		uint16_t remotePort = packet.remotePort();
		SendDiscoveryAnnouncement( &remoteIp, remotePort );
		sendBroadcast = millis();
	}
	else
	{
		char remoteIpBuf[ 16 ];
		FormatIpAddress( packet.remoteIP(), remoteIpBuf, sizeof( remoteIpBuf ) );
		Serial.printf( "Received other UDP packet on %s from %s:%u len=%u text='%s'\n",
		               listenerName,
		               remoteIpBuf,
		               packet.remotePort(),
		               ( unsigned int )packet.length(),
		               packetText );
	}
}

// Stream raw packet data to web clients if the MAC matches the hot MAC
void streamRawPacketData( const char* MAC, int rssi, const uint8_t* serviceData, uint8_t serviceDataSize, const uint8_t* manufacturerData, uint8_t manufacturerDataSize )
{
	if ( hotMACMutex == nullptr )
		return;

	xSemaphoreTake( hotMACMutex, portMAX_DELAY );
	bool shouldStream = false;
	if ( hotMAC[ 0 ] != '\0' )
	{
		const uint32_t nowMs = millis();
		if ( ( int32_t )( nowMs - hotMACLeaseExpiresAtMs ) >= 0 )
		{
			hotMAC[ 0 ] = '\0';
			hotMACLeaseExpiresAtMs = 0;
		}
		else
		{
			shouldStream = StrEqualsIgnoreCase( MAC, hotMAC );
		}
	}
	xSemaphoreGive( hotMACMutex );

	if ( !shouldStream )
		return;

	IncrementVolatileU32( DiagRawPacketsBuilt );

	// Build JSON with raw byte arrays
	int pos = 0;
	int rem = ( int )sizeof( sharedRespBuf );
#define RAW_APPEND( ... )                                                                        \
	do                                                                                            \
	{                                                                                             \
		RespBufAppendf( sharedRespBuf, ( int )sizeof( sharedRespBuf ), pos, rem, __VA_ARGS__ ); \
	} while ( 0 )

	RAW_APPEND( "{\"address\":\"%s\",\"rssi\":%d,\"serviceData\":[", MAC, rssi );

	// Add service data bytes
	for ( int i = 0; i < serviceDataSize; i++ )
	{
		RAW_APPEND( "%s%u", i > 0 ? "," : "", ( unsigned )serviceData[ i ] );
	}

	RAW_APPEND( "],\"manufacturerData\":[" );

	// Add manufacturer data bytes
	for ( int i = 0; i < manufacturerDataSize; i++ )
	{
		RAW_APPEND( "%s%u", i > 0 ? "," : "", ( unsigned )manufacturerData[ i ] );
	}

	RAW_APPEND( "]}" );
#undef RAW_APPEND

	// Send to all connected SSE clients
	rawPacketEvents.send( sharedRespBuf, "packet", millis() );
	IncrementVolatileU32( DiagRawPacketsSent );
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
	char message[ 1024 ];
	int pos = 0;
	pos += snprintf( message + pos, sizeof( message ) - pos, "File Not Found\n\nURI: %s\nMethod: %s\nArguments: %u\n",
		request->url().c_str(),
		( request->method() == HTTP_GET ) ? "GET" : "POST",
		( unsigned int )request->args() );

	for ( uint8_t i = 0; i < request->args() && i < 20; i++ )  // limit to 20 args to prevent overflow
	{
		pos += snprintf( message + pos, sizeof( message ) - pos, " %s: %s\n",
			request->argName( i ).c_str(),
			request->arg( i ).c_str() );
		if ( pos >= ( int )sizeof( message ) - 100 ) break;  // stop if getting close to buffer end
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
		IncrementVolatileU32( DiagScanOnResultCalls );
		LastAdvertSeenAtMs = millis();
		const uint8_t* serviceDataBuf = nullptr;
		uint8_t serviceDataLen = 0;
		const uint8_t* manufacturerDataBuf = nullptr;
		uint8_t manufacturerDataLen = 0;
		if ( ExtractSwitchBotAdvData( advertisedDevice, &serviceDataBuf, &serviceDataLen,
		                             &manufacturerDataBuf, &manufacturerDataLen ) )
		{
			const NimBLEAddress& address = advertisedDevice->getAddress();
			char deviceAddress[ 18 ];
			FormatBleAddress( address, deviceAddress, sizeof( deviceAddress ) );
			if ( serviceDataLen == 0 )
			{
				NumMatchedEmptyPayload++;
				return;
			}
			NumMatchedServiceData++;
			bool dataUpdated = false;
			bool failedValidation = false;
			bool unknownType = false;

			// Always stream raw packet data if this device matches the hot MAC,
			// regardless of whether AddDevice succeeds or fails validation
			int rssi = advertisedDevice->getRSSI();
			streamRawPacketData( deviceAddress, rssi, serviceDataBuf, serviceDataLen, manufacturerDataBuf, manufacturerDataLen );

			if ( BLE_Devices.AddDevice( deviceAddress, rssi, address.getType(), ( uint8_t* )serviceDataBuf, serviceDataLen, ( uint8_t* )manufacturerDataBuf, manufacturerDataLen, &dataUpdated, &failedValidation, &unknownType ) )
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
					if ( serviceDataBuf[ 0 ] == 0 && serviceDataLen >= 7 )
					{
						RecordUnknownType( serviceDataBuf[ 0 ], deviceAddress, true, serviceDataBuf[ 4 ], serviceDataBuf[ 5 ], serviceDataBuf[ 6 ] );
					}
					else
					{
						RecordUnknownType( serviceDataBuf[ 0 ], deviceAddress );
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
		if ( ScanPausedByUser )
		{
			Serial.printf( "Scan ended reason = %d; paused by user, not restarting\n", reason );
			return;
		}

		if ( BLECommandConnectInProgress )
		{
			Serial.printf( "Scan ended reason = %d; connect in progress, delaying scan restart\n", reason );
			return;
		}

		uint32_t now = millis();
		if ( ( uint32_t )( now - LastScanRestartAttemptAtMs ) < SCAN_RESTART_COOLDOWN_MS )
		{
			IncrementVolatileU32( DiagScanRestartSuppressedCount );
			return;
		}

		Serial.printf( "Scan ended reason = %d; restarting scan\n", reason );
		LastScanRestartAttemptAtMs = now;
		StartBleScanTracked( NimBLEDevice::getScan(), true );
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
	hotMACMutex = xSemaphoreCreateMutex();
	if ( hotMACMutex == nullptr )
	{
		Serial.println( "Failed to create hotMAC mutex" );
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

	server.on( "/update", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		request->send( 200, "text/html", OTA_UPDATE_HTML );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/devices/table", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		Serial.println( "Received request for devices table" );
		AsyncWebServerResponse* response = request->beginResponse( 200, "text/html", ( const uint8_t* )DEVICES_TABLE_HTML, sizeof( DEVICES_TABLE_HTML ) - 1 );
		response->addHeader( "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0" );
		request->send( response );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/stats/page", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		AsyncWebServerResponse* response = request->beginResponse( 200, "text/html", ( const uint8_t* )STATS_HTML, sizeof( STATS_HTML ) - 1 );
		response->addHeader( "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0" );
		response->addHeader( "Pragma", "no-cache" );
		response->addHeader( "Expires", "0" );
		request->send( response );
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

		char clientIP[ 64 ];
		FormatIpAddress( IPAddress( request->client()->getRemoteAddress() ), clientIP, sizeof( clientIP ) );
		Serial.printf( "API function %s not found from %s\n", request->url().c_str(), clientIP );

		request->send( 404, "text/plain", "Not found" );
	} );

	server.onRequestBody(
	    []( AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total ) {
		    if ( request->method() != HTTP_POST )
		    {
			    return;
		    }

		    const char* requestUrl = request->url().c_str();
		    if ( ( strcmp( requestUrl, "/api/v1/callback/add" ) != 0 ) &&
		         ( strcmp( requestUrl, "/api/v1/callback/remove" ) != 0 ) &&
		         ( strcmp( requestUrl, "/api/v1/device/write" ) != 0 ) )
		    {
			    // Leave unrelated POST routes (e.g. OTA /ota multipart upload)
			    // to their dedicated handlers.
			    return;
		    }

		    digitalWrite( led, 1 );

		    // Body can arrive in multiple chunks. Buffer it and parse only once complete.
		    if ( index == 0 )
		    {
			    if ( request->_tempObject != nullptr )
			    {
				    POST_BODY_BUFFER* oldBody = ( POST_BODY_BUFFER* )request->_tempObject;
				    delete[] oldBody->data;
				    delete oldBody;
				    request->_tempObject = nullptr;
			    }
			    POST_BODY_BUFFER* body = new POST_BODY_BUFFER();
			    if ( body != nullptr )
			    {
				    body->cap = total + 1;
				    body->len = 0;
				    body->data = new char[ body->cap ];
				    if ( body->data != nullptr )
				    {
					    body->data[ 0 ] = '\0';
					    request->_tempObject = body;
				    }
				    else
				    {
					    delete body;
				    }
			    }
		    }

		    if ( request->_tempObject == nullptr )
		    {
			    request->send( 500, "text/plain", "Internal Error" );
			    digitalWrite( led, 0 );
			    return;
		    }

		    POST_BODY_BUFFER* body = ( POST_BODY_BUFFER* )request->_tempObject;
		    if ( body->len + len >= body->cap )
		    {
			    request->send( 413, "text/plain", "Payload Too Large" );
			    delete[] body->data;
			    delete body;
			    request->_tempObject = nullptr;
			    digitalWrite( led, 0 );
			    return;
		    }
		    memcpy( body->data + body->len, data, len );
		    body->len += len;
		    body->data[ body->len ] = '\0';

		    if ( ( index + len ) < total )
		    {
			    digitalWrite( led, 0 );
			    return;
		    }

		    const uint8_t* payloadData = ( const uint8_t* )body->data;
		    size_t payloadLen = body->len;

		    if ( strcmp( requestUrl, "/api/v1/callback/add" ) == 0 )
		    {
			    Serial.println( "Received request for /api/v1/callback/add" );
			    char uri[ 512 ];
			    bool hasUri = TryExtractUriFromBody( payloadData, payloadLen, uri, sizeof( uri ) );
			    if ( !hasUri && request->hasArg( "uri" ) )
			    {
				    if ( CopyRequestArgToBuf( request, "uri", uri, sizeof( uri ) ) )
				    {
					    UrlDecodeInPlace( uri );
					    hasUri = true;
				    }
			    }
			    if ( hasUri )
			    {
				    lockCallbacks();
				    bool addOk = OurCallbacks.Add( uri, millis() );
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
				    Serial.printf( "Callback error 400: no uri found. payloadLen=%u args=%u payload='%s'\n",
				                   ( unsigned int )payloadLen,
				                   request->args(),
				                   ( const char* )payloadData );
			    }
		    }
		    else if ( strcmp( requestUrl, "/api/v1/callback/remove" ) == 0 )
		    {
			    Serial.println( "Received request for /api/v1/callback/remove" );
			    char uri[ 512 ];
			    bool hasUri = TryExtractUriFromBody( payloadData, payloadLen, uri, sizeof( uri ) );
			    if ( !hasUri && request->hasArg( "uri" ) )
			    {
				    if ( CopyRequestArgToBuf( request, "uri", uri, sizeof( uri ) ) )
				    {
					    UrlDecodeInPlace( uri );
					    hasUri = true;
				    }
			    }
			    if ( hasUri )
			    {
				    lockCallbacks();
				    bool removeOk = OurCallbacks.Remove( uri );
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
		    else if ( strcmp( requestUrl, "/api/v1/device/write" ) == 0 )
		    {
			    char sourceIP[ 64 ];
			    FormatIpAddress( IPAddress( request->client()->getRemoteAddress() ), sourceIP, sizeof( sourceIP ) );
			    Serial.printf( "Received /api/v1/device/write from %s payloadLen=%u payload=%s\n",
			                   sourceIP,
			                   ( unsigned int )payloadLen,
			                   ( const char* )payloadData );
			    char clientAddress[ 32 ];
			    clientAddress[ 0 ] = '\0';
			    char dataToWrite[ 128 ];
			    dataToWrite[ 0 ] = '\0';
			    uint8_t commandData[ sizeof( BLE_COMMAND::Data ) ];
			    uint8_t commandDataLen = 0;
			    bool hasAddress = TryGetJsonStringFieldIntoBuf( payloadData, payloadLen, "address", clientAddress, sizeof( clientAddress ) );
			    if ( !hasAddress )
				    hasAddress = TryGetJsonStringFieldIntoBuf( payloadData, payloadLen, "Address", clientAddress, sizeof( clientAddress ) );
			    if ( !hasAddress )
				    hasAddress = TryGetJsonStringFieldIntoBuf( payloadData, payloadLen, "mac", clientAddress, sizeof( clientAddress ) );
			    if ( !hasAddress )
				    hasAddress = TryGetJsonStringFieldIntoBuf( payloadData, payloadLen, "MAC", clientAddress, sizeof( clientAddress ) );
			    if ( !hasAddress && request->hasArg( "address" ) )
			    {
				    hasAddress = CopyRequestArgToBuf( request, "address", clientAddress, sizeof( clientAddress ) );
			    }

			    bool hasData = TryParseJsonByteArrayField( payloadData, payloadLen, "data", commandData, sizeof( commandData ), commandDataLen );
			    if ( !hasData )
				    hasData = TryParseJsonByteArrayField( payloadData, payloadLen, "Data", commandData, sizeof( commandData ), commandDataLen );
			    if ( !hasData )
				    hasData = TryParseJsonByteArrayField( payloadData, payloadLen, "payload", commandData, sizeof( commandData ), commandDataLen );
			    if ( !hasData )
				    hasData = TryParseJsonByteArrayField( payloadData, payloadLen, "commandData", commandData, sizeof( commandData ), commandDataLen );
			    if ( !hasData && request->hasArg( "data" ) )
			    {
				    char argData[ 112 ];
				    if ( CopyRequestArgToBuf( request, "data", argData, sizeof( argData ) ) )
				    {
					    if ( argData[ 0 ] == '[' )
					    {
						    strncpy( dataToWrite, argData, sizeof( dataToWrite ) - 1 );
						    dataToWrite[ sizeof( dataToWrite ) - 1 ] = '\0';
					    }
					    else
					    {
						    snprintf( dataToWrite, sizeof( dataToWrite ), "[%s]", argData );
					    }
					    hasData = ParseByteListString( dataToWrite, commandData, sizeof( commandData ), commandDataLen );
				    }
			    }
			    bool parsedData = hasData;
			    bool hasNonEmptyAddress = ( clientAddress[ 0 ] != '\0' );
			    Serial.printf( "Parsed /api/v1/device/write from %s hasAddress=%d address=%s hasData=%d data=%s parsedData=%d dataLen=%u\n",
			                   sourceIP,
			                   hasAddress ? 1 : 0,
			                   clientAddress,
			                   hasData ? 1 : 0,
			                   dataToWrite,
			                   parsedData ? 1 : 0,
			                   ( unsigned int )commandDataLen );

			    if ( hasAddress && hasData && parsedData && hasNonEmptyAddress )
			    {
				    // Check that we have seen that device
				    int deviceIdx = BLE_Devices.FindDevice( clientAddress );
				    if ( deviceIdx >= 0 )
				    {
					    Serial.printf( "Received request to write device %s with %s (%u) from %s\n", clientAddress, dataToWrite, ( unsigned int )strlen( dataToWrite ), sourceIP );

					    if ( BLECommandQ.Find( clientAddress, commandData, commandDataLen ) )
					    {
						    // Same command already queued
						    request->send( 200, "text/plain", "OK" );
						    Serial.println( "Command already in the Q" );
						    RecordBleCommandRequest( sourceIP, clientAddress, dataToWrite, "already-queued" );
					    }
					    else if ( BLECommandQ.Push( clientAddress, commandData, commandDataLen, sourceIP ) )
					    {
						    request->send( 200, "text/plain", "OK" );
						    RecordBleCommandRequest( sourceIP, clientAddress, dataToWrite, "queued" );
					    }
					    else
					    {
						    int pos = 0;
						    pos += snprintf( sharedRespBuf + pos, sizeof( sharedRespBuf ) - pos, "{\"message\":\"Too Many Requests\",\"error\":\"QueueFull\",\"address\":\"" );
						    pos += JsonEscapeInto( sharedRespBuf + pos, sizeof( sharedRespBuf ) - pos, clientAddress );
						    pos += snprintf( sharedRespBuf + pos, sizeof( sharedRespBuf ) - pos, "\"}" );
						    request->send( 429, "application/json", sharedRespBuf );
						    Serial.println( "I have too much in my command Q" );
						    RecordBleCommandRequest( sourceIP, clientAddress, dataToWrite, "queue-full" );
					    }
				    }
				    else
				    {
					    int pos = 0;
					    pos += snprintf( sharedRespBuf + pos, sizeof( sharedRespBuf ) - pos, "{\"message\":\"Unknown device\",\"error\":\"UnknownDevice\",\"address\":\"" );
					    pos += JsonEscapeInto( sharedRespBuf + pos, sizeof( sharedRespBuf ) - pos, clientAddress );
					    pos += snprintf( sharedRespBuf + pos, sizeof( sharedRespBuf ) - pos, "\"}" );
					    request->send( 422, "application/json", sharedRespBuf );
					    Serial.printf( "Received request to write device %s but I have not seen that device)\n", clientAddress );
					    RecordBleCommandRequest( sourceIP, clientAddress, dataToWrite, "unknown-device" );
				    }
			    }
			    else
			    {
				    char details[ 64 ] = "";
				    if ( !hasAddress )
				    {
					    strcpy( details, "missing-address" );
				    }
				    else if ( !hasNonEmptyAddress )
				    {
					    strcpy( details, "empty-address" );
				    }

				    if ( !hasData )
				    {
					    if ( details[ 0 ] != '\0' )
					    {
						    strncat( details, ",", sizeof( details ) - strlen( details ) - 1 );
					    }
					    strncat( details, "missing-data", sizeof( details ) - strlen( details ) - 1 );
				    }
				    else if ( !parsedData )
				    {
					    if ( details[ 0 ] != '\0' )
					    {
						    strncat( details, ",", sizeof( details ) - strlen( details ) - 1 );
					    }
					    strncat( details, "invalid-data", sizeof( details ) - strlen( details ) - 1 );
				    }

				    int pos = 0;
				    pos += snprintf( sharedRespBuf + pos, sizeof( sharedRespBuf ) - pos, "{\"message\":\"Bad Request\",\"error\":\"InvalidPayload\",\"details\":\"" );
				    pos += JsonEscapeInto( sharedRespBuf + pos, sizeof( sharedRespBuf ) - pos, details );
				    pos += snprintf( sharedRespBuf + pos, sizeof( sharedRespBuf ) - pos, "\",\"expected\":{\"address\":\"AA:BB:CC:DD:EE:FF\",\"data\":[87,15,71,1,5,0,255,85]}}" );
				    request->send( 400, "application/json", sharedRespBuf );
				    char badReqResult[ 128 ];
				    snprintf( badReqResult, sizeof( badReqResult ), "bad-request:%s", details );
				    RecordBleCommandRequest( sourceIP, clientAddress, dataToWrite, badReqResult );
			    }
		    }

		    // Cleanup buffered POST body after all parsing is complete.
		    delete[] body->data;
		    delete body;
		    request->_tempObject = nullptr;

		    digitalWrite( led, 0 );
	    } );

	server.on( "/api/v1/devices", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		IncrementVolatileU32( DiagDevicesRequests );

		Serial.println( "Received request for devices" );

		// Decide whether the caller wants raw JSON (e.g. API clients that send
		// Accept: application/json, or no Accept header at all) or a pretty HTML
		// page (browser). Browsers always include text/html in their Accept header.
		bool wantJson = true;
		if ( request->hasHeader( "Accept" ) )
		{
			const char* accept = request->getHeader( "Accept" )->value().c_str();
			// Only serve the HTML page if the caller explicitly wants text/html
			// and has NOT asked for application/json
			wantJson = ( strstr( accept, "text/html" ) == nullptr ||
			             strstr( accept, "application/json" ) != nullptr );
		}

		if ( wantJson )
		{
			BLE_Devices.AllToJson( sharedRespBuf, sizeof( sharedRespBuf ), false, macAddress );
			request->send( 200, "application/json", sharedRespBuf );
		}
		else
		{
			request->send( 200, "text/html", DEVICES_JSON_HTML );
		}
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/stats/free-heap-history", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		int pos = 0;
		int rem = ( int )sizeof( sharedRespBuf );
#define HIST_APPEND( ... )                                                                      \
	do                                                                                          \
	{                                                                                           \
		RespBufAppendf( sharedRespBuf, ( int )sizeof( sharedRespBuf ), pos, rem, __VA_ARGS__ ); \
	} while ( 0 )
		HIST_APPEND( "{\"intervalMs\":%lu,\"maxPoints\":%u,\"count\":%u,\"values\":[",
		             ( unsigned long )STATS_SAMPLE_MS, ( unsigned )FREE_HEAP_HISTORY_MAX, ( unsigned )FreeHeapHistoryCount );
		for ( uint16_t i = 0; i < FreeHeapHistoryCount; i++ )
		{
			const uint16_t idx = ( FreeHeapHistoryStart + i ) % FREE_HEAP_HISTORY_MAX;
			HIST_APPEND( "%s%lu", i > 0 ? "," : "", ( unsigned long )FreeHeapHistory[ idx ] );
		}
		HIST_APPEND( "]}" );
#undef HIST_APPEND
		request->send( 200, "application/json", sharedRespBuf );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/stats/adverts-history", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		int pos = 0;
		int rem = ( int )sizeof( sharedRespBuf );
#define HIST_APPEND( ... )                                                                      \
	do                                                                                          \
	{                                                                                           \
		RespBufAppendf( sharedRespBuf, ( int )sizeof( sharedRespBuf ), pos, rem, __VA_ARGS__ ); \
	} while ( 0 )
		HIST_APPEND( "{\"intervalMs\":%lu,\"maxPoints\":%u,\"count\":%u,\"values\":[",
		             ( unsigned long )STATS_SAMPLE_MS, ( unsigned )FREE_HEAP_HISTORY_MAX, ( unsigned )BleRateHistoryCount );
		for ( uint16_t i = 0; i < BleRateHistoryCount; i++ )
		{
			const uint16_t idx = ( BleRateHistoryStart + i ) % FREE_HEAP_HISTORY_MAX;
			HIST_APPEND( "%s%ld", i > 0 ? "," : "", ( long )AdvertsPerMinuteHistory[ idx ] );
		}
		HIST_APPEND( "]}" );
#undef HIST_APPEND
		request->send( 200, "application/json", sharedRespBuf );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/stats/matches-history", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		int pos = 0;
		int rem = ( int )sizeof( sharedRespBuf );
#define HIST_APPEND( ... )                                                                      \
	do                                                                                          \
	{                                                                                           \
		RespBufAppendf( sharedRespBuf, ( int )sizeof( sharedRespBuf ), pos, rem, __VA_ARGS__ ); \
	} while ( 0 )
		HIST_APPEND( "{\"intervalMs\":%lu,\"maxPoints\":%u,\"count\":%u,\"values\":[",
		             ( unsigned long )STATS_SAMPLE_MS, ( unsigned )FREE_HEAP_HISTORY_MAX, ( unsigned )BleRateHistoryCount );
		for ( uint16_t i = 0; i < BleRateHistoryCount; i++ )
		{
			const uint16_t idx = ( BleRateHistoryStart + i ) % FREE_HEAP_HISTORY_MAX;
			HIST_APPEND( "%s%ld", i > 0 ? "," : "", ( long )MatchesPerMinuteHistory[ idx ] );
		}
		HIST_APPEND( "]}" );
#undef HIST_APPEND
		request->send( 200, "application/json", sharedRespBuf );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/stats/actual-updates-history", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		int pos = 0;
		int rem = ( int )sizeof( sharedRespBuf );
#define HIST_APPEND( ... )                                                                      \
	do                                                                                          \
	{                                                                                           \
		RespBufAppendf( sharedRespBuf, ( int )sizeof( sharedRespBuf ), pos, rem, __VA_ARGS__ ); \
	} while ( 0 )
		HIST_APPEND( "{\"intervalMs\":%lu,\"maxPoints\":%u,\"count\":%u,\"values\":[",
		             ( unsigned long )STATS_SAMPLE_MS, ( unsigned )FREE_HEAP_HISTORY_MAX, ( unsigned )BleRateHistoryCount );
		for ( uint16_t i = 0; i < BleRateHistoryCount; i++ )
		{
			const uint16_t idx = ( BleRateHistoryStart + i ) % FREE_HEAP_HISTORY_MAX;
			HIST_APPEND( "%s%ld", i > 0 ? "," : "", ( long )ActualUpdatesPerMinuteHistory[ idx ] );
		}
		HIST_APPEND( "]}" );
#undef HIST_APPEND

		request->send( 200, "application/json", sharedRespBuf );
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

		int pos = 0;
		int rem = ( int )sizeof( sharedRespBuf );
#define MON_APPEND( ... )                                                                       \
	do                                                                                          \
	{                                                                                           \
		RespBufAppendf( sharedRespBuf, ( int )sizeof( sharedRespBuf ), pos, rem, __VA_ARGS__ ); \
	} while ( 0 )
#define MON_ESC( str )                                            \
	do                                                            \
	{                                                             \
		int _n = JsonEscapeInto( sharedRespBuf + pos, rem, str ); \
		pos += _n;                                                \
		rem -= _n;                                                \
	} while ( 0 )

		MON_APPEND( "{\"registered\":[" );
		char uriBuf[ 256 ];
		uint8_t callbackIndex = 0;
		bool callbackAdded = false;
		while ( true )
		{
			lockCallbacks();
			bool gotCallback = OurCallbacks.Get( callbackIndex, uriBuf, sizeof( uriBuf ) );
			unlockCallbacks();
			if ( !gotCallback )
				break;
			char ipBuf[ 64 ] = { 0 };
			ExtractIpFromUri( uriBuf, ipBuf, sizeof( ipBuf ) );
			if ( callbackAdded )
				MON_APPEND( "," );
			MON_APPEND( "{\"uri\":\"" );
			MON_ESC( uriBuf );
			MON_APPEND( "\",\"ip\":\"" );
			MON_ESC( ipBuf );
			MON_APPEND( "\"}" );
			callbackAdded = true;
			callbackIndex++;
		}
		MON_APPEND( "],\"pushUpdates\":[" );
		lockHistory();
		for ( int i = PushUpdateHistoryCount - 1; i >= 0; i-- )
		{
			uint8_t idx = ( PushUpdateHistoryStart + i ) % HOMEY_HISTORY_MAX;
			if ( i != PushUpdateHistoryCount - 1 )
				MON_APPEND( "," );
			MON_APPEND( "{\"atMs\":%lu,\"payload\":\"", ( unsigned long )PushUpdateHistory[ idx ].atMs );
			MON_ESC( PushUpdateHistory[ idx ].payload );
			MON_APPEND( "\",\"ip\":\"" );
			MON_ESC( PushUpdateHistory[ idx ].ip );
			MON_APPEND( "\",\"bytes\":%d,\"httpCode\":%d}", PushUpdateHistory[ idx ].bytes, PushUpdateHistory[ idx ].httpCode );
		}
		unlockHistory();
		MON_APPEND( "],\"bleCommands\":[" );
		lockHistory();
		for ( int i = BleCommandHistoryCount - 1; i >= 0; i-- )
		{
			uint8_t idx = ( BleCommandHistoryStart + i ) % HOMEY_HISTORY_MAX;
			if ( i != BleCommandHistoryCount - 1 )
				MON_APPEND( "," );
			MON_APPEND( "{\"atMs\":%lu,\"sourceIp\":\"", ( unsigned long )BleCommandHistory[ idx ].atMs );
			MON_ESC( BleCommandHistory[ idx ].sourceIp );
			MON_APPEND( "\",\"address\":\"" );
			MON_ESC( BleCommandHistory[ idx ].address );
			MON_APPEND( "\",\"data\":\"" );
			MON_ESC( BleCommandHistory[ idx ].data );
			MON_APPEND( "\",\"result\":\"" );
			MON_ESC( BleCommandHistory[ idx ].result );
			MON_APPEND( "\"}" );
		}
		unlockHistory();
		MON_APPEND( "],\"uptimeMs\":%lu,\"nextUpdateAtMs\":%lu,\"bleCommandSeq\":%lu,\"pushUpdateSeq\":%lu}",
		            ( unsigned long )millis(), ( unsigned long )( LastStatsAt + STATS_SAMPLE_MS ),
		            ( unsigned long )BleCommandSeq, ( unsigned long )PushUpdateSeq );
#undef MON_APPEND
#undef MON_ESC

		request->send( 200, "application/json", sharedRespBuf );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/homey/pushseq", HTTP_GET, []( AsyncWebServerRequest* request ) {
		char buf[ 32 ];
		snprintf( buf, sizeof( buf ), "{\"seq\":%lu}", ( unsigned long )PushUpdateSeq );
		request->send( 200, "application/json", buf );
	} );

	server.on( "/api/v1/homey/bleseq", HTTP_GET, []( AsyncWebServerRequest* request ) {
		char buf[ 32 ];
		snprintf( buf, sizeof( buf ), "{\"seq\":%lu}", ( unsigned long )BleCommandSeq );
		request->send( 200, "application/json", buf );
	} );

	server.on( "/api/v1/stats", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		IncrementVolatileU32( DiagStatsRequests );
		BLEScan* pBLEScan = BLEDevice::getScan();
		bool bleScanning = ( pBLEScan != nullptr ) ? pBLEScan->isScanning() : false;

		int pos = 0;
		int rem = ( int )sizeof( sharedRespBuf );
#define STATS_APPEND( ... )                                                                     \
	do                                                                                           \
	{                                                                                            \
		RespBufAppendf( sharedRespBuf, ( int )sizeof( sharedRespBuf ), pos, rem, __VA_ARGS__ ); \
	} while ( 0 )

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
		STATS_APPEND( ",\"noAdvertMinutes\":%lu", ( unsigned long )( NumZeroAdvertIntervals / ( int32_t )STATS_PER_MINUTE_SCALE ) );
		STATS_APPEND( ",\"lastAdvertSeenAtMs\":%lu", ( unsigned long )LastAdvertSeenAtMs );
		STATS_APPEND( ",\"lastForcedScanRecoveryAtMs\":%lu", ( unsigned long )LastForcedScanRecoveryAtMs );
		STATS_APPEND( ",\"lastSuccessfulScanRestartAtMs\":%lu", ( unsigned long )LastSuccessfulScanRestartAtMs );
		STATS_APPEND( ",\"lastMemoryScanRecoveryAtMs\":%lu", ( unsigned long )LastMemoryScanRecoveryAtMs );
		STATS_APPEND( ",\"freeHeap\":%lu", ( unsigned long )LastFreeHeap );
		STATS_APPEND( ",\"largestHeapBlock\":%lu", ( unsigned long )LastLargestHeapBlock );
		STATS_APPEND( ",\"lastStatsAtMs\":%lu", ( unsigned long )LastStatsAt );
		STATS_APPEND( ",\"statsIntervalMs\":%lu", ( unsigned long )STATS_SAMPLE_MS );
		STATS_APPEND( ",\"cpuUsage\":%u", ( unsigned )LastCpuUsagePercent );
		STATS_APPEND( ",\"registeredDevices\":%u", ( unsigned )BLE_Devices.GetNumberOfDevices() );
		STATS_APPEND( ",\"bleScanning\":%s", bleScanning ? "true" : "false" );
		STATS_APPEND( ",\"scanPausedByUser\":%s", ScanPausedByUser ? "true" : "false" );
		STATS_APPEND( ",\"diagStatsRequests\":%lu", ( unsigned long )DiagStatsRequests );
		STATS_APPEND( ",\"diagDevicesRequests\":%lu", ( unsigned long )DiagDevicesRequests );
		STATS_APPEND( ",\"diagRawPacketsBuilt\":%lu", ( unsigned long )DiagRawPacketsBuilt );
		STATS_APPEND( ",\"diagRawPacketsSent\":%lu", ( unsigned long )DiagRawPacketsSent );
		STATS_APPEND( ",\"diagSseBleSent\":%lu", ( unsigned long )DiagSseBleSent );
		STATS_APPEND( ",\"diagSseStatsSent\":%lu", ( unsigned long )DiagSseStatsSent );
		STATS_APPEND( ",\"diagScanOnResultCalls\":%lu", ( unsigned long )DiagScanOnResultCalls );
		STATS_APPEND( ",\"diagScanRestartCount\":%lu", ( unsigned long )DiagScanRestartCount );
		STATS_APPEND( ",\"diagScanRestartSuppressedCount\":%lu", ( unsigned long )DiagScanRestartSuppressedCount );
		STATS_APPEND( ",\"diagScanStaleRecoveryCount\":%lu", ( unsigned long )DiagScanStaleRecoveryCount );
		STATS_APPEND( ",\"diagScanMemoryRecoveryCount\":%lu", ( unsigned long )DiagScanMemoryRecoveryCount );
		STATS_APPEND( ",\"diagDiscoveryPacketsRx\":%lu", ( unsigned long )DiagDiscoveryPacketsRx );
		STATS_APPEND( ",\"diagDiscoveryQueryRx\":%lu", ( unsigned long )DiagDiscoveryQueryRx );
		STATS_APPEND( ",\"diagDiscoveryAnnouncementsTx\":%lu", ( unsigned long )DiagDiscoveryAnnouncementsTx );
		STATS_APPEND( ",\"diagPushPostCount\":%lu", ( unsigned long )DiagPushPostCount );
		STATS_APPEND( ",\"diagPushPostErrorCount\":%lu", ( unsigned long )DiagPushPostErrorCount );
		STATS_APPEND( ",\"unknownTypes\":[" );

		lockHistory();
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
		unlockHistory();

		STATS_APPEND( "]}" );
#undef STATS_APPEND

		AsyncWebServerResponse* response = request->beginResponse( 200, "application/json", sharedRespBuf );
		response->addHeader( "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0" );
		response->addHeader( "Pragma", "no-cache" );
		response->addHeader( "Expires", "0" );
		request->send( response );

		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/scan/control", HTTP_POST, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );

		char action[ 16 ];
		action[ 0 ] = '\0';
		CopyRequestArgToBuf( request, "action", action, sizeof( action ), true, true );

		BLEScan* pBLEScan = BLEDevice::getScan();
		if ( pBLEScan == nullptr )
		{
			request->send( 500, "application/json", "{\"ok\":false,\"error\":\"scan-unavailable\"}" );
			digitalWrite( led, 0 );
			return;
		}

		if ( strcmp( action, "stop" ) == 0 )
		{
			ScanPausedByUser = true;
			pBLEScan->stop();
			for ( int i = 0; i < 25 && pBLEScan->isScanning(); i++ )
			{
				delay( 20 );
			}
		}
		else if ( strcmp( action, "start" ) == 0 )
		{
			ScanPausedByUser = false;
			if ( !BLECommandConnectInProgress && !otaInProgress && !pBLEScan->isScanning() )
			{
				StartBleScanTracked( pBLEScan, true );
				IncrementVolatileU32( DiagScanRestartCount );
				LastForcedScanRecoveryAtMs = millis();
			}
		}
		else
		{
			request->send( 400, "application/json", "{\"ok\":false,\"error\":\"invalid-action\"}" );
			digitalWrite( led, 0 );
			return;
		}

		char out[ 128 ];
		snprintf( out, sizeof( out ),
		          "{\"ok\":true,\"action\":\"%s\",\"bleScanning\":%s,\"scanPausedByUser\":%s}",
		          action,
		          pBLEScan->isScanning() ? "true" : "false",
		          ScanPausedByUser ? "true" : "false" );
		request->send( 200, "application/json", out );

		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/device/watch", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		char address[ 18 ];
		address[ 0 ] = '\0';
		CopyRequestArgToBuf( request, "address", address, sizeof( address ) );
		Serial.printf( "Setting hot MAC to: %s\n", address );

		if ( hotMACMutex != nullptr )
		{
			xSemaphoreTake( hotMACMutex, portMAX_DELAY );
			strncpy( hotMAC, address, sizeof( hotMAC ) - 1 );
			hotMAC[ sizeof( hotMAC ) - 1 ] = '\0';
			hotMACLeaseExpiresAtMs = ( hotMAC[ 0 ] != '\0' ) ? ( millis() + HOT_MAC_LEASE_MS ) : 0;
			xSemaphoreGive( hotMACMutex );
		}

		char out[ 96 ];
		snprintf( out, sizeof( out ), "{\"ok\":true,\"address\":\"%s\"}", address );
		request->send( 200, "application/json", out );
		digitalWrite( led, 0 );
	} );

	rawPacketEvents.onConnect( []( AsyncEventSourceClient* client ) {
		Serial.println( "Raw packet stream client connected" );
		client->send( "connected", "init", millis() );
	} );

	server.addHandler( &rawPacketEvents );

	server.on( "/api/v1/device/viewer", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		AsyncWebServerResponse* response = request->beginResponse( 200, "text/html", ( const uint8_t* )RAW_PACKET_VIEWER_HTML, sizeof( RAW_PACKET_VIEWER_HTML ) - 1 );
		response->addHeader( "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0" );
		response->addHeader( "Pragma", "no-cache" );
		response->addHeader( "Expires", "0" );
		request->send( response );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/device", HTTP_GET, []( AsyncWebServerRequest* request ) {
		digitalWrite( led, 1 );
		char address[ 18 ];
		address[ 0 ] = '\0';
		CopyRequestArgToBuf( request, "address", address, sizeof( address ) );
		Serial.printf( "Received request for device: %s\n", address );

		int deviceIdx = BLE_Devices.FindDevice( address );
		BLE_Devices.DeviceToJson( deviceIdx, sharedRespBuf, sizeof( sharedRespBuf ), macAddress );
		request->send( 200, "application/json", sharedRespBuf );
		digitalWrite( led, 0 );
	} );

	server.on( "/api/v1/ota/prepare", HTTP_POST, []( AsyncWebServerRequest* request ) {
		if ( !otaInProgress )
		{
			otaInProgress = true;
			BLEScan* pBLEScan = BLEDevice::getScan();
			pBLEScan->stop();
			Serial.println( "BLE scan stopped for OTA upload" );
		}
		request->send( 200, "application/json", "{\"ok\":true}" );
	} );

	server.on( "/api/v1/ota/cancel", HTTP_POST, []( AsyncWebServerRequest* request ) {
		if ( otaInProgress )
		{
			otaInProgress = false;
			BLEScan* pBLEScan = BLEDevice::getScan();
			if ( !ScanPausedByUser )
			{
				StartBleScanTracked( pBLEScan, true );
				LastForcedScanRecoveryAtMs = millis();
				IncrementVolatileU32( DiagScanRestartCount );
				Serial.println( "BLE scan restarted after OTA cancel/failure" );
			}
			else
			{
				Serial.println( "BLE scan remains paused by user after OTA cancel/failure" );
			}
		}
		request->send( 200, "application/json", "{\"ok\":true}" );
	} );

	//	server.onNotFound( handleNotFound );

	_updateServer.setup( &server, "/ota" );

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
	StartBleScanTracked( pBLEScan, false );

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
			while ( true )
				;
		}

		if ( pendingSSEUpdate && ( millis() - lastSSESend >= 1000 ) )
		{
			pendingSSEUpdate = false;
			lastSSESend = millis();
			bleEvents.send( "update", "ble", millis() );
			IncrementVolatileU32( DiagSseBleSent );
		}

		if ( pendingSSEStats && ( millis() - lastSSEStatsSend >= 1000 ) )
		{
			pendingSSEStats = false;
			lastSSEStatsSend = millis();
			bleEvents.send( "update", "stats", millis() );
			IncrementVolatileU32( DiagSseStatsSent );
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

			if ( LastAdvertsSeenPerMinute == 0 )
			{
				NumZeroAdvertIntervals++;
			}
			else
			{
				NumZeroAdvertIntervals = 0;
			}

			if ( NumUpdatesAt0 > 3 && !otaInProgress && !ScanPausedByUser )
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
			if ( largestHeapBlock < 10000 )
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

		// Scan health-check and stuck-scan recovery.
		if ( !BLECommandConnectInProgress && !otaInProgress && !ScanPausedByUser )
		{
			BLEScan* pBLEScan = BLEDevice::getScan();
			if ( !pBLEScan->isScanning() )
			{
				const uint32_t now = millis();
				if ( ( uint32_t )( now - LastScanRestartAttemptAtMs ) >= SCAN_RESTART_COOLDOWN_MS )
				{
					Serial.println( "BLE scan not running - restarting" );
					LastScanRestartAttemptAtMs = now;
					StartBleScanTracked( pBLEScan, true );
					IncrementVolatileU32( DiagScanRestartCount );
					LastForcedScanRecoveryAtMs = now;
					// Reset zero-update counters so the watchdog doesn't fire on this glitch
					NumZeroUpdateIntervals = 0;
					NumUpdatesAt0 = 0;
					NumZeroAdvertIntervals = 0;
				}
				else
				{
					IncrementVolatileU32( DiagScanRestartSuppressedCount );
				}
			}
			else
			{
				const uint32_t now = millis();
				const bool memoryFragmented = ( LastLargestHeapBlock > 0 ) &&
				                             ( LastLargestHeapBlock < SCAN_MEMORY_RECOVERY_LARGEST_BLOCK_THRESHOLD ) &&
				                             ( ( uint32_t )( now - LastMemoryScanRecoveryAtMs ) > SCAN_MEMORY_RECOVERY_COOLDOWN_MS ) &&
				                             ( ( uint32_t )( now - LastScanRestartAttemptAtMs ) > SCAN_RESTART_COOLDOWN_MS );

				if ( memoryFragmented )
				{
					Serial.printf( "BLE scan memory recovery (largest block %lu < %lu) - cycling scanner\n",
					               ( unsigned long )LastLargestHeapBlock,
					               ( unsigned long )SCAN_MEMORY_RECOVERY_LARGEST_BLOCK_THRESHOLD );
					pBLEScan->stop();
					for ( int i = 0; i < 25 && pBLEScan->isScanning(); i++ )
					{
						delay( 20 );
					}
					delay( 30 );
					LastScanRestartAttemptAtMs = now;
					StartBleScanTracked( pBLEScan, true );
					IncrementVolatileU32( DiagScanRestartCount );
					IncrementVolatileU32( DiagScanMemoryRecoveryCount );
					LastMemoryScanRecoveryAtMs = now;
					LastForcedScanRecoveryAtMs = now;
				}

				const bool staleNoAdverts = ( NumZeroAdvertIntervals >= 2 ) &&
				                            ( ( uint32_t )( now - LastAdvertSeenAtMs ) > ( STATS_SAMPLE_MS * 2 ) ) &&
				                            ( ( uint32_t )( now - LastForcedScanRecoveryAtMs ) > 10000 );
				if ( staleNoAdverts )
				{
					Serial.printf( "BLE scan stale (%lu ms since advert) - force cycling scanner\n", ( unsigned long )( now - LastAdvertSeenAtMs ) );
					pBLEScan->stop();
					for ( int i = 0; i < 25 && pBLEScan->isScanning(); i++ )
					{
						delay( 20 );
					}
					delay( 30 );
					LastScanRestartAttemptAtMs = now;
					StartBleScanTracked( pBLEScan, true );
					IncrementVolatileU32( DiagScanRestartCount );
					IncrementVolatileU32( DiagScanStaleRecoveryCount );
					LastForcedScanRecoveryAtMs = now;
					NumZeroAdvertIntervals = 0;
				}
			}
		}

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
			IncrementVolatileU32( DiagPushPostCount );
			if ( httpCode <= 0 )
			{
				IncrementVolatileU32( DiagPushPostErrorCount );
			}
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

	// Use only the cached address type captured during scan.
	const uint8_t connectAddrType = addrType;

	BLECommandConnectInProgress = true;
	pBLEScan->stop();
	for ( int i = 0; i < 25 && pBLEScan->isScanning(); i++ )
	{
		delay( 20 );
	}
	delay( 50 );

	{
		bool complete = false;
		int retries = 5;
		int attemptNumber = 0;

		while ( !complete && ( retries-- > 0 ) )
		{
			attemptNumber++;
			NimBLEClient* pBLEClient = NimBLEDevice::createClient();
			if ( pBLEClient == nullptr )
			{
				Serial.println( "Failed to create BLE client" );
				finalResult = "error-client-create";
				delay( 120 );
				continue;
			}

			pBLEClient->setConnectTimeout( 12 * 1000 );
			// Tune connection establishment scan parameters for better reliability.
			pBLEClient->setConnectionParams( 24, 48, 0, 600, 48, 48 );
			NimBLEAddress bleAddress;
			if ( !ParseMacAddressToNimble( BLECommand->Address, connectAddrType, bleAddress ) )
			{
				Serial.printf( "Invalid BLE MAC address: %s\n", BLECommand->Address );
				finalResult = "error-invalid-address";
				NimBLEDevice::deleteClient( pBLEClient );
				delay( 120 );
				continue;
			}
			bool connected = pBLEClient->connect( bleAddress, true, false, false );
			int connectErr = pBLEClient->getLastError();
			if ( !connected )
			{
				if ( connectErr == 2 )
				{
					bool cancelled = pBLEClient->cancelConnect();
					delay( 900 );
				}
			}

			if ( connected )
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
							while ( ( BLENotifyLength == 0 ) && ( millis() < endTime ) )
								;
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
				int lastErr = connectErr;
				finalResult = "error-connect";

				// Recovery strategy for connect timeout/busy states.
				// 13 = BLE_HS_ETIMEOUT, 2 = BLE_HS_EALREADY.
				if ( ( lastErr == 13 ) && ( retries > 0 ) )
				{
					pBLEScan->start( 2, false, true );
					delay( 2200 );
					pBLEScan->stop();
					for ( int i = 0; i < 25 && pBLEScan->isScanning(); i++ )
					{
						delay( 20 );
					}
				}

				delay( ( lastErr == 2 ) ? 700 : 250 );
			}

			NimBLEDevice::deleteClient( pBLEClient );
			if ( !complete )
			{
				delay( 180 );
			}
		}
	}

	UpdateBleCommandResult( BLECommand->ReplyTo, BLECommand->Address, commandDataText, finalResult );

	Serial.println( "Restarting BLE scan" );
	BLECommandConnectInProgress = false;
	StartBleScanTracked( pBLEScan, true );
	LastForcedScanRecoveryAtMs = millis();
	BLESending = millis() + 1000;
}
