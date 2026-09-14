"""Bounded training-room test: read target pixels and calibrate Android swipes.

Default is preview only. --test-input sends at most two calibration swipes and
four corrections, never fires, and locks one target for the whole test. This
host diagnostic is not the module's Android input adapter.
"""
import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import subprocess
import time
import frida

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--metadata',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--width',type=int,required=True)
    parser.add_argument('--height',type=int,required=True)
    parser.add_argument('--test-input',action='store_true')
    args=parser.parse_args()
    data=json.loads(args.metadata.read_text(encoding='utf-8'))
    adb=next(Path('C:/Users/Admin/AppData/Local/Microsoft/WinGet/Packages').glob('Google.PlatformTools_*/platform-tools/adb.exe'))
    command=[str(adb),'-s','192.168.5.102:5555']
    report={'startedAt':datetime.now(timezone.utc).isoformat(),'pid':data['pid'],'steps':[],'inputEnabled':args.test_input}
    session=frida.get_device_manager().add_remote_device('127.0.0.1:27043').attach(data['pid'])
    try:
        script=session.create_script((ROOT/'tools/unity_profile.js').read_text(encoding='utf-8')+'\n'+
                                     (ROOT/'tools/aim_snapshot.js').read_text(encoding='utf-8'))
        script.load()
        def sample():
            for _ in range(40):
                start=time.monotonic()
                value=script.exports_sync.snapshot(data,args.width,args.height)
                if value.get('ready') and time.monotonic()-start<0.25:return value
                time.sleep(0.02)
            raise RuntimeError('No stable snapshot: '+value.get('reason','transport delay'))
        initial=sample()
        report['initial']=initial
        if not initial['candidates'] or initial['candidates'][0]['distance']>200:
            raise RuntimeError('No candidate within 200 pixels')
        chosen=initial['candidates'][0]['address']
        def current():
            state=sample()
            target=next((t for t in state['candidates'] if t['address']==chosen),None)
            if target is None or target['distance']>500:raise RuntimeError('Locked target lost')
            if math.dist(state['cameraPosition'],initial['cameraPosition'])>0.1:raise RuntimeError('Player moved during calibration')
            if abs(state['fovY']-initial['fovY'])>0.1:raise RuntimeError('Zoom changed during calibration')
            return state,target
        print('Target',chosen,'error',initial['candidates'][0]['error'],'FOV',initial['fovY'],flush=True)
        if args.test_input:
            def swipe(dx,dy):
                dx,dy=round(dx),round(dy)
                if max(abs(dx),abs(dy))>40:raise RuntimeError('Swipe exceeds test bound')
                before,target_before=current()
                x,y=round(args.width*0.66),round(args.height*0.4)
                subprocess.run(command+['shell','input','touchscreen','swipe',str(x),str(y),str(x+dx),str(y+dy),'120'],check=True,timeout=5)
                time.sleep(0.18)
                after,target_after=current()
                record={'drag':[dx,dy],'before':target_before,'after':target_after,
                        'frameBefore':before['frame'],'frameAfter':after['frame']}
                report['steps'].append(record)
                print('Drag',record['drag'],'residual',target_after['error'],flush=True)
                return [target_after['screen'][i]-target_before['screen'][i] for i in range(2)]
            calibration=32
            dx=swipe(calibration,0);dy=swipe(0,calibration)
            a,c=dx[0]/calibration,dx[1]/calibration;b,d=dy[0]/calibration,dy[1]/calibration
            det=a*d-b*c
            norm=a*a+b*b+c*c+d*d
            if abs(det)<0.01 or norm/max(abs(det),1e-12)>10 or max(abs(a),abs(b),abs(c),abs(d))>100:
                raise RuntimeError('Input calibration is too weak or inconsistent')
            report['jacobian']=[[a,b],[c,d]]
            for _ in range(4):
                _,target=current();ex,ey=target['error']
                if math.hypot(ex,ey)<8:break
                sx=0.8*(-d*ex+b*ey)/det;sy=0.8*(c*ex-a*ey)/det
                scale=min(1.,40/max(abs(sx),abs(sy),1.))
                sx,sy=round(sx*scale),round(sy*scale)
                if sx==0 and sy==0:break
                response=swipe(sx,sy)
                # Blend the observed response into the local calibration. Small
                # strokes are too sensitive to integer rounding to fit again.
                length=sx*sx+sy*sy
                if length>=64 and math.hypot(*response)>1:
                    rx=response[0]-(a*sx+b*sy);ry=response[1]-(c*sx+d*sy)
                    na,nb=a+0.5*rx*sx/length,b+0.5*rx*sy/length
                    nc,nd=c+0.5*ry*sx/length,d+0.5*ry*sy/length
                    ndet=na*nd-nb*nc
                    if abs(ndet)>0.01 and (na*na+nb*nb+nc*nc+nd*nd)/abs(ndet)<=10:
                        a,b,c,d,det=na,nb,nc,nd,ndet
            report['final'],target=current()
            report['finalErrorPixels']=target['distance']
            report['converged']=target['distance']<8
            print('Final error:',target['distance'],'pixels; converged:',report['converged'],flush=True)
        else:report['finalErrorPixels']=initial['candidates'][0]['distance']
    except Exception as error:
        report['error']=str(error)
        raise
    finally:
        session.detach()
        args.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('Saved:',args.output,flush=True)

if __name__=='__main__':main()
