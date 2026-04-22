#include "TETHEliteBoard.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(MESH_ETHERNET_WEB)

#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <ETH.h>
#include <FS.h>
#include <SPI.h>
#include <Update.h>
#include <WiFi.h>

namespace {

constexpr size_t CONSOLE_HISTORY_LINES = 48;
constexpr size_t CONSOLE_LINE_BUFFER = 256;
constexpr size_t OTA_ID_BUFFER = 80;
constexpr size_t RAW_TCP_MAX_CLIENTS = 4;
constexpr size_t RAW_TCP_INPUT_BUFFER = 160;

constexpr unsigned long NETWORK_RELOAD_DELAY_MS = 250;
constexpr unsigned long WEBSERIAL_IDLE_TIMEOUT_MS = 600000;
constexpr unsigned long WEBSERIAL_RECONNECT_DELAY_MS = 2000;
constexpr uint16_t WEBSERIAL_KEEPALIVE_SECONDS = 30;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 120000;
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 15000;

constexpr const char* NETWORK_CONFIG_FILE = "/network.cfg";
constexpr const char* FALLBACK_AP_SSID = "meshcore";
constexpr const char* FALLBACK_AP_HOST = "meshcore";
constexpr const char* FALLBACK_AP_DOMAIN = "meshcore.local";
constexpr uint16_t RAW_TCP_PORT = 4403;

const IPAddress FALLBACK_AP_IP(192, 168, 4, 1);
const IPAddress FALLBACK_AP_GATEWAY(192, 168, 4, 1);
const IPAddress FALLBACK_AP_SUBNET(255, 255, 255, 0);

const char* WEB_CONSOLE_HTML = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>MeshCore WebSerial</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f4f1e8;
      --panel: #fffaf0;
      --ink: #1d1f1c;
      --accent: #0f766e;
      --line: #d7d0c2;
    }
    body {
      margin: 0;
      padding: 24px;
      font-family: "IBM Plex Mono", "Courier New", monospace;
      background:
        radial-gradient(circle at top right, rgba(15, 118, 110, 0.12), transparent 28%),
        linear-gradient(180deg, #f7f3ea 0%, var(--bg) 100%);
      color: var(--ink);
    }
    main {
      max-width: 980px;
      margin: 0 auto;
      display: grid;
      gap: 16px;
    }
    .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 16px;
      padding: 18px;
      box-shadow: 0 18px 45px rgba(29, 31, 28, 0.08);
    }
    h1 {
      margin: 0 0 8px;
      font-size: 1.4rem;
    }
    p {
      margin: 0;
      line-height: 1.5;
    }
    #status {
      font-size: 0.95rem;
      color: #4b5563;
    }
    #console {
      min-height: 420px;
      max-height: 60vh;
      overflow: auto;
      white-space: pre-wrap;
      line-height: 1.5;
      background: #1a1d1a;
      color: #f1f5f2;
      border-radius: 14px;
      padding: 16px;
      border: 1px solid #27322d;
    }
    form {
      display: grid;
      grid-template-columns: 1fr auto;
      gap: 10px;
    }
    input {
      width: 100%;
      padding: 12px 14px;
      border-radius: 12px;
      border: 1px solid var(--line);
      background: #fff;
      color: var(--ink);
      font: inherit;
    }
    button, a.button {
      appearance: none;
      border: 0;
      border-radius: 12px;
      padding: 12px 16px;
      background: var(--accent);
      color: #fff;
      font: inherit;
      cursor: pointer;
      text-decoration: none;
      display: inline-flex;
      align-items: center;
      justify-content: center;
    }
    .actions {
      display: flex;
      gap: 10px;
      flex-wrap: wrap;
    }
    @media (max-width: 640px) {
      body {
        padding: 14px;
      }
      form {
        grid-template-columns: 1fr;
      }
    }
  </style>
</head>
<body>
  <main>
    <section class="panel">
      <h1>MeshCore WebSerial</h1>
      <p id="status">Connecting...</p>
    </section>
    <section class="panel">
      <div id="console"></div>
    </section>
    <section class="panel">
      <form id="command-form">
        <input id="command" autocomplete="off" spellcheck="false" placeholder="Enter a MeshCore CLI command">
        <button type="submit">Send</button>
      </form>
    </section>
    <section class="panel actions">
      <a class="button" href="/network">Network Settings</a>
      <a class="button" href="/log" target="_blank" rel="noopener">Packet Log</a>
      <a class="button" href="/update" target="_blank" rel="noopener">Firmware Update</a>
      <button id="reconnect" type="button">Reconnect</button>
      <button id="clear" type="button">Clear Console</button>
    </section>
  </main>
  <script>
    const consoleEl = document.getElementById("console");
    const statusEl = document.getElementById("status");
    const inputEl = document.getElementById("command");
    const formEl = document.getElementById("command-form");
    const reconnectEl = document.getElementById("reconnect");
    const clearEl = document.getElementById("clear");
    const IDLE_TIMEOUT_MS = 600000;
    const RECONNECT_DELAY_MS = 2000;

    let socket = null;
    let reconnectTimer = 0;
    let idleTimer = 0;
    let idleTimedOut = false;

    function appendLine(line) {
      consoleEl.textContent += line.endsWith("\n") ? line : line + "\n";
      consoleEl.scrollTop = consoleEl.scrollHeight;
    }

    function clearConsole() {
      consoleEl.textContent = "";
    }

    function cancelReconnect() {
      if (reconnectTimer) {
        window.clearTimeout(reconnectTimer);
        reconnectTimer = 0;
      }
    }

    function scheduleIdleTimeout() {
      window.clearTimeout(idleTimer);
      idleTimer = window.setTimeout(() => {
        idleTimedOut = true;
        clearConsole();
        appendLine("[webserial timed out after 10 minutes of inactivity]");
        statusEl.textContent = "Timed out";
        reconnectEl.disabled = false;
        if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) {
          socket.close(1000, "idle timeout");
        }
      }, IDLE_TIMEOUT_MS);
    }

    function markUserActivity() {
      idleTimedOut = false;
      reconnectEl.disabled = true;
      scheduleIdleTimeout();
    }

    async function refreshStatus() {
      try {
        const response = await fetch("/api/status", { cache: "no-store" });
        const status = await response.json();
        statusEl.textContent =
          `${status.node_name} | ${status.role} | primary ${status.ip} via ${status.link} | ` +
          `eth ${status.eth_ip} (${status.eth_link}) | wifi ${status.wifi_state} ${status.wifi_ip}`;
      } catch (error) {
        statusEl.textContent = "Status unavailable";
      }
    }

    function connectSocket(clearFirst = false) {
      if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING)) {
        return;
      }

      cancelReconnect();
      if (clearFirst) {
        clearConsole();
      }

      statusEl.textContent = "Connecting...";
      const scheme = location.protocol === "https:" ? "wss" : "ws";
      socket = new WebSocket(`${scheme}://${location.host}/ws`);

      socket.addEventListener("open", () => {
        reconnectEl.disabled = true;
        appendLine("[webserial connected]");
        refreshStatus();
        if (!idleTimedOut) {
          scheduleIdleTimeout();
        }
      });

      socket.addEventListener("message", (event) => {
        appendLine(event.data);
      });

      socket.addEventListener("close", () => {
        socket = null;
        reconnectEl.disabled = false;
        if (idleTimedOut) {
          statusEl.textContent = "Timed out";
          return;
        }

        appendLine("[webserial disconnected]");
        statusEl.textContent = "Disconnected, reconnecting...";
        reconnectTimer = window.setTimeout(() => {
          reconnectTimer = 0;
          connectSocket(true);
        }, RECONNECT_DELAY_MS);
      });

      socket.addEventListener("error", () => {
        if (socket && socket.readyState === WebSocket.OPEN) {
          socket.close();
        }
      });
    }

    formEl.addEventListener("submit", (event) => {
      event.preventDefault();
      const command = inputEl.value.trim();
      markUserActivity();
      if (!command) {
        return;
      }
      if (!socket || socket.readyState !== WebSocket.OPEN) {
        connectSocket(true);
        return;
      }
      socket.send(command);
      inputEl.value = "";
    });

    reconnectEl.addEventListener("click", () => {
      markUserActivity();
      connectSocket(true);
    });

    clearEl.addEventListener("click", () => {
      markUserActivity();
      clearConsole();
    });

    document.addEventListener("keydown", markUserActivity);
    document.addEventListener("mousedown", markUserActivity);
    document.addEventListener("touchstart", markUserActivity, { passive: true });
    window.addEventListener("focus", () => {
      if (!idleTimedOut && (!socket || socket.readyState === WebSocket.CLOSED)) {
        connectSocket(true);
      }
    });

    reconnectEl.disabled = true;
    markUserActivity();
    connectSocket(true);
    setInterval(refreshStatus, 5000);
  </script>
</body>
</html>
)HTML";

const char* OTA_PAGE_HTML = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>MeshCore OTA</title>
  <style>
    body { font-family: monospace; background: #f5efe3; color: #1d1f1c; padding: 24px; }
    main { max-width: 640px; margin: 0 auto; background: #fffaf0; border: 1px solid #d7d0c2; border-radius: 16px; padding: 24px; }
    input, button { font: inherit; }
    form { display: grid; gap: 12px; }
    button { padding: 12px 16px; border: 0; border-radius: 12px; background: #0f766e; color: white; cursor: pointer; }
  </style>
</head>
<body>
  <main>
    <h1>MeshCore Firmware Update</h1>
    <p>Upload a firmware binary for the T-ETH Elite.</p>
    <form method="POST" action="/update" enctype="multipart/form-data">
      <input type="file" name="update" required>
      <button type="submit">Upload</button>
    </form>
  </main>
</body>
</html>
)HTML";

struct SavedNetworkConfig {
  bool eth_dhcp = true;
  String eth_ip;
  String eth_gateway;
  String eth_subnet = "255.255.255.0";
  String eth_dns1;
  String eth_dns2;

  bool wifi_enabled = true;
  String wifi_ssid;
  String wifi_password;
  bool wifi_dhcp = true;
  String wifi_ip;
  String wifi_gateway;
  String wifi_subnet = "255.255.255.0";
  String wifi_dns1;
  String wifi_dns2;

  bool wifiConfigured() const {
    return wifi_ssid.length() > 0;
  }
};

TETHEliteBoard* active_board = nullptr;

}  // namespace

class TETHEliteBoardState {
public:
  fs::FS* fs = nullptr;
  AsyncWebServer* server = nullptr;
  AsyncWebSocket* websocket = nullptr;
  AsyncServer* raw_tcp_server = nullptr;
  TETHEliteCommandHandler handler = nullptr;

  String node_name = "MeshCore";
  String role = "headless";
  String node_id = "-";
  String hostname;

  SavedNetworkConfig config;
  bool config_loaded = false;
  bool event_handler_registered = false;

  bool network_reload_requested = false;
  unsigned long network_reload_at = 0;

  bool ethernet_requested = false;
  bool ethernet_started = false;
  bool ethernet_connected = false;
  bool ethernet_has_ip = false;
  bool web_started = false;
  bool reported_eth_ready = false;
  bool reported_eth_disconnected = false;
  IPAddress last_reported_eth_ip;

  bool wifi_sta_requested = false;
  bool wifi_connected = false;
  bool wifi_has_ip = false;
  bool wifi_ap_running = false;
  bool fallback_dns_active = false;
  bool fallback_mdns_active = false;
  bool reported_wifi_ready = false;
  bool reported_wifi_waiting = false;
  bool reported_wifi_ap = false;
  unsigned long wifi_connect_deadline = 0;
  unsigned long wifi_last_reconnect_at = 0;
  IPAddress last_reported_wifi_ip;

  String console_history[CONSOLE_HISTORY_LINES];
  size_t console_head = 0;
  size_t console_count = 0;
  AsyncClient* raw_tcp_clients[RAW_TCP_MAX_CLIENTS] = {};
  char raw_tcp_input[RAW_TCP_MAX_CLIENTS][RAW_TCP_INPUT_BUFFER + 1] = {};
  size_t raw_tcp_input_len[RAW_TCP_MAX_CLIENTS] = {};
  bool raw_tcp_started = false;
};

namespace {

static TETHEliteBoardState state;
static DNSServer fallback_dns_server;

static String makeDefaultHostname() {
  uint64_t mac = ESP.getEfuseMac();
  char host[40];
  snprintf(host, sizeof(host), "meshcore-teth-%06X", (uint32_t)(mac & 0xFFFFFF));
  return String(host);
}

static String jsonEscape(const String& input) {
  String output;
  output.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    switch (c) {
      case '\\':
      case '"':
        output += '\\';
        output += c;
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      default:
        output += c;
        break;
    }
  }
  return output;
}

static String htmlEscape(const String& input) {
  String output;
  output.reserve(input.length() + 16);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    switch (c) {
      case '&':
        output += "&amp;";
        break;
      case '<':
        output += "&lt;";
        break;
      case '>':
        output += "&gt;";
        break;
      case '"':
        output += "&quot;";
        break;
      case '\'':
        output += "&#39;";
        break;
      default:
        output += c;
        break;
    }
  }
  return output;
}

static String escapeConfigValue(const String& input) {
  String output;
  output.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    switch (c) {
      case '\\':
        output += "\\\\";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output += c;
        break;
    }
  }
  return output;
}

static String unescapeConfigValue(const String& input) {
  String output;
  output.reserve(input.length());
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if (c == '\\' && i + 1 < input.length()) {
      char next = input[++i];
      switch (next) {
        case 'n':
          output += '\n';
          break;
        case 'r':
          output += '\r';
          break;
        case 't':
          output += '\t';
          break;
        case '\\':
          output += '\\';
          break;
        default:
          output += next;
          break;
      }
    } else {
      output += c;
    }
  }
  return output;
}

static bool parseBool(const String& value, bool default_value) {
  String normalized = value;
  normalized.trim();
  normalized.toLowerCase();
  if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on") {
    return true;
  }
  if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off") {
    return false;
  }
  return default_value;
}

static String sanitizeSingleLine(const String& input, bool trim_spaces) {
  String value = input;
  value.replace("\r", "");
  value.replace("\n", "");
  if (trim_spaces) {
    value.trim();
  }
  return value;
}

static bool parseOptionalIp(const String& text, IPAddress& out) {
  String value = sanitizeSingleLine(text, true);
  if (value.length() == 0) {
    out = IPAddress();
    return true;
  }
  return out.fromString(value);
}

static bool parseStaticIpConfig(const String& ip_text, const String& gateway_text, const String& subnet_text,
                                const String& dns1_text, const String& dns2_text, IPAddress& ip,
                                IPAddress& gateway, IPAddress& subnet, IPAddress& dns1, IPAddress& dns2,
                                String& error) {
  if (!parseOptionalIp(ip_text, ip) || ip == IPAddress()) {
    error = "Static IP address is invalid.";
    return false;
  }
  if (!parseOptionalIp(gateway_text, gateway) || gateway == IPAddress()) {
    error = "Gateway address is invalid.";
    return false;
  }
  if (!parseOptionalIp(subnet_text, subnet) || subnet == IPAddress()) {
    error = "Subnet mask is invalid.";
    return false;
  }
  if (!parseOptionalIp(dns1_text, dns1)) {
    error = "Primary DNS address is invalid.";
    return false;
  }
  if (!parseOptionalIp(dns2_text, dns2)) {
    error = "Secondary DNS address is invalid.";
    return false;
  }
  return true;
}

static String getFallbackApPassword() {
#if defined(ADMIN_PASSWORD)
  String password = ADMIN_PASSWORD;
#else
  String password = "meshcore";
#endif
  if (password.length() < 8) {
    password = "meshcore";
  }
  return password;
}

static String getEthernetIpString() {
  if (!state.ethernet_has_ip) {
    return "pending";
  }
  return ETH.localIP().toString();
}

static String getWiFiIpString() {
  if (state.wifi_has_ip) {
    return WiFi.localIP().toString();
  }
  if (state.wifi_ap_running) {
    return WiFi.softAPIP().toString();
  }
  return "pending";
}

static String getPrimaryAccessIpString() {
  if (state.ethernet_has_ip) {
    return ETH.localIP().toString();
  }
  if (state.wifi_has_ip) {
    return WiFi.localIP().toString();
  }
  if (state.wifi_ap_running) {
    return WiFi.softAPIP().toString();
  }
  return "pending";
}

static String getPrimaryAccessLinkString() {
  if (state.ethernet_has_ip) {
    return "eth";
  }
  if (state.wifi_has_ip) {
    return "wifi";
  }
  if (state.wifi_ap_running) {
    return "ap";
  }
  return "down";
}

static String getWiFiStateString() {
  if (!state.config.wifi_enabled) {
    return "disabled";
  }
  if (state.wifi_has_ip) {
    return "connected";
  }
  if (state.wifi_ap_running && state.config.wifiConfigured()) {
    return "fallback-ap";
  }
  if (state.wifi_ap_running) {
    return "setup-ap";
  }
  if (state.config.wifiConfigured()) {
    return "connecting";
  }
  return "waiting-setup";
}

static String getWiFiTargetName() {
  if (!state.config.wifi_enabled) {
    return "-";
  }
  if (state.wifi_has_ip) {
    return WiFi.SSID();
  }
  if (state.config.wifiConfigured()) {
    return state.config.wifi_ssid;
  }
  return FALLBACK_AP_SSID;
}

static String getFallbackApIpString() {
  if (!state.wifi_ap_running) {
    return FALLBACK_AP_IP.toString();
  }
  return WiFi.softAPIP().toString();
}

static String getFallbackApUrlString() {
  return String("http://") + FALLBACK_AP_DOMAIN + "/";
}

static String getFallbackApSummaryString() {
  return getFallbackApUrlString() + " (" + getFallbackApIpString() + ")";
}

static String getRawTcpUrlString() {
  String ip = getPrimaryAccessIpString();
  if (ip == "pending") {
    return "socket://<ip>:" + String(RAW_TCP_PORT);
  }
  return "socket://" + ip + ":" + String(RAW_TCP_PORT);
}

static void setConfigValue(const String& key, const String& value) {
  if (key == "eth_dhcp") {
    state.config.eth_dhcp = parseBool(value, true);
  } else if (key == "eth_ip") {
    state.config.eth_ip = value;
  } else if (key == "eth_gateway") {
    state.config.eth_gateway = value;
  } else if (key == "eth_subnet") {
    state.config.eth_subnet = value;
  } else if (key == "eth_dns1") {
    state.config.eth_dns1 = value;
  } else if (key == "eth_dns2") {
    state.config.eth_dns2 = value;
  } else if (key == "wifi_enabled") {
    state.config.wifi_enabled = parseBool(value, true);
  } else if (key == "wifi_ssid") {
    state.config.wifi_ssid = value;
  } else if (key == "wifi_password") {
    state.config.wifi_password = value;
  } else if (key == "wifi_dhcp") {
    state.config.wifi_dhcp = parseBool(value, true);
  } else if (key == "wifi_ip") {
    state.config.wifi_ip = value;
  } else if (key == "wifi_gateway") {
    state.config.wifi_gateway = value;
  } else if (key == "wifi_subnet") {
    state.config.wifi_subnet = value;
  } else if (key == "wifi_dns1") {
    state.config.wifi_dns1 = value;
  } else if (key == "wifi_dns2") {
    state.config.wifi_dns2 = value;
  }
}

static void loadNetworkConfig() {
  state.config = SavedNetworkConfig();
  state.config_loaded = true;

  if (state.fs == nullptr) {
    return;
  }

  File file = state.fs->open(NETWORK_CONFIG_FILE, "r");
  if (!file) {
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    if (line.endsWith("\r")) {
      line.remove(line.length() - 1);
    }
    if (line.length() == 0 || line[0] == '#') {
      continue;
    }

    int separator = line.indexOf('\t');
    if (separator < 0) {
      separator = line.indexOf('=');
    }
    if (separator <= 0) {
      continue;
    }

    String key = line.substring(0, separator);
    key.trim();
    String value = unescapeConfigValue(line.substring(separator + 1));
    setConfigValue(key, value);
  }

  file.close();
}

static bool writeConfigLine(File& file, const char* key, const String& value) {
  return file.print(key) && file.print('\t') && file.println(escapeConfigValue(value));
}

static bool saveNetworkConfig() {
  if (state.fs == nullptr) {
    return false;
  }

  File file = state.fs->open(NETWORK_CONFIG_FILE, "w");
  if (!file) {
    return false;
  }

  bool ok = true;
  ok &= writeConfigLine(file, "eth_dhcp", state.config.eth_dhcp ? "1" : "0");
  ok &= writeConfigLine(file, "eth_ip", state.config.eth_ip);
  ok &= writeConfigLine(file, "eth_gateway", state.config.eth_gateway);
  ok &= writeConfigLine(file, "eth_subnet", state.config.eth_subnet);
  ok &= writeConfigLine(file, "eth_dns1", state.config.eth_dns1);
  ok &= writeConfigLine(file, "eth_dns2", state.config.eth_dns2);
  ok &= writeConfigLine(file, "wifi_enabled", state.config.wifi_enabled ? "1" : "0");
  ok &= writeConfigLine(file, "wifi_ssid", state.config.wifi_ssid);
  ok &= writeConfigLine(file, "wifi_password", state.config.wifi_password);
  ok &= writeConfigLine(file, "wifi_dhcp", state.config.wifi_dhcp ? "1" : "0");
  ok &= writeConfigLine(file, "wifi_ip", state.config.wifi_ip);
  ok &= writeConfigLine(file, "wifi_gateway", state.config.wifi_gateway);
  ok &= writeConfigLine(file, "wifi_subnet", state.config.wifi_subnet);
  ok &= writeConfigLine(file, "wifi_dns1", state.config.wifi_dns1);
  ok &= writeConfigLine(file, "wifi_dns2", state.config.wifi_dns2);
  file.close();
  return ok;
}

static String buildStatusJson() {
  String json = "{";
  json += "\"node_name\":\"" + jsonEscape(state.node_name) + "\",";
  json += "\"role\":\"" + jsonEscape(state.role) + "\",";
  json += "\"node_id\":\"" + jsonEscape(state.node_id) + "\",";
  json += "\"hostname\":\"" + jsonEscape(state.hostname) + "\",";
  json += "\"ip\":\"" + jsonEscape(getPrimaryAccessIpString()) + "\",";
  json += "\"link\":\"" + jsonEscape(getPrimaryAccessLinkString()) + "\",";
  json += "\"eth_ip\":\"" + jsonEscape(getEthernetIpString()) + "\",";
  json += "\"eth_link\":\"";
  json += state.ethernet_connected ? "up" : "down";
  json += "\",";
  json += "\"eth_mode\":\"";
  json += state.config.eth_dhcp ? "dhcp" : "static";
  json += "\",";
  json += "\"wifi_ip\":\"" + jsonEscape(getWiFiIpString()) + "\",";
  json += "\"wifi_state\":\"" + jsonEscape(getWiFiStateString()) + "\",";
  json += "\"wifi_target\":\"" + jsonEscape(getWiFiTargetName()) + "\",";
  json += "\"raw_tcp_url\":\"" + jsonEscape(getRawTcpUrlString()) + "\"";
  json += "}";
  return json;
}

static void appendTextInput(String& page, const char* label, const char* name, const String& value,
                            const char* placeholder, const char* type = "text") {
  page += "<label><span>";
  page += label;
  page += "</span><input type=\"";
  page += type;
  page += "\" name=\"";
  page += name;
  page += "\" value=\"";
  page += htmlEscape(value);
  page += "\" placeholder=\"";
  page += placeholder;
  page += "\"></label>";
}

static String buildHomePage() {
  String page;
  page.reserve(3200);
  page += "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">";
  page += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  page += "<title>MeshCore T-ETH Elite</title>";
  page += "<style>";
  page += "body{margin:0;padding:24px;font-family:\"IBM Plex Mono\",\"Courier New\",monospace;background:radial-gradient(circle at top right,rgba(15,118,110,.12),transparent 28%),linear-gradient(180deg,#f7f3ea 0%,#f4f1e8 100%);color:#1d1f1c;}";
  page += "main{max-width:980px;margin:0 auto;display:grid;gap:16px;}";
  page += "section{background:#fffaf0;border:1px solid #d7d0c2;border-radius:16px;padding:18px;box-shadow:0 18px 45px rgba(29,31,28,.08);}";
  page += "h1,h2{margin:0 0 10px;}p{margin:0;line-height:1.5;}pre{margin:0;background:#1a1d1a;color:#f1f5f2;padding:16px;border-radius:14px;overflow:auto;}";
  page += ".actions{display:flex;gap:10px;flex-wrap:wrap;}.button{appearance:none;border:0;border-radius:12px;padding:12px 16px;background:#0f766e;color:#fff;font:inherit;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;justify-content:center;}.button.subtle{background:#475569;}";
  page += "@media (max-width:640px){body{padding:14px;}}";
  page += "</style></head><body><main>";
  page += "<section>";
  page += "<h1>MeshCore T-ETH Elite</h1>";
  page += "<p>Ethernet-enabled repeater board with local WebSerial and network setup.</p>";
  page += "</section>";
  page += "<section>";
  page += "<h2>Status</h2>";
  page += "<pre>";
  page += "Name: " + state.node_name + "\n";
  page += "Role: " + state.role + "\n";
  page += "Node ID: " + state.node_id + "\n";
  page += "Hostname: " + state.hostname + "\n";
  page += "Ethernet: ";
  page += state.ethernet_connected ? "link up" : "link down";
  page += " (" + String(state.config.eth_dhcp ? "dhcp" : "static") + ")";
  page += "\nEthernet IP: " + getEthernetIpString() + "\n";
  page += "WiFi: " + getWiFiStateString();
  page += "\nWiFi Target: " + getWiFiTargetName() + "\n";
  page += "WiFi IP: " + getWiFiIpString() + "\n";
  page += "Fallback URL: " + getFallbackApSummaryString() + "\n";
  page += "Raw TCP: " + getRawTcpUrlString() + "\n";
  page += "</pre>";
  page += "</section>";
  page += "<section class=\"actions\">";
  page += "<a class=\"button\" href=\"/webserial\">Open WebSerial</a>";
  page += "<a class=\"button\" href=\"/network\">Network Settings</a>";
  page += "<a class=\"button subtle\" href=\"/log\">Packet Log</a>";
  page += "<a class=\"button subtle\" href=\"/update\">Firmware Update</a>";
  page += "</section>";
  page += "</main></body></html>";
  return page;
}

static String buildNetworkConfigPage(const String& notice, bool is_error) {
  String page;
  page.reserve(7000);
  page += "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">";
  page += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  page += "<title>MeshCore Network Settings</title>";
  page += "<style>";
  page += "body{margin:0;padding:24px;font-family:\"IBM Plex Mono\",\"Courier New\",monospace;background:radial-gradient(circle at top right,rgba(15,118,110,.12),transparent 28%),linear-gradient(180deg,#f7f3ea 0%,#f4f1e8 100%);color:#1d1f1c;}";
  page += "main{max-width:980px;margin:0 auto;display:grid;gap:16px;}";
  page += "section{background:#fffaf0;border:1px solid #d7d0c2;border-radius:16px;padding:18px;box-shadow:0 18px 45px rgba(29,31,28,.08);}";
  page += "h1,h2{margin:0 0 10px;}p{margin:0;line-height:1.5;}pre{margin:0;background:#1a1d1a;color:#f1f5f2;padding:16px;border-radius:14px;overflow:auto;}";
  page += "form{display:grid;gap:16px;}fieldset{border:1px solid #d7d0c2;border-radius:14px;padding:16px;display:grid;gap:12px;}legend{padding:0 8px;font-weight:700;}";
  page += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}";
  page += "label{display:grid;gap:6px;}label>span{font-size:.9rem;color:#4b5563;}input,select{width:100%;padding:11px 12px;border-radius:12px;border:1px solid #d7d0c2;background:#fff;color:#1d1f1c;font:inherit;box-sizing:border-box;}";
  page += ".toggle{display:flex;align-items:center;gap:10px;padding:12px 14px;border:1px solid #d7d0c2;border-radius:12px;background:#fffdf6;}.toggle input{width:auto;margin:0;}";
  page += ".notice{padding:12px 14px;border-radius:12px;border:1px solid ";
  page += is_error ? "#d9485f;background:#fff1f2;color:#9f1239;" : "#0f766e;background:#ecfdf5;color:#115e59;";
  page += "}.actions{display:flex;gap:10px;flex-wrap:wrap;}.button,button{appearance:none;border:0;border-radius:12px;padding:12px 16px;background:#0f766e;color:#fff;font:inherit;cursor:pointer;text-decoration:none;display:inline-flex;align-items:center;justify-content:center;}.subtle{font-size:.9rem;color:#4b5563;}";
  page += "@media (max-width:640px){body{padding:14px;}}";
  page += "</style></head><body><main>";
  page += "<section><h1>Network Settings</h1><p>Configure Ethernet and Wi-Fi.</p></section>";

  if (notice.length() > 0) {
    page += "<section class=\"notice\">";
    page += htmlEscape(notice);
    page += "</section>";
  }

  page += "<section><h2>Current Status</h2><pre>";
  page += "Primary access: " + getPrimaryAccessIpString() + " via " + getPrimaryAccessLinkString() + "\n";
  page += "Ethernet: " + String(state.ethernet_connected ? "link up" : "link down") + " / " + getEthernetIpString() + "\n";
  page += "WiFi: " + getWiFiStateString() + " / " + getWiFiIpString() + "\n";
  page += "WiFi target: " + getWiFiTargetName() + "\n";
  page += "Fallback AP: " + String(FALLBACK_AP_SSID) + " / " + getFallbackApSummaryString() + "\n";
  page += "</pre></section>";

  page += "<section><form method=\"POST\" action=\"/network\">";
  page += "<fieldset><legend>Ethernet</legend><div class=\"grid\">";
  page += "<label><span>Mode</span><select name=\"eth_mode\">";
  page += "<option value=\"dhcp\"";
  if (state.config.eth_dhcp) {
    page += " selected";
  }
  page += ">DHCP</option><option value=\"static\"";
  if (!state.config.eth_dhcp) {
    page += " selected";
  }
  page += ">Static IPv4</option></select></label>";
  appendTextInput(page, "Static IP", "eth_ip", state.config.eth_ip, "192.168.1.50");
  appendTextInput(page, "Gateway", "eth_gateway", state.config.eth_gateway, "192.168.1.1");
  appendTextInput(page, "Subnet", "eth_subnet", state.config.eth_subnet, "255.255.255.0");
  appendTextInput(page, "Primary DNS", "eth_dns1", state.config.eth_dns1, "1.1.1.1");
  appendTextInput(page, "Secondary DNS", "eth_dns2", state.config.eth_dns2, "8.8.8.8");
  page += "</div><p class=\"subtle\">Leave Ethernet on DHCP unless you want the board fixed to a specific IPv4 address.</p></fieldset>";

  page += "<fieldset><legend>Wi-Fi</legend>";
  page += "<label class=\"toggle\"><input type=\"checkbox\" name=\"wifi_enabled\"";
  if (state.config.wifi_enabled) {
    page += " checked";
  }
  page += "><span>Enable Wi-Fi client and fallback AP behavior</span></label>";
  page += "<div class=\"grid\">";
  appendTextInput(page, "Wi-Fi SSID", "wifi_ssid", state.config.wifi_ssid, "YourNetwork");
  appendTextInput(page, "Wi-Fi Password", "wifi_password", state.config.wifi_password, "", "password");
  page += "<label><span>Mode</span><select name=\"wifi_mode\">";
  page += "<option value=\"dhcp\"";
  if (state.config.wifi_dhcp) {
    page += " selected";
  }
  page += ">DHCP</option><option value=\"static\"";
  if (!state.config.wifi_dhcp) {
    page += " selected";
  }
  page += ">Static IPv4</option></select></label>";
  appendTextInput(page, "Static IP", "wifi_ip", state.config.wifi_ip, "192.168.1.60");
  appendTextInput(page, "Gateway", "wifi_gateway", state.config.wifi_gateway, "192.168.1.1");
  appendTextInput(page, "Subnet", "wifi_subnet", state.config.wifi_subnet, "255.255.255.0");
  appendTextInput(page, "Primary DNS", "wifi_dns1", state.config.wifi_dns1, "1.1.1.1");
  appendTextInput(page, "Secondary DNS", "wifi_dns2", state.config.wifi_dns2, "8.8.8.8");
  page += "</div>";
  page += "<p class=\"subtle\">Blank SSID starts the <strong>meshcore</strong> fallback AP.</p>";
  page += "</fieldset>";

  page += "<div class=\"actions\"><button type=\"submit\">Save And Apply</button><a class=\"button\" href=\"/\">Back Home</a><a class=\"button\" href=\"/webserial\">WebSerial</a></div>";
  page += "</form></section></main></body></html>";
  return page;
}

static String buildNetworkSavedPage() {
  String page;
  page.reserve(1400);
  page += "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">";
  page += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  page += "<title>MeshCore Network Saved</title>";
  page += "<style>body{font-family:monospace;background:#f5efe3;color:#1d1f1c;padding:24px;}main{max-width:720px;margin:0 auto;background:#fffaf0;border:1px solid #d7d0c2;border-radius:16px;padding:24px;}a{color:#0f766e;}pre{background:#1a1d1a;color:#f1f5f2;padding:14px;border-radius:12px;overflow:auto;}</style></head><body><main>";
  page += "<h1>Network Settings Saved</h1>";
  page += "<p>The board is restarting its Ethernet and Wi-Fi services now. If an IP address changes, reconnect using the new address or join the <strong>meshcore</strong> Wi-Fi access point.</p>";
  page += "<pre>";
  page += "Primary access before reload: " + getPrimaryAccessIpString() + " via " + getPrimaryAccessLinkString() + "\n";
  page += "Fallback AP: meshcore / " + getFallbackApSummaryString() + "\n";
  page += "</pre>";
  page += "<p><a href=\"/\">Return home</a></p>";
  page += "</main></body></html>";
  return page;
}

static int findRawTcpClientSlot(AsyncClient* client) {
  if (client == nullptr) {
    return -1;
  }

  for (size_t i = 0; i < RAW_TCP_MAX_CLIENTS; ++i) {
    if (state.raw_tcp_clients[i] == client) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

static int reserveRawTcpClientSlot(AsyncClient* client) {
  int existing = findRawTcpClientSlot(client);
  if (existing >= 0) {
    return existing;
  }

  for (size_t i = 0; i < RAW_TCP_MAX_CLIENTS; ++i) {
    if (state.raw_tcp_clients[i] == nullptr) {
      state.raw_tcp_clients[i] = client;
      state.raw_tcp_input_len[i] = 0;
      state.raw_tcp_input[i][0] = 0;
      return static_cast<int>(i);
    }
  }
  return -1;
}

static void clearRawTcpClientSlot(AsyncClient* client) {
  int slot = findRawTcpClientSlot(client);
  if (slot < 0) {
    return;
  }

  state.raw_tcp_clients[slot] = nullptr;
  state.raw_tcp_input_len[slot] = 0;
  state.raw_tcp_input[slot][0] = 0;
}

static void writeRawTcpLine(AsyncClient* client, const char* line) {
  if (client == nullptr || !client->connected() || line == nullptr) {
    return;
  }

  size_t len = strlen(line);
  if (len > 0) {
    client->write(line, len);
  }
  client->write("\n", 1);
}

static void broadcastRawTcpLine(const String& line) {
  if (line.length() == 0) {
    return;
  }

  for (size_t i = 0; i < RAW_TCP_MAX_CLIENTS; ++i) {
    AsyncClient* client = state.raw_tcp_clients[i];
    if (client == nullptr) {
      continue;
    }
    if (!client->connected()) {
      clearRawTcpClientSlot(client);
      continue;
    }
    writeRawTcpLine(client, line.c_str());
  }
}

static void runBoardCommand(const char* command, char reply[]) {
  if (reply != nullptr) {
    reply[0] = 0;
  }

  if (state.handler != nullptr) {
    state.handler(const_cast<char*>(command), reply);
  } else if (reply != nullptr) {
    strcpy(reply, "No command handler configured");
  }
}

static void executeConsoleCommand(const char* command, bool echo_to_console, char reply[]) {
  if (command == nullptr || *command == 0) {
    if (reply != nullptr) {
      reply[0] = 0;
    }
    return;
  }

  if (echo_to_console && active_board != nullptr) {
    active_board->consolePrintf("> %s", command);
  }

  runBoardCommand(command, reply);

  if (echo_to_console && reply != nullptr && reply[0] != 0 && active_board != nullptr) {
    active_board->consolePrintf("< %s", reply);
  }
}

static void onRawTcpDisconnect(void* arg, AsyncClient* client) {
  (void)arg;
  clearRawTcpClientSlot(client);
  delete client;
}

static void onRawTcpError(void* arg, AsyncClient* client, int8_t error) {
  (void)arg;
  (void)error;
  clearRawTcpClientSlot(client);
}

static void onRawTcpTimeout(void* arg, AsyncClient* client, uint32_t time) {
  (void)arg;
  (void)time;
  if (client != nullptr) {
    client->close();
  }
}

static void onRawTcpData(void* arg, AsyncClient* client, void* data, size_t len) {
  (void)arg;
  if (client == nullptr || data == nullptr || len == 0) {
    return;
  }

  int slot = findRawTcpClientSlot(client);
  if (slot < 0) {
    return;
  }

  const char* bytes = reinterpret_cast<const char*>(data);
  for (size_t i = 0; i < len; ++i) {
    char c = bytes[i];
    if (c == '\r' || c == '\n') {
      if (state.raw_tcp_input_len[slot] == 0) {
        continue;
      }

      state.raw_tcp_input[slot][state.raw_tcp_input_len[slot]] = 0;
      char reply[160];
      executeConsoleCommand(state.raw_tcp_input[slot], false, reply);
      if (reply[0] != 0) {
        writeRawTcpLine(client, reply);
      }
      state.raw_tcp_input_len[slot] = 0;
      state.raw_tcp_input[slot][0] = 0;
      continue;
    }

    if (state.raw_tcp_input_len[slot] >= RAW_TCP_INPUT_BUFFER) {
      state.raw_tcp_input_len[slot] = 0;
      state.raw_tcp_input[slot][0] = 0;
      writeRawTcpLine(client, "Command too long");
      continue;
    }

    state.raw_tcp_input[slot][state.raw_tcp_input_len[slot]++] = c;
    state.raw_tcp_input[slot][state.raw_tcp_input_len[slot]] = 0;
  }
}

static void onRawTcpClient(void* arg, AsyncClient* client) {
  (void)arg;
  if (client == nullptr) {
    return;
  }

  int slot = reserveRawTcpClientSlot(client);
  client->setNoDelay(true);
  client->onDisconnect(onRawTcpDisconnect, nullptr);
  client->onError(onRawTcpError, nullptr);
  client->onTimeout(onRawTcpTimeout, nullptr);
  client->onData(onRawTcpData, nullptr);

  if (slot < 0) {
    writeRawTcpLine(client, "Busy");
    client->close();
  }
}

static void startRawTcpServer() {
  if (state.raw_tcp_started) {
    return;
  }

  state.raw_tcp_server = new AsyncServer(RAW_TCP_PORT);
  state.raw_tcp_server->onClient(onRawTcpClient, nullptr);
  state.raw_tcp_server->begin();
  state.raw_tcp_started = true;
}

static void rememberConsoleLine(const String& line) {
  if (line.length() == 0) {
    return;
  }

  state.console_history[state.console_head] = line;
  state.console_head = (state.console_head + 1) % CONSOLE_HISTORY_LINES;
  if (state.console_count < CONSOLE_HISTORY_LINES) {
    state.console_count++;
  }

  if (state.websocket != nullptr) {
    state.websocket->textAll(line);
  }
  broadcastRawTcpLine(line);
}

static void sendConsoleHistory(AsyncWebSocketClient* client) {
  if (client == nullptr) {
    return;
  }

  size_t start = (state.console_head + CONSOLE_HISTORY_LINES - state.console_count) % CONSOLE_HISTORY_LINES;
  for (size_t i = 0; i < state.console_count; ++i) {
    const String& line = state.console_history[(start + i) % CONSOLE_HISTORY_LINES];
    if (line.length() > 0) {
      client->text(line);
    }
  }
}

static void onConsoleEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                           AwsEventType type, void* arg, uint8_t* data, size_t len) {
  (void)server;

  if (type == WS_EVT_CONNECT) {
    client->setCloseClientOnQueueFull(false);
    client->keepAlivePeriod(WEBSERIAL_KEEPALIVE_SECONDS);
    sendConsoleHistory(client);
    return;
  }

  if (type != WS_EVT_DATA || client == nullptr || active_board == nullptr) {
    return;
  }

  AwsFrameInfo* info = reinterpret_cast<AwsFrameInfo*>(arg);
  if (info == nullptr || !info->final || info->index != 0 || info->opcode != WS_TEXT) {
    return;
  }

  char command[161];
  size_t copy_len = len < sizeof(command) - 1 ? len : sizeof(command) - 1;
  memcpy(command, data, copy_len);
  command[copy_len] = 0;

  String trimmed(command);
  trimmed.trim();
  if (trimmed.length() == 0) {
    return;
  }

  trimmed.toCharArray(command, sizeof(command));

  char reply[160];
  executeConsoleCommand(command, true, reply);
}

static void stopWiFiServices();
static void startWiFiSetupAp();
static void startConfiguredWiFi(bool keep_ap_running);
static void restartNetworkServices();

static void onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info) {
  (void)info;

  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      state.ethernet_started = true;
      ETH.setHostname(state.hostname.c_str());
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      state.ethernet_connected = true;
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      state.ethernet_connected = true;
      state.ethernet_has_ip = true;
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      state.ethernet_has_ip = false;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      state.ethernet_connected = false;
      state.ethernet_has_ip = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      state.ethernet_started = false;
      state.ethernet_connected = false;
      state.ethernet_has_ip = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      state.wifi_connected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      state.wifi_connected = true;
      state.wifi_has_ip = true;
      state.wifi_connect_deadline = 0;
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      state.wifi_has_ip = false;
      if (state.config.wifi_enabled && state.config.wifiConfigured()) {
        state.wifi_connect_deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      state.wifi_connected = false;
      state.wifi_has_ip = false;
      if (state.config.wifi_enabled && state.config.wifiConfigured()) {
        state.wifi_connect_deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
      }
      break;
    case ARDUINO_EVENT_WIFI_AP_START:
      state.wifi_ap_running = true;
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      state.wifi_ap_running = false;
      break;
    default:
      break;
  }
}

static void registerNetworkEvents() {
  if (state.event_handler_registered) {
    return;
  }
  WiFi.onEvent(onNetworkEvent);
  state.event_handler_registered = true;
}

static void scheduleNetworkReload() {
  state.network_reload_requested = true;
  state.network_reload_at = millis() + NETWORK_RELOAD_DELAY_MS;
}

static void logLine(const char* line) {
  Serial.println(line);
  rememberConsoleLine(line);
}

static void resetEthernetState() {
  state.ethernet_requested = false;
  state.ethernet_started = false;
  state.ethernet_connected = false;
  state.ethernet_has_ip = false;
  state.reported_eth_ready = false;
  state.reported_eth_disconnected = false;
  state.last_reported_eth_ip = IPAddress();
}

static void resetWiFiState() {
  state.wifi_sta_requested = false;
  state.wifi_connected = false;
  state.wifi_has_ip = false;
  state.wifi_ap_running = false;
  state.fallback_dns_active = false;
  state.fallback_mdns_active = false;
  state.reported_wifi_ready = false;
  state.reported_wifi_waiting = false;
  state.reported_wifi_ap = false;
  state.wifi_connect_deadline = 0;
  state.wifi_last_reconnect_at = 0;
  state.last_reported_wifi_ip = IPAddress();
}

static void stopFallbackDiscovery() {
  if (state.fallback_dns_active) {
    fallback_dns_server.stop();
    state.fallback_dns_active = false;
  }

  if (state.fallback_mdns_active) {
    MDNS.end();
    state.fallback_mdns_active = false;
  }
}

static void startFallbackDiscovery() {
  if (!state.wifi_ap_running) {
    return;
  }

  stopFallbackDiscovery();

  IPAddress ap_ip = WiFi.softAPIP();
  if (fallback_dns_server.start(DNS_DEFAULT_PORT, FALLBACK_AP_DOMAIN, ap_ip)) {
    state.fallback_dns_active = true;
  } else {
    logLine("Fallback DNS start failed");
  }

  if (MDNS.begin(FALLBACK_AP_HOST)) {
    state.fallback_mdns_active = true;
    MDNS.setInstanceName(state.node_name);
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "path", "/");
  } else {
    logLine("Fallback mDNS start failed");
  }

  char line[CONSOLE_LINE_BUFFER];
  snprintf(line, sizeof(line), "Fallback access: http://%s/  (%s)",
           FALLBACK_AP_DOMAIN, ap_ip.toString().c_str());
  logLine(line);
}

static void stopEthernetService() {
  if (state.ethernet_requested || state.ethernet_started) {
    ETH.end();
  }
  resetEthernetState();
}

static void startEthernetService() {
  state.hostname = makeDefaultHostname();
  resetEthernetState();

  SPI.begin(P_ETH_SCLK, P_ETH_MISO, P_ETH_MOSI, P_ETH_CS);
  pinMode(P_ETH_CS, OUTPUT);
  digitalWrite(P_ETH_CS, HIGH);

  if (!ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, P_ETH_CS, P_ETH_IRQ, P_ETH_RST, SPI)) {
    logLine("ETH begin failed");
    return;
  }

  state.ethernet_requested = true;

  if (!state.config.eth_dhcp) {
    IPAddress ip;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns1;
    IPAddress dns2;
    String error;
    if (parseStaticIpConfig(state.config.eth_ip, state.config.eth_gateway, state.config.eth_subnet,
                            state.config.eth_dns1, state.config.eth_dns2,
                            ip, gateway, subnet, dns1, dns2, error)) {
      if (ETH.config(ip, gateway, subnet, dns1, dns2)) {
        char line[CONSOLE_LINE_BUFFER];
        snprintf(line, sizeof(line), "ETH static config requested: %s", ip.toString().c_str());
        logLine(line);
      } else {
        logLine("ETH static config failed, falling back to DHCP");
      }
    } else {
      logLine("ETH static config invalid, falling back to DHCP");
    }
  } else {
    logLine("ETH init requested (DHCP)");
  }
}

static void stopWiFiServices() {
  stopFallbackDiscovery();
  WiFi.softAPdisconnect(false);
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_OFF);
  resetWiFiState();
}

static void startWiFiSetupAp() {
  if (!state.config.wifi_enabled) {
    return;
  }

  String ap_password = getFallbackApPassword();
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(FALLBACK_AP_IP, FALLBACK_AP_GATEWAY, FALLBACK_AP_SUBNET, IPAddress(), FALLBACK_AP_IP);
  WiFi.softAPsetHostname(FALLBACK_AP_HOST);
  if (WiFi.softAP(FALLBACK_AP_SSID, ap_password.c_str())) {
    state.wifi_ap_running = true;
    state.reported_wifi_ap = true;
    startFallbackDiscovery();
    char line[CONSOLE_LINE_BUFFER];
    snprintf(line, sizeof(line), "WiFi setup AP active: %s  http://%s/",
             FALLBACK_AP_SSID, FALLBACK_AP_DOMAIN);
    logLine(line);
  } else {
    logLine("WiFi setup AP failed");
  }
}

static void startConfiguredWiFi(bool keep_ap_running) {
  if (!state.config.wifi_enabled || !state.config.wifiConfigured()) {
    return;
  }

  WiFi.persistent(false);
  WiFi.mode(keep_ap_running ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(state.hostname.c_str());
  WiFi.setAutoReconnect(true);

  if (state.config.wifi_dhcp) {
    WiFi.config(IPAddress(), IPAddress(), IPAddress(), IPAddress(), IPAddress());
  } else {
    IPAddress ip;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns1;
    IPAddress dns2;
    String error;
    if (parseStaticIpConfig(state.config.wifi_ip, state.config.wifi_gateway, state.config.wifi_subnet,
                            state.config.wifi_dns1, state.config.wifi_dns2,
                            ip, gateway, subnet, dns1, dns2, error)) {
      if (!WiFi.config(ip, gateway, subnet, dns1, dns2)) {
        logLine("WiFi static config failed, falling back to DHCP");
        WiFi.config(IPAddress(), IPAddress(), IPAddress(), IPAddress(), IPAddress());
      }
    } else {
      logLine("WiFi static config invalid, falling back to DHCP");
      WiFi.config(IPAddress(), IPAddress(), IPAddress(), IPAddress(), IPAddress());
    }
  }

  const char* password = state.config.wifi_password.length() > 0 ? state.config.wifi_password.c_str() : nullptr;
  WiFi.begin(state.config.wifi_ssid.c_str(), password);
  state.wifi_sta_requested = true;
  state.wifi_connect_deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
  state.wifi_last_reconnect_at = millis();
  state.reported_wifi_waiting = false;
  state.reported_wifi_ap = keep_ap_running;

  char line[CONSOLE_LINE_BUFFER];
  snprintf(line, sizeof(line), "WiFi connect requested: %s (%s)",
           state.config.wifi_ssid.c_str(), state.config.wifi_dhcp ? "DHCP" : "static");
  logLine(line);
}

static void startWiFiFallbackAp() {
  if (!state.config.wifi_enabled || state.wifi_ap_running) {
    return;
  }

  String ap_password = getFallbackApPassword();
  WiFi.mode(state.config.wifiConfigured() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAPConfig(FALLBACK_AP_IP, FALLBACK_AP_GATEWAY, FALLBACK_AP_SUBNET, IPAddress(), FALLBACK_AP_IP);
  WiFi.softAPsetHostname(FALLBACK_AP_HOST);

  if (WiFi.softAP(FALLBACK_AP_SSID, ap_password.c_str())) {
    state.wifi_ap_running = true;
    state.reported_wifi_ap = true;
    startFallbackDiscovery();
    char line[CONSOLE_LINE_BUFFER];
    snprintf(line, sizeof(line), "WiFi AP active: %s  http://%s/",
             FALLBACK_AP_SSID, FALLBACK_AP_DOMAIN);
    logLine(line);
  } else {
    logLine("WiFi AP start failed");
  }
}

static void stopWiFiFallbackAp() {
  if (!state.wifi_ap_running) {
    return;
  }
  stopFallbackDiscovery();
  WiFi.softAPdisconnect(false);
  state.wifi_ap_running = false;
  state.reported_wifi_ap = false;
  logLine("WiFi AP stopped");
}

static void restartNetworkServices() {
  stopEthernetService();
  stopWiFiServices();

  startEthernetService();

  if (!state.config.wifi_enabled) {
    logLine("WiFi disabled");
    return;
  }

  if (state.config.wifiConfigured()) {
    startConfiguredWiFi(false);
  } else {
    startWiFiSetupAp();
  }
}

static void applyPostedConfig(AsyncWebServerRequest* request) {
  state.config.eth_dhcp = !request->hasArg("eth_mode") || request->arg("eth_mode") != "static";
  state.config.eth_ip = sanitizeSingleLine(request->arg("eth_ip"), true);
  state.config.eth_gateway = sanitizeSingleLine(request->arg("eth_gateway"), true);
  state.config.eth_subnet = sanitizeSingleLine(request->arg("eth_subnet"), true);
  state.config.eth_dns1 = sanitizeSingleLine(request->arg("eth_dns1"), true);
  state.config.eth_dns2 = sanitizeSingleLine(request->arg("eth_dns2"), true);

  state.config.wifi_enabled = request->hasArg("wifi_enabled");
  state.config.wifi_ssid = sanitizeSingleLine(request->arg("wifi_ssid"), true);
  state.config.wifi_password = sanitizeSingleLine(request->arg("wifi_password"), false);
  state.config.wifi_dhcp = !request->hasArg("wifi_mode") || request->arg("wifi_mode") != "static";
  state.config.wifi_ip = sanitizeSingleLine(request->arg("wifi_ip"), true);
  state.config.wifi_gateway = sanitizeSingleLine(request->arg("wifi_gateway"), true);
  state.config.wifi_subnet = sanitizeSingleLine(request->arg("wifi_subnet"), true);
  state.config.wifi_dns1 = sanitizeSingleLine(request->arg("wifi_dns1"), true);
  state.config.wifi_dns2 = sanitizeSingleLine(request->arg("wifi_dns2"), true);
}

static String validatePostedConfig() {
  IPAddress ip;
  IPAddress gateway;
  IPAddress subnet;
  IPAddress dns1;
  IPAddress dns2;
  String error;

  if (!state.config.eth_dhcp &&
      !parseStaticIpConfig(state.config.eth_ip, state.config.eth_gateway, state.config.eth_subnet,
                           state.config.eth_dns1, state.config.eth_dns2,
                           ip, gateway, subnet, dns1, dns2, error)) {
    return "Ethernet settings were not saved: " + error;
  }

  if (state.config.wifi_enabled && state.config.wifiConfigured() && !state.config.wifi_dhcp &&
      !parseStaticIpConfig(state.config.wifi_ip, state.config.wifi_gateway, state.config.wifi_subnet,
                           state.config.wifi_dns1, state.config.wifi_dns2,
                           ip, gateway, subnet, dns1, dns2, error)) {
    return "Wi-Fi settings were not saved: " + error;
  }

  return "";
}

static void startWebServer() {
  if (state.web_started) {
    if (!state.raw_tcp_started) {
      startRawTcpServer();
    }
    return;
  }

  state.server = new AsyncWebServer(80);
  state.websocket = new AsyncWebSocket("/ws");
  state.websocket->onEvent(onConsoleEvent);
  state.server->addHandler(state.websocket);

  state.server->on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", buildHomePage());
  });
  state.server->on("/health", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/plain", "test");
  });
  state.server->on("/api/status", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "application/json", buildStatusJson());
  });
  state.server->on("/webserial", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", WEB_CONSOLE_HTML);
  });
  state.server->on("/network", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", buildNetworkConfigPage("", false));
  });
  state.server->on("/network", HTTP_POST, [](AsyncWebServerRequest* request) {
    applyPostedConfig(request);

    String validation_error = validatePostedConfig();
    if (validation_error.length() > 0) {
      request->send(400, "text/html", buildNetworkConfigPage(validation_error, true));
      return;
    }

    if (!saveNetworkConfig()) {
      request->send(500, "text/html", buildNetworkConfigPage("Failed to save network settings.", true));
      return;
    }

    scheduleNetworkReload();
    request->send(200, "text/html", buildNetworkSavedPage());
  });
  state.server->on("/log", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (state.fs != nullptr && state.fs->exists("/packet_log")) {
      request->send(*state.fs, "/packet_log", "text/plain");
    } else {
      request->send(404, "text/plain", "packet log not found");
    }
  });

#if defined(ADMIN_PASSWORD)
  state.server->on("/update", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!request->authenticate("admin", ADMIN_PASSWORD)) {
      return request->requestAuthentication();
    }
    request->send(200, "text/html", OTA_PAGE_HTML);
  });
  state.server->on("/update", HTTP_POST, [](AsyncWebServerRequest* request) {
    if (!request->authenticate("admin", ADMIN_PASSWORD)) {
      return request->requestAuthentication();
    }

    AsyncWebServerResponse* response =
        request->beginResponse(Update.hasError() ? 500 : 200, "text/plain",
                               Update.hasError() ? "FAIL" : "OK");
    response->addHeader("Connection", "close");
    request->send(response);

    if (!Update.hasError()) {
      delay(100);
      ESP.restart();
    }
  }, [](AsyncWebServerRequest* request, String filename, size_t index, uint8_t* data, size_t len, bool final) {
    if (!request->authenticate("admin", ADMIN_PASSWORD)) {
      return request->requestAuthentication();
    }

    if (index == 0) {
      int command = (filename == "filesystem") ? U_SPIFFS : U_FLASH;
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, command)) {
        Update.printError(Serial);
      }
    }

    if (len > 0 && Update.write(data, len) != len) {
      Update.printError(Serial);
    }

    if (final && !Update.end(true)) {
      Update.printError(Serial);
    }
  });
#endif

  startRawTcpServer();
  state.server->begin();
  state.web_started = true;
}

static void loopWiFiStateMachine() {
  if (!state.config_loaded || !state.config.wifi_enabled) {
    return;
  }

  if (!state.config.wifiConfigured()) {
    if (!state.wifi_ap_running) {
      startWiFiSetupAp();
    }
    return;
  }

  if (state.wifi_has_ip) {
    if (state.wifi_ap_running) {
      stopWiFiFallbackAp();
    }

    IPAddress current_ip = WiFi.localIP();
    if (!state.reported_wifi_ready || current_ip != state.last_reported_wifi_ip) {
      state.last_reported_wifi_ip = current_ip;
      state.reported_wifi_ready = true;
      state.reported_wifi_waiting = false;

      char line[CONSOLE_LINE_BUFFER];
      String ip_text = current_ip.toString();
      snprintf(line, sizeof(line), "WiFi ready: %s  http://%s/  raw=socket://%s:%u",
               WiFi.SSID().c_str(), ip_text.c_str(), ip_text.c_str(),
               static_cast<unsigned>(RAW_TCP_PORT));
      logLine(line);
    }
    return;
  }

  if (state.reported_wifi_ready) {
    state.reported_wifi_ready = false;
    state.last_reported_wifi_ip = IPAddress();
    logLine("WiFi link down, retrying before fallback AP");
  }

  if (!state.wifi_sta_requested) {
    startConfiguredWiFi(state.wifi_ap_running);
  }

  if (!state.reported_wifi_waiting) {
    state.reported_wifi_waiting = true;
    logLine("WiFi waiting for connection");
  }

  if (!state.wifi_ap_running && state.wifi_connect_deadline != 0 && millis() >= state.wifi_connect_deadline) {
    startWiFiFallbackAp();
  }

  if (state.wifi_ap_running && millis() - state.wifi_last_reconnect_at >= WIFI_RECONNECT_INTERVAL_MS) {
    WiFi.reconnect();
    state.wifi_last_reconnect_at = millis();
  }
}

}  // namespace

#endif

void TETHEliteBoard::begin() {
  ESP32Board::begin();

#if defined(MESH_ETHERNET_WEB)
  active_board = this;
  inhibit_sleep = true;
  state.hostname = makeDefaultHostname();
#endif
}

bool TETHEliteBoard::startOTAUpdate(const char* id, char reply[]) {
  (void)id;

#if defined(MESH_ETHERNET_WEB)
  if (!state.web_started) {
    strcpy(reply, "Ethernet web service is not started");
    return false;
  }

  if (!ethernetReady()) {
    strcpy(reply, "Ethernet is up, waiting for DHCP");
    return true;
  }

  snprintf(reply, OTA_ID_BUFFER, "Started: http://%s/update", ETH.localIP().toString().c_str());
  return true;
#else
  return ESP32Board::startOTAUpdate(id, reply);
#endif
}

void TETHEliteBoard::configureNetworkServices(fs::FS* fs, const char* node_name,
                                              const char* role, const char* node_id,
                                              TETHEliteCommandHandler handler) {
#if defined(MESH_ETHERNET_WEB)
  state.fs = fs;
  state.node_name = (node_name != nullptr && *node_name) ? node_name : "MeshCore";
  state.role = (role != nullptr && *role) ? role : "headless";
  state.node_id = (node_id != nullptr && *node_id) ? node_id : "-";
  state.handler = handler;

  if (!state.config_loaded) {
    loadNetworkConfig();
  }

  registerNetworkEvents();
  restartNetworkServices();
  if (!state.web_started && (state.ethernet_requested || state.config.wifi_enabled)) {
    startWebServer();
  }

  consolePrintf("WebSerial armed for %s (%s)", state.node_name.c_str(), state.role.c_str());
#else
  (void)fs;
  (void)node_name;
  (void)role;
  (void)node_id;
  (void)handler;
#endif
}

void TETHEliteBoard::loopNetworkServices() {
#if defined(MESH_ETHERNET_WEB)
  if (state.websocket != nullptr) {
    state.websocket->cleanupClients();
  }

  if (state.network_reload_requested &&
      (long)(millis() - state.network_reload_at) >= 0) {
    state.network_reload_requested = false;
    restartNetworkServices();
  }

  if (ethernetReady()) {
    IPAddress current_ip = ETH.localIP();
    if (!state.reported_eth_ready || current_ip != state.last_reported_eth_ip) {
      state.last_reported_eth_ip = current_ip;
      state.reported_eth_ready = true;
      state.reported_eth_disconnected = false;

      char line[CONSOLE_LINE_BUFFER];
      String ip_text = current_ip.toString();
      snprintf(line, sizeof(line), "ETH ready: http://%s/  webserial=/webserial  ota=/update  raw=socket://%s:%u",
               ip_text.c_str(), ip_text.c_str(),
               static_cast<unsigned>(RAW_TCP_PORT));
      logLine(line);
    }
  } else if (state.reported_eth_ready && !state.reported_eth_disconnected) {
    state.reported_eth_disconnected = true;
    state.reported_eth_ready = false;
    state.last_reported_eth_ip = IPAddress();
    logLine("ETH link down");
  }

  loopWiFiStateMachine();
#endif
}

void TETHEliteBoard::consolePrintLine(const char* line) {
#if defined(MESH_ETHERNET_WEB)
  if (line == nullptr || *line == 0) {
    return;
  }
  rememberConsoleLine(String(line));
#else
  (void)line;
#endif
}

void TETHEliteBoard::consolePrintf(const char* format, ...) {
#if defined(MESH_ETHERNET_WEB)
  char line[CONSOLE_LINE_BUFFER];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  consolePrintLine(line);
#else
  (void)format;
#endif
}

bool TETHEliteBoard::ethernetReady() const {
#if defined(MESH_ETHERNET_WEB)
  return state.ethernet_connected && state.ethernet_has_ip;
#else
  return false;
#endif
}
