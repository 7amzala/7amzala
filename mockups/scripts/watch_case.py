from PIL import Image, ImageDraw, ImageFont
F='/tmp/claude-0/-home-user-7amzala/ba326ba7-13b7-5834-81bf-91b86d9a13f8/scratchpad/fonts/Montserrat.ttf'
def font(sz,w=600):
    f=ImageFont.truetype(F,sz)
    try: f.set_variation_by_axes([w])
    except Exception: pass
    return f
W,H=2300,1750
im=Image.new('RGB',(W,H),(16,18,24)); d=ImageDraw.Draw(im)
TX=(234,243,255); DIM=(140,155,180); CY=(34,211,255); OR=(255,186,82); RED=(255,77,94); GR=(61,220,132); VI=(155,140,255)
CASE=(58,64,78); CASE2=(88,96,112); GLASS=(70,140,180); PCB=(30,120,70); BAT=(200,170,60); BZ=(150,90,200); STRAP=(40,44,52)
def T(x,y,s,sz=26,c=TX,w=600,anchor='la'): d.text((x,y),s,font=font(sz,w),fill=c,anchor=anchor)
T(40,30,"StrikeSense Watch - case design with 850 mAh battery",44,TX,700)
T(40,86,"Sizes are typical - measure your board (A, B) and battery before printing. Scale: 10 px = 1 mm",24,DIM,500)

# ---------- 1. cross-section ----------
ox,oy=60,200; S=10  # px per mm
T(ox,oy-50,"1. Side cut (what is inside, top to bottom)",30,CY,700)
x0=ox+150
layers=[  # name, thickness mm, colour, width mm
 ("Glass / bezel ring (case top)",1.5,GLASS,44),
 ("AMOLED + board (Waveshare 1.43)",6.5,PCB,40),
 ("Foam pad 1 mm",1.0,(90,90,90),38),
 ("Battery 850 mAh  (803035: 8 x 30 x 35 mm)",8.0,BAT,35),
 ("Back cover + O-ring",2.0,CASE2,46),
]
y=oy+20
# case walls
tot=sum(l[1] for l in layers)
d.rounded_rectangle([x0-30,y-10,x0+46*S+30,y+tot*S+10],radius=40,outline=CASE2,width=6)
for name,t,c,w in layers:
    xa=x0+(46-w)/2*S
    d.rounded_rectangle([xa,y,xa+w*S,y+t*S],radius=8,fill=c)
    ly=oy+20+layers.index((name,t,c,w))*62+10
    d.line([(xa+w*S,y+t*S/2),(x0+46*S+70,ly+12)],fill=(90,98,115),width=2)
    T(x0+46*S+80,ly+12,f"{name}  -  {t:g} mm",22,TX,500,'lm')
    y+=t*S
# haptic beside battery
by=oy+20+(1.5+6.5+1.0)*S
d.rounded_rectangle([x0+36.5*S+30,by+5,x0+44*S+30,by+5+5*S],radius=6,fill=BZ)
T(x0+40*S+30,by+5+2.5*S,"Bzzz",18,TX,700,'mm')
# thickness arrow
d.line([(x0-60,oy+20),(x0-60,oy+20+tot*S)],fill=OR,width=3)
T(x0-70,oy+20+tot*S/2,f"{tot:.0f} mm",24,OR,700,'rm')
y=oy+20+5*62; T(ox,y+40,"Total about 19 mm thick (a rugged G-Shock style watch is 16-18 mm).",22,DIM,500)
T(ox,y+70,"Thinner option: 2 x 502530 in parallel (5 mm, ~2 x 400 mAh) -> about 16 mm.",22,GR,600)
T(ox,y+100,"Haptic Bzzz sits beside the battery (5 mm), wires go round the edge.",22,DIM,500)

# ---------- 2. top view ----------
cx,cy=1780,760; R=24*S
T(1330,200,"2. Top view (real size x10)",30,CY,700)
# strap
d.rounded_rectangle([cx-11*S,cy-R-150,cx+11*S,cy-R+20],radius=30,fill=STRAP)
d.rounded_rectangle([cx-11*S,cy+R-20,cx+11*S,cy+R+150],radius=30,fill=STRAP)
for i in range(3): d.ellipse([cx-8,cy+R+60+i*30-8,cx+8,cy+R+60+i*30+8],fill=(25,28,34))
# lugs
for sy in (-1,1):
    d.rounded_rectangle([cx-13*S,cy+sy*(R-10)-25,cx+13*S,cy+sy*(R-10)+25],radius=12,fill=CASE)
# case
d.ellipse([cx-R,cy-R,cx+R,cy+R],fill=CASE)
# bezel with marks
d.ellipse([cx-R+18,cy-R+18,cx+R-18,cy+R-18],fill=CASE2)
import math
for k in range(60):
    a=math.radians(k*6); r1=R-22; r2=R-(34 if k%5==0 else 28)
    d.line([(cx+r1*math.sin(a),cy-r1*math.cos(a)),(cx+r2*math.sin(a),cy-r2*math.cos(a))],fill=(200,205,215) if k%5==0 else (120,128,140),width=3 if k%5==0 else 1)
# screen
rs=18.5*S
d.ellipse([cx-rs,cy-rs,cx+rs,cy+rs],fill=(0,0,0))
d.arc([cx-rs+8,cy-rs+8,cx+rs-8,cy+rs-8],135,300,fill=OR,width=8)
d.arc([cx-rs+8,cy-rs+8,cx+rs-8,cy+rs-8],300,405,fill=(30,34,44),width=8)
T(cx,cy-40,"SLOW PULL",26,OR,700,'mm'); T(cx,cy+10,"+2.5 deg",40,TX,700,'mm')
# side buttons
d.rounded_rectangle([cx+R-6,cy-70,cx+R+22,cy-20],radius=8,fill=CASE2); T(cx+R+40,cy-45,"PWR",20,DIM,600,'lm')
d.rounded_rectangle([cx+R-6,cy+20,cx+R+22,cy+70],radius=8,fill=CASE2); T(cx+R+40,cy+45,"BOOT",20,DIM,600,'lm')
# usb
d.rounded_rectangle([cx-R-20,cy-25,cx-R+6,cy+25],radius=6,fill=(20,20,20)); T(cx-R-30,cy,"USB-C (cover)\nor magnetic",20,DIM,600,'rm')
# dims
d.line([(cx-R,cy+R+180),(cx+R,cy+R+180)],fill=OR,width=3); T(cx,cy+R+195,"case ~48 mm  (board A + 2 x 3 mm walls)",22,OR,700,'mt')
T(cx,cy-R-185,"strap 22 mm (standard watch pins)",22,DIM,600,'mt')

# ---------- 3. back + parts ----------
bx,byy=60,1250
T(bx,byy,"3. Back cover (opens with 4 screws)",30,CY,700)
c2x,c2y=230,1500; R2=150
d.ellipse([c2x-R2,c2y-R2,c2x+R2,c2y+R2],fill=CASE2)
d.ellipse([c2x-R2+18,c2y-R2+18,c2x+R2-18,c2y+R2-18],outline=(20,20,20),width=6)
for a in (45,135,225,315):
    r=R2-35; d.ellipse([c2x+r*math.cos(math.radians(a))-10,c2y+r*math.sin(math.radians(a))-10,c2x+r*math.cos(math.radians(a))+10,c2y+r*math.sin(math.radians(a))+10],fill=(30,30,30))
d.rounded_rectangle([c2x-30,c2y-50,c2x+30,c2y-20],radius=6,fill=(200,170,60)); T(c2x,c2y-35,"pins",16,(20,20,20),700,'mm')
T(c2x,c2y+20,"O-ring",20,TX,600,'mm'); T(c2x,c2y+50,"4 x M1.6 screws",18,DIM,500,'mm')
T(bx,byy+40,"Optional: 2 pogo pins + magnetic USB charger (no open port)",22,DIM,500)

px=60
py=700
T(px,py,"Parts (about EUR 25-40 extra)",30,CY,700)
parts=[("Battery 3.7 V 850 mAh LiPo, MX1.25 plug (803035)","EUR 6-10"),
       ("Case: 3D print in PETG or ASA (resin = nicer finish)","EUR 5-15"),
       ("O-ring 40-42 mm + 4 x M1.6 x 4 mm screws","EUR 3"),
       ("Watch strap 22 mm silicone + 2 spring bars","EUR 5"),
       ("Magnetic USB-C cable + pogo pins (optional)","EUR 6"),
       ("Thin foam pad + kapton tape (protects the battery)","EUR 3")]
for i,(a,b) in enumerate(parts):
    T(px,py+50+i*38,"- "+a,22,TX,500); T(px+900,py+50+i*38,b,22,GR,600,'ra')
T(px,py+300,"Splash-proof with O-ring + sealed buttons. Not for swimming.",22,OR,600)
T(px,py+332,"Battery never touches the board: foam pad between them. Red wire to + !",22,RED,600)
im.save('/home/user/7amzala/mockups/watch_case_design.png')
print("ok")
