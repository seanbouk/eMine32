// eMine32 web: command line + video config for the engine. Game data is preloaded at /game.
// URL options:
//   ?args=-foo%20bar   extra engine command-line args
//   ?res=1720x720      another render resolution (default 960x720)
//   ?stats             once-a-second timing in the JS console
var emineParams = new URLSearchParams(location.search);
Module['arguments'] = ['-nosetup', '-usecwd', '-j', '/game', '-j', '/emine', '-mh', 'emine32.def', '-x', 'EMINE.CON', '-gamegrp', 'DUKE3D.GRP'];
(function () {
  var extra = emineParams.get('args');
  if (extra) Module['arguments'] = Module['arguments'].concat(extra.trim().split(/\s+/));
})();
Module['emineArgs'] = Module['arguments'];

// The render resolution: always 960x720 (4:3). It's set once and never changed while the game runs: changing the
// engine's video mode mid-game corrupts memory on the web (the 2026-11-13 adaptive-resolution crash, and again with
// fullscreen on 2026-11-19). The page scales the picture to its box (smaller windows, fullscreen).
//   ?res=WxH still picks another size, for tests.
Module['emineRes'] = function () {
  var m = (emineParams.get('res') || '').match(/^(\d+)x(\d+)$/);
  return m ? [+m[1], +m[2]] : [960, 720];
};

Module['preRun'] = Module['preRun'] || [];
Module['preRun'].push(function () {
  var r = Module['emineRes']();
  FS.writeFile('/eduke32.cfg', '[Screen Setup]\nScreenBPP = 8\nScreenMode = 0\nScreenWidth = ' + r[0] + '\nScreenHeight = ' + r[1] + '\n');
  console.log('eMine32 render resolution ' + r[0] + 'x' + r[1]);
});
