#pragma once

#include "Graphics/D2DCommon.h"
#include "Graphics/Direct2DRenderer.h"
#include "Core/Models.h"
#include "Core/ConfigStore.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace CodexQuotaBar {

    // 跨线程消息：工作线程通过 PostMessage 投递，全部由 UI 线程消费
    constexpr UINT WM_CQB_REFRESH = WM_APP + 1;      // wParam=0
    constexpr UINT WM_CQB_STATS = WM_APP + 3;        // 数据从线程安全待处理槽读取
    constexpr UINT WM_CQB_SHOW = WM_APP + 4;
    constexpr UINT WM_CQB_HIDE = WM_APP + 5;
    constexpr UINT WM_CQB_TOGGLE = WM_APP + 6;
    constexpr UINT WM_CQB_QUOTA_RESULT = WM_APP + 7; // 数据从共享抓取状态读取
    constexpr UINT WM_CQB_COMPANION_RESULT = WM_APP + 8;

    class MainWindow {
    public:
        MainWindow();
        ~MainWindow();

        bool Create(bool forceInitialRefresh = false);
        void Show();
        void BeginShutdown();
        HWND GetHwnd() const { return m_hwnd.load(); }
        bool PostStats(TokenStats stats);

    private:
        static LRESULT CALLBACK WndProcSetup(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

        void OnPaint();
        void OnDpiChanged(int newDpi, const RECT* suggestedRect);
        void ApplyDpiScale(int dpi);
        void ApplyUiScale();
        void ApplyUserScale(int level);
        void ApplyRefreshInterval(int level);
        void ApplyAppearance(const std::wstring& validationErrors = L"");
        void SetAppearanceMode(AppearanceMode mode);
        void ConfigureBackgroundTransparency();
        void ReloadAppearance();
        void OpenConfigFile();
        bool SaveSettingsWithFeedback(bool preserveDiskAppearance = true);
        void Hide();
        void ToggleVisible();
        void ToggleExpanded();
        void RefreshQuota();
        void SetStats(const TokenStats& stats);
        void ToggleCompanionMode();
        void PollCompanionMode();
        void HandleCompanionResult(bool running);
        void OnWindowMoved();
        void HandleContextMenu(int screenX, int screenY);
        bool IsHeaderArea(POINT pt) const;
        bool IsExpandButtonArea(POINT pt) const;
        POINT ClampToScreens(POINT pt, SIZE sz) const;

        struct FetchResultState {
            std::mutex mutex;
            std::optional<QuotaSnapshot> pending;
            std::atomic<bool> canceled{ false };
            std::atomic<bool> inFlight{ false };
        };

        struct CompanionPollState {
            std::atomic<bool> canceled{ false };
            std::atomic<bool> inFlight{ false };
        };

        std::atomic<HWND> m_hwnd{ NULL };
        float m_dpiScale = 1.0f;  // 系统 DPI 系数（96 基准）
        float m_uiScale = 1.0f;   // 总缩放 = dpiScale × 用户档位（几何/字体/命中统一使用）
        int m_userScaleLevel = 2; // 用户缩放档位索引（0.75/0.875/1.0/1.125/1.25）
        int m_refreshIntervalLevel = 0; // 自动刷新档位索引（1/5/10/30/60 分钟）
        int m_currentDpi = 96;
        AppSettings m_settings;
        bool m_companionMode = false;
        bool m_codexDesktopRunning = false;
        int m_codexMissingPolls = 0;

        bool m_expanded = false;
        POINT m_collapsedLocation = { 0, 0 };
        bool m_hasCollapsedLocation = false;

        // 退出与抓取并发保护
        std::atomic<bool> m_shuttingDown{ false };
        // 在途抓取期间收到的新刷新请求合并为一次补抓，本轮结束后自动再抓一轮
        std::atomic<bool> m_refreshQueued{ false };
        std::thread m_fetchThread;
        std::shared_ptr<FetchResultState> m_fetchState = std::make_shared<FetchResultState>();
        std::mutex m_statsMutex;
        std::optional<TokenStats> m_pendingStats;
        std::shared_ptr<CompanionPollState> m_companionPollState =
            std::make_shared<CompanionPollState>();

        // 手动拖拽状态（取代 HTCAPTION 模态循环，保留双击刷新）
        bool m_dragging = false;
        POINT m_dragOrigin = { 0, 0 };
        POINT m_dragClickPoint = { 0, 0 };
        POINT m_dragWindowPos = { 0, 0 };

        QuotaSnapshot m_snapshot;
        SyncState m_syncState = SyncState::Waiting;

        std::unique_ptr<Direct2DRenderer> m_renderer;
    };

} // namespace CodexQuotaBar
