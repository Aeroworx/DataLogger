// Copyright (c) 2015 Joseph D Poirier
// Distributable under the terms of The Simplified BSD License
// that can be found in the LICENSE file.

#ifdef _WIN32 /* this is set for 64 bit as well */
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
#endif

#ifdef _APPLE_
 #pragma clang diagnostic ignored "-Wall"
 #include <gl.h>
#else
 //#include <GL/gl.h>
#endif

// #include <stdlib.h>
// #include <stdio.h>
// #include <math.h>

#ifdef _WIN32
#include <io.h>
#define ACCESS _access
#else
#include <unistd.h>
#define ACCESS access
#endif

#include <sys/stat.h>
#include <string>
#include <time.h>
#include <fstream>
#include <atomic>
#include <cstring>
#include <algorithm>

#include "./SDK/CHeaders/XPLM/XPLMPlugin.h"
#include "./SDK/CHeaders/XPLM/XPLMProcessing.h"
#include "./SDK/CHeaders/XPLM/XPLMDataAccess.h"
#include "./SDK/CHeaders/XPLM/XPLMUtilities.h"
#include "./SDK/CHeaders/XPLM/XPLMDisplay.h"
#include "./SDK/CHeaders/XPLM/XPLMGraphics.h"

// #include "readerwriterqueue.h"
#include "./include/defs.h"
#include "./include/main.h"


using namespace std;
// using namespace moodycamel;

static void get_line(istream &is, string &s);
static bool error_bit_set(ifstream* f);
static bool dir_exists(const string &dir);
static void enableLogging(void);
static void disableLogging(void);
static bool openLogFile(void);
static void closeLogFile(void);
static const string currentDateTime(bool useDash);
static void writeFileProlog(const string &t);
static void writeFileEpilog(void);
static void writeData(double lat, double lon, double alt, const string &t);
static void DrawWindowCallback(XPLMWindowID inWindowID, void* inRefcon);
static void HandleKeyCallback(XPLMWindowID inWindowID, char inKey,
                              XPLMKeyFlags inFlags, char inVirtualKey,
                              void* inRefcon, int losingFocus);
static int HandleMouseCallback(XPLMWindowID inWindowID, int x, int y,
                               XPLMMouseStatus inMouse, void* inRefcon);
static float LoggerCallback(float inElapsedSinceLastCall,
                            float inElapsedTimeSinceLastFlightLoop,
                            int inCounter, void* inRefcon);
static float StatusCheckCallback(float inElapsedSinceLastCall,
                                 float inElapsedTimeSinceLastFlightLoop,
                                 int inCounter, void* inRefcon);

// To define, pass -DVERSION=vX.Y.X when building,
// e.g. in a make file
#ifndef VERSION
#define VERSION "vUNKNOWN"
#endif

// sigh. two levels of macros needed to stringify
// the result of expansion of a macro argument
#define STR(v) "DataLogger " #v  "  " __DATE__ " (jdpoirier@gmail.com)\0"
#define DESC(v) STR(v)

#define STRING2(x) #x
#define STRING(x) STRING2(x)

#pragma message (DESC(VERSION))


static XPLMWindowID gDataLogWindow = NULL;
static atomic<bool> gPluginEnabled(false);
// static atomic<int> gPlaneLoaded(0);

// time interval > 0.0 (no callback) > flight loop frame rate
static atomic<float> gFlCbInterval(0.100f); // 10Hz update rate?

#define WINDOW_WIDTH (220)
#define WINDOW_HEIGHT (15)
static int gLogWinPosX;
static int gLogWinPosY;
static int gLastMouseX;
static int gLastMouseY;

// general & misc
enum {
    PLUGIN_PLANE_ID = 0
    ,DATALOGGER_WINDOW
};

XPLMDataRef gs_dref = NULL;
XPLMDataRef lat_dref = NULL;
XPLMDataRef lon_dref = NULL;
XPLMDataRef alt_dref = NULL;

static const string gLogFileName = "DataLogPath.txt";
static string gLogFilePath = "";

static atomic<bool> gLogging(false);
static atomic<bool> gFileOpenErr(false);
static atomic<bool> gFlashUI(false);
static atomic<bool> gFlashUIMsgOn(false);
static atomic<int> gLogStatIndCnt(1);
static ofstream gFd;

XPLMDataRef panel_visible_win_t_dataref;

/**
 *
 */
PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc)
{
    LPRINTF("DataLogger Plugin: XPluginStart\n");
    // const string outname = "DataLogger\0";
    // const string outsig = "jdp.data.logger\0";
    // const string outdesc = string("DataLogger ") + string("VERSION") +
    //                               string(" ") + string(__DATE__) +
    //                               string(" (jdpoirier@gmail.com)\0");
    // copy_n(outName, outname.length()+1, (char*)outname.c_str());
    // copy_n(outSig , outsig.length()+1, (char*)outsig.c_str());
    // copy_n(outDesc, outdesc.length()+1, (char*)outdesc.c_str());
    strcpy(outName, "DataLogger");
    strcpy(outSig , "jdp.data.logger");
    strcpy(outDesc, DESC(VERSION));

    ifstream pathFile;
    pathFile.open(gLogFileName, ofstream::app);
    if (pathFile.is_open() && !error_bit_set(&pathFile)) {
        string path;
        // get_line(pathFile, path);
        if (getline(pathFile, path)) {
            replace(path.begin(), path.end(), '\\', '/');
            if (path.back() != '/')
                path.append("/");
            if (dir_exists(path))
                gLogFilePath = path;
            // LPRINTF(path.c_str()); LPRINTF("\n");
            // LPRINTF(gLogFilePath.c_str()); LPRINTF("\n");
        } else {
            LPRINTF("DataLogger Plugin: getline failed... \n");
        }
        pathFile.close();
    }

    gs_dref = XPLMFindDataRef("sim/flightmodel/position/groundspeed");
    lat_dref = XPLMFindDataRef("sim/flightmodel/position/latitude");
    lon_dref = XPLMFindDataRef("sim/flightmodel/position/longitude");
    alt_dref = XPLMFindDataRef("sim/flightmodel/position/elevation");
    XPLMRegisterFlightLoopCallback(StatusCheckCallback, 5.0, NULL);
    panel_visible_win_t_dataref = XPLMFindDataRef("sim/graphics/view/panel_visible_win_t");
    int top = (int)XPLMGetDataf(panel_visible_win_t_dataref);
    gLogWinPosX = 0;
    gLogWinPosY = top - 150;
    gDataLogWindow = XPLMCreateWindow(gLogWinPosX,                  // left
                                      gLogWinPosY,                  // top
                                      gLogWinPosX+WINDOW_WIDTH,     // right
                                      gLogWinPosY-WINDOW_HEIGHT,    // bottom
                                      true,                         // is visible
                                      DrawWindowCallback,
                                      HandleKeyCallback,
                                      HandleMouseCallback,
                                      (void*)DATALOGGER_WINDOW);  // Refcon
    LPRINTF("DataLogger Plugin: startup completed\n");
    return PROCESSED_EVENT;
}

void get_line(istream &is, string &s)
{
    char ch;
    s.clear();
    // while (is.good())
        while (is.get(ch) && ch != '\n' && ch != '\r')
            s += ch;
}

bool error_bit_set(ifstream* f)
{
    if (f->eof()) {
        LPRINTF("DataLogger Plugin: stream eofbit error state...\n");
        // EOF after std::getline() is not the criterion to stop processing
        // data: In case there is data between the last delimiter and EOF,
        // getline() extracts it and sets the eofbit.
        return true;
    } else if (f->fail()) {
        LPRINTF("DataLogger Plugin: stream failbit (or badbit) error state...\n");
        return true;
    } else if (f->bad()) {
        LPRINTF("DataLogger Plugin: stream badbit error state\n");
        return true;
    }
    return false;
}

/**
 *
 */
bool dir_exists(const string &dir)
{
    if (dir.empty())
        return false;
    if (!ACCESS(dir.c_str(), 0)) {
        return true;
    } else {
        LPRINTF("DataLogger Plugin: file access failed...\n");
    }
    return false;
}

/**
 *
 */
bool openLogFile(void)
{
    if (gFd.is_open())
        closeLogFile();

    string t = currentDateTime(true);
    string f = string("DataLog-") +  t + string(".gpx");
    string file = gLogFilePath + f;

    // LPRINTF(file.c_str()); LPRINTF("\n");

    gFd.open(file, ofstream::app); // creates the file if it doesn't exist
    if (!gFd.is_open()) {
        LPRINTF("DataLogger Plugin: unable to open the output file ");
        LPRINTF(file.c_str()); LPRINTF("\n");
        LPRINTF("DataLogger Plugin: trying to open the base file...\n");
        gFd.open(f, ofstream::app);
        if (!gFd.is_open()) {
            LPRINTF("DataLogger Plugin: couldn't open the base file either...\n");
            return false;
        } else {
            gLogFilePath = "";
        }
    }
    writeFileProlog(t);
    return true;
}

/**
 *
 */
void closeLogFile(void)
{
    if (gFd.is_open()) {
        writeFileEpilog();
        gFd.close();
    }
}

/**
 *
 */
void writeFileProlog(const string &t)
{
    gFd << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    gFd << "<gpx version=\"1.0\">\n";
    gFd << "<metadata>\n";
    gFd << "<time>" << t << "</time>\n";
    gFd << "</metadata>\n";
    gFd << "<trk><name>DataLogger plugin</name><trkseg>\n";
}

/**
 *
 */
void writeFileEpilog(void)
{
    gFd << "</trkseg></trk>\n";
    gFd << "</gpx>\n";
}

/**
 *
 */
void writeData(double lat, double lon, double alt, const string &t)
{
    static double lat_ = 0.0;
    static double lon_ = 0.0;
    static double alt_ = 0.0;

    if (lat == lat_ && lon == lon_ && alt == alt_)
        return;

    lat_ = lat;
    lon_ = lon;
    alt_ = alt;

    // <trkpt lat="46.57608333" lon="8.89241667"><ele>2376.640205</ele></trkpt>
    gFd << "<trkpt lat=\""
        << to_string(lat)
        << "\" lon=\""
        << to_string(lon)
        << "\"><ele>"
        << to_string(alt)
        // << to_string(static_cast<int>(alt))
        // << to_string(static_cast<int>(alt/METERS_PER_FOOT))
        << "</ele><time>"
        << t
        << "</time></trkpt>\n";
}

/**
 * Returns the current date and Zulu time.
 *
 * @return
 *      if useDash: YYYY-MM-DDTHH-MM-SSZ
 *      else:       YYYY-MM-DDTHH:MM:SSZ
 */
const string currentDateTime(bool useDash)
{
    time_t now = time(0);
    string buf(22,  '\0');
    // strftime(buf, sizeof(buf), "%FT%XZ", gmtime(&now));
    // strftime(buf, sizeof(buf), "%FT%XZ", localtime(&now));
    if (useDash)
        strftime((char*)buf.c_str(), buf.length(), "%Y-%m-%dT%H-%M-%SZ", gmtime(&now));
    else
        strftime((char*)buf.c_str(),  buf.length(), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    return buf.substr(0, 20);
}

/**
 *
 */
#define UI_FLASHTIME (20)
#define GRNDSPEED_THRESH (2)
#define MOVING_THRESH (5)
#define LOGSTAT_IND_THRESH (10)
float StatusCheckCallback(float inElapsedSinceLastCall,
                          float inElapsedTimeSinceLastFlightLoop, int inCounter,
                          void* inRefcon)
{
    static int cnt = 0;
    static bool doGrndCheck = true;

    if (!gPluginEnabled.load()) {
        // LPRINTF("DataLogger Plugin: StatusCheckCallback...\n");
        return 10.0;
    }

    float cb_after = 1.0;
    if (!gLogging.load()) {
        if (gFlashUI.load()) {
            cnt += 1;
            if (cnt >= UI_FLASHTIME) {
                cnt = 0;
                gFlashUI.store(false);
                gFlashUIMsgOn.store(false);
            }
            gFlashUIMsgOn.store(!gFlashUIMsgOn.load());
            cb_after = 0.5;
        } else if (doGrndCheck) {
            int gs = (int)XPLMGetDataf(gs_dref);
            if (gs > GRNDSPEED_THRESH) {
                cnt += 1;
            } else {
                cnt = 0;
                doGrndCheck = true;
                gFlashUI.store(false);
            }
            if (cnt >= MOVING_THRESH) {
                cnt = 0;
                gFlashUI.store(true);
                doGrndCheck = false;
            }
            cb_after = 1.0;
        } else {
            cb_after = 2.0;
        }
        gLogStatIndCnt.store(1);
        return cb_after;
    } else {
        gLogStatIndCnt.fetch_add(1);
        if (gLogStatIndCnt.load() > LOGSTAT_IND_THRESH)
            gLogStatIndCnt.store(1);
        cnt = 0;
        doGrndCheck = true;
        cb_after = 1.0;
    }
    return cb_after;
}

/**
 *
 */
float LoggerCallback(float inElapsedSinceLastCall,
                     float inElapsedTimeSinceLastFlightLoop,
                     int inCounter, void* inRefcon)
{
    if (!gPluginEnabled.load() || !gLogging.load()) {
        return 0.0;  // disable the callback
    }
    // LPRINTF("DataLogger Plugin: LoggerCallback writing data...\n");
    writeData(XPLMGetDataf(lat_dref),
                XPLMGetDataf(lon_dref),
                XPLMGetDataf(alt_dref),
                currentDateTime(false));
    return gFlCbInterval.load();
}

/**
 *
 */
void enableLogging(void){
    if (openLogFile()) {
        gLogging.store(true);
        XPLMRegisterFlightLoopCallback(LoggerCallback, -1.0, NULL);
    } else {
        gFileOpenErr.store(true);
    }
}

/**
 *
 */
void disableLogging(void)
{
    gLogging.store(false);
    XPLMUnregisterFlightLoopCallback(LoggerCallback, NULL);
    closeLogFile();
}

/**
 *
 */
PLUGIN_API void XPluginStop(void)
{
    gPluginEnabled.store(false);
    gLogging.store(false);
    XPLMUnregisterFlightLoopCallback(LoggerCallback, NULL);
    closeLogFile();
    XPLMUnregisterFlightLoopCallback(StatusCheckCallback, NULL);
    LPRINTF("DataLogger Plugin: XPluginStop\n");
}

/**
 *
 */
PLUGIN_API void XPluginDisable(void)
{
    gPluginEnabled.store(false);
    disableLogging();
    LPRINTF("DataLogger Plugin: XPluginDisable\n");
}

/**
 *
 */
PLUGIN_API int XPluginEnable(void)
{
    gPluginEnabled.store(true);
     LPRINTF("DataLogger Plugin: XPluginEnable\n");
    return PROCESSED_EVENT;
}

/**
 *
 */
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFrom, long inMsg, void* inParam)
{
    if (inFrom == XPLM_PLUGIN_XPLANE) {
        // size_t inparam = reinterpret_cast<size_t>(inParam);
        switch (inMsg) {
        case XPLM_MSG_PLANE_LOADED:
            // gPlaneLoaded.store();
            // LPRINTF("DataLogger Plugin: XPluginReceiveMessage XPLM_MSG_PLANE_LOADED\n");
            break;
        case XPLM_MSG_AIRPORT_LOADED:
            // LPRINTF("DataLogger Plugin: XPluginReceiveMessage XPLM_MSG_AIRPORT_LOADED\n");
            break;
        case XPLM_MSG_SCENERY_LOADED:
            // LPRINTF("DataLogger Plugin: XPluginReceiveMessage XPLM_MSG_SCENERY_LOADED\n");
            break;
        case XPLM_MSG_AIRPLANE_COUNT_CHANGED:
            // LPRINTF("DataLogger Plugin: XPluginReceiveMessage XPLM_MSG_AIRPLANE_COUNT_CHANGED\n");
            break;
        case XPLM_MSG_PLANE_CRASHED:
            // XXX: system state and procedure, what's difference between
            // an unloaded and crashed plane?
            // LPRINTF("DataLogger Plugin: XPluginReceiveMessage XPLM_MSG_PLANE_CRASHED\n");
            break;
        case XPLM_MSG_PLANE_UNLOADED:
            // gPlaneLoaded.store();
            // LPRINTF("DataLogger Plugin: XPluginReceiveMessage XPLM_MSG_PLANE_UNLOADED\n");
            break;
        default:
            // unknown, anything to do?
            break;
        } // switch (inMsg)
    } // if (inFrom == XPLM_PLUGIN_XPLANE)
}

/**
 *
 */
#define FILEERR_OFF_THRESH (120)
void DrawWindowCallback(XPLMWindowID inWindowID, void* inRefcon)
{
    // RGB: White [1.0, 1.0, 1.0], Lime Green [0.0, 1.0, 0.0]
    static float datalogger_color[] = {0.0, 1.0, 0.0};
    static int errCnt = 0;

    if (inWindowID != gDataLogWindow)
        return;

    int left;
    int top;
    int right;
    int bottom;

    // XXX: are inWindowIDs our XPLMCreateWindow return pointers
    XPLMGetWindowGeometry(inWindowID, &left, &top, &right, &bottom);
    XPLMDrawTranslucentDarkBox(left, top, right, bottom);

    string str1;
    switch (reinterpret_cast<size_t>(inRefcon)) {
    case DATALOGGER_WINDOW:
        if (gLogging.load()) {
            str1 = "Data Logger :: Enabled " + string(gLogStatIndCnt.load(), '.');
        } else {
            if (gFileOpenErr.load()) {
                errCnt += 1;
                str1 = "Data Logger :: Error Opening File.";
                if (errCnt >= FILEERR_OFF_THRESH) {
                    errCnt = 0;
                    gFileOpenErr.store(false);
                }
            } else {
                if (gFlashUI.load()) {
                    if (gFlashUIMsgOn.load()) {
                        str1 = "Data Logger :: Click To Enable...";
                    } else {
                        str1 = "Data Logger ::";
                    }
                } else {
                    str1 = "Data Logger :: Click To Enable...";
                }
            }
        }
        XPLMDrawString(datalogger_color,
                       left+4,
                       top-10,
                       (char*)str1.c_str(),
                       NULL,
                       xplmFont_Basic);
        break;
    default:
        break;
    }
}

/**
 *
 */
void HandleKeyCallback(XPLMWindowID inWindowID, char inKey, XPLMKeyFlags inFlags,
                       char inVirtualKey, void* inRefcon, int losingFocus)
{
    if (inWindowID != gDataLogWindow)
        return;
}

/*
 *
 *
 */
int HandleMouseCallback(XPLMWindowID inWindowID, int x, int y,
                        XPLMMouseStatus inMouse, void* inRefcon)
{
    // static int com_changed = COMMS_UNCHANGED;
    static int MouseDownX;
    static int MouseDownY;

    if (inWindowID != gDataLogWindow)
        return IGNORED_EVENT;

    switch (inMouse) {
    case xplm_MouseDown:
        // if ((x >= gLogWinPosX+WINDOW_WIDTH-8) &&
        //     (x <= gLogWinPosX+WINDOW_WIDTH) &&
        //     (y <= gLogWinPosY) && (y >= gLogWinPosY-8)) {
        //         windowCloseRequest = 1;
        //     } else {
                MouseDownX = gLastMouseX = x;
                MouseDownY = gLastMouseY = y;
        // }
        break;
    case xplm_MouseDrag:
        // this event fires while xplm_MouseDown is active
        // and whether or not the window is being dragged
        gLogWinPosX += (x - gLastMouseX);
        gLogWinPosY += (y - gLastMouseY);
        XPLMSetWindowGeometry(gDataLogWindow,
                              gLogWinPosX,
                              gLogWinPosY,
                              gLogWinPosX+WINDOW_WIDTH,
                              gLogWinPosY-WINDOW_HEIGHT);
        gLastMouseX = x;
        gLastMouseY = y;
        break;
    case xplm_MouseUp:
        // Ignore mouse-clicks for a short time
        // when there was a previous open file error.
        if (gFileOpenErr.load())
            break;
        if (MouseDownX == x || MouseDownY == y) {
            if (gLogging.load())
                disableLogging();
            else
                enableLogging();
        }
        break;
    } // switch (inMouse)
    return PROCESSED_EVENT;
}
