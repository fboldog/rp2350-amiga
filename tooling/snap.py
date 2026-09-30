# Consistent HDMI frame capture: set hostCaptureHold, dump the displayed
# frame + record, clear the hold, replay scanout to PNG (sprites = 0x40).
# usage: snap.py <elf> <rows 256|240> <out prefix>
# Needs a build with hostCaptureHold (src/Host.c). Writes <out>.png.
import subprocess,sys,struct
from PIL import Image
E,H,out=sys.argv[1],int(sys.argv[2]),sys.argv[3]
def gdb(x): return int(subprocess.run(['arm-none-eabi-gdb','-batch','-ex',f'p/x {x}',E],capture_output=True,text=True).stdout.split()[-1],16)
F=gdb('(int)&sram_frames'); D=gdb('(int)&frame_state'); HOLD=gdb('(int)&hostCaptureHold'); RS=gdb('sizeof(dvi_indexed_frame_t)'); R=0x11780000
off={n:gdb(f'(int)&((dvi_indexed_frame_t*)0)->{n}') for n in ['initial_palette','row_segment','segments_used','segment','palette_log']}
oc=['openocd','-s','/usr/share/openocd/scripts','-f','interface/cmsis-dap.cfg','-f','target/rp2350.cfg','-c','adapter speed 15000','-c','init']
o=subprocess.run(oc+['-c',f'mww 0x{HOLD:x} 1','-c','sleep 100','-c',f'echo st:[capture {{mdw 0x{D:x} 1}}]','-c','exit'],capture_output=True,text=True).stderr
b=int([l for l in o.splitlines() if l.startswith('st:')][0].split(': ')[1],16)&1
subprocess.run(oc+['-c',f'dump_image {out}.frame 0x{F+b*640*H:x} {640*H}','-c',f'dump_image {out}.rec 0x{R+b*RS:x} {RS}','-c',f'mww 0x{HOLD:x} 0','-c','exit'],capture_output=True,text=True)
pix=open(out+'.frame','rb').read(); rec=open(out+'.rec','rb').read()
u16=lambda o,n: struct.unpack_from(f'<{n}H',rec,o); u32=lambda o,n: struct.unpack_from(f'<{n}I',rec,o)
init=u16(off['initial_palette'],32); rows=u16(off['row_segment'],257); used,logn=u16(off['segments_used'],2)
segs=u32(off['segment'],used); log=u32(off['palette_log'],min(logn,2047))
c8=lambda c:(((c>>8)&15)*17,((c>>4)&15)*17,(c&15)*17)
pal=[0]*64
def setr(r,c): pal[r]=c8(c); pal[r+32]=c8((c>>1)&0x777)
for i in range(32): setr(i,init[i])
applied=0; img=Image.new('RGB',(640,H)); px=img.load(); sprites=0
def apply(p):
    global applied
    while applied<min(p,len(log)): e=log[applied]; applied+=1; setr(e>>12,e&0xfff)
for y in range(H):
    f,l=min(rows[y],used),min(rows[y+1],used); row=pix[y*640:(y+1)*640]
    if f==l:
        for x in range(640): px[x,y]=pal[0]
        continue
    apply((segs[f]>>20)&0x7ff); border=pal[0]
    mn=min(s&0x3ff for s in segs[f:l]); mx=max((s>>10)&0x3ff for s in segs[f:l])
    for x in list(range(0,mn))+list(range(mx,640)): px[x,y]=border
    hold=border
    for s in segs[f:l]:
        apply((s>>20)&0x7ff); x0,x1,ham=s&0x3ff,(s>>10)&0x3ff,s>>31
        for x in range(x0,x1):
            c=row[x]
            if c&0x40: px[x,y]=pal[c&31]; sprites+=1; continue
            if ham:
                v=(c&15)*17; k=(c>>4)&3
                if k==0: hold=pal[c&15]
                elif k==1: hold=(hold[0],hold[1],v)
                elif k==2: hold=(v,hold[1],hold[2])
                else: hold=(hold[0],v,hold[2])
                px[x,y]=hold
            else: px[x,y]=pal[c&63]
print(f'buffer {b}: segments {used}, log {logn}, sprite pixels {sprites}')
img.resize((640,H*2)).save(out+'.png')
