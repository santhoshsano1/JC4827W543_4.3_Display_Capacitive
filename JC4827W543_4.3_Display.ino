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
const char* HA_HOST       = "http://192.168.1.135:8123";
const char* HA_TOKEN      = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJmZDhjOWI0OTMzMjY0YzRkODQ0MzNhYmVkMDdiOTRkNiIsImlhdCI6MTc2NTIzMjQ2MCwiZXhwIjoyMDgwNTkyNDYwfQ.LVH2dfJ23jgiLjK-Vrwh4_HLxlM0WYn16iHcKENlXAQ";

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
    lv_label_set_text(lbl_presence, sensorData.presence.c_str());
    lv_label_set_text(lbl_humidity, sensorData.humidity.c_str());
    lv_label_set_text(lbl_temperature, sensorData.temperature.c_str());
    lv_label_set_text(lbl_moving, sensorData.moving.c_str());
}

void my_touchpad_read(lv_indev_t *indev, lv_indev_data_t *data) {
    touchController.read();
    if(touchController.isTouched && touchController.touches > 0) {
        data->point.x = touchController.points[0].x;
        data->point.y = touchController.points[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else data->state = LV_INDEV_STATE_RELEASED;
}

// ---------------- Wi-Fi -----------------
void wifiConnect() {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to Wi-Fi");
    while(WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println(" connected!");
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
        sensorData.presence    = "Presence: "  + haGetState(PRESENCE_SENSOR);
        sensorData.humidity    = "Humidity: "  + haGetState(HUMIDITY_SENSOR);
        sensorData.temperature = "Temperature: " + haGetState(TEMP_SENSOR);
        sensorData.moving      = "Moving: "    + haGetState(MOVING_SENSOR);

        lv_async_call(updateLabels, nullptr); // safe update in LVGL task

        vTaskDelay(1000 / portTICK_PERIOD_MS);
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

    // ---------------- Wi-Fi ----------------
    wifiConnect();

    // ---------------- GRID CONTAINER ----------------
    static int32_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(2), LV_GRID_TEMPLATE_LAST}; // Left 1 unit, right 2 units
    static int32_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

    lv_obj_t *cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cont, screenWidth, screenHeight);
    lv_obj_set_grid_dsc_array(cont, col_dsc, row_dsc);

    // ---------------- BUTTONS ----------------
    lv_obj_t *btn_light = lv_btn_create(cont);
    lv_obj_set_grid_cell(btn_light, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_t *lbl1 = lv_label_create(btn_light);
    lv_label_set_text(lbl1, (String(LV_SYMBOL_CHARGE) + " LED Strip").c_str());
    lv_obj_center(lbl1);
    lv_obj_add_event_cb(btn_light, btn_light_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_switch1 = lv_btn_create(cont);
    lv_obj_set_grid_cell(btn_switch1, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_t *lbl2 = lv_label_create(btn_switch1);
    lv_label_set_text(lbl2, (String(LV_SYMBOL_POWER) + " Heater").c_str());
    lv_obj_center(lbl2);
    lv_obj_add_event_cb(btn_switch1, btn_switch_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_switch2 = lv_btn_create(cont);
    lv_obj_set_grid_cell(btn_switch2, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 2, 1);
    lv_obj_t *lbl3 = lv_label_create(btn_switch2);
    lv_label_set_text(lbl3, (String(LV_SYMBOL_POWER) + " Ceiling Light").c_str());
    lv_obj_center(lbl3);
    lv_obj_add_event_cb(btn_switch2, btn_switch_cb_2, LV_EVENT_CLICKED, NULL);

    lv_obj_t *slider_brightness = lv_slider_create(cont);
    lv_slider_set_range(slider_brightness, 0, 255);   // PWM 0–255
    lv_slider_set_value(slider_brightness, 60, LV_ANIM_OFF);
    lv_obj_set_grid_cell(slider_brightness, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_START, 3, 1);

    lv_obj_t *lbl_slider = lv_label_create(cont);
    lv_label_set_text(lbl_slider, "Brightness");
    lv_obj_set_grid_cell(lbl_slider, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_END, 3, 1);

    lv_obj_add_event_cb(slider_brightness, [](lv_event_t *e){
    lv_obj_t *s = (lv_obj_t *)lv_event_get_target(e); // explicit cast
    int val = lv_slider_get_value(s);
    setBrightness(val);
    }, LV_EVENT_VALUE_CHANGED, NULL);


    // ---------------- LABELS ----------------
    lbl_presence = lv_label_create(cont);
    lv_label_set_text(lbl_presence, "Presence: unknown");
    lv_obj_set_grid_cell(lbl_presence, LV_GRID_ALIGN_START, 1, 1, LV_GRID_ALIGN_STRETCH, 0, 1);

    lbl_humidity = lv_label_create(cont);
    lv_label_set_text(lbl_humidity, "Humidity: unknown");
    lv_obj_set_grid_cell(lbl_humidity, LV_GRID_ALIGN_START, 1, 1, LV_GRID_ALIGN_STRETCH, 1, 1);

    lbl_temperature = lv_label_create(cont);
    lv_label_set_text(lbl_temperature, "Temperature: unknown");
    lv_obj_set_grid_cell(lbl_temperature, LV_GRID_ALIGN_START, 1, 1, LV_GRID_ALIGN_STRETCH, 2, 1);

    lbl_moving = lv_label_create(cont);
    lv_label_set_text(lbl_moving, "Moving: unknown");
    lv_obj_set_grid_cell(lbl_moving, LV_GRID_ALIGN_START, 1, 1, LV_GRID_ALIGN_STRETCH, 3, 1);

    // ---------------- FreeRTOS ----------------
    haQueue = xQueueCreate(10, sizeof(HARequest));
    xTaskCreatePinnedToCore(haTask, "HA Task", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(sensorTask, "Sensor Task", 4096, NULL, 1, NULL, 1);

    Serial.println("Setup complete – grid UI ready");
    setBrightness(60);
}



void setBrightness(uint8_t value)
{
  ledcWrite(GFX_BL, value);
}

// ---------------- Loop -----------------
void loop() {
    lv_task_handler(); // keep GUI responsive
    delay(5);
}
