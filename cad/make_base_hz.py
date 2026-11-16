"""Jar of Thoughts base v2 - parametric generator (needs: pip install trimesh manifold3d numpy).
All sizes in mm. Run:  python3 make_base.py
Outputs: base_<variant>_tub.stl, base_<variant>_top.stl, button_cap.stl
"""
import os, numpy as np, trimesh
from trimesh.boolean import union, difference

OUT = os.path.dirname(os.path.abspath(__file__))

def box(x0,y0,z0,x1,y1,z1):
    b = trimesh.creation.box(extents=[x1-x0, y1-y0, z1-z0])
    b.apply_translation([(x0+x1)/2, (y0+y1)/2, (z0+z1)/2]); return b
def cylz(cx,cy,z0,z1,r,n=96):
    c = trimesh.creation.cylinder(radius=r, height=z1-z0, sections=n)
    c.apply_translation([cx, cy, (z0+z1)/2]); return c
def cyly(cx,cz,y0,y1,r,n=96):
    c = trimesh.creation.cylinder(radius=r, height=y1-y0, sections=n)
    c.apply_transform(trimesh.transformations.rotation_matrix(np.pi/2,[1,0,0]))
    c.apply_translation([cx,(y0+y1)/2,cz]); return c
def U(p): return union(p, engine='manifold')
def D(a,p): return difference([a]+p, engine='manifold')

# ---- Omron V-155-1C25 micro switch (body 27.8 x 15.9 x 10.3; roller ~6.4 above the plunger edge when free)
SW_L, SW_T, SW_H = 27.8, 10.3, 15.9
ROLLER_FREE = 6.4
CLR = 0.3
FLANGE_T, FLANGE_L, FLANGE_W = 3.0, 20.0, 10.0
BTN_D = 10.0
BTN_X = -16.0            # button / roller x position (off-centre so the terminals have room on the +X side)
ROLLER_FROM_END = 2.5     # roller centre this far in from the switch's -X end (from the photo; verify)
TERM_LEN = 22.0           # tabs + crimp clips sticking out of the +X end

VARIANTS_OLD = {
    # name: (jar band outside dia, band height, band lip inside dia, jar body dia)  <- MEASURE YOUR BAND
    'regular': dict(band_od=73.5, band_h=13.0, lip_id=63.0, body_d=80.0),
    'wide':    dict(band_od=90.0, band_h=13.0, lip_id=80.0, body_d=90.0),
}

VARIANTS = {'regular': dict(band_od=73.5, band_h=19.0, lip_id=56.0, body_d=80.0)}

def build(name, band_od, band_h, lip_id, body_d):
    F = max(body_d + 12, 92.0); h = F/2
    wall, floor, plate_t = 2.4, 3.0, 4.0
    y_in = -h + wall
    # ---------------- tub ----------------
    pc = h - wall - 4.5
    post_xy = [(sx*pc, sy*pc) for sx in (-1,1) for sy in (-1,1)]
    pl, pw = 56.0, 30.0
    pcx = h - wall - 0.5 - pl/2
    pcy = 8.0
    # switch lies flat (length along X). Shelf from the floor below it, vertical end walls, LID underside is the top of the bracket.
    btn_z = 20.0
    Ht = btn_z + SW_T/2 + 0.2                  # lid underside presses just on top of the switch
    ya = FLANGE_T + ROLLER_FREE
    gB = SW_T/2 + CLR
    yb1 = y_in + ya + SW_H + 1.6
    tub = D(box(-h,-h,0,h,h,Ht), [box(-h+wall,-h+wall,floor,h-wall,h-wall,Ht+1)])
    add = [tub] + [cylz(x,y,floor-0.1,Ht,4.0) for x,y in post_xy]
    for sy in (-1,1):
        yc = pcy + sy*(pw/2+1.0)
        add.append(box(pcx-20, yc-1.0, floor-0.1, pcx+20, yc+1.0, floor+13.0))
    add.append(box(pcx-pl/2-2.0, pcy-10, floor-0.1, pcx-pl/2, pcy+10, floor+13.0))
    # Switch from the photo: terminals exit the +X short end (3 tabs + crimp clips ~22 mm), roller near the -X end.
    # Button x = BTN_X; body spans [bx0, bx1]. Terminal/wire zone to the +X side of the body is left completely open.
    bx0 = BTN_X - ROLLER_FROM_END
    bx1 = bx0 + SW_L
    fa0, fa1 = BTN_X - FLANGE_L/2, BTN_X + FLANGE_L/2
    add.append(box(fa0-0.3, y_in-0.1, floor-0.1, fa1+0.3, y_in+ya, btn_z-gB))        # shelf under cap flange
    add.append(box(bx0-CLR, y_in+ya-0.1, floor-0.1, bx1+CLR, yb1, btn_z-gB))          # shelf under switch body
    add.append(box(fa0-0.3-2.4, y_in-0.1, floor-0.1, fa0-0.3, y_in+ya, Ht))        # flange end wall (-X)
    add.append(box(fa1+0.3, y_in-0.1, floor-0.1, fa1+0.3+2.4, y_in+ya, Ht))        # flange end wall (+X)
    add.append(box(bx0-CLR-2.4, y_in+ya-0.1, floor-0.1, bx0-CLR, y_in+ya+SW_H, Ht))  # body end wall (-X)
    add.append(box(bx0-CLR, y_in+ya+SW_H+0.15, floor-0.1, bx0+6, yb1, Ht))         # rear stop A
    add.append(box(bx1-6, y_in+ya+SW_H+0.15, floor-0.1, bx1+CLR, yb1, Ht))         # rear stop B
    tub = U(add)
    cuts = [cylz(x,y,Ht-12,Ht+1,1.3) for x,y in post_xy]
    cuts.append(box(h-wall-1.0, pcy-7.0, floor+1.0, h+1.0, pcy+7.0, Ht+1))          # USB slot, open to the lid
    cuts.append(cyly(BTN_X, btn_z, -h-1, y_in+0.5, BTN_D/2))                       # button hole
    cuts.append(box(BTN_X-BTN_D/2, -h-1, btn_z, BTN_X+BTN_D/2, y_in+0.5, Ht+1))    # ...open at the top (no bridge)
    tub = D(tub, cuts)

    # ---------------- top (band socket) ----------------
    rec_r = band_od/2 + 0.4
    col_r = rec_r + 2.4
    top_z = plate_t + band_h - 1.0
    top = U([box(-h,-h,0,h,h,plate_t), cylz(0,0,0,top_z,col_r)])
    ribs = []
    for k in range(8):
        a = 2*np.pi*k/8
        ribs.append(box(rec_r-0.5, -0.5, plate_t, rec_r+0.2, 0.5, top_z))
        ribs[-1].apply_transform(trimesh.transformations.rotation_matrix(a,[0,0,1]))
    top = D(top, [cylz(0,0,plate_t,top_z+1,rec_r)])      # band pocket
    top = U([top] + ribs)                                 # press-fit ribs
    tcuts = [cylz(0,0,-1,plate_t+1,lip_id/2)]            # opening under the band lip
    for x,y in post_xy:
        tcuts += [cylz(x,y,-1,plate_t+1,1.7), cylz(x,y,plate_t-2.5,plate_t+1,3.2)]
    top = D(top, tcuts)

    for m,nm in ((tub,'tub'),(top,'top')):
        m.merge_vertices(); m.export(os.path.join(OUT, f"hz_{nm}.stl"))
        print(name, nm, 'watertight', m.is_watertight, 'bbox', np.round(m.bounds[1]-m.bounds[0],1))

    # ---- fit checks with a dummy switch + cap ----
    sw = box(bx0, y_in+ya, btn_z-SW_T/2, bx1, y_in+ya+SW_H, btn_z+SW_T/2)
    terms = box(bx1, y_in+ya, btn_z-4.5, bx1+TERM_LEN, y_in+ya+SW_H, btn_z+4.5)
    flange = box(fa0, y_in, btn_z-FLANGE_W/2, fa1, y_in+FLANGE_T, btn_z+FLANGE_W/2)
    for nm, part in (('switch', sw), ('terminals+clips', terms), ('cap flange', flange)):
        vol = trimesh.boolean.intersection([tub, part], engine='manifold').volume
        print(f'  fit check {nm} vs tub: overlap volume {vol:.3f} mm3', '(OK)' if vol < 1e-3 else '(COLLISION)')
    for nm_,m in (('tub',tub),('top',top)):
        n=m.face_normals; c=m.triangles_center; ar=m.area_faces
        ov=(n[:,2]<-0.71)&(c[:,2]>0.05)
        print('  overhang area',nm_, round(float(ar[ov].sum()),1),'mm2')
    return tub, top, F

def button_cap():
    flange = box(-FLANGE_L/2, -FLANGE_W/2, 0, FLANGE_L/2, FLANGE_W/2, FLANGE_T)
    stem = cylz(0,0,FLANGE_T-0.1, FLANGE_T+2.4+2.5, (BTN_D-0.6)/2)
    cap = U([flange, stem]); cap.merge_vertices()
    cap.export(os.path.join(OUT, "hz_button_cap.stl"))
    print('button_cap watertight', cap.is_watertight, 'bbox', np.round(cap.bounds[1]-cap.bounds[0],1))

if __name__ == '__main__':
    button_cap()
    for k,v in VARIANTS.items(): build(k, **v)
