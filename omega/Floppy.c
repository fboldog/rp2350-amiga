//
//  Floppy.c
//  The Omega Project
//  https://github.com/h5n1xp/Omega
//
//  Created by Matt Parsons on 02/02/2019.
//  Copyright © 2019 Matt Parsons. All rights reserved.
//  <h5n1xp@gmail.com>
//
//  Clocking and Checksum Code contributed by Dirk Hoffmann
//
//  This Source Code Form is subject to the terms of the
//  Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
//  with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "Floppy.h"
#include "Chipset.h"
#include "Memory.h"

#ifndef PICO_BUILD
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#else
#include "../src/psram.h"
#endif


void encodeBlock(uint8_t* source, uint8_t* destination,int size){
    
    for(int i=0;i<size;i++){
        destination[i] = (source[i] >> 1) & 0x55;
        destination[i+size] = (source[i]) & 0x55;
    }
    
}

uint8_t addClockBits(uint8_t previous, uint8_t value) {
    // Clear all previously set clock bits
    value &= 0x55;
    
    // Compute clock bits (clock bit values are inverted)
    uint8_t lShifted = (value << 1);
    uint8_t rShifted = (value >> 1) | (previous << 7);
    uint8_t cBitsInv = lShifted | rShifted;
    
    // Reverse the computed clock bits
    uint64_t cBits = cBitsInv ^ 0xAA;
    
    // Return original value with the clock bits added
    return value | cBits;
}

#ifndef PICO_BUILD
void ADF2MFM(int fd, uint8_t* mfm){

    int size =(int) lseek(fd, 0, SEEK_END);
    
    //printf("FloppySize: %d\n",size);
    
    //512 bytes per sector
    int sectors = size/512;
    
    //22 sectors per track (each side 11 sectors)
    int tracks = (sectors / 22);
    
    //each track is 12798 bytes in size, multiplied by 2 because there are 2 sides
    //int mfmSize = tracks * (12798 * 2);
    
    uint8_t* adf = malloc(size);
    // uint8_t mfm[mfmSize];
    
    uint8_t lowlevelSector[544]; //bytes per low level sector
    
    lseek(fd, 0, SEEK_SET);
    read(fd, adf, size);
    
    
    int count = 0;
    int s = 0;
    
    for(int track = 0;track<tracks;track++){
        for(int side=0;side<2;side++){
            for(int sector=0;sector<11;sector++){
                
                //int secCountDown = 11 - sector;
                
                
                //printf("%d: Track %d, Side: %d, Sector %d (%d)\n",s,track,side,sector,secCountDown);
                
                //Build the sector
                lowlevelSector[0] = 0x0;
                lowlevelSector[1] = 0x0;
                
                lowlevelSector[2] = 0xA1; // will be a sync mark
                lowlevelSector[3] = 0xA1; // will be a sync mark
                
                //sector info
                
                lowlevelSector[4] = 0xFF;
                lowlevelSector[5] = track << 1 | side;
                lowlevelSector[6] = sector;
                lowlevelSector[7] = 11 - sector;
                
                //Sector label
                lowlevelSector[8]  = 0x0;
                lowlevelSector[9]  = 0x0;
                lowlevelSector[10] = 0x0;
                lowlevelSector[11] = 0x0;
                
                lowlevelSector[12] = 0x0;
                lowlevelSector[13] = 0x0;
                lowlevelSector[14] = 0x0;
                lowlevelSector[15] = 0x0;
                
                lowlevelSector[16] = 0x0;
                lowlevelSector[17] = 0x0;
                lowlevelSector[18] = 0x0;
                lowlevelSector[19] = 0x0;
                
                lowlevelSector[20] = 0x0;
                lowlevelSector[21] = 0x0;
                lowlevelSector[22] = 0x0;
                lowlevelSector[23] = 0x0;
                
                
                //data
                for(int i=0;i<512;++i){
                    lowlevelSector[32+i] = adf[i+(count*512)];
                    
                }
                
                
                
                //***************
                
                //Encode
                
                mfm[s+0] = 0xAA;
                mfm[s+1] = 0xAA;
                mfm[s+2] = 0xAA;
                mfm[s+3] = 0xAA;
                
                mfm[s+4] = 0x44;
                mfm[s+5] = 0x89;
                mfm[s+6] = 0x44;
                mfm[s+7] = 0x89;
                
                //info
                encodeBlock(&lowlevelSector[4], &mfm[s+8], 4); // adds 8 bytes
                
                //Disklabel
                encodeBlock(&lowlevelSector[8], &mfm[s+16], 16);//adds 32 bytes
                
                //Data section
                encodeBlock(&lowlevelSector[32], &mfm[s+64], 512);
                
                
                //Header checksum
                uint8_t hcheck[4] = { 0, 0, 0, 0 };
                for(unsigned i = 8; i < 48; i += 4) {
                    hcheck[0] ^= mfm[s+i];
                    hcheck[1] ^= mfm[s+i+1];
                    hcheck[2] ^= mfm[s+i+2];
                    hcheck[3] ^= mfm[s+i+3];
                }
                
                lowlevelSector[24] = hcheck[0];
                lowlevelSector[25] = hcheck[1];
                lowlevelSector[26] = hcheck[2];
                lowlevelSector[27] = hcheck[3];
                
                //header checksum
                encodeBlock(&lowlevelSector[24], &mfm[s+48], 4); //adds 8 bytes
                
                
                // Data checksum
                uint8_t dcheck[4] = { 0, 0, 0, 0 };
                for(unsigned i = 64; i < 1088; i += 4) {
                    dcheck[0] ^= mfm[s+i];
                    dcheck[1] ^= mfm[s+i+1];
                    dcheck[2] ^= mfm[s+i+2];
                    dcheck[3] ^= mfm[s+i+3];
                }
                
                lowlevelSector[28] = dcheck[0];
                lowlevelSector[29] = dcheck[1];
                lowlevelSector[30] = dcheck[2];
                lowlevelSector[31] = dcheck[3];
                
                //Encode Data checksum
                encodeBlock(&lowlevelSector[28], &mfm[s+56], 4); //adds 8 bytes
                
                
                
                //Add clocking bits
                for(int i=8;i<1088;i++){
                    uint8_t previous = mfm[s+i-1];
                    
                    mfm[s+i] = addClockBits(previous,mfm[s+i]);
                    //mfm[s+i] = clocking(previous, mfm[s+i]);
                    
                }
                
                s += 1088;    //Why not 1088, which is the size of the data we've produced
                count +=1;
                
                
            }
            
            //Add clocking bits to the track gap
            mfm[s]   = addClockBits(mfm[s-1],0);
            mfm[s+1] = 0xA8;
            mfm[s+2] = 0x55;
            mfm[s+3] = 0x55;
            mfm[s+4] = 0xAA;
            
            for(int i=5;i<700;i++){
                uint8_t previous = mfm[s+i-1];
                
                mfm[s+i] = addClockBits(previous,0);
                //mfm[s+i] = clocking(previous, mfm[s+i]);
                
            }
            
            s += 830;   //pad track to make 12798 bytes to meet the ADF-EXT spec.
            //printf("\n");
        }
        
    }
    //printf("loaded");
    free(adf);
}
#endif // !PICO_BUILD

// ── RP2350 version: ADF data already in a memory buffer ──────────────────
void ADF2MFM_from_mem(const uint8_t* adf, uint32_t size, uint8_t* mfm) {
    int sectors = (int)(size / 512);
    int tracks  = sectors / 22;

    uint8_t lowlevelSector[544];

    int count = 0;
    int s = 0;

    for (int track = 0; track < tracks; track++) {
        for (int side = 0; side < 2; side++) {
            for (int sector = 0; sector < 11; sector++) {
                lowlevelSector[0] = 0x00;
                lowlevelSector[1] = 0x00;
                lowlevelSector[2] = 0xA1;
                lowlevelSector[3] = 0xA1;

                lowlevelSector[4] = 0xFF;
                lowlevelSector[5] = (uint8_t)((track << 1) | side);
                lowlevelSector[6] = (uint8_t)sector;
                lowlevelSector[7] = (uint8_t)(11 - sector);

                for (int i = 8; i < 32; i++) lowlevelSector[i] = 0;

                for (int i = 0; i < 512; i++)
                    lowlevelSector[32 + i] = adf[i + count * 512];

                mfm[s+0] = 0xAA; mfm[s+1] = 0xAA;
                mfm[s+2] = 0xAA; mfm[s+3] = 0xAA;
                mfm[s+4] = 0x44; mfm[s+5] = 0x89;
                mfm[s+6] = 0x44; mfm[s+7] = 0x89;

                encodeBlock(&lowlevelSector[4],  &mfm[s+8],  4);
                encodeBlock(&lowlevelSector[8],  &mfm[s+16], 16);
                encodeBlock(&lowlevelSector[32], &mfm[s+64], 512);

                uint8_t hcheck[4] = {0,0,0,0};
                for (unsigned i = 8; i < 48; i += 4) {
                    hcheck[0] ^= mfm[s+i];   hcheck[1] ^= mfm[s+i+1];
                    hcheck[2] ^= mfm[s+i+2]; hcheck[3] ^= mfm[s+i+3];
                }
                lowlevelSector[24] = hcheck[0]; lowlevelSector[25] = hcheck[1];
                lowlevelSector[26] = hcheck[2]; lowlevelSector[27] = hcheck[3];
                encodeBlock(&lowlevelSector[24], &mfm[s+48], 4);

                uint8_t dcheck[4] = {0,0,0,0};
                for (unsigned i = 64; i < 1088; i += 4) {
                    dcheck[0] ^= mfm[s+i];   dcheck[1] ^= mfm[s+i+1];
                    dcheck[2] ^= mfm[s+i+2]; dcheck[3] ^= mfm[s+i+3];
                }
                lowlevelSector[28] = dcheck[0]; lowlevelSector[29] = dcheck[1];
                lowlevelSector[30] = dcheck[2]; lowlevelSector[31] = dcheck[3];
                encodeBlock(&lowlevelSector[28], &mfm[s+56], 4);

                for (int i = 8; i < 1088; i++) {
                    mfm[s+i] = addClockBits(mfm[s+i-1], mfm[s+i]);
                }

                s     += 1088;
                count += 1;
            }

            mfm[s]   = addClockBits(mfm[s-1], 0);
            mfm[s+1] = 0xA8; mfm[s+2] = 0x55;
            mfm[s+3] = 0x55; mfm[s+4] = 0xAA;
            for (int i = 5; i < 700; i++)
                mfm[s+i] = addClockBits(mfm[s+i-1], 0);
            s += 830;
        }
    }
}
//*******************************



int floppySync = 0;
int driveSelected=0;
Fd_t df[4];

void floppyIndexReset(){
    
       df[driveSelected].index = 4;
    
}

uint8_t floppyDataRead(){ //this function should be called by the DMA
    
    int position   = (df[driveSelected].cylinder * (12798 * 2)) + (df[driveSelected].side  * 12798) + df[driveSelected].index;
    
    df[driveSelected].index +=1;

    if(df[driveSelected].index>12667){ //ADF track has 12798 bytes, but on a normal AmigaOS disk 12668 are used.
        df[driveSelected].index= 0;
        CIAIndex(&CIAB);    // generate CIAB index interupt
        
        //int stinkmog = (df[driveSelected].track * (12798 * 2)) + (df[driveSelected].side  * 12798) + df[driveSelected].index;
        //uint8_t* dri = &df[driveSelected].mfmData[stinkmog];
        //printf("wartest du!");
        
    }
    
    uint8_t retVal = df[driveSelected].mfmData[position];
    
    /*
    if(retVal==0){
        //printf("Uh oh!");
    }
    */
    
    return retVal;
}


void floppyInsert(int drive){

    if(df[drive].idMode !=0){
        //Only Vaild drives have an ID Mode == 0 
        return;
    }
    
    if(df[drive].hasDisk){
        df[drive].hasDisk = 0;
        df[drive].pra &= 0xFB;      // /CHNG=0 (change: disk removed)
        printf("Disk ejected from df%d:\n",drive);
    }else{
        df[drive].hasDisk = 1;
        df[drive].pra &= 0xFB;      // /CHNG=0 (change: disk inserted)
        printf("Disk inserted in df%d:\n",drive);
    }
    
}

uint8_t* floppyInit(int drive){
    df[drive].idMode = -1;
    df[drive].index = 0;
    df[drive].cylinder = 0;
    df[drive].side = 0;
    df[drive].hasDisk = 0;
    df[drive].pra  |= 0x04;   // /CHNG=1 (stable, no change at power-on)
    df[drive].pra  &= 0xEF;   // cylinder 0 (bit4=0)
    df[drive].pra  |= 0x20;   // drive not ready (/DKRDY=1, no disk)
#ifdef PICO_BUILD
    // Assign MFM buffer into PSRAM; only drives 0 and 1 are supported
    static const uint32_t psram_offsets[4] = {
        PSRAM_DF0_OFFSET,
        PSRAM_DF1_OFFSET,
        0, 0   // drives 2/3 unsupported on RP2350 (insufficient PSRAM)
    };
    df[drive].mfmData = (drive < 2) ? psram_ptr(psram_offsets[drive]) : NULL;
#endif
    return df[drive].mfmData;
}

void floppyState(){
    CIAA.pra &= 0xC3;
    CIAA.pra |= df[driveSelected].pra;
}

void floppySetState(){            //To be called when Writes to CIAB prb happen.
    
    static uint8_t PRB;
#ifndef PICO_BUILD
    static int count = 0;
#endif
    
    PRB = CIAB.prb;
    
    switch (PRB & 0x78) {
        case 0x78:
            CIAA.pra &=0xC3;    //Need to take the Drives off the floppy bus.
            return;
            break;
            
        case 0x70:
            driveSelected = 0;
            df[0].pra |= 0x04;   // /CHNG ack: SEL asserted → clear /CHNG latch
            break;

        case 0x68:
            driveSelected = 1;
            df[1].pra |= 0x04;
            break;

        case 0x58:
            driveSelected = 2;
            df[2].pra |= 0x04;
            break;

        case 0x38:
            driveSelected = 3;
            df[3].pra |= 0x04;
            break;
            
        default:
            break;
    }
    //count++;

    //printf("%d: ",count);
    
    //ID mode... to identify external drives...
     if(df[driveSelected].idMode>0){   // Id mode
         // Report drive present (/DKRDY=0) only for drives that physically exist.
         // Drive 0 is always present (even with no ADF loaded). Drives 1-3 are only
         // present if an ADF is loaded. Without this distinction, the ROM sees four
         // 3.5" DD drives, times out reading all of them, then falls to ROM disk.
         if(driveSelected == 0 || df[driveSelected].hasDisk){
             df[driveSelected].pra  &= 0xDF;  // /DKRDY=0 → drive present, ID=0x00000000
         }
         df[driveSelected].idMode -=1;
         CIAA.pra = (CIAA.pra & 0xC3) | (df[driveSelected].pra & 0x3C);
         return;
     }
    
    

    
    // If no change in state just return
    if(PRB == df[driveSelected].prb){
        floppyState();
        //printf("---\n");
        return;
    }
    


    

    //printf("DF%d - ",driveSelected);
    
    if(PRB & 0x80){
        //printf(" Motor Off ");
        //df[driveSelected].prb  |= 0x80;
        
        if(df[driveSelected].idMode==-1){   //is this the first time the motor has been turned off?
            df[driveSelected].idMode = 32;  //if so activate ID Mode
        }
        
        df[driveSelected].pra  |= 0x20;     //Drive not ready
        
    }else{
        //printf(" Motor On ");
        //df[driveSelected].prb  &= 0x7F;
        
        // Report drive ready (/DKRDY=0) for drives that physically exist.
        // Drive 0 is always present; without this, KS 2.04's 200-read polling
        // loop sees /DKRDY=1 and falls back to its internal ROM disk instead
        // of showing the insert-disk screen when no ADF is loaded.
        if(df[driveSelected].hasDisk || driveSelected == 0){
            df[driveSelected].pra  &= 0xDF;  // /DKRDY=0 → drive ready
        } else {
            df[driveSelected].pra  |= 0x20;  // non-existent drive → not ready
        }
        //floppySync=0;
    }
    

    
    //Step head (don't step again if we've already stepped)
    if( (PRB & 0x1) && !(df[driveSelected].prb & 0x1) ){
#ifndef PICO_BUILD
        printf("%04x - DF%d Click\n",count,driveSelected);
#endif
        
        if(PRB & 0x2){
            df[driveSelected].cylinder -=1;
            //printf("DF%d Head Stepped back: %d\n",driveSelected,df[driveSelected].cylinder);
        }else{
            df[driveSelected].cylinder +=1;
           //printf("DF%d Head Stepped forward: %d\n",driveSelected,df[driveSelected].cylinder);
        }
        
        if(df[driveSelected].cylinder < 0){
            df[driveSelected].cylinder = 0;
        }
    
        
        if(df[driveSelected].cylinder >79){    //not sure why sometimes the drive tries to go up to track 80.. with ks1.3
            df[driveSelected].cylinder = 79;
        }
        
        //floppySync = 0;
        
        //printf(" to track %d|",df[driveSelected].track);
        
    }
    
    
    if(df[driveSelected].cylinder==0){
        df[driveSelected].pra  &= 0xEF; //Track 0 reached
    }else{
        df[driveSelected].pra  |= 0X10; // not track 0;
    }
    
    
    if(PRB & 0x4){
        df[driveSelected].side = 0;
        //df[driveSelected].prb |= 0x4;
        //printf(" -lower surface.\n");
        //floppySync = 0;
        
    }else{
        df[driveSelected].side = 1;
        //df[driveSelected].prb &= 0xFB;
        //printf(" -upper surface.\n");
        //floppySync = 0;
    }
    
    
    df[driveSelected].prb = PRB;
    
    //printf("\n");
    CIAA.pra &= 0xC3;
    CIAA.pra |= df[driveSelected].pra;
    
    //CIAB.prb |= df[driveSelected].prb & 0x87;
    

}
