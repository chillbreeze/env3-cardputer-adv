#include <Arduino.h>

/*
 * CardENV - ENV-III Sensor Display for M5Stack Cardputer ADV
 * Features:
 * - Main page: Three horizontal boxes with icons for Temp, Humidity, Pressure
 * - Press T for Temperature graph, H for Humidity graph, P for Pressure graph
 * - Press ESC (` or ~) to return to main page
 * - 1 hour history graphs
 * - Configurable screen timeout (10s, 30s, or Always On)
 */

#include <M5Cardputer.h>
#include <M5Unified.hpp>
#include <M5UnitENV.h>

SHT3X sht30;
QMP6988 qmp6988;

// Current readings
float temperature = 0.0;
float humidity = 0.0;
float pressure = 0.0;
// Previous displayed values (to detect changes)
float dispTemp = -999.0;
float dispHumidity = -999.0;
float dispPressure = -999.0;
// History for graphs (last 60 readings = ~1 hour at 1/min)
const int historySize = 60;
float tempHistory[historySize];
float humidityHistory[historySize];
float pressureHistory[historySize];
int historyIndex = 0;
int historyCount = 0;
unsigned long lastHistoryUpdate = 0;
const unsigned long historyInterval = 60000;  // 1 minute

// Screen dimensions (Cardputer: 240x135)
int screenW;
int screenH;
// Page management
// 0 = main, 1 = temp graph, 2 = humidity graph, 3 = pressure graph, 4 = settings
int currentPage = 0;
// Screen timeout
unsigned long lastActivityTime = 0;
int normalBrightness = 80;
enum ScreenState { SCREEN_ON, SCREEN_OFF };
ScreenState screenState = SCREEN_ON;
// Display update timing
unsigned long lastDisplayUpdate = 0;
const unsigned long displayInterval = 1000;
// Update display every 1 second

// Battery flash state
bool batteryFlashOn = true;
unsigned long lastFlashTime = 0;
const int flashInterval = 500;
int prevBatteryLevel = -1;
bool prevCharging = false;
// UI constants for horizontal layout
const int boxWidth = 72;
const int boxHeight = 85;
const int boxMargin = 6;
const int boxRadius = 8;
const int boxBorderWidth = 2;
const int topMargin = 22;
// Space for battery

// Box X positions (calculated in setup)
int tempBoxX;
int humidBoxX;
int pressBoxX;
int boxY;
// Colors (same as original)
const uint16_t COLOR_TEMP = 0xFD20;      // Coral
const uint16_t COLOR_HUMIDITY = 0x07FF;
// Cyan
const uint16_t COLOR_PRESSURE = 0xD01F;  // Purple

// Flag for redraw
bool needsFullRedraw = true;
// Graph page - stored current value for partial update
float graphDispValue = -999.0;

// Settings
bool useFahrenheit = false;
// Screen timeout options: 0 = 10s, 1 = 30s, 2 = Always On
int screenTimeoutOption = 2;  // Default to Always On
const unsigned long screenTimeoutValues[] = {10000, 30000, 0};  // 0 means always on
int settingsSelection = 0;  // 0 = brightness, 1 = temp unit, 2 = screen timeout

//----------------------------------------------------------
// Utility Functions
//----------------------------------------------------------

int getTextWidth(const char* text, int textSize) {
  return strlen(text) * 6 * textSize;
}

void drawCenteredText(const char* text, int y, int textSize, uint16_t color) {
  M5Cardputer.Display.setTextSize(textSize);
  M5Cardputer.Display.setTextColor(color);
  int textW = getTextWidth(text, textSize);
  int x = (screenW - textW) / 2;
  M5Cardputer.Display.setCursor(x, y);
  M5Cardputer.Display.print(text);
}

void drawCenteredTextInBox(const char* text, int boxX, int boxW, int y, int textSize, uint16_t color) {
  M5Cardputer.Display.setTextSize(textSize);
  M5Cardputer.Display.setTextColor(color);
  int textW = getTextWidth(text, textSize);
  int x = boxX + (boxW - textW) / 2;
  M5Cardputer.Display.setCursor(x, y);
  M5Cardputer.Display.print(text);
}

void drawThickRoundRect(int x, int y, int w, int h, int radius, int thickness, uint16_t color) {
  for (int i = 0; i < thickness; i++) {
    M5Cardputer.Display.drawRoundRect(x + i, y + i, w - (i * 2), h - (i * 2), radius, color);
  }
}

//----------------------------------------------------------
// Temperature Conversion
//----------------------------------------------------------

float getDisplayTemp(float tempC) {
  if (useFahrenheit) {
    return (tempC * 9.0 / 5.0) + 32.0;
  }
  return tempC;
}

const char* getTempUnit() {
  return useFahrenheit ? "F" : "C";
}

//----------------------------------------------------------
// Icon Drawing Functions
//----------------------------------------------------------

// Thermometer icon for Temperature
void drawThermometerIcon(int cx, int cy, uint16_t color) {
  // Bulb at bottom
  M5Cardputer.Display.fillCircle(cx, cy + 8, 6, color);
  // Stem
  M5Cardputer.Display.fillRoundRect(cx - 3, cy - 10, 6, 18, 2, color);
  // Inner darker area (cutout effect)
  M5Cardputer.Display.fillCircle(cx, cy + 8, 3, TFT_BLACK);
  M5Cardputer.Display.fillRect(cx - 1, cy - 6, 2, 12, TFT_BLACK);
  // Mercury level
  M5Cardputer.Display.fillCircle(cx, cy + 8, 2, color);
  M5Cardputer.Display.fillRect(cx - 1, cy - 2, 2, 10, color);
}

// Water droplet icon for Humidity
void drawDropletIcon(int cx, int cy, uint16_t color) {
  // Draw a droplet shape using triangles and circle
  // Bottom circle
  M5Cardputer.Display.fillCircle(cx, cy + 4, 7, color);
  // Top triangle part
  M5Cardputer.Display.fillTriangle(cx, cy - 12, cx - 7, cy + 2, cx + 7, cy + 2, color);
  // Inner highlight
  M5Cardputer.Display.fillCircle(cx - 2, cy + 2, 2, TFT_WHITE);
}

// Barometer/gauge icon for Pressure
void drawBarometerIcon(int cx, int cy, uint16_t color) {
  // Outer circle (gauge face)
  M5Cardputer.Display.fillCircle(cx, cy, 10, color);
  M5Cardputer.Display.fillCircle(cx, cy, 7, TFT_BLACK);
  // Tick marks
  M5Cardputer.Display.drawLine(cx - 6, cy, cx - 4, cy, color);
  // Left
  M5Cardputer.Display.drawLine(cx + 4, cy, cx + 6, cy, color);
  // Right
  M5Cardputer.Display.drawLine(cx, cy - 6, cx, cy - 4, color);
  // Top
  // Needle pointing to high pressure (upper right)
  M5Cardputer.Display.drawLine(cx, cy, cx + 4, cy - 4, color);
  M5Cardputer.Display.drawLine(cx, cy, cx + 5, cy - 3, color);
  // Center dot
  M5Cardputer.Display.fillCircle(cx, cy, 2, color);
}

//----------------------------------------------------------
// Battery Display
//----------------------------------------------------------

uint16_t getBatteryColor(int level, bool isCharging) {
  if (isCharging) return TFT_GREEN;
  if (level > 70) return TFT_GREEN;
  if (level > 30) return TFT_YELLOW;
  return TFT_RED;
}

void drawLightningBolt(int x, int y, uint16_t color) {
  M5Cardputer.Display.drawLine(x + 4, y, x + 1, y + 4, color);
  M5Cardputer.Display.drawLine(x + 1, y + 4, x + 3, y + 4, color);
  M5Cardputer.Display.drawLine(x + 3, y + 4, x, y + 8, color);
  M5Cardputer.Display.drawLine(x + 5, y, x + 2, y + 4, color);
  M5Cardputer.Display.drawLine(x + 4, y + 4, x + 1, y + 8, color);
}

void drawBattery(bool forceRedraw) {
  int batteryLevel = M5Cardputer.Power.getBatteryLevel();
  bool isCharging = M5Cardputer.Power.isCharging();
  if (batteryLevel <= 10 && !isCharging) {
    unsigned long now = millis();
    if (now - lastFlashTime >= flashInterval) {
      lastFlashTime = now;
      batteryFlashOn = !batteryFlashOn;
      forceRedraw = true;
    }
  } else {
    batteryFlashOn = true;
  }
  
  if (!forceRedraw && batteryLevel == prevBatteryLevel && isCharging == prevCharging) {
    return;
  }
  prevBatteryLevel = batteryLevel;
  prevCharging = isCharging;
  
  int battX = screenW - 55;
  int battY = 3;
  M5Cardputer.Display.fillRect(battX - 3, battY - 1, 58, 14, TFT_BLACK);
  
  if (!batteryFlashOn) return;
  
  uint16_t battColor = getBatteryColor(batteryLevel, isCharging);
  int battW = 22;
  int battH = 10;
  M5Cardputer.Display.drawRect(battX, battY, battW, battH, battColor);
  M5Cardputer.Display.fillRect(battX + battW, battY + 2, 2, 6, battColor);
  
  int fillW = map(batteryLevel, 0, 100, 0, battW - 4);
  if (fillW > 0) {
    M5Cardputer.Display.fillRect(battX + 2, battY + 2, fillW, battH - 4, battColor);
  }
  
  if (isCharging) {
    drawLightningBolt(battX + 6, battY + 1, TFT_BLACK);
  }
  
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(battColor);
  M5Cardputer.Display.setCursor(battX + 26, battY + 1);
  M5Cardputer.Display.printf("%d%%", batteryLevel);
}

//----------------------------------------------------------
// Screen Timeout Management
//----------------------------------------------------------

void updateScreenTimeout() {
  // Check if screen timeout is enabled (not "Always On")
  unsigned long timeoutDuration = screenTimeoutValues[screenTimeoutOption];
  if (timeoutDuration == 0) {
    // Always On - no timeout
    return;
  }

  unsigned long now = millis();
  unsigned long elapsed = now - lastActivityTime;

  if (screenState == SCREEN_ON && elapsed >= timeoutDuration) {
    M5Cardputer.Display.setBrightness(0);
    screenState = SCREEN_OFF;
    Serial.println("Screen off");
  }
}

void wakeScreen() {
  lastActivityTime = millis();
  if (screenState != SCREEN_ON) {
    M5Cardputer.Display.setBrightness(normalBrightness);
    screenState = SCREEN_ON;
    needsFullRedraw = true;
    Serial.println("Screen wake");
  }
}

//----------------------------------------------------------
// Main Page
//----------------------------------------------------------

void drawMainPageStatic() {
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  drawBattery(true);
  
  // Temperature box
  drawThickRoundRect(tempBoxX, boxY, boxWidth, boxHeight, boxRadius, boxBorderWidth, COLOR_TEMP);
  drawThermometerIcon(tempBoxX + boxWidth/2, boxY + 22, COLOR_TEMP);
  
  // Humidity box
  drawThickRoundRect(humidBoxX, boxY, boxWidth, boxHeight, boxRadius, boxBorderWidth, COLOR_HUMIDITY);
  drawDropletIcon(humidBoxX + boxWidth/2, boxY + 22, COLOR_HUMIDITY);
  
  // Pressure box
  drawThickRoundRect(pressBoxX, boxY, boxWidth, boxHeight, boxRadius, boxBorderWidth, COLOR_PRESSURE);
  drawBarometerIcon(pressBoxX + boxWidth/2, boxY + 22, COLOR_PRESSURE);
  
  // Reset displayed values to force redraw
  dispTemp = -999.0;
  dispHumidity = -999.0;
  dispPressure = -999.0;
}

void updateSingleBoxValue(int boxX, float value, float &dispValue, 
                          uint16_t color, const char* format, const char* unit) {
  // Only update if value changed (with small tolerance)
  float diff = value - dispValue;
  if (diff < 0) diff = -diff;
  if (diff < 0.05) return;
  
  dispValue = value;
  int valueY = boxY + 45;
  int unitY = boxY + 68;
  // Clear value and unit area
  int clearX = boxX + boxBorderWidth + 2;
  int clearW = boxWidth - (boxBorderWidth * 2) - 4;
  M5Cardputer.Display.fillRect(clearX, valueY - 2, clearW, 35, TFT_BLACK);
  // Draw the value
  char buf[15];
  sprintf(buf, format, value);
  drawCenteredTextInBox(buf, boxX, boxWidth, valueY, 2, color);
  // Draw the unit
  drawCenteredTextInBox(unit, boxX, boxWidth, unitY, 1, color);
}

void updateMainPageValues() {
  updateSingleBoxValue(tempBoxX, getDisplayTemp(temperature), dispTemp, COLOR_TEMP, "%.1f", getTempUnit());
  updateSingleBoxValue(humidBoxX, humidity, dispHumidity, COLOR_HUMIDITY, "%.0f", "%");
  updateSingleBoxValue(pressBoxX, pressure, dispPressure, COLOR_PRESSURE, "%.0f", "hPa");
  drawBattery(false);
}

//----------------------------------------------------------
// Graph Page
//----------------------------------------------------------

void drawGraphPageStatic(const char* title, float* history, uint16_t color, const char* unit, float currentVal, bool convertToF = false) {
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  drawBattery(true);
  
  // Title and current value at top
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(color);
  M5Cardputer.Display.setCursor(5, 5);
  M5Cardputer.Display.print(title);
  // Current value next to title
  char valBuf[20];
  sprintf(valBuf, "%.1f %s", currentVal, unit);
  int titleWidth = strlen(title) * 6;
  M5Cardputer.Display.setCursor(5 + titleWidth + 10, 5);
  M5Cardputer.Display.print(valBuf);
  
  // Graph area (leave room for labels)
  int graphX = 30;
  int graphY = 18;
  int graphW = screenW - 35;
  int graphH = screenH - 45;
  // Graph border
  M5Cardputer.Display.drawRect(graphX, graphY, graphW, graphH, TFT_DARKGREY);
  
  // X-axis labels
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(TFT_DARKGREY);
  M5Cardputer.Display.setCursor(graphX, graphY + graphH + 3);
  M5Cardputer.Display.print("-1hr");
  M5Cardputer.Display.setCursor(graphX + graphW - 18, graphY + graphH + 3);
  M5Cardputer.Display.print("now");
  // ESC hint at bottom left
  M5Cardputer.Display.setCursor(5, screenH - 10);
  M5Cardputer.Display.print("ESC:back");
  if (historyCount > 1) {
    // Find min/max for scaling (with conversion if needed)
    float graphMin, graphMax;
    int firstIdx = (historyIndex - historyCount + historySize) % historySize;
    float firstVal = history[firstIdx];
    if (convertToF) firstVal = (firstVal * 9.0 / 5.0) + 32.0;
    graphMin = graphMax = firstVal;
    for (int i = 0; i < historyCount; i++) {
      int idx = (historyIndex - historyCount + i + historySize) % historySize;
      float val = history[idx];
      if (convertToF) val = (val * 9.0 / 5.0) + 32.0;
      if (val < graphMin) graphMin = val;
      if (val > graphMax) graphMax = val;
    }
    
    // Add padding
    float range = graphMax - graphMin;
    if (range < 2.0) {
      float mid = (graphMax + graphMin) / 2.0;
      graphMin = mid - 1.0;
      graphMax = mid + 1.0;
      range = 2.0;
    } else {
      float padding = range * 0.1;
      graphMin -= padding;
      graphMax += padding;
      range = graphMax - graphMin;
    }
    
    // Y axis labels
    M5Cardputer.Display.setTextColor(TFT_DARKGREY);
    M5Cardputer.Display.setTextSize(1);
    char labelBuf[10];
    
    sprintf(labelBuf, "%.0f", graphMax);
    M5Cardputer.Display.setCursor(2, graphY);
    M5Cardputer.Display.print(labelBuf);
    
    sprintf(labelBuf, "%.0f", graphMin);
    M5Cardputer.Display.setCursor(2, graphY + graphH - 8);
    M5Cardputer.Display.print(labelBuf);
    // Draw the line graph
    int prevPx = 0, prevPy = 0;
    for (int i = 0; i < historyCount; i++) {
      int idx = (historyIndex - historyCount + i + historySize) % historySize;
      float val = history[idx];
      if (convertToF) val = (val * 9.0 / 5.0) + 32.0;
      int px = graphX + 2 + (i * (graphW - 4)) / (historySize - 1);
      int py = graphY + graphH - 2 - (int)((val - graphMin) / range * (graphH - 4));
      M5Cardputer.Display.fillCircle(px, py, 1, color);
      
      if (i > 0) {
        M5Cardputer.Display.drawLine(prevPx, prevPy, px, py, color);
      }
      prevPx = px;
      prevPy = py;
    }
    
  } else {
    drawCenteredText("Collecting...", graphY + graphH/2 - 8, 1, TFT_DARKGREY);
  }
  
  // Reset displayed value
  graphDispValue = -999.0;
}

void updateGraphValue(float value, uint16_t color, const char* unit, const char* title) {
  // Only update if value changed
  float diff = value - graphDispValue;
  if (diff < 0) diff = -diff;
  if (diff < 0.05) return;
  
  graphDispValue = value;
  // Current value displayed at top next to title
  int titleWidth = strlen(title) * 6;
  int valX = 5 + titleWidth + 10;
  
  // Clear the value area
  M5Cardputer.Display.fillRect(valX, 3, 70, 12, TFT_BLACK);
  // Draw current value
  char valBuf[20];
  sprintf(valBuf, "%.1f %s", value, unit);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(color);
  M5Cardputer.Display.setCursor(valX, 5);
  M5Cardputer.Display.print(valBuf);
}

//----------------------------------------------------------
// Settings Page
//----------------------------------------------------------

void drawSettingsPageStatic() {
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  drawBattery(true);
  
  // Title
  drawCenteredText("SETTINGS", 5, 2, TFT_WHITE);
  // Draw menu items
  int itemY = 35;
  int itemHeight = 35;
  // Brightness option
  uint16_t brightColor = (settingsSelection == 0) ? TFT_YELLOW : TFT_WHITE;
  if (settingsSelection == 0) {
    M5Cardputer.Display.fillRoundRect(10, itemY - 3, screenW - 20, itemHeight - 2, 5, 0x2104);
  }
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(brightColor);
  M5Cardputer.Display.setCursor(20, itemY + 5);
  M5Cardputer.Display.print("Brightness:");
  
  // Draw brightness bar
  int barX = 90;
  int barY = itemY + 3;
  int barW = 100;
  int barH = 12;
  M5Cardputer.Display.drawRect(barX, barY, barW, barH, brightColor);
  int fillW = map(normalBrightness, 20, 100, 0, barW - 4);
  M5Cardputer.Display.fillRect(barX + 2, barY + 2, fillW, barH - 4, brightColor);
  
  // Brightness percentage
  char brightBuf[10];
  sprintf(brightBuf, "%d%%", normalBrightness);
  M5Cardputer.Display.setCursor(barX + barW + 8, itemY + 5);
  M5Cardputer.Display.print(brightBuf);
  
  // Temperature unit option
  itemY += itemHeight;
  uint16_t unitColor = (settingsSelection == 1) ? TFT_YELLOW : TFT_WHITE;
  if (settingsSelection == 1) {
    M5Cardputer.Display.fillRoundRect(10, itemY - 3, screenW - 20, itemHeight - 2, 5, 0x2104);
  }
  M5Cardputer.Display.setTextColor(unitColor);
  M5Cardputer.Display.setCursor(20, itemY + 5);
  M5Cardputer.Display.print("Temp Unit:");
  
  // Draw toggle
  int toggleX = 90;
  int toggleY = itemY + 2;
  
  // Celsius option
  if (!useFahrenheit) {
    M5Cardputer.Display.fillRoundRect(toggleX, toggleY, 40, 14, 3, unitColor);
    M5Cardputer.Display.setTextColor(TFT_BLACK);
  } else {
    M5Cardputer.Display.drawRoundRect(toggleX, toggleY, 40, 14, 3, unitColor);
    M5Cardputer.Display.setTextColor(unitColor);
  }
  M5Cardputer.Display.setCursor(toggleX + 10, toggleY + 3);
  M5Cardputer.Display.print("C");
  
  // Fahrenheit option
  if (useFahrenheit) {
    M5Cardputer.Display.fillRoundRect(toggleX + 45, toggleY, 40, 14, 3, unitColor);
    M5Cardputer.Display.setTextColor(TFT_BLACK);
  } else {
    M5Cardputer.Display.drawRoundRect(toggleX + 45, toggleY, 40, 14, 3, unitColor);
    M5Cardputer.Display.setTextColor(unitColor);
  }
  M5Cardputer.Display.setCursor(toggleX + 55, toggleY + 3);
  M5Cardputer.Display.print("F");

  // Screen timeout option
  itemY += itemHeight;
  uint16_t timeoutColor = (settingsSelection == 2) ? TFT_YELLOW : TFT_WHITE;
  if (settingsSelection == 2) {
    M5Cardputer.Display.fillRoundRect(10, itemY - 3, screenW - 20, itemHeight - 2, 5, 0x2104);
  }
  M5Cardputer.Display.setTextColor(timeoutColor);
  M5Cardputer.Display.setCursor(20, itemY + 5);
  M5Cardputer.Display.print("Timeout:");

  // Draw timeout options
  int optX = 90;
  int optY = itemY + 2;
  const char* timeoutLabels[] = {"10s", "30s", "Off"};

  for (int i = 0; i < 3; i++) {
    int btnX = optX + (i * 35);
    if (screenTimeoutOption == i) {
      M5Cardputer.Display.fillRoundRect(btnX, optY, 32, 14, 3, timeoutColor);
      M5Cardputer.Display.setTextColor(TFT_BLACK);
    } else {
      M5Cardputer.Display.drawRoundRect(btnX, optY, 32, 14, 3, timeoutColor);
      M5Cardputer.Display.setTextColor(timeoutColor);
    }
    M5Cardputer.Display.setCursor(btnX + 6, optY + 3);
    M5Cardputer.Display.print(timeoutLabels[i]);
  }

  // Instructions at bottom
  M5Cardputer.Display.setTextColor(TFT_DARKGREY);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setCursor(5, screenH - 10);
  M5Cardputer.Display.print("ESC:back | < >:change");
}

//----------------------------------------------------------
// Keyboard Handling
//----------------------------------------------------------

void handleKeyboard() {
  M5Cardputer.update();
  if (M5Cardputer.Keyboard.isChange()) {
    if (M5Cardputer.Keyboard.isPressed()) {
      Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();
      // Any key wakes the screen
      if (screenState != SCREEN_ON) {
        wakeScreen();
        return;
      }
      
      wakeScreen();
      // Reset timeout on any keypress
      
      for (auto key : status.word) {
        char c = key;
        Serial.printf("Key pressed: %c (0x%02X)\n", c, c);
        
        // Convert to uppercase for comparison (for letter keys)
        char upperC = c;
        if (upperC >= 'a' && upperC <= 'z') {
          upperC = upperC - 32;
        }
        
        // ESC key handling (` or ~ on Cardputer)
        if (c == '`' || c == '~' || c == 27) {  // 27 is ESC
          if (currentPage != 0) {
            currentPage = 0;
            needsFullRedraw = true;
            Serial.println("-> BACK to main");
          }
        }
        // Main menu keys
        else if (currentPage == 0) {
          // T for Temperature
          if (upperC == 'T') {
            currentPage = 1;
            needsFullRedraw = true;
            Serial.println("-> TEMP graph");
          }
          // H for Humidity
          else if (upperC == 'H') {
            currentPage = 2;
            needsFullRedraw = true;
            Serial.println("-> HUMIDITY graph");
          }
          // P for Pressure
          else if (upperC == 'P') {
            currentPage = 3;
            needsFullRedraw = true;
            Serial.println("-> PRESSURE graph");
          }
          // S for Settings
          else if (upperC == 'S') {
            currentPage = 4;
            needsFullRedraw = true;
            Serial.println("-> SETTINGS");
          }
        }
        // Settings page navigation with ;
        // . , / keys
        else if (currentPage == 4) {
          if (c == ';') {  // ; = Up
            settingsSelection--;
            if (settingsSelection < 0) settingsSelection = 0;
            needsFullRedraw = true;
          }
          else if (c == '.') {  // . = Down
            settingsSelection++;
            if (settingsSelection > 2) settingsSelection = 2;
            needsFullRedraw = true;
          }
          else if (c == ',') {  // , = Left (decrease/select C)
            if (settingsSelection == 0) {
              // Brightness
              normalBrightness -= 20;
              if (normalBrightness < 20) normalBrightness = 20;
              M5Cardputer.Display.setBrightness(normalBrightness);
              needsFullRedraw = true;
            } else if (settingsSelection == 1) {
              // Temperature unit - select Celsius
              useFahrenheit = false;
              dispTemp = -999.0;  // Force redraw of temp
              needsFullRedraw = true;
            } else if (settingsSelection == 2) {
              // Screen timeout - cycle left
              screenTimeoutOption--;
              if (screenTimeoutOption < 0) screenTimeoutOption = 0;
              needsFullRedraw = true;
            }
          }
          else if (c == '/') {  // / = Right (increase/select F)
            if (settingsSelection == 0) {
              // Brightness
              normalBrightness += 20;
              if (normalBrightness > 100) normalBrightness = 100;
              M5Cardputer.Display.setBrightness(normalBrightness);
              needsFullRedraw = true;
            } else if (settingsSelection == 1) {
              // Temperature unit - select Fahrenheit
              useFahrenheit = true;
              dispTemp = -999.0;  // Force redraw of temp
              needsFullRedraw = true;
            } else if (settingsSelection == 2) {
              // Screen timeout - cycle right
              screenTimeoutOption++;
              if (screenTimeoutOption > 2) screenTimeoutOption = 2;
              needsFullRedraw = true;
            }
          }
        }
      }
    }
  }
}

//----------------------------------------------------------
// Data Collection
//----------------------------------------------------------

void updateHistory() {
  unsigned long now = millis();
  if (now - lastHistoryUpdate >= historyInterval || historyCount == 0) {
    lastHistoryUpdate = now;
    tempHistory[historyIndex] = temperature;
    humidityHistory[historyIndex] = humidity;
    pressureHistory[historyIndex] = pressure;
    historyIndex = (historyIndex + 1) % historySize;
    if (historyCount < historySize) historyCount++;
    Serial.printf("History: %d points\n", historyCount);
  }
}

//----------------------------------------------------------
// Setup
//----------------------------------------------------------

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(normalBrightness);
  
  screenW = M5Cardputer.Display.width();
  screenH = M5Cardputer.Display.height();
  
  // Calculate box positions (3 boxes horizontally)
  int totalWidth = (boxWidth * 3) + (boxMargin * 2);
  int startX = (screenW - totalWidth) / 2;
  tempBoxX = startX;
  humidBoxX = startX + boxWidth + boxMargin;
  pressBoxX = startX + (boxWidth + boxMargin) * 2;
  boxY = topMargin + (screenH - topMargin - boxHeight) / 2;
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== CardENV Starting ===");
  Serial.printf("Screen: %d x %d\n", screenW, screenH);
  
  // Startup screen
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  drawCenteredText("CardENV", 40, 2, TFT_CYAN);
  drawCenteredText("Initializing...", 65, 1, TFT_WHITE);
  
  // Initialize I2C (Grove Port: G2=SDA, G1=SCL - same as CoreS3)
  Wire.begin(2, 1);
  delay(300);
  
  // Initialize sensors
  if (sht30.begin(&Wire, SHT3X_I2C_ADDR, 2, 1)) {
    Serial.println("SHT30 OK!");
    drawCenteredText("SHT30: OK", 85, 1, TFT_GREEN);
  } else {
    Serial.println("SHT30 FAILED!");
    drawCenteredText("SHT30: FAILED", 85, 1, TFT_RED);
  }
  
  delay(100);
  
  if (qmp6988.begin(&Wire, QMP6988_SLAVE_ADDRESS_L, 2, 1)) {
    Serial.println("QMP6988 OK!");
    drawCenteredText("QMP6988: OK", 100, 1, TFT_GREEN);
  } else if (qmp6988.begin(&Wire, 0x56, 2, 1)) {
    Serial.println("QMP6988 OK (0x56)!");
    drawCenteredText("QMP6988: OK", 100, 1, TFT_GREEN);
  } else {
    Serial.println("QMP6988 FAILED!");
    drawCenteredText("QMP6988: FAILED", 100, 1, TFT_RED);
  }
  
  // Show key hints
  drawCenteredText("T:Temp H:Humid P:Press S:Set", 118, 1, TFT_DARKGREY);
  
  Serial.println("=== Setup Complete ===\n");
  delay(2000);
  
  // Initialize readings
  sht30.update();
  qmp6988.update();
  temperature = sht30.cTemp;
  humidity = sht30.humidity;
  pressure = qmp6988.pressure / 100.0;
  // Store first history point
  tempHistory[0] = temperature;
  humidityHistory[0] = humidity;
  pressureHistory[0] = pressure;
  historyCount = 1;
  historyIndex = 1;
  
  lastActivityTime = millis();
  lastDisplayUpdate = millis();
  
  needsFullRedraw = true;
}

//----------------------------------------------------------
// Main Loop
//----------------------------------------------------------

void loop() {
  handleKeyboard();
  updateScreenTimeout();
  // Read sensors
  sht30.update();
  temperature = sht30.cTemp;
  humidity = sht30.humidity;
  
  qmp6988.update();
  pressure = qmp6988.pressure / 100.0;
  // Update history
  updateHistory();
  
  // Only update display every second
  unsigned long now = millis();
  bool shouldUpdateDisplay = (now - lastDisplayUpdate >= displayInterval);
  
  if (screenState != SCREEN_OFF) {
    if (needsFullRedraw) {
      needsFullRedraw = false;
      lastDisplayUpdate = now;
      switch (currentPage) {
        case 0: 
          drawMainPageStatic();
          updateMainPageValues();
          break;
        case 1: 
          drawGraphPageStatic("TEMPERATURE", tempHistory, COLOR_TEMP, getTempUnit(), getDisplayTemp(temperature), useFahrenheit);
          updateGraphValue(getDisplayTemp(temperature), COLOR_TEMP, getTempUnit(), "TEMPERATURE");
          break;
        case 2: 
          drawGraphPageStatic("HUMIDITY", humidityHistory, COLOR_HUMIDITY, "%", humidity);
          updateGraphValue(humidity, COLOR_HUMIDITY, "%", "HUMIDITY");
          break;
        case 3: 
          drawGraphPageStatic("PRESSURE", pressureHistory, COLOR_PRESSURE, "hPa", pressure);
          updateGraphValue(pressure, COLOR_PRESSURE, "hPa", "PRESSURE");
          break;
        case 4:
          drawSettingsPageStatic();
          break;
      }
    } else if (shouldUpdateDisplay) {
      lastDisplayUpdate = now;
      switch (currentPage) {
        case 0:
          updateMainPageValues();
          break;
        case 1:
          updateGraphValue(getDisplayTemp(temperature), COLOR_TEMP, getTempUnit(), "TEMPERATURE");
          drawBattery(false);
          break;
        case 2:
          updateGraphValue(humidity, COLOR_HUMIDITY, "%", "HUMIDITY");
          drawBattery(false);
          break;
        case 3:
          updateGraphValue(pressure, COLOR_PRESSURE, "hPa", "PRESSURE");
          drawBattery(false);
          break;
        case 4:
          drawBattery(false);
          break;
      }
    }
  }
  
  delay(50);
}