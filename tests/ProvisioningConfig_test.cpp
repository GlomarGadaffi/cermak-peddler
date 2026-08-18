// Tests for zero-touch phone auto-provisioning (Issue #35): the pure Yealink
// config builder (src/SIP/ProvisioningConfig.hpp) and the HttpServer/
// RequestsHandler wiring around it.

#include <gtest/gtest.h>

#include "HttpServer.hpp"
#include "ProvisioningConfig.hpp"
#include "RequestsHandler.hpp"

TEST(ProvisioningConfig, OpenModeConfigHasNoAuthWarningAndBlankPassword)
{
	std::string cfg = provisioning::yealinkConfigFor("101", "192.168.4.1", 5060,
		/*authRequired=*/false);

	EXPECT_NE(cfg.find("account.1.user_name = 101"), std::string::npos) << cfg;
	EXPECT_NE(cfg.find("account.1.sip_server.1.address = 192.168.4.1"), std::string::npos) << cfg;
	EXPECT_NE(cfg.find("account.1.sip_server.1.port = 5060"), std::string::npos) << cfg;
	EXPECT_NE(cfg.find("account.1.password = \r\n"), std::string::npos)
		<< "password field must be present but blank" << cfg;
	EXPECT_EQ(cfg.find("requires a SIP password"), std::string::npos)
		<< "Open/Learn-mode config should not carry the auth warning";
	// Only the two codecs the server actually enforces (enforceG711).
	EXPECT_NE(cfg.find("PCMU"), std::string::npos) << cfg;
	EXPECT_NE(cfg.find("PCMA"), std::string::npos) << cfg;
	EXPECT_EQ(cfg.find("G729"), std::string::npos) << cfg;
}

TEST(ProvisioningConfig, SecureModeConfigWarnsPasswordMustBeSetByHand)
{
	std::string cfg = provisioning::yealinkConfigFor("102", "192.168.4.1", 5060,
		/*authRequired=*/true);

	EXPECT_NE(cfg.find("requires a SIP password"), std::string::npos) << cfg;
	EXPECT_NE(cfg.find("account.1.password = \r\n"), std::string::npos)
		<< "still blank -- the server never has the plaintext to give out" << cfg;
}

// ── HttpServer::isProvisioningConfigPath (pure path-shape check) ───────────────

TEST(ProvisioningConfig, PathShapeAcceptsExactly12LowercaseHexChars)
{
	EXPECT_TRUE(HttpServer::isProvisioningConfigPath("/config/805ec079c37f.cfg"));
	EXPECT_TRUE(HttpServer::isProvisioningConfigPath("/config/0000000000ff.cfg"));
}

TEST(ProvisioningConfig, PathShapeRejectsWrongLengthOrCharsOrMissingPieces)
{
	EXPECT_FALSE(HttpServer::isProvisioningConfigPath("/config/805ec079c37.cfg"));    // 11 hex
	EXPECT_FALSE(HttpServer::isProvisioningConfigPath("/config/805ec079c37ff.cfg"));  // 13 hex
	EXPECT_FALSE(HttpServer::isProvisioningConfigPath("/config/805EC079C37F.cfg"));   // uppercase
	EXPECT_FALSE(HttpServer::isProvisioningConfigPath("/config/805ec079c37g.cfg"));   // non-hex 'g'
	EXPECT_FALSE(HttpServer::isProvisioningConfigPath("/config/805ec079c37f.txt"));   // wrong suffix
	EXPECT_FALSE(HttpServer::isProvisioningConfigPath("/configs/805ec079c37f.cfg"));  // wrong prefix
	EXPECT_FALSE(HttpServer::isProvisioningConfigPath("/config/805ec079c37f"));       // no suffix
	EXPECT_FALSE(HttpServer::isProvisioningConfigPath("/config/../../etc/passwd"));   // traversal attempt
}

// ── RequestsHandler::findProvisioningInfo ───────────────────────────────────────

TEST(ProvisioningConfig, FindProvisioningInfoReturnsNulloptForUnknownMac)
{
	RequestsHandler handler("192.168.4.1", 5060,
		[](const sockaddr_in&, std::shared_ptr<SipMessage>) {});
	EXPECT_FALSE(handler.findProvisioningInfo("805ec079c37f").has_value());
}

// ── Config-line injection via the extension (Issue #107) ──────────────────────
//
// yealinkConfigFor() interpolates the extension into `key = value\r\n` lines. An
// extension carrying a CR or LF could therefore append config keys the admin
// never wrote -- `account.1.password` being the interesting one. onRegister()
// gates every adopted extension through isValidAor(), whose charset excludes
// CR/LF, so this is not reachable today; these pin the format-level backstop so
// it stays unreachable if a future caller feeds the builder from somewhere else.

TEST(ProvisioningConfig, BuilderRefusesExtensionCarryingCrlfInjection)
{
	std::string cfg = provisioning::yealinkConfigFor(
		"101\r\naccount.1.password = hunter2", "192.168.4.1", 5060,
		/*authRequired=*/false);

	EXPECT_TRUE(cfg.empty())
		<< "a CRLF-bearing extension must produce no config at all, rather than "
		   "one carrying the injected line: " << cfg;
}

TEST(ProvisioningConfig, BuilderRefusesBareCrAndBareLf)
{
	EXPECT_TRUE(provisioning::yealinkConfigFor("101\naccount.1.password = x",
		"192.168.4.1", 5060, false).empty()) << "bare LF";
	EXPECT_TRUE(provisioning::yealinkConfigFor("101\raccount.1.password = x",
		"192.168.4.1", 5060, false).empty()) << "bare CR";
	// Trailing, not just embedded -- a lone terminator still opens the next line.
	EXPECT_TRUE(provisioning::yealinkConfigFor("101\r\n",
		"192.168.4.1", 5060, false).empty()) << "trailing CRLF";
}

TEST(ProvisioningConfig, BuilderStillAcceptsTheRestOfTheAorCharset)
{
	// The guard rejects CR/LF only. Star codes, '+', and the RFC 3261 user-part
	// punctuation isValidAor() allows are legitimate extensions and must still
	// provision -- a stricter [0-9A-Za-z] strip would silently mangle these.
	for (const char* ext : {"*55", "+15551234", "1_0.1-a", "#77"})
	{
		std::string cfg = provisioning::yealinkConfigFor(ext, "192.168.4.1", 5060,
			/*authRequired=*/false);
		EXPECT_NE(cfg.find(std::string("account.1.user_name = ") + ext), std::string::npos)
			<< "extension '" << ext << "' should provision unchanged: " << cfg;
	}
}
