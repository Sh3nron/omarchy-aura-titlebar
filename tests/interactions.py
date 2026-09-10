import argparse, json, os, subprocess, time
parser = argparse.ArgumentParser(description='Input regression test for an isolated 1280x720 Aura fixture compositor')
parser.add_argument('--instance', required=True)
parser.add_argument('--wayland', required=True)
parser.add_argument('--pointer', required=True, help='Compiled tests/virtual-pointer.c')
parser.add_argument('--output', required=True, help='Existing directory for test screenshots')
args = parser.parse_args()
sig = args.instance
assert sig != os.environ.get('HYPRLAND_INSTANCE_SIGNATURE'), 'Never test on the user desktop'
def ctl(*args):
    return subprocess.check_output(['hyprctl','-i',sig,*args],text=True)
assert all(m['name']=='HEADLESS-1' for m in json.loads(ctl('monitors','-j')))
env=dict(os.environ,WAYLAND_DISPLAY=args.wayland)
p=subprocess.Popen([args.pointer],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True,env=env)
def event(text, delay=.15):
    p.stdin.write(text+'\n');p.stdin.flush()
    assert p.stdout.readline().strip()=='ok'
    time.sleep(delay)
    if text.startswith('move '):
        print(text, 'actual cursor:', ctl('cursorpos').strip(), flush=True)
def window():
    return json.loads(ctl('clients','-j'))[0]
try:
    clients=json.loads(ctl('clients','-j'))
    assert len(clients)==1 and clients[0]['title']=='Aura progressive blur check', 'Only run against the synthetic fixture'
    ctl('eval','hl.device({name="wl_pointer",enabled=false})')
    ctl('dispatch','hl.dsp.window.float({action="disable"})')
    time.sleep(.6)
    # The compositor warps when the virtual device is first attached. Let that
    # settle before testing the titlebar's hover/press ordering.
    event('move 500 300',.6)
    before=window();x,y=before['at'];w,h=before['size']
    grab=(x+min(250,w//3),y+5)
    event(f'move {grab[0]} {grab[1]}',.6)
    event('button 1')
    assert window()['at']==before['at'] and window()['size']==before['size'], 'press alone changes geometry'
    event(f'move {grab[0]+2} {grab[1]+1}')
    assert not window()['floating'], 'tiny pointer jitter detached the window'
    event(f'move {grab[0]+30} {grab[1]+40}',.6)
    dragging=window()
    print('before:',before['at'],before['size'],'mid-drag:',dragging['at'],dragging['size'],flush=True)
    assert dragging['floating'], 'native tiled drag did not pick the window up'
    assert dragging['size']==before['size'], 'drag changed the remembered size'
    event(f'move {grab[0]+80} {grab[1]+70}',.5)
    event('button 0',.3)
    # The drag controller is tiling-aware: dropping back over the layout
    # re-tiles the window (swap semantics), so release must never leave a
    # tiled window floating.
    assert not window()['floating'], 'release unexpectedly stayed floating'
    # Use maximize as an observable button action scoped to this compositor.
    cmd=f'hyprctl -i {sig} dispatch '+"'hl.dsp.window.fullscreen({mode=\"maximized\",action=\"toggle\"})'"
    ctl('eval','hl.plugin.aura_titlebar.add_button({bg_color="#7aa2f7",fg_color="#ffffff",size=11,icon="□",action='+json.dumps(cmd)+'})')
    # Reposition the floating fixture to fit the monitor for pointer testing.
    ctl('dispatch','hl.dsp.window.fullscreen({mode="maximized",action="toggle"})')
    time.sleep(.6)
    current=window();x,y=current['at'];w,h=current['size']
    button=(min(1270,x+w-8-8),y+14)
    event(f'move {button[0]} {y+5}',.6)
    event(f'move {button[0]} {button[1]}',.5)
    subprocess.run(['grim','-o','HEADLESS-1',os.path.join(args.output,'button-hover.png')],env=env,check=True)
    fs=window()['fullscreen']
    event('button 1',.5)
    assert window()['fullscreen']==fs,'button acted on press'
    subprocess.run(['grim','-o','HEADLESS-1',os.path.join(args.output,'button-pressed.png')],env=env,check=True)
    event(f'move {button[0]-120} {button[1]+100}')
    event('button 0')
    assert window()['fullscreen']==fs,'release outside did not cancel'
    event(f'move {button[0]} {y+5}',.6)
    event(f'move {button[0]} {button[1]}')
    event('button 1');event('button 0',.6)
    assert window()['fullscreen']!=fs,'release on button did not activate'
    print('PASS: tiled drag picks up, release re-tiles; click jitter; press/release/cancel')
    ctl('dispatch','hl.dsp.window.fullscreen({mode="fullscreen",action="toggle"})')
    time.sleep(.7)
    before=window();x,y=before['at'];w,h=before['size']
    event('move 250 100');event('move 250 5',.6);event('button 1')
    event('move 290 45',.6)
    after=window()
    print('fullscreen:',before['at'],before['size'],'detached:',after['at'],after['size'],flush=True)
    assert after['floating'] and after['fullscreen']==0
    assert after['size']==before['size'] and after['at']==[x+40,y+40], 'fullscreen detach changed size or anchor'
    event('button 0')
    print('PASS: fullscreen detach preserves geometry and stays floating')
finally:
    p.stdin.close();p.wait(timeout=5)
