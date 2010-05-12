# Use hardware GPS implementation if available.
#
ifneq ($(BOARD_GPS_LIBRARIES),)
  LOCAL_CFLAGS           += -DHAVE_GPS_HARDWARE
  LOCAL_SHARED_LIBRARIES += $(BOARD_GPS_LIBRARIES)
endif

# Use emulator GPS implementation if QEMU_HARDWARE is set.
#
USE_QEMU_GPS_HARDWARE := $(QEMU_HARDWARE)

ifeq ($(USE_QEMU_GPS_HARDWARE),true)
    LOCAL_CFLAGS    += -DHAVE_QEMU_GPS_HARDWARE
    LOCAL_SRC_FILES += gps/gps_qemu.c
endif

LOCAL_C_INCLUDES += \
	$(TARGET_OUT_HEADERS)/librpc

LOCAL_SRC_FILES += gps/gps.cpp
LOCAL_SRC_FILES += gps/gps_msm7k.c
LOCAL_SRC_FILES += gps/gps-rpc.c
LOCAL_SHARED_LIBRARIES += librpc

