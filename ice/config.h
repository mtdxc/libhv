#ifndef ICE_CONFIG_H
#define ICE_CONFIG_H

#include "mini.h"

#define GET_CONFIG(type, arg, key)                  \
    type arg = mINI::Instance()[key];  

#define GET_CONFIG_FUNC(type, arg, key, func)       \
    type arg = func(mINI::Instance()[key]);  
#endif