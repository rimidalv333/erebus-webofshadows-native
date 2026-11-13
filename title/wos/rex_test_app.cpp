/*
 * WebOfShadowsRecomp for the PS5 - the test app: the parts of the runtime the
 * game will stand on, checked on the console without the game's code.
 *
 * Built with WOS_TITLE_APP=test (wos/wos.cmake). It runs on the fork's PS5 app
 * layer through rex_ps5_app_main (wos/title_main.cpp) and checks, in order:
 *
 * 1. the window: the display surface's size;
 * 2. the GPU: the emulated Xenos GPU's Vulkan side, created as the game creates
 *    it (rex_gpu_create, backend "vulkan") and set up for presentation: RADV's
 *    device with the features the GPU emulation needs, which it lists, and the
 *    presenter on the display;
 * 3. the picture: the runtime's own ImGui overlay, through that presenter, over
 *    four seconds of red, green, blue and white;
 * 4. the guest memory: Memory::Initialize, and a read of this app's code;
 * 5. the sound: a one-second 440 Hz tone through the PS5 sound driver;
 *
 * then quits through the app context. Every line goes to the results file
 * (/app0/wos-test-results.txt) and klog before its step, so a file that stops
 * says where.
 *
 * Copyright (C) 2026 KongaTime (reeot/rex_test_app.cpp: the memory and sound steps)
 * SPDX-License-Identifier: MIT
 */

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>

#include <imgui.h>

#include <rex/audio/prospero/prospero_audio_driver.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/xmemory.h>
#include <rex/thread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context.h>

extern "C" void wos_note(const char *format, ...) __attribute__((format(printf, 1, 2)));
extern "C" rex::system::IGraphicsSystem *rex_gpu_create(uint32_t abi_version, const rex::system::GpuCreateInfo *info);

namespace
{
uint32_t BigEndianFloat(float value)
{
	return __builtin_bswap32(std::bit_cast<uint32_t>(value));
}

// The picture: the whole screen in one colour a second, with the title's name
class ColourDialog final : public rex::ui::ImGuiDialog {
    public:
	explicit ColourDialog(rex::ui::ImGuiDrawer *drawer) : ImGuiDialog(drawer) {}
	std::atomic<int> colour{0};
	std::atomic<uint64_t> frames{0};

    protected:
	void OnDraw(ImGuiIO &io) override
	{
		static const ImU32 colours[] = {IM_COL32(220, 20, 30, 255), IM_COL32(20, 200, 40, 255),
						IM_COL32(30, 60, 230, 255), IM_COL32(255, 255, 255, 255)};
		auto *list = ImGui::GetBackgroundDrawList();
		list->AddRectFilled(ImVec2(0, 0), io.DisplaySize, colours[colour.load() & 3]);
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
					ImVec2(0.5f, 0.5f));
		ImGui::Begin("WebOfShadowsRecomp", nullptr,
			     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
		ImGui::Text("Web of Shadows recomp for the PS5: test app");
		ImGui::Text("The emulated GPU's presenter on RADV, %.0fx%.0f", io.DisplaySize.x, io.DisplaySize.y);
		ImGui::End();
		frames.fetch_add(1, std::memory_order_relaxed);
	}
};

class RexTestApp final : public rex::ui::WindowedApp {
    public:
	static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext &context)
	{
		return std::unique_ptr<rex::ui::WindowedApp>(new RexTestApp(context));
	}

	bool OnInitialize() override
	{
		wos_note("==== WebOfShadowsRecomp test app: the runtime's GPU, picture, memory and sound ====");
		wos_note("window: creating it");
		window_ = rex::ui::Window::Create(app_context(), "WebOfShadowsRecomp");
		if (!window_) {
			wos_note("window: FAILED to create");
			return false;
		}
		if (!SetUpGpu()) {
			// The rest is still worth knowing
			window_->Open();
		}
		wos_note("window: open, %ux%u", window_->GetActualPhysicalWidth(), window_->GetActualPhysicalHeight());
		worker_ = std::thread([this] { Run(); });
		return true;
	}

	void OnDestroy() override
	{
		stopping_ = true;
		if (worker_.joinable())
			worker_.join();
		delete dialog_;
		dialog_ = nullptr;
		imgui_drawer_.reset();
		if (window_)
			window_->SetPresenter(nullptr);
		immediate_drawer_.reset();
		window_.reset();
		if (graphics_)
			graphics_->Shutdown();
		graphics_.reset();
		memory_.reset();
		wos_note("==== WebOfShadowsRecomp test app: done ====");
	}

    private:
	explicit RexTestApp(rex::ui::WindowedAppContext &context) : WindowedApp(context, "wos_test") {}

	// As the game's app does (wos/app/wos_ps5_app.cpp) and ReXApp then does with it
	bool SetUpGpu()
	{
		wos_note("gpu: rex_gpu_create (the emulated Xenos GPU linked into the title, Vulkan)");
		rex::system::GpuCreateInfo info{};
		info.struct_size = sizeof info;
		info.backend = "vulkan";
		graphics_.reset(rex_gpu_create(rex::system::kGpuPluginAbiVersion, &info));
		if (!graphics_) {
			wos_note("gpu: FAILED to create the graphics system");
			return false;
		}
		wos_note("gpu: SetupPresentation (RADV's instance and device, the presenter on the display)");
		const auto status = graphics_->SetupPresentation(&app_context());
		if (status != 0) {
			wos_note("gpu: SetupPresentation FAILED: %08X", unsigned(status));
			return false;
		}
		auto *provider = graphics_->provider();
		auto *presenter = graphics_->presenter();
		wos_note("gpu: provider %s, presenter %s", provider ? "ok" : "MISSING", presenter ? "ok" : "MISSING");
		if (!provider || !presenter)
			return false;
		LogDevice(*static_cast<rex::ui::vulkan::VulkanProvider *>(provider));

		if (!window_->Open()) {
			wos_note("window: FAILED to open");
			return false;
		}
		immediate_drawer_ = provider->CreateImmediateDrawer();
		if (!immediate_drawer_) {
			wos_note("picture: FAILED to create the immediate drawer");
			return false;
		}
		immediate_drawer_->SetPresenter(presenter);
		imgui_drawer_ = std::make_unique<rex::ui::ImGuiDrawer>(window_.get(), 64);
		imgui_drawer_->SetPresenterAndImmediateDrawer(presenter, immediate_drawer_.get());
		dialog_ = new ColourDialog(imgui_drawer_.get());
		window_->SetPresenter(presenter);
		gpu_ok_ = true;
		return true;
	}

	static void LogDevice(const rex::ui::vulkan::VulkanProvider &provider)
	{
		const auto *device = provider.vulkan_device();
		if (!device) {
			wos_note("gpu: no Vulkan device");
			return;
		}
		const auto &p = device->properties();
		wos_note("gpu: device driver id %d, max 2D image %u, max storage buffer range %u MB", int(p.driverID),
			 p.maxImageDimension2D, p.maxStorageBufferRange >> 20);
		wos_note("gpu: geometryShader %d tessellationShader %d fillModeNonSolid %d vertexStoresAndAtomics %d "
			 "fragmentStoresAndAtomics %d",
			 p.geometryShader, p.tessellationShader, p.fillModeNonSolid, p.vertexPipelineStoresAndAtomics,
			 p.fragmentStoresAndAtomics);
		wos_note("gpu: sparseBinding %d sparseResidencyBuffer %d fragmentShaderPixelInterlock %d "
			 "fragmentShaderSampleInterlock %d",
			 p.sparseBinding, p.sparseResidencyBuffer, p.fragmentShaderPixelInterlock,
			 p.fragmentShaderSampleInterlock);
		wos_note("gpu: dynamicRendering %d shaderDemoteToHelper %d customBorderColors %d nullDescriptor %d "
			 "independentBlend %d depthClamp %d occlusionQueryPrecise %d samplerAnisotropy %d (max %.0f)",
			 p.dynamicRendering, p.shaderDemoteToHelperInvocation, p.customBorderColors, p.nullDescriptor,
			 p.independentBlend, p.depthClamp, p.occlusionQueryPrecise, p.samplerAnisotropy,
			 double(p.maxSamplerAnisotropy));
		wos_note("gpu: shader stencil export %d, memory budget %d",
			 device->extensions().ext_EXT_shader_stencil_export, device->extensions().ext_EXT_memory_budget);
	}

	void Run()
	{
		if (gpu_ok_) {
			wos_note("picture: four seconds, red, green, blue, white");
			for (int colour = 0; colour < 4 && !stopping_; colour++) {
				dialog_->colour = colour;
				const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
				while (std::chrono::steady_clock::now() < end && !stopping_) {
					app_context().CallInUIThread([this] {
						if (window_)
							window_->RequestPaint();
					});
					std::this_thread::sleep_for(std::chrono::milliseconds(16));
				}
			}
			wos_note("picture: %llu frames drawn by the overlay",
				 (unsigned long long)dialog_->frames.load());
		} else {
			wos_note("picture: skipped (no GPU)");
		}
		if (TestMemory())
			TestSound();
		wos_note("quitting through the app context");
		app_context().CallInUIThread([this] { app_context().QuitFromUIThread(); });
	}

	bool TestMemory()
	{
		wos_note("memory: Memory::Initialize (the 4.5 GiB guest arena, the MMIO handler)");
		memory_ = std::make_unique<rex::memory::Memory>();
		if (!memory_->Initialize()) {
			wos_note("memory: FAILED to initialize");
			return false;
		}
		wos_note("memory: initialized");
		const volatile uint8_t *code = reinterpret_cast<const volatile uint8_t *>(&RexTestApp::Create);
		uint8_t bytes[4];
		for (int i = 0; i < 4; i++)
			bytes[i] = code[i];
		wos_note("code: readable: %02x %02x %02x %02x", bytes[0], bytes[1], bytes[2], bytes[3]);
		return true;
	}

	void TestSound()
	{
		using rex::audio::prospero::kSamplesPerFrame;
		constexpr size_t kGuestChannels = 6;
		constexpr int kFrames = 188; // a second of 256-sample periods at 48 kHz
		wos_note("sound: a one-second beep through the PS5 driver (sceAudioOut)");
		auto semaphore = rex::thread::Semaphore::Create(64, 64);
		auto driver = std::make_unique<rex::audio::prospero::ProsperoAudioDriver>(memory_.get(), semaphore.get());
		if (!driver->Initialize()) {
			wos_note("sound: FAILED to open the driver");
			return;
		}
		const uint32_t frame = memory_->SystemHeapAlloc(uint32_t(kSamplesPerFrame * kGuestChannels * 4));
		auto *planes = memory_->TranslateVirtual<uint32_t *>(frame);
		double phase = 0;
		int submitted = 0;
		for (; submitted < kFrames && !stopping_; submitted++) {
			if (rex::thread::Wait(semaphore.get(), false, std::chrono::milliseconds(1000)) !=
			    rex::thread::WaitResult::kSuccess)
				break;
			for (size_t i = 0; i < kSamplesPerFrame; i++) {
				const float value = float(0.25 * std::sin(phase));
				phase += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
				for (size_t channel = 0; channel < kGuestChannels; channel++)
					planes[channel * kSamplesPerFrame + i] = BigEndianFloat(channel < 2 ? value : 0.0f);
			}
			driver->SubmitFrame(frame);
		}
		driver->Shutdown();
		memory_->SystemHeapFree(frame);
		wos_note("sound: %d frames played", submitted);
	}

	std::unique_ptr<rex::ui::Window> window_;
	std::unique_ptr<rex::system::IGraphicsSystem> graphics_;
	std::unique_ptr<rex::ui::ImmediateDrawer> immediate_drawer_;
	std::unique_ptr<rex::ui::ImGuiDrawer> imgui_drawer_;
	ColourDialog *dialog_ = nullptr;
	bool gpu_ok_ = false;
	std::unique_ptr<rex::memory::Memory> memory_;
	std::thread worker_;
	std::atomic<bool> stopping_{false};
};
} // namespace

REX_DEFINE_APP(wos_test, RexTestApp::Create);
