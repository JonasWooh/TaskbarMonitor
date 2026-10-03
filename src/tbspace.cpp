#include "tbspace.h"

#include <ole2.h>
#include <uiautomation.h>

namespace tbspace {
namespace {

const CLSID kClsidCUIAutomation = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
const IID kIidIUIAutomation = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};

IUIAutomation* g_uia;
IUIAutomationCacheRequest* g_cache;  // BoundingRectangle, fetched in the same round trip
IUIAutomationCondition* g_true;
IUIAutomationElement* g_frame;       // the XAML "TaskbarFrame" hosting the taskbar items
HWND g_frameOwner;

template <class T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

bool EnsureUia() {
    if (g_uia) return true;
    if (FAILED(CoCreateInstance(kClsidCUIAutomation, nullptr, CLSCTX_INPROC_SERVER, kIidIUIAutomation,
                                reinterpret_cast<void**>(&g_uia))))
        return false;
    if (FAILED(g_uia->CreateCacheRequest(&g_cache)) ||
        FAILED(g_cache->AddProperty(UIA_BoundingRectanglePropertyId)) ||
        FAILED(g_uia->CreateTrueCondition(&g_true))) {
        Shutdown();
        return false;
    }
    return true;
}

int g_retryIn;  // back-off when the frame can't be found (e.g. not a Windows 11 taskbar)

bool EnsureFrame(HWND taskbar) {
    if (g_frame && g_frameOwner == taskbar) return true;
    Release(g_frame);
    if (g_frameOwner == taskbar && g_retryIn > 0) {
        --g_retryIn;
        return false;
    }
    g_retryIn = 30;
    IUIAutomationElement* root = nullptr;
    if (FAILED(g_uia->ElementFromHandle(taskbar, &root)) || !root) return false;
    VARIANT id;
    VariantInit(&id);
    id.vt = VT_BSTR;
    id.bstrVal = SysAllocString(L"TaskbarFrame");
    IUIAutomationCondition* cond = nullptr;
    if (SUCCEEDED(g_uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &cond)))
        root->FindFirst(TreeScope_Descendants, cond, &g_frame);
    Release(cond);
    VariantClear(&id);
    root->Release();
    g_frameOwner = taskbar;
    return g_frame != nullptr;
}

}  // namespace

int OccupiedRight(HWND taskbar, int limitX) {
    if (!taskbar || !EnsureUia() || !EnsureFrame(taskbar)) return -1;
    IUIAutomationElementArray* items = nullptr;
    if (FAILED(g_frame->FindAllBuildCache(TreeScope_Children, g_true, g_cache, &items)) || !items) {
        Release(g_frame);  // explorer restarted or the frame was rebuilt; look it up again next time
        return -1;
    }
    int right = -1, count = 0;
    items->get_Length(&count);
    for (int i = 0; i < count; ++i) {
        IUIAutomationElement* e = nullptr;
        if (FAILED(items->GetElement(i, &e)) || !e) continue;
        RECT r;
        if (SUCCEEDED(e->get_CachedBoundingRectangle(&r)) && r.right > r.left && r.left < limitX && r.right > right)
            right = r.right;
        e->Release();
    }
    items->Release();
    return right;
}

void Shutdown() {
    Release(g_frame);
    Release(g_true);
    Release(g_cache);
    Release(g_uia);
    g_frameOwner = nullptr;
}

}  // namespace tbspace
