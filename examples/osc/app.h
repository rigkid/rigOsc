#pragma once

#include <memory>
#include <string>

#include <entt/entt.hpp>

#include "core/U_core.h"
#include "core/util/CommandLineArgs.h"

namespace rigkit {
class rigOsc;
}

/// Example for rigOsc: lamp follows Master / Blackout / Color on the show bus.
class OscHeroApp : public rigkit::IApp {
  public:
	OscHeroApp();

	void parseCommandLineArgs(const rigkit::CommandLineArgs& args) override;

	void setup() override;
	void update(float dt) override;
	void draw() override;

  private:
	void syncShowBus();
	void applyLamp();
	void applyWindowTitle();
	bool runSmoke();
	void maybeScreenshot(float dt);

	rigkit::CommandLineArgs m_cliArgs;
	std::shared_ptr<rigkit::rigOsc> m_osc;
	entt::entity m_glow = entt::null;
	entt::entity m_lamp = entt::null;

	float m_master = 1.f;
	bool m_blackout = false;
	float m_color[3] = {1.f, 0.72f, 0.35f};
	int m_heartbeat = 0;
	float m_heartbeatAccum = 0.f;
	float m_time = 0.f;
	bool m_smokeOsc = false;
	bool m_screenshot = false;
	bool m_screenshotRequested = false;
	float m_screenshotWait = 0.f;
	std::string m_screenshotPath;
	std::string m_status = "idle";
};
