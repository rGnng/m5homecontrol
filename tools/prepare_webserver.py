"""Build a project-local WebServer copy with bounded request parsing.

The Arduino 2.x multipart reader waits forever on a connected silent peer.
Keep the upstream parser and abort paths, adding only read deadlines. Shared
PlatformIO packages are never modified. A source mismatch fails the build.
"""
from pathlib import Path
import hashlib
import json
import shutil


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise RuntimeError("Unsupported Arduino WebServer source: " + before[:70])
    return source.replace(before, after, 1)


def guarded_sources(header, parser):
    start = parser.index("static char* readBytesWithTimeout(")
    end = parser.index("\nbool WebServer::_parseRequest", start)
    parser = parser[:start] + """static char* readBytesWithTimeout(WiFiClient& client, size_t maxLength, size_t& dataLength, int timeout_ms) {
  char* buffer = nullptr;
  dataLength = 0;
  if (maxLength > 131072) { client.stop(); return nullptr; }
  uint32_t started = millis();
  while (dataLength < maxLength && uint32_t(millis() - started) < uint32_t(timeout_ms)) {
    size_t available = client.available();
    if (!available) {
      if (!client.connected()) break;
      delay(1); continue;
    }
    size_t count = std::min(available, maxLength - dataLength);
    char* grown = static_cast<char*>(realloc(buffer, dataLength + count + 1));
    if (!grown) { free(buffer); return nullptr; }
    buffer = grown;
    size_t received = client.readBytes(buffer + dataLength, count);
    dataLength += received;
    buffer[dataLength] = 0;
  }
  if (dataLength < maxLength) client.stop();
  return buffer;
}
""" + parser[end:]
    header = replace_once(header, "  WiFiServer  _server;", """  String _readStringUntil(WiFiClient& client, char terminator);
  uint32_t _requestReadStarted = 0, _requestReadLimit = 5000;
  bool _requestReadTimedOut = false;
  WiFiServer  _server;""")
    parser = parser.replace("client.readStringUntil(", "_readStringUntil(client, ")
    parser = replace_once(parser, "bool WebServer::_parseRequest(WiFiClient& client) {", """bool WebServer::_parseRequest(WiFiClient& client) {
  _requestReadStarted = millis();
  _requestReadLimit = 5000;
  _requestReadTimedOut = false;""")
    parser = replace_once(parser, "bool WebServer::_parseForm(WiFiClient& client, String boundary, uint32_t len){", """bool WebServer::_parseForm(WiFiClient& client, String boundary, uint32_t len){
  if (_requestReadTimedOut) return false;
  _requestReadStarted = millis();
  _requestReadLimit = 120000;""")
    start = parser.index("bool WebServer::_parseRequest(")
    end = parser.index("\nbool WebServer::_collectHeader", start)
    section = parser[start:end]
    section = replace_once(section, "return true;", "return !_requestReadTimedOut;")
    parser = parser[:start] + section + parser[end:]
    start = parser.index("int WebServer::_uploadReadByte(")
    end = parser.index("\nbool WebServer::_parseForm(", start)
    parser = parser[:start] + """String WebServer::_readStringUntil(WiFiClient& client, char terminator) {
  String result;
  uint32_t lastByte = millis();
  while (client.connected() || client.available()) {
    if (_requestReadTimedOut || uint32_t(millis() - _requestReadStarted) >= _requestReadLimit ||
        uint32_t(millis() - lastByte) >= 1500 || result.length() >= 8192) {
      _requestReadTimedOut = true;
      client.stop();
      return result;
    }
    int value = client.read();
    if (value < 0) { delay(1); continue; }
    lastByte = millis();
    if (value == terminator) return result;
    result += static_cast<char>(value);
  }
  return result;
}

int WebServer::_uploadReadByte(WiFiClient& client) {
  uint32_t started = millis();
  while (client.connected() || client.available()) {
    if (uint32_t(millis() - _requestReadStarted) >= _requestReadLimit ||
        uint32_t(millis() - started) >= 1500) {
      _requestReadTimedOut = true;
      client.stop();
      return -1;
    }
    int value = client.read();
    if (value >= 0) return value;
    delay(1);
  }
  return -1;
}
""" + parser[end:]
    return header, parser


def prepare(source, destination):
    source, destination = Path(source), Path(destination)
    script = Path(__file__) if "__file__" in globals() else Path(env.subst("$PROJECT_DIR")) / "tools/prepare_webserver.py"
    digest = hashlib.sha256(script.read_bytes())
    for path in sorted((source / "src").rglob("*")):
        if path.is_file(): digest.update(path.read_bytes())
    signature = digest.hexdigest()
    stamp = destination / ".source-sha256"
    if stamp.exists() and stamp.read_text() == signature:
        return
    header, parser = guarded_sources((source / "src/WebServer.h").read_text(),
                                     (source / "src/Parsing.cpp").read_text())
    if destination.exists(): shutil.rmtree(destination)
    shutil.copytree(source / "src", destination / "src")
    (destination / "src/WebServer.h").write_text(header)
    (destination / "src/Parsing.cpp").write_text(parser)
    (destination / "library.json").write_text(json.dumps({
        "name": "BoundedWebServer", "version": "2.0.17-guards.1",
        "frameworks": "arduino", "platforms": "espressif32",
    }))
    stamp.write_text(signature)


if "Import" in globals():
    Import("env")
    framework = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    prepare(Path(framework) / "libraries/WebServer",
            Path(env.subst("$PROJECT_DIR")) / ".pio/guarded-libs/BoundedWebServer")
