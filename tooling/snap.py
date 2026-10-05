# Consistent HDMI frame capture: set hostCaptureHold, dump the displayed
# frame + record, clear the hold, replay scanout to PNG (sprites = 0x40).
# While interlaced fields are woven (weave_active), both frames are dumped
# and their rows interleaved, as the scanout sends them.
# usage: snap.py <elf> <rows 256|240> <out prefix>
# Needs a build with hostCaptureHold (src/Host.c). Writes <out>.png.
import subprocess,sys,struct
from PIL import Image
E,H,out=sys.argv[1],int(sys.argv[2]),sys.argv[3]
def gdb(x): return int(subprocess.run(['arm-none-eabi-gdb','-batch','-ex',f'p/x {x}',E],capture_output=True,text=True).stdout.split()[-1],16)
def has(sym): return any(l.endswith(' '+sym) for l in subprocess.run(['arm-none-eabi-nm',E],capture_output=True,text=True).stdout.splitlines())
F=gdb('(int)&sram_frames'); D=gdb('(int)&frame_state'); HOLD=gdb('(int)&hostCaptureHold'); RS=gdb('sizeof(dvi_indexed_frame_t)'); R=0x117A8000
W=gdb('(int)&weave_active') if has('weave_active') else None
FR=gdb('(int)&frame_rec') if has('frame_rec') else None  # record shown per buffer
off={n:gdb(f'(int)&((dvi_indexed_frame_t*)0)->{n}') for n in ['initial_palette','row_segment','row_log','segments_used','segment','palette_log']}
oc=['openocd','-s','/usr/share/openocd/scripts','-f','interface/cmsis-dap.cfg','-f','target/rp2350.cfg','-c','adapter speed 15000','-c','init']
cmds=['-c',f'mww 0x{HOLD:x} 1','-c','sleep 100','-c',f'echo st:[capture {{mdw 0x{D:x} 1}}]']
if W is not None: cmds+=['-c',f'echo wv:[capture {{mdb 0x{W:x} 1}}]']
if FR is not None: cmds+=['-c',f'echo fr:[capture {{mdh 0x{FR:x} 1}}]']
o=subprocess.run(oc+cmds+['-c','exit'],capture_output=True,text=True).stderr
val=lambda tag: int([l for l in o.splitlines() if l.startswith(tag)][0].split(': ')[1],16)
weave=W is not None and val('wv:')&1
frames=[0,1] if weave else [val('st:')&1]
recs=[(val('fr:')>>(8*b))&0xff for b in (0,1)] if FR is not None else [0,1]
dump=[]
for b in frames: dump+=['-c',f'dump_image {out}.frame{b} 0x{F+b*640*H:x} {640*H}','-c',f'dump_image {out}.rec{b} 0x{R+recs[b]*RS:x} {RS}']
subprocess.run(oc+dump+['-c',f'mww 0x{HOLD:x} 0','-c','exit'],capture_output=True,text=True)
c8=lambda c:(((c>>8)&15)*17,((c>>4)&15)*17,(c&15)*17)

def render(b):
    pix=open(f'{out}.frame{b}','rb').read(); rec=open(f'{out}.rec{b}','rb').read()
    u16=lambda o,n: struct.unpack_from(f'<{n}H',rec,o); u32=lambda o,n: struct.unpack_from(f'<{n}I',rec,o)
    init=u16(off['initial_palette'],32); rows=u16(off['row_segment'],257); rlog=u16(off['row_log'],256); used,logn=u16(off['segments_used'],2)
    segs=u32(off['segment'],used); log=u32(off['palette_log'],min(logn,4095))
    pal=[0]*64
    def setr(r,c): pal[r]=c8(c); pal[r+32]=c8((c>>1)&0x777)
    for i in range(32): setr(i,init[i])
    applied=[0]; img=Image.new('RGB',(640,H)); px=img.load(); sprites=0
    def apply(p):
        while applied[0]<min(p,len(log)): e=log[applied[0]]; applied[0]+=1; setr(e>>12,e&0xfff)
    for y in range(H):
        f,l=min(rows[y],used),min(rows[y+1],used); row=pix[y*640:(y+1)*640]
        if f==l:  # no runs: COLOR00 as the row's line ended
            apply(min(rlog[y],logn))
            for x in range(640): px[x,y]=pal[0]
            continue
        apply((segs[f]>>18)&0x1fff); border=pal[0]
        mn=min((s&0x1ff)<<1 for s in segs[f:l]); mx=max(((s>>9)&0x1ff)<<1 for s in segs[f:l])
        for x in range(0,mn): px[x,y]=border
        hold=border
        for s in segs[f:l]:
            apply((s>>18)&0x1fff); x0,x1,ham=(s&0x1ff)<<1,((s>>9)&0x1ff)<<1,s>>31
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
        for x in range(mx,640): px[x,y]=pal[0]  # right of the runs: COLOR00 as they end
    print(f'buffer {b}: segments {used}, log {logn}, sprite pixels {sprites}')
    return img

if weave:
    a,b=render(0),render(1)
    img=Image.new('RGB',(640,2*H))
    for y in range(H):
        img.paste(a.crop((0,y,640,y+1)),(0,2*y)); img.paste(b.crop((0,y,640,y+1)),(0,2*y+1))
    print('woven interlaced fields')
    img.save(out+'.png')
else:
    render(frames[0]).resize((640,H*2)).save(out+'.png')
