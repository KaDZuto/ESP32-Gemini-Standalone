# ESP32-Gemini-Standalone (Без моста / Полностью автономный)

Голосовой ассистент на базе микроконтроллера **ESP32-S3** с прямым подключением к облаку **Google Gemini 2.0 Flash REST API** по Wi-Fi.

> **Важно**: Для работы не требуется компьютер или промежуточный сервер-мост. ESP32-S3 подключается к домашнему Wi-Fi роутеру и напрямую общается с Google Cloud по HTTPS.

---

## 💡 Принцип работы

1. **Запись**: ESP32 слушает микрофон INMP441 при обнаружении голоса (VAD) или удержании кнопки.
2. **Упаковка**: Записанное аудио кодируется в формат **WAV (16kHz 16-bit Mono)** и упаковывается в Base64 прямо в PSRAM микроконтроллера.
3. **Прямой запрос к Google**: ESP32 отправляет HTTPS POST запрос на:
   `https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=YOUR_API_KEY`
   с телом:
   - Аудио пользователя (`inlineData: audio/wav`)
   - Требование вернуть ответ голосом (`responseModalities: ["AUDIO"]`)
4. **Воспроизведение**: Google возвращает Base64-аудио ответа модели. ESP32 на лету декодирует сэмплы и воспроизводит их через ЦАП PCM5102 в динамик.

---

## 🔌 Подключение аппаратуры (Hardware)

### ESP32-S3 → I2S ЦАП PCM5102 (Вывод звука)
- **GPIO7**  → BCK (Bit Clock)
- **GPIO18** → DIN (Data In)
- **GPIO8**  → LRCK (Left-Right Clock)
- **GND**    → GND
- **5V**     → VCC

### ESP32-S3 → I2S Микрофон INMP441 (Ввод звука)
- **3.3V**   → VDD
- **GND**    → GND
- **GPIO4**  → SD (Data)
- **GPIO3**  → WS (Word Select / LRCK)
- **GPIO2**  → SCK (Serial Clock)
- **GND**    → L/R

---

## 🔑 Настройка ключа и сети
В файле [`src/main.cpp`](file:///run/media/alexius/%D0%9D%D0%BE%D0%B2%D1%8B%D0%B9%20%D1%82%D0%BE%D0%BC/%D0%9D%D0%95%D0%99%D0%A0%D0%9E%20%D0%9F%D0%A0%D0%9E%D0%95%D0%9A%D0%A2%D0%AB%20%D0%92%D0%A1%D0%AF%D0%9A%D0%98%D0%95/ESP32-Gemini-Standalone/src/main.cpp):
1. Укажите ваши `WIFI_SSID` и `WIFI_PASSWORD`.
2. Укажите бесплатный ключ `GEMINI_API_KEY` из [Google AI Studio](https://aistudio.google.com/).
