idf_component_register(SRCS "LED.cpp" "Robot.cpp" "main.cpp" INCLUDE_DIRS ".")

spiffs_create_partition_image(storage ../filesystem FLASH_IN_PROJECT)

