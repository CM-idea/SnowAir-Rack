#pragma once

#include "Common.h"
#include "ContextMenu.h"
#include "LayeredWindow.h"
#include "PreviewWindow.h"
#include "RackView.h"

class Plugin;

class RackWindow : public LayeredWindow {
public:
    RackWindow();
    ~RackWindow() override;

    bool Init(Plugin* plugin, Graphics* graphics);
    void Shutdown();

    /* addedId：新增卡片的 id（自定义卡排在组后面，不能再用 items.back()）。 */
    void OnItemsChanged(bool added, int addedId = -1);
    void OnConfigChanged();
    void OnThemeChanged();
    void RenderNow();

    void Expand(bool peek);
    void ToggleVisibility();
    void Collapse();
    /* 浣挎煇寮犲崱鐗囪交寰檭鍔紝鐢ㄤ簬鎻愮ず銆屽凡缁忔寕鍦ㄨ繖鍎夸簡銆嶃€?*/
    void ShakeCard(int index);
    /* 浠庡乏寰€鍙虫鏌ユ渶灏忛棿璺濓紝鎸ㄥお杩戝氨鐢ㄧ紦鍔ㄦ帹寮€銆?*/
    void NormalizeSpacing(float ms);
    /* 拖到两张卡中间松手：吸附组按落点左右分开、向外让开（缓动到目标偏移）。 */
    void PushGroupAside(int skipId, int splitIndex, float obstacleLeft, float obstacleRight);
    /* 还在缓动里的偏移按目标值算：判断自定义卡会不会压住邻居时用。 */
    float PendingGroupOffset(int id, float fallback) const;
    /* 绳子上一张自定义卡都没有时，把吸附组为让位产生的偏移收回槽位。 */
    void RelaxGroupOffsetsIfNoCustoms();
    /* 取消挂起的偏移缓动前先置于终点，避免卡片停在中间位置。 */
    void SettleOffsetAnims();
    /* 自定义卡和别的卡重叠时，自动并回吸附组的最近槽位。
       ignoreId：本轮跳过刚放下的卡片（它要停在新位置）。 */
    void ResolveCustomOverlaps(int ignoreId = -1);
    /* 鎷栨嫿鏈熼棿璁╃怀瀛愬榧犳爣閫忔槑锛歄LE 姣忔绉诲姩閮借鍋氬懡涓祴璇曪紝
       涓€鍧楀叏瀹藉垎灞傜獥鍙ｆ尅鍦ㄦ寚閽堜笅闈細鏄庢樉鎷栨參鎷栨嫿銆?*/
    void SetDragTransparent(bool on);
    /* 灞曞紑鏈熼棿鐩戝惉灞忓箷椤惰竟鐐瑰嚮鍜屽崱鐗囧彸閿€?*/
    void SetTopBarWatch(bool enabled);
    bool IsTopBarPoint(POINT screen) const;
    int CardIndexAtScreen(POINT screen) const;
    /* 灞忓箷鐐规槸鍚﹁惤鍦ㄤ俊灏佹寜閽笂锛氶潰鏉垮鐐瑰叧闂鎺掗櫎杩欏潡锛岃鐐逛俊灏佸彧璧板紑鍏抽摼璺€?*/
    bool IsEnvelopeScreenPoint(POINT screen) const;
    static LRESULT CALLBACK TopBarHookProc(int code, WPARAM wParam, LPARAM lParam);
    static RackWindow* topBarOwner_;
    bool rightCardDown_ = false;
    uint64_t rightCardDownAt_ = 0;
    bool startupResolveDone_ = false;
    HHOOK topBarHook_ = nullptr;
    bool expanded() const { return expand_ > 0.5f; }

    void Reposition();
    void DismissPreview();
    void RefreshPreview();

    HMONITOR monitor() const { return monitor_; }
    const RECT& workArea() const { return workArea_; }
    float scale() const { return scale_; }

protected:
    void BeforeDraw(ID2D1RenderTarget* target) override;
    void Paint(ID2D1RenderTarget* target) override;
    void AfterDraw(HDC dc, void* bits, int width, int height) override;
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) override;

private:
    enum TimerId {
        kTimerPoll = 1,
        kTimerAnim,
        kTimerClick,
        kTimerEditStart,
    kTimerCopyToast,
    kTimerFakeCursor,
    };

    enum MenuId {
        kMenuCopy = 1,
        kMenuOpen,
        kMenuImageEdit,
        kMenuEdit,
        kMenuEditColor,
        kMenuSaveAs,
        kMenuShowInFolder,
        kMenuNote,
        kMenuRemove,
        kMenuSettings,
        kMenuCollapse,
        kMenuBatch,
        kMenuBatchLeft,
        kMenuBatchRight,
        kMenuBatchKeep,
        kMenuBatchAll,
        kMenuRecycle,
        kMenuColorBase = 100,
    };

    void Tick();
    void Animate();
    void EnsureVisible();
    /* 璇昏交闆殑銆屽仠鐢ㄥ巻鍙茶褰曘€嶏紝鍙樹簡灏遍噸鏂板竷灞€骞堕噸缁樸€?*/
    void RefreshHistoryDisabled();
    void PositionForMonitor(HMONITOR monitor);
    void StartAnimation(float target);
    void UpdateHover();
    void UpdatePreview();
    void UpdateAccessoryHover(const RackHit& hit);
    void UpdateClickThrough();
    /* 鎸夊綋鍓嶆樉绀哄櫒瀹归噺锛屾妸銆岄殢灞忓箷鑰屽畾銆嶇殑涓婇檺鍚屾缁欐彃浠躲€?*/
    void UpdateMaxTotal();
    /* 榧犳爣绉诲姩鏃跺嵆鏃舵洿鏂?hover锛堣妭娴侊級锛屼笉鍐嶇瓑 50ms 杞銆?*/
    void UpdateHoverFromMouse();
    void SetClickThrough(bool enabled);
    /* 鍗＄墖/渚跨鏉＄殑绮剧‘鍛戒腑褰㈢姸锛堝惈鏃嬭浆涓庢偓鍋滄斁澶э級锛宔xtra 鏄鎵╁儚绱犮€?*/
    bool PointInCardShape(const CardBox& box, float x, float y, float extra) const;
    bool PointInNoteShape(const CardBox& box, float x, float y, float extra) const;
    /* 鎸囬拡涓嬭繖涓儚绱犳槸鍚︾湡鐨勭敾浜嗕笢瑗裤€傚垎灞傜獥鍙ｅ彧鐪?alpha锛?
       浣庣骇榧犳爣閽╁瓙鐢ㄥ畠鎶婄┖鐧藉尯鍩熻缁欎笅闈㈢殑绋嬪簭銆?*/
    bool PointHittable(POINT screen) const;
    /* 鍥剧墖/鏂囦欢琚垹闄ゅ悗锛屾妸缁冲瓙涓婄殑鍗＄墖鑷姩鍙栦笅銆?*/
    void CheckMissingFiles();

    RackHit HitAtCursor() const;
    int HitIndexAtCursor(RackPart* outPart = nullptr) const;
    /* 淇″皝鏄紑鍏虫寜閽€傞紶鏍囪繛鐐瑰お蹇椂锛岀浜屾鎸変笅鍦?Windows 閲屼笉鏄?
       WM_LBUTTONDOWN 鑰屾槸 WM_LBUTTONDBLCLK锛屼絾瀹冩槸鍚屼竴涓墿鐞嗗姩浣滐紱
       涓ょ娑堟伅閮戒粠杩欓噷璧帮紝淇濊瘉涓€娆℃寜涓嬪彧鍒囨崲涓€娆°€?*/
    void ToggleClipboardFromEnvelope();
    bool CursorOverLine() const;
    bool CursorOverContent() const;
    bool PointOverPreview(const POINT& screen) const;

    void ShowPreviewFor(int index);
    void HidePreviewNow();
    void CopyItem(int index);
    void OpenItem(int index);
    /* 閾炬帴姝ｆ枃鍗曞嚮锛氭墦寮€榛樿娴忚鍣紙鍙屽嚮涓嶉噸澶嶅紑锛夈€?*/
    void OpenCardLink(int index);
    /* 棰滆壊鍗″崟涓壊鍧?hex/rgb锛氬彧澶嶅埗杩欎釜棰滆壊銆俻art 0=鑹插潡 1=hex 2=rgb銆?*/
    void CopyCardColor(int index, int colorIndex, int part);
    /* 棰滆壊鍗℃粴杞細杩斿洖 true 琛ㄧず杩欐婊氳疆宸茶鑹插崱鍚冩帀銆?*/
    bool ScrollColorCard(int index, int delta);
    /* 鍙抽敭銆岀紪杈戦鑹层€嶏細鏀硅繖寮犲崱鐨勫ご閮ㄥ己璋冭壊锛堜究绛炬槸鏁村紶绾哥殑涓昏壊锛夈€?*/
    void PickCardColor(int index);
    void SaveItemAs(int index);
    void RemoveItem(int index, bool toBasket = true);
    void EditNote(int index);
    void QueueEditNote(int index);
    void SetNoteColor(int index, int color);
    void ShowItemMenu(int index, POINT screen);
    void ShowBatchMenu(int index, POINT screen);
    static void MenuSelected(void* user, int id);
    void ShowGeneralMenu(POINT screen);
    void HandleMenuCommand(int index, int command);

    bool IsFullscreenOn(HMONITOR monitor) const;
    RECT MonitorWorkArea(HMONITOR monitor) const;
    HMONITOR CursorMonitor() const;
    D2D1_RECT_F CardScreenRect(int index) const;

    Plugin* plugin_ = nullptr;
    RackView view_;
    PreviewWindow preview_;
    ContextMenu menu_;
    ContextMenu batchMenu_;
    bool menuOpen_ = false;
    int menuIndex_ = -1;
    bool hostMenuWasOpen_ = false;

    HMONITOR monitor_ = nullptr;
    RECT workArea_{};
    float scale_ = 1.0f;

    float expand_ = 0.0f;
    float animFrom_ = 0.0f;
    float animTo_ = 0.0f;
    uint64_t animStart_ = 0;
    uint64_t revealStart_ = 0;
    uint64_t swingUntil_ = 0;
    uint64_t normalizeAt_ = 0;
    /* 鍗曞紶鍗＄墖鏅冨姩鐨勬椂闂寸嚎锛屽拰鏁翠綋鎽嗗姩鍒嗗紑銆?*/
    uint64_t shakeUntil_ = 0;
    float basketHover_ = 0.0f;
    float basketHoverTarget_ = 0.0f;
    float basketHoverVelocity_ = 0.0f;
    float envelopeHover_ = 0.0f;
    float envelopeHoverTarget_ = 0.0f;
    float envelopeHoverVelocity_ = 0.0f;
    float paperPos_ = 0.0f;
    float paperVelocity_ = 0.0f;
    uint64_t envelopeEnterMs_ = 0;
    float cardHover_ = 0.0f;
    float cardHoverTarget_ = 0.0f;
    float cardHoverVelocity_ = 0.0f;
    int cardHoverIndex_ = -1;
    uint64_t lastAnimMs_ = 0;
    bool clickThrough_ = false;
    bool animating_ = false;
    bool expanding_ = false;
    uint64_t peekUntil_ = 0;

    uint64_t lastInside_ = 0;
    uint64_t hoverLineSince_ = 0;
    /* 鏀惰捣鍚庨棽缃竴娈垫椂闂村氨鎶婄礌鏉愪綅鍥?2x 琛ㄩ潰鏀炬帀锛屽父椹诲唴瀛橀殢涔嬪洖钀姐€?*/
    uint64_t lastExpandedMs_ = 0;
    bool idleMemoryReleased_ = false;
    bool lineHover_ = false;
    /* 鍒氬湪椤舵爮鐐硅繃涓€涓嬶細鎸囬拡绂诲紑椤惰竟涔嬪墠涓嶅啀鑷姩灞曞紑銆?*/
    bool topBarSuppressed_ = false;

    int hovered_ = -1;
    uint64_t hoveredSince_ = 0;
    int pressed_ = -1;
    int pressItemId_ = -1;
    RackPart pressedPart_ = RackPart::None;
    int pressColorIndex_ = -1;
    int pressColorPart_ = 0;
    uint64_t lastLinkOpenMs_ = 0;
    int lastLinkOpenIndex_ = -1;
    bool mouseDown_ = false;
    bool dragStarted_ = false;
    bool cardDragging_ = false;
    /* 按下时的偏移/屏幕左缘：只挪一点点松手要回原位（保留之前的自定义偏移）。 */
    float cardDragStartOffset_ = 0.0f;
    float cardDragStartLeft_ = 0.0f;
    uint64_t lastCardDragPaint_ = 0;
    bool hideWhenRemovalsDone_ = false;
    bool hotkeyPinned_ = false;
    struct OffsetAnim {
        int id = 0;
        float from = 0.0f;
        float to = 0.0f;
        uint64_t start = 0;
        float ms = 180.0f;
    };
    std::vector<OffsetAnim> offsetAnims_;
    int pendingAdjustId_ = -1;
    uint64_t pendingAdjustAt_ = 0;
    uint64_t menuCloseGraceUntil_ = 0;
    bool menuActionPending_ = false;
    /* 鐐瑰す瀛愯创鍥撅細绛夊崱鐗囦笅钀藉姩鐢昏蛋涓€鍗婏紝鍐嶈Е鍙戝師鐢熻创鍥俱€?*/
    enum class PendingPin { None, Image, Text, Files };
    PendingPin pendingPinKind_ = PendingPin::None;
    std::wstring pendingPinPath_;
    std::wstring pendingPinText_;
    std::vector<std::wstring> pendingPinPaths_;
    POINT pendingPinAnchor_{};
    uint64_t pendingPinAt_ = 0;

    void FinishCardDrag(int id);
    /* 拖动中实时重排：被拖的卡越过谁的槽位中线就插到那边，
       其他卡片只改槽位目标、靠弹簧自然让位。 */
    POINT pressPoint_{};

    int clickItemId_ = -1;
    int lastHoverPart_ = -1;
    int lastHoverPartIndex_ = -1;
    int lastHoverColorIndex_ = -1;
    int lastHoverColorPart_ = -1;
    int pendingEditIndex_ = -1;
    int lastCardSize_ = -1;
    float cardScaleValue_ = 1.0f;
    float cardScaleFrom_ = 1.0f;
    float cardScaleTarget_ = 1.0f;
    uint64_t cardScaleAnimStart_ = 0;
    bool cardScaleAnimating_ = false;
    /* 渚跨缂栬緫鍣ㄥ紑鐫€鏃剁怀瀛愪繚鎸佸睍寮€锛屼笉鑷姩鏀跺洖銆?*/
    bool editingNote_ = false;

    int previewIndex_ = -1;
    uint64_t previewHideAt_ = 0;
    RECT previewAnchor_{};
    uint64_t lastHoverPollMs_ = 0;
    bool updatingMaxTotal_ = false;

    bool hiddenForFullscreen_ = false;
    bool needsLayout_ = false;
    bool historyDisabled_ = false;
    /* 姣忔灞曞紑鍙煡涓€娆℃枃浠舵槸鍚﹁繕鍦紱涓嶅仛瀹氭椂杞銆?*/
    bool missingChecked_ = false;
    /* 鏄剧ず鏂瑰紡锛? 鎮仠灞曞紑銆? 蹇嵎閿垏鎹€? 甯搁┗鏄剧ず銆?*/
    int lastDisplayMode_ = 0;
    /* 蹇嵎閿ā寮忎笅鎵嬪姩闅愯棌锛氶殣钘忓悗涓嶅啀琚偓鍋?閰嶇疆杞鑷姩鎷夎捣銆?*/
    bool manuallyHidden_ = false;
    bool shownBeforeFullscreen_ = false;
    /* 棰勮鏈熼棿锛堣缃〉鐨勩€屾櫨琛ｆ灦澶栬銆嶏級鏀惰捣浼氳瀹夸富 pin 浣忥紱
       绛夐瑙堢粨鏉熷啀琛ヤ竴娆℃敹璧枫€?*/
    bool collapseAfterPreview_ = false;
    /* 璁剧疆椤垫妸鏅捐。鏋舵媺鍑烘潵杩囷細棰勮缁撴潫鍚庯紝蹇嵎閿?甯搁┗妯″紡涓嬭鏀跺洖銆?*/
    bool settingsPreviewShown_ = false;
};
