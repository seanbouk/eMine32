// eMine32 web: command line for the engine. Game data is preloaded at /game.
// Extra args can be passed in the page URL: eduke32.html?args=-map%20E1L1.MAP%20-s1
Module['arguments'] = ['-nosetup', '-usecwd', '-j', '/game', '-gamegrp', 'DUKE3D.GRP'];
(function () {
  try {
    var extra = new URLSearchParams(location.search).get('args');
    if (extra) Module['arguments'] = Module['arguments'].concat(extra.trim().split(/\s+/));
  } catch (e) {}
})();
Module['emineArgs'] = Module['arguments'];
