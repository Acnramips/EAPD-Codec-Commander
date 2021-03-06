/*
 *  Released under "The GNU General Public License (GPL-2.0)"
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#ifndef CodecCommander_Configuration_h
#define CodecCommander_Configuration_h

#include "Common.h"
#include "IntelHDA.h"

#define kRMCFCache "RMCF.cache"
#define kCodecCommanderKey "CodecCommander"
#define kCodecCommanderPowerHookKey "CodecCommanderPowerHook"
#define kCodecCommanderProbeInitKey "CodecCommanderProbeInit"

typedef struct
{
    bool OnInit;    // Execute command on initialization
    bool OnSleep;   // Execute command on sleep
    bool OnWake;    // Execute command on wake
    UInt32 layoutID;// layout-id filter
    UInt32 CommandCount;
    UInt32 Commands[0]; // 32-bit verb to execute (Codec Address will be filled in)
} CustomCommand;

class Configuration
{
    OSArray* mCustomCommands;
    OSArray* mPinConfigDefault;
    
    bool mCheckInfinite;
    UInt16 mCheckInterval;
    bool mPerformReset;
    bool mPerformResetOnExternalWake;
    bool mPerformResetOnEAPDFail;
    bool mUpdateNodes, mSleepNodes;
    UInt16 mSendDelay;
    bool mDisable;
    UInt16 mCodecAddressMask;

    static UInt32 parseInteger(const char* str);
    static OSDictionary* locateConfiguration(OSDictionary* profiles, UInt32 codecVendorId, UInt32 hdaSubsystemId);
    static OSDictionary* loadConfiguration(OSDictionary* profiles, UInt32 codecVendorId, UInt32 hdaSubsystemId);
    static bool getBoolValue(OSDictionary* dict, const char* key, bool defValue);
    static UInt32 getIntegerValue(OSDictionary* dict, const char* key, UInt32 defValue);
    static UInt32 getIntegerValue(OSObject* obj, UInt32 defValue);

    OSDictionary* getConfigurationOverride(const char* method, IOService* provider, const char* name);
    OSObject* translateArray(OSArray* array);
    OSObject* translateEntry(OSObject* obj);

public:
    inline bool getUpdateNodes() { return mUpdateNodes; };
    inline bool getSleepNodes() { return mSleepNodes; }
    inline bool getPerformReset() { return mPerformReset; };
    inline bool getPerformResetOnExternalWake() { return mPerformResetOnExternalWake; }
    inline bool getPerformResetOnEAPDFail() { return mPerformResetOnEAPDFail; }
    inline UInt16 getSendDelay() { return mSendDelay; };
    inline bool getCheckInfinite() { return mCheckInfinite; };
    inline UInt16 getCheckInterval() { return mCheckInterval; };
    inline OSArray* getCustomCommands() { return mCustomCommands; };
    inline bool getDisable() { return mDisable; }
    inline UInt16 getCodecAddressMask() { return mCodecAddressMask; }
    inline OSArray* getPinConfigDefault() { return mPinConfigDefault; }

    // Constructor
    Configuration(OSObject* codecProfiles, IntelHDA* intelHDA, const char* name);
    ~Configuration();

#ifdef DEBUG
    OSDictionary* mMergedConfig;
#endif
};

#endif
