#include <M5Unified.h>
#include <M5GFX.h>
#include <driver/i2s.h>

#include "fft.h"

// Poat A
#if defined(ARDUINO_M5STACK_Grey)
#define PIN_CLK 22
#define PIN_DATA 21
#elif defined(ARDUINO_M5STACK_Core2)
#define PIN_CLK 33
#define PIN_DATA 32
#elif defined(ARDUINO_M5STACK_CoreS3)
#define PIN_CLK 1
#define PIN_DATA 2
#endif

#define MODE_MIC 0

M5GFX DisFFTbuff;
M5Canvas canvas(&DisFFTbuff);

static QueueHandle_t fftvalueQueue = nullptr;
static QueueHandle_t i2sstateQueue = nullptr;

typedef struct
{
    uint8_t state;
    void *audioPtr;
    uint32_t audioSize;
} i2sQueueMsg_t;

bool InitI2SSpakerOrMic(int mode)
{
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER), // Set the I2S operating mode.
        .sample_rate = 44100,
        .bits_per_sample =
            I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format =
            I2S_CHANNEL_FMT_ONLY_RIGHT,
#if ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(4, 1, 0)
        .communication_format =
            I2S_COMM_FORMAT_STAND_I2S, // Set the format of the communication.
#else
        .communication_format = I2S_COMM_FORMAT_I2S,
#endif
        .intr_alloc_flags =
            ESP_INTR_FLAG_LEVEL1, // Set the interrupt flag.
        .dma_buf_count = 2,       // DMA buffer count.
        .dma_buf_len = 128,       // DMA buffer length.
    };
    if (mode == MODE_MIC)
    {
        i2s_config.mode =
            (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
    }

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_pin_config_t pin_config;
#if (ESP_IDF_VERSION > ESP_IDF_VERSION_VAL(4, 3, 0))
    pin_config.mck_io_num = I2S_PIN_NO_CHANGE;
#endif
    pin_config.bck_io_num = I2S_PIN_NO_CHANGE;
    pin_config.ws_io_num = PIN_CLK;
    pin_config.data_out_num = I2S_PIN_NO_CHANGE;
    pin_config.data_in_num = PIN_DATA;

    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_set_clk(I2S_NUM_0, 44100, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

    return true;
}

static void i2sMicroFFTtask(void *arg)
{
    uint8_t FFTDataBuff[128];
    uint8_t FFTValueBuff[24];
    uint8_t *microRawData = (uint8_t *)calloc(2048, sizeof(uint8_t));
    size_t bytesread;
    int16_t *buffptr;
    double data = 0;
    float adc_data;
    uint16_t ydata;
    uint32_t subData;

    uint8_t state = MODE_MIC;
    i2sQueueMsg_t QueueMsg;
    while (1)
    {
        if (xQueueReceive(i2sstateQueue, &QueueMsg, (TickType_t)0) == pdTRUE)
        {
            // Serial.println("Queue Now");
            if (QueueMsg.state == MODE_MIC)
            {
                InitI2SSpakerOrMic(MODE_MIC);
                state = MODE_MIC;
            }
        }
        else if (state == MODE_MIC)
        {
            fft_config_t *real_fft_plan =
                fft_init(1024, FFT_REAL, FFT_FORWARD, NULL, NULL);
            i2s_read(I2S_NUM_0, (char *)microRawData, 2048, &bytesread,
                     (100 / portTICK_RATE_MS));
            buffptr = (int16_t *)microRawData;

            for (int count_n = 0; count_n < real_fft_plan->size; count_n++)
            {
                adc_data = (float)map(buffptr[count_n], INT16_MIN, INT16_MAX,
                                      -2000, 2000);
                real_fft_plan->input[count_n] = adc_data;
            }
            fft_execute(real_fft_plan);

            for (int count_n = 1; count_n < real_fft_plan->size / 4;
                 count_n++)
            {
                data = sqrt(real_fft_plan->output[2 * count_n] *
                                real_fft_plan->output[2 * count_n] +
                            real_fft_plan->output[2 * count_n + 1] *
                                real_fft_plan->output[2 * count_n + 1]);
                if ((count_n - 1) < 128)
                {
                    data = (data > 2000) ? 2000 : data;
                    ydata = map(data, 0, 2000, 0, 255);
                    FFTDataBuff[128 - count_n] = ydata;
                }
            }

            for (int count = 0; count < 24; count++)
            {
                subData = 0;
                for (int count_i = 0; count_i < 5; count_i++)
                {
                    subData += FFTDataBuff[count * 5 + count_i];
                }
                subData /= 5;
                FFTValueBuff[count] = map(subData, 0, 255, 0, 8);
            }
            xQueueSend(fftvalueQueue, (void *)&FFTValueBuff, 0);
            fft_destroy(real_fft_plan);
        }
        else
        {
            delay(10);
        }
    }
}

void microPhoneSetup()
{
    fftvalueQueue = xQueueCreate(5, 24 * sizeof(uint8_t));
    if (fftvalueQueue == 0)
    {
        return;
    }

    i2sstateQueue = xQueueCreate(5, sizeof(i2sQueueMsg_t));
    if (i2sstateQueue == 0)
    {
        return;
    }

    InitI2SSpakerOrMic(MODE_MIC);
    xTaskCreatePinnedToCore(i2sMicroFFTtask, "microPhoneTask", 4096, NULL, 3, NULL, 0);
}

void MicroPhoneFFT()
{
    uint8_t FFTValueBuff[24];
    xQueueReceive(fftvalueQueue, (void *)&FFTValueBuff, portMAX_DELAY);

    canvas.createSprite(320, 54);
    canvas.fillRect(0, 0, 320, 54, canvas.color565(0x00, 0x00, 0x00));
    uint32_t colorY = canvas.color565(0xff, 0x9c, 0x00);
    uint32_t colorG = canvas.color565(0x66, 0xff, 0x00);
    uint32_t colorRect;

    DisFFTbuff.startWrite();
    for (int x = 0; x < 24; x++)
    {
        for (int y = 0; y < 9; y++)
        {
            if (y < FFTValueBuff[23 - x])
            {
                colorRect = colorY;
            }
            else if (y == FFTValueBuff[23 - x])
            {
                colorRect = colorG;
            }
            else
            {
                continue;
            }
            canvas.fillRect(x * 12, 54 - y * 6 - 5, 5, 5, colorRect);
        }
    }
    canvas.pushSprite(20, 120);

    DisFFTbuff.endWrite();
}

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.In_I2C.release();

    DisFFTbuff.begin();

    DisFFTbuff.fillScreen(BLACK);
    DisFFTbuff.setTextSize(1);
    DisFFTbuff.setTextColor(WHITE, BLACK);
    DisFFTbuff.fillRect(0, 0, 320, 30, BLACK);
    DisFFTbuff.setTextDatum(TC_DATUM);
    DisFFTbuff.drawString("PDM Unit", 160, 3, &fonts::efontJA_16);

    microPhoneSetup();
}

void loop()
{
    M5.update();

    MicroPhoneFFT();
}
