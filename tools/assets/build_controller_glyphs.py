#!/usr/bin/env python3
"""Generate flat, distinct native input hints (requires Pillow)."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
ROOT=Path(__file__).resolve().parents[2]
font=ImageFont.truetype(str(ROOT/'interfaceFX/TypefaceServer/Play-Regular.ttf'),46)
small=ImageFont.truetype(str(ROOT/'interfaceFX/TypefaceServer/Play-Regular.ttf'),32)
out=ROOT/'assets/shell/icons';out.mkdir(parents=True,exist_ok=True)
actions=('left','right','up','down','ok','cancel','options','extra')
for family in ('default','keyboard','playstation','xbox','steam','ouya'):
    for action in actions:
        image=Image.new('RGBA',(128,128));d=ImageDraw.Draw(image)
        color=(246,248,255,255)
        if action in actions[:4]:
            points=[(64,24),(96,86),(32,86)]
            rotation={'up':0,'right':90,'down':180,'left':270}[action]
            d.polygon(points,fill=color)
            image=image.rotate(-rotation)
        elif family=='keyboard':
            labels={'ok':'Enter','cancel':'Esc','options':'Tab','extra':'Space'}
            d.rounded_rectangle((4,22,124,108),radius=12,outline=color,width=5)
            d.text((64,64),labels[action],font=small,anchor='mm',fill=color)
        else:
            d.ellipse((7,7,121,121),outline=(210,220,239,240),width=5)
            if family in ('default','playstation'):
                if action=='ok':
                    d.line((40,40,88,88),fill=(144,181,248,255),width=8);d.line((88,40,40,88),fill=(144,181,248,255),width=8)
                elif action=='cancel': d.ellipse((32,32,96,96),outline=(247,142,151,255),width=7)
                elif action=='options': d.line([(64,29),(99,93),(29,93),(64,29)],fill=(145,235,193,255),width=7)
                else: d.rectangle((34,34,94,94),outline=(233,168,231,255),width=7)
            else:
                letters={'ok':'A','cancel':'B','options':'Y','extra':'X'}
                colors={'ok':(141,233,157,255),'cancel':(249,147,147,255),'options':(246,224,127,255),'extra':(151,187,251,255)}
                d.text((64,65),letters[action],font=font,anchor='mm',fill=colors[action])
        image.save(out/f'icon_button_{family}_{action}.png',optimize=True)
