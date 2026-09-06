//
//  DMA.c
//  The Omega Project
//  https://github.com/h5n1xp/Omega
//
//  Created by Matt Parsons on 02/02/2019.
//  Copyright © 2019 Matt Parsons. All rights reserved.
//  <h5n1xp@gmail.com>
//
//
//  This Source Code Form is subject to the terms of the
//  Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
//  with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "DMA.h"
#include "Memory.h"
#include "Chipset.h"
#include "CIA.h"
#include "Host.h"
#include "Blitter.h"

#include "CPU.h"


#include "debug.h"

#include "Floppy.h"
#include "VideoStandard.h"
#include "DisplayLayout.h"







































































void (*DMALores[])() = {
    evenCycle,
    dramCycle,
    evenCycle,
    dramCycle,
    evenCycle,
    dramCycle,
    evenCycle,
    diskCycle,
    evenCycle,
    diskCycle,
    evenCycle,
    diskCycle,
    evenCycle,
    audio0Cycle,
    evenCycle,
    audio1Cycle,
    evenCycle,
    audio2Cycle,
    evenCycle,
    audio3Cycle,
    evenCycle,
    spriteCycle,    //00
    evenCycle,
    spriteCycle,    //00
    evenCycle,
    spriteCycle,    //01
    evenCycle,
    spriteCycle,    //01
    evenCycle,
    spriteCycle,    //02
    evenCycle,
    spriteCycle,    //02
    evenCycle,
    spriteCycle,    //03
    evenCycle,
    spriteCycle,    //03
    evenCycle,
    spriteCycle,    //04
    evenCycle,
    spriteCycle,    //04
    evenCycle,
    spriteCycle,    //05
    evenCycle,
    spriteCycle,    //05
    evenCycle,
    spriteCycle,    //06
    evenCycle,
    spriteCycle,    //06
    evenCycle,
    spriteCycle,    //07
    evenCycle,
    spriteCycle,    //07
    evenCycle,
    oddCycle,
    evenCycle,
    oddCycle,
    evenCycle,   //Normal start
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    plane4,
    plane6,
    plane2,
    evenCycle,
    plane3,
    plane5,
    loresPlane1,
    evenCycle,
    oddCycle,
    evenCycle,
    oddCycle,
    evenCycle,
    oddCycle,
    evenCycle,
    oddCycle,
    evenCycle,
    oddCycle,

};


void (*DMAHires[]) (void) = {
    evenCycle,
    dramCycle,
    evenCycle,
    dramCycle,
    evenCycle,
    dramCycle,
    evenCycle,
    diskCycle,
    evenCycle,
    diskCycle,
    evenCycle,
    diskCycle,
    evenCycle,
    audio0Cycle,
    evenCycle,
    audio1Cycle,
    evenCycle,
    audio2Cycle,
    evenCycle,
    audio3Cycle,
    evenCycle,
    spriteCycle,//00
    evenCycle,
    spriteCycle,//00
    evenCycle,
    spriteCycle,//01
    evenCycle,
    spriteCycle,//01
    evenCycle,
    spriteCycle,//02
    evenCycle,
    spriteCycle,//02
    evenCycle,
    spriteCycle,//03
    evenCycle,
    spriteCycle,//03
    evenCycle,
    spriteCycle,//04
    evenCycle,
    spriteCycle,//04
    evenCycle,
    spriteCycle,//05
    evenCycle,
    spriteCycle,//05
    evenCycle,
    spriteCycle,//06
    evenCycle,
    spriteCycle,//06
    evenCycle,
    spriteCycle,//07
    evenCycle,
    spriteCycle,//07
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,    //Normal Plane Start
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    plane4,
    plane2,
    plane3,
    hiresPlane1,
    evenCycle,
    oddCycle,
    evenCycle,
    oddCycle,
    evenCycle,
    oddCycle,
    evenCycle,
    oddCycle,
    evenCycle,
    oddCycle,
};


void waitFreeSlot(){
#ifndef PICO_BUILD
    while(SDL_AtomicGet(&cpuWait)){
        //SDL_Delay(1);
    }
#endif
}

typedef struct {
    int lastCycle;
    int hiresWords;
    int loresWords;
    uint8_t enabledMask;
    uint8_t fetchEligibleMask;
    uint8_t fetchedMask;
    uint8_t fetchWindowComplete;
    uint32_t pointerAtFetchCompletion[4];
    uint8_t pointerHighWriteMask;
    uint8_t pointerLowWriteMask;
    uint8_t pointerReloadBeforeFetchMask;
} BitplaneLineState;

static BitplaneLineState bitplaneLine;
static uint8_t lastFetchedMask;
static uint32_t copperWaitPosition = 0;

static void resetBitplaneLine(void) {
    bitplaneLine.hiresWords = 0;
    bitplaneLine.loresWords = 0;
    bitplaneLine.enabledMask = 0;
    bitplaneLine.fetchEligibleMask = 0;
    bitplaneLine.fetchedMask = 0;
    bitplaneLine.fetchWindowComplete = 0;
    bitplaneLine.pointerHighWriteMask = 0;
    bitplaneLine.pointerLowWriteMask = 0;
    bitplaneLine.pointerReloadBeforeFetchMask = 0;
    for (unsigned plane = 0; plane < 4; plane++)
        bitplaneLine.pointerAtFetchCompletion[plane] = 0;
}

static void markBitplaneFetched(unsigned plane) {
    uint8_t planeMask = (uint8_t)(1u << (plane - 1));
    bitplaneLine.fetchedMask |= planeMask;
}

void dmaBitplanePointerWrite(unsigned plane, int highWord) {
    if (plane >= 1 && plane <= 8) {
        uint8_t planeMask = (uint8_t)(1u << (plane - 1));
        if (highWord)
            bitplaneLine.pointerHighWriteMask |= planeMask;
        else
            bitplaneLine.pointerLowWriteMask |= planeMask;
        if ((bitplaneLine.pointerHighWriteMask &
             bitplaneLine.pointerLowWriteMask & planeMask) != 0 &&
            internal.hPos < chipset.ddfstrt)
            bitplaneLine.pointerReloadBeforeFetchMask |= planeMask;
    }
}

static void applyBitplaneModulo(uint8_t planes) {
    if (planes & 0x01)
        chipset.bpl1pt += chipset.bpl1mod;
    if (planes & 0x02)
        chipset.bpl2pt += chipset.bpl2mod;
    if (planes & 0x04)
        chipset.bpl3pt += chipset.bpl1mod;
    if (planes & 0x08)
        chipset.bpl4pt += chipset.bpl2mod;
    if (planes & 0x10)
        chipset.bpl5pt += chipset.bpl1mod;
    if (planes & 0x20)
        chipset.bpl6pt += chipset.bpl2mod;
    if (planes & 0x40)
        chipset.bpl7pt += chipset.bpl1mod;
    if (planes & 0x80)
        chipset.bpl8pt += chipset.bpl2mod;
}

static void advanceBitplanePointers(void) {
    uint8_t fetched = bitplaneLine.fetchedMask;

    if (fetched != 0) {
        lastFetchedMask = fetched;
        return;
    }

    fetched = bitplaneLine.fetchEligibleMask != 0
        ? bitplaneLine.fetchEligibleMask
        : lastFetchedMask;
    fetched &= (uint8_t)~(bitplaneLine.pointerReloadBeforeFetchMask &
                          bitplaneLine.enabledMask);
    applyBitplaneModulo(fetched);
}

static uint8_t enabledBitplaneMask(void) {
    return (uint8_t)internal.bitplaneMask;
}

static int displayWindowContainsLine(int vpos) {
    int start = chipset.diwstrt >> 8;
    // On OCS the missing ninth comparator bits are fixed: VSTART8 is zero and
    // VSTOP8 is one. Thus the common DIWSTOP=$f4xx means line $1f4, not $0f4.
    // ECS/AGA DIWHIGH programmability is outside this OCS chipset model.
    int stop = 0x100 | (chipset.diwstop >> 8);
    return vpos >= start && vpos < stop;
}

static void hiresDisplayPrefetch(void) {
    if (!bitplaneLine.fetchWindowComplete ||
        (chipset.bplcon0 & 0x8000) == 0 ||
        omegaDdfIsFullWidth(chipset.ddfstrt) ||
        (chipset.dmaconr & 0x300) != 0x300 ||
        !displayWindowContainsLine(internal.vPos) ||
        host.pixels == NULL ||
        host.rasterRow < 0 || host.rasterRow >= HOST_RASTER_H ||
        host.rasterX < 0 || host.rasterX + 15 >= HOST_RASTER_W)
        return;

    uint16_t p1 = 0, p2 = 0, p3 = 0, p4 = 0;
    if (internal.bitplaneMask & 0x01)
        p1 = internal.chipramW[bitplaneLine.pointerAtFetchCompletion[0]];
    if (internal.bitplaneMask & 0x02)
        p2 = internal.chipramW[bitplaneLine.pointerAtFetchCompletion[1]];
    if (internal.bitplaneMask & 0x04)
        p3 = internal.chipramW[bitplaneLine.pointerAtFetchCompletion[2]];
    if (internal.bitplaneMask & 0x08)
        p4 = internal.chipramW[bitplaneLine.pointerAtFetchCompletion[3]];

    uint32_t *pixbuff = (uint32_t *)host.pixels;
    hiresPlanar2Chunky(&pixbuff[host.rasterRow * HOST_RASTER_W + host.rasterX],
                       p1, p2, p3, p4);
    host.rasterX += 16;
}

void dma_execute(){
    
    chipset.vposr   = OMEGA_VIDEO_VPOSR_ID | (internal.vPos >> 8);
    chipset.vhposr  = internal.vPos << 8;
    chipset.vhposr |= internal.hPos;
    uint8_t enabledPlanes = enabledBitplaneMask();
    int dmaEnabled = (chipset.dmaconr & 0x300) == 0x300;
    int displayWindowActive = displayWindowContainsLine(internal.vPos);
    bitplaneLine.enabledMask |= enabledPlanes;
    if (dmaEnabled && displayWindowActive)
        bitplaneLine.fetchEligibleMask |= enabledPlanes;
    

    // SDL_AtomicSet(&cpuWait, 1); // single-threaded on RP2350
    if(chipset.bplcon0 & 0x8000){
        // The 0x3c full-width window consumes one more word than the narrower
        // 0x40 fetch window. The latter's right-edge word is supplied by
        // hiresDisplayPrefetch() without changing its stride.
        bitplaneLine.lastCycle = chipset.ddfstop +
            omegaDdfHiresFetchTail(chipset.ddfstrt, chipset.ddfstop);
        DMAHires[internal.hPos]();
    }else{
        // LORES: 20 active fetches (table slots at ddfstrt+7 offset from real OCS)
        bitplaneLine.lastCycle =
            chipset.ddfstrt + OMEGA_DDF_LORES_FETCH_SPAN;
        DMALores[internal.hPos]();
    }

    if (internal.hPos == bitplaneLine.lastCycle) {
        bitplaneLine.fetchWindowComplete = 1;
        bitplaneLine.pointerAtFetchCompletion[0] = chipset.bpl1pt;
        bitplaneLine.pointerAtFetchCompletion[1] = chipset.bpl2pt;
        bitplaneLine.pointerAtFetchCompletion[2] = chipset.bpl3pt;
        bitplaneLine.pointerAtFetchCompletion[3] = chipset.bpl4pt;
        if (bitplaneLine.fetchedMask != 0) {
            lastFetchedMask = bitplaneLine.fetchedMask;
            applyBitplaneModulo(bitplaneLine.fetchedMask);
        }
    }

    eclock_execute(&chipset);   // CIA timers

    internal.hPos++;
    

    
    
    //end of line reached! 227 colour clocks have executed
    if(internal.hPos > 0xE3){
        // SDL_AtomicSet(&cpuWait, 0); // single-threaded on RP2350

        // Denise still presents the next HIRES word at the right edge even
        // though Agnus does not consume it as part of the line stride. Peek
        // at it for display only; do not advance any bitplane pointer.
        hiresDisplayPrefetch();

        if (bitplaneLine.fetchedMask != 0 && host.rasterRow >= 0 &&
            host.rasterRow < HOST_RASTER_H)
            host.rasterLineWidth[host.rasterRow] = (uint16_t)host.rasterX;

        internal.hPos = 0;
        internal.vPos +=1;
        CIATODEvent(&CIAB);
        
        advanceBitplanePointers();
        resetBitplaneLine();
        
        //VBL Time
        if(internal.vPos >= OMEGA_VIDEO_FRAME_LINES){
            internal.vPos = 0;
            copperWaitPosition = 0;
            //chipset.vposr   = (internal.LOF | 0x1000); //0x1000 is for NTSC / 0x0000 is for PAL

            //Reset Copper.
            putChipReg16[COPJMP1](0);
            CIATODEvent(&CIAA);
            
            
            //need to generate a vbl int
            putChipReg16[INTREQ](0x8020);
            
            host.rasterRow = 0;
            host.rasterX = 0;
            
            
            hostDisplay(); //Call the host to update the display.
        }
        
    }
}


void evenCycle(void){

    
    if(copperExecute()==1){
        return;
    }
    
    //if the copper doesn't want the even cycle, give it to the odd cycle devices.
    oddCycle();

}

void oddCycle(void){
    
    if(blitterExecute()==1){
        return;
    }
    
    //A Free slot for CPU... but the CPU isn't currently bound to the DMA timing
    // SDL_AtomicSet(&cpuWait, 0); // single-threaded on RP2350
}

void dramCycle(void){
    
}

int turboFloppy = 8;    //8 for turbo

void diskCycle(void){
    
        
    if( (chipset.dmaconr & 0x210) && chipset.dsklen & 0x8000 ){
        
        if(chipset.dsklen & 0x3FFF){
            
            
            //OS2+ Disk loading
            
            for(int i=0;i<turboFloppy;++i){
                
                if(chipset.adkconr & 0x400){
                    //printf("Needs a disk sync");
                    //sync = 0;
                }
            
                uint8_t b1 = floppyDataRead();
                uint8_t b2 = floppyDataRead();

                uint16_t syncword = b1 << 8 | b2;

                static int rdLog = 0;
                if (rdLog < 8) { printf("[RD] cyl=%d side=%d idx=%d b1=%02X b2=%02X sw=%04X dsksync=%04X\n", df[driveSelected].cylinder, df[driveSelected].side, df[driveSelected].index-2, b1, b2, syncword, chipset.dsksync); rdLog++; }

                if(syncword==chipset.dsksync){
                
                    putChipReg16[INTREQ](0x9000);   //DSKSYNC INT
                    chipset.dskbytr |= 0x1000;  // set word sync bit
                
                    if(floppySync==0){

                        static int syncHits = 0;
                        if (syncHits < 5) printf("[SYNC] found cyl=%d side=%d idx=%d dsklen=%04X (hit %d)\n", df[driveSelected].cylinder, df[driveSelected].side, df[driveSelected].index-2, chipset.dsklen, ++syncHits);
                        floppySync=1;
                        return;
                    }
                }
                
                if(floppySync == 1){
                    
                    //chipset.dskbytr &= 0xEFFF;      //clear word sync
                    CHIPRAM_BASE_PTR[chipset.dskpt & CHIPTOP] = b1;
                    chipset.dskpt += 1;

                    CHIPRAM_BASE_PTR[chipset.dskpt & CHIPTOP] = b2;
                    chipset.dskpt += 1;
                
                    chipset.dsklen -= 1;
                
                    if( (chipset.dsklen & 0x3FFF) == 0){
                        static int dskblkHits = 0;
                        if (dskblkHits < 5) printf("[DSKBLK] fired (hit %d) cyl=%d side=%d\n", ++dskblkHits, df[driveSelected].cylinder, df[driveSelected].side);
                        putChipReg16[INTREQ](0x8002);   //Disk block loaded INT
                        floppySync = 0;
                        return;
                    }
                
                }
            }
            
            
            return;
        }
       
    }
    
    oddCycle();
}


void audio0Cycle(void){
    
    if((chipset.dmacon & 0x201) == 0x201){
        
        internal.audio0Countdown -=1;
        
        if(internal.audio0Countdown<0){
            internal.audio0Countdown = chipset.aud0per;

        
            chipset.aud0len -=1;
        
            if(chipset.aud0len ==0){
                internal.audio0Countdown = 0;
                putChipReg16[INTREQ](0x8080);
            }
        }
        return;
    }
    
    oddCycle();
}


void audio1Cycle(void){
    
    if((chipset.dmacon & 0x202) == 0x202){
        
        internal.audio1Countdown -=1;
        
        if(internal.audio1Countdown<0){
            internal.audio1Countdown = chipset.aud1per;
            
            
            chipset.aud1len -=1;
            
            if(chipset.aud1len ==0){
                internal.audio1Countdown = 0;
                putChipReg16[INTREQ](0x8100);
            }
        }
        return;
    }
    
    oddCycle();
}

void audio2Cycle(void){
    
    if((chipset.dmacon & 0x204) == 0x204){
        
        internal.audio2Countdown -=1;
        
        if(internal.audio2Countdown<0){
            internal.audio2Countdown = chipset.aud2per;
            
            
            chipset.aud2len -=1;
            
            if(chipset.aud2len ==0){
                internal.audio2Countdown = 0;
                putChipReg16[INTREQ](0x8200);
            }
        }
        return;
    }
    
    oddCycle();
}

void audio3Cycle(void){
    
    if((chipset.dmacon & 0x208) == 0x208){
        
        internal.audio3Countdown -=1;
        
        if(internal.audio3Countdown<0){
            internal.audio3Countdown = chipset.aud3per;
            
            
            chipset.aud3len -=1;
            
            if(chipset.aud3len ==0){
                internal.audio3Countdown = 0;
                putChipReg16[INTREQ](0x8400);
            }
        }
        return;
    }
    
    oddCycle();
}


void spriteCycle(void){

    oddCycle();
}


int bitplaneActive(){
    //check if DMA is on, if not let the Copper and Blitter run.
    if((chipset.dmaconr & 0x300) != 0x300){
        return 0;
    }
    
    //too early horisonal position let the Copper and Blitter run
    if(internal.hPos<(chipset.ddfstrt)){
        return 0;
    }
 
    if (internal.hPos > bitplaneLine.lastCycle) {
        return 0;
    }
    
    if (!displayWindowContainsLine(internal.vPos))
        return 0;
    
    return 1;
}



void plane6(void){
    
    if(bitplaneActive()==0){
        evenCycle(); // let the copper run
        return;
    }
    
    chipset.bpl6dat = 0;
    if( (internal.bitplaneMask & 0x20)  == 0x20){
        markBitplaneFetched(6);
        uint16_t* p = &internal.chipramW[chipset.bpl6pt];
        chipset.bpl6pt +=1;
        chipset.bpl6dat = *p;
        return;
    }
    
    evenCycle();
}


void plane5(void){
    
    if(bitplaneActive()==0){
        evenCycle(); // let the copper run
        return;
    }
    
    chipset.bpl5dat = 0;
    if( (internal.bitplaneMask & 0x10)  == 0x10){
        markBitplaneFetched(5);
        uint16_t* p = &internal.chipramW[chipset.bpl5pt];
        chipset.bpl5pt +=1;
        chipset.bpl5dat = *p;
        return;
    }

    evenCycle();
    
}
void loresPlane1(void){
    
    
    if(bitplaneActive()==0){
        return;
    }
    
    if(host.pixels == NULL){
        return;
    }
    if( (internal.bitplaneMask & 0x1)  == 0x1){
        markBitplaneFetched(1);
        host.displayIsLores = 1;
        if (bitplaneLine.loresWords++ == 0) {
            int display_line = omegaDdfIsFullWidth(chipset.ddfstrt)
                             ? internal.vPos - (chipset.diwstrt >> 8)
                             : internal.vPos - OMEGA_DISPLAY_RASTER_ORIGIN;
            host.rasterRow = omegaDdfIsFullWidth(chipset.ddfstrt)
                           ? display_line * 2 : display_line;
            host.rasterX = 0;
        }
        uint16_t* p = &internal.chipramW[chipset.bpl1pt];
        chipset.bpl1pt +=1;
        chipset.bpl1dat = *p;
    }

    if (internal.vPos < OMEGA_LORES_FIRST_RENDER_LINE) {
        evenCycle();
        return;
    }
    if (host.rasterRow < 0 || host.rasterRow >= HOST_RASTER_H ||
        host.rasterX < 0 || host.rasterX + 31 >= HOST_RASTER_W)
        return;
    
    uint32_t* pixbuff = (uint32_t*)host.pixels;
    if(chipset.bplcon0 & 0x800){
        loresHAM2Chunky(&pixbuff[host.rasterRow * HOST_RASTER_W + host.rasterX], chipset.bpl1dat, chipset.bpl2dat, chipset.bpl3dat, chipset.bpl4dat,chipset.bpl5dat, chipset.bpl6dat);
    }else{
        loresPlanar2Chunky(&pixbuff[host.rasterRow * HOST_RASTER_W + host.rasterX], chipset.bpl1dat, chipset.bpl2dat, chipset.bpl3dat, chipset.bpl4dat,chipset.bpl5dat, chipset.bpl6dat);
    }
    host.rasterX += 32;
    
}



void plane4(){
    
    if(bitplaneActive()==0){
        evenCycle(); // let the copper run
        return;
    }
    
    chipset.bpl4dat = 0;
    if( (internal.bitplaneMask & 0x8)  == 0x8){
        markBitplaneFetched(4);
        uint16_t* p = &internal.chipramW[chipset.bpl4pt];
        chipset.bpl4pt +=1;
        chipset.bpl4dat = *p;
        return;
    }
    
    evenCycle();
    
}

void plane2(){
    
    if(bitplaneActive()==0){
        evenCycle(); // let the copper run
        return;
    }
    
    chipset.bpl2dat = 0;
    if( (internal.bitplaneMask & 0x2)  == 0x2){
        markBitplaneFetched(2);
        uint16_t* p = &internal.chipramW[chipset.bpl2pt];
        chipset.bpl2pt +=1;
        chipset.bpl2dat = *p;
        return;
    }
    
    oddCycle();
    
}

void plane3(){
    
    if(bitplaneActive()==0){
        evenCycle(); // let the copper run
        return;
    }
    
    chipset.bpl3dat = 0;
    if( (internal.bitplaneMask & 0x4)  == 0x4){
        markBitplaneFetched(3);
        uint16_t* p = &internal.chipramW[chipset.bpl3pt];
        chipset.bpl3pt +=1;
        chipset.bpl3dat = *p;
        return;
    }
    
    evenCycle();
    
}

void hiresPlane1(){

    if(bitplaneActive()==0){

        evenCycle(); // let the copper run
        return;
    }

    if(host.pixels == NULL){
        return;
    }
    chipset.bpl1dat = 0;
    if( (internal.bitplaneMask & 0x1)  == 0x1){
        markBitplaneFetched(1);
        host.displayIsLores = 0;
        uint16_t* p = &internal.chipramW[chipset.bpl1pt];
        chipset.bpl1pt +=1;
        chipset.bpl1dat = *p;
        bitplaneLine.hiresWords++;
    }

    // The full-width raster begins 40 PAL beam lines below DIWSTRT. Remove
    // that upper overscan so all 200 useful rows fit in the host framebuffer.
    if (omegaDdfIsFullWidth(chipset.ddfstrt) &&
        bitplaneLine.hiresWords == 1) {
        int upper_overscan =
            omegaDdfUpperOverscan(chipset.ddfstop);
        int display_line = internal.vPos - (chipset.diwstrt >> 8) -
                           upper_overscan;
        if (display_line < 0)
            return;
        host.rasterRow = display_line;
        host.rasterX = 0;
    } else if (!omegaDdfIsFullWidth(chipset.ddfstrt) &&
               bitplaneLine.hiresWords == 1) {
        host.rasterRow = internal.vPos - OMEGA_DISPLAY_RASTER_ORIGIN;
        host.rasterX = 0;
    }

    if (internal.vPos < OMEGA_DISPLAY_RASTER_ORIGIN) {
        evenCycle();
        return;
    }
    if (host.rasterRow < 0 || host.rasterRow >= HOST_RASTER_H ||
        host.rasterX < 0 || host.rasterX + 15 >= HOST_RASTER_W)
        return;

    uint32_t* pixbuff = (uint32_t*)host.pixels;
    uint32_t *line = &pixbuff[host.rasterRow * HOST_RASTER_W + host.rasterX];
    hiresPlanar2Chunky(line, chipset.bpl1dat, chipset.bpl2dat, chipset.bpl3dat, chipset.bpl4dat);
    host.rasterX += 16;
    return;
}







int copperExecute(){
    
    if((chipset.dmaconr & 0x280) != 0x280){
        return 0;
    }
        
    switch(internal.copperCycle){
        case 0:
            internal.IR1 = internal.chipramW[internal.copperPC];
            internal.IR1 = (internal.IR1 <<8) | (internal.IR1 >>8);
            internal.copperPC += 1;
            internal.copperCycle = 1;
            
            if( (internal.IR1 & 0x1) == 0x1){
                internal.copperCycle = 2;
            }
            return 1;
            break;
            
        case 1:
            internal.IR2 = internal.chipramW[internal.copperPC];
            internal.copperPC += 1;
            
            internal.IR1 = (internal.IR1 >> 1) & 255;   // divide by 2 and mask off bad bits
            
            if(internal.IR1<0x20){
                internal.copperCycle = 4;return 1;};         //pause copper until next vbl
            if(internal.IR1<0x40 && chipset.copcon==0){
                internal.copperCycle = 4;return 1;};         //pause copper until next vbl, unless we allow copper access to blitter
            

            
            //Move
            internal.IR2 = (internal.IR2 <<8) | (internal.IR2 >> 8);
            debugChipAddress = internal.IR1; //Debug what the Copper is writing to...
            debugChipValue   = internal.IR2;

            putChipReg16[internal.IR1](internal.IR2);
            internal.copperCycle = 0;
            return 1;
            break;
            
        case 2:
            internal.IR2 = internal.chipramW[internal.copperPC];
            internal.copperPC += 1;
            
            internal.IR2 = (internal.IR2 <<8) | (internal.IR2 >>8);
            
            internal.comparisonMask = (internal.IR2 | 0x0000); //ignore the instruction bits

            internal.IR1 &= internal.comparisonMask; //mask the wait position

            copperWaitPosition = internal.IR1;
            // PAL Copper lists cross the 8-bit vertical comparator boundary
            // with a wait near line 255 followed by a low-byte wait.  Keep
            // that second wait in the next 256-line bank instead of allowing
            // it to complete immediately at the wrap.
            if (OMEGA_VIDEO_STANDARD == OMEGA_VIDEO_PAL &&
                internal.vPos >= 255 &&
                ((internal.IR1 >> 8) & 0xff) < (internal.vPos & 0xff))
                copperWaitPosition += 0x10000;

            internal.copperCycle = 3;
            
            //Skip
            if( (internal.IR2 & 1) == 1){
                
                if( chipset.vhposr >= internal.IR1){
                    internal.copperPC +=2;
                }
                internal.copperCycle = 0;
                
                //printf("%04x Cop Skip!\n",internal.copperPC - 4);
                return 1;
                
            }
            
            return 1;
            break;
            
        case 3:
            
            /* old debugging stuff
            if(1){}
            uint16_t vhposr = chipset.vhposr;
            uint16_t wait = internal.IR1;
            uint16_t comparison = internal.IR2;
            */
            
            //Wait
            uint32_t beamPosition = ((uint32_t)internal.vPos << 8) |
                                    internal.hPos;
            uint32_t maskedBeam = beamPosition & (0xFFFF0000u | internal.IR2);
            if(maskedBeam >= copperWaitPosition){
                internal.copperCycle = 0;
            }
            
            break;
        case 4:
            //Copper operation frozen until next VBL/copper reset
            break;
    }
    
    return 0;
}



//********************Blitter



int blitterExecute(){
    
    static int state = 0;
    
    
    switch(state){
            
        case 0:
            //Blitter DMA Enabled and blitter busy flag set (which means bltsize was written to and blitting should start).
            if( (chipset.dmaconr & 0x4240) == 0x4240 ){
                
                if(chipset.bltcon1 & 1){
                    //Line Mode
                    //printf("Line Mode\n");
                    state = 1;
                }else{
                    //Copy Mode
                    //printf("Copy Mode\n");
                    /*
                    internal.useMask = chipset.bltcon0 >> 8;
                    internal.shiftA = chipset.bltcon0 >> 12;
                    internal.shiftB = chipset.bltcon1 >> 12;
                    internal.minterm = chipset.bltcon0 & 255;
                    internal.xIncrement = 1;
                    internal.fillmode = (chipset.bltcon1 & 0x18) >> 3; // 1= Inclusive Fill, 2 = Exclusive Fill
                    
                    //all pointers are word addressed and masked for 2meg only
                    internal.apt=(chipset.bltapt >> 1);
                    internal.bpt=(chipset.bltbpt >> 1);
                    internal.cpt=(chipset.bltcpt >> 1);
                    internal.dpt=(chipset.bltdpt >> 1);
                    
                    
                    internal.amod=chipset.bltamod / 2; //these are signed so need to be divided not shifted
                    internal.bmod=chipset.bltbmod / 2;
                    internal.cmod=chipset.bltcmod / 2;
                    internal.dmod=chipset.bltdmod / 2;
                    
                    //internal.lastHWord=chipset.bltsizh - 1;
                    internal.sizeh = chipset.bltsizh;
                    
                    Internal_t* debug = &internal;
                    */
                    state = 2;
                }
            }
            break;
            
        case 1:
            //Line Mode
            
            //No DMA cycle mode yet
            
            //Immediate mode Blitter
            blitter_execute(&chipset);
            state = 0;
            
            break;
            
        case 2:
            //Copy Mode
            //if descend mode is on.
            if((chipset.bltcon1 & 2)){
                internal.xIncrement = -1;
                
                if(internal.fillmode>0){
                    printf("NO FILL MODE YET!!");
                }
                
            }
            
            //DMA cycle mode
            //state = blitterCopyCycle();   //doesn't work at all.
            
            
            //Immediate mode Blitter
            blitter_execute(&chipset);
            state = 0;
            
            break;
    }
    
    
    
    return 0;
}


int blitterCopyCycle(){
    
    
    //Internal_t* debug = &internal;
    
        static uint16_t previousA = 0;
        static uint16_t previousB = 0;
        
            uint16_t channelD = 0;
            
            //Channel A
            if(internal.useMask & 8){
                chipset.bltadat = internal.chipramW[internal.apt];
                chipset.bltadat = chipset.bltadat << 8 | chipset.bltadat >> 8;
                internal.apt +=internal.xIncrement;
            }
            uint16_t channelA = chipset.bltadat;
            
            //Channel B
            if(internal.useMask & 4){
                chipset.bltbdat = internal.chipramW[internal.bpt];
                chipset.bltbdat = chipset.bltbdat << 8 | chipset.bltbdat >> 8;
                internal.bpt +=internal.xIncrement;
            }
            uint16_t channelB = chipset.bltbdat;
            
            //Channel C
            if(internal.useMask & 2){
                chipset.bltcdat = internal.chipramW[internal.cpt];
                chipset.bltcdat = chipset.bltcdat << 8 | chipset.bltcdat >> 8;
                internal.cpt +=internal.xIncrement;
            }
            
            
            //Masking section
            
            if(chipset.bltsizh==internal.sizeh){
                channelA = channelA & chipset.bltafwm;
            }
            
            if(chipset.bltsizh==1){
                channelA = channelA & chipset.bltalwm;
            }
            
            //shifting section
            if(internal.xIncrement==-1){
                
                channelA = (previousA >> (16-internal.shiftA)) | (channelA << internal.shiftA);
                channelB = (previousB >> (16-internal.shiftB)) | (channelB << internal.shiftB);
                
            }else{
                
                channelA = (previousA << (16-internal.shiftA)) | (channelA >> internal.shiftA);
                channelB = (previousB << (16-internal.shiftB)) | (channelB >> internal.shiftB);
                
            }
            previousA = chipset.bltadat;
            previousB = chipset.bltbdat;
            
            
            channelD = logicFunction(internal.minterm, channelA, channelB, chipset.bltcdat);
            
            //Zero Flag
            if(channelD==0){
                chipset.dmaconr = chipset.dmaconr | 0x2000;   //set zero flag
            }else{
                chipset.dmaconr = chipset.dmaconr & 0xDFFF; // clear zero flag
            }
            
            //Channel D
            if(internal.useMask & 1){
                
                channelD = channelD << 8 | channelD >>8;
                internal.chipramW[internal.dpt] = channelD;
                internal.dpt +=internal.xIncrement;
            }
            
        
        
        if(internal.xIncrement == -1){
            internal.apt -= internal.amod;
            internal.bpt -= internal.bmod;
            internal.cpt -= internal.cmod;
            internal.dpt -= internal.dmod;
        }else{
            internal.apt += internal.amod;
            internal.bpt += internal.bmod;
            internal.cpt += internal.cmod;
            internal.dpt += internal.dmod;
        }
        
        chipset.bltsizh -= 1;
        
        if(chipset.bltsizh == 0x0){
            chipset.bltsizh = internal.sizeh;
            chipset.bltsizv -=1;
            previousA = 0;
            previousB = 0;

        }
        
        if(chipset.bltsizv==0x0){
            //chipset.bltsizh = 0;
            
            //save blitter state - something might depend upon the blitter being in a known state
            //chipset.bltapt = internal.apt << 1;
            chipset.bltbpt = internal.bpt << 1;
            chipset.bltcpt = internal.cpt << 1;
            chipset.bltdpt = internal.dpt << 1;
            
            chipset.dmaconr = chipset.dmaconr & 49151; //clear blitter busy bit
            putChipReg16[INTREQ](0x8040);              // generate an interrupt!
            return 0; // no more copy to do
        }
    
    return 2; // more copy to do!
}
