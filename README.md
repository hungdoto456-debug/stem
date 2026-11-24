# STEM System - ESP32 Light Control with Display and Web Interface

API HTTP và ví dụ Python: xem [API_ROUTES.md](API_ROUTES.md). API không có xác thực; chỉ sử dụng trên mạng tin cậy.

## Overview

This project implements an ESP32-based system that:
- Monitors ambient light using a BH1750 sensor
- Controls a lamp based on light thresholds
- Displays real-time data on an SSD1306 OLED screen
- Maintains accurate time with a DS3231 RTC module
- Provides a WiFi Access Point for configuration
- Serves a web interface for monitoring and configuration

## Hardware Requirements

- ESP32 development board
- BH1750 light sensor (I2C)
- SSD1306 OLED display (128x64, I2C)
- DS3231 RTC module (I2C)
- Relay or transistor for lamp control

## Pin Connections

| Component | Pin | ESP32 Pin |
|-----------|-----|-----------|
| BH1750 SDA | DAT | GPIO 21 |
| BH1750 SCL | SCL | GPIO 22 |
| SSD1306 SDA | SDA | GPIO 21 |
| SSD1306 SCL | SCL | GPIO 22 |
| DS3231 SDA | SDA | GPIO 21 |
| DS3231 SCL | SCL | GPIO 22 |
| Lamp Control | OUT | GPIO 26 |
| Status LED | Anode via resistor | GPIO 5 |

Connect the LED cathode to GND and use a suitable series current-limiting resistor.
The LED is active-high: HIGH is on and LOW is off. GPIO 5 is a boot strapping
pin on the classic ESP32; ensure the LED circuit does not override its required
boot level, and check your board's pin restrictions.

**Note:** All I2C devices share the same SDA (GPIO 21) and SCL (GPIO 22) pins.

## Pump, moisture sensing, and safety

Moisture wiring uses water ADC GPIO35 and soil ADC GPIO34. The pump relay is GPIO25 and the lamp relay is GPIO26; both relays are active-LOW and are preloaded HIGH (OFF) before output enable. GPIO34/35 are input-only ESP32 ADC pins and must remain within the ESP32 voltage range.

The web page accepts persisted dry/wet ADC endpoints and a soil start threshold (percent). Settings are rejected unless numeric, in range, and each dry endpoint differs from its wet endpoint. Select manual mode to turn the pump or lamp on or off without calibration. Manual pump control bypasses sensor and cooldown checks, but still stops after 30 seconds; use an independent dry-run cutoff. Automatic pump control requires valid saved settings. The controller samples without blocking, starts at or below the configured threshold, stops at threshold + 5 percentage points (hysteresis), stops after 30 seconds maximum runtime, and enforces a 5-minute cooldown after every stop. Stale samples and invalid settings force automatic operation OFF. The UI reports raw ADC, calibrated percentages, calibration state, pump state, and these exact limits.

An ADC cannot reliably prove that a probe is disconnected or that wiring is safe; floating inputs can look valid. Water ADC is informational only. Install an independent low-water/dry-run cutoff and supervise initial testing with pump power disconnected. Firmware does not provide overflow or dry-run protection by itself.

## Features

### 1. Light Sensor Monitoring
- Uses BH1750 for accurate light measurement (lux)
- Implements moving average filtering for stable readings
- Configurable thresholds for lamp control

### 2. Lamp Control Logic
- **ON Threshold:** Lamp turns ON when light < 100 lux (default)
- **OFF Threshold:** Lamp turns OFF when light > 200 lux (default)
- **Debounce Delay:** 15 seconds to prevent rapid switching

### 3. OLED Display (SSD1306)
Displays:
- Current time (HH:MM:SS)
- Current date (DD/MM/YYYY)
- Instant light reading (lux)
- Moving average light reading (lux)
- Lamp status (ON/OFF)
- WiFi status and IP address

### 4. Real-Time Clock (DS3231)
- Maintains accurate time even when power is off
- Automatically sets to compile time on first run
- Provides date and time for display and web interface

### 5. WiFi Access Point
- Uses the compile-time AP defaults in `src/main.cpp` on first boot
- Allows configuration via web browser

### 6. Web Server
- **URL:** `http://stem.local` (via mDNS) or `http://192.168.4.1`
- Auto-refreshes every 5 seconds
- Displays:
  - Current time and date
  - Light sensor readings (instant and average)
  - Lamp status
  - RTC status
  - WiFi configuration
- Configuration form for:
  - ON threshold (lux)
  - OFF threshold (lux)
  - Debounce delay (ms)

## Installation

### 1. Install PlatformIO

Follow the [PlatformIO installation guide](https://docs.platformio.org/en/latest/installation.html).

### 2. Clone or Download the Project

```bash
cd D:\code_esp32\stem
```

### 3. Install Dependencies

PlatformIO will automatically install the required libraries:
- BH1750 (by Christopher Laws)
- RTClib (by Adafruit)
- Adafruit SSD1306
- NTPClient
- ArduinoJson

### 4. Build and Upload

```bash
pio run -t upload
```

Or use the PlatformIO extension in VSCode.

## Configuration

### WiFi Settings

The four `STEM_DEFAULT_STA_SSID`, `STEM_DEFAULT_STA_PASSWORD`,
`STEM_DEFAULT_AP_SSID`, and `STEM_DEFAULT_AP_PASSWORD` definitions at the top
of `src/main.cpp` are compile-time defaults. Set them before building; avoid
committing real credentials. A valid settings record saved through the web UI
always takes precedence, even after reflashing. On first boot, or if the record
is missing or invalid, defaults are used in RAM without writing to storage.
Invalid defaults are reported on serial and WiFi is not started.

An empty STA SSID disables station mode and retains the STA password for later.
Otherwise the device tries STA for 15 seconds, then falls back to the AP; an
empty STA password selects an open station network. SSIDs are 1–32 bytes (STA
may be empty), without control characters. Nonempty passwords are 8–63 printable
ASCII characters; the AP password is required. The same rules apply to defaults
and saved/web settings.

To change settings, connect to the active network and use **WiFi Configuration**
in the web UI. Blank password fields retain existing passwords; use the clear
checkbox to select an open STA network. Saving persists the entire settings record
and restarts. Passwords are not displayed.

#### Resetting saved data

The firmware stores application data in two separate ESP32 NVS namespaces:

- `stem-wifi`: the saved STA/AP SSIDs and passwords (`settings`).
- `stem-control`: the saved lamp/pump control configuration (`config`).

To reset **all application settings** while keeping the rest of the flash intact,
custom firmware can clear both namespaces using `Preferences::clear()` separately
for `stem-wifi` and `stem-control`, then reboot. This project does not provide a
reset command for that operation. A whole-chip erase is simpler but destructive.

To reset **all saved data in flash**, use the PlatformIO project command below
from the project root (for this checkout, `D:\code_esp32\stem`):

```bash
pio run -t erase
```

This is destructive: it erases the entire ESP32 flash, including the application,
NVS, and bootloader. No erase command has been executed as part of this work.
Afterward, re-upload the firmware (which also re-uploads the bootloader when the
board/package requires it), for example:

```bash
pio run -t upload
```

Reflashing without the erase step does not clear NVS, so saved values continue to
override compile-time defaults. After a whole-flash erase, the firmware uses the
compiled defaults: the current example AP SSID is `STEM_THPT_Trang_Dinh` and its
password is the value compiled in `src/main.cpp`; change that public example before
deployment. No reset, erase, upload, or credential change was performed here.

NVS-only erasure also removes other data stored in that partition. Its exact
command depends on the partition table and port, so no unverified address or
command is suggested here. The web interface is unauthenticated; restrict access
to the network.



### Status LED

GPIO 5 is initialized as an output at startup with a safe LOW default. The
active-high status LED is steady on while WiFi is connected and blinks at
500 ms intervals while WiFi is not connected (including AP mode). This is a
non-blocking indication updated from the main loop; it does not drive or alter
the GPIO 26 lamp output and does not add delays to WiFi handling. The firmware
constant is `STATUS_LED_PIN = 5` in `src/main.cpp`. The LED remains off during
setup, including the station connection attempt; indication starts in the main loop.

### Light Thresholds

Edit these constants in `src/main.cpp`:

```cpp
const float ON_THRESHOLD = 100;    // Lamp ON when light < 100 lux
const float OFF_THRESHOLD = 200;   // Lamp OFF when light > 200 lux
const unsigned long DEBOUNCE_DELAY = 15000; // 15 seconds
```

Or configure via the web interface.

### Pin Configuration

Edit these constants if using different pins:

```cpp
const int LAMP_PIN = 26;     // Lamp control pin
const int STATUS_LED_PIN = 5; // Active-high status LED
const int DAT_PIN = 21;     // I2C SDA pin
const int SCL_PIN = 22;     // I2C SCL pin
```

## Usage

### 1. Connect to WiFi

1. On your phone or computer, connect to the `STEM_Config` WiFi network
2. Use the configured AP password from your source defaults or saved settings.

### 2. Access Web Interface

Open a web browser and navigate to:
- `http://stem.local` (if mDNS works)
- `http://192.168.4.1` (direct IP)

### 3. Monitor Data

The web page will display:
- Current time and date from the DS3231 RTC
- Light sensor readings
- Lamp status
- System status

The OLED display will show the same information.

### 4. Configure Settings

Scroll down to the Configuration section on the web page:
- Set the ON threshold (lux)
- Set the OFF threshold (lux)
- Set the debounce delay (milliseconds)
- Click "Save Configuration"

**Note:** Currently, configuration changes are not persisted to EEPROM. They will be reset on power cycle. To make them persistent, you would need to add EEPROM support.

## Troubleshooting

### I2C Communication Issues

If devices are not detected:
1. Check all connections (SDA, SCL, GND, VCC)
2. Verify pull-up resistors are present (typically 4.7kΩ on SDA and SCL)
3. Check for address conflicts (all I2C devices must have unique addresses)
4. Use an I2C scanner to verify device addresses

Common I2C addresses:
- BH1750: 0x23 (default) or 0x5C
- SSD1306: 0x3C (default) or 0x3D
- DS3231: 0x68

### Display Not Working

1. Verify the display is powered (3.3V or 5V depending on model)
2. Check I2C connections
3. Verify the display address (try 0x3C or 0x3D)
4. Ensure the display library matches your screen size (128x64)

### RTC Not Working

1. Verify the DS3231 module is powered
2. Check I2C connections
3. Ensure the battery is inserted (for timekeeping when power is off)
4. Verify the address is 0x68

### WiFi Connection Issues

1. Verify the ESP32 antenna is connected
2. Check for interference from other devices
3. Try changing the WiFi channel
4. Ensure the password is correct

## Serial Monitor

Open the serial monitor at 115200 baud to see debug information:
- Light sensor readings
- Lamp state changes
- System initialization messages
- Error messages

## Future Enhancements

1. **EEPROM Support:** Save configuration to EEPROM for persistence
2. **NTP Sync:** Sync RTC with NTP server when WiFi is available
3. **MQTT Support:** Publish sensor data to MQTT broker
4. **OTA Updates:** Enable over-the-air firmware updates
5. **Multiple Sensors:** Add temperature, humidity, etc.
6. **Data Logging:** Log sensor data to SD card or cloud
7. **Mobile App:** Create a dedicated mobile app for monitoring

## License

This project is open source and available under the MIT License.

## Contributing

Contributions are welcome! Please feel free to submit issues or pull requests.





HỆ THỐNG STEM – ĐIỀU KHIỂN ĐÈN ESP32 VỚI MÀN HÌNH VÀ GIAO DIỆN WEB
1. Tổng quan
Dự án triển khai một hệ thống sử dụng ESP32 với các chức năng:
- Giám sát cường độ ánh sáng môi trường bằng cảm biến BH1750.
- Điều khiển đèn dựa trên các ngưỡng ánh sáng.
- Hiển thị dữ liệu thời gian thực trên màn hình OLED SSD1306.
- Duy trì thời gian chính xác bằng module đồng hồ thời gian thực DS3231 (RTC).
- Tạo điểm truy cập WiFi (Access Point) để cấu hình hệ thống.
- Cung cấp giao diện web để giám sát và cấu hình.
2. Yêu cầu phần cứng
- Bo mạch phát triển ESP32.
- Cảm biến ánh sáng BH1750 (I2C).
- Màn hình OLED SSD1306 (128x64, I2C).
- Module đồng hồ thời gian thực DS3231 (I2C).
- Relay hoặc transistor để điều khiển đèn.
3. Kết nối các chân
Linh kiện	Chân linh kiện	Chân ESP32
BH1750 SDA	DAT	GPIO 21
BH1750 SCL	SCL	GPIO 22
SSD1306 SDA	SDA	GPIO 21
SSD1306 SCL	SCL	GPIO 22
DS3231 SDA	SDA	GPIO 21
DS3231 SCL	SCL	GPIO 22
Điều khiển đèn	OUT	GPIO 26
LED trạng thái	Anode qua điện trở	GPIO 5

LED active-high: HIGH bật, LOW tắt; cathode nối GND. LED tắt khi khởi tạo,
sau đó sáng liên tục khi WiFi station kết nối, nhấp nháy mỗi 500 ms khi không
kết nối (kể cả chế độ AP). GPIO 5 là chân strapping, cần kiểm tra mạch và bo.

Lưu ý: Tất cả các thiết bị sử dụng giao tiếp I2C đều dùng chung hai chân SDA (GPIO 21) và SCL (GPIO 22).
4. Các chức năng chính
4.1. Giám sát cảm biến ánh sáng
- Sử dụng BH1750 để đo cường độ ánh sáng chính xác (đơn vị lux).
- Áp dụng phương pháp lọc trung bình trượt (Moving Average Filtering) để ổn định kết quả đo.
- Cho phép cấu hình các ngưỡng ánh sáng để điều khiển đèn.
4.2. Logic điều khiển đèn
- Ngưỡng bật (ON Threshold): Đèn bật khi ánh sáng nhỏ hơn 100 lux (mặc định).
- Ngưỡng tắt (OFF Threshold): Đèn tắt khi ánh sáng lớn hơn 200 lux (mặc định).
- Thời gian chống đóng/ngắt liên tục (Debounce Delay): 15 giây để tránh tình trạng đèn bật tắt quá nhanh.
4.3. Màn hình OLED (SSD1306)
Màn hình hiển thị:
- Thời gian hiện tại (HH:MM:SS).
- Ngày hiện tại (DD/MM/YYYY).
- Giá trị ánh sáng tức thời (lux).
- Giá trị ánh sáng trung bình trượt (lux).
- Trạng thái đèn (ON/OFF).
- Trạng thái WiFi và địa chỉ IP.
4.4. Đồng hồ thời gian thực (DS3231)
- Duy trì thời gian chính xác ngay cả khi mất nguồn điện.
- Tự động thiết lập thời gian theo thời điểm biên dịch chương trình trong lần chạy đầu tiên.
- Cung cấp ngày và giờ để hiển thị trên màn hình và giao diện web.
4.5. Điểm truy cập WiFi (Access Point)
- Tên WiFi (SSID): STEM_Config
- Mật khẩu: sử dụng mật khẩu AP đã cấu hình trong mã nguồn hoặc cài đặt đã lưu
- Địa chỉ IP: 192.168.4.1 (mặc định).
- Cho phép cấu hình hệ thống thông qua trình duyệt web.
4.6. Máy chủ web (Web Server)
- Địa chỉ truy cập: http://stem.local (thông qua mDNS) hoặc http://192.168.4.1.
- Tự động làm mới dữ liệu sau mỗi 5 giây.
Giao diện web hiển thị:
- Thời gian và ngày hiện tại.
- Số liệu cảm biến ánh sáng (tức thời và trung bình).
- Trạng thái đèn.
- Trạng thái RTC.
- Cấu hình WiFi.
Giao diện cũng cung cấp biểu mẫu cấu hình:
- Ngưỡng bật đèn (lux).
- Ngưỡng tắt đèn (lux).
- Thời gian chống đóng/ngắt liên tục (ms).
5. Cài đặt
5.1. Cài đặt PlatformIO
Thực hiện theo hướng dẫn cài đặt tại:

Hướng dẫn cài đặt PlatformIO.
5.2. Tải hoặc sao chép dự án
Mở terminal và nhập:
cd D:\code_esp32\stem


5.3. Cài đặt các thư viện phụ thuộc
PlatformIO sẽ tự động cài đặt các thư viện cần thiết:
- BH1750 (của Christopher Laws).
- RTClib (của Adafruit).
- Adafruit SSD1306.
- NTPClient.
- ArduinoJson.
5.4. Biên dịch và nạp chương trình
Sử dụng lệnh:
pio run -t upload


Hoặc sử dụng tiện ích mở rộng PlatformIO trong VS Code.
6. Cấu hình
6.1. Cài đặt WiFi
Chỉnh sửa các hằng số sau trong tệp src/main.cpp:
// Xem các hằng STEM_DEFAULT_STA_* và STEM_DEFAULT_AP_* trong src/main.cpp.


6.2. Cấu hình ngưỡng ánh sáng
Chỉnh sửa các hằng số trong src/main.cpp:
const float ON_THRESHOLD = 100;    // Bật đèn khi ánh sáng < 100 lux
const float OFF_THRESHOLD = 200;   // Tắt đèn khi ánh sáng > 200 lux
const unsigned long DEBOUNCE_DELAY = 15000; // 15 giây


Hoặc cấu hình trực tiếp thông qua giao diện web.
6.3. Cấu hình chân kết nối
Nếu sử dụng các chân GPIO khác, hãy chỉnh sửa các hằng số:
const int LAMP_PIN = 26;     // Chân điều khiển đèn
const int STATUS_LED_PIN = 5; // LED trạng thái active-high
const int DAT_PIN = 21;     // Chân I2C SDA
const int SCL_PIN = 22;     // Chân I2C SCL


7. Hướng dẫn sử dụng
7.1. Kết nối WiFi
1. Trên điện thoại hoặc máy tính, tìm và kết nối với mạng WiFi STEM_Config.
2. Nhập mật khẩu AP đã cấu hình.
7.2. Truy cập giao diện web
Mở trình duyệt và truy cập một trong hai địa chỉ:
- http://stem.local (nếu mDNS hoạt động).
- http://192.168.4.1 (truy cập trực tiếp bằng IP).
7.3. Giám sát dữ liệu
Trang web sẽ hiển thị:
- Thời gian và ngày hiện tại từ đồng hồ DS3231 RTC.
- Số liệu đo được từ cảm biến ánh sáng.
- Trạng thái đèn.
- Trạng thái hoạt động của hệ thống.
Màn hình OLED cũng sẽ hiển thị những thông tin tương tự.
7.4. Cấu hình hệ thống
Cuộn xuống phần Configuration (Cấu hình) trên trang web:
- Thiết lập ngưỡng bật đèn (lux).
- Thiết lập ngưỡng tắt đèn (lux).
- Thiết lập thời gian chống đóng/ngắt liên tục (mili giây).
- Nhấn nút Save Configuration (Lưu cấu hình).
Lưu ý: Hiện tại, các thay đổi cấu hình chưa được lưu vào EEPROM. Khi ngắt nguồn và khởi động lại, các thông số sẽ trở về giá trị mặc định.

Để lưu cấu hình lâu dài, cần bổ sung chức năng hỗ trợ EEPROM.
8. Xử lý sự cố
8.1. Lỗi giao tiếp I2C
Nếu các thiết bị không được nhận diện:
1. Kiểm tra toàn bộ kết nối (SDA, SCL, GND, VCC).
2. Kiểm tra điện trở kéo lên (Pull-up Resistors), thường có giá trị 4,7kΩ trên SDA và SCL.
3. Kiểm tra xung đột địa chỉ (các thiết bị I2C phải có địa chỉ riêng biệt).
4. Sử dụng chương trình quét I2C (I2C Scanner) để xác minh địa chỉ thiết bị.
Các địa chỉ I2C phổ biến:
- BH1750: 0x23 (mặc định) hoặc 0x5C.
- SSD1306: 0x3C (mặc định) hoặc 0x3D.
- DS3231: 0x68.
8.2. Màn hình không hoạt động
1. Kiểm tra nguồn điện cấp cho màn hình (3,3V hoặc 5V tùy model).
2. Kiểm tra kết nối I2C.
3. Xác minh địa chỉ màn hình (thử 0x3C hoặc 0x3D).
4. Đảm bảo thư viện màn hình tương thích với kích thước 128x64.
8.3. RTC không hoạt động
1. Kiểm tra nguồn điện cấp cho module DS3231.
2. Kiểm tra kết nối I2C.
3. Đảm bảo pin được lắp đúng vị trí để duy trì thời gian khi mất nguồn.
4. Xác minh địa chỉ thiết bị là 0x68.
8.4. Lỗi kết nối WiFi
1. Kiểm tra anten WiFi của ESP32.
2. Kiểm tra hiện tượng nhiễu sóng từ các thiết bị khác.
3. Thử thay đổi kênh WiFi.
4. Đảm bảo mật khẩu chính xác.
9. Serial Monitor
Mở Serial Monitor và thiết lập tốc độ truyền dữ liệu ở mức 115200 baud để theo dõi thông tin gỡ lỗi:
- Giá trị đo được từ cảm biến ánh sáng.
- Những thay đổi trạng thái của đèn.
- Thông báo khởi tạo hệ thống.
- Các thông báo lỗi.
10. Các tính năng có thể phát triển trong tương lai
1. Hỗ trợ EEPROM: Lưu cấu hình vào EEPROM để duy trì dữ liệu sau khi mất nguồn.
2. Đồng bộ thời gian NTP: Đồng bộ đồng hồ RTC với máy chủ NTP khi có kết nối WiFi.
3. Hỗ trợ MQTT: Gửi dữ liệu cảm biến đến máy chủ MQTT.
4. Cập nhật OTA: Cho phép cập nhật firmware thông qua mạng WiFi mà không cần kết nối cáp USB.
5. Nhiều loại cảm biến: Bổ sung cảm biến nhiệt độ, độ ẩm và các loại cảm biến khác.
6. Ghi nhật ký dữ liệu (Data Logging): Lưu dữ liệu cảm biến vào thẻ SD hoặc máy chủ đám mây.
7. Ứng dụng di động: Phát triển ứng dụng riêng để giám sát hệ thống.
11. Giấy phép
Dự án là mã nguồn mở và được phát hành theo giấy phép MIT (MIT License).
12. Đóng góp cho dự án
Mọi đóng góp đều được hoan nghênh! Người tham gia có thể gửi báo cáo lỗi (Issues) hoặc đề xuất thay đổi mã nguồn (Pull Requests).

Một số thuật ngữ kỹ thuật cần nắm
Vì tài liệu này liên quan trực tiếp đến phần lập trình ESP32 trong dự án STEM, mình tổng hợp thêm các thuật ngữ quan trọng:
Thuật ngữ	Ý nghĩa
ESP32	Vi điều khiển tích hợp WiFi và Bluetooth
I2C	Giao thức giao tiếp giữa các thiết bị
GPIO	Các chân vào/ra đa dụng
Lux	Đơn vị đo độ rọi ánh sáng
Moving Average	Phương pháp tính trung bình trượt
Threshold	Ngưỡng giới hạn
Debounce	Chống hiện tượng đóng/ngắt liên tục
RTC	Đồng hồ thời gian thực
Access Point	Điểm truy cập WiFi
Web Server	Máy chủ web
mDNS	Phương thức truy cập thiết bị bằng tên miền nội bộ
EEPROM	Bộ nhớ lưu trữ dữ liệu
OTA	Cập nhật chương trình qua mạng
MQTT	Giao thức truyền thông điệp nhẹ

Một lưu ý kỹ thuật: Tài liệu mô tả một hệ thống khá hoàn chỉnh, nhưng có hai điểm cần kiểm tra trước khi triển khai:
- Việc dùng chung I2C cho ba thiết bị là hoàn toàn hợp lý, miễn là địa chỉ không xung đột và kết nối điện phù hợp.
- GPIO 2 có thể ảnh hưởng đến quá trình khởi động trên một số phiên bản ESP32. Cần kiểm tra bo mạch thực tế trước khi chọn chân này để điều khiển relay.
Đây là tài liệu mô tả thiết kế và tính năng dự kiến, chưa phải bằng chứng rằng toàn bộ chức năng đã được lập trình và kiểm thử thành công.