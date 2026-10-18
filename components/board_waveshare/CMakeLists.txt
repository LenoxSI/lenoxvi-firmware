idf_component_register(
    SRCS "board_audio.c" "board_led.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_i2s esp_codec_dev esp_io_expander_tca95xx_16bit led_strip log freertos)
