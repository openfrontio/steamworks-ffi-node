// Linux OpenGL (GLX) Overlay for Steam Integration
// Uses glXSwapBuffers hook in gameoverlayrenderer.so to enable Steam overlay (Shift+Tab).

#if defined(__linux__) && !defined(__ANDROID__)

#include <node_api.h>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <ctime>

// X11 and OpenGL includes
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xcomposite.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <dlfcn.h>

// Global debug flag - controlled from JavaScript via SteamLogger
static bool g_debugMode = false;

// Debug logging macro - only logs when debug mode is enabled
#define OverlayLog(fmt, ...) do { if (g_debugMode) { printf("[Linux Overlay] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } } while(0)
#define OverlayLogError(fmt, ...) do { printf("[Linux Overlay] ERROR: " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while(0)

// OpenGL extensions for modern texture formats
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

// GLX extension function types
typedef GLXContext (*glXCreateContextAttribsARBProc)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
typedef void (*glXSwapIntervalEXTProc)(Display*, GLXDrawable, int);

// Whether Steam's overlay interposer is in our glXSwapBuffers call path, which
// is a different question from whether its library is loaded.
//
// The overlay works by LD_PRELOADing gameoverlayrenderer.so, which places it at
// the FRONT of the global symbol scope so glXSwapBuffers resolves to Steam's
// copy instead of the driver's. A library that is merely *mapped* -- dlopen'd
// after startup, say -- is appended to that scope rather than inserted at the
// front, so the symbol still binds to the driver and the overlay never draws,
// even though the library is plainly there in /proc/self/maps.
//
// So presence and interposition have to be reported separately. dladdr() on the
// globally-bound symbol answers the second one: it names the object that this
// addon's own glXSwapBuffers call resolves through, since that call goes through
// the ordinary global scope.
//
// Note what it does *not* tell you: it reports the global binding, not the
// target of an arbitrary call site. Code that deliberately bypasses the loader,
// by calling a pointer from dlsym(handle, "glXSwapBuffers"), would enter Steam's
// copy while this still reported libGL. Nothing here does that -- and such a
// call would not draw anyway, because Steam's interposer forwards through
// dlsym(RTLD_NEXT, ...), which resolves to nothing when the library is loaded
// last -- so "will the overlay draw" is still answered correctly either way.
enum class OverlayHookState { Active, MappedNotInterposing, NotPresent };

static const char* overlayHookStateName(OverlayHookState s) {
    switch (s) {
        case OverlayHookState::Active:               return "active";
        case OverlayHookState::MappedNotInterposing: return "mapped-not-interposing";
        default:                                     return "not-present";
    }
}

// Is Steam's overlay library mapped into this process at all?
//
// Matching "gameoverlayrenderer64" here never worked: that is the Windows
// filename. On Linux the library is <steam>/ubuntu12_64/gameoverlayrenderer.so
// -- the "64" is in the directory, not the filename -- so the old substring
// never matched and the "hook inactive" warning fired unconditionally.
static bool overlayLibraryMapped(std::string* mappingOut) {
    FILE* maps = fopen("/proc/self/maps", "r");
    if (!maps) return false;

    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), maps)) {
        if (strstr(line, "gameoverlayrenderer")) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (mappingOut) *mappingOut = line;
            found = true;
            break;
        }
    }
    fclose(maps);
    return found;
}

static OverlayHookState queryOverlayHookState(std::string* mappingOut, std::string* ownerOut) {
    std::string mapping;
    const bool mapped = overlayLibraryMapped(&mapping);
    if (mappingOut) *mappingOut = mapping;

    // What a glXSwapBuffers call resolves to right now, in this process.
    void* resolved = dlsym(RTLD_DEFAULT, "glXSwapBuffers");
    Dl_info info;
    const char* owner = nullptr;
    if (resolved && dladdr(resolved, &info) && info.dli_fname) owner = info.dli_fname;
    if (ownerOut) *ownerOut = owner ? owner : "";

    if (owner && strstr(owner, "gameoverlayrenderer")) return OverlayHookState::Active;
    return mapped ? OverlayHookState::MappedNotInterposing : OverlayHookState::NotPresent;
}

static OverlayHookState reportOverlayHookState() {
    std::string mapping, owner;
    const OverlayHookState state = queryOverlayHookState(&mapping, &owner);
    const char* boundTo = owner.empty() ? "an unidentified object" : owner.c_str();

    switch (state) {
        case OverlayHookState::Active:
            printf("[Linux Overlay] Steam overlay hook ACTIVE: glXSwapBuffers -> %s\n", boundTo);
            break;
        case OverlayHookState::MappedNotInterposing:
            printf("[Linux Overlay] WARNING: gameoverlayrenderer.so is mapped but NOT interposing "
                   "— glXSwapBuffers binds to %s. It reached this process too late to enter the "
                   "global symbol scope ahead of libGL; only LD_PRELOAD at exec can do that. The "
                   "overlay will not draw.\n", boundTo);
            printf("[Linux Overlay]   mapping: %s\n", mapping.c_str());
            break;
        case OverlayHookState::NotPresent:
            printf("[Linux Overlay] WARNING: gameoverlayrenderer.so is not loaded — this process "
                   "was not launched by Steam with the overlay enabled. The overlay will not "
                   "draw.\n");
            break;
    }
    fflush(stdout);
    return state;
}

// Xlib's default error handler calls exit(). A library making X calls on behalf
// of a host application must never let that happen: a bad window id should cost
// the overlay, not the entire application.
//
// This is not hypothetical. getNativeWindowHandle() only returns an X window id
// when Electron is running on X11; under Ozone/Wayland it decodes to something
// else entirely (observed: 1). XChangeProperty on that raises BadWindow, and the
// default handler terminates the process -- so on native Wayland the app dies
// during overlay attach, before addElectronSteamOverlay() returns.
static int overlayXErrorHandler(Display* dpy, XErrorEvent* error) {
    char text[256];
    text[0] = 0;
    XGetErrorText(dpy, error->error_code, text, sizeof(text) - 1);
    printf("[Linux Overlay] X error suppressed: %s (request %d.%d, resource 0x%lx)\n",
           text, (int)error->request_code, (int)error->minor_code,
           (unsigned long)error->resourceid);
    fflush(stdout);
    return 0;
}

// Installs the handler for the duration of our own X calls and restores whatever
// was there before.
//
// Scope caveat, since it is wider than "our own calls" suggests: the handler is
// process-global, so while it is installed an X error raised by any other thread
// -- including the host application's own X usage -- is swallowed and merely
// logged. That is the price of not permanently hijacking the host's handler,
// which seems the worse trade; the window is kept as short as possible to bound
// it. This file calls XInitThreads(), so the situation is real, not theoretical.
//
// Two details matter. The handler is process-global in Xlib, and the host
// application may well have installed its own -- Chromium does -- so leaving
// ours in place would silently take over its error handling. And X errors are
// delivered asynchronously, when the reply is processed, so the XSync before
// restoring is load-bearing: without it a pending error can arrive after the
// previous handler is back and terminate the process anyway.
class ScopedXErrorHandler {
public:
    explicit ScopedXErrorHandler(Display* dpy) : dpy_(dpy) {
        previous_ = XSetErrorHandler(overlayXErrorHandler);
    }
    ~ScopedXErrorHandler() {
        if (dpy_) XSync(dpy_, False);
        XSetErrorHandler(previous_);
    }
    ScopedXErrorHandler(const ScopedXErrorHandler&) = delete;
    ScopedXErrorHandler& operator=(const ScopedXErrorHandler&) = delete;
private:
    Display* dpy_;
    XErrorHandler previous_;
};

// Whether an id actually names a window on this display.
//
// A non-zero check is not a validity test: the value Ozone/Wayland hands back
// passes it and is not a window. Asking the server is the only way to know, and
// it must be done with the error handler installed, since the query itself
// raises BadWindow when the answer is no.
static bool isValidXWindow(Display* dpy, Window window) {
    if (!dpy || window == 0) return false;
    XWindowAttributes attrs;
    return XGetWindowAttributes(dpy, window, &attrs) != 0;
}

// Linux OpenGL/GLX Overlay Window — glXSwapBuffers is hooked by gameoverlayrenderer.so
class LinuxOverlayWindow {
public:
    Display* display = nullptr;
    Window window = 0;
    Window electronWindow = 0; // Electron XID — keyboard/mouse events are forwarded here
    GLXContext glContext = nullptr;
    Colormap colormap = 0;
    GLXFBConfig fbConfig = nullptr;
    XVisualInfo* visualInfo = nullptr;
    
    GLuint texture = 0;
    int texWidth = 0;
    int texHeight = 0;
    
    int width = 0;
    int height = 0;
    std::atomic<bool> isDestroyed{false};
    std::atomic<bool> isMapped{false};  // true while window is XMapRaised, false after XUnmapWindow
    std::mutex renderMutex;

    // Transparent mode: present empty frames and let the game window show
    // through, instead of mirroring it into a texture. See isTransparent().
    bool transparentRequested = false;
    bool hasArgbVisual = false;

    // Cursor warp suppression on Steam overlay close.
    // When Shift+Tab opens the overlay, Steam saves the cursor position.
    // When the overlay closes, Steam warps the cursor back to that saved position.
    // We detect this by: Shift+Tab sets overlayWasOpened=true, then the next FocusIn
    // (overlay handed focus back to us) triggers a 500ms MotionNotify suppression.
    // The 30px-distance approach doesn't work because lastMouseX/Y is already at the
    // restored position (we receive no MotionNotify while the overlay holds focus).
    bool overlayWasOpened = false;
    long long suppressMotionUntilMs = 0;

    // Timestamp of the last event forwarded to Electron.
    // Used by the idle refocus timer to re-grab X11 focus after Chromium steals it
    // when the user clicks an input element.
    long long lastForwardedEventMs = 0;

    // Timestamp of the last XSetInputFocus call. shouldSuppressNextBlur() suppresses
    // a blur only if it arrives within 200ms of this stamp — that is the spurious
    // blur caused by our own XSetInputFocus. Any real alt-tab/click-outside blur
    // arrives independently of our grabs and will not be suppressed.
    long long lastRequestFocusMs = 0;
    
    static long long getMonotonicMs() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }

    bool init(int w, int h, const char* title, bool transparent = false) {
        width = w;
        height = h;
        
        OverlayLog("Initializing Linux overlay window: %dx%d (%s)", w, h,
                   transparent ? "transparent" : "mirroring");
        transparentRequested = transparent;
        
        XInitThreads(); // Required for multi-threaded X11 access
        
        // Open X display
        display = XOpenDisplay(nullptr);
        if (!display) {
            OverlayLogError("Failed to open X display");
            return false;
        }
        
        // Check for required extensions
        int eventBase, errorBase;
        if (!XShapeQueryExtension(display, &eventBase, &errorBase)) {
            OverlayLogError("X Shape extension not available");
            XCloseDisplay(display);
            display = nullptr;
            return false;
        }
        
        // Check for XFixes (for input shape)
        int fixesEventBase, fixesErrorBase;
        bool hasXFixes = XFixesQueryExtension(display, &fixesEventBase, &fixesErrorBase);
        OverlayLog("XFixes extension: %s", hasXFixes ? "available" : "not available");
        
        // Get default screen
        int screen = DefaultScreen(display);
        Window root = RootWindow(display, screen);
        
        // Choose FBConfig with alpha support for transparency
        static int fbAttribs[] = {
            GLX_X_RENDERABLE, True,
            GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
            GLX_RENDER_TYPE, GLX_RGBA_BIT,
            GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
            GLX_RED_SIZE, 8,
            GLX_GREEN_SIZE, 8,
            GLX_BLUE_SIZE, 8,
            GLX_ALPHA_SIZE, 8,
            GLX_DEPTH_SIZE, 24,
            GLX_STENCIL_SIZE, 8,
            GLX_DOUBLEBUFFER, True,
            None
        };
        
        int fbCount;
        GLXFBConfig* fbConfigs = glXChooseFBConfig(display, screen, fbAttribs, &fbCount);
        if (!fbConfigs || fbCount == 0) {
            OverlayLogError("Failed to choose FBConfig");
            XCloseDisplay(display);
            display = nullptr;
            return false;
        }
        
        // Pick the first FBConfig, as before.
        fbConfig = fbConfigs[0];

        // Only in transparent mode, prefer one with a depth-32 ARGB visual.
        // GLX_ALPHA_SIZE above is a minimum on the GL drawable, not a promise
        // about the X visual -- glXChooseFBConfig will happily return a
        // depth-24 config first, and per-pixel window alpha needs depth 32.
        //
        // Deliberately not applied to the mirroring path: it does not need
        // alpha, and changing which visual existing callers get is not this
        // option's business.
        if (transparent) {
            for (int i = 0; i < fbCount; i++) {
                XVisualInfo* candidate = glXGetVisualFromFBConfig(display, fbConfigs[i]);
                if (!candidate) continue;
                if (candidate->depth == 32) {
                    fbConfig = fbConfigs[i];
                    hasArgbVisual = true;
                    XFree(candidate);
                    break;
                }
                XFree(candidate);
            }
        }
        XFree(fbConfigs);
        
        // Get visual info from FBConfig
        visualInfo = glXGetVisualFromFBConfig(display, fbConfig);
        if (!visualInfo) {
            OverlayLogError("Failed to get visual info");
            XCloseDisplay(display);
            display = nullptr;
            return false;
        }
        if (transparent) {
            OverlayLog("Chose visual depth %d (ARGB: %s)",
                       visualInfo->depth, hasArgbVisual ? "yes" : "no");
        }
        if (transparentRequested && !hasArgbVisual) {
            OverlayLogError("Transparency requested but no depth-32 visual is "
                            "available; the overlay window will be opaque");
        }
        
        // Create colormap
        colormap = XCreateColormap(display, root, visualInfo->visual, AllocNone);
        
        // Receive ALL input — we forward keyboard/mouse to Electron via XSendEvent.
        // The window must hold X11 keyboard focus for gameoverlayrenderer.so to
        // intercept Shift+Tab and trigger the Steam overlay.
        XSetWindowAttributes attrs;
        attrs.colormap = colormap;
        attrs.background_pixmap = None;
        attrs.background_pixel = 0;
        attrs.border_pixel = 0;
        attrs.event_mask = ExposureMask | StructureNotifyMask | VisibilityChangeMask
                         | KeyPressMask | KeyReleaseMask
                         | ButtonPressMask | ButtonReleaseMask | PointerMotionMask
                         | FocusChangeMask;
        attrs.override_redirect = True;  // Bypass KWin stacking entirely — window always on top
        
        // Create window with 32-bit depth for alpha
        window = XCreateWindow(
            display, root,
            0, 0, w, h,
            0,
            visualInfo->depth,
            InputOutput,
            visualInfo->visual,
            CWColormap | CWBackPixmap | CWBackPixel | CWBorderPixel | CWEventMask | CWOverrideRedirect,
            &attrs
        );
        
        if (!window) {
            OverlayLogError("Failed to create X window");
            XFree(visualInfo);
            XCloseDisplay(display);
            display = nullptr;
            return false;
        }
        
        // Set window title
        XStoreName(display, window, title);
        
        // STEAM_GAME atom — critical for Steam overlay detection
        const char* steamAppId = getenv("SteamAppId");
        if (steamAppId) {
            uint32_t appId = (uint32_t)atoi(steamAppId);
            Atom steamGameAtom = XInternAtom(display, "STEAM_GAME", False);
            XChangeProperty(display, window, steamGameAtom, XA_CARDINAL, 32, PropModeReplace,
                           (unsigned char*)&appId, 1);
            OverlayLog("Set STEAM_GAME atom to %u", appId);
        }
        
        // _NET_WM_PID
        pid_t pid = getpid();
        Atom wmPid = XInternAtom(display, "_NET_WM_PID", False);
        XChangeProperty(display, window, wmPid, XA_CARDINAL, 32, PropModeReplace,
                       (unsigned char*)&pid, 1);
        OverlayLog("Set _NET_WM_PID to %d", pid);
        
        // Set window type to utility/overlay for better window manager handling
        Atom windowType = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
        Atom windowTypeUtility = XInternAtom(display, "_NET_WM_WINDOW_TYPE_UTILITY", False);
        Atom windowTypeDialog = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
        Atom types[] = { windowTypeUtility, windowTypeDialog };
        XChangeProperty(display, window, windowType, XA_ATOM, 32, PropModeReplace,
                       (unsigned char*)types, 2);
        
        // Set window states: above, skip taskbar, skip pager
        Atom wmState = XInternAtom(display, "_NET_WM_STATE", False);
        Atom wmStateAbove = XInternAtom(display, "_NET_WM_STATE_ABOVE", False);
        Atom wmStateSkipTaskbar = XInternAtom(display, "_NET_WM_STATE_SKIP_TASKBAR", False);
        Atom wmStateSkipPager = XInternAtom(display, "_NET_WM_STATE_SKIP_PAGER", False);
        Atom states[] = { wmStateAbove, wmStateSkipTaskbar, wmStateSkipPager };
        XChangeProperty(display, window, wmState, XA_ATOM, 32, PropModeReplace,
                       (unsigned char*)states, 3);
        
        // Remove window decorations
        Atom motifHints = XInternAtom(display, "_MOTIF_WM_HINTS", False);
        struct {
            unsigned long flags;
            unsigned long functions;
            unsigned long decorations;
            long inputMode;
            unsigned long status;
        } hints = { 2, 0, 0, 0, 0 };  // flags=2 means decorations field is valid, decorations=0 means none
        XChangeProperty(display, window, motifHints, motifHints, 32, PropModeReplace,
                       (unsigned char*)&hints, 5);
        
        // Advertise WM_TAKE_FOCUS so KWin knows this window accepts focus
        Atom wmProtocols = XInternAtom(display, "WM_PROTOCOLS", False);
        Atom wmTakeFocus = XInternAtom(display, "WM_TAKE_FOCUS", False);
        XChangeProperty(display, window, wmProtocols, XA_ATOM, 32, PropModeReplace,
                       (unsigned char*)&wmTakeFocus, 1);

        // Presence is not interposition — reportOverlayHookState() tells the two apart.
        reportOverlayHookState();

        OverlayLog("Input forwarding mode: all events forwarded to Electron via XSendEvent");
        
        // Create modern OpenGL context using glXCreateContextAttribsARB if available
        glXCreateContextAttribsARBProc glXCreateContextAttribsARB = 
            (glXCreateContextAttribsARBProc)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
        
        if (glXCreateContextAttribsARB) {
            // Try to create OpenGL 3.3 core context first, fall back to legacy
            static int contextAttribs[] = {
                GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
                GLX_CONTEXT_MINOR_VERSION_ARB, 3,
                GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
                None
            };
            
            glContext = glXCreateContextAttribsARB(display, fbConfig, nullptr, True, contextAttribs);
            if (!glContext) {
                OverlayLog("Failed to create GL 3.3 context, trying legacy");
            }
        }
        
        // Fall back to legacy context creation
        if (!glContext) {
            glContext = glXCreateContext(display, visualInfo, nullptr, True);
        }
        
        if (!glContext) {
            OverlayLogError("Failed to create GLX context");
            XDestroyWindow(display, window);
            XFree(visualInfo);
            XCloseDisplay(display);
            display = nullptr;
            return false;
        }
        
        // Make context current
        if (!glXMakeCurrent(display, window, glContext)) {
            OverlayLogError("Failed to make GLX context current");
            glXDestroyContext(display, glContext);
            XDestroyWindow(display, window);
            XFree(visualInfo);
            XCloseDisplay(display);
            display = nullptr;
            return false;
        }
        
        // Try to disable vsync for lower latency
        glXSwapIntervalEXTProc glXSwapIntervalEXT = 
            (glXSwapIntervalEXTProc)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT");
        if (glXSwapIntervalEXT) {
            glXSwapIntervalEXT(display, window, 0);
            OverlayLog("VSync disabled");
        }
        
        // Initialize OpenGL state
        initGL();
        
        XSync(display, False);
        
        OverlayLog("Linux overlay window created successfully");
        OverlayLog("OpenGL Version: %s", glGetString(GL_VERSION));
        OverlayLog("OpenGL Renderer: %s", glGetString(GL_RENDERER));
        
        return true;
    }
    
    void initGL() {
        // Set up basic OpenGL state
        glEnable(GL_TEXTURE_2D);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glDisable(GL_CULL_FACE);
        
        // Set up orthographic projection for 2D rendering
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, width, height, 0, -1, 1);
        
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        
        // Enable blending for transparency
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        // Set clear color to transparent black
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        
        // Set viewport
        glViewport(0, 0, width, height);
    }
    
    void show() {
        if (isDestroyed) return;
        
        if (display && window) {
            OverlayLog("Showing overlay window");
            isMapped = true;
            XMapRaised(display, window);
            // Wait for the X server to process MapRaised before touching the GL drawable.
            // Without this, glXMakeCurrent may succeed on a not-yet-viewable window
            // and glXSwapBuffers silently no-ops, preventing Steam's hook from firing.
            XSync(display, False);
            // Re-acquire GL context on the now-viewable window
            if (glContext) {
                glXMakeCurrent(display, window, glContext);
            }
            requestFocus();
        }
    }

    void requestFocus() {
        if (!display || !window || !isMapped) return;
        XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
        XFlush(display);
        lastRequestFocusMs = getMonotonicMs();
    }

    void hide() {
        if (isDestroyed) return;
        
        if (display && window) {
            OverlayLog("Hiding overlay window");
            isMapped = false;
            // Release GL context before unmapping to prevent stale drawable state
            glXMakeCurrent(display, None, nullptr);
            XUnmapWindow(display, window);
            XFlush(display);
        }
    }
    
    void setFrame(int x, int y, int w, int h) {
        if (isDestroyed) return;
        
        OverlayLog("Setting frame: x=%d, y=%d, w=%d, h=%d", x, y, w, h);
        
        width = w;
        height = h;
        
        if (display && window) {
            XMoveResizeWindow(display, window, x, y, w, h);
            
            // Update OpenGL viewport
            if (glContext) {
                glXMakeCurrent(display, window, glContext);
                glViewport(0, 0, w, h);
                
                glMatrixMode(GL_PROJECTION);
                glLoadIdentity();
                glOrtho(0, w, h, 0, -1, 1);
                
                glMatrixMode(GL_MODELVIEW);
                glLoadIdentity();
            }
            
            XFlush(display);
        }
    }
    
    void renderFrame(const uint8_t* data, int w, int h) {
        if (isDestroyed) return;
        if (!isMapped) return;  // Don't render/swap when hidden — avoids GL errors on unmapped window
        
        std::lock_guard<std::mutex> lock(renderMutex);
        
        if (!display || !glContext || !window) return;
        
        if (!glXMakeCurrent(display, window, glContext)) {
            OverlayLogError("Failed to make context current in renderFrame");
            return;
        }
        
        // Create or update texture
        if (texture == 0 || w != texWidth || h != texHeight) {
            if (texture != 0) {
                glDeleteTextures(1, &texture);
            }
            
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            
            // Allocate texture storage (BGRA format from Electron)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
            
            texWidth = w;
            texHeight = h;
            
            OverlayLog("Created texture: %dx%d", w, h);
        }
        
        // Upload pixel data
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, data);
        
        // Clear with transparent color
        glClear(GL_COLOR_BUFFER_BIT);
        
        // Render textured quad
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture);
        
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        
        // Draw full-screen quad
        glBegin(GL_QUADS);
            glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, 0.0f);
            glTexCoord2f(1.0f, 0.0f); glVertex2f((float)width, 0.0f);
            glTexCoord2f(1.0f, 1.0f); glVertex2f((float)width, (float)height);
            glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, (float)height);
        glEnd();
        
        // Swap buffers
        glXSwapBuffers(display, window);
        
        pumpXEvents();
    }

    // Everything renderFrame does after the swap: input forwarding, the idle
    // refocus and the flush. Factored out so present() can share it -- in
    // transparent mode renderFrame is never called, and without this the X
    // event pump would stop and Shift+Tab would stop reaching Steam.
    void pumpXEvents() {
        // Process any pending X events
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);

            if (event.type == KeyPress || event.type == KeyRelease) {
                bool isShiftTab = (event.xkey.keycode == 23 && (event.xkey.state & ShiftMask));
                OverlayLog("%s: keycode=%d state=0x%x%s",
                    event.type == KeyPress ? "KeyPress" : "KeyRelease",
                    event.xkey.keycode, event.xkey.state,
                    isShiftTab ? " [Shift+Tab - overlay opening]" : "");

                // Shift+Tab: do NOT forward to Electron — Steam's hook consumes it.
                if (!isShiftTab && electronWindow) {
                    event.xkey.window    = electronWindow;
                    event.xkey.subwindow = None;
                    XSendEvent(display, electronWindow, True, NoEventMask, &event);
                    lastForwardedEventMs = getMonotonicMs();
                }

            } else if (electronWindow && (
                    event.type == ButtonPress || event.type == ButtonRelease ||
                    event.type == MotionNotify)) {

                if (event.type == MotionNotify) {
                    // Suppress motion during the cursor-restore window after overlay close
                    if (getMonotonicMs() < suppressMotionUntilMs) {
                        continue;
                    }
                }

                // Forward mouse event to Electron
                event.xkey.window    = electronWindow;
                event.xkey.subwindow = None;
                XSendEvent(display, electronWindow, True, NoEventMask, &event);

                if (event.type == ButtonPress) {
                    suppressMotionUntilMs = 0;
                    // Stamp lastForwardedEventMs so the idle refocus timer knows
                    // Chromium may have stolen X11 focus to handle this click.
                    // Do NOT call requestFocus() here — it immediately grabs focus
                    // back from Chromium before the input/dropdown can receive it.
                    lastForwardedEventMs = getMonotonicMs();
                }

            } else if (event.type == FocusOut) {
                if (isMapped) {
                    OverlayLog("FocusOut received");
                    overlayWasOpened = true;
                }
            } else if (event.type == FocusIn) {
                OverlayLog("FocusIn: overlay window has keyboard focus");
                if (overlayWasOpened) {
                    // Overlay just closed — Steam is about to warp cursor back to saved position.
                    // Suppress MotionNotify for 500ms so Electron doesn't snap hover state.
                    overlayWasOpened = false;
                    suppressMotionUntilMs = getMonotonicMs() + 500;
                    OverlayLog("FocusIn after overlay: suppressing cursor warp for 500ms");
                }
            }
            // Expose/other events: handled on next renderFrame
        }
        
        // Idle refocus: if Chromium stole X11 focus for an input element and the user
        // has stopped interacting for 1.5s, re-grab focus so Shift+Tab works again.
        // XGetInputFocus guard: only stamp lastRequestFocusMs when we actually move
        // focus — if we already have it, requestFocus() would be a no-op but would
        // refresh the timestamp, causing the next click-outside blur to be suppressed.
        if (isMapped && lastForwardedEventMs > 0 &&
                getMonotonicMs() - lastForwardedEventMs > 1500) {
            lastForwardedEventMs = 0;
            Window currentFocus; int revertTo;
            XGetInputFocus(display, &currentFocus, &revertTo);
            if (currentFocus != window) {
                requestFocus();
            }
        }

        // Ensure GL commands are flushed
        glFlush();
    }

    // Transparent mode: swap an empty frame so Steam's glXSwapBuffers hook
    // fires and it can draw the overlay, without copying the game window into
    // a texture first. The clear colour is already (0,0,0,0), so on an ARGB
    // visual with a compositor running the real window shows through.
    void present() {
        if (isDestroyed) return;
        if (!isMapped) return;

        std::lock_guard<std::mutex> lock(renderMutex);

        if (!display || !glContext || !window) return;

        if (!glXMakeCurrent(display, window, glContext)) {
            OverlayLogError("Failed to make context current in present");
            return;
        }

        glClear(GL_COLOR_BUFFER_BIT);
        glXSwapBuffers(display, window);

        pumpXEvents();
    }

    // Whether this window genuinely shows through. Two things have to hold:
    // the visual has to carry an alpha channel (a depth-32 ARGB visual, not
    // just a GLX config that reported GLX_ALPHA_SIZE), and a compositing
    // manager has to be running -- X11 does not blend anything on its own.
    bool isTransparent() {
        if (isDestroyed || !display) return false;
        return transparentRequested && hasArgbVisual && hasCompositor();
    }

    bool hasCompositor() const {
        if (!display) return false;
        // The standard handshake: a compositing manager owns _NET_WM_CM_S<screen>.
        char name[32];
        snprintf(name, sizeof(name), "_NET_WM_CM_S%d", DefaultScreen(display));
        Atom cmAtom = XInternAtom(display, name, False);
        return cmAtom != None && XGetSelectionOwner(display, cmAtom) != None;
    }

    void destroy() {
        if (isDestroyed.exchange(true)) return;
        
        OverlayLog("Destroying Linux overlay window...");
        
        // Delete texture
        if (texture && display && glContext) {
            glXMakeCurrent(display, window, glContext);
            glDeleteTextures(1, &texture);
            texture = 0;
        }
        
        if (glContext && display) {
            glXMakeCurrent(display, None, nullptr);
            glXDestroyContext(display, glContext);
            glContext = nullptr;
        }
        
        if (window && display) {
            XDestroyWindow(display, window);
            window = 0;
        }
        
        if (colormap && display) {
            XFreeColormap(display, colormap);
            colormap = 0;
        }
        
        if (visualInfo) {
            XFree(visualInfo);
            visualInfo = nullptr;
        }
        
        if (display) {
            XCloseDisplay(display);
            display = nullptr;
        }
        
        OverlayLog("Linux overlay window destroyed");
    }
    
    ~LinuxOverlayWindow() {
        destroy();
    }
};

// N-API wrapper functions
static napi_value CreateOverlayWindow(napi_env env, napi_callback_info info) {
    napi_status status;
    size_t argc = 1;
    napi_value args[1];
    status = napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (status != napi_ok || argc < 1) {
        napi_throw_error(env, nullptr, "Expected options object");
        return nullptr;
    }
    
    // Get options
    napi_value widthVal, heightVal, titleVal;
    napi_get_named_property(env, args[0], "width", &widthVal);
    napi_get_named_property(env, args[0], "height", &heightVal);
    napi_get_named_property(env, args[0], "title", &titleVal);
    
    napi_value transparentVal;
    bool transparent = false;
    if (napi_get_named_property(env, args[0], "transparent", &transparentVal) == napi_ok) {
        napi_get_value_bool(env, transparentVal, &transparent);
    }
    
    int width, height;
    napi_get_value_int32(env, widthVal, &width);
    napi_get_value_int32(env, heightVal, &height);
    
    char title[256] = "Steam Overlay";
    size_t titleLen;
    napi_get_value_string_utf8(env, titleVal, title, sizeof(title), &titleLen);
    
    // Create window
    LinuxOverlayWindow* window = new LinuxOverlayWindow();
    if (!window->init(width, height, title, transparent)) {
        delete window;
        napi_throw_error(env, nullptr, "Failed to create overlay window");
        return nullptr;
    }
    
    // Wrap pointer
    napi_value external;
    status = napi_create_external(env, window, nullptr, nullptr, &external);
    
    return external;
}

static napi_value PresentFrame(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    LinuxOverlayWindow* window;
    if (napi_get_value_external(env, args[0], (void**)&window) != napi_ok || !window) {
        return nullptr;
    }
    window->present();
    return nullptr;
}

static napi_value IsOverlayTransparent(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    LinuxOverlayWindow* window = nullptr;
    napi_value result;
    bool transparent = false;
    if (argc >= 1 &&
        napi_get_value_external(env, args[0], (void**)&window) == napi_ok && window) {
        transparent = window->isTransparent();
    }
    napi_get_boolean(env, transparent, &result);
    return result;
}

static napi_value ShowOverlayWindow(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    LinuxOverlayWindow* window;
    napi_get_value_external(env, args[0], (void**)&window);
    
    if (window) {
        window->show();
    }
    
    return nullptr;
}

static napi_value HideOverlayWindow(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    LinuxOverlayWindow* window;
    napi_get_value_external(env, args[0], (void**)&window);
    
    if (window) {
        window->hide();
    }
    
    return nullptr;
}

static napi_value SetOverlayWindowFrame(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    LinuxOverlayWindow* window;
    napi_get_value_external(env, args[0], (void**)&window);
    
    int x, y, width, height;
    napi_get_value_int32(env, args[1], &x);
    napi_get_value_int32(env, args[2], &y);
    napi_get_value_int32(env, args[3], &width);
    napi_get_value_int32(env, args[4], &height);
    
    if (window) {
        window->setFrame(x, y, width, height);
    }
    
    return nullptr;
}

static napi_value RenderFrame(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    LinuxOverlayWindow* window;
    napi_get_value_external(env, args[0], (void**)&window);
    
    void* buffer;
    size_t length;
    napi_get_buffer_info(env, args[1], &buffer, &length);
    
    int width, height;
    napi_get_value_int32(env, args[2], &width);
    napi_get_value_int32(env, args[3], &height);
    
    if (window && buffer) {
        window->renderFrame((const uint8_t*)buffer, width, height);
    }
    
    return nullptr;
}

static napi_value DestroyOverlayWindow(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    LinuxOverlayWindow* window;
    napi_get_value_external(env, args[0], (void**)&window);
    
    if (window) {
        delete window;
    }
    
    return nullptr;
}

static napi_value SetDebugMode(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    bool enabled;
    napi_get_value_bool(env, args[0], &enabled);
    g_debugMode = enabled;
    
    return nullptr;
}

// setSteamGameAtomOnWindow(xid, appId) — tags an X11 window with STEAM_GAME atom
// and stores it as the Electron target for input forwarding
static napi_value SetSteamGameAtomOnWindow(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    int64_t xWindowId = 0, appId = 0;
    napi_get_value_int64(env, args[0], &xWindowId);
    napi_get_value_int64(env, args[1], &appId);
    
    if (xWindowId == 0 || appId == 0) {
        napi_value result; napi_get_boolean(env, false, &result); return result;
    }
    
    // Note: linux-overlay.cpp uses per-instance handles, not a global.
    // Open a temporary display connection for this call.
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        napi_value result; napi_get_boolean(env, false, &result); return result;
    }
    
    bool tagged = false;
    {
        // The guard XSyncs the display when it is destroyed, so it must not
        // outlive it. An inner scope ends the guard while dpy is still open and
        // the display is closed afterwards -- and there is a single exit path,
        // so no return can outrun the close.
        ScopedXErrorHandler errorGuard(dpy);

        if (!isValidXWindow(dpy, (Window)xWindowId)) {
            // Expected when the application is on native Wayland rather than
            // XWayland: the handle is not an X window id at all. Skip the atom
            // rather than writing to an arbitrary resource.
            OverlayLogError("0x%lx is not a valid X window — not setting STEAM_GAME. "
                            "This is expected on native Wayland; run Electron on X11 "
                            "or XWayland for the Steam overlay.",
                            (unsigned long)xWindowId);
        } else {
            uint32_t appIdInt = (uint32_t)appId;
            Atom steamGameAtom = XInternAtom(dpy, "STEAM_GAME", False);
            XChangeProperty(dpy, (Window)xWindowId, steamGameAtom, XA_CARDINAL, 32,
                           PropModeReplace, (unsigned char*)&appIdInt, 1);
            XFlush(dpy);
            OverlayLog("Set STEAM_GAME=%u on Electron window 0x%lx", appIdInt,
                       (unsigned long)xWindowId);
            tagged = true;
        }
    }
    XCloseDisplay(dpy);

    napi_value result; napi_get_boolean(env, tagged, &result); return result;
}

// setElectronWindow(handle, xid) — stores the Electron XID on the overlay for input forwarding
static napi_value SetElectronWindow(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    LinuxOverlayWindow* window;
    napi_get_value_external(env, args[0], (void**)&window);
    
    int64_t xid = 0;
    napi_get_value_int64(env, args[1], &xid);
    
    if (window && xid && window->display) {
        // Validate before storing: this id is used for XSendEvent on every
        // frame, so a bad one would raise errors from the render loop rather
        // than from here, where they are far harder to attribute.
        ScopedXErrorHandler errorGuard(window->display);
        if (!isValidXWindow(window->display, (Window)xid)) {
            OverlayLogError("0x%lx is not a valid X window — input will not be "
                            "forwarded to the application.", (unsigned long)xid);
            napi_value result; napi_get_boolean(env, false, &result); return result;
        }
        window->electronWindow = (Window)xid;
        OverlayLog("Stored Electron window 0x%lx for input forwarding", (unsigned long)xid);
    }
    
    return nullptr;
}

// shouldSuppressNextBlur(handle) — returns true (once) if the last XSetInputFocus
// fired within the past 200ms.  Used by the TypeScript blur handler to distinguish
// our own spurious blurs from real alt-tab / click-outside blurs.
static napi_value ShouldSuppressNextBlur(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    LinuxOverlayWindow* window;
    napi_get_value_external(env, args[0], (void**)&window);

    bool suppress = false;
    if (window && window->lastRequestFocusMs > 0) {
        long long elapsed = LinuxOverlayWindow::getMonotonicMs() - window->lastRequestFocusMs;
        if (elapsed >= 0 && elapsed < 200) {
            suppress = true;
            window->lastRequestFocusMs = 0; // consume — one-shot
        }
    }

    napi_value result;
    napi_get_boolean(env, suppress, &result);
    return result;
}

// Expose the hook state to JavaScript. Callers currently have no way to tell
// "the overlay is attached and will draw" from "the overlay is attached and
// will silently do nothing", because attachOverlay() reports success either
// way. Returns "active", "mapped-not-interposing" or "not-present".
static napi_value GetOverlayHookState(napi_env env, napi_callback_info info) {
    std::string mapping, owner;
    const OverlayHookState state = queryOverlayHookState(&mapping, &owner);
    const char* name = overlayHookStateName(state);

    napi_value result;
    napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &result);
    return result;
}

// Module initialization - use same function names as other platforms for compatibility
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "createOverlayWindow",      nullptr, CreateOverlayWindow,      nullptr, nullptr, nullptr, napi_default, nullptr },
        { "showOverlayWindow",        nullptr, ShowOverlayWindow,        nullptr, nullptr, nullptr, napi_default, nullptr },
        { "hideOverlayWindow",        nullptr, HideOverlayWindow,        nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setOverlayFrame",          nullptr, SetOverlayWindowFrame,    nullptr, nullptr, nullptr, napi_default, nullptr },
        { "renderFrame",              nullptr, RenderFrame,              nullptr, nullptr, nullptr, napi_default, nullptr },
        { "presentFrame",             nullptr, PresentFrame,             nullptr, nullptr, nullptr, napi_default, nullptr },
        { "isOverlayTransparent",     nullptr, IsOverlayTransparent,     nullptr, nullptr, nullptr, napi_default, nullptr },
        { "destroyOverlayWindow",     nullptr, DestroyOverlayWindow,     nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setDebugMode",             nullptr, SetDebugMode,             nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setSteamGameAtomOnWindow",  nullptr, SetSteamGameAtomOnWindow,  nullptr, nullptr, nullptr, napi_default, nullptr },
        { "setElectronWindow",          nullptr, SetElectronWindow,          nullptr, nullptr, nullptr, napi_default, nullptr },
        { "shouldSuppressNextBlur",     nullptr, ShouldSuppressNextBlur,     nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getOverlayHookState",        nullptr, GetOverlayHookState,        nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)

#endif // __linux__
