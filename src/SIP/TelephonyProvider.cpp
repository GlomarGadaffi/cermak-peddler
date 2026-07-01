#include "TelephonyProvider.hpp"

const char* telephonyProviderName(TelephonyProviderType t)
{
	switch (t)
	{
	case TelephonyProviderType::Loopback: return "LOOPBACK";
	default:                              return "?";
	}
}

bool telephonyProviderImplemented(TelephonyProviderType t)
{
	return t == TelephonyProviderType::Loopback;
}

bool TelephonyProviderRegistry::registerProvider(TelephonyProviderType t, AnchorClient* provider)
{
	const size_t i = static_cast<size_t>(t);
	if (provider == nullptr || i >= kMaxProviders)
	{
		return false;
	}
	if (_providers[i] != nullptr && _providers[i] != provider)
	{
		return false;  // slot taken by a different instance
	}
	_providers[i] = provider;
	return true;
}

AnchorClient* TelephonyProviderRegistry::select(TelephonyProviderType t) const
{
	const size_t i = static_cast<size_t>(t);
	if (i >= kMaxProviders)
	{
		return nullptr;
	}
	return _providers[i];
}
