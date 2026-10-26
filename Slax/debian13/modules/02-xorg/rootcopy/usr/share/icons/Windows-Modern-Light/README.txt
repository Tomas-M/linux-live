Windows Modern Light for Slax

Based on Windows Modern Cursors v2.0.0 by Nurudin Imsirovic:
https://github.com/NurudinImsirovic/windows-cursors-modern
Source release: windows-modern-cursors-v2.7z, x1 white variant.
License: MIT; see LICENSE.txt.

The CUR/ANI files were converted to Xcursor using win2xcur, preserving the
original 32x32 canvases, hotspots, frame order and animation timing. Xcursor
stores premultiplied ARGB pixels, including the original antialiased alpha.
Colors are unchanged, including the blue wait and progress animations.

The checked-in binaries are the build inputs. No download or conversion is
needed when building Slax. 02-xorg/copy creates the toolkit and X11 aliases
listed in 02-xorg/cursor-aliases, avoiding duplicate bitmap files.
