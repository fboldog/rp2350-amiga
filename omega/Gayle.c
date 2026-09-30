//
//  Gayle.c
//  Omega
//
//  Created by Matt Parsons on 06/03/2019.
//  Copyright © 2019 Matt Parsons. All rights reserved.
//

#include "Gayle.h"

#define CLOCKBASE 0xDC0000


void writeGayleB(unsigned int address, unsigned int value){
    (void)address;
    (void)value;
}

void writeGayle(unsigned int address, unsigned int value){
    (void)address;
    (void)value;
}

void writeGayleL(unsigned int address, unsigned int value){
    (void)address;
    (void)value;
}

uint8_t readGayleB(unsigned int address){
    (void)address;
    return 0xFF;
}

uint16_t readGayle(unsigned int address){
    (void)address;
    return 0x8000;
}

uint32_t readGayleL(unsigned int address){
    
    static int count = 0;
    
    if(address>=CLOCKBASE){
        if(count==0){
            count +=1;
            return 1309036038;
        }
        
        return 1309101575;
        
        
    }

    return 0x8000;
}
