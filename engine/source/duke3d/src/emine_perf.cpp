// eMine32: frame timing for play-testing. `emine_perf` in the console summarises the last ~1000 drawn frames: how
// evenly they came, what each one spent on game tics (our systems broken out) and on drawing, and how far the view
// turned each frame against the mouse movement that came in, so a "jumpy" turn can be pinned on slow frames, uneven
// input or the angle itself. `emine_perf log` prints every late frame as it happens; `emine_perf clear` starts over.

#include "duke3d.h"
#include "emine.h"

#include <algorithm>
#ifdef __EMSCRIPTEN__
# include <emscripten.h>
#endif
#include <vector>

namespace
{
struct Frame
{
    double end;          // ms, when drawing finished
    float  tic, draw, wait;  // ms in game tics, ms drawing, ms waiting for the display (inside drawing's span)
    float  sys[EMINE_PERF_SYSTEMS];  // ms in our systems during those tics
    float  part[5];      // ms drawing: input, world, HUD and weapon, present, sound
    int    dang;         // view angle change since the last frame (Build angle units * 256)
    int    mousex;       // mouse movement read for this frame (engine units, after sensitivity)
    int    jsx, jsn;     // mouse movement the browser delivered to the page since the last frame (pixels, events)
    int    jslag;        // ms: the latest of those events (from the browser's timestamp to the page getting it)
    int    rawn, rawlag;     // the same for pointerrawupdate events (what the game reads, emine_rawmouse 1)
    uint8_t tics;
};
int constexpr N = 1024;
Frame ring[N];
int   count, head;
Frame cur;               // being filled in
int   lastAng;
bool  haveAng, logLate, logMouse;
int   pageStill, gameStill;  // frames in a row with no mouse movement (page / game), for `emine_perf mouse`
char const *const sysName[EMINE_PERF_SYSTEMS] = { "mobs", "world", "food", "voice", "other" };

double nowms() { return timerGetFractionalTicks(); }
}

void Emine_PerfSystem(int sys, double ms) { cur.sys[sys] += (float)ms; }
void Emine_PerfPart(int part, double ms) { cur.part[part] += (float)ms; }
void Emine_PerfMouse(int mousex) { cur.mousex += mousex; }
void Emine_PerfTics(int tics, double ms) { cur.tics += tics; cur.tic += (float)ms; }

void Emine_PerfFrame(double drawms, double waitms, int q16ang)
{
#ifdef __EMSCRIPTEN__
    cur.jsx = EM_ASM_INT({ var v = Module.emineMouseJS | 0; Module.emineMouseJS = 0; return v; });
    cur.jsn = EM_ASM_INT({ var v = Module.emineMouseEvents | 0; Module.emineMouseEvents = 0; return v; });
    cur.jslag = EM_ASM_INT({ var v = Module.emineMouseLag | 0; Module.emineMouseLag = 0; return v; });
    cur.rawn = EM_ASM_INT({ var v = Module.emineRawN | 0; Module.emineRawN = 0; return v; });
    cur.rawlag = EM_ASM_INT({ var v = Module.emineRawLag | 0; Module.emineRawLag = 0; return v; });
#endif
    cur.draw = (float)(drawms - waitms);
    cur.wait = (float)waitms;
    cur.end = nowms();
    int const ang = q16ang >> 8;
    cur.dang = haveAng ? ((ang - lastAng + (1024 << 8)) & ((2048 << 8) - 1)) - (1024 << 8) : 0;
    lastAng = ang, haveAng = true;
    Frame const *prev = count ? &ring[(head + N - 1) % N] : nullptr;
    if (logLate && prev && cur.end - prev->end > 25.0)
        OSD_Printf("emine_perf: late frame %.1f ms: tics %d (%.1f ms: mobs %.1f world %.1f food %.1f voice %.1f), draw %.1f ms (world %.1f hud %.1f present %.1f), mouse page %d game %d\n",
                   cur.end - prev->end, cur.tics, cur.tic, cur.sys[0], cur.sys[1], cur.sys[2], cur.sys[3], cur.draw,
                   cur.part[1], cur.part[2], cur.part[3], cur.jsx, cur.mousex);
    // `emine_perf mouse`: movement that stopped coming for a while and then came in a lump
    if (logMouse)
    {
        extern int emine_rawmouse;
        if ((cur.jsx && pageStill >= 4) || (cur.mousex && gameStill >= 4) || cur.jslag > 50 || cur.rawlag > 50)
            OSD_Printf("emine_perf: mouse after %d still frames (page) / %d (game): mousemove %d px in %d events, latest %d ms late; raw %d events, latest %d ms late; game %d (%s); frame gap %.1f ms\n",
                       pageStill, gameStill, cur.jsx, cur.jsn, cur.jslag, cur.rawn, cur.rawlag, cur.mousex,
                       emine_rawmouse ? "reads raw" : "reads mousemove", prev ? cur.end - prev->end : 0.0);
        pageStill = cur.jsx ? 0 : pageStill + 1;
        gameStill = cur.mousex ? 0 : gameStill + 1;
    }
    ring[head] = cur;
    head = (head + 1) % N;
    if (count < N) count++;
    cur = {};
}

static int osdcmd_emine_perf(osdcmdptr_t parm)
{
    if (parm->numparms >= 1 && !Bstrcasecmp(parm->parms[0], "clear")) { count = 0; head = 0; haveAng = false; OSD_Printf("emine_perf: cleared\n"); return OSDCMD_OK; }
    if (parm->numparms >= 1 && !Bstrcasecmp(parm->parms[0], "mouse")) { logMouse = !logMouse; pageStill = gameStill = 0; OSD_Printf("emine_perf: mouse stalls %s\n", logMouse ? "logged" : "not logged"); return OSDCMD_OK; }
    if (parm->numparms >= 1 && !Bstrcasecmp(parm->parms[0], "log")) { logLate = !logLate; OSD_Printf("emine_perf: late frames %s\n", logLate ? "logged" : "not logged"); return OSDCMD_OK; }
    if (count < 10) { OSD_Printf("emine_perf: not enough frames yet\n"); return OSDCMD_OK; }

    int const n = count - 1;  // intervals
    std::vector<int> idx(n);
    std::vector<float> iv(n);
    double tic = 0, draw = 0, wait = 0, sys[EMINE_PERF_SYSTEMS] = {}, part[5] = {};
    int tics = 0;
    for (int k = 0; k < n; k++)
    {
        Frame const &a = ring[(head + N - count + k) % N], &b = ring[(head + N - count + k + 1) % N];
        iv[k] = (float)(b.end - a.end), idx[k] = (head + N - count + k + 1) % N;
        tic += b.tic, draw += b.draw, wait += b.wait, tics += b.tics;
        for (int s = 0; s < EMINE_PERF_SYSTEMS; s++) sys[s] += b.sys[s];
        for (int s = 0; s < 5; s++) part[s] += b.part[s];
    }
    std::vector<float> sorted = iv;
    std::sort(sorted.begin(), sorted.end());
    float const med = sorted[n / 2], p95 = sorted[n * 95 / 100], mx = sorted[n - 1];
    double span = 0; for (float v : iv) span += v;
    int late = 0; for (float v : iv) late += v > med * 1.5f;

    OSD_Printf("emine_perf: %d frames over %.1f s at %dx%d: %.1f fps; frame gap median %.1f ms, 95%% %.1f ms, worst %.1f ms; %d late (over 1.5x median)\n",
               n, span / 1000.0, xdim, ydim, n * 1000.0 / span, med, p95, mx, late);
    OSD_Printf("emine_perf: per frame: drawing %.2f ms, waiting for the display %.2f ms, game tics %.2f ms (%.2f tics: mobs %.2f world %.2f food %.2f voice %.2f other %.2f)\n",
               draw / n, wait / n, tic / n, (double)tics / n, sys[0] / n, sys[1] / n, sys[2] / n, sys[3] / n, sys[4] / n);

    OSD_Printf("emine_perf: drawing per frame: input %.2f, world %.2f, HUD and weapon %.2f, present %.2f, sound %.2f ms\n",
               part[0] / n, part[1] / n, part[2] / n, part[3] / n, part[4] / n);

    // turning: frames where the view turned, how even the steps were per ms (a smooth turn is steady)
    int turned = 0, still = 0, withMouse = 0;
    double rate = 0, rate2 = 0;
    for (int k = 0; k < n; k++)
    {
        Frame const &f = ring[idx[k]];
        if (f.mousex) withMouse++;
        if (!f.dang) { still++; continue; }
        double const r = f.dang / 256.0 / iv[k];
        rate += r, rate2 += r * r, turned++;
    }
    if (turned > 5)
    {
        double const m = rate / turned, sd = sqrt(max(0.0, rate2 / turned - m * m));
        OSD_Printf("emine_perf: turning: %d frames turned (%d didn't, %d had mouse input); turn rate %.2f units/ms, unevenness %.0f%%\n",
                   turned, still, withMouse, m, m != 0 ? 100 * sd / fabs(m) : 0.0);
    }

    // the worst frames
    std::vector<int> order(n);
    for (int k = 0; k < n; k++) order[k] = k;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return iv[a] > iv[b]; });
    for (int j = 0; j < min(n, 6); j++)
    {
        Frame const &f = ring[idx[order[j]]];
        OSD_Printf("emine_perf:  %.1f ms gap: tics %d %.1f ms (mobs %.1f world %.1f food %.1f voice %.1f), draw %.1f ms, turned %.1f, mouse %d\n",
                   iv[order[j]], f.tics, f.tic, f.sys[0], f.sys[1], f.sys[2], f.sys[3], f.draw, f.dang / 256.0, f.mousex);
    }
    // the mouse, frame by frame: what the browser delivered to the page against what the game read. Steady movement
    // should show up in both every frame; runs of 0 in "page" mean the browser held it back, runs of 0 only in
    // "game" mean we did.
    {
        int const from = max(0, n - 40);
        char a[600], b[600];
        int la = Bsnprintf(a, sizeof(a), "emine_perf: mouse x per frame, page:"), lb = Bsnprintf(b, sizeof(b), "emine_perf: mouse x per frame, game:");
        for (int k = from; k < n; k++)
        {
            Frame const &f = ring[idx[k]];
            if (la < (int)sizeof(a) - 12) la += Bsnprintf(a + la, sizeof(a) - la, " %d", f.jsx);
            if (lb < (int)sizeof(b) - 12) lb += Bsnprintf(b + lb, sizeof(b) - lb, " %d", f.mousex);
        }
        OSD_Printf("%s\n%s\n", a, b);
        // longest runs with no movement while the mouse was moving around them
        int pageGap = 0, gameGap = 0, run1 = 0, run2 = 0, events = 0;
        for (int k = 0; k < n; k++)
        {
            Frame const &f = ring[idx[k]];
            events += f.jsn;
            run1 = f.jsx ? 0 : run1 + 1; run2 = f.mousex ? 0 : run2 + 1;
            if (f.jsx) gameGap = max(gameGap, run2);
            if (k + 1 < n && ring[idx[k + 1]].jsx) pageGap = max(pageGap, run1);
        }
        OSD_Printf("emine_perf: mouse: %d events to the page; longest stall: page %d frames, game %d frames (while moving)\n", events, pageGap, gameGap);
    }

    // the last few frames in order, to see the pattern of a turn
    char line[512];
    int len = Bsnprintf(line, sizeof(line), "emine_perf: last frames (gap ms/turned):");
    for (int k = max(0, n - 16); k < n && len < (int)sizeof(line) - 24; k++)
        len += Bsnprintf(line + len, sizeof(line) - len, " %.0f/%.1f", iv[k], ring[idx[k]].dang / 256.0);
    OSD_Printf("%s\n", line);
    (void)sysName;
    return OSDCMD_OK;
}

static int osdcmd_emine_rawmouse(osdcmdptr_t parm)
{
    extern int emine_rawmouse;
    if (parm->numparms >= 1) emine_rawmouse = !!Batol(parm->parms[0]);
    OSD_Printf("emine_rawmouse %d (%s)\n", emine_rawmouse,
               emine_rawmouse ? "mouse read from the page's pointerrawupdate total" : "mouse read from SDL's mousemove events");
    return OSDCMD_OK;
}

void Emine_RegisterPerfOSD(void)
{
    OSD_RegisterFunction("emine_rawmouse", "emine_rawmouse [0|1]: read the mouse from pointerrawupdate (1, default) or SDL's mousemove events (0)", osdcmd_emine_rawmouse);
    OSD_RegisterFunction("emine_perf", "emine_perf [log|mouse|clear]: frame timing over the last ~1000 frames (log: print each late frame; mouse: print each mouse stall and burst)", osdcmd_emine_perf);
}
