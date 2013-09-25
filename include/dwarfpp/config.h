//
//  config.h
//  libdwarfpp
//
//  Created by Ant on 25/09/2013.
//  Copyright (c) 2013 dervishsoftware. All rights reserved.
//

#ifndef libdwarfpp_config_h
#define libdwarfpp_config_h

#if defined(BSD) || (defined(__APPLE__) && defined(__MACH__))
#include <stdio.h>
#else
#include <fileno.hpp>
#endif

#endif
