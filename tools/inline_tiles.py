"""scripts/build-web.sh: put the page's pictures into the built page as data URLs, so it stays one file:
web/tiles.png (tools/mkpagetiles.py) for __EMINE_TILES__, web/boxart.jpg (tools/mkboxart.py) for __EMINE_BOXART__.
usage: python tools/inline_tiles.py <page.html>"""
import base64, os, sys
WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web')
page = sys.argv[1]
s = open(page, encoding='utf-8').read()
for key, name, mime in [('__EMINE_TILES__', 'tiles.png', 'image/png'), ('__EMINE_BOXART__', 'boxart.jpg', 'image/jpeg')]:
    s = s.replace(key, 'data:%s;base64,' % mime + base64.b64encode(open(os.path.join(WEB, name), 'rb').read()).decode())
open(page, 'w', encoding='utf-8').write(s)
