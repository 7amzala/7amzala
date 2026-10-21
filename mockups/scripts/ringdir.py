from PIL import Image, ImageDraw, ImageFont
F='/tmp/claude-0/-home-user-7amzala/ba326ba7-13b7-5834-81bf-91b86d9a13f8/scratchpad/fonts/Montserrat.ttf'
def font(sz,w=600):
    f=ImageFont.truetype(F,sz)
    try: f.set_variation_by_axes([w])
    except Exception: pass
    return f
S=2; W=466
CY=(34,211,255); OR=(255,186,82); RED=(255,77,94); VI=(155,140,255); TX=(234,243,255); DIM=(122,140,170); TR=(26,30,40)
def screen(val, col, pill, sub):
    im=Image.new('RGB',(W*S,W*S),(0,0,0)); d=ImageDraw.Draw(im)
    box=[7*S,7*S,(W-7)*S,(W-7)*S]
    # track: 270 deg, gap at bottom (PIL angles: 0=right, clockwise)
    d.arc(box,135,405,fill=TR,width=10*S)
    # centre mark at the top
    d.line([(W//2*S,4*S),(W//2*S,22*S)],fill=DIM,width=2*S)
    if val>0: d.arc(box,270,270+val*135,fill=col,width=10*S)
    if val<0: d.arc(box,270+val*135,270,fill=col,width=10*S)
    f=font(16*S,600); tw=d.textlength(pill,font=f)
    d.rounded_rectangle([(W*S-tw)/2-14*S,66*S,(W*S+tw)/2+14*S,96*S],radius=15*S,outline=col,width=S,fill=tuple(c//5 for c in col))
    d.text(((W*S-tw)/2,73*S),pill,font=f,fill=col)
    f=font(18*S,500)
    for i,l in enumerate(sub.split('\n')):
        tw=d.textlength(l,font=f); d.text(((W*S-tw)/2,(190+i*30)*S),l,font=f,fill=TX if i==0 else DIM)
    m=Image.new('L',(W*S,W*S),0); ImageDraw.Draw(m).ellipse([0,0,W*S-1,W*S-1],fill=255)
    bg=Image.new('RGB',(W*S,W*S),(22,22,26)); bg.paste(im,(0,0),m)
    return bg.resize((W,W),Image.LANCZOS)
shots=[(0,CY,"ARMED","rod at rest\nring empty"),
       (0.35,OR,"SLOW PULL","tip pulled forward a bit\nring fills to the right"),
       (1.0,RED,"RUN","tip forward to strike level\nring full on the right"),
       (-0.5,VI,"SLACK","tip goes back past rest\nring fills to the left")]
sh=Image.new('RGB',(4*490,540),(22,22,26)); d=ImageDraw.Draw(sh)
for i,(v,c,p,s) in enumerate(shots):
    sh.paste(screen(v,c,p,s),(i*490+12,10))
d.text((20,500),"Ring = where the rod tip is right now. Starts at the top mark: forward = right, back = left. It moves exactly as fast as the rod.",font=font(20,500),fill=TX)
sh.save('/home/user/7amzala/mockups/watch_ring_mirror_mockup.png')
