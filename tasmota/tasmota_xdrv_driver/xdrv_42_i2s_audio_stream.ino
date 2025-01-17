
#ifdef USE_I2S_AUDIO_STREAM
/*
* Should define USE_I2S to be able to custom pins.
* 
* Default pins:
* bclkPin = 26;
* wclkPin = 25;
* doutPin = 22;
*/

#define XDRV_42           42

#define USE_I2S_EXTERNAL_DAC   1
//#define USE_I2S_NO_DAC                         // Add support for transistor-based output without DAC

#include "AudioFileSourceHTTPStream.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#ifdef USE_I2S_NO_DAC
  #include "AudioOutputI2SNoDAC.h"  // Transistor-driven lower quality connected to RX pin
#else
  #include "AudioOutputI2S.h"       // External I2S DAC IC
#endif  // USE_I2S_NO_DAC

#ifdef ESP8266
const int preallocateBufferSize = 5*1024;
#endif  // ESP8266
#ifdef ESP32
const int preallocateBufferSize = 64*1024;
#endif  // ESP32
const int preallocateCodecSize = 29192; // MP3 codec max mem needed
//const int preallocateCodecSize = 85332; // AAC+SBR codec max mem needed

AudioGeneratorMP3* mp3 = nullptr;
AudioFileSourceHTTPStream* file = nullptr;
#ifdef USE_I2S_NO_DAC
AudioOutputI2SNoDAC* out;
#else
AudioOutputI2S* out;
#endif  // USE_I2S_NO_DAC

AudioFileSourceBuffer* buff = NULL;

void* preallocateBuffer = NULL;
void* preallocateCodec = NULL;

char title[256];
uint8_t pos = 0;
uint8_t bufLvl = 0;
bool paused = false;

// should be in settings
uint8_t is2_volume;

void I2S_Init(void) {

#if USE_I2S_EXTERNAL_DAC
  #ifdef USE_I2S_NO_DAC
  out = new AudioOutputI2SNoDAC();
  #else
  out = new AudioOutputI2S();
  if (PinUsed(GPIO_I2S_OUT_CLK) && PinUsed(GPIO_I2S_OUT_SLCT) && PinUsed(GPIO_I2S_OUT_DATA))
    out->SetPinout(Pin(GPIO_I2S_OUT_CLK), Pin(GPIO_I2S_OUT_SLCT), Pin(GPIO_I2S_OUT_DATA));
  #endif  // USE_I2S_NO_DAC
#else
  #ifdef USE_I2S_NO_DAC
  out = new AudioOutputI2SNoDAC();
  #else
  out = new AudioOutputI2S(0, 1);    // Internal DAC port 0
  #endif  // USE_I2S_NO_DAC
#endif  // USE_I2S_EXTERNAL_DAC

  is2_volume=10;
  out->SetGain(((float)is2_volume/256.0)*4.0);
  out->stop();

#ifdef ESP32
  if (UsePSRAM()) {
    preallocateBuffer = heap_caps_malloc(preallocateBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    preallocateCodec = heap_caps_malloc(preallocateCodecSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  } else {
    preallocateBuffer = malloc(preallocateBufferSize);
    preallocateCodec = malloc(preallocateCodecSize);
  }
#endif  // ESP32
#ifdef ESP8266
  preallocateBuffer = malloc(preallocateBufferSize);
  preallocateCodec = malloc(preallocateCodecSize);
#endif  // ESP8266

  if (!preallocateBuffer || !preallocateCodec) {
    AddLog(LOG_LEVEL_INFO, PSTR("FATAL ERROR:  Unable to preallocate %d bytes"), preallocateBufferSize + preallocateCodecSize);
  }
}


#ifdef ESP32
TaskHandle_t mp3_task_h;
bool playing;

void mp3_task(void* arg) {
  while (playing) {
    if (!play()) {
      break;
    }
    delay(1);
  }
  if (mp3 && mp3->isRunning()) // it is not the ended
    out->flush();

  vTaskDelete(mp3_task_h);
  mp3_task_h = nullptr;
}
#endif  // ESP32

const int lowBufferSize = 2 * 1024;
bool filling = false;

bool play() {
  if (mp3 && mp3->isRunning()) {
    AddLog(LOG_LEVEL_DEBUG_MORE, PSTR("buffer level: %d, size: %d, pos: %d"), buff->getFillLevel(), buff->getSize(), buff->getPos());

    pos = 100 * buff->getPos() / buff->getSize();
    bufLvl = 100 * buff->getFillLevel() / preallocateBufferSize;

    if (buff->getFillLevel() < lowBufferSize && buff->getSize() > buff->getPos()) // low buffer, but it is not the end
      filling = true;
    if (buff->getFillLevel() > 5 * lowBufferSize) // if it's filled enough
      filling = false;
    if (filling) {
      // try to catch up filling the buffer
      AddLog(LOG_LEVEL_DEBUG, PSTR("Low stream buffer level: %d/%d"), buff->getFillLevel(), preallocateBufferSize);
      buff->loop();
    }

    if (paused || filling) { // paused or filling the buffer
      // flush the output to stop the glitch noise
      out->flush();
    }
    else if (!mp3->loop()) {
      AddLog(LOG_LEVEL_INFO, PSTR("Stream end"));
      StopPlaying();
    }
    return true;
  }
  return false;
}


void MDCallback(void *cbData, const char *type, bool isUnicode, const char *str) {
  const char *ptr = reinterpret_cast<const char *>(cbData);
  (void) isUnicode; // Punt this ball for now
  (void) ptr;
  if (strstr_P(type, PSTR("Title"))) {
    strncpy(title, str, sizeof(title));
    title[sizeof(title)-1] = 0;
    AddLog(LOG_LEVEL_INFO, PSTR("Title: %s"), title);
  }

  char s1[32], s2[64];
  strncpy_P(s1, type, sizeof(s1));
  s1[sizeof(s1) - 1] = 0;
  strncpy_P(s2, str, sizeof(s2));
  s2[sizeof(s2) - 1] = 0;
  AddLog(LOG_LEVEL_DEBUG, PSTR("METADATA '%s' = '%s'"), s1, s2);
}

void StatusCallback(void *cbData, int code, const char *string) {
  const char *ptr = reinterpret_cast<const char *>(cbData);
  (void) code;
  (void) ptr;
  char s1[64];
  strncpy_P(s1, string, sizeof(s1));
  s1[sizeof(s1) - 1] = 0;
  AddLog(LOG_LEVEL_DEBUG, PSTR("STATUS '%d' = '%s'"), code, s1);
}

void PlayStream(const char *url) {
  if (mp3) return;
  if (!out) return;
  file = new AudioFileSourceHTTPStream(url);
  file->RegisterMetadataCB(MDCallback, NULL);
  buff = new AudioFileSourceBuffer(file, preallocateBuffer, preallocateBufferSize);
  buff->RegisterStatusCB(StatusCallback, NULL);

  mp3 = new AudioGeneratorMP3(preallocateCodec, preallocateCodecSize);
  mp3->RegisterStatusCB(StatusCallback, NULL);
  mp3->begin(buff, out);

  if (!mp3->isRunning()) {
    AddLog(LOG_LEVEL_INFO, PSTR("Can't connect to URL: %s"), url);
    StopPlaying();
  }
  else {
    AddLog(LOG_LEVEL_DEBUG, PSTR("Play stream: %s"), url);
    strncpy(title, url, sizeof(title));
    title[sizeof(title) - 1] = 0;
    paused = false;

    Response_P(PSTR("{\"Play\":\"%s\",\"Paused\":false,\"Position\":0,\"BufferLevel\":0}"), url);
    MqttPublishPrefixTopic_P(STAT, "I2S");
    ResponseClear();
	
#ifdef ESP32
    playing = true;
    xTaskCreatePinnedToCore(mp3_task, "MP3", 8192, NULL, 3, &mp3_task_h, 1);
#endif // ESP32
  }
}

void StopPlaying() {
  AddLog(LOG_LEVEL_DEBUG, PSTR("Stop stream"));

#ifdef ESP32
  playing = false;
  delay(100);
#endif // ESP32

  if (mp3) {
    mp3->stop();
    delete mp3;
    mp3 = nullptr;
  }
  if (buff) {
    buff->close();
    delete buff;
    buff = nullptr;
  }
  if (file) {
    file->close();
    delete file;
    file = nullptr;
  }

  Response_P(PSTR("{\"Play\":null}"));
  MqttPublishPrefixTopic_P(STAT, "I2S");
  ResponseClear();
}

#ifdef USE_WEBSERVER
const char HTTP_AUDIO_STREAM_TITLE[] PROGMEM =
   "{s}" "I2S-Title" "{m}%s{e}";
const char HTTP_AUDIO_STREAM_PAUSED[] PROGMEM =
   "{s}" "I2S-Paused" "{m}%s{e}";
const char HTTP_AUDIO_STREAM_POS[] PROGMEM =
   "{s}" "I2S-Position" "{m}%d{e}";
const char HTTP_AUDIO_STREAM_BUF_LVL[] PROGMEM =
   "{s}" "I2S-Buffer-Level" "{m}%d{e}";

void I2S_Show(void) {
  if (mp3 && mp3->isRunning()) {
    WSContentSend_PD(HTTP_AUDIO_STREAM_TITLE, title);
    WSContentSend_PD(HTTP_AUDIO_STREAM_PAUSED, paused ? "true" : "false");
    WSContentSend_PD(HTTP_AUDIO_STREAM_POS, pos);
    WSContentSend_PD(HTTP_AUDIO_STREAM_BUF_LVL, bufLvl);
  }
}
#endif  // USE_WEBSERVER

unsigned long updateTimer = millis();
void I2S_Update(void) {
  // if playing send MQTT update every 5 seconds
  if (millis() - updateTimer > 5000 && mp3 && mp3->isRunning()) {
    updateTimer = millis();
    Response_P(PSTR("{\"Position\":%d,\"BufferLevel\":%d}"), pos, bufLvl);
    MqttPublishPrefixTopic_P(STAT, "I2S");
    ResponseClear();
  }
}


const char kI2SAudio_Commands[] PROGMEM = "I2S|Gain|Play|Pause";

void (* const I2SAudio_Command[])(void) PROGMEM = { &Cmd_Gain, &Cmd_Play, &Cmd_Pause };


void Cmd_Play(void) {
  if (mp3) {
    StopPlaying();
  }
  if (XdrvMailbox.data_len > 0) {
    PlayStream(XdrvMailbox.data);
    ResponseCmndChar(XdrvMailbox.data);
  }
  else {
    ResponseCmndChar_P(PSTR("Stopped"));
  }
}

void Cmd_Pause(void) {
    if (XdrvMailbox.payload) {
      paused = true;
      ResponseCmndChar_P(PSTR("Pause"));
    }
    else {
      paused = false;
      ResponseCmndChar_P(PSTR("Play"));
    }

    Response_P(PSTR("{\"Paused\":%s}"), paused ? "true" : "false");
    MqttPublishPrefixTopic_P(STAT, "I2S");
    ResponseClear();
}

void Cmd_Gain(void) {
  if ((XdrvMailbox.payload >= 0) && (XdrvMailbox.payload <= 100)) {
    if (out) {
      is2_volume=XdrvMailbox.payload;
      out->SetGain(((float)(is2_volume-2)/256.0)*4.0);
    }
  }
  ResponseCmndNumber(is2_volume);
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xdrv42(uint8_t function) {
  bool result = false;

  switch (function) {
    case FUNC_COMMAND:
      result = DecodeCommand(kI2SAudio_Commands, I2SAudio_Command);
      break;
    case FUNC_INIT:
      I2S_Init();
      break;
    case FUNC_EVERY_SECOND:
      I2S_Update();
      break;
#ifdef ESP8266
    case FUNC_EVERY_50_MSECOND:
        play();
      break;
#endif  // ESP8266
#ifdef USE_WEBSERVER
    case FUNC_WEB_SENSOR:
      I2S_Show();
      break;
#endif  // USE_WEBSERVER
  }
  return result;
}

#endif //  USE_I2S_AUDIO_STREAM
