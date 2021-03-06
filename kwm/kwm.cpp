#include "kwm.h"
#include "helpers.h"
#include "daemon.h"
#include "display.h"
#include "space.h"
#include "window.h"
#include "keys.h"
#include "interpreter.h"
#include "scratchpad.h"
#include "border.h"
#include "config.h"
#include "cursor.h"
#include "axlib/axlib.h"
#include <getopt.h>

#define internal static
const std::string KwmCurrentVersion = "Kwm Version 3.0.7";
std::map<std::string, space_info> WindowTree;

ax_state AXState = {};
ax_display *FocusedDisplay = NULL;
ax_application *FocusedApplication = NULL;
ax_window *MarkedWindow = NULL;

kwm_mach KWMMach = {};
kwm_path KWMPath = {};
kwm_settings KWMSettings = {};
kwm_thread KWMThread = {};
kwm_hotkeys KWMHotkeys = {};
kwm_border FocusedBorder = {};
kwm_border MarkedBorder = {};
scratchpad Scratchpad = {};

internal CGEventRef
CGEventCallback(CGEventTapProxy Proxy, CGEventType Type, CGEventRef Event, void *Refcon)
{
    switch(Type)
    {
        case kCGEventTapDisabledByTimeout:
        case kCGEventTapDisabledByUserInput:
        {
            if(!KWMMach.DisableEventTapInternal)
            {
                DEBUG("Notice: Restarting Event Tap");
                CGEventTapEnable(KWMMach.EventTap, true);
            }
        } break;
        case kCGEventKeyDown:
        {
            /* TODO(koekeishiya): Is there a better way to decide whether
                                  we should eat the CGEventRef or not (?) */
            if(HasFlags(&KWMSettings, Settings_BuiltinHotkeys))
            {
                hotkey Eventkey = {}, *Hotkey = NULL;
                Hotkey = new hotkey;
                if(Hotkey)
                {
                    CreateHotkeyFromCGEvent(Event, &Eventkey);
                    if(HotkeyExists(Eventkey.Flags, Eventkey.Key, Hotkey, KWMHotkeys.ActiveMode->Name))
                    {
                        AXLibConstructEvent(AXEvent_HotkeyPressed, Hotkey, false);
                        if(!Hotkey->Passthrough)
                            return NULL;
                    }
                    else
                    {
                        delete Hotkey;
                    }
                }
            }
        } break;
        case kCGEventMouseMoved:
        {
            if(KWMSettings.Focus == FocusModeAutoraise)
                AXLibConstructEvent(AXEvent_MouseMoved, NULL, false);
        } break;
        default: {} break;
    }

    return Event;
}

internal bool
CheckPrivileges()
{
    bool Result = false;
    const void * Keys[] = { kAXTrustedCheckOptionPrompt };
    const void * Values[] = { kCFBooleanTrue };

    CFDictionaryRef Options;
    Options = CFDictionaryCreate(kCFAllocatorDefault,
                                 Keys, Values, sizeof(Keys) / sizeof(*Keys),
                                 &kCFCopyStringDictionaryKeyCallBacks,
                                 &kCFTypeDictionaryValueCallBacks);

    Result = AXIsProcessTrustedWithOptions(Options);
    CFRelease(Options);

    return Result;
}

internal bool
GetKwmFilePath()
{
    bool Result = false;
    char PathBuf[PROC_PIDPATHINFO_MAXSIZE];
    pid_t Pid = getpid();
    int Ret = proc_pidpath(Pid, PathBuf, sizeof(PathBuf));
    if (Ret > 0)
    {
        KWMPath.FilePath = PathBuf;

        std::size_t Split = KWMPath.FilePath.find_last_of("/\\");
        KWMPath.FilePath = KWMPath.FilePath.substr(0, Split);
        Result = true;
    }

    return Result;
}

internal void
KwmClearSettings()
{
    KWMHotkeys.Modes.clear();
    KWMSettings.WindowRules.clear();
    KWMSettings.SpaceSettings.clear();
    KWMSettings.DisplaySettings.clear();
    KWMHotkeys.ActiveMode = GetBindingMode("default");
}

internal void
KwmExecuteInitScript()
{
    if(KWMPath.Init.empty())
        KWMPath.Init = KWMPath.Home + "/init";

    struct stat Buffer;
    if(stat(KWMPath.Init.c_str(), &Buffer) == 0)
        KwmExecuteSystemCommand(KWMPath.Init);
}

internal void
SignalHandler(int Signum)
{
    ShowAllScratchpadWindows();
    DEBUG("SignalHandler() " << Signum);

    CloseBorder(&FocusedBorder);
    CloseBorder(&MarkedBorder);
    exit(Signum);
}

internal void
Fatal(const std::string &Err)
{
    std::cout << Err << std::endl;
    exit(1);
}

internal void
KwmInit()
{
    if(!CheckPrivileges())
        Fatal("Error: Could not access OSX Accessibility!");

    if(KwmStartDaemon())
        pthread_create(&KWMThread.Daemon, NULL, &KwmDaemonHandleConnectionBG, NULL);
    else
        Fatal("Error: Could not start daemon!");

#ifndef DEBUG_BUILD
    signal(SIGSEGV, SignalHandler);
    signal(SIGABRT, SignalHandler);
    signal(SIGTRAP, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGKILL, SignalHandler);
    signal(SIGINT, SignalHandler);
#else
    printf("Notice: Signal handlers disabled!\n");
#endif

    KWMSettings.SplitRatio = 0.5;
    KWMSettings.SplitMode = SPLIT_OPTIMAL;
    KWMSettings.DefaultOffset = CreateDefaultScreenOffset();
    KWMSettings.OptimalRatio = 1.618;

    AddFlags(&KWMSettings,
            Settings_MouseFollowsFocus |
            Settings_BuiltinHotkeys |
            Settings_StandbyOnFloat |
            Settings_CenterOnFloat |
            Settings_LockToContainer);

    KWMSettings.Space = SpaceModeBSP;
    KWMSettings.Focus = FocusModeAutoraise;
    KWMSettings.Cycle = CycleModeScreen;

    FocusedBorder.Radius = -1;
    MarkedBorder.Radius = -1;

    char *HomeP = std::getenv("HOME");
    if(HomeP)
    {
        KWMPath.EnvHome = HomeP;
        KWMPath.Home = KWMPath.EnvHome + "/.kwm";
        KWMPath.Include = KWMPath.Home;
        KWMPath.Layouts = KWMPath.Home + "/layouts";

        if(KWMPath.Config.empty())
            KWMPath.Config = KWMPath.Home + "/kwmrc";
    }
    else
    {
        Fatal("Error: Failed to get environment variable 'HOME'");
    }

    KWMHotkeys.ActiveMode = GetBindingMode("default");
    GetKwmFilePath();
}

void KwmQuit()
{
    ShowAllScratchpadWindows();
    CloseBorder(&FocusedBorder);
    CloseBorder(&MarkedBorder);

    exit(0);
}

void KwmReloadConfig()
{
    KwmClearSettings();
    KwmParseConfig(KWMPath.Config);
}

/* NOTE(koekeishiya): Returns true for operations that cause Kwm to exit. */
internal bool
ParseArguments(int argc, char **argv)
{
    int Option;
    const char *ShortOptions = "vc:";
    struct option LongOptions[] =
    {
        {"version", no_argument, NULL, 'v'},
        {"config", required_argument, NULL, 'c'},
        {NULL, 0, NULL, 0}
    };

    while((Option = getopt_long(argc, argv, ShortOptions, LongOptions, NULL)) != -1)
    {
        switch(Option)
        {
            case 'v':
            {
                printf("%s\n", KwmCurrentVersion.c_str());
                return true;
            } break;
            case 'c':
            {
                DEBUG("Notice: Using config file " << optarg);
                KWMPath.Config = optarg;
            } break;
        }
    }

    return false;
}

int main(int argc, char **argv)
{
    if(ParseArguments(argc, argv))
        return 0;

    NSApplicationLoad();
    if(!AXLibDisplayHasSeparateSpaces())
        Fatal("Error: 'Displays have separate spaces' must be enabled!");

    AXLibInit(&AXState);
    AXLibStartEventLoop();

    ax_display *MainDisplay = AXLibMainDisplay();
    ax_display *Display = MainDisplay;
    do
    {
        ax_space *PrevSpace = Display->Space;
        Display->Space = AXLibGetActiveSpace(Display);
        Display->PrevSpace = PrevSpace;
        Display = AXLibNextDisplay(Display);
    } while(Display != MainDisplay);

    FocusedDisplay = MainDisplay;
    FocusedApplication = AXLibGetFocusedApplication();

    KwmInit();
    KwmParseConfig(KWMPath.Config);
    KwmExecuteInitScript();
    CreateWindowNodeTree(MainDisplay);

    if(CGSIsSecureEventInputSet())
        fprintf(stderr, "Notice: Secure Keyboard Entry is enabled, hotkeys will not work!\n");

    KWMMach.EventMask = ((1 << kCGEventKeyDown) |
                         (1 << kCGEventMouseMoved));

    KWMMach.EventTap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, KWMMach.EventMask, CGEventCallback, NULL);
    if(!KWMMach.EventTap || !CGEventTapIsEnabled(KWMMach.EventTap))
        Fatal("Error: Could not create event-tap!");

    CFRunLoopAddSource(CFRunLoopGetMain(),
                       CFMachPortCreateRunLoopSource(kCFAllocatorDefault, KWMMach.EventTap, 0),
                       kCFRunLoopCommonModes);
    CGEventTapEnable(KWMMach.EventTap, true);
    CFRunLoopRun();
    return 0;
}
