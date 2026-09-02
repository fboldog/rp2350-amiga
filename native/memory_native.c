//
//  Memory.c
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

#include "Memory.h"
//#include "Kick13.h"
#include "Chipset.h"
#include "CIA.h"
#include "Floppy.h"
#include "debug.h"
#include "DMA.h"
#include "Gayle.h"
#include "../omega/m68k.h"

unsigned char low16Meg[16777216];

//Chipset_t* chipset = (Chipset_t*)&low16Meg[0xDFF000];

void loadROM(){
    
    for(int i=0;i<524288;++i){
//         low16Meg[0xF80000+i] = kick13[i];
    }
}


unsigned int chipReadByte(unsigned int address){
    //ROM
    if(address>0xF80000){
        return low16Meg[address];
    }
    
    //Autoconfig space
    if(address>0xDFFFFF){
        return 0;
    }
    
#ifdef THREADED_CPU
    waitFreeSlot(); //CPU must wait for DMA to complete;
#endif
    
    //Chipregs
    if(address>0xDFEFFF){
        address = (address - 0xDFF000);
        debugChipAddress = address;
        return getChipReg8[address]();
    }
    
    //IDE Interface
    if(address>0xD9FFFF){
        
        return readGayleB(address);
        
    }
    
    //Slow RAM
    if(address>0xBFFFFF){
        return low16Meg[address];
    }
    
    //CIA A
    if(address>=0xBFE001){
        address = (address - 0xBFE001) >> 8;
        {
            static int ciaa_pra_count = 0;
            if (address == 0 && ciaa_pra_count < 200) {
                uint8_t result = CIARead(&CIAA, 0);
                uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
                uint32_t d0 = m68k_get_reg(NULL, M68K_REG_D0);
                uint32_t d1 = m68k_get_reg(NULL, M68K_REG_D1);
                printf("[CIAA.pra] read=0x%02X (/CHNG=%d /DKRDY=%d) D0=%08X D1=%08X drv=%d  PC=%08X (hit %d)\n",
                       result, (result>>2)&1, (result>>5)&1,
                       d0, d1, driveSelected, pc, ++ciaa_pra_count);
                fflush(stdout);
                return result;
            }
        }
        return CIARead(&CIAA,address);
    }

    //CIA B
    if(address>=0xBFD000){
        address = (address - 0xBFD000) >> 8;
        return CIARead(&CIAB,address);
    }

    //24bit fast ram
    if(address>0x1FFFFF){
        //return 0;
        return low16Meg[address];
    }

    //RAM
    address &=CHIPTOP;
    return low16Meg[address];
}
unsigned int chipReadWord(unsigned int address){
    //ROM
    if(address>0xF7FFFF){
        //return READ_WORD(chipset.rom,address-0xF80000);
        uint16_t value = *(uint16_t*)&low16Meg[address];
        value = (value << 8) | (value >> 8);
        return value;
        
    }
    
    //Autoconfig space
    if(address>0xDFFFFF){
        return 0;
    }
    
#ifdef THREADED_CPU
    waitFreeSlot(); //CPU must wait for DMA to complete;
#endif
    
    //Chipregs
    if(address>0xDFEFFF){
        address = (address - 0xDFF000) >> 1;
        
        if(address>16){
            
            if(address ==62){
                return chipset.deniseid;
            }
            
            printf("Attempt to read write-only register %s (returning DeniseID by default)\n",regNames[address]);
            debugChipAddress = address;
            return chipset.deniseid;    // the only read only register up that high is DeniseID...
        }
        debugChipAddress = address;
        return getChipReg16[address]();
        //return ChipsetRead(&chipset, address);
    }
    
    //IDE Interface
    if(address>0xD9FFFF){
        
       return readGayle(address);
        
    }
    
    //Slow RAM
    if(address>0xBFFFFF){
        // Incomplete address decoding: the 16 readable chipset word-registers
        // (offsets 0x000-0x01E from 0xDFF000) are mirrored throughout this
        // region via the low 12 bits.  KS 3.x uses this shadow to verify
        // INTENA/INTREQ writes without going through the chipset address range.
        {
            uint32_t regOff = address & 0xFFFu;
            if (regOff < 0x20u)
                return getChipReg16[regOff >> 1]();
        }

#ifdef NOSLOWRAM
        // Fallback full shadow (all chipset offsets) — kept for reference but
        // unsafe without bounds checking; the selective path above covers
        // the common cases.
        address &=0xFFF;
        return getChipReg16[address >> 1]();
#endif

        //return READ_WORD(chipset.chipram,address);
        uint16_t value = *(uint16_t*)&low16Meg[address];
        value = (value << 8) | (value >> 8);
        return value;
    }
    
    //CIA A
    if(address>=0xBFE001){
        address = (address - 0xBFE001) >> 8;
        return CIARead(&CIAA,address);
    }
    
    //CIA B
    if(address>=0xBFD000){
        address = (address - 0xBFD000) >> 8;
        return CIARead(&CIAB,address);
    }
    
    //24bit fast ram
    if(address>0x1FFFFF){
        return 0;
    }
    
    //RAM - Incomplete address decding means chip addresses below 2meg wrap around
    address &=CHIPTOP;
    
    //return READ_WORD(chipset.chipram,address);
    uint16_t value = *(uint16_t*)&low16Meg[address];
    value = (value << 8) | (value >> 8);
    return value;
}
unsigned int chipReadLong(unsigned int address){
    
    //ROM
    if(address>0xF7FFFF){
        //return READ_LONG(chipset.rom,address-0xF80000);
        uint32_t value = *(uint32_t*)&low16Meg[address];
        value = ((value << 8) & 0xFF00FF00 ) | ((value >> 8) & 0xFF00FF );
        return value << 16 | value >> 16;
    }
    
#ifdef THREADED_CPU
    waitFreeSlot(); //CPU must wait for DMA to complete;
#endif
    
    //Chipregs
    if(address>0xDFEFFF){
        address = (address - 0xDFF000) >> 1;
        address &=0xFF;  //maskout stupid values
        debugChipAddress = address;
        return getChipReg32[address]();
    }
    
    //IDE Interface
    if(address>0xD9FFFF){
        
        return readGayleL(address);
        
    }
    
    //Slow RAM
    if(address>0xBFFFFF){
        //return READ_LONG(chipset.chipram,address);
        uint32_t value = *(uint32_t*)&low16Meg[address];
        value = ((value << 8) & 0xFF00FF00 ) | ((value >> 8) & 0xFF00FF );
        return value << 16 | value >> 16;
    }
    
    //CIA A
    if(address>=0xBFE001){
        address = (address - 0xBFE001) >> 8;
        return CIARead(&CIAA,address);
    }
    
    //CIA B
    if(address>=0xBFD000){
        address = (address - 0xBFD000) >> 8;
        return CIARead(&CIAB,address);
    }
    
    //24bit fast ram
    if(address>0x1FFFFF){
        return 0;
    }
    
    //RAM - Incomplete address decding means chip addresses below 2meg wrap around
    address &=CHIPTOP;
    
    //return READ_LONG(chipset.chipram,address);
    uint32_t value = *(uint32_t*)&low16Meg[address];
    value = ((value << 8) & 0xFF00FF00 ) | ((value >> 8) & 0xFF00FF );
    return value << 16 | value >> 16;
}

static int sig_byte_hook_count = 0;
void chipWriteByte(unsigned int address,unsigned int value){   //ROM
    // Catch any byte write into tc_SigRecvd (0xC00C22..0xC00C25)
    if (address >= 0xC00C22u && address <= 0xC00C25u && sig_byte_hook_count < 20) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("[SIGRECVD-B] write_byte @%08X = 0x%02X  PC=%08X  (hit %d)\n",
               address, value & 0xFF, pc, ++sig_byte_hook_count);
        fflush(stdout);
    }
    // Catch byte writes to Zorro II autoconfig space (0xE80000-0xE8FFFF)
    if (address >= 0xE80000u && address <= 0xE8FFFFu) {
        static int autoconf_bwrite_count = 0;
        if (autoconf_bwrite_count < 40) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
            printf("[AUTOCONF-B] write_byte @%08X = 0x%02X  PC=%08X  (hit %d)\n",
                   address, value & 0xFF, pc, ++autoconf_bwrite_count);
            fflush(stdout);
        }
    }
    if(address>0xF80000){
        return;
    }

#ifdef THREADED_CPU
    waitFreeSlot(); //CPU must wait for DMA to complete;
#endif
    
    //Chipregs
    if(address>0xDFEFFF){
        address = (address - 0xDFF000) >> 1;
        debugChipAddress = address;
        //ChipsetWrite(&chipset, address,value); // Does any software byte write to the custom chips?
        return;
    }
    
    //IDE Interface
    if(address>0xD9FFFF){
        
        writeGayleB(address, value);
        return;
        
    }
    
    //Slow RAM
    if(address>0xBFFFFF){
        low16Meg[address] = value; return;
    }
    
    //CIA A
    if(address>=0xBFE001){
        address = (address - 0xBFE001) >> 8;
        CIAWrite(&CIAA,address,value);return;
    }
    
    //CIA B
    if(address>=0xBFD000){
        address = (address - 0xBFD000) >> 8;
        static int ciab_prb_count = 0;
        if (address == 1 && ciab_prb_count < 200) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
            printf("[CIAB.prb] write 0x%02X  pra=0x%02X hasDisk=%d idMode=%d  PC=%08X (hit %d)\n",
                   value, CIAA.pra, df[0].hasDisk, df[0].idMode, pc, ++ciab_prb_count);
            fflush(stdout);
        }
        CIAWrite(&CIAB,address,value);return;
    }
    
    //CIA A - weird ROM 3 thing...
    if(address>=0xBFA001){
        address = (address - 0xBFA001) >> 8;
        CIAWrite(&CIAA,address,value);return;
    }
    
    //24bit fast ram
    if(address>0x1FFFFF){
        return;
    }
    
    //RAM - Incomplete address decding means chip addresses below 2meg wrap around
    address &=CHIPTOP;
    low16Meg[address] = value;
}

static int dsklen_hook_count = 0;
static int intena_hook_count = 0;
static int dmacon_hook_count = 0;
void chipWriteWord(unsigned int address,unsigned int value){
    // DSKLEN write (0xDFF024): disk DMA start
    if (address == 0xDFF024u && dsklen_hook_count < 30) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("[DSKLEN] write_word @0xDFF024 = 0x%04X  dmaconr=0x%04X  PC=%08X  (hit %d)\n",
               value, chipset.dmaconr, pc, ++dsklen_hook_count);
        fflush(stdout);
    }
    // DMACON write (0xDFF096): log disk DMA enable/disable
    if (address == 0xDFF096u && dmacon_hook_count < 40) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("[DMACON] write 0x%04X  dmaconr=0x%04X  PC=%08X  (hit %d)\n",
               value, chipset.dmaconr, pc, ++dmacon_hook_count);
        fflush(stdout);
    }
    // INTENA write (0xDFF09A): log when DSKBLK (bit1) is set or cleared
    if (address == 0xDFF09Au && intena_hook_count < 40) {
        if (value & 0x8002u) {  // SET bit1 (DSKBLK)
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
            printf("[INTENA] DSKBLK SET 0x%04X  PC=%08X  (hit %d)\n",
                   value, pc, ++intena_hook_count);
            fflush(stdout);
        } else if (value & 0x0002u) {  // CLR bit1
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
            printf("[INTENA] DSKBLK CLR 0x%04X  PC=%08X  (hit %d)\n",
                   value, pc, ++intena_hook_count);
            fflush(stdout);
        }
    }
    // INTREQ write (0xDFF09C): log DSKBLK done and DSKSYNC
    {
        static int intreq_hook_count = 0;
        if (address == 0xDFF09Cu && intreq_hook_count < 60) {
            if (value & 0x0003u) {   // bit0=TBE, bit1=DSKBLK, bit2=SOFT — any disk-related
                uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
                printf("[INTREQ] 0x%04X  PC=%08X  (hit %d)\n",
                       value, pc, ++intreq_hook_count);
                fflush(stdout);
            } else if (value & 0x1000u) {  // bit12=DSKSYNC
                uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
                printf("[INTREQ-DSKSYNC] 0x%04X  PC=%08X  (hit %d)\n",
                       value, pc, ++intreq_hook_count);
                fflush(stdout);
            }
        }
    }
    //ROM
    if(address>0xF80000){
        return;
    }

#ifdef THREADED_CPU
    waitFreeSlot(); //CPU must wait for DMA to complete;
#endif
    
    //Chipregs
    if(address>0xDFEFFF){
        address = (address - 0xDFF000) >> 1;
        
        debugChipAddress = address;    // used for debugging to identify the register being called
        debugChipValue = value;
        putChipReg16[address](value);
        return;
    }
    
    //IDE Interface
    if(address>0xD9FFFF){
        
        writeGayle(address,value);
        return;
    }
    
    //SLow RAM
    if(address>0xBFFFFF){
        
        //printf("Write %d Chipset Shadow @ %06x\n",value,address);
        
        /*
         if(address==0xDBF09){
         return ChipsetWrite(&chipset, (address &0xFFF) >> 1,value);
         }
         */
        
#ifdef NOSLOWRAM
        //Chipset shadow - incomplete address decoding
        address &=0xFFF;
        //ChipsetWrite(&chipset, address >> 1,value); return;
        putChipReg16[address >> 1](value);
#endif
        
        //WRITE_WORD(chipset.chipram,address,value);return;
        uint16_t* dest = (uint16_t*)&low16Meg[address];
        value = (value << 8) | (value >> 8);
        *dest = value;return;
    }
    
    //CIA A
    if(address>=0xBFE001){
        return;
    }
    
    //CIA B
    if(address>=0xBFD000){
        return;
    }
    
    //24bit fast ram
    if(address>0x1FFFFF){
        return;
    }
    
    //RAM - Incomplete address decding means chip addresses below 2meg wrap around
    address &=CHIPTOP;
    
    //WRITE_WORD(chipset.chipram,address,value);
    uint16_t* dest = (uint16_t*)&low16Meg[address];
    value = (value << 8) | (value >> 8);
    *dest = value;return;

}
static int sig_hook_count  = 0;
static int q_hook_count    = 0;
static int rp_hook_count   = 0;  // reply port message list
static int sig2_hook_count = 0;  // tc_SigRecvd of input.device @C026E2
static int tr_hook_count   = 0;  // TaskReady lh_TailPred
static int execlib_sig_count = 0; // tc_SigRecvd of exec.library task @C01570
static int condev_sig_count  = 0; // tc_SigRecvd of console.device @C0A7E0
static int tdsig_hook_count  = 0; // tc_SigRecvd of trackdisk.device @C0485E
void chipWriteLong(unsigned int address,unsigned int value){
    // Catch Signal() to trackdisk.device @C0485E: tc_SigRecvd at +0x1A = 0xC04878
    if (address == 0xC04878u && tdsig_hook_count < 40) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("[TRACKDISK-SIG] write_long @0xC04878 = 0x%08X  PC=%08X  (hit %d)\n",
               value, pc, ++tdsig_hook_count);
        fflush(stdout);
    }
    // Catch writes to tc_SigRecvd (0xC00C22) — old hook (stale)
    if (address == 0xC00C22 && sig_hook_count < 20) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("[SIGRECVD] write_long @0xC00C22 = 0x%08X  PC=%08X  (hit %d)\n",
               value, pc, ++sig_hook_count);
        fflush(stdout);
    }
    // Catch Signal() to input.device @C026E2: tc_SigRecvd at +0x1A = 0xC026FC
    if (address == 0xC026FCu && sig2_hook_count < 30) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("[INPUTDEV-SIG] write_long @0xC026FC = 0x%08X  PC=%08X  (hit %d)\n",
               value, pc, ++sig2_hook_count);
        fflush(stdout);
    }
    // Catch Signal() to exec.library boot task @C01570: tc_SigRecvd at +0x1A = 0xC0158A
    if (address == 0xC0158Au && execlib_sig_count < 30) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("[EXECLIB-SIG] write_long @0xC0158A = 0x%08X  PC=%08X  (hit %d)\n",
               value, pc, ++execlib_sig_count);
        fflush(stdout);
    }
    // Catch Signal() to console.device task @C0A7E0: tc_SigRecvd at +0x1A = 0xC0A7FA
    if (address == 0xC0A7FAu && condev_sig_count < 30) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("[CONDEV-SIG] write_long @0xC0A7FA = 0x%08X  PC=%08X  (hit %d)\n",
               value, pc, ++condev_sig_count);
        fflush(stdout);
    }
    // Catch AddTail() to TaskReady (ExecBase=0xC00276; lh_TailPred at ExecBase+0x19E=0xC00414)
    if (address == 0xC00414u && tr_hook_count < 30) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("[TASKREADY] write_long @0xC00414 = 0x%08X  PC=%08X  (hit %d)\n",
               value, pc, ++tr_hook_count);
        fflush(stdout);
    }
    // Catch AddTail() to ROM disk queue at 0xC061E8 (lh_TailPred at +8 = 0xC061F0)
    if ((address == 0xC061E8 || address == 0xC061F0 || address == 0xC061F4) && q_hook_count < 6) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("[ROMDISK-Q1] write_long @%08X = 0x%08X  PC=%08X  (hit %d)\n",
               address, value, pc, ++q_hook_count);
        fflush(stdout);
    }
    // Catch writes to reply port mp_MsgList (0xC075EE+0x14..+0x1C) to detect ReplyMsg()
    // lh_Head=0xC07602, lh_Tail=0xC07606, lh_TailPred=0xC0760A
    if (address >= 0xC07602u && address <= 0xC0760Cu && rp_hook_count < 20) {
        uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
        printf("[REPLYPORT] write_long @%08X = 0x%08X  PC=%08X  (hit %d)\n",
               address, value, pc, ++rp_hook_count);
        fflush(stdout);
    }
    //ROM
    if(address>0xF80000){
        return;
    }

    // Catch writes to Zorro II autoconfig space (0xE80000-0xE8FFFF)
    // On real HW this is where KS probes for expansion boards; Omega returns 0x00 (should be 0xFF)
    if (address >= 0xE80000u && address <= 0xE8FFFFu) {
        static int autoconf_write_count = 0;
        if (autoconf_write_count < 40) {
            uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
            printf("[AUTOCONF-W] write_long @%08X = 0x%08X  PC=%08X  (hit %d)\n",
                   address, value, pc, ++autoconf_write_count);
            fflush(stdout);
        }
    }

#ifdef THREADED_CPU
    waitFreeSlot(); //CPU must wait for DMA to complete;
#endif

    //Chipregs
    if(address>0xDFEFFF){
        address = (address - 0xDFF000) >> 1;

                debugChipAddress = address;    // used for debugging to identify the register being called

        putChipReg32[address](value);
        return;
    }
    
    //IDE Interface
    if(address>0xD9FFFF){
        
        writeGayleL(address, value);
        
    }
    
    //Slow RAM
    if(address>0xBFFFFF){
        //WRITE_LONG(chipset.chipram,address,value);
        uint32_t* dest = (uint32_t*)&low16Meg[address];
        value = ((value << 8) & 0xFF00FF00 ) | ((value >> 8) & 0xFF00FF );
        value = value << 16 | value >> 16;
        *dest = value; return;
    }
    
    //CIA A
    if(address>=0xBFE001){
        return;
    }
    
    //CIA B
    if(address>=0xBFD000){
        return;
    }
    
    //24bit fast ram
    if(address>0x1FFFFF){
        //return;
        uint32_t* dest = (uint32_t*)&low16Meg[address];
        value = ((value << 8) & 0xFF00FF00 ) | ((value >> 8) & 0xFF00FF );
        value = value << 16 | value >> 16;
        *dest = value; return;
    }
    
    //RAM - Incomplete address decding means chip addresses below 2meg wrap around
    address &=CHIPTOP;
    
    //WRITE_LONG(chipset.chipram,address,value);
    uint32_t* dest = (uint32_t*)&low16Meg[address];
    value = ((value << 8) & 0xFF00FF00 ) | ((value >> 8) & 0xFF00FF );
    value = value << 16 | value >> 16;
    *dest = value; return;

}




