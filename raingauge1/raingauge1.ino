/*
 * ESP32-C6 FireBeetle Rain Gauge (BTHome v2)
 * Hardware: FireBeetle 2 ESP32-C6
 * Sensor: Reed Switch on Pin 4 (Normally Closed, Opens on tip)
 */

#include <NimBLEDevice.h>
#include <driver/adc.h>
#include "driver/rtc_io.h"

// --- Configuration ---
#define REED_PIN 4
#define BATTERY_PIN 0          
#define DEBOUNCE_TIME_MS 200   
#define MM_PER_TIP 1       
#define DEVICE_NAME "RainGauge"

// --- Timing Configuration ---
#define ACTIVE_REPORT_INTERVAL 60000  // 1 minute
#define IDLE_TIMEOUT 300000           // 5 minutes
#define DAILY_SLEEP_US 86400000000ULL // 24 hours

// --- RTC Memory ---
struct RTCData {
  float totalRainMM;        
  float sessionRainMM;      
  uint32_t bootCount;
};

RTC_DATA_ATTR RTCData rtcData; 

// --- Global Variables ---
volatile unsigned long lastTipTime = 0;
volatile bool tipDetected = false;
unsigned long lastReportTime = 0;
unsigned long lastRainActivityTime = 0;

// --- Interrupt Service Routine ---
void IRAM_ATTR handleRainTip() {
  unsigned long now = millis();
  if (now - lastTipTime > DEBOUNCE_TIME_MS) {
    lastTipTime = now;
    tipDetected = true;
  }
}

// --- BTHome V2 Packet Construction ---
void sendBTHomeReport() {
  NimBLEAdvertisementData advData;
  std::string payload;

  // 1. BTHome Device Information
  payload += (char)0x02; payload += (char)0x01; payload += (char)0x06;

  // Service UUID & BTHome Info
  std::string serviceData;
  serviceData += (char)0xD2;
  serviceData += (char)0xFC; 
  serviceData += (char)0x40; 

  // 1. Rain Count (Type 0x3D - 2 bytes unsigned int)
  int tips = (int)(rtcData.sessionRainMM / MM_PER_TIP);
  serviceData += (char)0x3D; 
  serviceData += (char)(tips & 0xFF);
  serviceData += (char)((tips >> 8) & 0xFF);

  // 2. Battery Voltage (Type 0x0C - 2 bytes)
  uint32_t rawV = analogReadMilliVolts(BATTERY_PIN);
  int voltage = rawV * 2;
  serviceData += (char)0x0C;
  serviceData += (char)(voltage & 0xFF);
  serviceData += (char)((voltage >> 8) & 0xFF);

  // 3. Battery % (Type 0x01 - 1 byte)
  int pct = map(voltage, 3300, 4200, 0, 100);
  if(pct < 0) pct = 0; if(pct > 100) pct = 100;
  serviceData += (char)0x01;
  serviceData += (char)pct;

  // Compile payload
  payload += (char)(serviceData.length() + 1); 
  payload += (char)0x16; 
  payload += serviceData;

  Serial.println("advertising - ");
  Serial.println(tips);
  Serial.println(voltage);
  Serial.println(pct);

  // Advertise
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  
  NimBLEAdvertisementData data;
  data.addData((uint8_t*)payload.data(), payload.length());
  pAdvertising->setAdvertisementData(data);
  
  pAdvertising->start();
  delay(1000); 
  pAdvertising->stop();
}

void setup() {
  Serial.begin(115200);
  Serial.println("--- WAKE UP ---");
  
  // Initialize Input with Pullup (Internal resistor keeps it HIGH when open)
  pinMode(REED_PIN, INPUT_PULLUP);

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); 

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
     rtcData.sessionRainMM += MM_PER_TIP;
     rtcData.totalRainMM += MM_PER_TIP;
     lastRainActivityTime = millis();
     Serial.println("Woke up from tip!");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
     Serial.println("Woke up from timer");
     sendBTHomeReport();
     startDeepSleep(DAILY_SLEEP_US); 
  } else {
     rtcData.totalRainMM = 0;
     rtcData.sessionRainMM = 0;
     Serial.println("Fresh Boot");
  }
  
  attachInterrupt(digitalPinToInterrupt(REED_PIN), handleRainTip, RISING); 
}

void loop() {
  unsigned long now = millis();

  if (tipDetected) {
    rtcData.sessionRainMM += MM_PER_TIP;
    rtcData.totalRainMM += MM_PER_TIP;
    lastRainActivityTime = now;
    tipDetected = false;
    Serial.print("Tip! Total: ");
    Serial.println(rtcData.totalRainMM);
  }

  bool currentlyRaining = (now - lastRainActivityTime < IDLE_TIMEOUT);
  
  if (currentlyRaining) {
    if (now - lastReportTime > ACTIVE_REPORT_INTERVAL) {
       sendBTHomeReport();
       lastReportTime = now;
    }
  } else {
    Serial.println("Going to sleep.");
    startDeepSleep(DAILY_SLEEP_US);
  }
}

void startDeepSleep(uint64_t sleepDuration) {
  rtc_gpio_pulldown_en(GPIO_NUM_4);
  rtc_gpio_pullup_dis(GPIO_NUM_4);
  rtc_gpio_hold_en(GPIO_NUM_4);
  esp_sleep_enable_ext1_wakeup_io((1ULL << GPIO_NUM_4), ESP_EXT1_WAKEUP_ANY_HIGH);


  esp_sleep_enable_timer_wakeup(sleepDuration);
  
  Serial.flush();
  esp_deep_sleep_start();
}