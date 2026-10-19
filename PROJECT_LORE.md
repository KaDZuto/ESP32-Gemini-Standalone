# Контекст проекта ESP32-Gemini-Standalone

## 1. Концепция
Автономный голосовой помощник без локального моста / сервера. Единственное аппаратное устройство — ESP32-S3 с выходом в интернет через Wi-Fi.

## 2. Спецификация API
- **Endpoint**: `https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=...`
- **Протокол**: HTTPS POST (REST)
- **Вход**: WAV Base64 (16kHz 16-bit Mono) в блоке `inlineData`
- **Выход**: PCM/WAV Base64 в поле `responseModalities: ["AUDIO"]`

## 3. Требования к памяти
- Плата ESP32-S3 **обязательно** с PSRAM (N8R8 или N16R8), так как аудио-буферы для Base64 кодирования и декодирования размещаются в PSRAM (`ps_malloc`).

## 4. Пины
- DAC PCM5102: BCK=7, LRCK=8, DIN=18
- MIC INMP441: SCK=2, WS=3, SD=4
