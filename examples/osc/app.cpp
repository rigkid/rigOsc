#include "app.h"

#include <cmath>
#include <filesystem>
#include <iostream>

#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include "core/RigKitEngine.h"
#include "core/pack/MPack.h"
#include "core/util/AppPaths.h"
#include "packs/rigComponent/src/CDrawStyle.h"
#include "packs/rigComponent/src/CTransform.h"
#include "packs/rigComponent/src/rig.h"
#include "packs/rigComponent/src/rigComponent.h"
#include "packs/rigImGui/src/ExportPng.h"
#include "packs/rigImGui/src/IWindow.h"
#include "packs/rigImGui/src/Mui.h"
#include "packs/rigImGui/src/rigImGui.h"
#include "packs/rigOsc/src/CNetworkIdentity.h"
#include "packs/rigOsc/src/COscEndpoint.h"
#include "packs/rigOsc/src/COscShowBus.h"
#include "packs/rigOsc/src/rigOsc.h"
#include "packs/rigSystems/src/rigSystems.h"

namespace {

class ShowControlWindow : public rigkit::IWindow {
  public:
	ShowControlWindow(rigkit::rigOsc* osc, float* master, bool* blackout, float* color,
					  std::string* status, const int* heartbeat)
		: IWindow("Show Control", 0),
		  m_osc(osc),
		  m_master(master),
		  m_blackout(blackout),
		  m_color(color),
		  m_status(status),
		  m_heartbeat(heartbeat) {}

  protected:
	void renderContents() override {
		ImGui::TextUnformatted("Show bus drives the lamp");
		ImGui::Separator();

		if (m_master) {
			ImGui::SliderFloat("Master", m_master, 0.f, 1.f);
		}
		if (m_blackout) {
			ImGui::Checkbox("Blackout", m_blackout);
		}
		if (m_color) {
			ImGui::ColorEdit3("Color", m_color, ImGuiColorEditFlags_Float);
		}

		ImGui::Separator();
		ImGui::Text("Status: %s", m_status ? m_status->c_str() : "?");
		ImGui::Text("Heartbeat: %d", m_heartbeat ? *m_heartbeat : 0);
		if (m_osc) {
			const auto& from = m_osc->showBus().lastFrom;
			ImGui::Text("Last from: %s", from.empty() ? "-" : from.c_str());
		}

		if (!m_osc) {
			ImGui::TextDisabled("rigOsc not loaded");
			return;
		}

		ImGui::Separator();
		const auto& id = m_osc->identity();
		const auto& ep = m_osc->endpoint();
		ImGui::Text("Network id: %s", id.networkId.c_str());
		ImGui::Text("Listen: %s:%d", id.bindAddress.c_str(), ep.listenPort);
		ImGui::Text("Send: %s:%d", ep.sendHost.c_str(), ep.sendPort);
		if (m_osc->listening()) {
			ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.f), "Listening");
		} else {
			ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f), "Not listening");
		}
		if (!m_osc->lastError().empty()) {
			ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "%s", m_osc->lastError().c_str());
		}

		ImGui::TextWrapped("Directed: /rigkit/<id>/…  Broadcast: /rigkit/…  "
						   "Bus sends append sender id. Two instances need different "
						   "--listen-port; point --send-port at the peer.");

		ImGui::Separator();
		if (ImGui::Button("Broadcast Color") && m_color) {
			const bool ok = m_osc->send(m_osc->makeBroadcastAddress("color"),
										{m_color[0], m_color[1], m_color[2], id.networkId});
			if (m_status) {
				*m_status = ok ? ("broadcast color as " + id.networkId)
							   : ("broadcast failed: " + m_osc->lastError());
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Broadcast Master") && m_master) {
			const bool ok = m_osc->send(m_osc->makeBroadcastAddress("master"),
										{*m_master, id.networkId});
			if (m_status) {
				*m_status = ok ? ("broadcast master as " + id.networkId)
							   : ("broadcast failed: " + m_osc->lastError());
			}
		}
		if (ImGui::Button("Run smokeLoopback") && m_status) {
			*m_status = m_osc->smokeLoopback() ? "smokeLoopback OK" : "smokeLoopback FAILED";
		}
	}

  private:
	rigkit::rigOsc* m_osc = nullptr;
	float* m_master = nullptr;
	bool* m_blackout = nullptr;
	float* m_color = nullptr;
	std::string* m_status = nullptr;
	const int* m_heartbeat = nullptr;
};

} // namespace

OscHeroApp::OscHeroApp() {
	window().width = 800;
	window().height = 480;
	window().title = "rigOsc - osc";
	settings().appName = "osc";
}

void OscHeroApp::parseCommandLineArgs(const rigkit::CommandLineArgs& args) {
	rigkit::IApp::parseCommandLineArgs(args);
	m_cliArgs = args;
	if (args.hasFlag("smoke-osc")) {
		m_smokeOsc = true;
	}
	if (auto path = args.getValue("screenshot")) {
		m_screenshot = true;
		m_screenshotPath = *path;
	} else if (args.hasFlag("screenshot")) {
		m_screenshot = true;
	}
	if (args.hasFlag("help") || args.hasFlag("h")) {
		std::cout << "osc — rigOsc example (show-bus lamp)\n\n"
				  << "  --smoke-osc                 Bind UDP, loopback master, exit\n"
				  << "  --screenshot[=PATH]         Capture framebuffer PNG and exit\n"
				  << rigkit::rigOsc::commandLineHelp() << "\n"
				  << "  --help\n";
		std::exit(0);
	}
}

void OscHeroApp::applyWindowTitle() {
	std::string title = "rigOsc - osc";
	if (m_osc) {
		const auto& id = m_osc->identity().networkId;
		if (!id.empty()) {
			title += " [" + id + "]";
		}
	}
	window().title = title;
	if (auto* win = m_engine->getWindow()) {
		glfwSetWindowTitle(win, title.c_str());
	}
}

void OscHeroApp::setup() {
	spdlog::info("osc — show-bus lamp");
	m_engine->setClearColor(0.06f, 0.07f, 0.09f, 1.f);

	auto* packs = m_engine->getPackManager();
	if (!packs) {
		return;
	}

	packs->registerPack<rigkit::rigComponent>();
	packs->registerPack<rigkit::rigSystems>();
	m_osc = std::make_shared<rigkit::rigOsc>();
	packs->registerPack(m_osc);
	packs->registerPack<rigkit::rigImGui>();
	packs->initAll();
	packs->setupAll();

	if (m_osc && !m_osc->applyCommandLine(m_cliArgs)) {
		spdlog::error("osc: OSC bind failed — {}", m_osc->lastError());
		m_status = "bind failed";
	}
	applyWindowTitle();

	const float cx = static_cast<float>(window().width) * 0.5f;
	const float cy = static_cast<float>(window().height) * 0.52f;
	m_glow = rig::makeCircle(cx, cy, 150.f, rig::fill(m_color[0], m_color[1], m_color[2], 0.18f),
							 "glow");
	m_lamp = rig::makeCircle(cx, cy, 78.f, rig::fill(m_color[0], m_color[1], m_color[2]), "lamp");

	if (auto* ui = m_engine->getUiManager()) {
		if (auto* mui = dynamic_cast<rigkit::Mui*>(ui)) {
			mui->setDockPassthroughCentral(true);
			mui->setFirstRunHostDockLayout({"Show Control"});
		}
		if (auto* wm = ui->getWindowManager()) {
			wm->createWindow<ShowControlWindow>(m_osc.get(), &m_master, &m_blackout, m_color,
												&m_status, &m_heartbeat);
			wm->showWindow("Show Control");
		}
	}

	syncShowBus();
	applyLamp();

	if (m_smokeOsc) {
		const bool ok = runSmoke();
		if (auto* win = m_engine->getWindow()) {
			glfwSetWindowShouldClose(win, GLFW_TRUE);
		}
		if (!ok) {
			std::exit(1);
		}
	}
}

void OscHeroApp::syncShowBus() {
	if (!m_osc) {
		return;
	}
	auto& bus = m_osc->showBus();
	if (bus.masterFromNet) {
		m_master = bus.master;
		bus.masterFromNet = false;
	} else {
		bus.master = m_master;
	}
	if (bus.blackoutFromNet) {
		m_blackout = bus.blackout;
		bus.blackoutFromNet = false;
	} else {
		bus.blackout = m_blackout;
	}
	if (bus.colorFromNet) {
		m_color[0] = bus.colorR;
		m_color[1] = bus.colorG;
		m_color[2] = bus.colorB;
		bus.colorFromNet = false;
	} else {
		bus.colorR = m_color[0];
		bus.colorG = m_color[1];
		bus.colorB = m_color[2];
	}
	if (bus.statusFromNet) {
		m_status = bus.status;
		bus.statusFromNet = false;
	} else {
		bus.status = m_status;
	}
	bus.heartbeat = m_heartbeat;
}

void OscHeroApp::applyLamp() {
	auto* ecs = m_engine->getECSManager();
	if (!ecs) {
		return;
	}

	float level = m_blackout ? 0.f : m_master;
	const float pulse = 0.92f + 0.08f * std::sin(m_time * 2.2f);
	level *= pulse;

	const float r = m_color[0];
	const float g = m_color[1];
	const float b = m_color[2];

	if (m_lamp != entt::null && ecs->hasComponent<rigkit::ecs::CDrawStyle>(m_lamp)) {
		auto& style = ecs->getComponent<rigkit::ecs::CDrawStyle>(m_lamp);
		style.setFillColor(r * level, g * level, b * level, 1.f);
	}
	if (m_lamp != entt::null && ecs->hasComponent<rigkit::ecs::CTransform>(m_lamp)) {
		const float s = 0.4f + 0.6f * (m_blackout ? 0.f : m_master);
		ecs->getComponent<rigkit::ecs::CTransform>(m_lamp).scale = {s, s, 1.f};
	}
	if (m_glow != entt::null && ecs->hasComponent<rigkit::ecs::CDrawStyle>(m_glow)) {
		auto& style = ecs->getComponent<rigkit::ecs::CDrawStyle>(m_glow);
		style.setFillColor(r, g, b, 0.08f + 0.22f * level);
	}
	if (m_glow != entt::null && ecs->hasComponent<rigkit::ecs::CTransform>(m_glow)) {
		const float s = 0.55f + 0.45f * (m_blackout ? 0.f : m_master);
		ecs->getComponent<rigkit::ecs::CTransform>(m_glow).scale = {s, s, 1.f};
	}
}

bool OscHeroApp::runSmoke() {
	if (!m_osc) {
		spdlog::error("smoke-osc: rigOsc not registered");
		return false;
	}
	const bool ok = m_osc->smokeLoopback();
	m_status = ok ? "smokeLoopback OK" : "smokeLoopback FAILED";
	if (ok) {
		spdlog::info("osc --smoke-osc OK");
	} else {
		spdlog::error("osc --smoke-osc failed: {}", m_osc->lastError());
	}
	return ok;
}

void OscHeroApp::maybeScreenshot(float dt) {
	if (!m_screenshot || !m_engine) {
		return;
	}
	m_screenshotWait += dt;
	// Let the first-run dock + a couple of presents settle.
	if (!m_screenshotRequested && m_screenshotWait >= 1.0f) {
		if (auto* mui = dynamic_cast<rigkit::Mui*>(m_engine->getUiManager())) {
			mui->requestExportPng();
			m_screenshotRequested = true;
		} else {
			spdlog::error("screenshot: no Mui");
			m_screenshot = false;
		}
		return;
	}
	if (!m_screenshotRequested || m_screenshotWait < 1.15f) {
		return;
	}

	namespace fs = std::filesystem;
	const fs::path exportDir = fs::path(AppPaths::getDataDir()) / "export";
	fs::path newest;
	std::error_code ec;
	fs::file_time_type newestTime{};
	bool have = false;
	if (fs::exists(exportDir, ec)) {
		for (const auto& entry : fs::directory_iterator(exportDir, ec)) {
			if (!entry.is_regular_file(ec) || entry.path().extension() != ".png") {
				continue;
			}
			const auto t = entry.last_write_time(ec);
			if (!have || t > newestTime) {
				newest = entry.path();
				newestTime = t;
				have = true;
			}
		}
	}
	if (!have) {
		spdlog::error("screenshot: no PNG in {}", exportDir.string());
		m_screenshot = false;
		if (auto* win = m_engine->getWindow()) {
			glfwSetWindowShouldClose(win, GLFW_TRUE);
		}
		std::exit(1);
	}

	fs::path dest = m_screenshotPath.empty() ? newest : fs::path(m_screenshotPath);
	if (!m_screenshotPath.empty()) {
		fs::create_directories(dest.parent_path(), ec);
		fs::copy_file(newest, dest, fs::copy_options::overwrite_existing, ec);
		if (ec) {
			spdlog::error("screenshot: copy failed — {}", ec.message());
			m_screenshot = false;
			std::exit(1);
		}
	}
	spdlog::info("screenshot wrote {}", dest.string());
	m_screenshot = false;
	if (auto* win = m_engine->getWindow()) {
		glfwSetWindowShouldClose(win, GLFW_TRUE);
	}
}

void OscHeroApp::update(float dt) {
	m_time += dt;
	m_heartbeatAccum += dt;
	if (m_heartbeatAccum >= 1.f) {
		m_heartbeatAccum -= 1.f;
		++m_heartbeat;
	}
	syncShowBus();
	applyLamp();
	maybeScreenshot(dt);
}

void OscHeroApp::draw() {
	float level = m_blackout ? 0.f : m_master;
	m_engine->setClearColor(0.04f + 0.22f * m_color[0] * level, 0.04f + 0.22f * m_color[1] * level,
							0.05f + 0.22f * m_color[2] * level, 1.f);
}
