#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cmath>
#include <cstring>

#include "config.h"
#include "services/http_body_framing.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectTimeoutMs = 5000;  // TLS handshake needs room
constexpr unsigned long kRequestTimeoutMs = 6000;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
unsigned long s_last_update_ms = 0;
PollFn s_poll_fn = nullptr;
SemaphoreHandle_t s_mutex = nullptr;

/** Publish parsed aircraft to the shared buffer atomically. */
void publish(const Aircraft* src, size_t count) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  for (size_t i = 0; i < count; ++i) {
    s_aircraft[i] = src[i];
  }
  s_aircraft_count = count;
  s_last_update_ms = millis();  // base time for dead-reckoning
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

const char* httpClientErrorName(int code) {
  switch (code) {
    case HTTPC_ERROR_CONNECTION_REFUSED:
      return "connection refused";
    case HTTPC_ERROR_SEND_HEADER_FAILED:
      return "send header failed";
    case HTTPC_ERROR_SEND_PAYLOAD_FAILED:
      return "send payload failed";
    case HTTPC_ERROR_NOT_CONNECTED:
      return "not connected";
    case HTTPC_ERROR_CONNECTION_LOST:
      return "connection lost";
    case HTTPC_ERROR_NO_STREAM:
      return "no stream";
    case HTTPC_ERROR_NO_HTTP_SERVER:
      return "no http server";
    case HTTPC_ERROR_TOO_LESS_RAM:
      return "not enough RAM";
    case HTTPC_ERROR_ENCODING:
      return "encoding";
    case HTTPC_ERROR_STREAM_WRITE:
      return "stream write";
    case HTTPC_ERROR_READ_TIMEOUT:
      return "read timeout";
    default:
      return "error";
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectTimeoutMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  int last_code = HTTPC_ERROR_READ_TIMEOUT;
  bool logged_retry = false;
  while (millis() < deadline) {
    pollNetwork();
    const int code = http.GET();
    if (code > 0) {
      return code;
    }
    last_code = code;
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    if (config::kDebugLog && !logged_retry) {
      Serial.printf("adsb: connect retry (%s)\n", httpClientErrorName(code));
      logged_retry = true;
    }
    delay(5);
  }
  return last_code;
}

/**
 * Pumps the socket one byte at a time, in blocks, without buffering the whole
 * response.
 *
 * Each refill runs the network poll callback, which is why HTTPClient's own
 * body readers (getString/writeToStream) can't be used here -- they block
 * without giving the fetch task a chance to poll. That also means they can't
 * de-chunk for us, so the wire image comes out raw and BodyFramer unwraps it.
 *
 * Where the body ends is BodyFramer's business, not this class's: it stops
 * pulling at the right byte. Reading a little past that point into buffer_ is
 * harmless while the connection is torn down after every fetch.
 */
class PollingSocketSource {
 public:
  PollingSocketSource(HTTPClient& http, NetworkClient& stream,
                      unsigned long deadline)
      : http_(&http), stream_(&stream), deadline_(deadline) {}

  /** Next raw byte, or -1 once the socket closes or the deadline passes. */
  int read() {
    if (pos_ >= len_ && !refill()) {
      return -1;
    }
    return static_cast<unsigned char>(buffer_[pos_++]);
  }

 private:
  bool refill() {
    pos_ = 0;
    len_ = 0;
    while (millis() < deadline_) {
      pollNetwork();
      const int available = stream_->available();
      if (available > 0) {
        const int to_read = available > static_cast<int>(sizeof(buffer_))
                                ? static_cast<int>(sizeof(buffer_))
                                : available;
        const int read_bytes = stream_->readBytes(buffer_, to_read);
        if (read_bytes > 0) {
          len_ = static_cast<size_t>(read_bytes);
          return true;
        }
      }
      if (!http_->connected() && stream_->available() <= 0) {
        break;  // server closed and the socket is drained
      }
      delay(1);
    }
    return false;
  }

  HTTPClient* http_;
  NetworkClient* stream_;
  unsigned long deadline_;
  char buffer_[512];
  size_t pos_ = 0;
  size_t len_ = 0;
};

using BodyReader = services::http::BodyFramer<PollingSocketSource>;

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

const char* jsonNumberKind(JsonVariantConst value) {
  if (value.isNull()) {
    return "missing";
  }
  if (value.is<float>() || value.is<double>()) {
    return "float";
  }
  if (value.is<int>()) {
    return "int";
  }
  if (value.is<const char*>()) {
    return "string";
  }
  return "other";
}

void logDroppedPosition(const JsonObject& plane) {
  char hex[9];
  copyJsonStringTrimmed(plane, "hex", hex, sizeof(hex));
  Serial.printf("adsb: dropped %s (lat %s, lon %s)\n", hex[0] != '\0' ? hex : "?",
                jsonNumberKind(plane["lat"]), jsonNumberKind(plane["lon"]));
}

float approxDistKm(double center_lat, double center_lon, float lat, float lon) {
  constexpr float kKmPerDeg = 111.0f;
  constexpr float kDegToRad = 0.01745329252f;
  const float dx = (lon - static_cast<float>(center_lon)) * kKmPerDeg *
                   cosf(static_cast<float>(center_lat) * kDegToRad);
  const float dy = (lat - static_cast<float>(center_lat)) * kKmPerDeg;
  return sqrtf(dx * dx + dy * dy);
}

}  // namespace

void init() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
}

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

unsigned long lastUpdateMs() { return s_last_update_ms; }

size_t snapshotAircraft(Aircraft* out, size_t max_out,
                        unsigned long* out_last_update_ms) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  const size_t count =
      s_aircraft_count < max_out ? s_aircraft_count : max_out;
  for (size_t i = 0; i < count; ++i) {
    out[i] = s_aircraft[i];
  }
  if (out_last_update_ms != nullptr) {
    *out_last_update_ms = s_last_update_ms;
  }
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
  return count;
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  // Keep only the fields we render. Debug logs also keep the feed status
  // fields. Everything else never reaches RAM.
  JsonDocument filter;
  if (config::kDebugLog) {
    filter["msg"] = true;
    filter["total"] = true;
  }
  JsonObject f = filter["ac"].add<JsonObject>();
  for (const char* key :
       {"lat", "lon", "true_heading", "mag_heading", "track", "dir", "gs",
        "tas", "ias", "alt_baro", "alt_geom", "seen_pos", "flight", "hex", "t",
        "category"}) {
    f[key] = true;
  }

  if (config::kDebugLog) {
    Serial.printf(
        "adsb: GET %.6f,%.6f radius %.1f km (%.1f nm) rssi %d  free %u largest %u\n",
        center_lat, center_lon, fetch_radius_km, dist_nm, WiFi.RSSI(),
        static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(ESP.getMaxAllocHeap()));
    Serial.printf("adsb: %s\n", url.c_str());
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    if (config::kDebugLog) {
      Serial.printf("adsb: http.begin failed  free %u largest %u\n",
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMaxAllocHeap()));
    } else {
      Serial.println("adsb: http.begin failed");
    }
    return false;
  }

  // HTTPClient only records Transfer-Encoding in the collected headers when
  // it is asked for up front; _transferEncoding itself is private.
  static const char* kWantedHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(kWantedHeaders, 1);

  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
    if (config::kDebugLog && code < 0) {
      Serial.printf("adsb: HTTP %d (%s)  free %u largest %u\n", code,
                    httpClientErrorName(code),
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMaxAllocHeap()));
    } else if (config::kDebugLog) {
      Serial.printf("adsb: HTTP %d  free %u largest %u\n", code,
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMaxAllocHeap()));
    } else {
      Serial.printf("adsb: HTTP %d (free %u, largest %u)\n", code,
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMaxAllocHeap()));
    }
    http.end();
    return false;
  }

  NetworkClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    if (config::kDebugLog) {
      Serial.printf("adsb: no response stream  free %u largest %u\n",
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMaxAllocHeap()));
    } else {
      Serial.println("adsb: no response stream");
    }
    http.end();
    return false;
  }

  // On HTTP/1.1 the CDN answers with Transfer-Encoding: chunked, and
  // getStreamPtr() hands back the raw socket -- chunk sizes and all. BodyFramer
  // strips that framing back off.
  const services::http::BodyFraming framing =
      http.header("Transfer-Encoding").equalsIgnoreCase("chunked")
          ? services::http::BodyFraming::kChunked
          : services::http::BodyFraming::kIdentity;

  const int content_length = http.getSize();
  PollingSocketSource source(http, *stream, millis() + kRequestTimeoutMs);
  BodyReader body(source, framing, content_length);
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  // Read off the terminating chunk the parser stopped short of, so the socket
  // sits at the end of the message. Not needed while every fetch builds its own
  // connection, but a prerequisite for ever reusing one.
  body.drain();
  http.end();
  const char* framing_name =
      framing == services::http::BodyFraming::kChunked ? "chunked" : "identity";
  if (err) {
    if (body.framingError()) {
      if (config::kDebugLog) {
        Serial.printf("adsb: malformed chunked body  bytes %u  content-length %d\n",
                      static_cast<unsigned>(body.bytesRead()), content_length);
      } else {
        Serial.println("adsb: malformed chunked body");
      }
    } else if (body.bytesRead() == 0) {
      if (config::kDebugLog) {
        Serial.printf("adsb: empty response  content-length %d %s\n", content_length,
                      framing_name);
      } else {
        Serial.println("adsb: empty response");
      }
    } else if (config::kDebugLog) {
      Serial.printf(
          "adsb: JSON parse error: %s  bytes %u  content-length %d %s%s\n",
          err.c_str(), static_cast<unsigned>(body.bytesRead()), content_length,
          framing_name, body.truncated() ? " truncated" : "");
    } else {
      Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    }
    return false;
  }

  // Parse into a local buffer, then publish atomically so a reader on another
  // thread never sees a half-updated list.
  Aircraft parsed[kMaxAircraft];
  size_t n = 0;
  size_t in_feed = 0;
  size_t skip_pos = 0;
  size_t skip_ground = 0;
  size_t skip_cap = 0;
  float nearest_km = 0.0f;
  bool have_nearest = false;
  char nearest_id[9] = {};
  float nearest_lat = 0.0f;
  float nearest_lon = 0.0f;
  const bool ac_present = doc["ac"].is<JsonArray>();
  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac_present) {
    for (JsonObject plane : ac) {
      if (config::kDebugLog) {
        ++in_feed;
      }
      if (n >= kMaxAircraft) {
        if (!config::kDebugLog) {
          break;
        }
        ++skip_cap;
        continue;
      }
      if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
        if (config::kDebugLog) {
          if (skip_pos == 0) {
            logDroppedPosition(plane);
          }
          ++skip_pos;
        }
        continue;
      }
      if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
        if (config::kDebugLog) {
          ++skip_ground;
        }
        continue;
      }

      parsed[n].lat = plane["lat"].as<float>();
      parsed[n].lon = plane["lon"].as<float>();
      parsed[n].nose_deg = pickNoseHeading(plane);
      parsed[n].track_deg = pickTrackHeading(plane);
      parsed[n].gs_knots = pickGroundSpeed(plane);

      // seen_pos: seconds since this position was measured. Use it as the
      // dead-reckoning age offset, capped so a very stale fix isn't flung far.
      float seen_pos = 0.0f;
      readJsonFloat(plane, "seen_pos", &seen_pos);
      if (seen_pos < 0.0f) seen_pos = 0.0f;
      if (seen_pos > 30.0f) seen_pos = 30.0f;
      parsed[n].pos_age_ms = static_cast<uint32_t>(seen_pos * 1000.0f);

      fillTagFields(&parsed[n], plane);
      if (config::kDebugLog) {
        const float dist_km =
            approxDistKm(center_lat, center_lon, parsed[n].lat, parsed[n].lon);
        if (!have_nearest || dist_km < nearest_km) {
          nearest_km = dist_km;
          have_nearest = true;
          nearest_lat = parsed[n].lat;
          nearest_lon = parsed[n].lon;
          strncpy(nearest_id, parsed[n].callsign, sizeof(nearest_id) - 1);
          nearest_id[sizeof(nearest_id) - 1] = '\0';
        }
      }
      ++n;
    }
  }

  publish(parsed, n);

  if (!config::kDebugLog) {
    Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
    return true;
  }

  char msg[49];
  msg[0] = '\0';
  if (doc["msg"].is<const char*>()) {
    strncpy(msg, doc["msg"].as<const char*>(), sizeof(msg) - 1);
    msg[sizeof(msg) - 1] = '\0';
  }
  int feed_total = -1;
  if (doc["total"].is<int>() || doc["total"].is<float>()) {
    feed_total = doc["total"].as<int>();
  }

  Serial.printf(
      "adsb: kept %u/%u (ground %u, no-pos %u, cap %u)  bytes %u %s%s  "
      "total %d  msg \"%s\"\n",
      static_cast<unsigned>(n), static_cast<unsigned>(in_feed),
      static_cast<unsigned>(skip_ground), static_cast<unsigned>(skip_pos),
      static_cast<unsigned>(skip_cap), static_cast<unsigned>(body.bytesRead()),
      framing_name, body.truncated() ? " truncated" : "", feed_total, msg);
  if (!ac_present) {
    Serial.println("adsb: response has no \"ac\" array");
  }
  if (n == 0 && in_feed > 0 && skip_ground == in_feed) {
    Serial.println("adsb: all aircraft are on the ground and hidden");
  }
  if (have_nearest) {
    Serial.printf(
        "adsb: nearest %s at %.6f,%.6f (%.1f km, fetch radius %.1f km)\n",
        nearest_id[0] != '\0' ? nearest_id : "?", nearest_lat, nearest_lon,
        nearest_km, fetch_radius_km);
  }
  return true;
}

}  // namespace services::adsb
