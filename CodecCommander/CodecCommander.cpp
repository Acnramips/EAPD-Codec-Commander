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

#include <libkern/version.h>
#include "CodecCommander.h"

//REVIEW: avoids problem with Xcode 5.1.0 where -dead_strip eliminates these required symbols
#include <libkern/OSKextLib.h>
void* _org_rehabman_dontstrip_[] =
{
	(void*)&OSKextGetCurrentIdentifier,
	(void*)&OSKextGetCurrentLoadTag,
	(void*)&OSKextGetCurrentVersionString,
};

// Define usable power states
static IOPMPowerState powerStateArray[ kPowerStateCount ] =
{
    { 1,0,0,0,0,0,0,0,0,0,0,0 },
    { 1,kIOPMDeviceUsable, kIOPMDoze, kIOPMDoze, 0,0,0,0,0,0,0,0 },
    { 1,kIOPMDeviceUsable, IOPMPowerOn, IOPMPowerOn, 0,0,0,0,0,0,0,0 }
};

static IORecursiveLock* g_lock;

extern "C"
{

__attribute__((visibility("hidden")))
kern_return_t CodecCommander_Start(kmod_info_t* ki, void * d)
{
	AlwaysLog("Version %s starting on OS X Darwin %d.%d.\n", ki->version, version_major, version_minor);

	g_lock = IORecursiveLockAlloc();
	if (!g_lock)
		return KERN_FAILURE;

	return KERN_SUCCESS;
}

__attribute__((visibility("hidden")))
kern_return_t CodecCommander_Stop(kmod_info_t* ki, void * d)
{
	if (g_lock)
	{
		IORecursiveLockFree(g_lock);
		g_lock = 0;
	}

	return KERN_SUCCESS;
}

} // extern "C"

OSDefineMetaClassAndStructors(CodecCommanderResidency, IOService)

bool CodecCommanderResidency::start(IOService *provider)
{
	// announce version
	extern kmod_info_t kmod_info;

	// place version/build info in ioreg properties RM,Build and RM,Version
	char buf[128];
	snprintf(buf, sizeof(buf), "%s %s", kmod_info.name, kmod_info.version);
	setProperty("RM,Version", buf);
#ifdef DEBUG
	setProperty("RM,Build", "Debug-" LOGNAME);
#else
	setProperty("RM,Build", "Release-" LOGNAME);
#endif

	return super::start(provider);
}

OSDefineMetaClassAndStructors(CodecCommander, IOService)

//REVIEW: getHDADriver and getAudioDevice are only used by "Check Infinitely"
// Note: "Check Infinitely" should be called "Check Periodically"

static IOAudioDevice* getHDADriver(IORegistryEntry* registryEntry)
{
	IOAudioDevice* audioDevice = NULL;
	while (registryEntry)
	{
		audioDevice = OSDynamicCast(IOAudioDevice, registryEntry);
		if (audioDevice)
			break;
		registryEntry = registryEntry->getChildEntry(gIOServicePlane);
	}
#ifdef DEBUG
	if (!audioDevice)
		AlwaysLog("getHDADriver unable to find IOAudioDevice\n");
#endif
	return audioDevice;
}

IOAudioDevice* CodecCommander::getAudioDevice()
{
	if (!mAudioDevice)
	{
		mAudioDevice = getHDADriver(mProvider);
		if (mAudioDevice)
			mAudioDevice->retain();
	}
	return mAudioDevice;
}

/******************************************************************************
 * CodecCommander::init - parse kernel extension Info.plist
 ******************************************************************************/
bool CodecCommander::init(OSDictionary *dictionary)
{
	DebugLog("Initializing\n");
	uint32_t flag;
	if (PE_parse_boot_argn("-ccoff", &flag, sizeof(flag)))
	{
		AlwaysLog("stopping due to -ccoff kernel flag\n");
		return false;
	}

    if (!super::init(dictionary))
        return false;
	
    mWorkLoop = NULL;
    mTimer = NULL;
	
	mEAPDPoweredDown = true;
	mColdBoot = true; // assume booting from cold since hibernate is broken on most hacks
	mHDAPrevPowerState = kIOAudioDeviceSleep; // assume hda codec has no power at cold boot

    return true;
}

#ifdef DEBUG
/******************************************************************************
 * CodecCommander::probe - Determine if the attached device is supported
 ******************************************************************************/
IOService* CodecCommander::probe(IOService* provider, SInt32* score)
{
	DebugLog("Probe\n");

	return super::probe(provider, score);
}
#endif

static void setNumberProperty(IOService* service, const char* key, UInt32 value)
{
	OSNumber* num = OSNumber::withNumber(value, 32);
	if (num)
	{
		service->setProperty(key, num);
		num->release();
	}
}


/******************************************************************************
 * CodecCommander::start - start kernel extension and init PM
 ******************************************************************************/
bool CodecCommander::start(IOService *provider)
{
    if (!provider || !super::start(provider))
	{
		DebugLog("Error loading kernel extension.\n");
		return false;
	}

	// cache the provider
	mProvider = provider;

	IORecursiveLockLock(g_lock);
	mIntelHDA = new IntelHDA(provider, PIO);
	if (!mIntelHDA || !mIntelHDA->initialize())
	{
		IORecursiveLockUnlock(g_lock);
		AlwaysLog("Error initializing IntelHDA instance\n");
		stop(provider);
		return false;
	}

	// Populate HDA properties for client matching
	setNumberProperty(this, kCodecVendorID, mIntelHDA->getCodecVendorId());
	setNumberProperty(this, kCodecAddress, mIntelHDA->getCodecAddress());
	setNumberProperty(this, kCodecFuncGroupType, mIntelHDA->getCodecGroupType());

	mConfiguration = new Configuration(this->getProperty(kCodecProfile), mIntelHDA, kCodecCommanderKey);
	if (!mConfiguration || mConfiguration->getDisable())
	{
		IORecursiveLockUnlock(g_lock);
		AlwaysLog("stopping due to codec profile Disable flag\n");
		stop(provider);
		return false;
	}
#ifdef DEBUG
	setProperty("Merged Profile", mConfiguration->mMergedConfig);
#endif

	if (mConfiguration->getUpdateNodes())
	{
		// need to wait a bit until codec can actually respond to immediate verbs
		IOSleep(mConfiguration->getSendDelay());

		// Fetch Pin Capabilities from the range of nodes
		DebugLog("Getting EAPD supported node list.\n");
		
		mEAPDCapableNodes = OSArray::withCapacity(3);
		if (!mEAPDCapableNodes)
		{
			IORecursiveLockUnlock(g_lock);
			stop(provider);
			return false;
		}
		
		UInt16 start = mIntelHDA->getStartingNode();
		UInt16 end = start + mIntelHDA->getTotalNodes();
		for (UInt16 node = start; node < end; node++)
		{
			UInt32 response = mIntelHDA->sendCommand(node, HDA_VERB_GET_PARAM, HDA_PARM_PINCAP);
			if (response == -1)
			{
				DebugLog("Failed to retrieve pin capabilities for node 0x%02x.\n", node);
				continue;
			}
			
			// if bit 16 is set in pincap - node supports EAPD
			if (HDA_PINCAP_IS_EAPD_CAPABLE(response))
			{
				OSNumber* num = OSNumber::withNumber(node, 16);
				if (num)
				{
					mEAPDCapableNodes->setObject(num);
					num->release();
				}
				AlwaysLog("Node ID 0x%02x supports EAPD, will update state after sleep.\n", node);
			}
		}
	}
	
	// Execute any custom commands registered for initialization
	customCommands(kStateInit);

	IORecursiveLockUnlock(g_lock);
	
    // init power state management & set state as PowerOn
    PMinit();
    registerPowerDriver(this, powerStateArray, kPowerStateCount);
	provider->joinPMtree(this);

	// no need to start timer unless "Check Infinitely" is enabled
	if (mConfiguration->getCheckInfinite())
	{
		DebugLog("Infinite workloop requested, will start now!\n");

		// setup workloop and timer
		mWorkLoop = IOWorkLoop::workLoop();
		mTimer = IOTimerEventSource::timerEventSource(this,
													  OSMemberFunctionCast(IOTimerEventSource::Action, this,
													  &CodecCommander::onTimerAction));
		if (!mWorkLoop || !mTimer)
		{
			stop(provider);
			return false;
		}

		if (mWorkLoop->addEventSource(mTimer) != kIOReturnSuccess)
		{
			stop(provider);
			return false;
		}
	}

	this->registerService(0);
    return true;
}

/******************************************************************************
 * CodecCommander::stop & free - stop and free kernel extension
 ******************************************************************************/
void CodecCommander::stop(IOService *provider)
{
    DebugLog("Stopping...\n");

    // if workloop is active - release it
	if (mTimer)
		mTimer->cancelTimeout();
	if (mWorkLoop && mTimer)
		mWorkLoop->removeEventSource(mTimer);
    OSSafeReleaseNULL(mTimer);// disable outstanding calls
    OSSafeReleaseNULL(mWorkLoop);
	
    PMstop();
	
	// Free IntelHDA engine
	delete mIntelHDA;
	mIntelHDA = NULL;
	
	// Free Configuration
	delete mConfiguration;
	mConfiguration = NULL;
	
	OSSafeReleaseNULL(mEAPDCapableNodes);
	OSSafeReleaseNULL(mAudioDevice);
	mProvider = NULL;

    super::stop(provider);
}

/******************************************************************************
 * CodecCommander::onTimerAction - repeats the action each time timer fires
 ******************************************************************************/
void CodecCommander::onTimerAction()
{
	mTimer->setTimeoutMS(mConfiguration->getCheckInterval());

	IOAudioDevice* audioDevice = getAudioDevice();
	if (!audioDevice)
		return;

    // check if hda codec is powered - we are monitoring ocurrences of fugue state
	IOAudioDevicePowerState powerState = audioDevice->getPowerState();
	
	// if hda codec changed power state
	if (powerState != mHDAPrevPowerState)
	{
		DebugLog("Power state transition from %s to %s recorded.\n",
				  getPowerState(mHDAPrevPowerState), getPowerState(powerState));
		
		// store current power state as previous state for next workloop cycle
		mHDAPrevPowerState = powerState;
		
		// notify about codec power loss state
		if (powerState == kIOAudioDeviceSleep)
		{
			DebugLog("HDA codec lost power\n");
			handleStateChange(kIOAudioDeviceSleep); // power down EAPDs properly
		}

		// if no power after semi-sleep (fugue) state and power was restored - set EAPD bit
		if (powerState != kIOAudioDeviceSleep)
		{
			DebugLog("--> hda codec power restored\n");
			handleStateChange(kIOAudioDeviceActive);
		}
	}
}

/******************************************************************************
 * CodecCommander::handleStateChange - handles transitioning from one state to another, i.e. sleep --> wake
 ******************************************************************************/
void CodecCommander::handleStateChange(IOAudioDevicePowerState newState)
{
	switch (newState)
	{
		case kIOAudioDeviceSleep:
			mColdBoot = false;
			if (mConfiguration->getSleepNodes())
			{
				if (!setEAPD(0x00) && mConfiguration->getPerformResetOnEAPDFail())
				{
					AlwaysLog("BLURP! setEAPD(0x00) failed... attempt fix with codec reset\n");
					performCodecReset();
					setEAPD(0x00);
				}
			}

			customCommands(kStateSleep);
			mEAPDPoweredDown = true;
			break;

		case kIOAudioDeviceIdle:	// note kIOAudioDeviceIdle is not used
		case kIOAudioDeviceActive:
			mIntelHDA->applyIntelTCSEL();
			
			if (mConfiguration->getUpdateNodes())
			{
				if (!setEAPD(0x02) && mConfiguration->getPerformResetOnEAPDFail())
				{
					AlwaysLog("BLURP! setEAPD(0x02) failed... attempt fix with codec reset\n");
					performCodecReset();
					setEAPD(0x02);
				}
			}

			if (!mColdBoot)
				customCommands(kStateWake);

			mEAPDPoweredDown = false;
			break;
	}
}

/******************************************************************************
 * CodecCommander::customCommands - fires all configured custom commands
 ******************************************************************************/
void CodecCommander::customCommands(CodecCommanderState newState)
{
	UInt32 layoutID = mIntelHDA->getLayoutID();

	IORecursiveLockLock(g_lock);

	OSArray* commands = mConfiguration->getCustomCommands();
	unsigned count = commands->getCount();
	for (unsigned i = 0; i < count; i++)
	{
		OSData* data = (OSData*)commands->getObject(i);
		CustomCommand* customCommand = (CustomCommand*)data->getBytesNoCopy();

		if (((customCommand->OnInit && (newState == kStateInit)) ||
			(customCommand->OnWake && (newState == kStateWake)) ||
			(customCommand->OnSleep && (newState == kStateSleep))) &&
			(-1 == customCommand->layoutID || layoutID == customCommand->layoutID))
		{
			for (int i = 0; i < customCommand->CommandCount; i++)
			{
				DebugLog("--> custom command 0x%08x\n", customCommand->Commands[i]);
				executeCommand(customCommand->Commands[i]);
			}
		}
	}

	IORecursiveLockUnlock(g_lock);
}

/******************************************************************************
 * CodecCommander::setOutputs - set EAPD status bit on SP/HP
 ******************************************************************************/
bool CodecCommander::setEAPD(UInt8 logicLevel)
{
    // some codecs will produce loud pop when EAPD is enabled too soon, need custom delay until codec inits
    IOSleep(mConfiguration->getSendDelay());

	IORecursiveLockLock(g_lock);

    // for nodes supporting EAPD bit 1 in logicLevel defines EAPD logic state: 1 - enable, 0 - disable
	unsigned count = mEAPDCapableNodes->getCount();
	bool result = true;
	for (unsigned i = 0; i < count; i++)
	{
		OSNumber* nodeId = (OSNumber*)mEAPDCapableNodes->getObject(i);
		if (-1 == mIntelHDA->sendCommand(nodeId->unsigned8BitValue(), HDA_VERB_EAPDBTL_SET, logicLevel))
			result = false;
	}

	IORecursiveLockUnlock(g_lock);

	return result;
}

/******************************************************************************
 * CodecCommander::performCodecReset - reset function group and set power to D3
 *****************************************************************************/
void CodecCommander::performCodecReset()
{
	/*
     This function can be used to reset codec on dekstop boards, for example H87-HD3,
     to overcome audio loss and jack sense problem after sleep with AppleHDA v2.6.0+
     */

    if (!mColdBoot)
	{
		IORecursiveLockLock(g_lock);
		mIntelHDA->resetCodec();
        mEAPDPoweredDown = true;
		IORecursiveLockUnlock(g_lock);
    }
}

/******************************************************************************
 * CodecCommander::setPowerState - set active power state
 ******************************************************************************/
IOReturn CodecCommander::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
	DebugLog("setPowerState %ld\n", powerStateOrdinal);

	switch (powerStateOrdinal)
	{
		case kPowerStateSleep:
			DebugLog("--> asleep(%d)\n", (int)powerStateOrdinal);
			if (!mEAPDPoweredDown)
				// set EAPD logic level 0 to cause EAPD to power off properly
				handleStateChange(kIOAudioDeviceSleep);
			break;

		case kPowerStateDoze:	// note kPowerStateDoze never happens
		case kPowerStateNormal:
			DebugLog("--> awake(%d)\n", (int)powerStateOrdinal);
			if (mConfiguration->getPerformReset())
				// issue codec reset at wake and cold boot
				performCodecReset();

			// when "Perform Reset"=false and "Perform Reset on External Wake"=true...
			// we want power transitions, including setting EAPD to be handled
			// exclusively by setPowerStateExternal.
			if ((mConfiguration->getPerformReset() || !mConfiguration->getPerformResetOnExternalWake()) && mEAPDPoweredDown)
				// set EAPD bit at wake or cold boot
				handleStateChange(kIOAudioDeviceActive);

			// if infinite checking requested
			if (mConfiguration->getCheckInfinite())
			{
				// if checking infinitely then make sure to delay workloop
				if (mColdBoot)
					mTimer->setTimeoutMS(20000); // create a nasty 20sec delay for AudioEngineOutput to initialize
				// if we are waking it will be already initialized
				else
					mTimer->setTimeoutMS(100); // so fire timer for workLoop almost immediately
				
				DebugLog("--> workloop started\n");
			}
			break;
	}
	
    return IOPMAckImplied;
}

IOReturn CodecCommander::setPowerStateExternal(unsigned long powerStateOrdinal, IOService *policyMaker)
{
	DebugLog("setPowerStateExternal %ld\n", powerStateOrdinal);

	switch (powerStateOrdinal)
	{
		case kPowerStateSleep:
			DebugLog("--> asleep(%d)\n", (int)powerStateOrdinal);
			if (!mEAPDPoweredDown)
				// set EAPD logic level 0 to cause EAPD to power off properly
				handleStateChange(kIOAudioDeviceSleep);
			break;

		case kPowerStateDoze:	// note kPowerStateDoze never happens
		case kPowerStateNormal:
			DebugLog("--> awake(%d)\n", (int)powerStateOrdinal);
			if (mEAPDPoweredDown && mConfiguration->getPerformResetOnExternalWake())
				// issue codec reset at wake and cold boot
				performCodecReset();

			if (mEAPDPoweredDown)
				// set EAPD bit at wake or cold boot
				handleStateChange(kIOAudioDeviceActive);
			break;
	}

	return IOPMAckImplied;
}

/******************************************************************************
 * CodecCommander::executeCommand - Execute an external command
 ******************************************************************************/
UInt32 CodecCommander::executeCommand(UInt32 command)
{
	if (mIntelHDA)
		return mIntelHDA->sendCommand(command);
	
	return -1;
}

/******************************************************************************
 * CodecCommander::getPowerState - Get a textual description for a IOAudioDevicePowerState
 ******************************************************************************/
const char* CodecCommander::getPowerState(IOAudioDevicePowerState powerState)
{
	static const IONamedValue state_values[] = {
		{kIOAudioDeviceSleep,  "Sleep"  },
		{kIOAudioDeviceIdle,   "Idle"   },
		{kIOAudioDeviceActive, "Active" },
		{0,                    NULL     }
	};
	
	return IOFindNameForValue(powerState, state_values);
}


/******************************************************************************
 * CodecCommanderPowerHook - for tracking power states of IOAudioDevice nodes
 ******************************************************************************/

OSDefineMetaClassAndStructors(CodecCommanderPowerHook, IOService)

#ifdef DEBUG
bool CodecCommanderPowerHook::init(OSDictionary *dictionary)
{
	DebugLog("CodecCommanderPowerHook::init\n");
	uint32_t flag;
	if (PE_parse_boot_argn("-ccoff", &flag, sizeof(flag)))
	{
		AlwaysLog("stopping due to -ccoff kernel flag\n");
		return false;
	}

	if (!super::init(dictionary))
		return false;

	return true;
}

IOService* CodecCommanderPowerHook::probe(IOService* provider, SInt32* score)
{
	DebugLog("CodecCommanderPowerHook::probe\n");

	return super::probe(provider, score);
}
#endif //DEBUG

bool CodecCommanderPowerHook::start(IOService *provider)
{
	DebugLog("CodecCommanderPowerHook::start\n");

	if (!provider || !super::start(provider))
	{
		DebugLog("Error loading kernel extension.\n");
		return false;
	}

	// load configuration based on codec
	IORecursiveLockLock(g_lock);
	IntelHDA intelHDA(provider, PIO);
	Configuration config(this->getProperty(kCodecProfile), &intelHDA, kCodecCommanderPowerHookKey);
	IORecursiveLockUnlock(g_lock);

	// certain codecs are disabled (0x8086 for Intel HDMI, for example)
	if (config.getDisable())
	{
		AlwaysLog("no attempt to hook IOAudioDevice due to codec profile Disable flag\n");
		return false;
	}

	// walk up tree to find associated IOHDACodecFunction
	IORegistryEntry* entry = provider;
	while (entry)
	{
		if (OSDynamicCast(OSNumber, entry->getProperty(kCodecSubsystemID)))
			break;
		entry = entry->getParentEntry(gIOServicePlane);
	}
	if (!entry)
	{
		DebugLog("parent entry IOHDACodecFunction not found\n");
		return false;
	}
	// look at children for CodecCommander instance
	OSIterator* iter = entry->getChildIterator(gIOServicePlane);
	if (!iter)
	{
		DebugLog("can't get child iterator\n");
		return false;
	}
	while (OSObject* entry = iter->getNextObject())
	{
		CodecCommander* commander = OSDynamicCast(CodecCommander, entry);
		if (commander)
		{
			mCodecCommander = commander;
			break;
		}
	}
	iter->release();

	// if no CodecCommander instance found, don't attach
	if (!mCodecCommander)
	{
		DebugLog("no CodecCommander found with child iterator\n");
		return false;
	}

	// init power state management & set state as PowerOn
	PMinit();
	registerPowerDriver(this, powerStateArray, kPowerStateCount);
	provider->joinPMtree(this);

	this->registerService(0);
	return true;
}

void CodecCommanderPowerHook::stop(IOService *provider)
{
	mCodecCommander = NULL;

	PMstop();

	super::stop(provider);
}

IOReturn CodecCommanderPowerHook::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
	DebugLog("PowerHook: setPowerState %ld\n", powerStateOrdinal);

	if (mCodecCommander)
		return mCodecCommander->setPowerStateExternal(powerStateOrdinal, policyMaker);

	return IOPMAckImplied;
}

/******************************************************************************
 * CodecCommanderProbeInit - for hardware initialization at probe time
 ******************************************************************************/

OSDefineMetaClassAndStructors(CodecCommanderProbeInit, IOService)

static UInt32 getNumberFromArray(OSArray* array, unsigned index)
{
	OSNumber* num = OSDynamicCast(OSNumber, array->getObject(index));
	if (!num)
		return (UInt32)-1;
	return num->unsigned32BitValue();
}

IOService* CodecCommanderProbeInit::probe(IOService* provider, SInt32* score)
{
	DebugLog("CodecCommanderProbeInit::probe\n");
	uint32_t flag;
	if (PE_parse_boot_argn("-ccpioff", &flag, sizeof(flag)))
	{
		AlwaysLog("CodecCommanderProbeInit stopping due to -ccpioff kernel flag\n");
		return NULL;
	}

	IORecursiveLockLock(g_lock);

	IntelHDA intelHDA(provider, PIO);
	DebugLog("ProbeInit2 codec(pre-init) 0x%08x\n", intelHDA.getCodecVendorId());

	if (!intelHDA.initialize())
	{
		AlwaysLog("ProbeInit2 intelHDA.initialize failed\n");
		IORecursiveLockUnlock(g_lock);
		return NULL;
	}

	UInt32 layoutID = intelHDA.getLayoutID();
	if (-1 == layoutID)
	{
		IORecursiveLockUnlock(g_lock);
		return NULL;
	}

	DebugLog("ProbeInit2 codec 0x%08x\n", intelHDA.getCodecVendorId());

	Configuration config(this->getProperty(kCodecProfile), &intelHDA, kCodecCommanderProbeInitKey);

	// send any verbs in "Custom Commands"
	int commandsSent = 0;
	OSArray* commands = config.getCustomCommands();
	unsigned count = commands->getCount();
	for (unsigned i = 0; i < count; i++)
	{
		OSData* data = (OSData*)commands->getObject(i);
		CustomCommand* customCommand = (CustomCommand*)data->getBytesNoCopy();
		if (-1 == customCommand->layoutID || layoutID == customCommand->layoutID)
		{
			for (int i = 0; i < customCommand->CommandCount; i++)
			{
				DebugLog("--> custom probe command 0x%08x\n", customCommand->Commands[i]);
				intelHDA.sendCommand(customCommand->Commands[i]);
			}
			commandsSent++;
		}
	}

	if (commandsSent)
		AlwaysLog("CodecCommanderProbeInit sent %d command(s) during probe (0x%08x)\n", commandsSent, intelHDA.getCodecVendorId());

	// configure pin defaults from "PinConfigDefault"
	int pinConfigsSet = 0;
	if (OSArray* pinConfigs = config.getPinConfigDefault())
	{
		count = pinConfigs->getCount();
		for (unsigned i = 0; i < count; i++)
		{
			OSDictionary* dict = OSDynamicCast(OSDictionary, pinConfigs->getObject(i));
			if (!dict) continue;
			unsigned id = -1;
			if (OSNumber* num = OSDynamicCast(OSNumber, dict->getObject("LayoutID")))
				id = num->unsigned32BitValue();
			if ((UInt32)-1 == id || layoutID == id)
			{
				OSArray* pins = OSDynamicCast(OSArray, dict->getObject("PinConfigs"));
				if (!pins) continue;
				unsigned count = pins->getCount();
				if (count & 1) continue;
				for (int i = 0; i < count; i += 2)
				{
					UInt32 node = getNumberFromArray(pins, i);
					if ((UInt32)-1 != node)
					{
						UInt32 config = getNumberFromArray(pins, i+1);
						DebugLog("--> custom pin config, node=0x%02x : 0x%08x\n", node, config);
						intelHDA.sendCommand(node, HDA_VERB_SET_CONFIG_DEFAULT_BYTES_0, config>>0);
						intelHDA.sendCommand(node, HDA_VERB_SET_CONFIG_DEFAULT_BYTES_1, config>>8);
						intelHDA.sendCommand(node, HDA_VERB_SET_CONFIG_DEFAULT_BYTES_2, config>>16);
						intelHDA.sendCommand(node, HDA_VERB_SET_CONFIG_DEFAULT_BYTES_3, config>>24);
						pinConfigsSet++;
					}
				}
			}
		}
	}

	if (pinConfigsSet)
		AlwaysLog("CodecCommanderProbeInit set %d pinconfig(s) during probe (0x%08x)\n", pinConfigsSet, intelHDA.getCodecVendorId());

	IORecursiveLockUnlock(g_lock);

	return NULL;
}
