// Water Physics -- vehicles float, as plugins\mt_plugin_WaterPhysics.dll.
//
// The game has no buoyancy on vehicles, so they sink straight through the water. Each
// vehicle in the water gets a lift at four points of its footprint, each in proportion to
// how deep that point is, and a drag against its speed. Lifting at four points is what
// makes it tip and settle instead of bobbing like a cork.
//
// It lands as impulses (force x frame time) on a woken body: AddForceAtLocation at 1.3
// vehicle weights left a Raton motionless on the seabed.
//
// The surface height is SeaLevel in the ini. The map's WaterBodyOcean actor sits at 0 while
// the ground is around -22000, so it cannot be read from there. Park with the wheels at the
// water's edge and press Ctrl+F8: that saves it. Nothing floats until it is set.
//
// A vehicle takes on water: its lift fades over SinkSeconds in the water, so it floats, sits
// lower and lower, and goes down. Out of the water it drains twice as fast.
//
// Settings: Binaries\Win64\water_physics.ini, [WaterPhysics] Buoyancy (1.05: the lift on a
// fully sunk dry vehicle, in vehicle weights), SinkSeconds (30), Drag (0.8, per second),
// VerticalDrag (2.5, per second, what stops the bobbing), SeaLevel (cm).
//
// shortcut: one flat height for the whole map -- land lower than it counts as water, and
// lakes and rivers keep sinking. Use the WaterBody components once floating is proven.
// shortcut: impulses land once a frame at best, through the game thread queue. Physics
// substeps in between get none, so a very heavy vehicle may bob; move to a physics callback
// if that shows.
// shortcut: single player / host, wherever the vehicle is simulated.

#include <cmath>
#include <map>
#include <mutex>
#include <vector>
#include "../include/mt_plugin.hpp"

using namespace mt;

struct Vec { double x, y, z; };
struct Ref { int index; uintptr_t ptr; };
static bool alive(const Ref& r) { return r.ptr && api->object(r.index) == r.ptr; }

static uintptr_t fIsSim, fLoc, fForward, fRight, fVelocity, fBounds, fRoot, fMass, fForce;
static int oSimRet, oBoundsOrigin, oBoundsExtent, oForceLoc, oForceBone;
static float kBuoyancy, kDrag, kVerticalDrag, kSink;
static std::map<uintptr_t, double> soaked;  // vehicle -> seconds of water taken on
static std::map<uintptr_t, DWORD> dozing;   // vehicle -> when it could first have reached the water
static std::map<uintptr_t, double> bottomOff;  // vehicle -> actor z minus bounds bottom, from its last bounds call
static double seaLevel;
static bool haveSea;

static uintptr_t fWake;
static double dt;
static LARGE_INTEGER lastTick, hz;
static volatile LONG queued;  // one tick in the game-thread queue at a time

static std::mutex lock;
static DWORD lastTrace;
static bool traceDue, trace;
static uintptr_t fPawn, gameplayCDO;
static std::vector<Ref> vehicles;

static Vec callVec(uintptr_t obj, uintptr_t fn) {
    Vec v{};
    api->process_event(obj, fn, &v);
    return v;
}

static void push(uintptr_t root, const Vec& force, const Vec& at) {
    unsigned char parms[0x60] = {0};
    Vec impulse{force.x * dt, force.y * dt, force.z * dt};
    memcpy(parms, &impulse, sizeof impulse);
    memcpy(parms + oForceLoc, &at, sizeof at);
    api->process_event(root, fForce, parms);
}

static uintptr_t pawnOf(uintptr_t context) {
    struct { uintptr_t context; int index; uintptr_t ret; } p = {context, 0, 0};
    api->process_event(gameplayCDO, fPawn, &p);
    return p.ret;
}

// A dry vehicle is looked at again after the time it needs to reach the water falling at
// 40 m/s, but never sooner than 0.25 s (staggered by object index so they do not all wake on one
// frame) and never later than 3 s. That, not the physics, was the plugin's cost on land: every
// vehicle near sea level paid a GetActorBounds every frame or two.
static void doze(uintptr_t v, int index, DWORD clock, double above) {
    double ms = above / 4, floor = 250 + index % 250;
    dozing[v] = clock + (DWORD)(ms < floor ? floor : ms > 3000 ? 3000 : ms);
}

static void floatOne(uintptr_t v, int index) {
    DWORD clock = GetTickCount();
    auto wait = dozing.find(v);
    if (!trace && wait != dozing.end() && (int)(wait->second - clock) > 0) return;
    Vec loc = callVec(v, fLoc);
    auto off = bottomOff.find(v);
    // Bounds are the dear call: a vehicle clearly above the sea (3 m of margin for tilt) skips it.
    if (!trace && off != bottomOff.end() && !soaked.count(v) && loc.z - off->second - 300 > seaLevel) {
        doze(v, index, clock, loc.z - off->second - seaLevel);
        return;
    }
    unsigned char b[0x60] = {0};
    b[0] = 1;  // bOnlyCollidingComponents
    api->process_event(v, fBounds, b);
    Vec org, ext; memcpy(&org, b + oBoundsOrigin, sizeof org); memcpy(&ext, b + oBoundsExtent, sizeof ext);
    double bottom = org.z - ext.z;  // the bounds' own centre: the actor origin need not be mid-height
    bottomOff[v] = loc.z - bottom;
    if (trace) MT_LOG("%s z %.0f bottom %.0f sea %.0f extent %.0f %.0f %.0f", objName(classOf(v)).c_str(), loc.z, bottom, seaLevel, ext.x, ext.y, ext.z);
    if (bottom > seaLevel) {
        auto dry = soaked.find(v);
        if (dry != soaked.end() && (dry->second -= dt * 2) <= 0) soaked.erase(dry);
        if (dry == soaked.end()) doze(v, index, clock, bottom - seaLevel);
        return;
    }

    uintptr_t root = 0;
    api->process_event(v, fRoot, &root);
    struct { float ret; } mass = {0};
    if (root) api->process_event(root, fMass, &mass);
    if (trace) {
        unsigned char sim[0x20] = {0};
        if (root) api->process_event(root, fIsSim, sim);
        MT_LOG("  in water: root %s mass %.0f simulating %d", root ? objName(classOf(root)).c_str() : "none", mass.ret, sim[oSimRet]);
    }
    if (!root || mass.ret <= 0 || ext.z <= 0) return;
    // Parked and far-off traffic is not simulated: pushing it does nothing, and waking it
    // every frame is what made "land lower than the sea" cost frames.
    unsigned char sim[0x20] = {0};
    api->process_event(root, fIsSim, sim);
    if (!sim[oSimRet]) { doze(v, index, clock, 0); return; }

    api->process_event(root, fWake, nullptr);
    Vec f = callVec(v, fForward), r = callVec(v, fRight), vel = callVec(v, fVelocity);
    double a = ext.x > ext.y ? ext.x : ext.y, c = ext.x > ext.y ? ext.y : ext.x;
    const double g = 980;
    double& soak = soaked[v];
    soak += dt;
    double lift = kBuoyancy * (soak < kSink ? 1 - soak / kSink : 0);
    double sunk = 0;
    for (int i = 0; i < 4; i++) {
        double sa = i < 2 ? a : -a, sc = (i & 1) ? c : -c;
        Vec p{org.x + f.x * sa + r.x * sc, org.y + f.y * sa + r.y * sc, org.z + f.z * sa + r.z * sc};
        double d = (seaLevel - (p.z - ext.z)) / (2 * ext.z);
        d = d < 0 ? 0 : d > 1 ? 1 : d;
        sunk += d / 4;
        if (d > 0) push(root, {0, 0, mass.ret * g * lift * d / 4}, p);
    }
    if (trace) MT_LOG("  pushing: sunk %.2f, lift %.2f, velocity z %.0f", sunk, lift, vel.z);
    double k = -mass.ret * kDrag * sunk, kz = -mass.ret * kVerticalDrag * sunk;
    push(root, {vel.x * k, vel.y * k, vel.z * kz}, org);
}

static void tick(void*) {
    std::lock_guard<std::mutex> g(lock);
    InterlockedExchange(&queued, 0);
    // QPC, not GetTickCount: its 16 ms steps made dt 0 one tick and double the next, so the
    // lift came in alternating kicks.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    dt = lastTick.QuadPart ? (double)(now.QuadPart - lastTick.QuadPart) / hz.QuadPart : 0;
    dt = dt > 0.05 ? 0.05 : dt;  // a hitch must not land as one huge kick
    lastTick = now;
    traceDue = loggingOn() && GetTickCount() - lastTrace > 3000;  // what the plugin sees of the player's vehicle
    if (traceDue) lastTrace = GetTickCount();
    uintptr_t pawn = 0;
    if (dozing.size() > 4000) { dozing.clear(); bottomOff.clear(); }  // cars come and go
    for (auto& r : vehicles) {
        if (!alive(r)) continue;
        if (traceDue && !pawn) pawn = pawnOf(r.ptr);
        trace = traceDue && r.ptr == pawn;
        floatOne(r.ptr, r.index);
    }
}

static void calibrate(void*) {
    std::lock_guard<std::mutex> g(lock);
    uintptr_t pawn = 0;
    for (auto& r : vehicles)
        if (alive(r)) { pawn = pawnOf(r.ptr); break; }
    if (!pawn) { MT_LOG("Ctrl+F8: no player found"); return; }
    unsigned char b[0x60] = {0};
    b[0] = 1;
    api->process_event(pawn, fBounds, b);
    Vec ext; memcpy(&ext, b + oBoundsExtent, sizeof ext);
    seaLevel = callVec(pawn, fLoc).z - ext.z;
    haveSea = true;
    wchar_t v[32];
    swprintf_s(v, L"%.0f", seaLevel);
    iniSet(L"water_physics.ini", L"WaterPhysics", L"SeaLevel", v, true);
    MT_LOG("Ctrl+F8: sea level saved as %.0f, from %s", seaLevel, objName(classOf(pawn)).c_str());
}

// The tick is queued from a thread of its own: the object scan in run() takes a large part of a second,
// and queuing from the scanning thread stalled the tick for that long each time.
static DWORD WINAPI pump(LPVOID) {
    for (;;) {  // one tick at a time (under 60 fps several in one drain each pushed again)
        if (haveSea && api->game_thread_ready() && !InterlockedCompareExchange(&queued, 1, 0)) api->run_on_game_thread(tick, nullptr);
        Sleep(16);
    }
}

static DWORD WINAPI run(LPVOID) {
    MT_LOG("Water Physics loaded");
    kBuoyancy = (float)iniNum(L"water_physics.ini", L"WaterPhysics", L"Buoyancy", 1.05);
    kDrag = (float)iniNum(L"water_physics.ini", L"WaterPhysics", L"Drag", 0.8);
    kVerticalDrag = (float)iniNum(L"water_physics.ini", L"WaterPhysics", L"VerticalDrag", 2.5);
    kSink = (float)iniNum(L"water_physics.ini", L"WaterPhysics", L"SinkSeconds", 30);
    double sea = iniNum(L"water_physics.ini", L"WaterPhysics", L"SeaLevel", 0);
    if (sea != 0) { seaLevel = sea; haveSea = true; }

    QueryPerformanceFrequency(&hz);
    if (!api->wait_engine(INFINITE)) { MT_LOG("FAILED: no engine"); return 0; }
    uintptr_t cVeh = api->find_object("Class", "MTVehicle", "/Script/MotorTown");
    fLoc = api->find_object("Function", "K2_GetActorLocation", "Actor");
    fForward = api->find_object("Function", "GetActorForwardVector", "Actor");
    fRight = api->find_object("Function", "GetActorRightVector", "Actor");
    fVelocity = api->find_object("Function", "GetVelocity", "Actor");
    fBounds = api->find_object("Function", "GetActorBounds", "Actor");
    fRoot = api->find_object("Function", "K2_GetRootComponent", "Actor");
    fMass = api->find_object("Function", "GetMass", "PrimitiveComponent");
    fForce = api->find_object("Function", "AddImpulseAtLocation", "PrimitiveComponent");
    fWake = api->find_object("Function", "WakeAllRigidBodies", "PrimitiveComponent");
    fPawn = api->find_object("Function", "GetPlayerPawn", "GameplayStatics");
    gameplayCDO = api->find_object("GameplayStatics", "Default__GameplayStatics", "/Script/Engine");
    fIsSim = api->find_object("Function", "IsSimulatingPhysics", "SceneComponent");
    oSimRet = off(fIsSim, "ReturnValue");
    oBoundsOrigin = off(fBounds, "Origin"); oBoundsExtent = off(fBounds, "BoxExtent");
    oForceLoc = off(fForce, "Location"); oForceBone = off(fForce, "BoneName");
    uintptr_t need[] = {fWake, cVeh, fPawn, gameplayCDO, fIsSim, fLoc, fForward, fRight, fVelocity, fBounds, fRoot, fMass, fForce};
    bool ok = oBoundsExtent >= 0 && oForceLoc >= 0 && oSimRet >= 0;
    for (uintptr_t p : need) ok = ok && p;
    if (!ok) { MT_LOG("FAILED: functions not found -- vehicles sink as before"); return 0; }

    bool keyDown = false;
    CloseHandle(CreateThread(nullptr, 0, pump, nullptr, 0, nullptr));
    for (int beat = 0;; beat++) {
        if (beat % 300 == 0) {  // about five seconds
            std::vector<Ref> v;
            for (int i = 0, n = api->num_objects(); i < n; i++) {
                uintptr_t o = api->object(i);
                if (o && !isCDO(o) && api->is_a(classOf(o), cVeh)) v.push_back({i, o});
            }
            std::lock_guard<std::mutex> g(lock);
            vehicles.swap(v);
        }
        bool key = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F8) & 0x8000);
        if (key && !keyDown && api->game_thread_ready()) api->run_on_game_thread(calibrate, nullptr);
        keyDown = key;
        Sleep(16);
    }
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    if (iniDisabled(L"water_physics.ini")) return 0;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
