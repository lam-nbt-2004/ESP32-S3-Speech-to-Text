# Trợ lý giọng nói ESP32-S3

Thiết bị trợ lý giọng nói sử dụng ESP32-S3-N16R8, microphone INMP441, amplifier MAX98357A và loa 8 Ω / 3 W. Wake word được nhận diện trên board; server thực hiện STT, xử lý hội thoại và TTS. Firmware dùng ESP-IDF 6.1, có thể mở bằng Espressif IDE.

## Tính năng

- Nhận diện “Hi ESP” cục bộ bằng ESP-SR WakeNet `wn9_hiesp`.
- Thu mono 16 kHz; WebRTC NS xử lý tiếng mic trước khi mã hóa nếu `VA_NS_ENABLE` bật. Có thể tắt NS để so sánh với mic thô.
- Gain thích ứng từ 1× đến tối đa 2×, giới hạn mẫu trong miền signed 16-bit. Gain không tăng thêm trên frame gần im lặng; gain đã đạt được vẫn được giữ.
- Gửi Opus mono 16 kHz, 60 ms mỗi gói, bitrate encoder 24 kbit/s.
- Nhận TTS Opus từ server, giải mã thành PCM mono **16 kHz** và phát qua I2S ở biên độ **65%**. Theo giao thức server, TTS được công bố ở 24 kHz; decoder Opus trên board chọn đầu ra 16 kHz.
- Task phát riêng sở hữu decoder và I2S. Hàng đợi tối đa 48 gói và prebuffer 4 gói hoặc tối đa 240 ms kể từ gói đầu giúp chịu được nhịp nhận không đều. Khi server báo dừng TTS, board phát hết hàng đợi rồi mới tắt loa.
- Trò chuyện liên tục sau wake word đầu tiên; tự mở lượt nghe sau mỗi câu trả lời.
- Half-duplex: tạm ngừng đọc mic trong lúc TTS phát. Chưa có barge-in hoặc AEC.
- HTTPS discovery lấy URL/token qua `/api/device/ota/`, xác thực WebSocket bằng Bearer token và Device-Id là MAC Wi-Fi. Hỗ trợ ghép nối thiết bị trên trang server.
- Tự kết nối lại Wi-Fi/WebSocket; Wi-Fi power saving được tắt để giảm độ trễ truyền audio.

## Phần cứng

| Tín hiệu | GPIO |
|---|---:|
| INMP441 BCLK / WS / DOUT | 14 / 15 / 16 |
| MAX98357A BCLK / WS / DIN | 17 / 18 / 8 |

INMP441 dùng kênh trái, chân L/R nối GND. Loa nối giữa SPK+ và SPK− của MAX98357A; không nối hai đầu loa xuống GND. Dùng nguồn phù hợp từng module và nối chung GND. Firmware không sử dụng nút bấm hoặc LED.

## AFE và nhận dạng

AFE dùng một microphone (`M`), `AFE_TYPE_SR`, `AFE_MODE_HIGH_PERF`. WakeNet và VAD được bật; WebRTC NS tùy chọn, AEC tắt và AGC của AFE tắt. Gain thích ứng được áp dụng trong firmware trước encoder.

Khi NS bật, AFE tiếp tục xử lý trong các lượt nghe và PCM đầu ra AFE được gửi lên server, kể cả khoảng lặng. Firmware không dùng VAD cục bộ để cắt câu ở Stage Server: server tự xác định lúc nói xong trong mode `auto`. Khi NS tắt, AFE phục vụ wake word rồi tạm dừng trong hội thoại; đường gửi dùng mic thô.

## Cấu hình và sử dụng

Project dùng target `esp32s3`, flash 16 MB, octal PSRAM 8 MB và phân vùng `model` cho ESP-SR. Mở `menuconfig → Voice assistant` để thiết lập:

| Mục | Ý nghĩa |
|---|---|
| Bring-up stage | Stage 1: kiểm tra mic/loa; Stage 2: WakeNet/AFE và phát lại cục bộ; Stage 3: hội thoại với server |
| `VA_WIFI_SSID`, `VA_WIFI_PASSWORD` | Thông tin Wi-Fi |
| `VA_DEVICE_WS_URI` | URI dự phòng và địa chỉ cơ sở tạo URL discovery |
| `VA_DEVICE_TOKEN` | Token dự phòng khi discovery thất bại |
| `VA_WAKE_WORD_TEXT` | Cụm wake word báo server; đổi chuỗi không đổi model WakeNet |
| `VA_NS_ENABLE` | Bật/tắt WebRTC NS trên mic |

1. Cấp nguồn và chờ log `Voice server hello received`.
2. Nếu log báo chưa ghép nối, vào trang **Thiết bị giọng nói → Ghép nối thiết bị**, cho phép đúng MAC rồi khởi động lại board để lấy token.
3. Nói “Hi ESP”, sau đó nói câu lệnh. Sau khi loa phát xong, nói câu tiếp theo mà không cần wake word.

Serial monitor hiển thị STT, nội dung câu trả lời, trạng thái kết nối và cảnh báo audio. Chất lượng nhận dạng phụ thuộc mức tiếng mic, môi trường, NS và model/VAD trên server; NS cần được đánh giá bằng thử nghiệm thực tế.

Thông tin chi tiết về task, giao thức và xử lý audio nằm trong [WORKFLOW_REPORT.md](WORKFLOW_REPORT.md).
