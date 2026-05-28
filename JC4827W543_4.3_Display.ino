#include <Arduino.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <lvgl.h>
#include "TAMC_GT911.h"
#include <PINS_JC4827W543.h>
#include <queue>

// ---------------- WIFI + HOME ASSISTANT -----------------
const char* WIFI_SSID     = "SSL_EXT";
const char* WIFI_PASSWORD = "redtomato934";
const char* HA_HOST       = "http://192.168.1.183:8123";
const char* HA_TOKEN      = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJiMzA0M2VhOTgxYTg0YzllODEwNmM0OTY0NjdiYzlhZSIsImlhdCI6MTc4MDAwNjU1OSwiZXhwIjoyMDk1MzY2NTU5fQ.Qy19q7y_gobb24Llkj_B2qFlyviWxRPsa-MDGZmuRv0";

// Home Assistant Entities
#define LIGHT_ENTITY    "light.elk_bledob_161e"
#define SWITCH_ENTITY_2    "switch.0xa4c138c2343f08b8"
#define SWITCH_ENTITY   "switch.0xa4c138065f5259eb"
#define PRESENCE_SENSOR "binary_sensor.esphome_web_214ba4_presence"
#define HUMIDITY_SENSOR "sensor.bthome_sensor_5986_humidity"
#define TEMP_SENSOR     "sensor.bthome_sensor_5986_temperature"
#define MOVING_SENSOR   "binary_sensor.esphome_web_214ba4_moving_target"

// Touch Controller
#define TOUCH_SDA 8
#define TOUCH_SCL 4
#define TOUCH_INT 3
#define TOUCH_RST 38
#define TOUCH_WIDTH 480
#define TOUCH_HEIGHT 272
TAMC_GT911 touchController = TAMC_GT911(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, TOUCH_WIDTH, TOUCH_HEIGHT);

#define LCD_BL 1
const int freq = 5000;
const int resolution = 8;


// Display global variables
uint32_t screenWidth;
uint32_t screenHeight;
uint32_t bufSize;
lv_display_t *disp;
lv_color_t *disp_draw_buf;

// LVGL Labels
lv_obj_t *lbl_presence;
lv_obj_t *lbl_humidity;
lv_obj_t *lbl_temperature;
lv_obj_t *lbl_moving;

struct SensorData {
    String presence;
    String humidity;
    String temperature;
    String moving;
};

SensorData sensorData;

// ---------------- FreeRTOS Queue for HTTP -----------------
struct HARequest { const char* entity_id; };
QueueHandle_t haQueue;

// ---------------- LVGL callbacks -----------------
void my_print(lv_log_level_t level, const char *buf) { LV_UNUSED(level); Serial.println(buf); }
uint32_t millis_cb(void) { return millis(); }

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint32_t w = lv_area_get_width(area);
    uint32_t h = lv_area_get_height(area);
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, w, h);
    lv_disp_flush_ready(disp);
}

void updateLabels(void* param) {
    // 1. Presence occupancy state
    String pres = sensorData.presence;
    if(pres == "on" || pres == "detected" || pres.indexOf("on") != -1) {
        lv_label_set_text(lbl_presence, "Occupied");
        lv_obj_set_style_text_color(lbl_presence, lv_color_hex(0x10B981), 0); // Emerald Green
    } else {
        lv_label_set_text(lbl_presence, "Empty");
        lv_obj_set_style_text_color(lbl_presence, lv_color_hex(0x94A3B8), 0); // Slate Gray
    }

    // 2. Humidity level
    if(sensorData.humidity == "offline" || sensorData.humidity == "error") {
        lv_label_set_text(lbl_humidity, "offline");
    } else {
        lv_label_set_text(lbl_humidity, (sensorData.humidity + "%").c_str());
    }

    // 3. Temperature reading
    if(sensorData.temperature == "offline" || sensorData.temperature == "error") {
        lv_label_set_text(lbl_temperature, "offline");
    } else {
        lv_label_set_text(lbl_temperature, (sensorData.temperature + "°C").c_str());
    }

    // 4. Motion moving target detection
    String mov = sensorData.moving;
    if(mov == "on" || mov == "detected" || mov.indexOf("on") != -1) {
        lv_label_set_text(lbl_moving, "Motion");
        lv_obj_set_style_text_color(lbl_moving, lv_color_hex(0xFBBF24), 0); // Amber alert
    } else {
        lv_label_set_text(lbl_moving, "Static");
        lv_obj_set_style_text_color(lbl_moving, lv_color_hex(0x64748B), 0); // Slate Gray
    }
}
// ---------------- SCREEN TIMEOUT & WAKE VARIABLES -----------------
uint32_t lastTouchTime = 0;
uint32_t lastTouchPressTime = 0;
bool isScreenOn = true;
uint8_t currentBrightness = 102; // 40% of 255 is ~102

void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
    touchController.read();
    
    bool touched = (touchController.isTouched && touchController.touches > 0);
    uint32_t now = millis();
    
    static bool was_touched = false;
    
    if (touched) {
        data->point.x = touchController.points[0].x;
        data->point.y = touchController.points[0].y;
        
        // Detect touch press (rising edge)
        if (!was_touched) {
            if (!isScreenOn) {
                // If screen is OFF, check for double tap
                if (now - lastTouchPressTime < 450 && now - lastTouchPressTime > 80) {
                    // Double tap detected! Wake up screen
                    isScreenOn = true;
                    setBrightness(currentBrightness);
                    lastTouchTime = now;
                }
                lastTouchPressTime = now;
            } else {
                // Screen is ON, refresh active timer
                lastTouchTime = now;
            }
        }
        was_touched = true;
        
        if (isScreenOn) {
            data->state = LV_INDEV_STATE_PRESSED;
            lastTouchTime = now; // keep refreshing timer as long as held down
        } else {
            // Swallow touches while screen is OFF to prevent accidental dashboard clicks
            data->state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        was_touched = false;
    }
}

// ---------------- Wi-Fi -----------------
void wifiConnect(lv_obj_t *lbl_status) {
    // Force clean state
    WiFi.disconnect(true); 
    delay(200);
    WiFi.mode(WIFI_STA);
    delay(200);
    
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to Wi-Fi");
    int attempts = 0;
    while(WiFi.status() != WL_CONNECTED) {
        attempts++;
        String statusStr = "Connecting to Wi-Fi...\nSSID: " + String(WIFI_SSID) + "\nAttempt " + String(attempts);
        if (attempts > 20) {
            statusStr += "\nChecking credentials / signal...\n(Note: Router must support 2.4GHz)";
        }
        lv_label_set_text(lbl_status, statusStr.c_str());
        lv_task_handler();
        delay(500);
        Serial.print(".");
    }
    Serial.println(" connected!");
    lv_label_set_text(lbl_status, "Wi-Fi Connected!\nFetching sensor data...");
    lv_task_handler();
    delay(500);
}

// ---------------- Home Assistant HTTP -----------------
void haToggleEntity(const char* entity_id) {
    if(WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    String url = String(HA_HOST) + "/api/services/homeassistant/toggle";
    http.begin(url);
    http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
    http.addHeader("Content-Type", "application/json");
    String payload = "{\"entity_id\":\"" + String(entity_id) + "\"}";
    int code = http.POST(payload);
    Serial.printf("Toggled %s, response code: %d\n", entity_id, code);
    http.end();
}

String haGetState(const char* entity_id) {
    if(WiFi.status() != WL_CONNECTED) return "offline";
    HTTPClient http;
    String url = String(HA_HOST) + "/api/states/" + String(entity_id);
    http.begin(url);
    http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
    http.addHeader("Content-Type", "application/json");
    int code = http.GET();
    String payload = "";
    if(code == 200) {
        payload = http.getString();
        int start = payload.indexOf("\"state\":\"") + 9;
        int end = payload.indexOf("\"", start);
        payload = payload.substring(start, end);
    } else payload = "error";
    http.end();
    return payload;
}

// ---------------- Button callbacks -----------------
static void btn_light_cb(lv_event_t *e) {
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        HARequest req = {LIGHT_ENTITY};
        xQueueSend(haQueue, &req, 0);
    }
}

static void btn_switch_cb_2(lv_event_t *e) {
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        HARequest req = {SWITCH_ENTITY_2};
        xQueueSend(haQueue, &req, 0);
    }
}

static void btn_switch_cb(lv_event_t *e) {
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        HARequest req = {SWITCH_ENTITY};
        xQueueSend(haQueue, &req, 0);
    }
}



// ---------------- FreeRTOS Tasks -----------------
void haTask(void *pvParameters) {
    HARequest req;
    while(true) {
        if(xQueueReceive(haQueue, &req, portMAX_DELAY) == pdTRUE) {
            haToggleEntity(req.entity_id);
        }
    }
}

void sensorTask(void* pvParameters) {
    while(true) {
        sensorData.presence    = haGetState(PRESENCE_SENSOR);
        sensorData.humidity    = haGetState(HUMIDITY_SENSOR);
        sensorData.temperature = haGetState(TEMP_SENSOR);
        sensorData.moving      = haGetState(MOVING_SENSOR);

        lv_async_call(updateLabels, nullptr); // safe update in LVGL task

        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}


// ---------------- Setup -----------------
void setup() {
    Serial.begin(115200);

    // ---------------- Init Display ----------------
    if(!gfx->begin()) while(true);
    pinMode(GFX_BL, OUTPUT);
    ledcAttach(GFX_BL, freq, resolution);
    gfx->fillScreen(RGB565_BLACK);
    
    // Turn backlight on immediately so the user can see the boot screen!
    setBrightness(128);

    // ---------------- Touch ----------------
    touchController.begin();
    touchController.setRotation(ROTATION_INVERTED);

    // ---------------- LVGL ----------------
    lv_init();
    lv_tick_set_cb(millis_cb);

    screenWidth  = gfx->width();
    screenHeight = gfx->height();
    bufSize = screenWidth * 40;
    disp_draw_buf = (lv_color_t *)heap_caps_malloc(bufSize * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!disp_draw_buf) while(true);

    disp = lv_display_create(screenWidth, screenHeight);
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, disp_draw_buf, NULL, bufSize * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);

    // ---------------- BOOT SCREEN UI ----------------
    lv_obj_t *boot_screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(boot_screen, screenWidth, screenHeight);
    lv_obj_set_style_bg_color(boot_screen, lv_color_hex(0x15151F), 0); // Premium dark blue-gray background
    lv_obj_set_style_border_width(boot_screen, 0, 0);
    lv_obj_set_style_radius(boot_screen, 0, 0);

    lv_obj_t *lbl_boot_status = lv_label_create(boot_screen);
    lv_label_set_text(lbl_boot_status, "Starting up...");
    lv_obj_set_style_text_color(lbl_boot_status, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_align(lbl_boot_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_boot_status, LV_ALIGN_CENTER, 0, 0);

    // Render the initial boot screen state
    lv_task_handler();

    // ---------------- Wi-Fi Connection (Interactive) ----------------
    wifiConnect(lbl_boot_status);

    // Clean up boot screen now that we are connected
    lv_obj_delete(boot_screen);

    // ---------------- PREMIUM DARK CARD STYLING ----------------
    static lv_style_t style_card;
    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_hex(0x161925)); // Premium navy-slate card back
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_color(&style_card, lv_color_hex(0x23283C)); // Modern thin border
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_radius(&style_card, 14); // Rounded modern corners
    lv_style_set_pad_all(&style_card, 8);
    lv_style_set_text_color(&style_card, lv_color_hex(0xF1F5F9)); // Near-white text

    // ---------------- GRID CONTAINER ----------------
    // 3 Columns: Col 0 (Buttons - 140px), Col 1 (Sensors - 145px), Col 2 (Status & Brightness - 145px)
    static int32_t col_dsc[] = {140, 145, 145, LV_GRID_TEMPLATE_LAST};
    // 3 Rows: 74px each (fits perfectly inside 272px height, leaving space for margins/padding)
    static int32_t row_dsc[] = {74, 74, 74, LV_GRID_TEMPLATE_LAST};

    lv_obj_t *cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cont, screenWidth, screenHeight);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x0A0B10), 0); // Ultra-premium absolute dark background
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 8, 0);
    lv_obj_set_style_pad_row(cont, 8, 0);
    lv_obj_set_style_pad_column(cont, 8, 0);
    lv_obj_set_grid_dsc_array(cont, col_dsc, row_dsc);

    // ---------------- BUTTONS (COLUMN 0) ----------------
    // LED Strip Button
    lv_obj_t *btn_light = lv_btn_create(cont);
    lv_obj_add_style(btn_light, &style_card, 0);
    lv_obj_set_style_bg_color(btn_light, lv_color_hex(0x1B1B3A), 0); // Accent dark blue
    lv_obj_set_grid_cell(btn_light, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_t *lbl1 = lv_label_create(btn_light);
    lv_label_set_text(lbl1, (String(LV_SYMBOL_CHARGE) + " LED Strip").c_str());
    lv_obj_set_style_text_color(lbl1, lv_color_hex(0x818CF8), 0); // Glow Indigo
    lv_obj_center(lbl1);
    lv_obj_add_event_cb(btn_light, btn_light_cb, LV_EVENT_CLICKED, NULL);

    // Ceiling Light Button
    lv_obj_t *btn_switch2 = lv_btn_create(cont);
    lv_obj_add_style(btn_switch2, &style_card, 0);
    lv_obj_set_style_bg_color(btn_switch2, lv_color_hex(0x1F2A38), 0); // Accent dark amber
    lv_obj_set_grid_cell(btn_switch2, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_t *lbl3 = lv_label_create(btn_switch2);
    lv_label_set_text(lbl3, (String(LV_SYMBOL_POWER) + " Ceiling").c_str());
    lv_obj_set_style_text_color(lbl3, lv_color_hex(0xFBBF24), 0); // Amber Yellow
    lv_obj_center(lbl3);
    lv_obj_add_event_cb(btn_switch2, btn_switch_cb_2, LV_EVENT_CLICKED, NULL);

    // Heater Button
    lv_obj_t *btn_switch1 = lv_btn_create(cont);
    lv_obj_add_style(btn_switch1, &style_card, 0);
    lv_obj_set_style_bg_color(btn_switch1, lv_color_hex(0x2D1E24), 0); // Accent dark rose
    lv_obj_set_grid_cell(btn_switch1, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
    lv_obj_t *lbl2 = lv_label_create(btn_switch1);
    lv_label_set_text(lbl2, (String(LV_SYMBOL_POWER) + " Heater").c_str());
    lv_obj_set_style_text_color(lbl2, lv_color_hex(0xF87171), 0); // Warm Rose
    lv_obj_center(lbl2);
    lv_obj_add_event_cb(btn_switch1, btn_switch_cb, LV_EVENT_CLICKED, NULL);

    // ---------------- TEMPERATURE CARD (COLUMN 1, ROWS 0 & 1 - SPAN 2) ----------------
    lv_obj_t *card_temp = lv_obj_create(cont);
    lv_obj_add_style(card_temp, &style_card, 0);
    lv_obj_set_grid_cell(card_temp, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 0, 2); // Span 2 rows
    lv_obj_set_style_pad_all(card_temp, 6, 0);

    lv_obj_t *lbl_temp_title = lv_label_create(card_temp);
    lv_label_set_text(lbl_temp_title, "Temperature");
    lv_obj_set_style_text_color(lbl_temp_title, lv_color_hex(0x94A3B8), 0); // Slate-400
    lv_obj_align(lbl_temp_title, LV_ALIGN_TOP_LEFT, 4, 4);

    lbl_temperature = lv_label_create(card_temp);
    lv_label_set_text(lbl_temperature, "--.-°C");
    lv_obj_set_style_text_color(lbl_temperature, lv_color_hex(0xF43F5E), 0); // Premium Coral-red
    lv_obj_align(lbl_temperature, LV_ALIGN_CENTER, 0, 12);

    // ---------------- HUMIDITY CARD (COLUMN 1, ROW 2) ----------------
    lv_obj_t *card_hum = lv_obj_create(cont);
    lv_obj_add_style(card_hum, &style_card, 0);
    lv_obj_set_grid_cell(card_hum, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
    lv_obj_set_style_pad_all(card_hum, 6, 0);

    lv_obj_t *lbl_hum_title = lv_label_create(card_hum);
    lv_label_set_text(lbl_hum_title, "Humidity");
    lv_obj_set_style_text_color(lbl_hum_title, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(lbl_hum_title, LV_ALIGN_TOP_LEFT, 4, 2);

    lbl_humidity = lv_label_create(card_hum);
    lv_label_set_text(lbl_humidity, "--%");
    lv_obj_set_style_text_color(lbl_humidity, lv_color_hex(0x38BDF8), 0); // Soft sky-blue
    lv_obj_align(lbl_humidity, LV_ALIGN_BOTTOM_LEFT, 4, -4);

    // ---------------- ENVIRONMENT & OCCUPANCY CARD (COLUMN 2, ROW 0) ----------------
    lv_obj_t *card_occ = lv_obj_create(cont);
    lv_obj_add_style(card_occ, &style_card, 0);
    lv_obj_set_grid_cell(card_occ, LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_style_pad_all(card_occ, 6, 0);

    lbl_presence = lv_label_create(card_occ);
    lv_label_set_text(lbl_presence, "Unknown");
    lv_obj_align(lbl_presence, LV_ALIGN_LEFT_MID, 6, -10);

    lbl_moving = lv_label_create(card_occ);
    lv_label_set_text(lbl_moving, "Static");
    lv_obj_align(lbl_moving, LV_ALIGN_LEFT_MID, 6, 10);

    // ---------------- BRIGHTNESS PANEL (COLUMN 2, ROWS 1 & 2 - SPAN 2) ----------------
    lv_obj_t *card_bright = lv_obj_create(cont);
    lv_obj_add_style(card_bright, &style_card, 0);
    lv_obj_set_grid_cell(card_bright, LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_STRETCH, 1, 2); // Span 2 rows
    lv_obj_set_style_pad_all(card_bright, 6, 0);

    lv_obj_t *lbl_bright_title = lv_label_create(card_bright);
    lv_label_set_text(lbl_bright_title, "Backlight");
    lv_obj_set_style_text_color(lbl_bright_title, lv_color_hex(0x94A3B8), 0);
    lv_obj_align(lbl_bright_title, LV_ALIGN_TOP_MID, 0, 2);

    // Beautiful vertical slider
    lv_obj_t *slider_brightness = lv_slider_create(card_bright);
    lv_slider_set_range(slider_brightness, 5, 255); // Minimum 5 to avoid complete darkness
    lv_slider_set_value(slider_brightness, 102, LV_ANIM_OFF); // Default slider at 40% (102)
    lv_obj_set_size(slider_brightness, 14, 86); // Taller than wide automatically renders vertical!
    lv_obj_align(slider_brightness, LV_ALIGN_BOTTOM_MID, 0, -6);
    
    // Style the slider indicator/knob for premium feel
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(0x3B82F6), LV_PART_INDICATOR); // Blue track
    lv_obj_set_style_bg_color(slider_brightness, lv_color_hex(0xFFFFFF), LV_PART_KNOB); // White modern handle

    lv_obj_add_event_cb(slider_brightness, [](lv_event_t *e){
        lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e);
        int val = lv_slider_get_value(s);
        currentBrightness = val;
        if (isScreenOn) {
            setBrightness(val);
        }
        lastTouchTime = millis(); // Refresh timeout on adjustment
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // ---------------- FreeRTOS ----------------
    haQueue = xQueueCreate(10, sizeof(HARequest));
    xTaskCreatePinnedToCore(haTask, "HA Task", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(sensorTask, "Sensor Task", 4096, NULL, 1, NULL, 1);

    Serial.println("Setup complete – grid UI ready");
    
    // Initialize active touch time and set initial 40% brightness
    lastTouchTime = millis();
    currentBrightness = 102;
    setBrightness(102);
}

void setBrightness(uint8_t value)
{
  ledcWrite(GFX_BL, value);
}

// ---------------- Loop -----------------
void loop() {
    lv_task_handler(); // keep GUI responsive
    
    // Auto screen off check: turn off screen if inactive for >20 seconds (20000ms)
    if (isScreenOn && (millis() - lastTouchTime > 20000)) {
        isScreenOn = false;
        setBrightness(0); // Turn off backlight
    }
    
    delay(5);
}
