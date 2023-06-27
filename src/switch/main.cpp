/*
    Copyright 2019-2023 Hydr8gon

    This file is part of NooDS.

    NooDS is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS. If not, see <https://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <switch.h>
#include <malloc.h>

#include "switch_ui.h"
#include "../common/nds_icon.h"
#include "../common/screen_layout.h"
#include "../core.h"
#include "../settings.h"

#define GYRO_TOUCH_RANGE  0.08f
#define STICK_TOUCH_RANGE 0xB000

const uint32_t keyMap[] =
{
    HidNpadButton_A,        HidNpadButton_B,       HidNpadButton_Minus,    HidNpadButton_Plus,
    HidNpadButton_AnyRight, HidNpadButton_AnyLeft, HidNpadButton_AnyUp,    HidNpadButton_AnyDown,
    HidNpadButton_ZR,       HidNpadButton_ZL,      HidNpadButton_X,        HidNpadButton_Y,
    (HidNpadButton_L | HidNpadButton_R)
};

const int clockSpeeds[] = { 1020000000, 1224000000, 1581000000, 1785000000 };

int screenFilter = 1;
int showFpsCounter = 0;
int dockedTouchMode = 0;
int switchOverclock = 3;

std::string ndsPath, gbaPath;
Core *core;

bool running = false;
std::thread *coreThread, *audioThread, *saveThread;
std::condition_variable cond;
std::mutex mutex;
ClkrstSession cpuSession;

ScreenLayout layout;
uint32_t framebuffer[256 * 192 * 8] = {};
bool gbaMode = false;

AudioOutBuffer audioBuffers[2];
AudioOutBuffer *audioReleasedBuffer;
int16_t *audioData[2];
uint32_t count;

int pointerMode = 0;
bool initialAngleDirty = false;
float initialAngleX = 0, initialAngleZ = 0;
HidSixAxisSensorHandle sensorHandles[3];

void runCore()
{
    // Run the emulator
    while (running)
        core->runFrame();
}

void outputAudio()
{
    while (running)
    {
        audoutWaitPlayFinish(&audioReleasedBuffer, &count, UINT64_MAX);
        int16_t *buffer = (int16_t*)audioReleasedBuffer->buffer;

        // The NDS sample rate is 32768Hz, but audout uses 48000Hz
        // Get 699 samples at 32768Hz, which is equal to approximately 1024 samples at 48000Hz
        uint32_t *original = core->spu.getSamples(699);

        // Stretch the 699 samples out to 1024 samples in the audio buffer
        for (int i = 0; i < 1024; i++)
        {
            uint32_t sample = original[i * 699 / 1024];
            buffer[i * 2 + 0] = sample >>  0;
            buffer[i * 2 + 1] = sample >> 16;
        }

        delete[] original;
        audoutAppendAudioOutBuffer(audioReleasedBuffer);
    }
}

void checkSave()
{
    while (running)
    {
        // Check save files every few seconds and update them if changed
        std::unique_lock<std::mutex> lock(mutex);
        cond.wait_for(lock, std::chrono::seconds(3), [&]{ return !running; });
        core->cartridgeNds.writeSave();
        core->cartridgeGba.writeSave();
    }
}

bool createCore()
{
    try
    {
        // Attempt to create the core
        if (core) delete core;
        core = new Core(ndsPath, gbaPath);
        return true;
    }
    catch (CoreError e)
    {
        std::vector<std::string> message;

        // Inform the user of the error if loading wasn't successful
        switch (e)
        {
            case ERROR_BIOS: // Missing BIOS files
                message.push_back("Make sure the path settings point to valid BIOS files and try again.");
                message.push_back("You can modify the path settings in the noods.ini file.");
                SwitchUI::message("Error Loading BIOS", message);
                break;

            case ERROR_FIRM: // Non-bootable firmware file
                message.push_back("Make sure the path settings point to a bootable firmware file or try another boot method.");
                message.push_back("You can modify the path settings in the noods.ini file.");
                SwitchUI::message("Error Loading Firmware", message);
                break;

            case ERROR_ROM: // Unreadable ROM file
                message.push_back("Make sure the ROM file is accessible and try again.");
                SwitchUI::message("Error Loading ROM", message);
                break;
        }

        core = nullptr;
        return false;
    }
}

void startCore()
{
    if (running) return;
    running = true;

    // Overclock the Switch CPU
    clkrstInitialize();
    clkrstOpenSession(&cpuSession, PcvModuleId_CpuBus, 0);
    clkrstSetClockRate(&cpuSession, clockSpeeds[switchOverclock]);

    // Start audio output
    audoutInitialize();
    audoutStartAudioOut();

    // Set up the audio buffers
    for (int i = 0; i < 2; i++)
    {
        int size = 1024 * 2 * sizeof(int16_t);
        int alignedSize = (size + 0xFFF) & ~0xFFF;
        audioData[i] = (int16_t*)memalign(0x1000, size);
        memset(audioData[i], 0, alignedSize);
        audioBuffers[i].next = NULL;
        audioBuffers[i].buffer = audioData[i];
        audioBuffers[i].buffer_size = alignedSize;
        audioBuffers[i].data_size = size;
        audioBuffers[i].data_offset = 0;
        audoutAppendAudioOutBuffer(&audioBuffers[i]);
    }

    // Start the threads
    coreThread  = new std::thread(runCore);
    audioThread = new std::thread(outputAudio);
    saveThread  = new std::thread(checkSave);
}

void stopCore()
{
    // Signal for the threads to stop if the core is running
    if (running)
    {
        std::lock_guard<std::mutex> guard(mutex);
        running = false;
        cond.notify_one();
    }
    else
    {
        return;
    }

    // Wait for the threads to stop
    coreThread->join();
    delete coreThread;
    audioThread->join();
    delete audioThread;
    saveThread->join();
    delete saveThread;

    // Free the audio buffers
    delete[] audioData[0];
    delete[] audioData[1];

    // Stop audio output
    audoutStopAudioOut();
    audoutExit();

    // Disable the overclock
    clkrstSetClockRate(&cpuSession, 1020000000);
    clkrstExit();
}

void settingsMenu()
{
    const std::vector<std::string> toggle      = { "Off", "On"                                    };
    const std::vector<std::string> position    = { "Center", "Top", "Bottom", "Left", "Right"     };
    const std::vector<std::string> rotation    = { "None", "Clockwise", "Counter-Clockwise"       };
    const std::vector<std::string> arrangement = { "Automatic", "Vertical", "Horizontal"          };
    const std::vector<std::string> sizing      = { "Even", "Enlarge Top", "Enlarge Bottom"        };
    const std::vector<std::string> gap         = { "None", "Quarter", "Half", "Full"              };
    const std::vector<std::string> touchMode   = { "Gyroscope", "Joystick"                        };
    const std::vector<std::string> overclock   = { "1020 MHz", "1224 MHz", "1581 MHz", "1785 MHz" };

    unsigned int index = 0;

    while (true)
    {
        // Get the list of settings and current values
        std::vector<ListItem> settings =
        {
            ListItem("Direct Boot",        toggle[Settings::directBoot]),
            ListItem("FPS Limiter",        toggle[Settings::fpsLimiter]),
            ListItem("Threaded 2D",        toggle[Settings::threaded2D]),
            ListItem("Threaded 3D",        toggle[(bool)Settings::threaded3D]),
            ListItem("High-Resolution 3D", toggle[Settings::highRes3D]),
            ListItem("Screen Position",    position[ScreenLayout::screenPosition]),
            ListItem("Screen Rotation",    rotation[ScreenLayout::screenRotation]),
            ListItem("Screen Arrangement", arrangement[ScreenLayout::screenArrangement]),
            ListItem("Screen Sizing",      sizing[ScreenLayout::screenSizing]),
            ListItem("Screen Gap",         gap[ScreenLayout::screenGap]),
            ListItem("Integer Scale",      toggle[ScreenLayout::integerScale]),
            ListItem("GBA Crop",           toggle[ScreenLayout::gbaCrop]),
            ListItem("Screen Filter",      toggle[screenFilter]),
            ListItem("Show FPS Counter",   toggle[showFpsCounter]),
            ListItem("Docked Touch Mode",  touchMode[dockedTouchMode]),
            ListItem("Switch Overclock",   overclock[switchOverclock])
        };

        // Create the settings menu
        Selection menu = SwitchUI::menu("Settings", &settings, index);
        index = menu.index;

        // Handle menu input
        if (menu.pressed & HidNpadButton_A)
        {
            // Change the chosen setting to its next value
            // Light FPS limiter doesn't seem to have issues, so there's no need for advanced selection
            // 1 thread for 3D seems to work best, so there's no need for advanced selection
            switch (index)
            {
                case  0: Settings::directBoot            = (Settings::directBoot            + 1) % 2; break;
                case  1: Settings::fpsLimiter            = (Settings::fpsLimiter            + 1) % 2; break;
                case  2: Settings::threaded2D            = (Settings::threaded2D            + 1) % 2; break;
                case  3: Settings::threaded3D            = (Settings::threaded3D            + 1) % 2; break;
                case  4: Settings::highRes3D             = (Settings::highRes3D             + 1) % 2; break;
                case  5: ScreenLayout::screenPosition    = (ScreenLayout::screenPosition    + 1) % 5; break;
                case  6: ScreenLayout::screenRotation    = (ScreenLayout::screenRotation    + 1) % 3; break;
                case  7: ScreenLayout::screenArrangement = (ScreenLayout::screenArrangement + 1) % 3; break;
                case  8: ScreenLayout::screenSizing      = (ScreenLayout::screenSizing      + 1) % 3; break;
                case  9: ScreenLayout::screenGap         = (ScreenLayout::screenGap         + 1) % 4; break;
                case 10: ScreenLayout::integerScale      = (ScreenLayout::integerScale      + 1) % 2; break;
                case 11: ScreenLayout::gbaCrop           = (ScreenLayout::gbaCrop           + 1) % 2; break;
                case 12: screenFilter                    = (screenFilter                    + 1) % 2; break;
                case 13: showFpsCounter                  = (showFpsCounter                  + 1) % 2; break;
                case 14: dockedTouchMode                 = (dockedTouchMode                 + 1) % 2; break;
                case 15: switchOverclock                 = (switchOverclock                 + 1) % 4; break;
            }
        }
        else
        {
            // Close the settings menu
            layout.update(1280, 720, gbaMode);
            Settings::save();
            return;
        }
    }
}

int setPath(std::string path)
{
    // Set the ROM path if the extension matches
    if (path.find(".nds", path.length() - 4) != std::string::npos) // NDS ROM
    {
        // If a GBA path is set, allow clearing it
        if (gbaPath != "")
        {
            if (!SwitchUI::message("Loading NDS ROM", std::vector<std::string>{"Load the previous GBA ROM alongside this ROM?"}, true))
                gbaPath = "";
        }

        // Set the NDS ROM path
        ndsPath = path;

        // Attempt to boot the core with the set ROMs
        if (createCore())
        {
            startCore();
            return 2;
        }

        // Clear the NDS ROM path if booting failed
        ndsPath = "";
        return 1;
    }
    else if (path.find(".gba", path.length() - 4) != std::string::npos) // GBA ROM
    {
        // If an NDS path is set, allow clearing it
        if (ndsPath != "")
        {
            if (!SwitchUI::message("Loading GBA ROM", std::vector<std::string>{"Load the previous NDS ROM alongside this ROM?"}, true))
                ndsPath = "";
        }

        // Set the GBA ROM path
        gbaPath = path;

        // Attempt to boot the core with the set ROMs
        if (createCore())
        {
            startCore();
            return 2;
        }

        // Clear the GBA ROM path if booting failed
        gbaPath = "";
        return 1;
    }

    return 0;
}

void fileBrowser()
{
    std::string path = "sdmc:/";
    unsigned int index = 0;

    // Load the appropriate icons for the current theme
    romfsInit();
    uint32_t *file   = SwitchUI::bmpToTexture(SwitchUI::isDarkTheme() ? "romfs:/file-dark.bmp"   : "romfs:/file-light.bmp");
    uint32_t *folder = SwitchUI::bmpToTexture(SwitchUI::isDarkTheme() ? "romfs:/folder-dark.bmp" : "romfs:/folder-light.bmp");
    romfsExit();

    while (true)
    {
        std::vector<ListItem> files;
        std::vector<NdsIcon*> icons;
        DIR *dir = opendir(path.c_str());
        dirent *entry;

        // Get all the folders and ROMs at the current path
        while ((entry = readdir(dir)))
        {
            std::string name = entry->d_name;

            if (entry->d_type == DT_DIR)
            {
                // Add a directory with a generic icon to the list
                files.push_back(ListItem(name, "", folder, 64));
            }
            else if (name.find(".nds", name.length() - 4) != std::string::npos)
            {
                // Add an NDS ROM with its decoded icon to the list
                icons.push_back(new NdsIcon(path + "/" + name));
                files.push_back(ListItem(name, "", icons[icons.size() - 1]->getIcon(), 32));
            }
            else if (name.find(".gba", name.length() - 4) != std::string::npos)
            {
                // Add a GBA ROM with a generic icon to the list
                files.push_back(ListItem(name, "", file, 64));
            }
        }

        closedir(dir);
        sort(files.begin(), files.end());

        // Create the file browser menu
        Selection menu = SwitchUI::menu("NooDS", &files, index, "Settings", "Exit");
        index = menu.index;

        // Free the NDS icon memory
        for (unsigned int i = 0; i < icons.size(); i++)
            delete[] icons[i];

        // Handle menu input
        if (menu.pressed & HidNpadButton_A)
        {
            // Do nothing if there are no files to select
            if (files.empty()) continue;

            // Navigate to the selected directory
            path += "/" + files[menu.index].name;
            index = 0;

            // Try to set a ROM path
            switch (setPath(path))
            {
                case 1: // ROM failed to load
                    // Remove the ROM from the path and continue browsing
                    path = path.substr(0, path.rfind("/"));
                    index = 0;
                case 0: // ROM not selected
                    continue;

                case 2: // ROM loaded
                    return;
            }
        }
        else if (menu.pressed & HidNpadButton_B)
        {
            // Navigate to the previous directory
            if (path != "sdmc:/")
            {
                path = path.substr(0, path.rfind("/"));
                index = 0;
            }
        }
        else if (menu.pressed & HidNpadButton_X)
        {
            // Open the settings menu   
            settingsMenu();
        }
        else
        {
            // Close the file browser
            return;
        }
    }
}

bool saveTypeMenu()
{
    unsigned int index = 0;
    std::vector<ListItem> items;

    if (core->isGbaMode())
    {
        // Set up list items for GBA save types
        items.push_back(ListItem("None"));
        items.push_back(ListItem("EEPROM 0.5KB"));
        items.push_back(ListItem("EEPROM 8KB"));
        items.push_back(ListItem("SRAM 32KB"));
        items.push_back(ListItem("FLASH 64KB"));
        items.push_back(ListItem("FLASH 128KB"));
    }
    else
    {
        // Set up list items for NDS save types
        items.push_back(ListItem("None"));
        items.push_back(ListItem("EEPROM 0.5KB"));
        items.push_back(ListItem("EEPROM 8KB"));
        items.push_back(ListItem("EEPROM 64KB"));
        items.push_back(ListItem("EEPROM 128KB"));
        items.push_back(ListItem("FRAM 32KB"));
        items.push_back(ListItem("FLASH 256KB"));
        items.push_back(ListItem("FLASH 512KB"));
        items.push_back(ListItem("FLASH 1024KB"));
        items.push_back(ListItem("FLASH 8192KB"));
    }

    while (true)
    {
        // Create the save type menu
        Selection menu = SwitchUI::menu("Change Save Type", &items, index);
        index = menu.index;

        // Handle menu input
        if (menu.pressed & HidNpadButton_A)
        {
            // Confirm the change because accidentally resizing a working save file could be bad!
            if (!SwitchUI::message("Changing Save Type", std::vector<std::string>{"Are you sure? This may result in data loss!"}, true))
                continue;

            // Apply the change
            if (core->isGbaMode())
            {
                switch (index)
                {
                    case 0: core->cartridgeGba.resizeSave(0);       break; // None
                    case 1: core->cartridgeGba.resizeSave(0x200);   break; // EEPROM 0.5KB
                    case 2: core->cartridgeGba.resizeSave(0x2000);  break; // EEPROM 8KB
                    case 3: core->cartridgeGba.resizeSave(0x8000);  break; // SRAM 32KB
                    case 4: core->cartridgeGba.resizeSave(0x10000); break; // FLASH 64KB
                    case 5: core->cartridgeGba.resizeSave(0x20000); break; // FLASH 128KB
                }
            }
            else
            {
                switch (index)
                {
                    case 0: core->cartridgeNds.resizeSave(0);        break; // None
                    case 1: core->cartridgeNds.resizeSave(0x200);    break; // EEPROM 0.5KB
                    case 2: core->cartridgeNds.resizeSave(0x2000);   break; // EEPROM 8KB
                    case 3: core->cartridgeNds.resizeSave(0x10000);  break; // EEPROM 64KB
                    case 4: core->cartridgeNds.resizeSave(0x20000);  break; // EEPROM 128KB
                    case 5: core->cartridgeNds.resizeSave(0x8000);   break; // FRAM 32KB
                    case 6: core->cartridgeNds.resizeSave(0x40000);  break; // FLASH 256KB
                    case 7: core->cartridgeNds.resizeSave(0x80000);  break; // FLASH 512KB
                    case 8: core->cartridgeNds.resizeSave(0x100000); break; // FLASH 1024KB
                    case 9: core->cartridgeNds.resizeSave(0x800000); break; // FLASH 8192KB
                }
            }

            return true;
        }

        return false;
    }
}

void pauseMenu()
{
    // Pause the emulator
    stopCore();

    unsigned int index = 0;

    std::vector<ListItem> items =
    {
        ListItem("Resume"),
        ListItem("Restart"),
        ListItem("Change Save Type"),
        ListItem("Settings"),
        ListItem("File Browser")
    };

    while (true)
    {
        // Create the pause menu
        Selection menu = SwitchUI::menu("NooDS", &items, index);
        index = menu.index;

        // Handle menu input
        if (menu.pressed & HidNpadButton_A)
        {
            // Handle the selected item
            switch (index)
            {
                case 0: // Resume
                    // Return to the emulator
                    startCore();
                    return;

                case 1: // Restart
                    // Restart and return to the emulator
                    createCore() ? startCore() : fileBrowser();
                    return;

                case 2: // Change Save Type
                    // Open the save type menu and restart if the save changed
                    if (saveTypeMenu())
                    {
                        createCore() ? startCore() : fileBrowser();
                        return;
                    }
                    break;

                case 3: // Settings
                    // Open the settings menu
                    settingsMenu();
                    break;

                case 4: // File Browser
                    // Open the file browser and close the pause menu
                    fileBrowser();
                    return;
            }
        }
        else if (menu.pressed & HidNpadButton_B)
        {
            // Return to the emulator
            startCore();
            return;
        }
        else
        {
            // Close the pause menu
            return;
        }
    }
}

int main(int argc, char **argv)
{
    appletLockExit();
    SwitchUI::initialize();

    // Initialize the motion sensors
    hidGetSixAxisSensorHandles(&sensorHandles[0], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(&sensorHandles[1], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    for (int i = 0; i < 3; i++)
        hidStartSixAxisSensor(sensorHandles[i]);

    // Define the platform settings
    std::vector<Setting> platformSettings =
    {
        Setting("screenFilter",    &screenFilter,    false),
        Setting("showFpsCounter",  &showFpsCounter,  false),
        Setting("dockedTouchMode", &dockedTouchMode, false),
        Setting("switchOverclock", &switchOverclock, false)
    };

    // Load the settings
    ScreenLayout::addSettings();
    Settings::add(platformSettings);
    if (!Settings::load()) Settings::save();
    layout.update(1280, 720, gbaMode);

    // Get the system language
    uint64_t langCode;
    SetLanguage lang;
    setInitialize();
    setGetSystemLanguage(&langCode);
    setMakeLanguage(langCode, &lang);
    setExit();

    // Set the language for the generated firmware
    switch (lang)
    {
        case SetLanguage_JA: Spi::setLanguage(LG_JAPANESE); break;
        case SetLanguage_FRCA:
        case SetLanguage_FR: Spi::setLanguage(LG_FRENCH);   break;
        case SetLanguage_DE: Spi::setLanguage(LG_GERMAN);   break;
        case SetLanguage_IT: Spi::setLanguage(LG_ITALIAN);  break;
        case SetLanguage_ES419:
        case SetLanguage_ES: Spi::setLanguage(LG_SPANISH);  break;
        default:             Spi::setLanguage(LG_ENGLISH);  break;
    }

    // Open the file browser if a ROM can't be loaded from arguments
    if (argc < 2 || setPath(argv[1]) < 2)
        fileBrowser();

    while (appletMainLoop() && running)
    {
        SwitchUI::clear(Color(0, 0, 0));

        // Scan for key input
        padUpdate(SwitchUI::getPad());
        uint32_t held = padGetButtons(SwitchUI::getPad());
        uint32_t pressed = padGetButtonsDown(SwitchUI::getPad());
        uint32_t released = padGetButtonsUp(SwitchUI::getPad());

        // Ignore stick movement while a stick is pressed
        if (held & HidNpadButton_StickL)
            pressed &= ~(HidNpadButton_StickLRight | HidNpadButton_StickLLeft | HidNpadButton_StickLUp | HidNpadButton_StickLDown);
        if (held & HidNpadButton_StickR)
            pressed &= ~(HidNpadButton_StickRRight | HidNpadButton_StickRLeft | HidNpadButton_StickRUp | HidNpadButton_StickRDown);

        // Send input to the core
        for (int i = 0; i < 12; i++)
        {
            if (pressed & keyMap[i])
                core->input.pressKey(i);
            else if (released & keyMap[i])
                core->input.releaseKey(i);
        }

        // Update the layout if GBA mode changed
        if (gbaMode != (core->isGbaMode() && ScreenLayout::gbaCrop))
        {
            gbaMode = !gbaMode;
            layout.update(1280, 720, gbaMode);
        }

        // Get a new frame if one is ready
        core->gpu.getFrame(framebuffer, gbaMode);

        // Shift the screen resolutions if high-res is enabled
        bool resShift = Settings::highRes3D;

        if (gbaMode)
        {
            // Draw the GBA screen
            SwitchUI::drawImage(&framebuffer[0], 240 << resShift, 160 << resShift, layout.topX, layout.topY,
                layout.topWidth, layout.topHeight, screenFilter, ScreenLayout::screenRotation);
        }
        else // NDS mode
        {
            // Draw the DS top screen
            SwitchUI::drawImage(&framebuffer[0], 256 << resShift, 192 << resShift, layout.topX, layout.topY,
                layout.topWidth, layout.topHeight, screenFilter, ScreenLayout::screenRotation);

            // Draw the DS bottom screen
            SwitchUI::drawImage(&framebuffer[(256 * 192) << (resShift * 2)], 256 << resShift, 192 << resShift,
                layout.botX, layout.botY, layout.botWidth, layout.botHeight, screenFilter, ScreenLayout::screenRotation);

            // Handle touch input, depending on the current operation mode
            if (appletGetOperationMode() == AppletOperationMode_Console &&
                (held & (HidNpadButton_StickL | HidNpadButton_StickR))) // Docked, stick pressed
            {
                int screenX, screenY;

                // Set the pointer mode depending on which stick is initially pressed
                if (pointerMode == 0)
                {
                    pointerMode = (held & HidNpadButton_StickL) ? 1 : 2;
                    initialAngleDirty = true;
                }

                if (dockedTouchMode == 0) // Gyroscope
                {
                    // Read the sensor state of the appropriate controller
                    // For Joy-Cons, use the one that contains the initially pressed stick
                    HidSixAxisSensorState sensorState;
                    bool joycon = padGetStyleSet(SwitchUI::getPad()) & HidNpadStyleTag_NpadJoyDual;
                    hidGetSixAxisSensorStates(sensorHandles[joycon ? pointerMode : 0], &sensorState, 1);

                    // Save the initial motion angle; this position will be the middle of the touch screen
                    if (initialAngleDirty)
                    {
                        initialAngleX = sensorState.angle.x;
                        initialAngleZ = sensorState.angle.z;
                        initialAngleDirty = false;
                    }

                    // Get the current motion angle, clamped, relative to the initial angle
                    float relativeX = -std::max(std::min(sensorState.angle.z - initialAngleZ,
                        GYRO_TOUCH_RANGE / 2), -(GYRO_TOUCH_RANGE / 2)) + GYRO_TOUCH_RANGE / 2;
                    float relativeY = -std::max(std::min(sensorState.angle.x - initialAngleX,
                        GYRO_TOUCH_RANGE / 2), -(GYRO_TOUCH_RANGE / 2)) + GYRO_TOUCH_RANGE / 2;

                    // Scale the motion angle to a position on the touch screen
                    screenX = layout.botX + relativeX * layout.botWidth  / GYRO_TOUCH_RANGE;
                    screenY = layout.botY + relativeY * layout.botHeight / GYRO_TOUCH_RANGE;
                }
                else // Joystick
                {
                    HidAnalogStickState stick = padGetStickPos(SwitchUI::getPad(), pointerMode - 1);

                    // Get the current stick position, clamped, relative to the center
                    int relativeX =  std::max(std::min(stick.x,
                        STICK_TOUCH_RANGE / 2), -(STICK_TOUCH_RANGE / 2)) + STICK_TOUCH_RANGE / 2;
                    int relativeY = -std::max(std::min(stick.y,
                        STICK_TOUCH_RANGE / 2), -(STICK_TOUCH_RANGE / 2)) + STICK_TOUCH_RANGE / 2;

                    // Scale the stick position to a position on the touch screen
                    screenX = layout.botX + relativeX * layout.botWidth  / STICK_TOUCH_RANGE;
                    screenY = layout.botY + relativeY * layout.botHeight / STICK_TOUCH_RANGE;
                }

                // Draw a pointer on the screen to show the current touch position
                uint8_t c = (held & keyMap[12]) ? 0x7F : 0xFF;
                SwitchUI::drawRectangle(screenX - 10, screenY - 10, 20, 20, Color(0, 0, 0));
                SwitchUI::drawRectangle(screenX -  8, screenY -  8, 16, 16, Color(c, c, c));

                // Override the menu mapping, and touch the screen while it's held
                if (held & keyMap[12])
                {
                    // Determine the touch position relative to the emulated touch screen
                    int touchX = layout.getTouchX(screenX, screenY);
                    int touchY = layout.getTouchY(screenX, screenY);

                    // Send the touch coordinates to the core
                    core->input.pressScreen();
                    core->spi.setTouch(touchX, touchY);
                }
                else
                {
                    // Release the touch screen press
                    core->input.releaseScreen();
                    core->spi.clearTouch();
                }
            }
            else
            {
                // Reset the pointer mode, since it's not being used
                pointerMode = 0;

                // Scan for touch input
                HidTouchScreenState touch;
                hidGetTouchScreenStates(&touch, 1);

                if (touch.count > 0) // Pressed
                {
                    // Determine the touch position relative to the emulated touch screen
                    int touchX = layout.getTouchX(touch.touches[0].x, touch.touches[0].y);
                    int touchY = layout.getTouchY(touch.touches[0].x, touch.touches[0].y);

                    // Send the touch coordinates to the core
                    core->input.pressScreen();
                    core->spi.setTouch(touchX, touchY);
                }
                else // Released
                {
                    // Release the touch screen press
                    core->input.releaseScreen();
                    core->spi.clearTouch();
                }
            }
        }

        // Draw the FPS counter if enabled
        if (showFpsCounter)
            SwitchUI::drawString(std::to_string(core->getFps()) + " FPS", 5, 0, 48, Color(255, 255, 255));

        SwitchUI::update();

        // Open the pause menu if requested
        if (!pointerMode && (pressed & keyMap[12]))
            pauseMenu();
    }

    // Clean up
    stopCore();
    delete core;
    for (int i = 0; i < 3; i++)
        hidStopSixAxisSensor(sensorHandles[i]);
    SwitchUI::deinitialize();
    appletUnlockExit();
    return 0;
}
