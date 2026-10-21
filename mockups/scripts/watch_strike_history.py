from PIL import Image, ImageDraw, ImageFont, ImageFilter
import math, random
F='/tmp/claude-0/-home-user-7amzala/ba326ba7-13b7-5834-81bf-91b86d9a13f8/scratchpad/fonts/Montserrat.ttf'
def font(sz,w=600):
    f=ImageFont.truetype(F,sz)
    try: f.set_variation_by_axes([w])
    except Exception: pass
    return f
S=2; W=466
RED=(255,77,94); CY=(34,211,255); VI=(155,140,255); OR=(255,186,82); TX=(234,243,255); DIM=(122,140,170); TRACK=(26,30,40); GRY=(52,58,74); PANEL=(12,16,26)
def s(v): return int(v*S)
def tc(d,y,t,f,c): w=d.textlength(t,font=f); d.text(((W*S-w)/2,s(y)),t,font=f,fill=c)
def pill(d,y,t,c,dot=True):
    f=font(s(15),700); w=d.textlength(t,font=f)+(s(18) if dot else 0)
    x0=(W*S-w)/2-s(14); d.rounded_rectangle([x0,s(y),x0+w+s(28),s(y+30)],radius=s(15),fill=tuple(int(v*0.2) for v in c),outline=tuple(int(v*0.6) for v in c),width=S)
    x=x0+s(14)
    if dot: d.ellipse([x,s(y+11),x+s(8),s(y+19)],fill=c); x+=s(18)
    d.text((x,s(y+6)),t,font=f,fill=c)
# fake strike recording: 10 s before + 45 s after, 10/s
random.seed(3); N=550; pre=100
k=[]; t=[]
for i in range(N):
    sec=(i-pre)/10
    base=0.6*math.sin(2*math.pi*i/70)
    kn=random.uniform(5,25)
    if -6<sec<-1 and random.random()<0.06: kn=random.uniform(110,200)
    tl=base
    if 0<=sec<14: tl=base+2.4+1.0*math.sin(2*math.pi*2*sec)*(0.6+0.4*math.sin(sec))+ (0.8 if sec<2 else 0); kn+= random.uniform(20,90)
    if 14<=sec<21: tl=base+2.4*(1-(sec-14)/7)
    if 21<=sec<26: tl=base-1.6
    if 26<=sec<30 and random.random()<0.1: kn=random.uniform(110,170)
    k.append(kn); t.append(tl)
def state(i):
    sec=(i-pre)/10
    if 0<=sec<14: return RED
    if 21<=sec<26: return VI
    if 26<=sec<30: return OR
    if -6<sec<-1: return OR
    return GRY
def screen(view):
    im=Image.new('RGB',(W*S,W*S),(0,0,0)); d=ImageDraw.Draw(im)
    # top
    pill(d,40,"STRIKE 3 OF 7",RED)
    tc(d,78,"SLOW PULL  -  12 min ago",font(s(15),600),TX)
    # graph panel
    px0,py0,pw,ph=56,108,354,140
    d.rounded_rectangle([s(px0),s(py0),s(px0+pw),s(py0+ph)],radius=s(18),fill=PANEL,outline=(30,36,50),width=S)
    if view=='close': a,b=pre-60,pre+140   # 20 s window
    else: a,b=0,N
    n=b-a; x=lambda i: px0+10+(i-a)*(pw-20)/(n-1)
    ky=lambda v: py0+10+(1-min(v,300)/300)*(ph/2-10)
    ty=lambda v: py0+10+(900-v*100)*(ph-20)/1200
    # knock area
    for i in range(a,b):
        X=x(i); d.line([(s(X),s(ky(k[i]))),(s(X),s(py0+ph/2))],fill=(20,90,120),width=max(1,int(S*(pw-20)/n)))
    d.line([(s(x(i)),s(ky(k[i]))) for i in range(a,b)],fill=CY,width=s(1.6))
    yl=ky(100); 
    for xx in range(px0+10,px0+pw-10,10): d.line([(s(xx),s(yl)),(s(xx+5),s(yl))],fill=(120,96,50),width=S)
    d.line([(s(x(i)),s(ty(t[i]))) for i in range(a,b)],fill=VI,width=s(2.4))
    # strike marker
    X=x(pre); 
    for yy in range(py0+14,py0+ph-8,9): d.line([(s(X),s(yy)),(s(X),s(yy+5))],fill=RED,width=s(2))
    d.ellipse([s(X-5),s(py0+5),s(X+5),s(py0+15)],fill=RED)
    # time labels
    f=font(s(11),600)
    marks=[-6,0,6,12] if view=='close' else [-10,0,10,20,30,40]
    for m in marks:
        i=pre+m*10
        if a<=i<b:
            lab="0:00" if m==0 else (f"{m:+d}s" if view=='close' else f"{m:+d}s")
            w=d.textlength(lab,font=f); d.text((s(x(i))-w/2,s(py0+ph+4)),lab,font=f,fill=RED if m==0 else DIM)
    # story bar: whole recording, window box in close view
    by=py0+ph+22; bx0=px0+10; bw=pw-20
    for i in range(0,N,5):
        X0=bx0+i*bw/N; X1=bx0+(i+5)*bw/N-1
        d.rectangle([s(X0),s(by),s(X1),s(by+7)],fill=state(i))
    if view=='close':
        X0=bx0+a*bw/N; X1=bx0+b*bw/N
        d.rounded_rectangle([s(X0-2),s(by-4),s(X1+2),s(by+11)],radius=s(4),outline=TX,width=S)
    # summary
    tc(d,by+20,"RUN 14 s  -  SLACK 0:21  -  quiet from 0:30",font(s(13),600),TX)
    # metrics
    f1=font(s(30),700); f2=font(s(12),600)
    for cx,val,unit,cap,c in ((-95,"+3.6","deg","TILT PEAK",VI),(0,"0:45","","LASTED",TX),(95,"100","%","PULL MAX",TX)):
        w=d.textlength(val,font=f1); uw=d.textlength(unit,font=f2) if unit else 0
        X=W*S/2+s(cx)-(w+uw+s(3))/2
        d.text((X,s(318)),val,font=f1,fill=c)
        if unit: d.text((X+w+s(3),s(334)),unit,font=f2,fill=c)
        cw=d.textlength(cap,font=f2); d.text((W*S/2+s(cx)-cw/2,s(356)),cap,font=f2,fill=DIM)
    # home button
    d.ellipse([s(233-26),s(380),s(233+26),s(432)],fill=(20,25,36),outline=CY,width=s(2))
    # house icon
    hx,hy=233,406
    d.polygon([(s(hx-11),s(hy-1)),(s(hx),s(hy-11)),(s(hx+11),s(hy-1))],fill=CY)
    d.rectangle([s(hx-7),s(hy-1),s(hx+7),s(hy+10)],fill=CY); d.rectangle([s(hx-2),s(hy+3),s(hx+2),s(hy+10)],fill=(20,25,36))
    hint="drag graph < >   -   tap: whole strike" if view=='close' else "tap: close-up (20 s, drag to scroll)"
    tc(d,440-2,hint,font(s(10),600),DIM)
    m=Image.new('L',(W*S,W*S),0); ImageDraw.Draw(m).ellipse([0,0,W*S-1,W*S-1],fill=255)
    bg=Image.new('RGB',(W*S,W*S),(22,22,26)); bg.paste(im,(0,0),m)
    return bg.resize((W,W),Image.LANCZOS)
sheet=Image.new('RGB',(1500,700),(22,22,26)); d=ImageDraw.Draw(sheet)
sheet.paste(screen('close'),(20,70)); sheet.paste(screen('whole'),(510,70))
T=lambda x,y,t,sz=22,c=TX,w=700: d.text((x,y),t,font=font(sz,w),fill=c)
T(20,20,"1. Close-up: 20 s, drag left/right",24,CY); T(510,20,"2. Whole strike: all of it on one screen",24,CY)
T(20,560,"Box on the colour bar = the part you see. Red RUN, orange KNOCK, violet SLACK, grey quiet.",18,DIM,500)
# navigation map
nx=1010; T(nx,20,"3. Moving around",24,CY)
items=[("LIVE GRAPH (home)",CY),("Strike 7 (newest)",RED),("Strike 6",RED),("...",DIM),("Strike 1 (oldest)",RED)]
for i,(lab,c) in enumerate(items):
    y=90+i*90
    d.rounded_rectangle([nx,y,nx+300,y+52],radius=26,outline=c,width=3,fill=(14,18,28)); T(nx+24,y+13,lab,20,c)
    if i<len(items)-1:
        d.line([(nx+150,y+52),(nx+150,y+90)],fill=DIM,width=2)
        T(nx+165,y+56,"swipe down",15,DIM,500); T(nx+320,y+56,"",15)
T(nx,560,"Swipe up = back toward the live graph.",18,TX,600)
T(nx,588,"Home button = straight to the live graph.",18,TX,600)
T(nx,616,"Strike / pull / drop-back / knock = back home",18,OR,600)
T(nx,642,"by itself (knock: at most every 30 s).",18,OR,600)
sheet.save('/home/user/7amzala/mockups/watch_strike_history_mockup.png'); print("ok")
