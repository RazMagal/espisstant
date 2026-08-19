cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espisstant)

# Flash the contents of spiffs_data/ (Breeze IR remote set etc.) to the
# storage partition together with the app.
spiffs_create_partition_image(storage spiffs_data FLASH_IN_PROJECT)
