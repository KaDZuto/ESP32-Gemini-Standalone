/**
 * ESP32-S3 Standalone Gemini Voice Assistant
 * Полностью автономная работа: прямое общение с Google Gemini Cloud (HTTPS).
 * 
 * Пайплайн обработки:
 * 1. Запись 4 сек звука с I2S микрофона INMP441 в буфер PSRAM (WAV 16kHz Mono).
 * 2. Base64 кодирование аудио.
 * 3. Отправка в gemini-3.5-flash (понимание речи + генерация краткого ответа).
 * 4. Запрос синтеза речи в gemini-3.8-flash-tts -> получение аудиопотока WAV.
 * 5. Декодирование и прямое воспроизведение через ЦАП PCM5102.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <mbedtls/base64.h>

#if __has_include("config.h")
#include "config.h"
#else
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* GEMINI_API_KEY = "YOUR_GEMINI_API_KEY";
#endif

// Эндпоинты моделей Google
const char* LLM_URL = "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.5-flash:generateContent";
const char* TTS_URL = "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash-tts:generateContent";

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
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1; header[21] = 0; // PCM
    header[22] = 1; header[23] = 0; // Mono
    header[24] = (uint8_t)(sampleRate & 0xff);
    header[25] = (uint8_t)((sampleRate >> 8) & 0xff);
    header[26] = (uint8_t)((sampleRate >> 16) & 0xff);
    header[27] = (uint8_t)((sampleRate >> 24) & 0xff);
    header[28] = (uint8_t)(byteRate & 0xff);
    header[29] = (uint8_t)((byteRate >> 8) & 0xff);
    header[30] = (uint8_t)((byteRate >> 16) & 0xff);
    header[31] = (uint8_t)((byteRate >> 24) & 0xff);
    header[32] = 2; header[33] = 0; // BlockAlign
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
    // Если аудио содержит 44-байтный WAV заголовок, пропускаем его для воспроизведения чистого PCM
    size_t offset = 0;
    if (length > 44 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F') {
        offset = 44;
    }
    i2s_write(I2S_DAC_PORT, data + offset, length - offset, &written, portMAX_DELAY);
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

    Serial.println("[MIC] Запись завершена. Отправка в Gemini...");
}

// Запрос синтеза речи TTS по полученному тексту
void requestTTS(const String& textToSpeak) {
    Serial.printf("[TTS] Запрос синтеза речи: \"%s\"\n", textToSpeak.c_str());

    String url = String(TTS_URL) + "?key=" + GEMINI_API_KEY;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;
    if (https.begin(client, url)) {
        https.addHeader("Content-Type", "application/json");

        DynamicJsonDocument reqDoc(1024);
        JsonObject part = reqDoc["contents"][0]["parts"].createNestedObject();
        part["text"] = textToSpeak;
        reqDoc["generationConfig"]["responseModalities"][0] = "AUDIO";

        String jsonPayload;
        serializeJson(reqDoc, jsonPayload);

        int httpCode = https.POST(jsonPayload);
        if (httpCode == HTTP_CODE_OK) {
            String respStr = https.getString();
            DynamicJsonDocument respDoc(65536);
            DeserializationError err = deserializeJson(respDoc, respStr);

            if (!err) {
                const char* b64Audio = respDoc["candidates"][0]["content"]["parts"][0]["inlineData"]["data"];
                if (b64Audio) {
                    size_t rawLen = strlen(b64Audio);
                    size_t outLen = 0;
                    uint8_t* audioBytes = (uint8_t*)ps_malloc(rawLen);

                    if (audioBytes && mbedtls_base64_decode(audioBytes, rawLen, &outLen, (const uint8_t*)b64Audio, rawLen) == 0) {
                        Serial.printf("[TTS] Воспроизведение звука (%d байт)...\n", outLen);
                        playPCM(audioBytes, outLen);
                        free(audioBytes);
                    } else {
                        Serial.println("[ERROR] Ошибка декодирования Base64 в TTS!");
                    }
                }
            } else {
                Serial.printf("[ERROR] Ошибка десериализации JSON TTS: %s\n", err.c_str());
            }
        } else {
            Serial.printf("[ERROR] Ошибка HTTP TTS: %d\n", httpCode);
            Serial.println(https.getString());
        }
        https.end();
    }
}

// Отправка записанного голоса в LLM gemini-3.5-flash
void processVoiceInteraction() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Нет подключения к сети!");
        return;
    }

    // 1. Кодируем записанный WAV в Base64
    size_t b64Len = 0;
    mbedtls_base64_encode(NULL, 0, &b64Len, wavBuffer, TOTAL_WAV_SIZE);

    char* b64Audio = (char*)ps_malloc(b64Len + 1);
    if (!b64Audio) {
        Serial.println("[ERROR] Недостаточно памяти PSRAM для Base64 буфера!");
        return;
    }

    mbedtls_base64_encode((uint8_t*)b64Audio, b64Len + 1, &b64Len, wavBuffer, TOTAL_WAV_SIZE);
    b64Audio[b64Len] = '\0';

    String url = String(LLM_URL) + "?key=" + GEMINI_API_KEY;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;
    if (https.begin(client, url)) {
        https.addHeader("Content-Type", "application/json");

        String jsonPayload = "{\"contents\":[{\"parts\":[";
        jsonPayload += "{\"text\":\"Ты голосовой ассистент Юи. Отвечай кратко, дружелюбно, одним или двумя предложениями по-русски.\"},";
        jsonPayload += "{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"";
        jsonPayload += b64Audio;
        jsonPayload += "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":120}}";

        free(b64Audio); // Освобождаем память PSRAM

        Serial.println("[Gemini] Отправка голоса в gemini-3.5-flash...");
        int httpCode = https.POST(jsonPayload);

        if (httpCode == HTTP_CODE_OK) {
            String respStr = https.getString();
            DynamicJsonDocument doc(16384);
            DeserializationError err = deserializeJson(doc, respStr);

            if (!err) {
                const char* answerText = doc["candidates"][0]["content"]["parts"][0]["text"];
                if (answerText) {
                    Serial.printf("[Gemini Ответ]: %s\n", answerText);
                    // Озвучиваем ответ через Gemini TTS
                    requestTTS(String(answerText));
                } else {
                    Serial.println("[Gemini] Текст в ответе отсутствует");
                }
            } else {
                Serial.printf("[ERROR] Ошибка десериализации JSON: %s\n", err.c_str());
            }
        } else {
            Serial.printf("[ERROR] Ошибка HTTP от Gemini: %d\n", httpCode);
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
    Serial.println("Система готова к диалогу. Скажите что-нибудь в микрофон...");
}

void loop() {
    // Мониторим уровень шума / голоса с микрофона INMP441
    int16_t sample = 0;
    size_t bytesRead = 0;
    i2s_read(I2S_MIC_PORT, &sample, sizeof(int16_t), &bytesRead, 10);

    if (abs(sample) > VAD_THRESHOLD) {
        Serial.printf("[VAD] Голос обнаружен (амплитуда=%d)!\n", abs(sample));
        recordAudio();
        processVoiceInteraction();
        Serial.println("\nГотов к следующему вопросу...");
    }

    delay(20);
}
