#include "Workspace.hpp"
#include "../Compositor.hpp"
#include "../config/ConfigValue.hpp"

#include <hyprutils/string/String.hpp>
using namespace Hyprutils::String;

PHLWORKSPACE CWorkspace::create(WORKSPACEID id, MONITORID monitorID, std::string name, bool special, bool isEmtpy) {
    PHLWORKSPACE workspace = makeShared<CWorkspace>(id, monitorID, name, special, isEmtpy);
    workspace->init(workspace);
    return workspace;
}

CWorkspace::CWorkspace(WORKSPACEID id, MONITORID monitorID, std::string name, bool special, bool isEmtpy) {
    m_iMonitorID          = monitorID;
    m_iID                 = id;
    m_szName              = name;
    m_bIsSpecialWorkspace = special;
    m_bWasCreatedEmtpy    = isEmtpy;
}

void CWorkspace::init(PHLWORKSPACE self) {
    m_pSelf = self;

    m_vRenderOffset.create(m_bIsSpecialWorkspace ? g_pConfigManager->getAnimationPropertyConfig("specialWorkspaceIn") :
                                                   g_pConfigManager->getAnimationPropertyConfig("workspacesIn"),
                           self, AVARDAMAGE_ENTIRE);
    m_fAlpha.create(AVARTYPE_FLOAT,
                    m_bIsSpecialWorkspace ? g_pConfigManager->getAnimationPropertyConfig("fadeSpecialWorkspaceIn") :
                                            g_pConfigManager->getAnimationPropertyConfig("fadeWorkspacesIn"),
                    self, AVARDAMAGE_ENTIRE);
    m_fAlpha.setValueAndWarp(1.f);
    m_fScaleClients.create(
        AVARTYPE_FLOAT, m_bIsSpecialWorkspace ? g_pConfigManager->getAnimationPropertyConfig("specialWorkspaceIn") : g_pConfigManager->getAnimationPropertyConfig("workspacesIn"),
        self, AVARDAMAGE_ENTIRE);
    m_fScaleClients.setValueAndWarp(1.f);

    m_vRenderOffset.registerVar();
    m_fAlpha.registerVar();
    m_fScaleClients.registerVar();

    const auto RULEFORTHIS = g_pConfigManager->getWorkspaceRuleFor(self);
    if (RULEFORTHIS.defaultName.has_value())
        m_szName = RULEFORTHIS.defaultName.value();

    m_pFocusedWindowHook = g_pHookSystem->hookDynamic("closeWindow", [this](void* self, SCallbackInfo& info, std::any param) {
        const auto PWINDOW = std::any_cast<PHLWINDOW>(param);

        if (PWINDOW == m_pLastFocusedWindow.lock())
            m_pLastFocusedWindow.reset();
    });

    m_bInert = false;

    const auto WORKSPACERULE = g_pConfigManager->getWorkspaceRuleFor(self);
    m_bPersistent            = WORKSPACERULE.isPersistent;

    if (self->m_bWasCreatedEmtpy)
        if (auto cmd = WORKSPACERULE.onCreatedEmptyRunCmd)
            g_pKeybindManager->spawn(*cmd);

    g_pEventManager->postEvent({"createworkspace", m_szName});
    g_pEventManager->postEvent({"createworkspacev2", std::format("{},{}", m_iID, m_szName)});
    EMIT_HOOK_EVENT("createWorkspace", this);
}

SWorkspaceIDName CWorkspace::getPrevWorkspaceIDName(bool perMonitor) const {
    if (perMonitor)
        return m_sPrevWorkspacePerMonitor;

    return m_sPrevWorkspace;
}

CWorkspace::~CWorkspace() {
    m_vRenderOffset.unregister();

    Debug::log(LOG, "Destroying workspace ID {}", m_iID);

    // check if g_pHookSystem and g_pEventManager exist, they might be destroyed as in when the compositor is closing.
    if (g_pHookSystem)
        g_pHookSystem->unhook(m_pFocusedWindowHook);

    if (g_pEventManager) {
        g_pEventManager->postEvent({"destroyworkspace", m_szName});
        g_pEventManager->postEvent({"destroyworkspacev2", std::format("{},{}", m_iID, m_szName)});
        EMIT_HOOK_EVENT("destroyWorkspace", this);
    }
}

void CWorkspace::startAnim(bool in, bool left, bool instant) {
    if (!instant) {
        const std::string ANIMNAME  = std::format("{}{}", m_bIsSpecialWorkspace ? "specialWorkspace" : "workspaces", in ? "In" : "Out");
        const std::string ANIMNAME2 = std::format("fade{}{}", m_bIsSpecialWorkspace ? "SpecialWorkspace" : "Workspaces", in ? "In" : "Out");

        m_fAlpha.m_pConfig        = g_pConfigManager->getAnimationPropertyConfig(ANIMNAME2);
        m_vRenderOffset.m_pConfig = g_pConfigManager->getAnimationPropertyConfig(ANIMNAME);
        m_fScaleClients.m_pConfig = g_pConfigManager->getAnimationPropertyConfig(ANIMNAME);
    }

    const auto  ANIMSTYLE     = m_vRenderOffset.m_pConfig->pValues->internalStyle;
    static auto PWORKSPACEGAP = CConfigValue<Hyprlang::INT>("general:gaps_workspaces");

    // set floating windows offset callbacks
    m_vRenderOffset.setUpdateCallback([&](void*) {
        for (auto& w : g_pCompositor->m_vWindows) {
            if (!validMapped(w) || w->workspaceID() != m_iID)
                continue;

            w->onWorkspaceAnimUpdate();
        };
    });

    m_fAlpha.setValueAndWarp(in ? 0.F : 1.F);
    m_fAlpha = in ? 1.F : 0.F;

    // handle animation styles for the movement one
    if (m_vRenderOffset.m_pConfig->pValues->internalEnabled) {
        if (ANIMSTYLE.starts_with("slide") && ANIMSTYLE.contains("%")) {
            const auto PMONITOR = g_pCompositor->getMonitorFromID(m_iMonitorID);
            float      movePerc = 0.F;

            try {
                auto percstr = ANIMSTYLE.substr(ANIMSTYLE.find_last_of(' ') + 1);
                movePerc     = std::stoi(percstr.substr(0, percstr.length() - 1));
            } catch (std::exception& e) { Debug::log(ERR, "Error in startAnim: invalid percentage"); }

            m_vRenderOffset.setValueAndWarp(Vector2D(0, 0));
            m_fScaleClients.setValueAndWarp(1.F);

            if (ANIMSTYLE.starts_with("slidevert")) {
                if (in) {
                    m_vRenderOffset.setValueAndWarp(Vector2D(0.0, (left ? PMONITOR->vecSize.y : -PMONITOR->vecSize.y) * (movePerc / 100.f)));
                    m_vRenderOffset = Vector2D(0, 0);
                } else
                    m_vRenderOffset = Vector2D(0.0, (left ? -PMONITOR->vecSize.y : PMONITOR->vecSize.y) * (movePerc / 100.f));

            } else {
                if (in) {
                    m_vRenderOffset.setValueAndWarp(Vector2D((left ? PMONITOR->vecSize.x : -PMONITOR->vecSize.x) * (movePerc / 100.f), 0.0));
                    m_vRenderOffset = Vector2D(0, 0);
                } else
                    m_vRenderOffset = Vector2D((left ? -PMONITOR->vecSize.x : PMONITOR->vecSize.x) * (movePerc / 100.f), 0.0);
            }
        } else if (ANIMSTYLE.starts_with("popin")) {
            m_vRenderOffset.setValueAndWarp(Vector2D(0, 0));

            float startPerc = 0.F;

            try {
                auto percstr = ANIMSTYLE.substr(ANIMSTYLE.find_last_of(' ') + 1);
                startPerc    = std::stoi(percstr.substr(0, percstr.length() - 1));
            } catch (std::exception& e) { Debug::log(ERR, "Error in startAnim: invalid percentage"); }

            if (in) {
                m_fScaleClients.setValueAndWarp(startPerc / 100.F);
                m_fScaleClients = 1.F;
            } else {
                m_fScaleClients.setValueAndWarp(1.F);
                m_fScaleClients = startPerc / 100.F;
            }
        } else {
            // fallback is slide
            const auto PMONITOR  = g_pCompositor->getMonitorFromID(m_iMonitorID);
            const auto XDISTANCE = PMONITOR->vecSize.x + *PWORKSPACEGAP;

            m_fScaleClients.setValueAndWarp(1.F);

            if (in) {
                m_vRenderOffset.setValueAndWarp(Vector2D(left ? XDISTANCE : -XDISTANCE, 0.0));
                m_vRenderOffset = Vector2D(0, 0);
            } else
                m_vRenderOffset = Vector2D(left ? -XDISTANCE : XDISTANCE, 0.0);
        }
    }

    if (instant) {
        m_vRenderOffset.warp();
        m_fAlpha.warp();
        m_fScaleClients.warp();
    }
}

void CWorkspace::setActive(bool on) {
    ; // empty until https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/40
}

void CWorkspace::moveToMonitor(const MONITORID& id) {
    ; // empty until https://gitlab.freedesktop.org/wayland/wayland-protocols/-/merge_requests/40
}

PHLWINDOW CWorkspace::getLastFocusedWindow() {
    if (!validMapped(m_pLastFocusedWindow) || m_pLastFocusedWindow->workspaceID() != m_iID)
        return nullptr;

    return m_pLastFocusedWindow.lock();
}

void CWorkspace::rememberPrevWorkspace(const PHLWORKSPACE& prev) {
    if (!prev) {
        m_sPrevWorkspace.id   = -1;
        m_sPrevWorkspace.name = "";
        return;
    }

    if (prev->m_iID == m_iID) {
        Debug::log(LOG, "Tried to set prev workspace to the same as current one");
        return;
    }

    m_sPrevWorkspace.id   = prev->m_iID;
    m_sPrevWorkspace.name = prev->m_szName;

    if (prev->m_iMonitorID == m_iMonitorID) {
        m_sPrevWorkspacePerMonitor.id   = prev->m_iID;
        m_sPrevWorkspacePerMonitor.name = prev->m_szName;
    }
}

std::string CWorkspace::getConfigName() {
    if (m_bIsSpecialWorkspace) {
        return m_szName;
    }

    if (m_iID > 0)
        return std::to_string(m_iID);

    return "name:" + m_szName;
}

bool CWorkspace::matchesStaticSelector(const std::string& selector_) {
    auto selector = trim(selector_);

    if (selector.empty())
        return true;

    if (isNumber(selector)) {
        const auto& [wsid, wsname] = getWorkspaceIDNameFromString(selector);

        if (wsid == WORKSPACE_INVALID)
            return false;

        return wsid == m_iID;

    } else if (selector.starts_with("name:")) {
        return m_szName == selector.substr(5);
    } else if (selector.starts_with("special")) {
        return m_szName == selector;
    } else {
        // parse selector

        for (size_t i = 0; i < selector.length(); ++i) {
            const char& cur = selector[i];
            if (std::isspace(cur))
                continue;

            // Allowed selectors:
            // r - range: r[1-5]
            // s - special: s[true]
            // n - named: n[true] or n[s:string] or n[e:string]
            // m - monitor: m[monitor_selector]
            // w - windowCount: w[1-4] or w[1], optional flag t or f for tiled or floating and
            //                  flag g to count groups instead of windows, e.g. w[t1-2], w[fg4]
            //                  flag v will count only visible windows
            // f - fullscreen state : f[-1], f[0], f[1], or f[2] for different fullscreen states
            //                        -1: no fullscreen, 0: fullscreen, 1: maximized, 2: fullscreen without sending fs state to window

            const auto  NEXTSPACE = selector.find_first_of(' ', i);
            std::string prop      = selector.substr(i, NEXTSPACE == std::string::npos ? std::string::npos : NEXTSPACE - i);
            i                     = std::min(NEXTSPACE, std::string::npos - 1);

            if (cur == 'r') {
                WORKSPACEID from = 0, to = 0;
                if (!prop.starts_with("r[") || !prop.ends_with("]")) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                prop = prop.substr(2, prop.length() - 3);

                if (!prop.contains("-")) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                const auto DASHPOS = prop.find("-");
                const auto LHS = prop.substr(0, DASHPOS), RHS = prop.substr(DASHPOS + 1);

                if (!isNumber(LHS) || !isNumber(RHS)) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                try {
                    from = std::stoll(LHS);
                    to   = std::stoll(RHS);
                } catch (std::exception& e) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                if (to < from || to < 1 || from < 1) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                if (std::clamp(m_iID, from, to) != m_iID)
                    return false;
                continue;
            }

            if (cur == 's') {
                if (!prop.starts_with("s[") || !prop.ends_with("]")) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                prop = prop.substr(2, prop.length() - 3);

                const auto SHOULDBESPECIAL = configStringToInt(prop);

                if ((bool)SHOULDBESPECIAL != m_bIsSpecialWorkspace)
                    return false;
                continue;
            }

            if (cur == 'm') {
                if (!prop.starts_with("m[") || !prop.ends_with("]")) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                prop = prop.substr(2, prop.length() - 3);

                const auto PMONITOR = g_pCompositor->getMonitorFromString(prop);

                if (!(PMONITOR ? PMONITOR->ID == m_iMonitorID : false))
                    return false;
                continue;
            }

            if (cur == 'n') {
                if (!prop.starts_with("n[") || !prop.ends_with("]")) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                prop = prop.substr(2, prop.length() - 3);

                if (prop.starts_with("s:") && !m_szName.starts_with(prop.substr(2)))
                    return false;
                if (prop.starts_with("e:") && !m_szName.ends_with(prop.substr(2)))
                    return false;

                const auto WANTSNAMED = configStringToInt(prop);

                if (WANTSNAMED != (m_iID <= -1337))
                    return false;
                continue;
            }

            if (cur == 'w') {
                WORKSPACEID from = 0, to = 0;
                if (!prop.starts_with("w[") || !prop.ends_with("]")) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                prop = prop.substr(2, prop.length() - 3);

                int  wantsOnlyTiled    = -1;
                bool wantsCountGroup   = false;
                bool wantsCountVisible = false;

                int  flagCount = 0;
                for (auto& flag : prop) {
                    if (flag == 't' && wantsOnlyTiled == -1) {
                        wantsOnlyTiled = 1;
                        flagCount++;
                    } else if (flag == 'f' && wantsOnlyTiled == -1) {
                        wantsOnlyTiled = 0;
                        flagCount++;
                    } else if (flag == 'g' && !wantsCountGroup) {
                        wantsCountGroup = true;
                        flagCount++;
                    } else if (flag == 'v' && !wantsCountVisible) {
                        wantsCountVisible = true;
                        flagCount++;
                    } else {
                        break;
                    }
                }
                prop = prop.substr(flagCount);

                if (!prop.contains("-")) {
                    // try single

                    if (!isNumber(prop)) {
                        Debug::log(LOG, "Invalid selector {}", selector);
                        return false;
                    }

                    try {
                        from = std::stoll(prop);
                    } catch (std::exception& e) {
                        Debug::log(LOG, "Invalid selector {}", selector);
                        return false;
                    }

                    int count;
                    if (wantsCountGroup)
                        count = g_pCompositor->getGroupsOnWorkspace(m_iID, wantsOnlyTiled == -1 ? std::nullopt : std::optional<bool>((bool)wantsOnlyTiled),
                                                                    wantsCountVisible ? std::optional<bool>(wantsCountVisible) : std::nullopt);
                    else
                        count = g_pCompositor->getWindowsOnWorkspace(m_iID, wantsOnlyTiled == -1 ? std::nullopt : std::optional<bool>((bool)wantsOnlyTiled),
                                                                     wantsCountVisible ? std::optional<bool>(wantsCountVisible) : std::nullopt);

                    if (count != from)
                        return false;
                    continue;
                }

                const auto DASHPOS = prop.find("-");
                const auto LHS = prop.substr(0, DASHPOS), RHS = prop.substr(DASHPOS + 1);

                if (!isNumber(LHS) || !isNumber(RHS)) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                try {
                    from = std::stoll(LHS);
                    to   = std::stoll(RHS);
                } catch (std::exception& e) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                if (to < from || to < 1 || from < 1) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                WORKSPACEID count;
                if (wantsCountGroup)
                    count = g_pCompositor->getGroupsOnWorkspace(m_iID, wantsOnlyTiled == -1 ? std::nullopt : std::optional<bool>((bool)wantsOnlyTiled),
                                                                wantsCountVisible ? std::optional<bool>(wantsCountVisible) : std::nullopt);
                else
                    count = g_pCompositor->getWindowsOnWorkspace(m_iID, wantsOnlyTiled == -1 ? std::nullopt : std::optional<bool>((bool)wantsOnlyTiled),
                                                                 wantsCountVisible ? std::optional<bool>(wantsCountVisible) : std::nullopt);

                if (std::clamp(count, from, to) != count)
                    return false;
                continue;
            }

            if (cur == 'f') {
                if (!prop.starts_with("f[") || !prop.ends_with("]")) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                prop        = prop.substr(2, prop.length() - 3);
                int FSSTATE = -1;
                try {
                    FSSTATE = std::stoi(prop);
                } catch (std::exception& e) {
                    Debug::log(LOG, "Invalid selector {}", selector);
                    return false;
                }

                switch (FSSTATE) {
                    case -1: // no fullscreen
                        if (m_bHasFullscreenWindow)
                            return false;
                        break;
                    case 0: // fullscreen full
                        if (!m_bHasFullscreenWindow || m_efFullscreenMode != FSMODE_FULLSCREEN)
                            return false;
                        break;
                    case 1: // maximized
                        if (!m_bHasFullscreenWindow || m_efFullscreenMode != FSMODE_MAXIMIZED)
                            return false;
                        break;
                    default: break;
                }
                continue;
            }

            Debug::log(LOG, "Invalid selector {}", selector);
            return false;
        }

        return true;
    }

    UNREACHABLE();
    return false;
}

void CWorkspace::markInert() {
    m_bInert     = true;
    m_iID        = WORKSPACE_INVALID;
    m_iMonitorID = MONITOR_INVALID;
    m_bVisible   = false;
}

bool CWorkspace::inert() {
    return m_bInert;
}
