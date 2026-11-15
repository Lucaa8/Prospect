#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <memory>
#include <cstring>
#include <ctime>

#include "discord.h" // discord_game_sdk.dll

#include <lua.hpp> // for lua_state (allows to pass lua context to C++ functions

#include <chrono> // to get current time or current millis
#include <iomanip> // for std::put_time

#include <iostream> // to manipulate strings, streams and format numbers
#include <string>
#include <sstream>
#include <fstream>
#include <locale> 

#include <thread> // only used to allow discord ticking (RunCallbacks) every 250ms
#include <atomic>

// When defined, DLL will log everything, from simple info logs to each Lua call to this DLL.
// Error/Fatal logs are still enabled even if not defined
#define DO_LOG

std::ofstream logfile;

std::ofstream& printlog(const char* func, const char* level)
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_local;
    localtime_s(&tm_local, &t);

    logfile << "[" << std::put_time(&tm_local, "%d/%m/%Y %Hh%M") << "] " << "[" << level << "] " << func << ": ";
    return logfile;
}

#define HEX(val) "0x" << std::hex << std::setfill('0') << std::setw(sizeof(val)*2) << (uintptr_t)(val) << std::dec

std::unique_ptr<discord::Core> core;
std::atomic<bool> g_ticking = false;
std::atomic<bool> g_clearDone = false;
std::thread g_DiscordThread;
CRITICAL_SECTION g_PresenceLock;

int InitDiscord()
{
    discord::Core* rawCore{};

    auto result = discord::Core::Create((long long)1494982020776988692, DiscordCreateFlags_NoRequireDiscord, &rawCore);

    if (result != discord::Result::Ok)
    {
        printlog(__func__, "Fatal") << "Something went wrong while initialising Discord core, make sure Discord is started BEFORE the game." << std::endl;
        return -1;
    }

    core.reset(rawCore);

    return 0;
}

void DiscordTick()
{
    while (g_ticking)
    {
        {
            EnterCriticalSection(&g_PresenceLock);
            // avoid reading activity when Lua context is currently updating the activity with DC_UpdatePresence
            if (core)
            {
                core->RunCallbacks();
            }
            LeaveCriticalSection(&g_PresenceLock);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void UpdatePresence(long long started, 
    const char* logo_name, const char* logo_descr,
    const char* small_logo, const char* small_logo_descr,
    const char* map, const char* details)
{

    discord::Activity activity{};

    activity.SetName("The Cycle: Frontier"); //Not responsible for the app title on discord rich presence. Determined by the app name on dev portal.
    activity.SetType(discord::ActivityType::Playing);

    activity.SetDetails(details);
    activity.SetState(map);

    activity.GetAssets().SetLargeImage(logo_name);
    if (strlen(logo_descr) != 0)
    {
        activity.GetAssets().SetLargeText(logo_descr);
    }

    activity.GetAssets().SetSmallImage(small_logo);
    if (strlen(small_logo_descr) != 0)
    {
        activity.GetAssets().SetSmallText(small_logo_descr);
    }

    activity.GetTimestamps().SetStart(started);

    // Locking because UpdatePresence is called from Lua context, which is not in sync with DiscordTick()
    EnterCriticalSection(&g_PresenceLock);
    if (!core)
    {
        LeaveCriticalSection(&g_PresenceLock);
        return;
    }
    core->ActivityManager().UpdateActivity(activity, [](discord::Result result) {});
    LeaveCriticalSection(&g_PresenceLock);

    #ifdef DO_LOG
    printlog(__func__, "Info") << "Updated presence! Started at: " << started << ", text: " << details << ", state: " << map << std::endl;
    #endif
}

void ShutdownDiscord()
{

    g_ticking = false;
    g_DiscordThread.join();

    // No need of mutex here because DiscordTick() (the async thread) is not running aynmore.
    if (!core) return;

    core->ActivityManager().UpdateActivity(discord::Activity(), [](discord::Result result)
        {
            g_clearDone = true;
        });

    #ifdef DO_LOG
    printlog(__func__, "Info") << "Waiting for activity presence to be cleared!" << std::endl;
    #endif
    while (!g_clearDone)
    {
        core->RunCallbacks();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    #ifdef DO_LOG
    printlog(__func__, "Info") << "Activity cleared, will shutdown discord sdk." << std::endl;
    #endif
    core.reset();
}

const uintptr_t AreaNameStart = 0x02D0;
const uintptr_t FTextDataStart = 0x02D8;
const uintptr_t SourceStringStart = 0x0040;

// __try __except is microsoft's SEH exception handling. Because deferencing an invalid pointer is not handled by C++ stantard exceptions.
// This __try __except avoid crashing the entire game just because an area name couldnt be recovered (not a big deal)
const wchar_t* _ReadAreaName(uintptr_t MapAreaLocation_BP_C)
{
    __try
    {
        uintptr_t AreaName = MapAreaLocation_BP_C + AreaNameStart;
        // Memory zone between 0x02D0 and 0x2D8 is 00 00 00 00 00 00 00 00 is AreaName is blank
        uintptr_t HasText = (*(uintptr_t*)AreaName) != 0;
        // Sometimes, an Area does not have any name (e.g Power Plant Office is a defined MapAreaLocation in game files but does not have any displayed name in game.
        if (HasText) 
        {
            uintptr_t FAreaNameTextData = *(uintptr_t*)(MapAreaLocation_BP_C + FTextDataStart);
            uintptr_t SourceStringText16Ptr = *(uintptr_t*)(FAreaNameTextData + SourceStringStart);
            return (const wchar_t*)SourceStringText16Ptr;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printlog(__func__, "Error") << "Failed to dereference pointer for FText(AreaName) at offset " << HEX(MapAreaLocation_BP_C) << "+" << HEX(AreaNameStart) << std::endl;
    }
    return L"None";
}

// Convert an UTF16 string to UTF8 string (mostly used because Lua cannot handle UTF16 (idk if its Lua or lua54.dll/lua.hpp but cannot pass utf16 chars to "lua_pushstring")
std::string WideCharToChar(const std::wstring& wide)
{
    if (wide.empty() || wide == L"None") return "None";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

extern "C" {

    __declspec(dllexport) int ReadAreaName(lua_State* L)
    {
        uintptr_t maparea_location_addr = (uintptr_t)luaL_checkinteger(L, 1);
        std::wstring name = _ReadAreaName(maparea_location_addr);
        // try catch to avoid crashing the whole game just because an area name couldnt be converted, would be quite bad...
        try
        {
            std::string nameAsUtf8 = WideCharToChar(name); // because it seems like lua does not accept utf-16 strings
            if (!nameAsUtf8.empty() && nameAsUtf8 != "None")
            {
                lua_pushstring(L, nameAsUtf8.c_str());
                #ifdef DO_LOG
                printlog(__func__, "Info") << "Lua call! AreaName value found: " << nameAsUtf8 << "!" << std::endl;
                #endif
                return 1;
            }
        }
        catch (...)
        {
            printlog(__func__, "Error") << "Failed to convert SourceStringText16 to UTF-8!" << std::endl;
        }
        lua_pushstring(L, "None");
        #ifdef DO_LOG
        printlog(__func__, "Info") << "Lua call! AreaName was empty (or not found)!" << std::endl;
        #endif
        return 1;
    }

    __declspec(dllexport) int ToStringFormatted(lua_State* L)
    {
        uint32_t number = (uint32_t)luaL_checkinteger(L, 1);
        const char* locale_lang = luaL_checkstring(L, 2);

        std::string locale_str(locale_lang);
        locale_str.append(".UTF-8");

        std::stringstream ss;
        try {
            ss.imbue(std::locale(locale_str));
        }
        catch (const std::runtime_error&) {
            // in case the local does not exists on this system
            ss.imbue(std::locale::classic());
        }

        ss << number;

        std::string formatted = ss.str();
        lua_pushstring(L, formatted.c_str());

        #ifdef DO_LOG
        printlog(__func__, "Info") << "Lua call! Formatted " << number << " to " << formatted << " with locale " << locale_lang << std::endl;
        #endif

        return 1;
    }

    __declspec(dllexport) int GetCurrentMillis(lua_State* L) {
        long long millis = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now().time_since_epoch()).count();
        lua_pushinteger(L, (lua_Integer)millis);
        #ifdef DO_LOG
        printlog(__func__, "Info") << "Lua call! Current millis: " << millis << "!" << std::endl;
        #endif
        return 1;
    }

    __declspec(dllexport) int DC_InitDiscord(lua_State* L) {
        int discord_init_result = InitDiscord();
        lua_pushinteger(L, discord_init_result);
        #ifdef DO_LOG
        printlog(__func__, "Info") << "Lua call! Discord init result: " << discord_init_result << "!" << std::endl;
        #endif
        return 1;
    }

    __declspec(dllexport) int DC_Ready(lua_State* L) {
        InitializeCriticalSection(&g_PresenceLock);
        g_ticking = true;
        g_DiscordThread = std::thread(DiscordTick);
        #ifdef DO_LOG
        printlog(__func__, "Info") << "Lua call! Critical section is initialized. Starting discord ticking..." << std::endl;
        #endif
        return 0;
    }

    __declspec(dllexport) int DC_UpdatePresence(lua_State* L) {

        long long started = (long long)luaL_checknumber(L, 1);
        std::string logo_name        = luaL_checkstring(L, 2);
        std::string logo_descr       = luaL_checkstring(L, 3);
        std::string small_logo       = luaL_checkstring(L, 4);
        std::string small_logo_descr = luaL_checkstring(L, 5);
        std::string map              = luaL_checkstring(L, 6);
        std::string details          = luaL_checkstring(L, 7);

        UpdatePresence(started, logo_name.c_str(), logo_descr.c_str(), small_logo.c_str(), small_logo_descr.c_str(), map.c_str(), details.c_str());

        return 0;
    }

    __declspec(dllexport) int DC_ShutdownDiscord(lua_State* L) {
        ShutdownDiscord();
        return 0;
    }

}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        logfile = std::ofstream(".\\Prospect.Discord.log", std::ios::app);
        #ifdef DO_LOG
        printlog("DllMain", "Info") << "Loaded at address: " << hModule << std::endl;
        printlog("DllMain", "Info") << "Loading in process: " << GetCurrentProcessId() << std::endl;
        #endif
    }
    else if (ul_reason_for_call == DLL_PROCESS_DETACH)
    {
        logfile.close();
    }
    return TRUE;
}


