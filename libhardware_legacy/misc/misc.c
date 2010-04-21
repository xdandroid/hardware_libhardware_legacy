/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "hardware_legacy/misc.h"


#define JOGBALL "/sys/class/leds/jogball-backlight/brightness"

int set_jogball_mode(int mode)
{
    int fd,nwr;
    int ret;
    char value[20];
	
    fd = open(JOGBALL, O_RDWR);
    if(fd <0) return errno;
    nwr = sprintf(value,"%d\n",0);
    ret = write (fd,value, nwr);
    close(fd);
    return 0;
}

int getUsbRndis()
{
    return 0;
}
int setUsbRndis()
{
    return 0;
}
int setNetSharingInterface()
{
    return 0;
}

int enableEthernetSharing()
{
    return 0;
}

int set_dual_led()
{
    return 0;
}
int set_caps_fn_led()
{
    return 0;
}
int getEthernetSharingStatus()
{
    return 0;
}
