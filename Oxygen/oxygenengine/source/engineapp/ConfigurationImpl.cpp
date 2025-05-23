#include "engineapp/pch.h"
#include "engineapp/ConfigurationImpl.h"

#include "oxygen/helper/JsonHelper.h"


ConfigurationImpl::ConfigurationImpl()
{
	// Change the default value for script optimization, it should be relatively low to not introduce wrong debugging outputs
	mScriptOptimizationLevel = 1;
}

bool ConfigurationImpl::loadConfigurationInternal(JsonSerializer& serializer)
{
	// Enable dev mode in any case
	Configuration::instance().mDevMode.mEnabled = true;

	return true;
}

bool ConfigurationImpl::loadSettingsInternal(JsonSerializer& serializer, SettingsType settingsType)
{
	return true;
}
