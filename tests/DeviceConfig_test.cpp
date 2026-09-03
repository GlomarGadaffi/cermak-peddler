// DeviceConfig_test.cpp — SoftAP security settings on the host build.
//
// The flash-time seed reader (applyFlashSeed) is a deliberate no-op off-device:
// there is no `cfgseed` partition on the desktop simulator. What IS exercised
// here is everything the dashboard endpoints depend on — the default, the
// generated passphrase, and the validation that stands between an operator
// typo and an access point that refuses to come up.
//
// Why the passphrase rules matter enough to test: esp_wifi rejects a WPA2-PSK
// passphrase outside 8..63 characters outright. A device that accepted one and
// stored it would come up open (or not at all) on the *next boot*, long after
// the operator saw a success message — exactly the kind of silent downgrade a
// security toggle must never do.

#include <gtest/gtest.h>

#include "DeviceConfig.hpp"

#include <set>
#include <string>

namespace
{
	// The alphabet deliberately excludes ambiguous glyphs (0/O, 1/I/L, U)
	// because this passphrase is read off a small LCD or a serial log and
	// retyped into a desk phone.
	bool isFromPskAlphabet(const std::string& s)
	{
		const std::string alphabet = DeviceConfig::kPskAlphabet;
		for (char c : s)
		{
			if (alphabet.find(c) == std::string::npos)
			{
				return false;
			}
		}
		return true;
	}
}

TEST(DeviceConfig, ApSecurityDefaultsOff)
{
	// Fleet compatibility: a firmware update must not switch a live access point
	// to WPA2 and strand every phone already associated with it.
	DeviceConfig::clearAll();
	EXPECT_FALSE(DeviceConfig::isApSecure());
}

TEST(DeviceConfig, ApSecurityRoundTrips)
{
	DeviceConfig::clearAll();
	EXPECT_TRUE(DeviceConfig::setApSecure(true));
	EXPECT_TRUE(DeviceConfig::isApSecure());
	EXPECT_TRUE(DeviceConfig::setApSecure(false));
	EXPECT_FALSE(DeviceConfig::isApSecure());
	DeviceConfig::clearAll();
}

TEST(DeviceConfig, PassphraseIsGeneratedOnFirstAccess)
{
	// getApPsk() must never return something WPA2 would reject, even on a device
	// that has never been configured — the dashboard toggle has to have a real
	// passphrase to display the moment an operator opens it.
	DeviceConfig::clearAll();
	const std::string psk = DeviceConfig::getApPsk();

	EXPECT_EQ(psk.size(), DeviceConfig::kGeneratedPskChars);
	EXPECT_GE(psk.size(), DeviceConfig::kMinPskChars);
	EXPECT_LE(psk.size(), DeviceConfig::kMaxPskChars);
	EXPECT_TRUE(isFromPskAlphabet(psk)) << "unreadable glyphs in " << psk;

	// Stable across reads: regenerating on every call would mean the passphrase
	// shown on screen never matched the one the radio actually came up with.
	EXPECT_EQ(psk, DeviceConfig::getApPsk());
	DeviceConfig::clearAll();
}

TEST(DeviceConfig, GeneratedPassphrasesDiffer)
{
	// Per-device, not per-build. The bug this guards against is the one this
	// work removed: a single hardcoded "12345678" compiled into every unit and
	// published in a public repo, which made the onboarding AP's WPA2 purely
	// decorative — anyone who captured the 4-way handshake could derive the PTK.
	std::set<std::string> seen;
	for (int i = 0; i < 8; ++i)
	{
		DeviceConfig::clearAll();
		seen.insert(DeviceConfig::getApPsk());
	}
	EXPECT_GE(seen.size(), 7u) << "generated passphrases are not varying";
	DeviceConfig::clearAll();
}

TEST(DeviceConfig, RegenerateReplacesThePassphrase)
{
	DeviceConfig::clearAll();
	const std::string before = DeviceConfig::getApPsk();
	const std::string after  = DeviceConfig::regenerateApPsk();

	EXPECT_NE(before, after);
	EXPECT_EQ(after, DeviceConfig::getApPsk()) << "regenerate must persist";
	EXPECT_TRUE(isFromPskAlphabet(after));
	DeviceConfig::clearAll();
}

TEST(DeviceConfig, SetPassphraseEnforcesWpa2Bounds)
{
	DeviceConfig::clearAll();
	const std::string original = DeviceConfig::getApPsk();

	// Too short / too long: esp_wifi would reject these at AP bringup.
	EXPECT_FALSE(DeviceConfig::setApPsk(""));
	EXPECT_FALSE(DeviceConfig::setApPsk("short7"));
	EXPECT_FALSE(DeviceConfig::setApPsk(std::string(DeviceConfig::kMaxPskChars + 1, 'a')));

	// Non-printable bytes are refused too — a stray control character would be
	// accepted here and rejected by the radio much later.
	EXPECT_FALSE(DeviceConfig::setApPsk(std::string("has\tatab8")));

	// A rejected passphrase must leave the stored one untouched, so a typo can
	// never leave the access point half-configured.
	EXPECT_EQ(DeviceConfig::getApPsk(), original);

	// The boundaries themselves are valid.
	EXPECT_TRUE(DeviceConfig::setApPsk(std::string(DeviceConfig::kMinPskChars, 'a')));
	EXPECT_EQ(DeviceConfig::getApPsk(), std::string(DeviceConfig::kMinPskChars, 'a'));
	EXPECT_TRUE(DeviceConfig::setApPsk(std::string(DeviceConfig::kMaxPskChars, 'b')));
	EXPECT_EQ(DeviceConfig::getApPsk(), std::string(DeviceConfig::kMaxPskChars, 'b'));

	DeviceConfig::clearAll();
}

TEST(DeviceConfig, ApplyFlashSeedIsANoOpOffDevice)
{
	// There is no `cfgseed` partition on the host simulator. The same "absent
	// partition is not an error" path runs on real hardware whenever firmware
	// built with this feature lands on a board flashed before the partition
	// existed — an OTA update does not rewrite the partition table.
	DeviceConfig::clearAll();
	EXPECT_FALSE(DeviceConfig::applyFlashSeed());
	EXPECT_FALSE(DeviceConfig::isApSecure());
	DeviceConfig::clearAll();
}
