#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ui.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>   // for esp_wifi_get_mac

/* ────────────────────────────────────────────── */
/* SCREEN SETUP                                   */
/* ────────────────────────────────────────────── */

static const uint16_t screenWidth  = 320;
static const uint16_t screenHeight = 170;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[ screenWidth * screenHeight / 10 ];

TFT_eSPI tft = TFT_eSPI();

/* T-Display S3 POWER PINS */
#define PIN_POWER_ON 15
#define PIN_LCD_BL   38

/* Serial debug */
#if LV_USE_LOG != 0
void my_print(const char * buf)
{
    Serial.printf("%s", buf);
    Serial.flush();
}
#endif

/* Display flushing */
void my_disp_flush( lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p )
{
    uint32_t w = ( area->x2 - area->x1 + 1 );
    uint32_t h = ( area->y2 - area->y1 + 1 );

    tft.startWrite();
    tft.setAddrWindow( area->x1, area->y1, w, h );
    tft.pushColors( ( uint16_t * )&color_p->full, w * h, true );
    tft.endWrite();
    lv_disp_flush_ready( disp );
}

/* Dummy touch */
void my_touchpad_read( lv_indev_drv_t * indev_driver, lv_indev_data_t * data )
{
    data->state = LV_INDEV_STATE_REL;
}

/* SquareLine UI elements */
extern lv_obj_t* ui_slider_speed;
extern lv_obj_t* ui_lbl_speed;
extern lv_obj_t* ui_roller_mode;
extern lv_obj_t* ui_lbl_trip_value;
extern lv_obj_t* ui_bar_battery;
extern lv_obj_t* ui_lbl_battery_value;
extern lv_obj_t* ui_lbl_range;
extern lv_obj_t* ui_slider_range;

/* ────────────────────────────────────────────── */
/* ESP-NOW PACKET – must match sender             */
/* ────────────────────────────────────────────── */

typedef struct {
  uint16_t rpm;
  float batt;
  float motor;
  float dk;
  float gp;
  uint8_t funk;
} DashPacket;

static DashPacket latestData;
static volatile bool hasNewData = false;

/* Trip */
static unsigned long lastTripMillis = 0;
static float totalDistanceKm = 0.0f;

/* CONNECTION DOT */
static lv_obj_t* ui_link_dot = nullptr;
static unsigned long lastPacketMillis = 0;

/* ────────────────────────────────────────────── */
/* ESP-NOW RECEIVE CALLBACK (NEW API)             */
/* ────────────────────────────────────────────── */

void onDataRecv(const esp_now_recv_info_t *info,
                const uint8_t *incomingData,
                int len)
{
  if (len == sizeof(DashPacket)) {
    memcpy((void*)&latestData, incomingData, sizeof(DashPacket));
    hasNewData = true;
    lastPacketMillis = millis();   // update link status
  }
}

/* ────────────────────────────────────────────── */
/* CONNECTION INDICATOR DOT                       */
/* ────────────────────────────────────────────── */

void create_link_indicator()
{
    ui_link_dot = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ui_link_dot, 8, 8);
    lv_obj_align(ui_link_dot, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_border_width(ui_link_dot, 0, 0);
    lv_obj_set_style_radius(ui_link_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ui_link_dot, lv_color_hex(0xFF0000), 0); // red start
}

void update_link_indicator()
{
    if (!ui_link_dot) return;

    bool link_ok = (millis() - lastPacketMillis) < 1000; // <1s => connected
    lv_color_t col = link_ok ? lv_color_hex(0x00FF00) : lv_color_hex(0xFF0000);
    lv_obj_set_style_bg_color(ui_link_dot, col, 0);
}

/* ────────────────────────────────────────────── */
/* UPDATE LVGL UI FROM ESP-NOW DATA               */
/* ────────────────────────────────────────────── */

void updateUIFromData()
{
    /* 1) Convert RPM → speed scale */
    int speedValue = latestData.rpm / 32;  // 8000 rpm → 250
    if (speedValue < 0) speedValue = 0;
    if (speedValue > 250) speedValue = 250;

    lv_slider_set_value(ui_slider_speed, speedValue, LV_ANIM_OFF);

    char speedText[10];
    snprintf(speedText, sizeof(speedText), "%d", speedValue);
    lv_label_set_text(ui_lbl_speed, speedText);

    /* 2) Change roller mode */
    if (speedValue < 60)          lv_roller_set_selected(ui_roller_mode, 0, LV_ANIM_OFF);
    else if (speedValue < 80)     lv_roller_set_selected(ui_roller_mode, 1, LV_ANIM_OFF);
    else if (speedValue < 140)    lv_roller_set_selected(ui_roller_mode, 2, LV_ANIM_OFF);
    else                          lv_roller_set_selected(ui_roller_mode, 3, LV_ANIM_OFF);

    /* 3) Trip distance */
    unsigned long now = millis();
    if (lastTripMillis == 0) {
        lastTripMillis = now;
    } else {
        float hours = (now - lastTripMillis) / 3600000.0f;
        totalDistanceKm += speedValue * hours;
        lastTripMillis = now;
    }

    char tripText[20];
    snprintf(tripText, sizeof(tripText), "%.1f ", totalDistanceKm);
    lv_label_set_text(ui_lbl_trip_value, tripText);

    /* 4) Battery % */
    float batt = latestData.batt;
    batt = constrain(batt, 10.0f, 14.0f);
    int battPercent = (int)(((batt - 10.0f) / 4.0f) * 100.0f);
    battPercent = constrain(battPercent, 0, 100);

    lv_bar_set_value(ui_bar_battery, battPercent, LV_ANIM_OFF);

    char batteryText[10];
    snprintf(batteryText, sizeof(batteryText), "%d%%", battPercent);
    lv_label_set_text(ui_lbl_battery_value, batteryText);

    /* 5) Real voltage */
    char voltText[10];
    snprintf(voltText, sizeof(voltText), "%.1fV", latestData.batt);
    lv_label_set_text(ui_lbl_range, voltText);

    /* 6) Range slider */
    int rangeLevel = map(battPercent, 0, 100, 0, 70);
    lv_slider_set_value(ui_slider_range, rangeLevel, LV_ANIM_OFF);
}

/* ────────────────────────────────────────────── */
/* SETUP                                          */
/* ────────────────────────────────────────────── */

void setup()
{
    Serial.begin(115200);
    delay(100);

    /* Turn on LCD + power rails (fix GPIO38 errors) */
    pinMode(PIN_POWER_ON, OUTPUT);
    pinMode(PIN_LCD_BL, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
    digitalWrite(PIN_LCD_BL, HIGH);

    /* LVGL */
    lv_init();
#if LV_USE_LOG != 0
    lv_log_register_print_cb( my_print );
#endif

    /* TFT */
    tft.begin();
    tft.setRotation(3);

    lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * screenHeight / 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    ui_init();

    /* Connection indicator dot */
    create_link_indicator();

    /* ─── ESP-NOW INIT ─── */
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Get real MAC from WiFi station interface
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        Serial.printf(
            "LilyGo MAC = %02X:%02X:%02X:%02X:%02X:%02X\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
        );
    } else {
        Serial.println("Failed to get MAC with esp_wifi_get_mac()");
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed!");
    } else {
        esp_now_register_recv_cb(onDataRecv);
        Serial.println("ESP-NOW ready (receiver)");
    }

    lastTripMillis = millis();
}

/* ────────────────────────────────────────────── */
/* LOOP                                           */
/* ────────────────────────────────────────────── */

void loop()
{
    lv_timer_handler();
    delay(5);

    if (hasNewData) {
        hasNewData = false;
        updateUIFromData();
    }

    update_link_indicator();  // green/red dot
}
