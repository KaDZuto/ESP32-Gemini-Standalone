/**
 * ESP32-S3 Standalone Gemini Voice Assistant (Direct HTTPS, NO PC / Bridge)
 * 
 * Напрямую общается с Google Gemini 2.0 Flash REST API по HTTPS:
 * 1. Записывает аудио с микрофона INMP441 в WAV-буфер.
 * 2. Кодирует WAV в Base64.
 * 3. Отправляет JSON POST с inlineData: audio/wav и generationConfig: responseModalities: ["AUDIO"].
 * 4. Получает аудио-ответ модели и проигрывает его через ЦАП PCM5102.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <mbedtls/base64.h>

// ================= НАСТРОЙКИ СЕТИ И GEMINI =================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// Бесплатный ключ из Google AI Studio (https://aistudio.google.com/)
const char* GEMINI_API_KEY = "YOUR_GEMINI_API_KEY";

// Модель Gemini с поддержкой аудио ввода и вывода
const char* GEMINI_URL = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent";

// ================= ПИНЫ I2S =================
// ЦАП PCM5102 (Вывод аудио)
#define I2S_DAC_PORT   I2S_NUM_0
#define PIN_DAC_BCK    7
#define PIN_DAC_LRCK   8
#define PIN_DAC_DIN    18

// Микрофон INMP441 (Ввод аудио)
#define I2S_MIC_PORT   I2S_NUM_1
#define PIN_MIC_SCK    2
#define PIN_MIC_WS     3
#define PIN_MIC_SD     4

// ================= АУДИО НАСТРОЙКИ =================
#define SAMPLE_RATE 16000
#define RECORD_SECONDS 4
#define TOTAL_SAMPLES (SAMPLE_RATE * RECORD_SECONDS)
#define PCM_BUFFER_SIZE (TOTAL_SAMPLES * sizeof(int16_t))
#define WAV_HEADER_SIZE 44
#define TOTAL_WAV_SIZE (PCM_BUFFER_SIZE + WAV_HEADER_SIZE)

// Буферы в PSRAM
uint8_t* wavBuffer = nullptr;

// Порог громкости для обнаружения речи (VAD)
#define VAD_THRESHOLD 1200

void createWavHeader(uint8_t* header, int totalDataLen) {
    int totalFileLen = totalDataLen + 36;
    int sampleRate = SAMPLE_RATE;
    int byteRate = sampleRate * 1 * 2; // Mono 16-bit

    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[4] = (uint8_t)(totalFileLen & 0xff);
    header[5] = (uint8_t)((totalFileLen >> 8) & 0xff);
    header[6] = (uint8_t)((totalFileLen >> 16) & 0xff);
    header[7] = (uint8_t)((totalFileLen >> 24) & 0xff);
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0; // Subchunk1Size (16 for PCM)
    header[20] = 1; header[21] = 0; // AudioFormat (1 for PCM)
    header[22] = 1; header[23] = 0; // NumChannels (1 = Mono)
    header[24] = (uint8_t)(sampleRate & 0xff);
    header[25] = (uint8_t)((sampleRate >> 8) & 0xff);
    header[26] = (uint8_t)((sampleRate >> 16) & 0xff);
    header[27] = (uint8_t)((sampleRate >> 24) & 0xff);
    header[28] = (uint8_t)(byteRate & 0xff);
    header[29] = (uint8_t)((byteRate >> 8) & 0xff);
    header[30] = (uint8_t)((byteRate >> 16) & 0xff);
    header[31] = (uint8_t)((byteRate >> 24) & 0xff);
    header[32] = 2; header[33] = 0; // BlockAlign (channels * bits / 8)
    header[34] = 16; header[35] = 0; // BitsPerSample
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    header[40] = (uint8_t)(totalDataLen & 0xff);
    header[41] = (uint8_t)((totalDataLen >> 8) & 0xff);
    header[42] = (uint8_t)((totalDataLen >> 16) & 0xff);
    header[43] = (uint8_t)((totalDataLen >> 24) & 0xff);
}

void initI2S() {
    // 1. ЦАП PCM5102 (Воспроизведение, 24kHz / 16-bit Mono)
    i2s_config_t dac_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 24000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false
    };

    i2s_pin_config_t dac_pins = {
        .bck_io_num = PIN_DAC_BCK,
        .ws_io_num = PIN_DAC_LRCK,
        .data_out_num = PIN_DAC_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    i2s_driver_install(I2S_DAC_PORT, &dac_config, 0, NULL);
    i2s_set_pin(I2S_DAC_PORT, &dac_pins);

    // 2. Микрофон INMP441 (Запись, 16kHz / 16-bit Mono)
    i2s_config_t mic_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false
    };

    i2s_pin_config_t mic_pins = {
        .bck_io_num = PIN_MIC_SCK,
        .ws_io_num = PIN_MIC_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PIN_MIC_SD
    };

    i2s_driver_install(I2S_MIC_PORT, &mic_config, 0, NULL);
    i2s_set_pin(I2S_MIC_PORT, &mic_pins);

    Serial.println("[I2S] Аудио порты инициализированы.");
}

void playPCM(const uint8_t* data, size_t length) {
    size_t written = 0;
    i2s_write(I2S_DAC_PORT, data, length, &written, portMAX_DELAY);
}

void recordAudio() {
    Serial.println("[MIC] Идет запись аудио (4 сек)... Говорите!");

    createWavHeader(wavBuffer, PCM_BUFFER_SIZE);

    size_t bytesRead = 0;
    size_t totalRead = 0;
    uint8_t* pcmTarget = wavBuffer + WAV_HEADER_SIZE;

    while (totalRead < PCM_BUFFER_SIZE) {
        size_t toRead = PCM_BUFFER_SIZE - totalRead;
        if (toRead > 1024) toRead = 1024;

        i2s_read(I2S_MIC_PORT, pcmTarget + totalRead, toRead, &bytesRead, portMAX_DELAY);
        totalRead += bytesRead;
    }

    Serial.println("[MIC] Запись завершена. Отправка в Gemini Cloud...");
}

void sendToGemini() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Нет подключения к сети!");
        return;
    }

    // 1. Кодируем WAV в Base64
    size_t b64Len = 0;
    mbedtls_base64_encode(NULL, 0, &b64Len, wavBuffer, TOTAL_WAV_SIZE);

    char* b64Audio = (char*)ps_malloc(b64Len + 1);
    if (!b64Audio) {
        Serial.println("[ERROR] Недостаточно PSRAM для Base64 буфера!");
        return;
    }

    mbedtls_base64_encode((uint8_t*)b64Audio, b64Len + 1, &b64Len, wavBuffer, TOTAL_WAV_SIZE);
    b64Audio[b64Len] = '\0';

    // 2. Формируем URL с ключом
    String url = String(GEMINI_URL) + "?key=" + GEMINI_API_KEY;

    WiFiClientSecure client;
    client.setInsecure(); // Для простоты без корневого CA сертификата

    HTTPClient https;
    if (https.begin(client, url)) {
        https.addHeader("Content-Type", "application/json");

        // Формируем тело JSON
        // Используем String / stream для экономии оперативной памяти
        String jsonPayload = "{\"contents\":[{\"parts\":[";
        jsonPayload += "{\"text\":\"Ты голосовой ассистент Юи. Отвечай кратко на русском языке голосом.\"},";
        jsonPayload += "{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
        jsonPayload += b64Audio;
        jsonPayload += "\"}}]}],\"generationConfig\":{\"responseModalities\":[\"AUDIO\"]}}";

        // Освобождаем b64 буфер сразу после упаковки в payload
        free(b64Audio);

        Serial.println("[Gemini] Отправка HTTPS POST запроса...");
        int httpCode = https.POST(jsonPayload);

        if (httpCode == HTTP_CODE_OK) {
            Serial.println("[Gemini] Ответ получен! Парсинг аудио...");
            String responseStr = https.getString();

            // Парсим ответ JSON
            DynamicJsonDocument doc(32768);
            DeserializationError err = deserializeJson(doc, responseStr);

            if (!err) {
                const char* b64RespAudio = doc["candidates"][0]["content"]["parts"][0]["inlineData"]["data"];
                if (b64RespAudio) {
                    size_t outLen = 0;
                    size_t rawRespLen = strlen(b64RespAudio);
                    uint8_t* decodedAudio = (uint8_t*)ps_malloc(rawRespLen);

                    if (decodedAudio && mbedtls_base64_decode(decodedAudio, rawRespLen, &outLen, (const uint8_t*)b64RespAudio, rawRespLen) == 0) {
                        Serial.printf("[Gemini] Воспроизведение ответа (%d байт)...\n", outLen);
                        playPCM(decodedAudio, outLen);
                        free(decodedAudio);
                    } else {
                        Serial.println("[ERROR] Ошибка декодирования Base64 аудио ответа!");
                    }
                } else {
                    Serial.println("[Gemini] В ответе нет поля inlineData (возможно текстовый ответ)");
                    const char* textResp = doc["candidates"][0]["content"]["parts"][0]["text"];
                    if (textResp) Serial.println(textResp);
                }
            } else {
                Serial.printf("[ERROR] Ошибка десериализации JSON: %s\n", err.c_str());
            }
        } else {
            Serial.printf("[ERROR] HTTP Код ошибки: %d\n", httpCode);
            Serial.println(https.getString());
        }

        https.end();
    } else {
        Serial.println("[ERROR] Не удалось подключиться к Google API");
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== ESP32-S3 Standalone Gemini Voice Assistant ===");

    // Проверяем наличие PSRAM
    if (psramInit()) {
        Serial.printf("[PSRAM] Доступно памяти: %d байт\n", ESP.getFreePsram());
        wavBuffer = (uint8_t*)ps_malloc(TOTAL_WAV_SIZE);
        if (!wavBuffer) {
            Serial.println("[FATAL] Не удалось выделить буфер WAV в PSRAM!");
            while (true) delay(1000);
        }
    } else {
        Serial.println("[FATAL] Плата не обнаружила PSRAM!");
        while (true) delay(1000);
    }

    initI2S();

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("[WiFi] Подключение");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[WiFi] Подключено! IP: " + WiFi.localIP().toString());
    Serial.println("Система готова. Ожидание голоса...");
}

void loop() {
    // Детекция активности голоса (VAD)
    int16_t sample = 0;
    size_t bytesRead = 0;
    i2s_read(I2S_MIC_PORT, &sample, sizeof(int16_t), &bytesRead, 10);

    if (abs(sample) > VAD_THRESHOLD) {
        Serial.printf("[VAD] Обнаружен звук (амплитуда=%d). Начинаем запись...\n", abs(sample));
        recordAudio();
        sendToGemini();
        Serial.println("\nГотов к следующему вопросу...");
    }

    delay(20);
}
