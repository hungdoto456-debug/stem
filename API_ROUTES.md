# Các đường dẫn HTTP

Kết nối cùng mạng với thiết bị, ví dụ `http://192.168.4.1` ở chế độ AP hoặc `http://stem.local` nếu mDNS hoạt động. API `/api/*` trả `application/json; charset=utf-8`; số đo không có hoặc không hữu hạn là `null`. Không bật CORS. Giá trị chuỗi kỹ thuật và tên khóa giữ nguyên để lập trình.

## Bảo mật và an toàn

Không có xác thực, phân quyền, TLS hay bảo vệ CSRF: bất kỳ máy nào truy cập được ESP32 đều có thể điều khiển bơm, đèn hoặc đổi WiFi; trang web độc hại cũng có thể gửi biểu mẫu POST. Không công khai ra Internet, không chuyển tiếp cổng, dùng mạng tin cậy/cách ly và đổi mật khẩu AP mặc định. Không trả mật khẩu trong API hoặc giao diện. Không có CORS không ngăn CSRF.

Nước GPIO35, đất GPIO34, bơm GPIO25, đèn GPIO26. Relay kích mức thấp (LOW bật, HIGH tắt). ADC nước chỉ để tham khảo; không phát hiện chắc chắn đầu dò hở hoặc hết nước. Cần bộ ngắt cạn/chạy khô độc lập.

## GET /api/status

Ví dụ 200:

```json
{"health":{"light":true,"rtc":true},"light":{"lux":120.00,"average_lux":110.00},"water":{"adc":2000,"percent":48.84},"soil":{"adc":1000,"percent":24.42},"pump":false,"lamp":true,"rtc_time":"2026-11-24T10:00:00","ds3231_temperature_c":25.25,"wifi":{"mode":"AP","ip":"192.168.4.1"}}
```

`health.light` chỉ đúng sau mẫu BH1750 hợp lệ; trước đó hoặc khi lỗi, cả hai lux là `null`, trung bình chỉ gồm các mẫu hợp lệ sau lần kết nối gần nhất và relay giữ lệnh trước đó. `health.rtc` chỉ đúng khi thời gian hợp lệ, cờ mất nguồn đã xóa và các lần đọc I2C thành công; khi không hợp lệ, `rtc_time` và nhiệt độ là `null`. RTC được đọc tối đa mỗi giây khi có yêu cầu trạng thái. `lamp` phản ánh mức GPIO26 thực tế được đọc (LOW là lệnh bật), không chứng minh relay hay tải vật lý hoạt động; `pump` là lệnh bơm. `adc` là số ADC, `percent` là phần trăm nội suy theo cài đặt (có thể chưa được hiệu chuẩn). `rtc_time` không kèm múi giờ; nhiệt độ là nhiệt độ chip DS3231, không nhất thiết là nhiệt độ không khí. `wifi.mode` là `AP`, `STA`, hoặc `offline`; `ip` rỗng khi ngoại tuyến. Số liệu độ ẩm ban đầu là null; dữ liệu sau đó là mẫu gần nhất.

## POST /api/time

Đặt DS3231 theo giờ địa phương của trình duyệt. Gửi form `application/x-www-form-urlencoded` với **duy nhất** trường `time`, giá trị chính xác dạng `YYYY-MM-DDTHH:MM:SS` (năm 2000–2099, không có múi giờ hoặc hậu tố `Z`). Firmware kiểm tra lịch/ngày-tháng, ghi các thanh ghi DS3231 qua I2C, xóa cờ oscillator-stop và đọc lại cả thời gian lẫn nhiệt độ để xác minh. Thành công trả 200 `{"ok":true}`; dữ liệu sai trả 400; lỗi I2C hoặc xác minh thất bại trả 503 và thời gian vẫn không khả dụng. Không có đồng bộ tự động, không dùng thời điểm biên dịch, không diễn giải hay chuyển đổi múi giờ. Nút giao diện “Đặt giờ DS3231 theo thiết bị này” lấy giờ địa phương của thiết bị đang mở trình duyệt; giờ đó có thể khác múi giờ nơi ESP32 đặt.


## GET /api/config

Ví dụ 200:

```json
{"lamp":{"on_threshold_lux":100,"off_threshold_lux":200,"debounce_ms":15000},"moisture":{"soil_dry":0,"soil_wet":4095,"water_dry":0,"water_wet":4095,"soil_alert":30,"soil_calibrated":false,"water_calibrated":false},"pump_safety":{"max_runtime_s":30,"cooldown_s":300,"hysteresis_percent":5}}
```

Các ngưỡng đèn là hằng số firmware. Các trường độ ẩm là giá trị hiệu chuẩn ADC khô/ướt và ngưỡng bắt đầu theo phần trăm. Không chứa mật khẩu.

## POST /api/command

Gửi form `application/x-www-form-urlencoded` với **duy nhất** trường `command`, giá trị chính xác một trong `lamp_on`, `lamp_off`, `pump_on`, `pump_off`. Không chấp nhận JSON body. Ví dụ 200 `{"ok":true,"lamp":true}` hoặc `{"ok":true,"pump":false}`. Lệnh sai/thiếu/thừa trường hoặc lệnh trong chế độ tự động trả 400.

Chọn chế độ thủ công bằng `POST /api/mode` trước khi gửi lệnh. Bơm thủ công bỏ qua kiểm tra cấu hình, cảm biến, độ ẩm và thời gian nghỉ, nhưng vẫn dừng sau 30 giây. Gửi lại lệnh bật khi bơm đang chạy không đặt lại đồng hồ 30 giây. Trong chế độ tự động, bơm vẫn chịu các điều kiện cảm biến và thời gian nghỉ 5 phút.

## POST /api/mode

Gửi đúng một trường form `mode=manual` hoặc `mode=auto`. Chế độ được lưu vào bộ nhớ. Khi chuyển chế độ, trạng thái relay hiện tại được giữ; trong chế độ tự động, ngưỡng cảm biến có thể thay đổi trạng thái sau đó.

## POST /api/moisture

Gửi form có đủ `soil_dry`, `soil_wet`, `water_dry`, `water_wet` là số nguyên 0–4095, và `soil_alert` là số nguyên 0–95. Mỗi cặp khô/ướt phải khác nhau. Lưu vào bộ nhớ và đánh dấu đã hiệu chuẩn; bơm đang chạy bị tắt và bắt đầu nghỉ. Thành công 200 `{"ok":true}`; dữ liệu sai hoặc lưu thất bại trả 400 `{"ok":false,"reason":"Cài đặt độ ẩm không hợp lệ"}` hoặc lý do lưu thất bại.

## Các đường dẫn giao diện còn lại

- `GET /`: trang HTML giám sát, các biểu mẫu.
- `POST /moisture`: cùng tham số và hiệu lực như `/api/moisture`, thành công chuyển hướng 303 về `/`, lỗi trả văn bản.
- `POST /wifi`: biểu mẫu gồm `sta_ssid`, `sta_password`, `ap_ssid`, `ap_password`; tùy chọn `clear_sta_password`. Cả bốn trường bắt buộc, mật khẩu trống giữ giá trị cũ; `clear_sta_password` xóa mật khẩu trạm và không được gửi cùng mật khẩu trạm mới. SSID trạm trống tắt trạm. Mật khẩu không trống dài 8–63 ký tự ASCII in được, mật khẩu AP bắt buộc. Thành công trả văn bản 200 và khởi động lại; lỗi trả văn bản 400 hoặc 500. Đây không phải endpoint JSON.
- `GET /config`: đường dẫn cũ chỉ ghi log các tham số `on_threshold`, `off_threshold`, `delay`, không thay đổi ngưỡng; thiếu `on_threshold` trả 400, nếu có trả 302 về `/`.
- Đường dẫn hoặc phương thức `/api/*` không tồn tại trả 404 JSON `{"ok":false,"reason":"Không tìm thấy đường dẫn hoặc phương thức"}`; các đường dẫn khác trả văn bản 404.

## Python requests

```python
import requests

base = "http://192.168.4.1"
status = requests.get(f"{base}/api/status", timeout=5)
status.raise_for_status()
print(status.json())
print(requests.get(f"{base}/api/config", timeout=5).json())

requests.post(f"{base}/api/mode", data={"mode": "manual"}, timeout=5).raise_for_status()
response = requests.post(f"{base}/api/command", data={"command": "pump_on"}, timeout=5)
print(response.status_code, response.json())  # 409: xem reason

response = requests.post(f"{base}/api/moisture", data={
    "soil_dry": 3500, "soil_wet": 1500,
    "water_dry": 3500, "water_wet": 1500, "soil_alert": 30,
}, timeout=5)
response.raise_for_status()
print(response.json())
```

Chỉ hiệu chuẩn với số đo thực tế; ví dụ không đại diện cho đầu dò của bạn. Tránh ghi cài đặt thường xuyên vì bộ nhớ flash có giới hạn chu kỳ ghi.
