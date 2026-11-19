// eMine32 web: command line + video config for the engine. Game data is preloaded at /game.
// URL options:
//   ?args=-foo%20bar   extra engine command-line args
//   ?res=1720x720      exact render resolution
//   ?scale=2           render at window size / 2 (default: native window pixels, capped at about a megapixel)
//   ?stats             once-a-second timing in the JS console
var emineParams = new URLSearchParams(location.search);
Module['arguments'] = ['-nosetup', '-usecwd', '-j', '/game', '-j', '/emine', '-mh', 'emine32.def', '-x', 'EMINE.CON', '-gamegrp', 'DUKE3D.GRP'];
(function () {
  var extra = emineParams.get('args');
  if (extra) Module['arguments'] = Module['arguments'].concat(extra.trim().split(/\s+/));
})();
Module['emineArgs'] = Module['arguments'];

// Pick the render resolution: 4:3, as big as fits the game's box on the page (960x720, or the screen in fullscreen;
// the page letterboxes it), at its real pixel count by default.
Module['emineRes'] = function () {
  var m = (emineParams.get('res') || '').match(/^(\d+)x(\d+)$/);
  if (m) return [+m[1], +m[2]];
  var dpr = window.devicePixelRatio || 1;
  var box = document.getElementById('game'), bw = box ? box.clientWidth : window.innerWidth, bh = box ? box.clientHeight : window.innerHeight;
  var W = Math.min(bw, bh * 4 / 3) * dpr, H = W * 3 / 4;
  // default: those pixels, but no more than about a megapixel
  var scale = parseFloat(emineParams.get('scale')) || Math.max(1, Math.sqrt(W * H / 1.0e6));
  var w = Math.round(W / scale), h = Math.round(H / scale);
  return [w & ~3, h & ~1];  // keep the width a multiple of 4 for the column renderer
};

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  var r = Module['emineRes']();
  FS.writeFile('/eduke32.cfg', '[Screen Setup]\nScreenBPP = 8\nScreenMode = 0\nScreenWidth = ' + r[0] + '\nScreenHeight = ' + r[1] + '\n');
  console.log('eMine32 render resolution ' + r[0] + 'x' + r[1]);
});
