/*
 *  libdwarfpp.cp
 *  libdwarfpp
 *
 *  Created by Ant on 25/09/2013.
 *  Copyright (c) 2013 dervishsoftware. All rights reserved.
 *
 */

#include <iostream>
#include "libdwarfpp.h"
#include "libdwarfppPriv.h"

void libdwarfpp::HelloWorld(const char * s)
{
	 libdwarfppPriv *theObj = new libdwarfppPriv;
	 theObj->HelloWorldPriv(s);
	 delete theObj;
};

void libdwarfppPriv::HelloWorldPriv(const char * s) 
{
	std::cout << s << std::endl;
};

