/*
================================================================================
  XIAO ESP32-S3 SENSE - CAMERA & VIDEO RECORDING SYSTEM
================================================================================
  
  HARDWARE: Seeed XIAO ESP32-S3 Sense (OV2640 Camera)
  
  MAIN FEATURES:
  • Live MJPEG video streaming (15 FPS)
  • Single-shot image capture to SD card
  • Video recording (MJPEG + thumbnail + duration metadata)
  • WiFi connectivity with mDNS discovery
  • WebSocket real-time UI control
  • Touch button controls (capture, stream toggle)
  • Vibration motor feedback
  • Gallery with filtering and sorting
  • Thread-safe camera access (mutex protection)
  
================================================================================
  CODE STRUCTURE & LINE LOCATIONS:
================================================================================

  HARDWARE & CONFIGURATION:
    Lines 65-80:    Camera pin configuration (XIAO ESP32-S3 specific)
    Lines 82-87:    Control pins (touch, vibration motor, SD card)
    Lines 89-93:    WiFi & network configuration
    Lines 95-114:   Global state variables (WiFi, WiFi security, gallery, recording)
    
  MUTEX & THREADING:
    Lines 90:       Camera mutex for thread-safe access
    Lines 150-180:  Recording capture task (Core 1, lower priority)
    
  VIDEO RECORDING FUNCTIONS:
    Lines 150-167:  startRecording() - Initialize MJPEG file + metadata
    Lines 169-197:  stopRecording() - Finalize video + save duration metadata
    Lines 199-240:  recordingCaptureTask() - Independent recording thread
    Lines 242-252:  recordFrame() - Write MJPEG frame + save first frame as thumbnail
    
  CAMERA ACCESS:
    Lines 289-310:  captureStreamFrame() - Get frame for streaming (with mutex)
    Lines 312-352:  captureFrame() - Capture standalone photo (with mutex)
    Lines 354-380:  initCamera() - Initialize OV2640 with QVGA settings
    Lines 382-415:  updateCameraSetting() - Adjust exposure, brightness, etc.
    
  WEB INTERFACE & HTTP HANDLERS:
    Lines 417-598:  HTML/CSS/JavaScript (full web UI in string)
    Lines 600-610:  loadGalleryThumbnails() - Fetch gallery from server
    Lines 605-650:  updateGalleryDisplay() - Render gallery with filtering/sorting
    Lines 612-640:  Sort & filter functions (applySorting, extractTimestamp)
    
  HTTP ENDPOINTS:
    Lines 657-760:  handleStream() - Live MJPEG video stream + server.handleClient()
    Lines 762-781:  handleRoot() - Serve HTML page
    Lines 782-800:  handleCapture() - Single photo capture (WebSocket command)
    Lines 802-850:  handleImage() - Serve captured images/thumbnails
    Lines 851-900:  handleGallery() - Return gallery JSON (cached, no SD scan)
    Lines 901-950:  handleGalleryImage() - Serve images/videos from SD
    Lines 951-1000: handleDeleteImage() - Delete single file
    Lines 1001-1050:handleDeleteAll() - Delete all files
    
  GALLERY CACHE:
    Lines 801-848:  rebuildGalleryCache() - Scan SD, skip thumbnails, include duration
    Lines 849-855:  handleGallery() - Return cached JSON (O(1) instead of O(n))
    
  WEBSOCKET COMMUNICATION:
    Lines 876-945:  webSocketEvent() - Handle real-time commands from UI
    Lines 947-950:  sendToAllClients() - Broadcast JSON to all WebSocket clients
    
  WIFI & NETWORKING:
    Lines 953-1000: connectWiFi() - WiFi setup with 20s timeout, 15dBm power
    Lines 1001-1100:setup() - Initialize WiFi, camera, SD, servers, tasks
    Lines 1232-1300:loop() - Handle WiFi reconnection, gallery updates, WebSocket
    
  UTILITY FUNCTIONS:
    Lines 1050-1100:getTimestamp() - Return YYYYMMDD_HHMMSS format
    Lines 1101-1130:handleFavicon(), handleNotFound()
    
================================================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "secrets.h"
#include <time.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <ESPmDNS.h>

// Camera pin configuration (XIAO ESP32-S3 Sense)
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    10
#define SIOD_GPIO_NUM    40
#define SIOC_GPIO_NUM    39
#define Y9_GPIO_NUM      48
#define Y8_GPIO_NUM      11
#define Y7_GPIO_NUM      12
#define Y6_GPIO_NUM      14
#define Y5_GPIO_NUM      16
#define Y4_GPIO_NUM      18
#define Y3_GPIO_NUM      17
#define Y2_GPIO_NUM      15
#define VSYNC_GPIO_NUM   38
#define HREF_GPIO_NUM    47
#define PCLK_GPIO_NUM    13

// SD card pin configuration
#define SD_CS 21  // SD card chip select pin
bool sdCardReady = false;  // Track if SD card is successfully initialized

// NTP time synchronization
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800;      // IST (GMT+5:30)
const int   daylightOffset_sec = 0;

// Physical input/output pins
#define TOUCH_PIN 2        // Capacitive touchpad
#define VIB_PIN 4         // Vibration motor
#define TOUCH_THRESHOLD 20000
#define TOUCH_DEBOUNCE 500
#define LONG_HOLD_TIME 3000
int vibrationIntensity = 120;
bool isTouched = false;
unsigned long lastTouchTime = 0;
unsigned long lastTouchReleasedTime = 0;
int touchCounter = 0;
const int DOUBLE_TAP_DELAY = 400;
volatile bool touchEnabled = false;
volatile bool longHoldTriggered = false;

// Camera access synchronization
SemaphoreHandle_t camMutex;

// Video recording variables
File videoFile;
volatile bool isRecording = false;
unsigned long videoStartTime = 0;
unsigned long lastFrameMillis = 0;
int frameCount = 0;
bool thumbnailSaved = false;
String currentThumbPath = "";
int recordedFPS = 15;  // Same as streaming FPS

WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);

volatile bool streamActive = false;
volatile bool captureRequested = false;
int photoIndex = 1;
std::vector<uint8_t> streamFrameBuffer;
std::vector<uint8_t> lastCapturedImage;
std::vector<std::vector<uint8_t>> imageGallery;
const int MAX_GALLERY_SIZE = 10;
unsigned long lastFrameTime = 0;

sensor_t * sensor = NULL;

// WiFi connection state tracking
unsigned long lastWiFiCheck = 0;
unsigned long wifiConnectionTime = 0;
bool wifiConnected = false;
int consecutiveWiFiFailures = 0;
const int MAX_WIFI_RETRIES = 3;

// mDNS state tracking
bool mdnsStarted = false;

// Gallery JSON cache - rebuilt only when files change (reduces SD card access)
String galleryCache = "";
bool galleryDirty = true;  // Flag to rebuild cache only when needed

// Function declarations
void initCamera();
void handleRoot();
void handleFavicon();
void handleStream();
void handleCapture();
void handleImage();
void handleGallery();
void handleGalleryImage();
void handleDeleteImage();
void handleDeleteAll();
void startRecording();
void stopRecording();
void recordFrame(camera_fb_t * fb);
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);
void sendToAllClients(String json);
void captureFrame();
void captureStreamFrame();
void updateCameraSetting(const char* setting, int value);
void rebuildGalleryCache();  // Scans SD and rebuilds the gallery cache

// Video recording functions
void startRecording() {
  if (isRecording) return;

  String path = "/" + getTimestamp() + ".mjpeg";
  videoFile = SD.open(path.c_str(), FILE_WRITE);

  if (!videoFile) {
    Serial.println("[REC] Failed to open MJPEG file");
    return;
  }

  isRecording = true;
  frameCount = 0;
  videoStartTime = millis();
  thumbnailSaved = false;
  currentThumbPath = "/thumb_" + getTimestamp() + ".jpg";

  Serial.printf("[REC] Started MJPEG: %s\n", path.c_str());
  sendToAllClients("{\"action\":\"recording_status\",\"status\":\"started\"}");
}

void stopRecording() {
  if (!isRecording) return;

  isRecording = false;

  if (videoFile) {
    videoFile.flush();
    videoFile.close();
  }

  int durationSec = frameCount / recordedFPS;
  
  // Write duration to metadata file for gallery display
  String metaPath = currentThumbPath;
  metaPath.replace("thumb_", "meta_");
  metaPath.replace(".jpg", ".txt");

  File meta = SD.open(metaPath.c_str(), FILE_WRITE);
  if (meta) {
    meta.print(durationSec);
    meta.close();
    Serial.printf("[REC] Duration metadata saved: %s (%d sec)\n", metaPath.c_str(), durationSec);
  }

  Serial.printf("[REC] Stopped MJPEG. Frames: %d, Duration: %d sec\n", frameCount, durationSec);
  galleryDirty = true;

  sendToAllClients("{\"action\":\"recording_status\",\"status\":\"stopped\"}");
  sendToAllClients("{\"action\":\"refresh_gallery\"}");
}
// Recording thread - captures frames to SD independently from streaming
// Runs on Core 1 with lower priority to avoid blocking WiFi operations
void recordingCaptureTask(void *pvParameters) {
  static bool taskStarted = false;
  
  while (true) {
    if (isRecording && videoFile) {
      if (frameCount == 0 && !taskStarted) {
        Serial.println("[TASK] Recording task started capturing frames");
        taskStarted = true;
      }
      
      // Skip if stream is active (stream loop handles recording)
      if (!streamActive) {
        if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(50))) {
          camera_fb_t * fb = esp_camera_fb_get();
          if (fb) {
            recordFrame(fb);
            esp_camera_fb_return(fb);
            
            if (frameCount % 10 == 0) {
              Serial.printf("[TASK] Recording: %d frames captured\n", frameCount);
            }
          } else {
            Serial.println("[TASK] Failed to get frame buffer");
          }
          xSemaphoreGive(camMutex);
        }
        vTaskDelay(40 / portTICK_PERIOD_MS);
      } else {
        vTaskDelay(5 / portTICK_PERIOD_MS);
      }
    } else {
      if (taskStarted && !isRecording) {
        Serial.println("[TASK] Recording task idle");
        taskStarted = false;
      }
      vTaskDelay(50 / portTICK_PERIOD_MS);
    }
  }
}

void recordFrame(camera_fb_t * fb) {
  if (!isRecording || !fb || !videoFile) return;

  // Save first recorded frame as video thumbnail
  if (!thumbnailSaved) {
    File thumb = SD.open(currentThumbPath.c_str(), FILE_WRITE);
    if (thumb) {
      thumb.write(fb->buf, fb->len);
      thumb.close();
      thumbnailSaved = true;
      Serial.printf("[THUMB] Saved thumbnail: %s\n", currentThumbPath.c_str());
    }
  }

  // Write MJPEG frame with multipart boundaries
  videoFile.print("--frame\r\n");
  videoFile.print("Content-Type: image/jpeg\r\n");
  videoFile.printf("Content-Length: %u\r\n\r\n", fb->len);
  videoFile.write(fb->buf, fb->len);
  videoFile.print("\r\n");

  frameCount++;

  // Flush to SD card periodically for safety
  if (frameCount % 20 == 0) {
    videoFile.flush();
  }
}

// Camera initialization
void initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 15;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("[ERROR] Camera init failed!");
    return;
  }

  sensor = esp_camera_sensor_get();
  sensor->set_brightness(sensor, 0);
  sensor->set_contrast(sensor, 0);
  sensor->set_saturation(sensor, 0);
  sensor->set_special_effect(sensor, 0);
  sensor->set_whitebal(sensor, 1);
  sensor->set_awb_gain(sensor, 1);
  sensor->set_wb_mode(sensor, 0);
  sensor->set_exposure_ctrl(sensor, 1);
  sensor->set_aec_value(sensor, 300);
  sensor->set_gain_ctrl(sensor, 1);
  sensor->set_agc_gain(sensor, 0);
  sensor->set_gainceiling(sensor, GAINCEILING_2X);
  sensor->set_bpc(sensor, 1);
  sensor->set_wpc(sensor, 1);
  sensor->set_raw_gma(sensor, 1);
  sensor->set_lenc(sensor, 1);
  sensor->set_hmirror(sensor, 0);
  sensor->set_vflip(sensor, 0);

  delay(200);
  
  Serial.println("[OK] Camera initialized successfully");
}

// Timestamp utility
String getTimestamp() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return String(millis());
  }
  char timeStringBuff[20];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y%m%d_%H%M%S", &timeinfo);
  return String(timeStringBuff);
}

// Capture frame for streaming (temporary buffer only)
// Get frame for live streaming - uses mutex for thread-safe camera access
void captureStreamFrame() {
  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(50))) {
    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      xSemaphoreGive(camMutex);
      return;
    }

    streamFrameBuffer.assign(fb->buf, fb->buf + fb->len);
    
    // Record frame to video if recording is active
    if (isRecording) {
      recordFrame(fb);
    }

    // Save still image if requested by UI
    if (captureRequested) {
      String path = "/" + getTimestamp() + ".jpg";
      File file = SD.open(path.c_str(), FILE_WRITE);
      if (file) {
        file.write(fb->buf, fb->len);
        file.close();
        Serial.printf("[OK] Saved during stream: %s\n", path.c_str());
        // Mark gallery for refresh since new file was saved
        galleryDirty = true;
        sendToAllClients("{\"action\":\"refresh_gallery\"}");
      }
      captureRequested = false;
    }

    esp_camera_fb_return(fb);
    xSemaphoreGive(camMutex);
  }
}

// Capture and save image to SD card
// Capture standalone photo - independent from streaming
// Uses mutex for thread-safe camera access
void captureFrame() {
  if (streamActive) {
    captureRequested = true;
    Serial.println("[OK] Capture flagged for Stream Loop");
    return;
  }
  
  if (xSemaphoreTake(camMutex, pdMS_TO_TICKS(100))) {
    // Discard stale frames to get fresh image from sensor
    for (int i = 0; i < 2; i++) {
      camera_fb_t * stale_fb = esp_camera_fb_get();
      if (stale_fb) esp_camera_fb_return(stale_fb);
    }
    delay(20);

    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[ERROR] Cold capture failed");
      xSemaphoreGive(camMutex);
      return;
    }

    String path = "/" + getTimestamp() + ".jpg";
    File file = SD.open(path.c_str(), FILE_WRITE);
    if (file) {
      file.write(fb->buf, fb->len);
      file.close();
      lastCapturedImage.assign(fb->buf, fb->buf + fb->len);
      Serial.printf("[OK] Fresh Capture Saved: %s\n", path.c_str());
      // Mark gallery dirty to trigger refresh
      galleryDirty = true;
    }

    esp_camera_fb_return(fb);
    xSemaphoreGive(camMutex);
  }
  
  sendToAllClients("{\"action\":\"refresh_gallery\"}");
}

// HTML web interface
const char INDEX_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Professional Camera System</title>
    <style>
        @keyframes fadeIn{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:translateY(0)}}@keyframes slideUp{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}@keyframes subtlePulse{0%,100%{opacity:1}50%{opacity:0.8}}*{margin:0;padding:0;box-sizing:border-box}html,body{height:100%;overflow:hidden;background:#0f172a}body{font-family:'Segoe UI','Inter','-apple-system','BlinkMacSystemFont','Helvetica Neue','Roboto',sans-serif;background:#0f172a;color:#cbd5e1;display:flex;flex-direction:column}.header{background:#111827;border-bottom:1px solid #1e293b;padding:16px 24px;display:flex;align-items:center;justify-content:space-between;box-shadow:0 1px 3px rgba(0,0,0,0.12)}.header h1{font-size:20px;margin:0;font-weight:600;color:#f1f5f9;letter-spacing:-0.3px}.tab-buttons{background:#0f172a;border-bottom:1px solid #1e293b;padding:0 16px;display:flex;gap:0}.tab-btn{background:none;border:none;padding:12px 20px;color:#94a3b8;cursor:pointer;font-weight:600;font-size:12px;text-transform:uppercase;letter-spacing:0.5px;border-bottom:3px solid transparent;transition:all 0.2s ease}.tab-btn.active{color:#2563eb;border-bottom-color:#2563eb}.tab-btn:hover{color:#cbd5e1}.main-container{display:flex;flex:1;gap:16px;padding:16px;overflow:hidden}.tab-content{display:none;flex:1;overflow:hidden}.tab-content.active{display:flex}.feed-layout{display:flex;gap:16px;width:100%;height:100%}.video-section{flex:1.5;display:flex;flex-direction:column;gap:12px}.video-header{display:flex;align-items:center;justify-content:space-between;padding:0 4px}.video-title{font-size:13px;font-weight:600;color:#94a3b8;text-transform:uppercase;letter-spacing:0.5px}.stream-status{display:flex;align-items:center;gap:6px;font-size:12px;color:#94a3b8}.stream-status-dot{width:8px;height:8px;border-radius:50%;background:#64748b;transition:all 0.3s ease}.stream-status-dot.active{background:#22c55e;box-shadow:0 0 8px rgba(34,197,94,0.4)}.stream-status-dot.idle{background:#78716c}.video-container{background:#111827;border:1px solid #1e293b;border-radius:8px;overflow:hidden;flex:1;display:flex;justify-content:center;align-items:center;box-shadow:0 4px 12px rgba(0,0,0,0.3)}.video-container.inactive{border-color:#334155}.no-stream{color:#64748b;text-align:center;font-size:14px;font-weight:500}#stream{width:100%;height:100%;object-fit:contain}#stream.stream-active{border-radius:0}.control-section{width:380px;background:#111827;border:1px solid #1e293b;border-radius:8px;display:flex;flex-direction:column;overflow:hidden;box-shadow:0 4px 12px rgba(0,0,0,0.3)}.control-header{background:#0f172a;border-bottom:1px solid #1e293b;padding:14px 16px;display:flex;align-items:center}.control-title{font-size:12px;font-weight:600;color:#94a3b8;text-transform:uppercase;letter-spacing:0.5px;margin:0}.control-content{flex:1;overflow-y:auto;padding:16px;display:flex;flex-direction:column;gap:12px}.control-content::-webkit-scrollbar{width:6px}.control-content::-webkit-scrollbar-track{background:transparent}.control-content::-webkit-scrollbar-thumb{background:#334155;border-radius:3px}.control-content::-webkit-scrollbar-thumb:hover{background:#475569}.section-card{display:flex;flex-direction:column;gap:10px;padding:14px;background:#0f172a;border:1px solid #1e293b;border-radius:6px;animation:slideUp 0.4s ease}.section-label{font-size:10px;font-weight:600;color:#94a3b8;text-transform:uppercase;letter-spacing:0.4px}.btn{padding:10px 14px;font-size:11px;font-weight:600;border:none;border-radius:6px;cursor:pointer;transition:all 0.2s ease;text-transform:uppercase;letter-spacing:0.4px;display:flex;align-items:center;justify-content:center;gap:6px;position:relative;white-space:nowrap}.btn:disabled{opacity:0.5;cursor:not-allowed}.btn-start{background:#2563eb;color:white;box-shadow:0 2px 8px rgba(37,99,235,0.2)}.btn-start:hover:not(:disabled){background:#1d4ed8;box-shadow:0 4px 12px rgba(37,99,235,0.3)}.btn-stop{background:#dc2626;color:white;box-shadow:0 2px 8px rgba(220,38,38,0.2)}.btn-stop:hover:not(:disabled){background:#b91c1c;box-shadow:0 4px 12px rgba(220,38,38,0.3)}.btn-capture{background:#22c55e;color:white;box-shadow:0 2px 8px rgba(34,197,94,0.2)}.btn-capture:hover:not(:disabled){background:#16a34a;box-shadow:0 4px 12px rgba(34,197,94,0.3)}.btn-capture:disabled{background:#64748b;box-shadow:0 2px 8px rgba(100,116,139,0.2)}.btn-record{background:#f59e0b;color:white;box-shadow:0 2px 8px rgba(245,158,11,0.2)}.btn-record:hover{background:#d97706;box-shadow:0 4px 12px rgba(245,158,11,0.3)}.recording-pulse{animation:subtlePulse 1s infinite;background:#dc2626 !important}.status-panel{padding:12px;background:#0f172a;border:1px solid #1e293b;border-radius:6px;display:flex;flex-direction:column;gap:8px;font-size:11px}.status-item{display:flex;align-items:center;justify-content:space-between}.status-label{color:#94a3b8}.status-value{color:#e2e8f0;font-weight:600;font-family:'SF Mono','Monaco','Courier New',monospace;font-size:10px}.controls-group{display:flex;flex-direction:column;gap:8px}.gallery-section{flex:1;display:flex;flex-direction:column;gap:12px}.gallery-header{display:flex;align-items:center;justify-content:space-between;padding:0 4px}.gallery-label{font-size:13px;font-weight:600;color:#94a3b8;text-transform:uppercase;letter-spacing:0.5px;margin:0}.gallery-actions{display:flex;gap:8px;align-items:center}.gallery-count{background:#2563eb;color:white;padding:4px 12px;border-radius:4px;font-size:11px;font-weight:600;min-width:28px;text-align:center}.gallery-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:12px;flex:1;overflow-y:auto;padding-right:4px}.gallery-grid::-webkit-scrollbar{width:6px}.gallery-grid::-webkit-scrollbar-track{background:transparent}.gallery-grid::-webkit-scrollbar-thumb{background:#334155;border-radius:3px}.gallery-item{position:relative;background:#1e293b;border:2px solid transparent;border-radius:10px;overflow:hidden;aspect-ratio:1;transition:transform 0.2s,border-color 0.2s;cursor:pointer;display:flex;flex-direction:column;justify-content:flex-end}.gallery-info{position:relative;width:100%;background:rgba(0,0,0,0.75);color:white;font-size:12px;padding:8px;text-align:center;backdrop-filter:blur(3px);word-wrap:break-word;white-space:normal;line-height:1.3}.gallery-item:hover{transform:translateY(-3px);border-color:#3b82f6}.gallery-item img{width:100%;height:auto;display:block;object-fit:cover}.gallery-item-overlay{position:absolute;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.7);display:flex;justify-content:center;align-items:center;gap:4px;opacity:0;transition:opacity 0.2s ease;backdrop-filter:blur(2px)}.gallery-item:hover .gallery-item-overlay{opacity:1}.gallery-btn{background:white;border:none;padding:6px;border-radius:4px;cursor:pointer;transition:all 0.2s ease;display:flex;align-items:center;justify-content:center;color:#111827}.gallery-btn:hover{transform:scale(1.1);background:#e2e8f0}.gallery-btn.delete{background:#ef4444;color:white}.gallery-btn.delete:hover{background:#dc2626;transform:scale(1.1)}.empty-gallery{text-align:center;padding:24px 16px;color:#64748b;font-size:12px;display:flex;align-items:center;justify-content:center;height:100%}.modal{display:none;position:fixed;z-index:1000;left:0;top:0;width:100%;height:100%;background:rgba(0,0,0,0.8);animation:fadeIn 0.2s ease;backdrop-filter:blur(4px)}.modal.show{display:flex;justify-content:center;align-items:center}.modal-content{background:#111827;max-width:90vw;max-height:90vh;border-radius:8px;overflow:auto;position:relative;border:1px solid #1e293b;box-shadow:0 20px 48px rgba(0,0,0,0.4)}.modal-close{position:absolute;top:12px;right:12px;background:#0f172a;border:1px solid #1e293b;color:#94a3b8;font-size:20px;cursor:pointer;padding:4px 10px;border-radius:4px;transition:all 0.2s ease;z-index:1001;width:32px;height:32px;display:flex;align-items:center;justify-content:center}.modal-close:hover{background:#1e293b;color:#e2e8f0}.modal img{width:100%;height:auto;display:block}@media (max-width:1600px){.control-section{width:340px}.feed-layout{gap:12px}}@media (max-width:1200px){.feed-layout{flex-direction:column}.video-section{flex:none;height:50%}.control-section{width:100%;height:50%}}
    .video-container{position:relative}#recordingDisplay{position:absolute;top:8px;left:8px;background-color:rgba(220,38,38,0.85);color:white;padding:4px 10px;border-radius:5px;font-size:14px;font-weight:700;font-family:'SF Mono','Monaco','Courier New',monospace;letter-spacing:1px;box-shadow:0 2px 5px rgba(0,0,0,0.4);z-index:10;display:none}</style>
</head>
<body>
    <div class="header">
        <h1>Camera Control System</h1>
    </div>

    <div class="tab-buttons">
        <button class="tab-btn active" onclick="switchTab('feed')">📹 Live Feed</button>
        <button class="tab-btn" onclick="switchTab('gallery')">🎬 Gallery</button>
    </div>

    <div class="main-container">
        <!-- FEED TAB -->
        <div id="feed-tab" class="tab-content active">
            <div class="feed-layout">
                <div class="video-section">
                    <div class="video-header">
                        <h2 class="video-title">📷 Live Camera Feed</h2>
                        <div class="stream-status">
                            <div class="stream-status-dot active" id="statusDot"></div>
                            <span id="streamStatusText">LIVE</span>
                        </div>
                    </div>
                    <div class="video-container" id="videoContainer">
                        <img id="stream" src="" alt="Live Stream" style="display:none">
                        <div class="no-stream" id="noStream">Stream Inactive</div>
                        <div id="recordingDisplay">00:00:00</div>
                    </div>
                </div>

                <div class="control-section">
                    <div class="control-header">
                        <span class="control-title">⚙️ Controls & Status</span>
                    </div>

                    <div class="control-content">
                        <div class="section-card">
                            <div class="section-label">Stream Control</div>
                            <div class="controls-group">
                                <button class="btn btn-start" id="btnStart" style="width:100%">▶ Start Stream</button>
                                <button class="btn btn-stop" id="btnStop" style="display:none;width:100%">⏹ Stop Stream</button>
                            </div>
                        </div>

                        <div class="section-card">
                            <div class="section-label">📸 Capture</div>
                            <button class="btn btn-capture" id="btnCapture" style="width:100%">📸 Capture Photo</button>
                        </div>

                        <div class="section-card">
                            <div class="section-label">🎥 Recording</div>
                            <button class="btn btn-record" id="btnRecord" style="width:100%">⏺ Start Recording</button>
                        </div>

                        <div class="section-card">
                            <div class="section-label">📊 Live Status</div>
                            <div class="status-panel">
                                <div class="status-item">
                                    <span class="status-label">Stream:</span>
                                    <span class="status-value" id="statusStream">STOPPED</span>
                                </div>
                                <div class="status-item">
                                    <span class="status-label">Recording:</span>
                                    <span class="status-value" id="statusRecording">OFF</span>
                                </div>
                                <div class="status-item">
                                    <span class="status-label">Connection:</span>
                                    <span class="status-value" id="connValue">OFFLINE</span>
                                </div>
                                <div class="status-item">
                                    <span class="status-label">Frames:</span>
                                    <span class="status-value" id="statusFrames">0</span>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>
            </div>
        </div>

        <!-- GALLERY TAB -->
        <div id="gallery-tab" class="tab-content" style="flex-direction:column;">
            <div class="gallery-section" style="flex:1;">
                <div class="gallery-header">
                    <span class="gallery-label">Gallery</span>
                    <div class="gallery-actions">
                        <select id="sortType" onchange="applySorting()" style="background:#1e293b; color:#e2e8f0; border:1px solid #334155; border-radius:4px; padding:4px 8px; font-size:10px; cursor:pointer;">
                          <option value="all">All</option>
                          <option value="video">Videos</option>
                          <option value="image">Images</option>
                        </select>
                        <select id="sortOrder" onchange="applySorting()" style="background:#1e293b; color:#e2e8f0; border:1px solid #334155; border-radius:4px; padding:4px 8px; font-size:10px; cursor:pointer;">
                          <option value="newest">Newest first</option>
                          <option value="oldest">Oldest first</option>
                        </select>
                        <button onclick="deleteAllImages()" style="background:#dc2626; color:white; border:none; border-radius:4px; padding:4px 12px; font-size:10px; cursor:pointer; font-weight:600;">DELETE ALL</button>
                        <span class="gallery-count" id="galleryCount">0</span>
                    </div>
                </div>
                
                <!-- SD Card Storage Information -->
                <div class="storage-panel status-panel" style="background:#0f172a; border:1px solid #1e293b; border-radius:6px; padding:12px; margin-bottom:8px; display:none;">
                    <div class="status-item">
                        <span class="status-label">Total Storage:</span>
                        <span class="status-value" id="storageTotal">0 MB</span>
                    </div>
                    <div class="status-item">
                        <span class="status-label">Used Space:</span>
                        <span class="status-value" id="storageUsed">0 MB</span>
                    </div>
                    <div class="status-item">
                        <span class="status-label">Available:</span>
                        <span class="status-value" id="storageAvailable">0 MB</span>
                    </div>
                    <div style="width:100%; background:#1e293b; border-radius:4px; height:8px; margin-top:8px; overflow:hidden;">
                        <div id="storageBar" style="height:100%; background:#3b82f6; width:0%; transition:width 0.3s ease;"></div>
                    </div>
                </div>
                
                <div id="galleryContainer" class="gallery-grid">
                    <div class="empty-gallery">No captures</div>
                </div>
            </div>
        </div>
    </div>

    <div id="imageModal" class="modal">
        <div class="modal-content">
            <button class="modal-close" onclick="closeModal()">&times;</button>
            <img id="modalImage" src="" alt="Full view" style="width:100%; display:block;">
            <h3 id="modalTitle" style="color:white; text-align:center; padding:10px; font-size:14px;"></h3>
        </div>
    </div>

    <div id="videoModal" class="modal">
        <div class="modal-content">
            <button class="modal-close" onclick="closeVideoModal()">&times;</button>
            <video id="player" controls width="100%" style="background:#000;"></video>
        </div>
    </div>

    <script>
function switchTab(tabName){const tabs=document.querySelectorAll('.tab-content');const buttons=document.querySelectorAll('.tab-btn');tabs.forEach(t=>t.classList.remove('active'));buttons.forEach(b=>b.classList.remove('active'));document.getElementById(tabName+'-tab').classList.add('active');event.target.classList.add('active')}
let ws;let wsReconnectAttempt=0;const MAX_RECONNECT_ATTEMPTS=10;const RECONNECT_DELAYS=[1000,2000,4000,8000,8000,8000,8000,8000,8000,8000];function initWebSocket(){console.log('[WS] Reconnect attempt '+(wsReconnectAttempt+1)+'/'+MAX_RECONNECT_ATTEMPTS);try{ws=new WebSocket('ws://'+window.location.hostname+':81/');ws.onopen=function(){console.log('[WS] Connected');wsReconnectAttempt=0;statusDot.classList.add('active');statusDot.classList.remove('idle','warning');connValue.textContent='Connected';loadGalleryThumbnails()};ws.onmessage=function(event){try{const data=JSON.parse(event.data);if(data.action==='stream_toggle'){if(data.state==='started'){startStreamDisplay()}else if(data.state==='stopped'){stopStreamDisplay()}}if(data.action==='refresh_gallery'||data.status==='physical_capture'){loadGalleryThumbnails()}if(data.stream==='started'){streamStatusText.textContent='Active';statusStream.textContent='STREAMING'}else if(data.stream==='stopped'){streamStatusText.textContent='Inactive';statusStream.textContent='STOPPED'}}catch(e){console.error('[WS] Parse error:',e)}};ws.onerror=function(error){console.error('[WS] Error:',error);statusDot.classList.add('warning');statusDot.classList.remove('active','idle');connValue.textContent='Connection Error'};ws.onclose=function(){console.log('[WS] Closed');statusDot.classList.remove('active');statusDot.classList.add('idle');connValue.textContent='Offline';if(wsReconnectAttempt<MAX_RECONNECT_ATTEMPTS){const delay=RECONNECT_DELAYS[wsReconnectAttempt];wsReconnectAttempt++;console.log('[WS] Reconnecting in '+delay+'ms');setTimeout(initWebSocket,delay)}else{console.error('[WS] Max reconnects reached');connValue.textContent='Offline - Max Retries'}}}catch(e){console.error('[WS] Creation failed:',e);statusDot.classList.add('warning');connValue.textContent='Connection Failed';if(wsReconnectAttempt<MAX_RECONNECT_ATTEMPTS){const delay=RECONNECT_DELAYS[wsReconnectAttempt];wsReconnectAttempt++;setTimeout(initWebSocket,delay)}}}
const stream=document.getElementById('stream');
const noStream=document.getElementById('noStream');
const recordingDisplay=document.getElementById('recordingDisplay');
const btnStart=document.getElementById('btnStart');
const btnStop=document.getElementById('btnStop');
const btnCapture=document.getElementById('btnCapture');
const statusDot=document.getElementById('statusDot');
const streamStatusText=document.getElementById('streamStatusText');
const statusStream=document.getElementById('statusStream');
const connValue=document.getElementById('connValue');
const videoContainer=document.getElementById('videoContainer');
const galleryContainer=document.getElementById('galleryContainer');
const galleryCount=document.getElementById('galleryCount');
const imageModal=document.getElementById('imageModal');
const modalImage=document.getElementById('modalImage');
const btnRecord=document.getElementById('btnRecord');
const videoModal=document.getElementById('videoModal');
const player=document.getElementById('player');

let streamRunning=false;
let galleryImages=[];
let recording=false;
let recordingStartTime=0;
let recordingTimerInterval=null;

initWebSocket();function wsConnected(){return ws&&ws.readyState===WebSocket.OPEN}function checkConnection(){if(!wsConnected()){connValue.textContent='Offline - Waiting...';return false}return true}btnStart.addEventListener('click',function(){if(checkConnection()){ws.send(JSON.stringify({action:'stream_start'}));startStreamDisplay()}else{statusStream.textContent='Connection Lost'}});
btnStop.addEventListener('click',function(){if(checkConnection()){ws.send(JSON.stringify({action:'stream_stop'}));stopStreamDisplay()}else{statusStream.textContent='Connection Lost'}});
btnCapture.addEventListener('click',function(){if(checkConnection()){btnCapture.disabled=true;ws.send(JSON.stringify({action:'capture'}));statusStream.textContent='Saving...';setTimeout(function(){loadGalleryThumbnails();statusStream.textContent='Captured';btnCapture.disabled=false},600)}else{statusStream.textContent='Connection Lost'}});
btnRecord.onclick=function(){if(!checkConnection()){statusStream.textContent='Connection Lost';return}recording=!recording;ws.send(JSON.stringify({action:recording?'record_start':'record_stop'}));btnRecord.textContent=recording?'STOP RECORDING':'RECORD VIDEO';btnRecord.classList.toggle('recording-pulse',recording);if(recording){recordingStartTime=Date.now();recordingDisplay.style.display='block';recordingTimerInterval=setInterval(function(){const elapsedMs=Date.now()-recordingStartTime;const hours=Math.floor(elapsedMs/3600000);const minutes=Math.floor((elapsedMs%3600000)/60000);const seconds=Math.floor((elapsedMs%60000)/1000);recordingDisplay.textContent='REC '+String(hours).padStart(2,'0')+':'+String(minutes).padStart(2,'0')+':'+String(seconds).padStart(2,'0');recordingDisplay.style.opacity=(seconds%2===0)?'1':'0.6'},100)}else{clearInterval(recordingTimerInterval);recordingDisplay.style.display='none'}};
function startStreamDisplay(){console.log('[Stream] Starting stream display');streamRunning=true;btnStart.style.display='none';btnStop.style.display='flex';noStream.style.display='none';stream.style.display='block';stream.src='';setTimeout(function(){stream.src='http://'+window.location.hostname+':80/stream';console.log('[Stream] Set src to: http://'+window.location.hostname+':80/stream')},10);stream.classList.add('stream-active');videoContainer.classList.remove('inactive');btnCapture.disabled=false;streamStatusText.textContent='Active';statusStream.textContent='STREAMING'}
function stopStreamDisplay(){console.log('[Stream] Stopping stream display');streamRunning=false;btnStart.style.display='flex';btnStop.style.display='none';stream.src='';stream.style.display='none';stream.classList.remove('stream-active');noStream.style.display='block';videoContainer.classList.add('inactive');streamStatusText.textContent='Inactive';statusStream.textContent='STOPPED'}
function loadGalleryThumbnails(){fetch('/gallery').then(function(r){return r.json()}).then(function(data){var newImages=data.images||[];var storageData=data.storage||{total:0,used:0};if(newImages.length===0&&galleryImages.length===0){updateStorageDisplay(storageData);return}if(newImages.length!==galleryImages.length){galleryImages=newImages;updateGalleryDisplay();updateStorageDisplay(storageData)}else if(newImages.length>0&&galleryImages.length>0){var changed=false;for(let i=0;i<newImages.length;i++){if(newImages[i].size!==galleryImages[i].size){changed=true;break}}if(changed){galleryImages=newImages;updateGalleryDisplay();updateStorageDisplay(storageData)}}else{updateStorageDisplay(storageData)}}).catch(function(e){console.error('Gallery fetch error:',e)})}
function updateStorageDisplay(storageData){if(!storageData||storageData.total===0||storageData.total===undefined){document.querySelector('.storage-panel').style.display='none';return}document.querySelector('.storage-panel').style.display='block';var totalBytes=storageData.total;var usedBytes=storageData.used;var availableBytes=totalBytes-usedBytes;var usagePercent=totalBytes>0?(usedBytes/totalBytes)*100:0;function formatBytes(bytes){if(bytes===0)return '0 B';var k=1024;var sizes=['B','KB','MB','GB'];var i=Math.floor(Math.log(bytes)/Math.log(k));return(bytes/Math.pow(k,i)).toFixed(1)+' '+sizes[i]}document.getElementById('storageTotal').textContent=formatBytes(totalBytes);document.getElementById('storageUsed').textContent=formatBytes(usedBytes);document.getElementById('storageAvailable').textContent=formatBytes(availableBytes);document.getElementById('storageBar').style.width=usagePercent+'%'}
function formatTimestamp(filename){var match=filename.match(/(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})/);if(match){return match[3]+'/'+match[2]+' '+match[4]+':'+match[5]}return 'Unknown'}
function extractTimestamp(name){const m=name.match(/(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})/);if(!m)return 0;return new Date(m[1]+'-'+m[2]+'-'+m[3]+'T'+m[4]+':'+m[5]+':'+m[6]).getTime()}
function applySorting(){const type=document.getElementById('sortType').value;const order=document.getElementById('sortOrder').value;let filtered=[...galleryImages];if(type==='video'){filtered=filtered.filter(i=>i.name.endsWith('.mjpeg'))}else if(type==='image'){filtered=filtered.filter(i=>i.name.endsWith('.jpg'))}filtered.sort((a,b)=>{const ta=extractTimestamp(a.name);const tb=extractTimestamp(b.name);return order==='newest'?tb-ta:ta-tb});updateGalleryDisplay(filtered)}
function updateGalleryDisplay(list=galleryImages){const count=list.length;galleryCount.textContent=count;if(count===0){galleryContainer.innerHTML='<div class="empty-gallery">No captures</div>'}else{let html='';for(let i=0;i<count;i++){const item=list[i];const imgName=item.name;const timestampStr=formatTimestamp(imgName);const isVideo=imgName.endsWith('.mjpeg');const imgUrl='http://'+window.location.hostname+':80/galleryImage?name='+imgName;const viewAction=isVideo?'playSavedVideo(\''+imgName+'\')':'viewImage('+i+')';const videoBadge=isVideo?'<div style="position:absolute;top:8px;left:8px;background:#ef4444;color:white;padding:2px 6px;font-size:10px;border-radius:3px;font-weight:bold;">VIDEO</div>':'';const durationBadge=isVideo&&item.duration>0?'<div style="position:absolute;bottom:8px;right:8px;background:rgba(0,0,0,0.8);color:white;padding:2px 6px;font-size:10px;border-radius:4px;font-weight:600;">'+Math.floor(item.duration/60)+':'+String(item.duration%60).padStart(2,'0')+'</div>':'';const thumbUrl=isVideo?'http://'+window.location.hostname+':80/galleryImage?name=thumb_'+imgName.replace('.mjpeg','.jpg'):imgUrl;html+='<div class="gallery-item">'+videoBadge+durationBadge+'<img data-src="'+thumbUrl+'" alt="Media" style="display:block;width:100%;height:100%;object-fit:cover;" onerror="this.src=\''+imgUrl+'\'"><div class="gallery-info">'+timestampStr+'</div><div class="gallery-item-overlay"><button class="gallery-btn" title="'+(isVideo?'Play':'View')+'" onclick="'+viewAction+'"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">'+(isVideo?'<polygon points="5 3 19 12 5 21 5 3"></polygon>':'<path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"></path><circle cx="12" cy="12" r="3"></circle>')+'</svg></button><button class="gallery-btn" title="Download" onclick="downloadFile(\''+imgName+'\')"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path><polyline points="7 10 12 15 17 10"></polyline><line x1="12" y1="15" x2="12" y2="3"></line></svg></button><button class="gallery-btn delete" title="Delete" onclick="deleteFile(\''+imgName+'\')"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"></polyline><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path><line x1="10" y1="11" x2="10" y2="17"></line><line x1="14" y1="11" x2="14" y2="17"></line></svg></button></div></div>'}galleryContainer.innerHTML=html;const imgs=galleryContainer.querySelectorAll('img[data-src]');imgs.forEach(img=>observer.observe(img))}}
function viewImage(idx){if(galleryImages[idx]){const imgName=galleryImages[idx].name;const imgUrl='http://'+window.location.hostname+':80/galleryImage?name='+imgName;modalImage.src=imgUrl;imageModal.classList.add('show')}}
function playSavedVideo(name){const url='http://'+window.location.hostname+':80/galleryImage?name='+name+'&t='+Date.now();window.open(url,'_blank')}
function downloadFile(name){const link=document.createElement('a');link.href='http://'+window.location.hostname+':80/galleryImage?name='+name;link.download=name;link.click()}
function deleteFile(name){if(confirm('Delete this file?')){fetch('/deleteImage?name='+name).then(function(){loadGalleryThumbnails()}).catch(function(e){console.error('Delete error:',e)})}}
function downloadImage(idx){if(galleryImages[idx]){const imgName=galleryImages[idx].name;const link=document.createElement('a');link.href='http://'+window.location.hostname+':80/galleryImage?name='+imgName;link.download=imgName;link.click()}}
function deleteImage(idx){if(confirm('Delete this image?')){const imgName=galleryImages[idx].name;fetch('/deleteImage?name='+imgName).then(function(){loadGalleryThumbnails()}).catch(function(e){console.error('Delete error:',e)})}}
function deleteAllImages(){if(confirm('Are you sure you want to delete ALL images from the SD card? This cannot be undone.')){const btn=event.target;btn.textContent='DELETING...';btn.disabled=true;fetch('/deleteAll').then(function(r){return r.json()}).then(function(data){btn.textContent='DELETE ALL';btn.disabled=false;loadGalleryThumbnails()}).catch(function(err){console.error('Delete all failed:',err);btn.disabled=false})}}
function closeModal(){imageModal.classList.remove('show')}
function closeVideoModal(){videoModal.classList.remove('show');player.pause();player.src=''}
imageModal.addEventListener('click',function(e){if(e.target===imageModal)closeModal()});
videoModal.addEventListener('click',function(e){if(e.target===videoModal)closeVideoModal()});
// Load gallery on page startup (offline-safe)
window.addEventListener('load', function() {
  loadGalleryThumbnails();
});
// Gallery updates via WebSocket events when files change (event-driven, not polling)
const observer = new IntersectionObserver(function(entries){
  entries.forEach(function(entry){
    if(entry.isIntersecting){
      const img = entry.target;
      if(img.dataset.src && !img.src){
        img.src = img.dataset.src;
        observer.unobserve(img);
      }
    }
  });
},{ rootMargin: '50px' });
console.log('[INIT] Camera Control System ready');
console.log('[INIT] WebSocket will connect to: ws://'+window.location.hostname+':81/');
    </script>
</body>
</html>
)html";

// 2. Now define the handler that uses that string
void handleRoot() {
  server.setContentLength(sizeof(INDEX_HTML));
  server.send(200, "text/html");
  server.sendContent_P(INDEX_HTML, sizeof(INDEX_HTML));
  server.sendContent("");
}

// Favicon handler
void handleFavicon() {
  server.send(204);
}

// Stream endpoint - MJPEG format with timeout handling
void handleStream() {
  Serial.println("[STREAM] Client connected - starting MJPEG stream");
  streamActive = true;
  sendToAllClients("{\"stream\":\"started\"}");

  WiFiClient client = server.client();
  if (!client.connected()) {
    Serial.println("[ERROR] Client disconnected immediately after connection");
    streamActive = false;
    return;
  }
  
  client.print("HTTP/1.1 200 OK\r\n");
  client.print("Content-Type: multipart/x-mixed-replace; boundary=frame\r\n");
  client.print("Connection: close\r\n\r\n");

  // Use 15 FPS instead of 30 for reliable streaming at varying WiFi conditions
  const int target_fps = 15;
  const int frame_time = 1000 / target_fps;
  int frame_skip_counter = 0;
  unsigned long lastTouchCheck = millis();
  unsigned long streamStartTime = millis();
  unsigned long lastFrameSentTime = millis();
  const unsigned long STREAM_TIMEOUT = 120000;
  
  while (client.connected() && streamActive) {
    if (millis() - streamStartTime > STREAM_TIMEOUT) {
      Serial.println("[STREAM] Timeout - closing stream for safety");
      break;
    }
    
    if (!client) {
      Serial.println("[STREAM] Client object invalid");
      break;
    }
    
    unsigned long frame_start_time = millis();

    // 1. Capture the frame
    captureStreamFrame();

    if (!streamFrameBuffer.empty()) {
      unsigned long capture_time = millis() - frame_start_time;
      if (capture_time > frame_time * 2) {
        frame_skip_counter++;
        if (frame_skip_counter < 3) {
          continue;
        }
        frame_skip_counter = 0;
      }
      
      client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", streamFrameBuffer.size());
      size_t written = client.write(streamFrameBuffer.data(), streamFrameBuffer.size());
      if (written != streamFrameBuffer.size()) {
        Serial.printf("[STREAM] Write incomplete - sent %d of %d bytes\n", written, streamFrameBuffer.size());
      }
      client.print("\r\n");
      lastFrameSentTime = millis();
    }

    vTaskDelay(1);
    yield();  // Prevent watchdog and allow other tasks to run
    
    // Process HTTP requests and WebSocket messages during video stream (prevents blocking)
    server.handleClient();
    webSocket.loop();

    if (millis() - lastTouchCheck > 100) {
      lastTouchCheck = millis();
      if (touchEnabled) {
        int touchValue = touchRead(TOUCH_PIN);
        unsigned long currentTime = millis();

        if (touchValue > TOUCH_THRESHOLD && !isTouched) {
          isTouched = true;
          touchCounter++;
          lastTouchTime = currentTime;
          longHoldTriggered = false;
          analogWrite(VIB_PIN, vibrationIntensity);
          delay(50);
          analogWrite(VIB_PIN, 0);
        }
        else if (touchValue < TOUCH_THRESHOLD && isTouched) {
          isTouched = false;
          lastTouchReleasedTime = currentTime;
          longHoldTriggered = false;
        }

        if (isTouched && !longHoldTriggered && (currentTime - lastTouchTime >= LONG_HOLD_TIME)) {
          longHoldTriggered = true;
          Serial.println("[TOUCH] Long Hold (3s) -> Toggling Live Stream");
          
          // Directly toggle the stream state
          streamActive = !streamActive;
          Serial.printf("[CMD] Stream toggled via touch: %s\n", streamActive ? "ON" : "OFF");
          
          String response = streamActive ? "{\"action\":\"stream_toggle\",\"state\":\"started\"}" : "{\"action\":\"stream_toggle\",\"state\":\"stopped\"}";
          webSocket.broadcastTXT(response.c_str());
          
          for(int i = 0; i < 2; i++) {
            analogWrite(VIB_PIN, vibrationIntensity);
            delay(100);
            analogWrite(VIB_PIN, 0);
            delay(100);
          }
          
          touchCounter = 0;
        }
      }
    }

    unsigned long frame_end_time = millis();
    unsigned long elapsed_time = frame_end_time - frame_start_time;
    if (elapsed_time < frame_time) {
        vTaskDelay((frame_time - elapsed_time) / portTICK_PERIOD_MS);
    } else {
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
  }

  streamActive = false;
  Serial.println("[STREAM] Client disconnected - stream ended");
  sendToAllClients("{\"stream\":\"stopped\"}");
}

void handleCapture() {
  captureFrame();

  if (!lastCapturedImage.empty()) {
    server.send_P(200, "image/jpeg", (const char*)lastCapturedImage.data(), lastCapturedImage.size());
  } else {
    server.send(500, "text/plain", "Failed to capture image");
  }
}

void handleImage() {
  if (!lastCapturedImage.empty()) {
    server.send_P(200, "image/jpeg", (const char*)lastCapturedImage.data(), lastCapturedImage.size());
  } else {
    server.send(404, "text/plain", "No image captured yet");
  }
}

// Scan SD card and build JSON list of all images and videos
void rebuildGalleryCache() {
  if (!sdCardReady) {
    galleryCache = "{\"images\":[],\"storage\":{\"total\":0,\"used\":0}}";
    galleryDirty = false;
    return;
  }
  
  galleryCache = "{\"images\":[";
  
  File root = SD.open("/");
  if (!root) {
    galleryCache = "{\"images\":[],\"storage\":{\"total\":0,\"used\":0}}";
    galleryDirty = false;
    return;
  }
  
  bool first = true;
  File file = root.openNextFile();
  int fileCount = 0;
  
  while (file) {
    String fileName = String(file.name());
    if (!file.isDirectory()) {
      // Skip thumbnails - they are preview files only
      if (fileName.startsWith("thumb_")) {
        file = root.openNextFile();
        continue;
      }
      
      // Include photos and videos only
      if (fileName.endsWith(".jpg") || fileName.endsWith(".mjpeg")) {
        if (!first) {
          galleryCache += ",";
        }
        
        // For videos, try to read duration metadata
        int duration = 0;
        if (fileName.endsWith(".mjpeg")) {
          String metaPath = "/meta_" + fileName.substring(0, fileName.indexOf('.')) + ".txt";
          if (SD.exists(metaPath)) {
            File meta = SD.open(metaPath);
            if (meta) {
              duration = meta.parseInt();
              meta.close();
            }
          }
          galleryCache += "{\"name\":\"" + fileName + "\",\"size\":" + String(file.size()) + ",\"duration\":" + String(duration) + "}";
        } else {
          galleryCache += "{\"name\":\"" + fileName + "\",\"size\":" + String(file.size()) + "}";
        }
        
        first = false;
        fileCount++;
      }
    }
    
    file = root.openNextFile();
  }
  root.close();
  
  galleryCache += "]";
  
  uint64_t totalBytes = SD.totalBytes();
  uint64_t usedBytes = SD.usedBytes();
  galleryCache += ",\"storage\":{\"total\":" + String(totalBytes) + ",\"used\":" + String(usedBytes) + "}";
  galleryCache += "}";
  
  galleryDirty = false;
  Serial.printf("[GALLERY] Cache rebuilt: %d files\n", fileCount);
}

void handleGallery() {
  // Return cached list if nothing has changed (O(1) instead of SD scan)
  if (galleryDirty) {
    rebuildGalleryCache();
  }
  
  server.send(200, "application/json", galleryCache);
}

void handleGalleryImage() {
  if (server.hasArg("name")) {
    String name = server.arg("name");
    String path = "/" + name;
    if (SD.exists(path)) {
      File file = SD.open(path, FILE_READ);
      if (file) {
        String contentType = "image/jpeg";
        if (name.endsWith(".mjpeg")) {
          contentType = "multipart/x-mixed-replace; boundary=frame";
        }
        // MJPEG requires no-cache headers to prevent blank tab/incomplete loads
        server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
        server.sendHeader("Pragma", "no-cache");
        server.sendHeader("Expires", "0");
        server.streamFile(file, contentType);
        file.close();
        return;
      }
    }
  }
  server.send(404, "application/json", "{\"error\":\"image not found\"}");
}

void handleDeleteImage() {
  if (server.hasArg("name")) {
    String name = server.arg("name");
    String path = "/" + name;
    if (SD.exists(path)) {
      if (SD.remove(path)) {
        // Mark gallery for refresh since file was removed
        galleryDirty = true;
        server.send(200, "application/json", "{\"status\":\"deleted\"}");
        Serial.printf("[OK] Image deleted from SD: %s\n", name.c_str());
      } else {
        server.send(500, "application/json", "{\"status\":\"failed to delete\"}");
      }
    } else {
      server.send(404, "application/json", "{\"status\":\"file not found\"}");
    }
  } else {
    server.send(400, "application/json", "{\"status\":\"missing name\"}");
  }
}

void handleDeleteAll() {
  File root = SD.open("/");
  if (!root) {
    server.send(500, "application/json", "{\"error\":\"SD Fail\"}");
    return;
  }
  
  File file = root.openNextFile();
  while (file) {
    String fileName = String(file.name());
    if (!file.isDirectory() && (fileName.endsWith(".jpg") || fileName.endsWith(".mjpeg"))) {
      String path = "/" + fileName;
      SD.remove(path.c_str());
    }
    file = root.openNextFile();
  }
  root.close();
  
  // Mark gallery for refresh since multiple files were removed
  galleryDirty = true;
  server.send(200, "application/json", "{\"status\":\"deleted_all\"}");
  sendToAllClients("{\"action\":\"refresh_gallery\"}");
  Serial.println("[OK] All files deleted from SD");
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }

  server.send(404, "text/plain", message);
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WS] Client %u disconnected\n", num);
      break;

    case WStype_CONNECTED:
      Serial.printf("[WS] Client %u connected\n", num);
      break;

    case WStype_TEXT: {
      String msg = String((char*)payload);
      msg = msg.substring(0, length);

      JsonDocument doc;
      if (deserializeJson(doc, msg) == DeserializationError::Ok) {
        String action = doc["action"];

        if (action == "stream_start") {
          streamActive = true;
          Serial.println("[CMD] Stream started via WebSocket");
        } else if (action == "stream_stop") {
          streamActive = false;
          Serial.println("[CMD] Stream stopped via WebSocket");
        } else if (action == "stream_toggle") {
          streamActive = !streamActive;
          Serial.printf("[CMD] Stream toggled via touch: %s\n", streamActive ? "ON" : "OFF");
          // Send back the new state to all clients
          String response = streamActive ? "{\"action\":\"stream_toggle\",\"state\":\"started\"}" : "{\"action\":\"stream_toggle\",\"state\":\"stopped\"}";
          webSocket.broadcastTXT(response.c_str());
        } else if (action == "capture") {
          captureFrame();
          Serial.println("[CMD] Image captured via WebSocket");
        } else if (action == "record_start") {
          startRecording();
        } else if (action == "record_stop") {
          stopRecording();
        } else if (action == "delete") {
          String name = doc["name"];
          String path = "/" + name;
          if (SD.exists(path)) {
            if (SD.remove(path)) {
              sendToAllClients("{\"action\":\"refresh_gallery\"}");
            }
          }
        } else if (action == "setting") {
          // Handle camera settings adjustments
          String setting = doc["param"];
          int value = doc["value"];
          updateCameraSetting(setting.c_str(), value);
        }
      }
      break;
    }

    case WStype_BIN:
      break;
  }
}

void updateCameraSetting(const char* setting, int value) {
  if (!sensor) return;

  if (strcmp(setting, "brightness") == 0) {
    sensor->set_brightness(sensor, value);
    Serial.printf("[SETTING] Brightness: %d\n", value);
  } 
  else if (strcmp(setting, "contrast") == 0) {
    sensor->set_contrast(sensor, value);
    Serial.printf("[SETTING] Contrast: %d\n", value);
  }
  else if (strcmp(setting, "saturation") == 0) {
    sensor->set_saturation(sensor, value);
    Serial.printf("[SETTING] Saturation: %d\n", value);
  }
  else if (strcmp(setting, "exposure") == 0) {
    sensor->set_aec_value(sensor, value);
    Serial.printf("[SETTING] Exposure: %d\n", value);
  }
  else if (strcmp(setting, "gain") == 0) {
    sensor->set_agc_gain(sensor, value);
    Serial.printf("[SETTING] Gain: %d\n", value);
  }
  else if (strcmp(setting, "hmirror") == 0) {
    sensor->set_hmirror(sensor, value);
    Serial.printf("[SETTING] H-Mirror: %d\n", value);
  }
  else if (strcmp(setting, "vflip") == 0) {
    sensor->set_vflip(sensor, value);
    Serial.printf("[SETTING] V-Flip: %d\n", value);
  }
}

void sendToAllClients(String json) {
  for(uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) {
    webSocket.sendTXT(i, json);
  }
}

// Establish WiFi connection with optimized timeout and power settings
void connectWiFi() {
  Serial.printf("[WIFI] Attempting connection to: %s\n", WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_15dBm);  // Reduced power for stability (#3)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // 20-second timeout instead of 8 seconds (#2)
  unsigned long start = millis();
  int dotCount = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
    dotCount++;
    if (dotCount % 10 == 0) Serial.println();
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[OK] WiFi connected after %lu ms!\n", millis() - start);
    Serial.printf("[INFO] SSID: %s\n", WiFi.SSID().c_str());
    Serial.printf("[INFO] Signal Strength: %d dBm\n", WiFi.RSSI());
    Serial.printf("[IP] http://%s\n", WiFi.localIP().toString().c_str());
    wifiConnected = true;
    consecutiveWiFiFailures = 0;
  } else {
    Serial.printf("\n[OFFLINE] WiFi connection failed after %lu ms.\n", millis() - start);
    wifiConnected = false;
  }
}

void setup() {
  // Initialize vibration motor pin FIRST before anything else
  pinMode(VIB_PIN, OUTPUT);
  digitalWrite(VIB_PIN, LOW);  // Ensure motor stays OFF during boot
  
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n================================");
  Serial.println("XIAO ESP32-S3 Sense - Camera Control");
  Serial.println("================================\n");
  Serial.printf("[OK] Touchpad GPIO %d initialized (Threshold: %d)\n", TOUCH_PIN, TOUCH_THRESHOLD);

  // Connect WiFi before initializing SD and camera for proper startup order
  connectWiFi();
  
  // Use streaming mode for variable-length responses (MJPEG, large files)
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  
  // After WiFi connection, initialize peripherals
  Serial.println("[INIT] Initializing SD card...");
  if (!SD.begin(SD_CS)) {
    Serial.println("[ERROR] SD card mount failed!");
    sdCardReady = false;
  } else {
    Serial.println("[OK] SD card mounted successfully");
    sdCardReady = true;
  }

  initCamera();

  // Create camera mutex for thread-safe access
  camMutex = xSemaphoreCreateMutex();
  if (camMutex == NULL) {
    Serial.println("[ERROR] Camera mutex creation failed!");
  } else {
    Serial.println("[OK] Camera mutex created");
  }

  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/favicon.ico", handleFavicon);
  server.on("/stream", handleStream);
  server.on("/capture", handleCapture);
  server.on("/image", handleImage);
  server.on("/gallery", handleGallery);
  server.on("/galleryImage", handleGalleryImage);
  server.on("/deleteImage", handleDeleteImage);
  server.on("/deleteAll", handleDeleteAll);
  server.onNotFound(handleNotFound);

  // Only start HTTP server and WebSocket if WiFi is connected
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[HAPTIC] WiFi connected - pulsing vibration");
    for(int i = 0; i < 3; i++) {
      analogWrite(VIB_PIN, 60);
      delay(100);
      analogWrite(VIB_PIN, 0);
      delay(100);
    }
    
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    delay(300);
    Serial.println("[OK] Time sync started");
    
    if (MDNS.begin("esp32-camera")) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
      Serial.println("[MDNS] Started: esp32-camera.local");
    } else {
      mdnsStarted = false;
      Serial.println("[MDNS] Failed to start");
    }
    
    server.begin();
    Serial.println("[OK] HTTP server started on port 80");
    
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    Serial.println("[OK] WebSocket server started on port 81");
  } else {
    Serial.println("[HAPTIC] WiFi failed - warning vibration");
    for(int i = 0; i < 4; i++) {
      analogWrite(VIB_PIN, vibrationIntensity);
      delay(100);
      analogWrite(VIB_PIN, 0);
      delay(100);
    }
    delay(200);
    for(int i = 0; i < 4; i++) {
      analogWrite(VIB_PIN, vibrationIntensity);
      delay(100);
      analogWrite(VIB_PIN, 0);
      delay(100);
    }
    
    int status = WiFi.status();
    switch(status) {
      case WL_IDLE_STATUS:       Serial.println("[ERROR] WiFi idle - check antenna"); break;
      case WL_NO_SSID_AVAIL:     Serial.println("[ERROR] SSID not found - check SSID spelling"); break;
      case WL_SCAN_COMPLETED:    Serial.println("[ERROR] Scan completed"); break;
      case WL_CONNECTED:         Serial.println("[ERROR] Connected (unexpected state)"); break;
      case WL_CONNECT_FAILED:    Serial.println("[ERROR] Connection failed - wrong password?"); break;
      case WL_CONNECTION_LOST:   Serial.println("[ERROR] Connection lost"); break;
      case WL_DISCONNECTED:      Serial.println("[ERROR] Disconnected from WiFi"); break;
      default:                   Serial.println("[ERROR] Unknown WiFi error"); break;
    }
    
    Serial.println("[OFFLINE] Recording mode ONLY.");
    Serial.println("[OFFLINE] Servers NOT started (WiFi required).");
  }

  Serial.println("[INIT] Touch sensing will be enabled in 1 second...");
  delay(1000);
  touchEnabled = true;
  Serial.println("[OK] Touch sensing enabled");

  // Create recording task on Core 1 with low priority (priority 0 < WiFi tasks)
  xTaskCreatePinnedToCore(
    recordingCaptureTask,
    "RecordingTask",
    4096,
    NULL,
    0,  // Priority 0 = lower than default, WiFi gets priority 1+
    NULL,
    1
  );
  Serial.println("[OK] Recording capture task started on Core 1");
}

// Main control loop
void loop() {
  // Check WiFi connection periodically and reconnect if needed
  if (millis() - lastWiFiCheck > 15000) {  // Check every 15 seconds instead of 5
    lastWiFiCheck = millis();
    
    if (WiFi.status() == WL_CONNECTED) {
      if (!wifiConnected) {
        wifiConnected = true;
        consecutiveWiFiFailures = 0;
        Serial.printf("[WIFI] Re-established connection after %lu ms\n", millis() - wifiConnectionTime);
      }
      
      // Restart mDNS after WiFi reconnect to update network presence
      if (WiFi.status() == WL_CONNECTED && !mdnsStarted) {
        if (MDNS.begin("esp32-camera")) {
          MDNS.addService("http", "tcp", 80);
          mdnsStarted = true;
          Serial.println("[MDNS] Restarted after reconnect");
        }
      }
    } else {
      // Reset mDNS flag when WiFi drops
      mdnsStarted = false;
      
      // Avoid reconnection during active operations (stream or recording)
      if (!streamActive && !isRecording) {
        wifiConnected = false;
        consecutiveWiFiFailures++;
        
        // Attempt reconnection every 15 seconds (not aggressive)
        Serial.printf("[WIFI] Lost connection - attempting controlled reconnect (attempt %d/%d)\n", consecutiveWiFiFailures, MAX_WIFI_RETRIES);
        WiFi.disconnect(false);  // Don't turn off radio
        delay(500);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        
        if (consecutiveWiFiFailures >= MAX_WIFI_RETRIES) {
          Serial.println("[WIFI] Max retries exceeded - performing full WiFi reset");
          WiFi.disconnect(true);  // Turn off radio
          delay(1000);
          connectWiFi();
          consecutiveWiFiFailures = 0;
        }
      } else {
        // Don't attempt reconnect during critical operations
        wifiConnected = false;
      }
    }
  }

  webSocket.loop();
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();
  } else {
    delay(100);
  }

  if (touchEnabled) {
    int touchValue = touchRead(TOUCH_PIN);
    unsigned long currentTime = millis();

    if (touchValue > TOUCH_THRESHOLD && !isTouched) {
      isTouched = true;
      touchCounter++;
      lastTouchTime = currentTime;
      longHoldTriggered = false;
      
      // Haptic feedback
      analogWrite(VIB_PIN, vibrationIntensity);
      delay(50);
      analogWrite(VIB_PIN, 0);
    }
    else if (touchValue < TOUCH_THRESHOLD && isTouched) {
      isTouched = false;
      lastTouchReleasedTime = currentTime;
      longHoldTriggered = false;
    }

    if (isTouched && !longHoldTriggered && (currentTime - lastTouchTime >= LONG_HOLD_TIME)) {
      longHoldTriggered = true;
      Serial.println("[TOUCH] Long Hold (3s) -> Toggling Live Stream");
      
      streamActive = !streamActive;
      Serial.printf("[CMD] Stream toggled via touch: %s\n", streamActive ? "ON" : "OFF");
      
      String response = streamActive ? "{\"action\":\"stream_toggle\",\"state\":\"started\"}" : "{\"action\":\"stream_toggle\",\"state\":\"stopped\"}";
      webSocket.broadcastTXT(response.c_str());
      
      for(int i = 0; i < 2; i++) {
        analogWrite(VIB_PIN, vibrationIntensity);
        delay(100);
        analogWrite(VIB_PIN, 0);
        delay(100);
      }
      
      touchCounter = 0;
    }

    if (!longHoldTriggered && touchCounter > 0 && (currentTime - lastTouchReleasedTime > DOUBLE_TAP_DELAY) && !isTouched) {
      
      if (touchCounter == 1) {
        Serial.println("[TOUCH] Single Tap -> Capturing Photo");
        captureFrame();
      }
      else if (touchCounter >= 2) {
        if (!isRecording) {
          Serial.println("[TOUCH] Double Tap -> Starting Recording");
          startRecording();
          for(int i = 0; i < 3; i++) {
            analogWrite(VIB_PIN, vibrationIntensity);
            delay(50);
            analogWrite(VIB_PIN, 0);
            delay(50);
          }
        } else {
          Serial.println("[TOUCH] Double Tap -> Stopping Recording");
          stopRecording();
          analogWrite(VIB_PIN, vibrationIntensity);
          delay(300);
          analogWrite(VIB_PIN, 0);
        }
      }
      
      touchCounter = 0;
    }
  }
}