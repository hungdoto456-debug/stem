# Water Level & Soil Moisture Sensor Integration Guide

## 🌊 **Water Level Sensor Integration**

### Supported Sensor Types

#### 1. **Capacitive Water Level Sensor (Recommended)**
- **Pros:** No corrosion, long-lasting, accurate
- **Cons:** More expensive
- **Example Models:** 
  - Capacitive Water Level Sensor V1.0
  - DFRobot SEN0193
- **Output:** Analog voltage (0-3.3V)
- **Wiring:**
  - VCC → ESP32 3.3V
  - GND → ESP32 GND
  - SIG → ESP32 GPIO 34

#### 2. **Ultrasonic Water Level Sensor**
- **Pros:** Non-contact, no corrosion
- **Cons:** Requires more power, larger size
- **Example Models:** HC-SR04, JSN-SR04T
- **Note:** Requires additional code for ultrasonic ranging

#### 3. **Float Switch with Resistive Divider**
- **Pros:** Simple, cheap
- **Cons:** May corrode, mechanical parts
- **Wiring:** Similar to capacitive sensors

### Configuration in Code

```cpp
// Pin configuration
const int WATER_LEVEL_PIN = 34;    // Analog pin for water level sensor

// Calibration values (adjust based on your sensor)
const int WATER_LEVEL_DRY = 0;      // Value when no water
const int WATER_LEVEL_FULL = 4095;  // Value when fully submerged
const int WATER_LEVEL_MIN = 0;      // Minimum percentage
const int WATER_LEVEL_MAX = 100;    // Maximum percentage

// Alert threshold
const float WATER_LEVEL_ALERT = 20.0;   // Alert when water level < 20%
```

### Calibration Procedure

1. **Empty the container** and note the analog reading:
   ```cpp
   int dryReading = analogRead(WATER_LEVEL_PIN);
   // Update: const int WATER_LEVEL_DRY = dryReading;
   ```

2. **Fill the container completely** and note the analog reading:
   ```cpp
   int fullReading = analogRead(WATER_LEVEL_PIN);
   // Update: const int WATER_LEVEL_FULL = fullReading;
   ```

3. **Test at 50% level** to verify linear response

### Code Functions

```cpp
// Initialize water level sensor
void setupWaterSensor() {
  pinMode(WATER_LEVEL_PIN, INPUT);
  // Test the sensor
  int test_reading = analogRead(WATER_LEVEL_PIN);
  if (test_reading >= 0 && test_reading <= 4095) {
    water_sensor_ok = true;
  }
}

// Read water level with moving average
int readWaterLevel() {
  if (!water_sensor_ok) return -1;
  
  int raw_value = analogRead(WATER_LEVEL_PIN);
  
  // Apply moving average filter
  tong_water = tong_water - mang_water[water_vi_tri] + raw_value;
  mang_water[water_vi_tri] = raw_value;
  water_vi_tri = (water_vi_tri + 1) % water_mau;
  
  int avg_value = tong_water / water_mau;
  water_level_raw = avg_value;
  
  // Convert to percentage
  water_level_percent = map(avg_value, WATER_LEVEL_DRY, WATER_LEVEL_FULL, WATER_LEVEL_MIN, WATER_LEVEL_MAX);
  water_level_percent = constrain(water_level_percent, 0, 100);
  
  // Check for alert
  water_alert = (water_level_percent < WATER_LEVEL_ALERT);
  
  return avg_value;
}
```

---

## 🌱 **Soil Moisture Sensor Integration**

### Supported Sensor Types

#### 1. **Capacitive Soil Moisture Sensor (Recommended)**
- **Pros:** No corrosion, long-lasting, accurate
- **Cons:** More expensive
- **Example Models:**
  - Capacitive Soil Moisture Sensor v1.2
  - DFRobot SEN0193
  - Adafruit Stemma Soil Sensor
- **Output:** Analog voltage (0-3.3V)
- **Wiring:**
  - VCC → ESP32 3.3V
  - GND → ESP32 GND
  - SIG → ESP32 GPIO 35

#### 2. **Resistive Soil Moisture Sensor**
- **Pros:** Cheap, widely available
- **Cons:** Corrodes over time, less accurate
- **Example Models:** FC-28, YL-69, HL-69
- **Output:** Analog voltage (resistance changes with moisture)
- **Wiring:**
  - VCC → ESP32 3.3V or 5V (check sensor specs)
  - GND → ESP32 GND
  - SIG → ESP32 GPIO 35

### Configuration in Code

```cpp
// Pin configuration
const int SOIL_MOISTURE_PIN = 35;   // Analog pin for soil moisture sensor

// Calibration values (adjust based on your sensor)
const int SOIL_DRY = 0;             // Value when completely dry
const int SOIL_WET = 4095;          // Value when completely wet
const int SOIL_MOISTURE_MIN = 0;    // Minimum percentage
const int SOIL_MOISTURE_MAX = 100;  // Maximum percentage

// Alert threshold
const float SOIL_MOISTURE_ALERT = 30.0; // Alert when soil moisture < 30%
```

### Calibration Procedure

#### For Capacitive Sensors:
1. **In air (completely dry):**
   ```cpp
   int dryReading = analogRead(SOIL_MOISTURE_PIN);
   // Update: const int SOIL_DRY = dryReading;
   ```

2. **Submerged in water:**
   ```cpp
   int wetReading = analogRead(SOIL_MOISTURE_PIN);
   // Update: const int SOIL_WET = wetReading;
   ```

3. **In moist soil:** Verify reading is between dry and wet values

#### For Resistive Sensors (FC-28, YL-69):
**Note:** Resistive sensors often work inversely - higher resistance (lower analog value) = more moisture.

1. **In air (completely dry):**
   ```cpp
   int dryReading = analogRead(SOIL_MOISTURE_PIN);
   // This will be a HIGH value (close to 4095)
   ```

2. **Submerged in water:**
   ```cpp
   int wetReading = analogRead(SOIL_MOISTURE_PIN);
   // This will be a LOW value (close to 0)
   ```

3. **Update the mapping in code:**
   ```cpp
   // For inverse relationship sensors:
   soil_moisture_percent = map(avg_value, SOIL_DRY, SOIL_WET, SOIL_MOISTURE_MAX, SOIL_MOISTURE_MIN);
   // Instead of:
   // soil_moisture_percent = map(avg_value, SOIL_DRY, SOIL_WET, SOIL_MOISTURE_MIN, SOIL_MOISTURE_MAX);
   ```

### Code Functions

```cpp
// Initialize soil moisture sensor
void setupSoilSensor() {
  pinMode(SOIL_MOISTURE_PIN, INPUT);
  // Test the sensor
  int test_reading = analogRead(SOIL_MOISTURE_PIN);
  if (test_reading >= 0 && test_reading <= 4095) {
    soil_sensor_ok = true;
  }
}

// Read soil moisture with moving average
int readSoilMoisture() {
  if (!soil_sensor_ok) return -1;
  
  int raw_value = analogRead(SOIL_MOISTURE_PIN);
  
  // Apply moving average filter
  tong_soil = tong_soil - mang_soil[soil_vi_tri] + raw_value;
  mang_soil[soil_vi_tri] = raw_value;
  soil_vi_tri = (soil_vi_tri + 1) % soil_mau;
  
  int avg_value = tong_soil / soil_mau;
  soil_moisture_raw = avg_value;
  
  // Convert to percentage
  soil_moisture_percent = map(avg_value, SOIL_DRY, SOIL_WET, SOIL_MOISTURE_MIN, SOIL_MOISTURE_MAX);
  soil_moisture_percent = constrain(soil_moisture_percent, 0, 100);
  
  // Check for alert
  soil_alert = (soil_moisture_percent < SOIL_MOISTURE_ALERT);
  
  return avg_value;
}
```

---

## 📊 **Data Display**

### OLED Display

The OLED shows:
- **Water Level:** XX.X% [ALERT! or OK]
- **Soil Moisture:** XX.X% [ALERT! or OK]
- Alerts are clearly marked for easy identification

### Web Interface

The web interface shows:
- **Water Level Card:**
  - Level percentage
  - Raw analog value
  - Status (OK/ALERT/NOT CONNECTED)
  
- **Soil Moisture Card:**
  - Moisture percentage
  - Raw analog value
  - Status (OK/ALERT/NOT CONNECTED)

- **Alerts Summary Section:**
  - Lists all active alerts
  - Shows alert thresholds
  - Visual indicators for severity

---

## ⚠️ **Alert System**

### Alert Conditions

```cpp
// Water level alert
water_alert = (water_level_percent < WATER_LEVEL_ALERT);

// Soil moisture alert
soil_alert = (soil_moisture_percent < SOIL_MOISTURE_ALERT);
```

### Alert Display

- **OLED:** Shows "ALERT!" next to the sensor reading
- **Web:** Shows red "ALERT" text with details
- **Serial Monitor:** Prints alert messages

### Configurable Thresholds

Both alert thresholds can be configured via the web interface:
- Water Level Alert Threshold: Default 20%
- Soil Moisture Alert Threshold: Default 30%

---

## 🔧 **Troubleshooting**

### Water Level Sensor Issues

| Issue | Possible Cause | Solution |
|-------|----------------|----------|
| Always 0% | Sensor not connected | Check wiring |
| Always 100% | Sensor shorted | Check for water damage |
| Unstable readings | Electrical noise | Add 0.1µF capacitor |
| Incorrect values | Wrong calibration | Recalibrate sensor |
| Corrosion | Using resistive sensor in water | Use capacitive sensor |

### Soil Moisture Sensor Issues

| Issue | Possible Cause | Solution |
|-------|----------------|----------|
| Always 0% or 100% | Sensor not connected | Check wiring |
| Readings don't change | Sensor in wrong medium | Test in air and water |
| Slow response | Soil type affects readings | Be patient, take multiple readings |
| Corrosion | Using resistive sensor | Use capacitive sensor |
| Salt buildup | Fertilizers in soil | Clean sensor or use capacitive |

### General Issues

| Issue | Solution |
|-------|----------|
| Analog readings unstable | Add capacitor between SIG and GND |
| ESP32 resets | Check power supply (use 3.3V for sensors) |
| Sensors not detected | Verify pin assignments in code |
| Incorrect percentages | Recalibrate sensors |

---

## 🎯 **Testing Your Sensors**

### Quick Test Code

Add this to your `loop()` function for testing:

```cpp
void testSensors() {
  static unsigned long lastTest = 0;
  if (millis() - lastTest >= 5000) {
    lastTest = millis();
    
    Serial.println("\n=== Sensor Test ===");
    
    // Water level sensor
    int waterRaw = analogRead(WATER_LEVEL_PIN);
    Serial.print("Water Level Raw: ");
    Serial.println(waterRaw);
    
    // Soil moisture sensor
    int soilRaw = analogRead(SOIL_MOISTURE_PIN);
    Serial.print("Soil Moisture Raw: ");
    Serial.println(soilRaw);
    
    // Light sensor
    float light = lightsensor.readLightLevel();
    Serial.print("Light: ");
    Serial.print(light);
    Serial.println(" lux");
    
    Serial.println("===================");
  }
}
```

### Expected Values

#### Water Level Sensor
- **Empty:** Close to `WATER_LEVEL_DRY` (0 by default)
- **Full:** Close to `WATER_LEVEL_FULL` (4095 by default)
- **Half:** Approximately halfway between dry and full

#### Soil Moisture Sensor (Capacitive)
- **In air:** Close to `SOIL_DRY` (0 by default)
- **In water:** Close to `SOIL_WET` (4095 by default)
- **In moist soil:** Between dry and wet values

#### Soil Moisture Sensor (Resistive - FC-28, YL-69)
- **In air:** HIGH value (close to 4095)
- **In water:** LOW value (close to 0)
- **In moist soil:** Between 0 and 4095

---

## 📈 **Sensor Data Interpretation**

### Water Level Sensor

| Percentage | Interpretation |
|------------|----------------|
| 0-20% | Low water - needs refilling |
| 20-40% | Warning level |
| 40-80% | Normal level |
| 80-100% | Full/near full |

### Soil Moisture Sensor

| Percentage | Interpretation | Action |
|------------|----------------|--------|
| 0-30% | Dry soil | Water plants |
| 30-60% | Moist soil | Monitor |
| 60-80% | Wet soil | Good for most plants |
| 80-100% | Saturated | May need drainage |

**Note:** Optimal moisture levels vary by plant type. Adjust thresholds accordingly.

---

## 🔄 **Maintenance Tips**

### For Resistive Sensors
1. **Clean regularly** - Remove corrosion with sandpaper or vinegar
2. **Use corrosion-resistant coatings** - Apply conformal coating
3. **Replace periodically** - Resistive sensors have limited lifespan
4. **Power down when not in use** - Extends sensor life

### For Capacitive Sensors
1. **Clean occasionally** - Wipe with damp cloth
2. **Avoid physical damage** - Handle with care
3. **Check connections** - Ensure good electrical contact

### For All Sensors
1. **Calibrate periodically** - Sensor characteristics may change over time
2. **Check wiring** - Loose connections can cause issues
3. **Verify power supply** - Ensure stable 3.3V or 5V power
4. **Test in different conditions** - Verify readings are accurate

---

## 🚀 **Advanced Usage**

### Multiple Sensors

To add multiple water level or soil moisture sensors:

1. **Define new pins:**
   ```cpp
   const int WATER_LEVEL_PIN_2 = 36;
   const int SOIL_MOISTURE_PIN_2 = 39;
   ```

2. **Add variables:**
   ```cpp
   int water_level_raw_2 = 0;
   float water_level_percent_2 = 0;
   ```

3. **Add initialization and reading functions:**
   ```cpp
   void setupWaterSensor2() { /* ... */ }
   int readWaterLevel2() { /* ... */ }
   ```

4. **Update display and web interface:**
   ```cpp
   // In updateDisplay():
   display.print("Water 2: ");
   display.print(water_level_percent_2, 1);
   display.println("%");
   ```

### Data Logging

To log sensor data to SD card:

1. **Add SD card module**
2. **Include SD library:**
   ```cpp
   #include <SD.h>
   ```
3. **Initialize SD card:**
   ```cpp
   if (!SD.begin(5)) {  // CS pin
     Serial.println("SD card initialization failed!");
   }
   ```
4. **Log data periodically:**
   ```cpp
   void logData() {
     File dataFile = SD.open("datalog.csv", FILE_WRITE);
     if (dataFile) {
       dataFile.print(millis());
       dataFile.print(",");
       dataFile.print(lux);
       dataFile.print(",");
       dataFile.print(water_level_percent);
       dataFile.print(",");
       dataFile.print(soil_moisture_percent);
       dataFile.println();
       dataFile.close();
     }
   }
   ```

### Automated Watering System

To add automated watering based on soil moisture:

1. **Add a water pump relay:**
   ```cpp
   const int PUMP_PIN = 4;
   ```

2. **Add watering logic:**
   ```cpp
   const float WATERING_THRESHOLD = 40.0; // Water when below 40%
   const unsigned long WATERING_DURATION = 5000; // 5 seconds
   
   void checkWatering() {
     if (soil_moisture_percent < WATERING_THRESHOLD) {
       digitalWrite(PUMP_PIN, HIGH);
       delay(WATERING_DURATION);
       digitalWrite(PUMP_PIN, LOW);
     }
   }
   ```

---

## 📚 **Additional Resources**

### Sensor Datasheets
- [BH1750 Datasheet](https://www.mouser.com/datasheet/Rohm/bh1750fvc-e-186247.pdf)
- [DS3231 Datasheet](https://datasheets.maximintegrated.com/en/ds/DS3231.pdf)
- [SSD1306 Datasheet](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)

### Capacitive Sensor Guides
- [Adafruit Capacitive Soil Sensor Guide](https://learn.adafruit.com/adafruit-stemma-soil-sensor-i2c-capacitive-moisture-sensor)
- [DFRobot Capacitive Water Level Sensor](https://wiki.dfrobot.com/Capacitive_Water_Level_Sensor_SKU_SEN0193)

### Resistive Sensor Guides
- [FC-28 Soil Moisture Sensor Guide](https://lastminuteengineers.com/soil-moisture-sensor-arduino-tutorial/)
- [YL-69 Soil Moisture Sensor Guide](https://components101.com/sensors/yl-69-soil-moisture-sensor)

---

## 💡 **Tips for Best Results**

1. **Use capacitive sensors** for long-term reliability
2. **Calibrate in your specific environment** - different soils/water containers may affect readings
3. **Take multiple readings** and average them for stability
4. **Power sensors only when reading** to prevent corrosion (for resistive sensors)
5. **Use separate power supplies** for pumps/motors to avoid electrical noise
6. **Shield your sensors** from direct sunlight and rain
7. **Test in different conditions** to understand your sensor's behavior
8. **Monitor long-term trends** rather than individual readings

---

## ✅ **Checklist Before Deployment**

- [ ] All sensors are properly connected
- [ ] All sensors are calibrated
- [ ] Alert thresholds are set appropriately
- [ ] OLED display is working
- [ ] WiFi AP is accessible
- [ ] Web interface loads correctly
- [ ] Serial monitor shows no errors
- [ ] All alerts are displaying correctly
- [ ] Lamp control is working as expected
- [ ] RTC is keeping accurate time

---

**Happy Monitoring!** 🌊🌱📊

For questions or issues, refer to the main README.md or the troubleshooting section above.
