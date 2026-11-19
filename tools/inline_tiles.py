"""scripts/build-web.sh: put web/tiles.png into the built page (its __EMINE_TILES__) as a data URL.
usage: python tools/inline_tiles.py <page.html> <tiles.png>"""
import base64, sys
page, png = sys.argv[1], sys.argv[2]
s = open(page, encoding='utf-8').read()
open(page, 'w', encoding='utf-8').write(s.replace('__EMINE_TILES__', 'data:image/png;base64,' + base64.b64encode(open(png, 'rb').read()).decode()))
