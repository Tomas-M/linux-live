from pathlib import Path
from PIL import Image

# Regenerate the prepared framebuffer data when the original logo changes.
folder=Path(__file__).resolve().parent
logo=Image.open(folder.parent/'bootfiles/bootlogo.png').convert('RGB').crop((174,123,474,273))
# Fill from the left, then clear from the left: ".", "..", "...", " ..", "  .".
loader=[(97,202,0) if mask&(1<<(x//12)) and x%12<4 else (0,0,0) for mask in (1,3,7,6,4) for row in range(4) for x in range(28)]
pixels=logo.tobytes()
for name,pixels in [('logo',list(zip(pixels[0::3],pixels[1::3],pixels[2::3]))),('loader',loader)]:
    for depth in (16,24,32):
        if depth==16:
            data=b''.join(((r>>3)<<11|(g>>2)<<5|(b>>3)).to_bytes(2,'little') for r,g,b in pixels)
        else:
            data=b''.join(bytes((b,g,r,255)) if depth==32 else bytes((b,g,r)) for r,g,b in pixels)
        (folder/(name+'.'+str(depth))).write_bytes(data)
