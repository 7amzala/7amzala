from PIL import Image, ImageDraw, ImageFont, ImageFilter, ImageChops
F='/tmp/claude-0/-home-user-7amzala/ba326ba7-13b7-5834-81bf-91b86d9a13f8/scratchpad/fonts/Montserrat.ttf'
def font(sz,w=600):
    f=ImageFont.truetype(F,sz)
    try: f.set_variation_by_axes([w])
    except Exception: pass
    return f
W,H=2000,1250
im=Image.new('RGB',(W,H),(12,14,20)); d=ImageDraw.Draw(im)
TX=(234,243,255); DIM=(140,155,180); CY=(34,211,255); OR=(255,186,82); RED=(255,77,94); VI=(155,140,255); GR=(61,220,132); BL=(30,123,255)
def T(x,y,t,sz=26,c=TX,w=600,a='la'): d.text((x,y),t,font=font(sz,w),fill=c,anchor=a)
T(40,30,"Rod-tip light: a smart glow stick (8 tiny LEDs, about 7 cm)",42,TX,700)
T(40,84,"Like the glow sticks anglers clip on the tip at night - but it shows what the fish does, in the watch colours.",24,DIM,500)

def tip(y0, lit, col, label, sub, glow=True):
    # rod blank coming from the left, getting thinner to the tip
    x0,x1=120,1180
    for i in range(x0,x1):
        th=14-9*(i-x0)/(x1-x0)
        d.line([(i,y0-th/2),(i,y0+th/2)],fill=(55,60,72))
    # guide rings
    for gx in (380,720,1010):
        d.ellipse([gx-12,y0-26,gx+12,y0-2],outline=(150,155,165),width=3)
    # tip ring
    d.ellipse([x1-6,y0-22,x1+18,y0+2],outline=(170,175,185),width=3)
    # sensor box
    d.rounded_rectangle([700-60,y0+10,700+60,y0+52],radius=10,fill=(30,34,46),outline=(70,78,96),width=2)
    T(700,y0+31,"sensor",16,DIM,600,'mm')
    # light bar: clear tube along the last 7 cm (8 LEDs)
    bx0,bx1=880,1150
    gl=Image.new('RGB',(W,H),(0,0,0)); gd=ImageDraw.Draw(gl)
    for k in range(8):
        cx=bx0+18+k*(bx1-bx0-36)/7
        on=k<lit
        c=col if on else (40,44,56)
        if on and glow: gd.ellipse([cx-26,y0-26,cx+26,y0+26],fill=col)
    gl=gl.filter(ImageFilter.GaussianBlur(18)).point(lambda v: int(v*0.6))
    im.paste(ImageChops.screen(im,gl),(0,0))
    dd=ImageDraw.Draw(im)
    dd.rounded_rectangle([bx0,y0-12,bx1,y0+12],radius=12,fill=(28,32,42),outline=(120,130,150),width=2)
    for k in range(8):
        cx=bx0+18+k*(bx1-bx0-36)/7
        on=k<lit
        dd.rounded_rectangle([cx-9,y0-7,cx+9,y0+7],radius=3,fill=col if on else (48,52,64))
    T(1240,y0-18,label,28,col if lit else DIM,800)
    T(1240,y0+18,sub,20,DIM,500)

tip(210,1,BL,"ARMED","one tiny blue blink every 5 s (\"I'm working\")")
tip(350,3,OR,"SLOW PULL","orange, fills up as the pull grows")
tip(490,8,RED,"STRIKE","full red, flashing until you answer")
tip(630,3,VI,"SLACK","violet: the line went loose")
tip(770,1,OR,"KNOCK","a short orange flash per knock")

# parts + wiring
y=880
T(40,y,"Parts (about EUR 6-10)",30,CY,700)
parts=["- WS2812B-2020 LED strip, 4 mm wide, 160 LEDs/m: cut 8 LEDs (5 cm)",
       "- Clear silicone / heat-shrink tube, 6-8 mm (waterproof glow-stick look)",
       "- 1 extra thin wire in the cable (or 5-core cable) + 330 ohm resistor",
       "- 2 small cable ties or tip clips to hold it on the last 7 cm"]
for i,p in enumerate(parts): T(40,y+48+i*36,p,22,TX,500)
x=1080
T(x,y,"Wiring (3 wires, shares the sensor cable)",30,CY,700)
wl=[("3V3  ->  LED +5V/VDD",TX),("GND  ->  LED GND",TX),("D3 (GPIO4) -> 330 ohm -> LED DIN",OR),("Night: dimmed by itself (the rod knows the time)",DIM)]
for i,(p,c) in enumerate(wl): T(x,y+48+i*36,p,22,c,600 if c!=DIM else 500)
T(40,1210,"Weight about 1-2 g at the tip: too light to change how the rod bends or what the sensor measures.",22,GR,600)
im.save('/home/user/7amzala/mockups/rod_tip_light_mockup.png'); print('ok')
