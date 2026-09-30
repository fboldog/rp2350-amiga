# From a reset that mounts Workbench 1.3, poll the displayed frame over SWD
# until the Workbench icon screen appears (colour 0 body, colour 1 title
# text) and print the elapsed time.
# usage: boot.py <elf> <rows 256|240> [limit seconds]
import subprocess,sys,time,tempfile,os
E=sys.argv[1]; H=int(sys.argv[2]); limit=float(sys.argv[3]) if len(sys.argv)>3 else 200
def sym(n): return int([l.split()[0] for l in subprocess.run(['arm-none-eabi-nm',E],capture_output=True,text=True).stdout.splitlines() if l.endswith(' '+n)][0],16)
F,D=sym('sram_frames'),sym('frame_state')
dump=os.path.join(tempfile.gettempdir(),'omega-boot-poll.bin')
oc=['openocd','-s','/usr/share/openocd/scripts','-f','interface/cmsis-dap.cfg','-f','target/rp2350.cfg','-c','adapter speed 15000','-c','init']
t0=time.time(); w=640
while time.time()-t0<limit:
    out=subprocess.run(oc+['-c',f'echo disp:[capture {{mdw 0x{D:x} 1}}]','-c',f'dump_image {dump} 0x{F:x} {2*w*H}','-c','exit'],capture_output=True,text=True).stderr
    try: i=int([l for l in out.splitlines() if l.startswith('disp:')][0].split()[1],16)&1
    except Exception: continue
    f=open(dump,'rb').read()[i*w*H:(i+1)*w*H]
    body=all(set(f[y*w:y*w+500])=={0} for y in range(20,191,10))
    if body and 1 in f[3*w:4*w]:
        print('Workbench after %.1f s'%(time.time()-t0)); break
else: print('not reached in %d s'%limit)
