#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "epaper_driver_bsp.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "esp_heap_caps.h" 

static const char *TAG = "driver";

const uint8_t WF_Full_1IN54[159] =
{											
    0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x80,	0x48,	0x40,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x40,	0x48,	0x80,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,
    0xA,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x8,	0x1,	0x0,	0x8,	0x1,	0x0,	0x2,					
    0xA,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x0,	0x0,	0x0,	0x0,	0x0,	0x0,	0x0,					
    0x22,	0x22,	0x22,	0x22,	0x22,	0x22,	0x0,	0x0,	0x0,			
    0x22,	0x17,	0x41,	0x0,	0x32,	0x20
};

unsigned char WF_PARTIAL_1IN54_0[159] =
{
    0x0,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x80,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x40,0x40,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0xF,0x0,0x0,0x0,0x0,0x0,0x0,
    0x1,0x1,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x0,0x0,0x0,0x0,0x0,0x0,0x0,
    0x22,0x22,0x22,0x22,0x22,0x22,0x0,0x0,0x0,
    0x02,0x17,0x41,0xB0,0x32,0x28,
};

epaper_driver_display::epaper_driver_display(int width, int height,custom_lcd_spi_t _lcd_spi_data) : 
    lcd_spi_data(_lcd_spi_data),
    Width(width),
    Height(height) {

    ESP_LOGI(TAG, "Initialize SPI");
	spi_port_init();
	spi_gpio_init();

    buffer = (uint8_t *)heap_caps_malloc(lcd_spi_data.buffer_len, MALLOC_CAP_SPIRAM);
	assert(buffer);
}

epaper_driver_display::~epaper_driver_display() {

}

void epaper_driver_display::spi_gpio_init() {
    int rst = lcd_spi_data.rst;
    int cs = lcd_spi_data.cs;
    int dc = lcd_spi_data.dc;
    int busy = lcd_spi_data.busy;

    gpio_config_t gpio_conf = {};
	gpio_conf.intr_type = GPIO_INTR_DISABLE;
	gpio_conf.mode = GPIO_MODE_OUTPUT;
	gpio_conf.pin_bit_mask = (0x1ULL<<rst) | (0x1ULL<<dc) | (0x1ULL<<cs);
	gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

	gpio_conf.mode = GPIO_MODE_INPUT;
	gpio_conf.pin_bit_mask = (0x1ULL<<busy);
	ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

    set_rst_1();
}

void epaper_driver_display::spi_port_init() {
    int mosi = lcd_spi_data.mosi;
    int scl = lcd_spi_data.scl;
    int spi_host = lcd_spi_data.spi_host;
    esp_err_t ret;
  	spi_bus_config_t buscfg = {};
  	  	buscfg.miso_io_num = -1;
  	  	buscfg.mosi_io_num = mosi;
  	  	buscfg.sclk_io_num = scl;
  	  	buscfg.quadwp_io_num = -1;
  	  	buscfg.quadhd_io_num = -1;
  	  	buscfg.max_transfer_sz = Width * Height;

  	spi_device_interface_config_t devcfg = {};
  	  	devcfg.spics_io_num = -1;
  	  	devcfg.clock_speed_hz = 40 * 1000 * 1000;  //Clock out at 10 MHz
  	  	devcfg.mode = 0;                           //SPI mode 0
  	  	devcfg.queue_size = 7;                     //We want to be able to queue 7 transactions at a time

  	ret = spi_bus_initialize((spi_host_device_t)spi_host, &buscfg, SPI_DMA_CH_AUTO);
  	ESP_ERROR_CHECK(ret);
  	ret = spi_bus_add_device((spi_host_device_t)spi_host, &devcfg, &spi);
  	ESP_ERROR_CHECK(ret);
}

/* A panel that never answers must not take the device with it.

   This used to spin forever while BUSY stayed high. EPD_Init calls it before
   anything else in setup(), so an unresponsive panel meant setup() never
   returned: no UI, no button handling, no crash and no reboot - just the last
   image frozen on the glass, which on e-paper persists with no power at all.
   From the outside that is indistinguishable from software that ignores every
   button, and it is exactly what happened.

   Five seconds is far longer than any real refresh - a full update is about
   2.7s - so a timeout here means the panel is genuinely not talking. */
bool epaper_driver_display::read_busy() {
    int busy = lcd_spi_data.busy;
    const int64_t start = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)busy) == 1) {
        if (esp_timer_get_time() - start > 5000000) return false;   /* gave up */
        vTaskDelay(pdMS_TO_TICKS(5));   //LOW: idle, HIGH: busy
    }
    return true;
}

void epaper_driver_display::SPI_SendByte(uint8_t data) {
    esp_err_t ret;
  	spi_transaction_t t; 
  	memset(&t, 0, sizeof(t));
  	t.length = 8;      
  	t.tx_buffer = &data;
  	ret = spi_device_polling_transmit(spi, &t); //Transmit!
  	assert(ret == ESP_OK);                      //Should have had no issues.
}

void epaper_driver_display::EPD_SendData(uint8_t data) {
    set_dc_1();
  	set_cs_0();
  	SPI_SendByte(data);
  	set_cs_1();
}

void epaper_driver_display::EPD_SendCommand(uint8_t command) {
    set_dc_0();
  	set_cs_0();
  	SPI_SendByte(command);
  	set_cs_1();
}

void epaper_driver_display::writeBytes(uint8_t *buffer,int len) {
    set_dc_1();
  	set_cs_0();
  	esp_err_t ret;
  	spi_transaction_t t; 
  	memset(&t, 0, sizeof(t));
  	t.length = 8 * len;      
  	t.tx_buffer = buffer;
  	ret = spi_device_polling_transmit(spi, &t); //Transmit!
  	assert(ret == ESP_OK);
  	set_cs_1();
}

void epaper_driver_display::writeBytes(const uint8_t *buffer, int len) {
    set_dc_1();
  	set_cs_0();
  	esp_err_t ret;
  	spi_transaction_t t; 
  	memset(&t, 0, sizeof(t));
  	t.length = 8 * len;      
  	t.tx_buffer = buffer;
  	ret = spi_device_polling_transmit(spi, &t); //Transmit!
  	assert(ret == ESP_OK);
  	set_cs_1();
}

void epaper_driver_display::EPD_SetWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend)
{
    EPD_SendCommand(0x44);  // SET_RAM_X_ADDRESS_START_END_POSITION
    EPD_SendData((Xstart>>3) & 0xFF);
    EPD_SendData((Xend>>3) & 0xFF);
	
    EPD_SendCommand(0x45);  // SET_RAM_Y_ADDRESS_START_END_POSITION
    EPD_SendData(Ystart & 0xFF);
    EPD_SendData((Ystart >> 8) & 0xFF);
    EPD_SendData(Yend & 0xFF);
    EPD_SendData((Yend >> 8) & 0xFF);
}

void epaper_driver_display::EPD_SetCursor(uint16_t Xstart, uint16_t Ystart)
{
    EPD_SendCommand(0x4E); // SET_RAM_X_ADDRESS_COUNTER
    EPD_SendData(Xstart & 0xFF);

    EPD_SendCommand(0x4F); // SET_RAM_Y_ADDRESS_COUNTER
    EPD_SendData(Ystart & 0xFF);
    EPD_SendData((Ystart >> 8) & 0xFF);
}

void epaper_driver_display::EPD_SetLut(const uint8_t *lut) {
	EPD_SendCommand(0x32);
    writeBytes(lut,153);
	read_busy();
	
    EPD_SendCommand(0x3f);
    EPD_SendData(lut[153]);
	
    EPD_SendCommand(0x03);
    EPD_SendData(lut[154]);
	
    EPD_SendCommand(0x04);
    EPD_SendData(lut[155]);
	EPD_SendData(lut[156]);
	EPD_SendData(lut[157]);

	EPD_SendCommand(0x2c);
    EPD_SendData(lut[158]);
}

void epaper_driver_display::EPD_TurnOnDisplay() {
    EPD_SendCommand(0x22);
    EPD_SendData(0xc7);
	EPD_SendCommand(0x20);
    read_busy();
}

void epaper_driver_display::EPD_TurnOnDisplayPart() {
    EPD_SendCommand(0x22);
    EPD_SendData(0xcf);
    EPD_SendCommand(0x20);
    read_busy();
}

bool epaper_driver_display::panelResponded() const { return panel_ok; }

void epaper_driver_display::EPD_Init() {
    panel_ok = true;
    set_rst_1();
  	vTaskDelay(pdMS_TO_TICKS(50));
  	set_rst_0();
  	vTaskDelay(pdMS_TO_TICKS(20));
  	set_rst_1();
  	vTaskDelay(pdMS_TO_TICKS(50));

    if (!read_busy()) panel_ok = false;
    EPD_SendCommand(0x12);  //SWRESET
    if (!read_busy()) panel_ok = false;

    EPD_SendCommand(0x01); //Driver output control
    EPD_SendData(0xC7);
    EPD_SendData(0x00);
    EPD_SendData(0x01);

    EPD_SendCommand(0x11); //data entry mode
    EPD_SendData(0x01);

	EPD_SetWindows(0, Width-1, Height-1, 0);

    EPD_SendCommand(0x3C); //BorderWavefrom
    EPD_SendData(0x01);

    EPD_SendCommand(0x18);
    EPD_SendData(0x80);

    EPD_SendCommand(0x22); //Load Temperature and waveform setting.
    EPD_SendData(0XB1);
    EPD_SendCommand(0x20);

    EPD_SetCursor(0, Height-1);
	read_busy();
	
	EPD_SetLut(WF_Full_1IN54);
}

void epaper_driver_display::EPD_Clear() {
    int buffer_len = lcd_spi_data.buffer_len;
    memset(buffer,0xff,buffer_len);
}

void epaper_driver_display::EPD_Display() {
    int buffer_len = lcd_spi_data.buffer_len;
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer,buffer_len);
    EPD_TurnOnDisplay();
}

void epaper_driver_display::EPD_DisplayPartBaseImage() {
    int buffer_len = lcd_spi_data.buffer_len;
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer,buffer_len);
    EPD_SendCommand(0x26);
    writeBytes(buffer,buffer_len);
    EPD_TurnOnDisplay();
}

void epaper_driver_display::EPD_Init_Partial() {
    set_rst_1();
  	vTaskDelay(pdMS_TO_TICKS(50));
  	set_rst_0();
  	vTaskDelay(pdMS_TO_TICKS(20));
  	set_rst_1();
  	vTaskDelay(pdMS_TO_TICKS(50));

	read_busy();
	
	EPD_SetLut(WF_PARTIAL_1IN54_0);

    EPD_SendCommand(0x37); 
    EPD_SendData(0x00);  
    EPD_SendData(0x00);  
    EPD_SendData(0x00);  
    EPD_SendData(0x00); 
    EPD_SendData(0x00);  	
    EPD_SendData(0x40);  
    EPD_SendData(0x00);  
    EPD_SendData(0x00);   
    EPD_SendData(0x00);  
    EPD_SendData(0x00);
	
    EPD_SendCommand(0x3C); //BorderWavefrom
    EPD_SendData(0x80);
	
	EPD_SendCommand(0x22); 
	EPD_SendData(0xc0); 
	EPD_SendCommand(0x20); 
	read_busy();
}

/* The full-frame case sets the window to (0, H-1, W-1, 0) with the cursor at
   (0, H-1) and data entry decrementing Y, so buffer row r lands on panel row
   H-1-r. Everything here is that same mapping, narrowed - derived from the
   working full-frame path rather than from the datasheet, so it cannot drift
   away from whatever convention this panel actually uses. */
void epaper_driver_display::EPD_DisplayPartRows(int r0, int r1) {
    if (r0 < 0) r0 = 0;
    if (r1 > Height - 1) r1 = Height - 1;
    if (r1 < r0) return;

    EPD_SetWindows(0, Height - 1 - r0, Width - 1, Height - 1 - r1);
    EPD_SetCursor(0, Height - 1 - r0);
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer + r0 * 25, (r1 - r0 + 1) * 25);
    EPD_TurnOnDisplayPart();

    /* Leave the window as the rest of the driver expects to find it. */
    EPD_SetWindows(0, Height - 1, Width - 1, 0);
    EPD_SetCursor(0, Height - 1);
}

void epaper_driver_display::EPD_DisplayPart() {
    EPD_SendCommand(0x24);
    assert(buffer);
    writeBytes(buffer,5000);
    EPD_TurnOnDisplayPart();
}

void epaper_driver_display::EPD_DrawColorPixel(uint16_t x, uint16_t y,uint8_t color) {
    if (x >= Width || y >= Height)
    {
        ESP_LOGE("EPD", "Out of bounds pixel: (%d,%d)", x, y);
        return; 
    }

    uint16_t index = y * 25 + (x >> 3); //25是200/8
    uint8_t bit = 7 - (x & 0x07);
    if(color == DRIVER_COLOR_WHITE)
    {
        buffer[index] |= (0x01 << bit);
    }
    else
    {
        buffer[index] &= ~(0x01 << bit);
    }
}
