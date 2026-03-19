# get IDF version for comparison
set(idf_version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")

set(priv_requires "")

# set conversion sources
set(srcs
  conversions/yuv.c
  conversions/to_jpg.cpp
  conversions/to_bmp.c
  conversions/jpge.cpp
  )

set(priv_include_dirs
  conversions/private_include
  )

set(include_dirs
  driver/include
  conversions/include
  )

# set driver sources only for supported platforms
if(IDF_TARGET STREQUAL "esp32" OR IDF_TARGET STREQUAL "esp32s2" OR IDF_TARGET STREQUAL "esp32s3")
  list(APPEND srcs
    driver/esp_camera.c
    driver/esp_camera_af.c
    driver/cam_hal.c
    driver/sensor.c
    )

  if(CONFIG_OV2640_SUPPORT)
    list(APPEND srcs sensors/ov2640.c)
  endif()

  if(CONFIG_OV3660_SUPPORT)
    list(APPEND srcs sensors/ov3660.c)
  endif()

  if(CONFIG_OV5640_SUPPORT)
    list(APPEND srcs sensors/ov5640.c)
    if(CONFIG_CAMERA_AF_SUPPORT)
      list(APPEND srcs sensors/ov5640_af.c)
    endif()
  endif()

  if(CONFIG_OV7725_SUPPORT)
    list(APPEND srcs sensors/ov7725.c)
  endif()

  if(CONFIG_OV7670_SUPPORT)
    list(APPEND srcs sensors/ov7670.c)
  endif()

  if(CONFIG_NT99141_SUPPORT)
    list(APPEND srcs sensors/nt99141.c)
  endif()

  if(CONFIG_GC0308_SUPPORT)
    list(APPEND srcs sensors/gc0308.c)
  endif()

  if(CONFIG_GC2145_SUPPORT)
    list(APPEND srcs sensors/gc2145.c)
  endif()

  if(CONFIG_GC032A_SUPPORT)
    list(APPEND srcs sensors/gc032a.c)
  endif()

  if(CONFIG_BF3005_SUPPORT)
    list(APPEND srcs sensors/bf3005.c)
  endif()

  if(CONFIG_BF20A6_SUPPORT)
    list(APPEND srcs sensors/bf20a6.c)
  endif()

  if(CONFIG_SC101IOT_SUPPORT)
    list(APPEND srcs sensors/sc101iot.c)
  endif()

  if(CONFIG_SC030IOT_SUPPORT)
    list(APPEND srcs sensors/sc030iot.c)
  endif()

  if(CONFIG_SC031GS_SUPPORT)
    list(APPEND srcs sensors/sc031gs.c)
  endif()

  if(CONFIG_MEGA_CCM_SUPPORT)
    list(APPEND srcs sensors/mega_ccm.c)
  endif()

  if(CONFIG_HM1055_SUPPORT)
    list(APPEND srcs sensors/hm1055.c)
  endif()

  if(CONFIG_HM0360_SUPPORT)
    list(APPEND srcs sensors/hm0360.c)
  endif()

  list(APPEND priv_include_dirs
    driver/private_include
    sensors/private_include
    target/private_include
    )

  if(IDF_TARGET STREQUAL "esp32")
    list(APPEND srcs
      target/xclk.c
      target/esp32/ll_cam.c
      )
  endif()

  if(IDF_TARGET STREQUAL "esp32s2")
    list(APPEND srcs
      target/xclk.c
      target/esp32s2/ll_cam.c
      )

    list(APPEND priv_include_dirs
      target/esp32s2/private_include
      )
  endif()

  if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND srcs
      target/esp32s3/ll_cam.c
      )
  endif()

  list(APPEND priv_requires freertos nvs_flash esp_mm)

  set(min_version_for_esp_timer "4.2")
  if (idf_version VERSION_GREATER_EQUAL min_version_for_esp_timer)
    list(APPEND priv_requires esp_timer)
  endif()

  # include the SCCB I2C driver
  # this uses either the legacy I2C API or the newer version from IDF v5.4
  # as this features a method to obtain the I2C driver from a port number
  if (idf_version VERSION_GREATER_EQUAL "5.4" AND NOT CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY)
    list(APPEND srcs driver/sccb-ng.c)
  else()
    list(APPEND srcs driver/sccb.c)
  endif()

endif()

set(req driver)
if (idf_version VERSION_GREATER_EQUAL "6.0")
  list(APPEND priv_requires esp_driver_gpio esp_driver_spi esp_driver_i2c)
  list(APPEND req esp_driver_ledc)
endif()
if (idf_version VERSION_GREATER_EQUAL "6.1")
  list(APPEND priv_requires esp_hal_clock)
endif()

idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  PRIV_INCLUDE_DIRS ${priv_include_dirs}
  REQUIRES ${req}
  PRIV_REQUIRES ${priv_requires}
)
