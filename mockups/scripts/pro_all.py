from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops
import math
FONT='/tmp/claude-0/-home-user-7amzala/ba326ba7-13b7-5834-81bf-91b86d9a13f8/scratchpad/fonts/Montserrat.ttf'
SS=3; S=466; W=S*SS
def font(px, weight='Medium'):
    f=ImageFont.truetype(FONT, int(px*SS)); f.set_variation_by_name(weight); return f
def s(v): return int(v*SS)
RED=(255,77,94); CY=(34,211,255); VI=(155,140,255); OR=(255,186,82); TX=(234,243,255); DIM=(122,140,170); TRACK=(26,30,40); GR=(61,220,132); BLUE=(30,123,255)

def glow(base, layer, radius=10, strength=1.0):
    g=layer.filter(ImageFilter.GaussianBlur(s(radius)))
    if strength!=1.0: g=Image.eval(g, lambda v: int(min(255, v*strength)))
    base.alpha_composite(g); base.alpha_composite(layer)
def text_c(d, y, t, f, col, cx=S/2):
    w=d.textlength(t, font=f); d.text((s(cx)-w/2, s(y)), t, font=f, fill=col)
def background(tint=(18,24,40)):
    im=Image.new('RGBA',(W,W),(0,0,0,255)); g=Image.new('RGBA',(W,W),(0,0,0,0)); d=ImageDraw.Draw(g)
    d.ellipse([s(60),s(40),s(406),s(386)], fill=tint+(255,)); g=g.filter(ImageFilter.GaussianBlur(s(70))); im.alpha_composite(g); return im
def arc(im, start, end, width, col, r=S/2-14, glow_r=0):
    L=Image.new('RGBA',(W,W),(0,0,0,0)); d=ImageDraw.Draw(L); c=W/2; R=s(r)
    if end-start>0.5:
        d.arc([c-R,c-R,c+R,c+R], start, end, fill=col+(255,), width=s(width))
        for a in (start,end):
            x=c+(R-s(width)/2)*math.cos(math.radians(a)); y=c+(R-s(width)/2)*math.sin(math.radians(a))
            d.ellipse([x-s(width)/2,y-s(width)/2,x+s(width)/2,y+s(width)/2], fill=col+(255,))
    if glow_r: glow(im,L,glow_r)
    else: im.alpha_composite(L)
def arc_at(im, cx, cy, r, start, end, width, col, glow_r=0):
    L=Image.new('RGBA',(W,W),(0,0,0,0)); d=ImageDraw.Draw(L)
    d.arc([s(cx-r),s(cy-r),s(cx+r),s(cy+r)], start, end, fill=col+(255,), width=s(width))
    for a in (start,end):
        x=s(cx)+(s(r)-s(width)/2)*math.cos(math.radians(a)); y=s(cy)+(s(r)-s(width)/2)*math.sin(math.radians(a))
        d.ellipse([x-s(width)/2,y-s(width)/2,x+s(width)/2,y+s(width)/2], fill=col+(255,))
    if glow_r: glow(im,L,glow_r)
    else: im.alpha_composite(L)
def gauge(im, frac, col, track=True, width=12):
    st,en=135,405
    if track: arc(im,st,en,width,TRACK)
    if frac>0: arc(im,st,st+(en-st)*frac,width,col,glow_r=9)
def pill(im, y, label, col, dot=True):
    d=ImageDraw.Draw(im); f=font(15,'Bold'); w=d.textlength(label,font=f)/SS
    extra=25 if dot else 12
    px0=S/2-(w+extra+12)/2; px1=S/2+(w+extra+12)/2
    P=Image.new('RGBA',(W,W),(0,0,0,0)); pd=ImageDraw.Draw(P)
    pd.rounded_rectangle([s(px0),s(y),s(px1),s(y+28)], radius=s(14), fill=col+(46,), outline=col+(160,), width=s(1))
    im.alpha_composite(P); d=ImageDraw.Draw(im)
    if dot: d.ellipse([s(px0+11),s(y+10),s(px0+19),s(y+18)], fill=col+(255,))
    d.text((s(px0+(25 if dot else 12)), s(y+5)), label, font=f, fill=col+(255,))
    return px1
def topbar(im, bat="78%"):
    d=ImageDraw.Draw(im)
    for i,h in enumerate([5,8,11,14]):
        d.rounded_rectangle([s(196+i*6),s(52-h),s(200+i*6),s(52)], radius=s(1), fill=(CY if i<3 else (52,58,74))+(255,))
    d.text((s(226),s(38)), bat, font=font(14,'SemiBold'), fill=DIM+(255,))
def metric(im, cx, y, val, unit, lab, col):
    d=ImageDraw.Draw(im); fv=font(34,'SemiBold'); fu=font(15,'SemiBold')
    wv=d.textlength(val,font=fv)/SS; wu=d.textlength(unit,font=fu)/SS; x=cx-(wv+wu+3)/2
    d.text((s(x),s(y)), val, font=fv, fill=col+(255,)); d.text((s(x+wv+3),s(y+16)), unit, font=fu, fill=col+(200,))
    text_c(d, y+42, lab, font(10,'SemiBold'), DIM+(255,), cx)
def chart(im, box, kfun, tfun, strike=None, story=None, n=240):
    x0,y0,x1,y1=box
    P=Image.new('RGBA',(W,W),(0,0,0,0)); d=ImageDraw.Draw(P)
    d.rounded_rectangle([s(x0),s(y0),s(x1),s(y1)], radius=s(18), fill=(12,16,26,255), outline=(30,36,50,255), width=s(1)); im.alpha_composite(P)
    ix0,ix1=x0+12,x1-12; ky0,ky1=y0+14,y0+(y1-y0)*0.58; ty0,ty1=y0+(y1-y0)*0.55,y1-12
    A=Image.new('RGBA',(W,W),(0,0,0,0)); d=ImageDraw.Draw(A)
    pts=[(s(ix0+(ix1-ix0)*i/(n-1)), s(ky1-(ky1-ky0)*kfun(i/(n-1)))) for i in range(n)]
    d.polygon(pts+[(s(ix1),s(ky1)),(s(ix0),s(ky1))], fill=CY+(255,))
    grad=Image.new('L',(1,W),0)
    for yy in range(W):
        t=(yy-s(ky0))/max(1,(s(ky1)-s(ky0))); grad.putpixel((0,yy), int(max(0,min(1,1-t))*150+15) if s(ky0)<=yy<=s(ky1) else 0)
    grad=grad.resize((W,W)); A.putalpha(ImageChops.multiply(A.getchannel('A'), grad)); im.alpha_composite(A)
    L=Image.new('RGBA',(W,W),(0,0,0,0)); ImageDraw.Draw(L).line(pts, fill=CY+(255,), width=s(2), joint='curve'); glow(im,L,4,0.8)
    d=ImageDraw.Draw(im); yl=s(ky1-(ky1-ky0)*0.40); x=s(ix0)
    while x<s(ix1): d.line([(x,yl),(min(x+s(6),s(ix1)),yl)], fill=(255,186,82,120), width=s(1)); x+=s(11)
    T=Image.new('RGBA',(W,W),(0,0,0,0))
    tp=[(s(ix0+(ix1-ix0)*i/(n-1)), s(ty1-(ty1-ty0)*tfun(i/(n-1)))) for i in range(n)]
    ImageDraw.Draw(T).line(tp, fill=VI+(255,), width=s(4), joint='curve'); glow(im,T,8,1.2)
    ex,ey=tp[-1]; D=Image.new('RGBA',(W,W),(0,0,0,0)); ImageDraw.Draw(D).ellipse([ex-s(6),ey-s(6),ex+s(6),ey+s(6)], fill=VI+(255,)); glow(im,D,8,1.5)
    d=ImageDraw.Draw(im); d.ellipse([ex-s(3),ey-s(3),ex+s(3),ey+s(3)], fill=(255,255,255,255))
    if strike is not None:
        sx=s(ix0+(ix1-ix0)*strike); yy=s(y0+8)
        while yy<s(y1-8): d.line([(sx,yy),(sx,min(yy+s(5),s(y1-8)))], fill=RED+(230,), width=s(2)); yy+=s(9)
        d.ellipse([sx-s(5),s(y0+4),sx+s(5),s(y0+14)], fill=RED+(255,))
    if story:
        cy0=s(y1+10); h=s(6); M=Image.new('RGBA',(W,W),(0,0,0,0)); md=ImageDraw.Draw(M)
        for a,b,c in story: md.rectangle([s(x0+10)+(s(x1-10)-s(x0+10))*a, cy0, s(x0+10)+(s(x1-10)-s(x0+10))*b-s(2), cy0+h], fill=c+(255,))
        mask=Image.new('L',(W,W),0); ImageDraw.Draw(mask).rounded_rectangle([s(x0+10),cy0,s(x1-10),cy0+h], radius=h//2, fill=255)
        M.putalpha(ImageChops.multiply(M.getchannel('A'),mask)); im.alpha_composite(M)
# icons
def icon_eye(d,cx,cy,col):
    R=26; d.arc([s(cx-R),s(cy-9),s(cx+R),s(cy-9+2*R)],225,315,fill=col,width=s(3)); d.arc([s(cx-R),s(cy+9-2*R),s(cx+R),s(cy+9)],45,135,fill=col,width=s(3)); d.ellipse([s(cx-6),s(cy-6),s(cx+6),s(cy+6)],fill=col)
def icon_reel(d,cx,cy,col):
    d.ellipse([s(cx-14),s(cy-14),s(cx+14),s(cy+14)],outline=col,width=s(3)); d.ellipse([s(cx-4),s(cy-4),s(cx+4),s(cy+4)],fill=col)
    d.line([(s(cx),s(cy)),(s(cx+16),s(cy-14))],fill=col,width=s(3)); d.ellipse([s(cx+13),s(cy-18),s(cx+21),s(cy-10)],fill=col)
def icon_play(d,cx,cy,col): d.polygon([(s(cx-9),s(cy-13)),(s(cx-9),s(cy+13)),(s(cx+14),s(cy))],fill=col)
def icon_skip(d,cx,cy,col):
    d.polygon([(s(cx-12),s(cy-10)),(s(cx-12),s(cy+10)),(s(cx+3),s(cy))],fill=col); d.rectangle([s(cx+5),s(cy-10),s(cx+9),s(cy+10)],fill=col)
def icon_wave(d,cx,cy,col,w=3):
    pts=[(s(cx-18+i),s(cy+5*math.sin(i/36*2*math.pi*1.5))) for i in range(37)]; d.line(pts,fill=col,width=s(w))
def icon_wind(d,cx,cy,col):
    for i,(L,dy) in enumerate([(30,-8),(22,0),(26,8)]):
        d.line([(s(cx-15),s(cy+dy)),(s(cx-15+L),s(cy+dy))],fill=col,width=s(3))
def icon_sea(d,cx,cy,col):
    icon_wave(d,cx,cy-6,col); icon_wave(d,cx,cy+6,col)
def icon_compass(d,cx,cy,col):
    d.ellipse([s(cx-14),s(cy-14),s(cx+14),s(cy+14)],outline=col,width=s(3)); d.polygon([(s(cx),s(cy-9)),(s(cx+5),s(cy+2)),(s(cx-5),s(cy+2))],fill=col)
def icon_look(d,cx,cy,col):
    d.rounded_rectangle([s(cx-10),s(cy-13),s(cx+10),s(cy+13)],radius=s(5),outline=col,width=s(3)); d.line([(s(cx-15),s(cy-4)),(s(cx-19),s(cy-8))],fill=col,width=s(2)); d.line([(s(cx+15),s(cy-4)),(s(cx+19),s(cy-8))],fill=col,width=s(2))
def rbutton(im,cx,cy,r,col,icon,label,filled=False):
    B=Image.new('RGBA',(W,W),(0,0,0,0)); d=ImageDraw.Draw(B)
    d.ellipse([s(cx-r),s(cy-r),s(cx+r),s(cy+r)], fill=(col+(255,)) if filled else (20,25,36,255), outline=col+(255,), width=s(2))
    if filled: glow(im,B,10,0.9)
    else: im.alpha_composite(B)
    d=ImageDraw.Draw(im); icon(d,cx,cy-1,((8,18,30,255) if filled else col+(255,)))
    if label: text_c(d, cy+r+5, label, font(10,'SemiBold'), DIM+(255,), cx)
def dots(im,i):
    d=ImageDraw.Draw(im)
    for k in range(3):
        x=S/2-14+k*14; c=TX if k==i else (58,66,84)
        d.ellipse([s(x-3),s(436-3),s(x+3),s(436+3)], fill=c+(255,))
def finish(im):
    out=im.resize((S,S), Image.LANCZOS); m=Image.new('L',(S,S),0); ImageDraw.Draw(m).ellipse([0,0,S-1,S-1],fill=255)
    r=Image.new('RGBA',(S,S),(0,0,0,0)); r.paste(out,(0,0),m); return r

calmk=lambda fr: 0.10+0.03*abs(math.sin(fr*90))
def knockk(fr): return 0.62+0.1*abs(math.sin(fr*260)) if 0.80<fr<0.84 else calmk(fr)
wavet=lambda fr: 0.30+0.06*math.sin(fr*12)

# 1 HOME armed
def home():
    im=background(); gauge(im,0.04,CY); topbar(im)
    px=pill(im,64,"ARMED",CY)
    metric(im,150,104,"+0.4","deg","TILT",VI); metric(im,316,104,"0","","KNOCKS",TX)
    chart(im,(56,172,410,298),knockk,wavet)
    d=ImageDraw.Draw(im); text_c(d,306,"waves small  ·  wind calm",font(13,'Medium'),DIM+(255,))
    rbutton(im,233,364,30,RED,icon_reel,"REEL IN"); dots(im,1); return finish(im)
# 2 HOME pull building
def building():
    im=background((40,28,10)); gauge(im,0.45,OR); topbar(im)
    pill(im,64,"SLOW PULL  45%",OR)
    metric(im,150,104,"+1.8","deg","TILT",VI); metric(im,316,104,"45","%","PULL",OR)
    chart(im,(56,172,410,298),calmk,lambda fr: 0.3 if fr<0.7 else 0.3+(fr-0.7)/0.3*0.45)
    d=ImageDraw.Draw(im); text_c(d,306,"building up ↑",font(13,'SemiBold'),OR+(255,))
    rbutton(im,233,364,30,RED,icon_reel,"REEL IN"); dots(im,1); return finish(im)
# 3 REELED IN
def reeled():
    im=background((10,24,48)); gauge(im,0,CY); topbar(im)
    pill(im,64,"REELED IN",DIM,dot=False)
    d=ImageDraw.Draw(im)
    text_c(d,118,"Cast,",font(30,'SemiBold'),TX+(255,)); text_c(d,156,"then tap READY",font(20,'Medium'),DIM+(255,))
    rbutton(im,233,272,56,CY,icon_play,None,filled=True)
    text_c(d,338,"READY",font(16,'Bold'),CY+(255,))
    text_c(d,372,"rod watches again after 8 s",font(12,'Medium'),DIM+(255,))
    return finish(im)
# 4 PULL TEST
def pulltest():
    im=background((40,28,10)); gauge(im,0.62,OR); topbar(im)
    pill(im,64,"PULL TEST",OR)
    d=ImageDraw.Draw(im)
    text_c(d,108,"1.2",font(54,'SemiBold'),TX+(255,)); text_c(d,170,"of 2.0 deg",font(15,'Medium'),DIM+(255,))
    # mini rod illustration
    d.line([(s(150),s(262)),(s(306),s(214))],fill=(120,130,150,255),width=s(4))
    d.line([(s(306),s(214)),(s(330),s(256))],fill=CY+(255,),width=s(2))
    d.polygon([(s(324),s(246)),(s(336),s(262)),(s(316),s(258))],fill=CY+(255,))
    text_c(d,276,"pull the line gently, then let go",font(13,'Medium'),DIM+(255,))
    rbutton(im,233,352,26,DIM,icon_skip,"SKIP"); return finish(im)
# 5 SEA STATE
def sea():
    im=background(); topbar(im)
    pill(im,64,"SEA STATE",CY,dot=False)
    for cx,lab,val,frac,col,ic in [(150,"WAVES","small",0.40,CY,icon_sea),(316,"WIND","calm",0.15,VI,icon_wind)]:
        arc_at(im,cx,214,58,135,405,10,TRACK); arc_at(im,cx,214,58,135,135+270*frac,10,col,glow_r=7)
        d=ImageDraw.Draw(im); ic(d,cx,206,col+(255,))
        text_c(d,226,val,font(18,'SemiBold'),TX+(255,),cx); text_c(d,282,lab,font(11,'SemiBold'),DIM+(255,),cx)
    d=ImageDraw.Draw(im); text_c(d,330,"measured by the rod",font(12,'Medium'),DIM+(255,))
    dots(im,2); return finish(im)
# 6 SETUP
def setup():
    im=background(); topbar(im)
    pill(im,64,"SETUP",OR,dot=False)
    rows=[(OR,icon_sea,"Relearn the sea","hold 2 s"),(OR,icon_compass,"New pull direction","hold 2 s"),(CY,icon_look,"Teach my look","tap")]
    for i,(c,ic,t,h) in enumerate(rows):
        y=112+i*76
        P=Image.new('RGBA',(W,W),(0,0,0,0)); pd=ImageDraw.Draw(P)
        pd.rounded_rectangle([s(76),s(y),s(390),s(y+62)],radius=s(20),fill=(20,25,36,255),outline=(36,42,58,255),width=s(1)); im.alpha_composite(P)
        B=Image.new('RGBA',(W,W),(0,0,0,0)); ImageDraw.Draw(B).ellipse([s(88),s(y+9),s(132),s(y+53)],fill=c+(46,)); im.alpha_composite(B)
        d=ImageDraw.Draw(im); ic(d,110,y+31,c+(255,))
        d.text((s(146),s(y+12)),t,font=font(16,'SemiBold'),fill=TX+(255,)); d.text((s(146),s(y+35)),h,font=font(12,'Medium'),fill=DIM+(255,))
        d.text((s(366),s(y+18)),"›",font=font(24,'Medium'),fill=DIM+(255,))
    dots(im,0); return finish(im)
# 7 BATTERY
def battery():
    im=background((12,36,24))
    arc_at(im,233,196,112,135,405,14,TRACK); arc_at(im,233,196,112,135,135+270*0.78,14,GR,glow_r=8)
    arc_at(im,233,196,88,135,405,14,TRACK); arc_at(im,233,196,88,135,135+270*0.41,14,OR,glow_r=8)
    d=ImageDraw.Draw(im)
    text_c(d,150,"9h 40m",font(30,'SemiBold'),TX+(255,)); text_c(d,190,"rod left",font(12,'Medium'),DIM+(255,))
    text_c(d,212,"3h 15m",font(20,'SemiBold'),OR+(255,)); text_c(d,240,"watch left",font(12,'Medium'),DIM+(255,))
    for cx,lab,pc,c in [(160,"ROD","78%",GR),(306,"WATCH","41%",OR)]:
        d.ellipse([s(cx-34),s(330),s(cx-26),s(338)],fill=c+(255,))
        d.text((s(cx-20),s(324)),pc,font=font(18,'SemiBold'),fill=TX+(255,)); d.text((s(cx-20),s(348)),lab,font=font(10,'SemiBold'),fill=DIM+(255,))
    return finish(im)
# 8 LAST STRIKE
def last():
    im=background((40,10,16)); topbar(im)
    pill(im,64,"LAST STRIKE",RED)
    d=ImageDraw.Draw(im); text_c(d,100,"SLOW PULL  ·  12 min ago",font(14,'SemiBold'),TX+(255,))
    def zk(fr):
        b=0.10+0.03*abs(math.sin(fr*130))
        if 0.47<fr<0.53: b=0.85-abs(fr-0.5)*18
        return b
    zt=lambda fr: 0.25 if fr<0.5 else 0.25+min(1,(fr-0.5)/0.4)*0.5
    chart(im,(56,128,410,262),zk,zt,strike=0.5,story=[(0,0.45,(52,58,74)),(0.45,0.55,OR),(0.55,1,RED)])
    d=ImageDraw.Draw(im); text_c(d,282,"ZOOM  2 s | 2 s   ·  tap for 30 s",font(12,'SemiBold'),DIM+(255,))
    metric(im,150,304,"+3.6","deg","TILT PEAK",VI); metric(im,316,304,"100","%","PULL",TX)
    text_c(d,374,"3 strikes since start",font(12,'Medium'),DIM+(255,))
    return finish(im)
# 9 LOOKING
def looking():
    im=background((10,24,48))
    for r,a in [(120,40),(90,80),(60,140)]:
        L=Image.new('RGBA',(W,W),(0,0,0,0)); ImageDraw.Draw(L).ellipse([s(233-r),s(200-r),s(233+r),s(200+r)],outline=BLUE+(a,),width=s(2)); im.alpha_composite(L)
    arc_at(im,233,200,120,250,330,4,CY,glow_r=8)
    B=Image.new('RGBA',(W,W),(0,0,0,0)); ImageDraw.Draw(B).ellipse([s(205),s(172),s(261),s(228)],fill=BLUE+(255,)); glow(im,B,12)
    d=ImageDraw.Draw(im); wh=(255,255,255,255); w=s(3)
    pts=[(226,189),(241,211),(233,219),(233,181),(241,189),(226,211)]
    d.line([(s(x),s(y)) for x,y in pts],fill=wh,width=w,joint='curve')
    text_c(d,334,"Looking for the rod",font(18,'SemiBold'),TX+(255,)); text_c(d,362,"scan 7  ·  12 devices",font(12,'Medium'),DIM+(255,))
    return finish(im)

screens=[(looking,"Looking for the rod"),(home,"Home - rod armed"),(building,"Home - slow pull building"),
         (reeled,"Reeled in"),(pulltest,"Pull test (first time)"),(sea,"Sea state (swipe left)"),
         (setup,"Setup (swipe right)"),(battery,"Battery (swipe up)"),(last,"Last strike (swipe down)")]
cols=3; rows=3
sheet=Image.new('RGBA',(cols*(S+30)+30, rows*(S+70)+30),(17,19,25,255)); dd=ImageDraw.Draw(sheet)
fl=ImageFont.truetype(FONT,22); fl.set_variation_by_name('SemiBold')
for i,(fn,cap) in enumerate(screens):
    x=30+(i%cols)*(S+30); y=30+(i//cols)*(S+70)
    sheet.alpha_composite(fn(),(x,y)); w=dd.textlength(cap,font=fl); dd.text((x+(S-w)/2,y+S+14),cap,font=fl,fill=(220,228,240))
sheet.convert('RGB').save('/home/user/7amzala/mockups/watch_pro_theme_all.png')
print("ok")
