// Regression coverage for the dashboard snapshot swap in RequestsHandler::tick().
//
// The snapshot has two kinds of field. Most are rebuilt from scratch every tick
// (clients, sessions, cdr, dnd, forwards, ringGroups, parkedCalls). Two are
// mirrored out of band because their sources only move on an admin action or a
// REGISTER: `pageZones` (setPageZone) and `devices` (the Registrar registry).
// tick() assembles a fresh RegistrarSnapshot and move-assigns it over the live
// one, so the out-of-band fields have to be carried across that swap or they are
// blanked once a second.

#include <gtest/gtest.h>

#include "RequestsHandler.hpp"

namespace
{
	RequestsHandler makeHandler()
	{
		return RequestsHandler("127.0.0.1", 5060,
			[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	}
}

// A configured paging zone must still be on the dashboard after a tick.
TEST(DashboardSnapshot, TickPreservesConfiguredPageZones)
{
	auto handler = makeHandler();

	handler.setPageZone("980", "101,102,103");
	ASSERT_EQ(handler.getPageZones().size(), 1u);

	handler.tick();

	auto zones = handler.getPageZones();
	ASSERT_EQ(zones.size(), 1u) << "tick() blanked the out-of-band pageZones mirror";
	EXPECT_EQ(zones[0].first, "980");
	EXPECT_EQ(zones[0].second, "101,102,103");

	// Still there after several more ticks — the swap must not erode it.
	handler.tick();
	handler.tick();
	EXPECT_EQ(handler.getPageZones().size(), 1u);
}

// Clearing a zone must survive a tick too — the carry-over must not resurrect
// state that was deliberately removed.
TEST(DashboardSnapshot, TickPreservesPageZoneRemoval)
{
	auto handler = makeHandler();

	handler.setPageZone("981", "201,202");
	ASSERT_EQ(handler.getPageZones().size(), 1u);

	handler.setPageZone("981", "");          // empty membership deletes the zone
	ASSERT_TRUE(handler.getPageZones().empty());

	handler.tick();
	EXPECT_TRUE(handler.getPageZones().empty());
}

// The adopted-device mirror rides the same carry-over. It cannot be populated
// on host (adoption needs an ARP lookup, which is a nullopt stub off-device), so
// this pins the reachable half: an empty registry stays empty and tick() does
// not fault on the moved-from vectors.
TEST(DashboardSnapshot, TickKeepsDeviceMirrorConsistent)
{
	auto handler = makeHandler();

	EXPECT_TRUE(handler.getAdoptedDevices().empty());
	handler.tick();
	EXPECT_TRUE(handler.getAdoptedDevices().empty());
	handler.tick();
	EXPECT_TRUE(handler.getAdoptedDevices().empty());
}
