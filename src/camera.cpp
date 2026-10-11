#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <MD5Builder.h>
#include <esp_heap_caps.h>
#include "camera.h"
#include "secrets.h"

// Snapshot JPEG d'une AXIS M1034-W (API VAPIX, auth Digest), redefinissable dans secrets.h.
// 320x240 (~15 Ko) est la plus petite resolution acceptee (160x120 renvoie 400) ; la
// resolution native 800x600 pese ~135 Ko et ne tient pas a coup sur en RAM (pas de PSRAM).
#ifndef CAMERA_SNAPSHOT_PATH
#define CAMERA_SNAPSHOT_PATH "/axis-cgi/jpg/image.cgi?resolution=320x240"
#endif
#ifndef CAMERA_PORT
#define CAMERA_PORT 80
#endif

namespace Camera {

static const size_t MAX_JPEG_SIZE = 150 * 1024;
static const size_t HEAP_MARGIN = 24 * 1024;  // laisse de la place a la pile WiFi/TCP
static const uint16_t HTTP_TIMEOUT_MS = 5000;

static uint8_t* jpegBuf = nullptr;
static size_t jpegCap = 0;

// Stream qui ecrit dans jpegBuf ; refuse les octets au-dela de la capacite,
// ce qui fait echouer writeToStream proprement sur une image trop grosse
class BufferStream : public Stream {
public:
  size_t len = 0;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buf, size_t size) override {
    if (len + size > jpegCap) return 0;
    memcpy(jpegBuf + len, buf, size);
    len += size;
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// Etat de l'authentification, conserve entre deux snapshots pour envoyer
// l'en-tete d'emblee au lieu de prendre un 401 a chaque requete
enum class AuthMode { Unknown, Basic, Digest };
static AuthMode authMode = AuthMode::Unknown;
static String realm, nonce, opaque, qop;
static uint32_t nonceCount = 0;

static String md5(const String& s) {
  MD5Builder b;
  b.begin();
  b.add(s);
  b.calculate();
  return b.toString();
}

// Extrait key="valeur" (ou key=valeur) d'un en-tete WWW-Authenticate
static String headerParam(const String& header, const char* key) {
  String k = String(key) + "=";
  int start = 0;
  while ((start = header.indexOf(k, start)) >= 0) {
    // evite de matcher "nonce" dans "cnonce"
    if (start == 0 || header[start - 1] == ' ' || header[start - 1] == ',') break;
    start += k.length();
  }
  if (start < 0) return "";
  start += k.length();
  if (header[start] == '"') {
    int end = header.indexOf('"', start + 1);
    return header.substring(start + 1, end < 0 ? header.length() : end);
  }
  int end = header.indexOf(',', start);
  String v = header.substring(start, end < 0 ? header.length() : end);
  v.trim();
  return v;
}

static void parseChallenge(const String& header) {
  if (header.startsWith("Digest")) {
    authMode = AuthMode::Digest;
    realm = headerParam(header, "realm");
    nonce = headerParam(header, "nonce");
    opaque = headerParam(header, "opaque");
    // qop peut lister plusieurs valeurs ("auth,auth-int") : on ne gere que "auth"
    qop = headerParam(header, "qop").indexOf("auth") >= 0 ? "auth" : "";
    nonceCount = 0;
  } else {
    authMode = AuthMode::Basic;
  }
}

static String digestAuthorization(const char* uri) {
  char nc[9];
  snprintf(nc, sizeof(nc), "%08x", (unsigned)++nonceCount);
  String cnonce = String(esp_random(), HEX);
  String ha1 = md5(String(CAMERA_USER) + ":" + realm + ":" + CAMERA_PASSWORD);
  String ha2 = md5(String("GET:") + uri);
  String response = qop.length()
      ? md5(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2)
      : md5(ha1 + ":" + nonce + ":" + ha2);

  String h = "Digest username=\"" CAMERA_USER "\", realm=\"" + realm + "\", nonce=\"" + nonce +
             "\", uri=\"" + uri + "\", response=\"" + response + "\"";
  if (opaque.length()) h += ", opaque=\"" + opaque + "\"";
  if (qop.length()) h += ", qop=" + qop + ", nc=" + nc + ", cnonce=\"" + cnonce + "\"";
  return h;
}

bool setup() {
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  jpegCap = largest > HEAP_MARGIN ? min(MAX_JPEG_SIZE, largest - HEAP_MARGIN) : 0;
  jpegBuf = jpegCap ? (uint8_t*)malloc(jpegCap) : nullptr;
  if (!jpegBuf) jpegCap = 0;
  Serial.printf("Tampon JPEG : %u octets (plus grand bloc libre : %u)\n", jpegCap, largest);
  return jpegBuf != nullptr;
}

// Une requete GET ; renvoie le code HTTP (ou une erreur negative de HTTPClient)
static int request(BufferStream& out) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.useHTTP10(true);  // pas de chunked : le corps arrive tel quel
  if (!http.begin(client, CAMERA_HOST, CAMERA_PORT, CAMERA_SNAPSHOT_PATH)) return HTTPC_ERROR_CONNECTION_REFUSED;

  const char* keys[] = {"WWW-Authenticate"};
  http.collectHeaders(keys, 1);
  if (authMode == AuthMode::Basic) {
    http.setAuthorization(CAMERA_USER, CAMERA_PASSWORD);
  } else if (authMode == AuthMode::Digest) {
    http.addHeader("Authorization", digestAuthorization(CAMERA_SNAPSHOT_PATH));
  }

  int code = http.GET();
  if (code == HTTP_CODE_UNAUTHORIZED) {
    parseChallenge(http.header("WWW-Authenticate"));
  } else if (code == HTTP_CODE_OK) {
    out.len = 0;
    int written = http.writeToStream(&out);
    if (written < 0) {
      Serial.printf("Lecture snapshot : erreur %d (%u octets max)\n", written, jpegCap);
      code = written;
    }
  }
  http.end();
  return code;
}

size_t fetchSnapshot(uint8_t** data) {
  if (!jpegBuf) return 0;
  BufferStream out;
  int code = request(out);
  // 401 : premier passage ou nonce expire, on rejoue avec le nouveau challenge
  if (code == HTTP_CODE_UNAUTHORIZED) code = request(out);
  if (code != HTTP_CODE_OK) {
    if (code > 0) Serial.printf("Snapshot : HTTP %d\n", code);
    else if (code != HTTPC_ERROR_STREAM_WRITE) Serial.printf("Snapshot : %s\n", HTTPClient::errorToString(code).c_str());
    return 0;
  }
  *data = jpegBuf;
  return out.len;
}

}
