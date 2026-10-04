#include "menu.h"

#include <d3d11.h>
#include <dxgi.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include "settings.h"

// The in-game options menu: a Dear ImGui overlay drawn by the plugin itself (built into the DLL,
// nothing else to install). Settings::menuKey (F10) opens and closes it, Esc closes it. While it
// is open the game is frozen and gets no input: the plugin swaps the event list Skyrim's input
// device manager sends out for an empty one (and feeds the real events to ImGui). Every change is
// saved to Skydive.ini straight away.
//
// Drawing: a hook on the swap chain's Present (vtable slot 8) renders ImGui onto the back buffer.

namespace menu {

namespace {

std::atomic<bool> g_open{false};
bool g_ready = false;
bool g_inputHooked = false;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;

// Input for ImGui, queued by the input hook (main thread) and read in Present.
struct Event {
    enum Type { kMouseMove, kMouseButton, kWheel, kKey, kChar } type;
    int a = 0, b = 0;
    bool down = false;
};
std::mutex g_eventLock;
std::vector<Event> g_events;
float g_mouseX = -1.0f, g_mouseY = -1.0f;
bool g_frozeTime = false;
bool g_dirty = false;  // settings changed, not saved yet

void SetOpen(bool open) {
    if (open == g_open.load()) return;
    g_open = open;
    RE::Main* main = RE::Main::GetSingleton();
    if (open) {
        g_frozeTime = !main->GetRuntimeData().freezeTime;
        main->GetRuntimeData().freezeTime = true;
    } else if (g_dirty) {
        Settings::Get().Save();
        g_dirty = false;
    }
    if (!open && g_frozeTime) {
        main->GetRuntimeData().freezeTime = false;
        g_frozeTime = false;
    }
}

ImGuiKey KeyFromScanCode(std::uint32_t sc) {
    switch (sc) {
        case 0x0E: return ImGuiKey_Backspace;
        case 0x0F: return ImGuiKey_Tab;
        case 0x1C: return ImGuiKey_Enter;
        case 0x1D: return ImGuiKey_LeftCtrl;
        case 0x2A: return ImGuiKey_LeftShift;
        case 0xC7: return ImGuiKey_Home;
        case 0xC8: return ImGuiKey_UpArrow;
        case 0xCB: return ImGuiKey_LeftArrow;
        case 0xCD: return ImGuiKey_RightArrow;
        case 0xCF: return ImGuiKey_End;
        case 0xD0: return ImGuiKey_DownArrow;
        case 0xD3: return ImGuiKey_Delete;
        default: return ImGuiKey_None;
    }
}

// Watches for the menu key / Esc and, while open, queues the events for ImGui.
void HandleInput(RE::InputEvent* const* a_events) {
    if (!a_events) return;
    RE::UI* ui = RE::UI::GetSingleton();
    const std::uint32_t menuKey = Settings::Get().menuKey;
    std::lock_guard lock(g_eventLock);
    for (RE::InputEvent* e = *a_events; e; e = e->next) {
        if (const RE::ButtonEvent* b = e->AsButtonEvent()) {
            const bool down = b->IsDown(), up = b->IsUp();
            if (!down && !up) continue;
            if (b->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                const std::uint32_t sc = b->GetIDCode();
                if (down && sc == menuKey && (g_open || !ui->GameIsPaused())) {
                    SetOpen(!g_open.load());
                    continue;
                }
                if (down && sc == 0x01 && g_open) {  // Esc
                    SetOpen(false);
                    continue;
                }
                if (g_open) g_events.push_back({Event::kKey, static_cast<int>(sc), 0, down});
            } else if (b->GetDevice() == RE::INPUT_DEVICE::kMouse && g_open) {
                const std::uint32_t id = b->GetIDCode();
                if (id == 8 || id == 9) {  // wheel up / down
                    if (down) g_events.push_back({Event::kWheel, id == 8 ? 1 : -1, 0, true});
                } else if (id < 3) {
                    g_events.push_back({Event::kMouseButton, static_cast<int>(id), 0, down});
                }
            }
        } else if (g_open && e->GetEventType() == RE::INPUT_EVENT_TYPE::kMouseMove) {
            const auto* m = static_cast<const RE::MouseMoveEvent*>(e);
            g_events.push_back({Event::kMouseMove, m->mouseInputX, m->mouseInputY, false});
        } else if (g_open && e->GetEventType() == RE::INPUT_EVENT_TYPE::kChar) {
            g_events.push_back({Event::kChar, static_cast<int>(static_cast<const RE::CharEvent*>(e)->keyCode), 0, true});
        }
    }
}

// BSInputDeviceManager::PollInputDevices sends the frame's input events to every sink through one
// call; while the menu is open it sends an empty list instead.
struct InputDispatch {
    static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent* const* a_events) {
        HandleInput(a_events);
        if (g_open) {
            static RE::InputEvent* const kNone[] = {nullptr};
            func(a_source, kNone);
        } else {
            func(a_source, a_events);
        }
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

void Render() {
    Settings& s = Settings::Get();
    bool changed = false;
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    bool open = true;
    ImGui::Begin("Skydive", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::SeparatorText("Mechanics");
    changed |= ImGui::Checkbox("Grappling hook", &s.grapple);
    changed |= ImGui::Checkbox("Parachute", &s.parachute);
    ImGui::TextDisabled("The skydive is always on.");

    ImGui::SeparatorText("Immersive stamina");
    changed |= ImGui::Checkbox("Grapple and hanging cost stamina", &s.stamina);
    ImGui::BeginDisabled(!s.stamina);
    ImGui::PushItemWidth(260.0f);
    changed |= ImGui::SliderFloat("Per grapple shot", &s.grappleCost, 0.0f, 100.0f, "%.0f");
    changed |= ImGui::SliderFloat("Per second on a wall or ceiling", &s.hangCost, 0.0f, 20.0f, "%.1f");
    ImGui::TextDisabled("Below the shot's cost the grapple won't fire; at 0 you let go of walls.");
    changed |= ImGui::Checkbox("Steering the parachute costs stamina", &s.chuteStamina);
    ImGui::BeginDisabled(!s.chuteStamina);
    changed |= ImGui::SliderFloat("Per second of full steering", &s.chuteCost, 0.0f, 30.0f, "%.1f");
    changed |= ImGui::SliderFloat("Steering when exhausted", &s.exhaustedSteer, 0.0f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    ImGui::PopItemWidth();
    ImGui::EndDisabled();

    ImGui::SeparatorText("Grapple");
    ImGui::PushItemWidth(260.0f);
    changed |= ImGui::SliderFloat("Heaviest object pulled in", &s.pullMaxMass, 0.0f, 2000.0f, "%.0f kg");
    ImGui::PopItemWidth();
    ImGui::TextDisabled("Hold the grapple key, let go aimed elsewhere: pulls the two together.");

    ImGui::SeparatorText("Ragdoll");
    changed |= ImGui::Checkbox("Skydive into the ground ragdolls you", &s.landingRagdoll);
    ImGui::TextDisabled("Off: only when the fall kills you.");
    ImGui::PushItemWidth(260.0f);
    changed |= ImGui::SliderFloat("Get up speed", &s.getUpSpeed, 1.0f, 8.0f, "%.1fx");
    ImGui::PopItemWidth();

    ImGui::SeparatorText("Sound");
    ImGui::PushItemWidth(260.0f);
    changed |= ImGui::SliderFloat("Volume", &s.soundVolume, 0.0f, 2.0f, "%.2f");
    ImGui::PopItemWidth();
    ImGui::TextDisabled("On top of Skyrim's Effects volume.");

    ImGui::Separator();
    ImGui::TextDisabled("Ctrl+click a slider to type a value. Esc closes.");
    ImGui::End();
    // Saved once a change is done (a slider let go), not on every frame of a drag.
    g_dirty |= changed;
    if (g_dirty && !ImGui::IsAnyItemActive()) {
        s.Save();
        g_dirty = false;
    }
    if (!open) SetOpen(false);
}

void Draw(IDXGISwapChain* swapChain) {
    const bool open = g_open.load();
    std::vector<Event> events;
    {
        std::lock_guard lock(g_eventLock);
        events.swap(g_events);
    }
    if (!open) return;

    ImGuiIO& io = ImGui::GetIO();
    DXGI_SWAP_CHAIN_DESC desc{};
    swapChain->GetDesc(&desc);
    const ImVec2 size(static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height));
    if (g_mouseX < 0.0f) {
        g_mouseX = size.x * 0.5f;
        g_mouseY = size.y * 0.5f;
    }
    for (const Event& e : events) {
        switch (e.type) {
            case Event::kMouseMove:
                g_mouseX = std::clamp(g_mouseX + static_cast<float>(e.a), 0.0f, size.x);
                g_mouseY = std::clamp(g_mouseY + static_cast<float>(e.b), 0.0f, size.y);
                io.AddMousePosEvent(g_mouseX, g_mouseY);
                break;
            case Event::kMouseButton: io.AddMouseButtonEvent(e.a, e.down); break;
            case Event::kWheel: io.AddMouseWheelEvent(0.0f, static_cast<float>(e.a)); break;
            case Event::kKey:
                if (const ImGuiKey k = KeyFromScanCode(static_cast<std::uint32_t>(e.a)); k != ImGuiKey_None) {
                    io.AddKeyEvent(k, e.down);
                    if (k == ImGuiKey_LeftCtrl) io.AddKeyEvent(ImGuiMod_Ctrl, e.down);
                    if (k == ImGuiKey_LeftShift) io.AddKeyEvent(ImGuiMod_Shift, e.down);
                }
                break;
            case Event::kChar: io.AddInputCharacter(static_cast<unsigned int>(e.a)); break;
        }
    }
    io.AddMousePosEvent(g_mouseX, g_mouseY);

    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::clamp(std::chrono::duration<float>(now - last).count(), 1e-4f, 0.1f);
    last = now;
    io.DisplaySize = size;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    Render();
    ImGui::Render();

    // Onto the back buffer, restoring the game's render targets after.
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return;
    ID3D11RenderTargetView* rtv = nullptr;
    g_device->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    if (!rtv) return;
    ID3D11RenderTargetView* oldRtv = nullptr;
    ID3D11DepthStencilView* oldDsv = nullptr;
    g_context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    g_context->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &oldRtv, oldDsv);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
    rtv->Release();
}

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
PresentFn g_present = nullptr;

HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    if (g_ready) Draw(swapChain);
    return g_present(swapChain, syncInterval, flags);
}

}  // namespace

void Install() {
    // The input hook: the call to BSTEventSource<InputEvent*>::SendEvent in PollInputDevices.
    // Checked to be a call before patching (a game update could move it): without it the menu
    // still opens, but the game keeps its input.
    REL::Relocation<std::uintptr_t> target{RELOCATION_ID(67315, 68617), 0x7B};
    if (*reinterpret_cast<const std::uint8_t*>(target.address()) == 0xE8) {
        InputDispatch::func = SKSE::GetTrampoline().write_call<5>(target.address(), InputDispatch::thunk);
        g_inputHooked = true;
    } else {
        SKSE::log::warn("menu: input dispatch call not found; the menu can't take the game's input");
    }
}

void Init() {
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    if (!renderer || g_ready) return;
    auto& data = renderer->GetRuntimeData();
    auto* swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
    g_device = reinterpret_cast<ID3D11Device*>(data.forwarder);
    g_context = reinterpret_cast<ID3D11DeviceContext*>(data.context);
    if (!swapChain || !g_device || !g_context || !g_inputHooked) {
        SKSE::log::warn("menu: no swap chain or input hook, no options menu (use Skydive.ini)");
        return;
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.MouseDrawCursor = true;
    io.FontGlobalScale = 1.4f;
    ImGui::StyleColorsDark();
    ImGui_ImplDX11_Init(g_device, g_context);

    // Present is slot 8 of IDXGISwapChain's vtable.
    void** vtbl = *reinterpret_cast<void***>(swapChain);
    DWORD old = 0;
    VirtualProtect(&vtbl[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    g_present = reinterpret_cast<PresentFn>(vtbl[8]);
    vtbl[8] = reinterpret_cast<void*>(&Present);
    VirtualProtect(&vtbl[8], sizeof(void*), old, &old);
    g_ready = true;
    SKSE::log::info("options menu ready on key {:#x}", Settings::Get().menuKey);
}

bool IsOpen() { return g_open.load(); }

}  // namespace menu
