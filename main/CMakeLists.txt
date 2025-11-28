idf_component_register(
    SRCS
        "app_main.cpp" 
        "libs/display/blink.cpp" 
   
    INCLUDE_DIRS
        "." 
        "libs" 
    REQUIRES
        nvs_flash 
        driver 
        esp_lcd
        button
    )
