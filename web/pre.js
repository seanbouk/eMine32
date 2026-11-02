// eMine32 web: command line + video config for the engine. Game data is preloaded at /game.
// URL options:
//   ?args=-foo%20bar   extra engine command-line args
//   ?res=1720x720      exact render resolution
//   ?scale=2           render at window size / 2 (default 1 = native window pixels)
//   ?stats             once-a-second timing in the JS console
var emineParams = new URLSearchParams(location.search);
Module['arguments'] = ['-nosetup', '-usecwd', '-j', '/game', '-j', '/emine', '-mh', 'emine32.def', '-gamegrp', 'DUKE3D.GRP'];
(function () {
  var extra = emineParams.get('args');
  if (extra) Module['arguments'] = Module['arguments'].concat(extra.trim().split(/\s+/));
})();
Module['emineArgs'] = Module['arguments'];

// Pick the render resolution: match the window's aspect (so 21:9 works) and, by default, its real pixel count.
Module['emineRes'] = function () {
  var m = (emineParams.get('res') || '').match(/^(\d+)x(\d+)$/);
  if (m) return [+m[1], +m[2]];
  var scale = parseFloat(emineParams.get('scale')) || 1;
  var dpr = window.devicePixelRatio || 1;
  var w = Math.round(window.innerWidth * dpr / scale), h = Math.round(window.innerHeight * dpr / scale);
  return [w & ~3, h & ~1];  // keep the width a multiple of 4 for the column renderer
};

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  var r = Module['emineRes']();
  FS.writeFile('/eduke32.cfg', '[Screen Setup]\nScreenBPP = 8\nScreenMode = 0\nScreenWidth = ' + r[0] + '\nScreenHeight = ' + r[1] + '\n');
  console.log('eMine32 render resolution ' + r[0] + 'x' + r[1]);
});
