#include <hardware_legacy/gps.h>
#include <cutils/properties.h>

#define LOG_TAG "libhardware_legacy"
#include <utils/Log.h>
#include "qemu.h"

static const GpsInterface*  sGpsInterface = NULL;

static void
gps_find_hardware( void )
{
    sGpsInterface = gps_get_hardware_interface();
    if (!sGpsInterface)
        LOGD("no GPS hardware on this device\n");
}

const GpsInterface*
gps_get_interface()
{
    if (sGpsInterface == NULL)
         gps_find_hardware();

    return sGpsInterface;
}
