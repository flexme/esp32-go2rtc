cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_go2rtc)
# GCC 15 / IDF 6 defines uint32_t as unsigned long on Xtensa; the older
# registry SRTP component mixes it with unsigned int (both 32-bit). Isolate
# compatibility flags to that dependency, retaining all application warnings.
idf_component_get_property(SRTP_LIBRARY sepfy__srtp COMPONENT_LIB)
target_compile_options(${SRTP_LIBRARY} PRIVATE -Wno-error=incompatible-pointer-types -fno-strict-aliasing)
