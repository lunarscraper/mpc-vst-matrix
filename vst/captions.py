#!/usr/bin/env python3
"""Gives every label of the Matrix skin the size and face of the device's own knob names:
Titillium Web SemiBold at JUCE height 19.55 (= ascent + descent in px; mpc-vst-plugins' knob Name
labels, label_scale 1.15). Run by build.sh right after gen_vst.py.
Taken over from mpc-vst-drumgen's vst/captions.py; here the popup/enum options come from module.json's
chain_params (or params.json if present), and enum_h groups are handled as well.

  captions.py <layout.conf> "<skin>/Plugin Skins" <TitilliumWeb-SemiBold.ttf>

gen_vst.py bakes frame titles, readout/popup labels, button text and popup list options with
shadow_art's small 9x9 bitmap font, and draws toggle names live at height 15. This script:
  - frames, readouts, popups with caption="..." (the layout gives them title/label "-" so nothing
    is baked): draws the caption on the tab background sh_bg_<tab>.png
  - buttons: repaints the button face and draws its label (sh_btn_<key>_<LABEL>_{on,off}.png)
  - popup list options: redraws each option tile (sh_popopt_<tab>_<key>_<n>_{on,off}.png)
  - enum_h with caption="..." (label "" so nothing is baked): draws the group caption above the
    segments on sh_bg_<tab>.png and redraws each segment (sh_seg_<key>_<n>_{on,off}.png)
  - TUI.json: toggle Name labels from height 15 to the knob names' 19.55
Coordinates: layout y - 86 = skin y (shadow_skin.Y_OFF)."""
import json
import os
import re
import shlex
import sys

from PIL import Image, ImageDraw, ImageFont

Y_OFF = 86
NAME_HEIGHT = 19.55      # mpc-vst-plugins: knob Name label 17.0 * label_scale 1.15


def parse(path):
    theme, tabs = {}, []
    for raw in open(path):
        line = raw.strip()
        if not line or line.startswith("#") or line.startswith("qlinks"):
            continue
        m = re.match(r"\[tab (.+)\]$", line)
        if m:
            tabs.append([])
            continue
        if not tabs and "=" in line:
            k, _, v = line.partition("=")
            theme[k.strip()] = v.strip()
            continue
        toks = shlex.split(line)
        w = {"kind": toks[0]}
        for t in toks[1:]:
            k, _, v = t.partition("=")
            w[k] = v
        tabs[-1].append(w)
    return theme, tabs


def font_px(path, height):
    """The knob names' height as a Pillow size, the way mpc-vst-plugins' own skin preview (studio.py)
    draws them; that matches how much larger they read on the device than the baked bitmap text."""
    return ImageFont.truetype(path, int(round(height)))


def draw_center(dr, box, text, font, fill):
    x, y, w, h = box
    tb = dr.textbbox((0, 0), text, font=font)
    tw, th = tb[2] - tb[0], tb[3] - tb[1]
    dr.text((x + (w - tw) / 2 - tb[0], y + (h - th) / 2 - tb[1]), text, font=font, fill=fill)


def draw_above(dr, x, bottom, text, font, fill):
    """left-aligned, cap line resting `bottom` px above"""
    tb = dr.textbbox((0, 0), text, font=font)
    dr.text((x - tb[0], bottom - tb[3]), text, font=font, fill=fill)


def slug(t):   # shadow_skin.slug()
    return re.sub(r"[^A-Za-z0-9]+", "_", t).strip("_")


def main():
    layout, skin, ttf = sys.argv[1:4]
    here = os.path.dirname(layout)
    if os.path.exists(os.path.join(here, "params.json")):
        plist = json.load(open(os.path.join(here, "params.json")))["params"]
    else:
        plist = json.load(open(os.path.join(here, "module.json")))["chain_params"]
    options = {p["key"]: p["options"] for p in plist if p.get("options")}
    theme, tabs = parse(layout)
    col = lambda k, d: "#" + theme.get(k, d)
    ink, title_ink, lcd = col("theme_ink", "e8e4da"), col("theme_accent_hi", "ffd08a"), col("theme_lcd", "07080a")
    on_bg, on_tx, btn_tx = col("theme_seg_active", "ffb547"), col("theme_seg_active_tx", "1b1408"), col("theme_btn_text", "1b1408")
    seg_off, ink_dim = col("theme_seg_inactive", "2b2f35"), col("theme_ink_dim", "9a9890")
    font = font_px(ttf, NAME_HEIGHT)
    n = {"caption": 0, "button": 0, "option": 0, "segment": 0, "toggle": 0}
    for t, ws in enumerate(tabs):
        bg = os.path.join(skin, "sh_bg_%d.png" % t)
        im = Image.open(bg).convert("RGB")
        dr = ImageDraw.Draw(im)
        for w in ws:
            cap = w.get("caption")
            if not cap:
                continue
            if w["kind"] == "frame":       # in the title band, where frame_box() puts its own title
                x, y = int(w["x"]), int(w["y"]) - Y_OFF
                face = im.getpixel((x + int(w["w"]) - 40, y + 14))   # cover the "-" placeholder title
                dr.rectangle([x + 10, y + 3, x + 90, y + 33], fill=face)
                draw_above(dr, x + 18, y + 31, cap, font, title_ink)
            elif w["kind"] in ("readout", "popup"):
                x0 = int(w["cx"]) - int(w["w"]) // 2 + 2
                draw_above(dr, x0, int(w["cy"]) - int(w["h"]) // 2 - Y_OFF - 6, cap, font, ink)
            elif w["kind"] == "enum_h":     # centred over the segment row (shadow_skin.seg_rects(), one row)
                cx, top = int(w["cx"]), int(w["cy"]) - 33 // 2 - Y_OFF
                tb = dr.textbbox((0, 0), cap, font=font)
                dr.text((cx - (tb[2] - tb[0]) / 2 - tb[0], top - 6 - tb[3]), cap, font=font, fill=ink)
            n["caption"] += 1
        im.save(bg)
        for w in ws:
            if w["kind"] == "button":
                for state in ("on", "off"):
                    p = os.path.join(skin, "sh_btn_%s_%s_%s.png" % (w["key"], slug(w.get("label", "")), state))
                    if not os.path.exists(p):
                        continue
                    bi = Image.open(p).convert("RGBA")
                    bw, bh = bi.size
                    face = bi.getpixel((bw // 2, 9))          # above the centred text, inside the ring
                    d = ImageDraw.Draw(bi)
                    d.rectangle([9, 9, bw - 10, bh - 10], fill=face)
                    draw_center(d, (0, 0, bw, bh), w["label"], font, btn_tx)
                    bi.save(p)
                    n["button"] += 1
            if w["kind"] == "enum_h":
                for o, text in enumerate(options[w["key"]]):
                    for state in ("on", "off"):
                        p = os.path.join(skin, "sh_seg_%s_%d_%s.png" % (w["key"], o, state))
                        if not os.path.exists(p):
                            continue
                        si = Image.open(p).convert("RGB")
                        d = ImageDraw.Draw(si)
                        d.rectangle([0, 0, si.size[0], si.size[1]], fill=on_bg if state == "on" else seg_off)
                        draw_center(d, (0, 0) + si.size, text, font, on_tx if state == "on" else ink_dim)
                        si.save(p)
                        n["segment"] += 1
            if w["kind"] == "popup":
                for o, text in enumerate(options[w["key"]]):
                    for state in ("on", "off"):
                        p = os.path.join(skin, "sh_popopt_%d_%s_%d_%s.png" % (t, w["key"], o, state))
                        if not os.path.exists(p):
                            continue
                        oi = Image.open(p).convert("RGB")
                        d = ImageDraw.Draw(oi)
                        d.rectangle([0, 0, oi.size[0], oi.size[1]], fill=on_bg if state == "on" else lcd)
                        draw_center(d, (0, 0) + oi.size, text, font, on_tx if state == "on" else ink)
                        oi.save(p)
                        n["option"] += 1
    tui = os.path.join(skin, "TUI.json")
    j = json.load(open(tui))
    for d in j["pageData"]["componentDefinitions"]["localComponentDefinitions"]:
        if not d["key"].startswith("shToggle"):
            continue
        for c in d["value"].get("componentsData", []):
            cd = c["componentData"]
            if cd.get("type") == "Label" and cd["data"]["textStyle"]["font"]["height"] == 15.0:
                cd["data"]["textStyle"]["font"]["height"] = NAME_HEIGHT
                x, y, bw, bh = (int(v) for v in c["bounds"]["bounds"].split())
                c["bounds"]["bounds"] = "%d %d %d %d" % (x, y - 2, bw, bh + 4)
                n["toggle"] += 1
    json.dump(j, open(tui, "w"), indent=1)
    print("captions.py: %(caption)d captions, %(button)d button, %(option)d popup option and %(segment)d "
          "segment images, %(toggle)d toggle name labels" % n)


if __name__ == "__main__":
    main()
