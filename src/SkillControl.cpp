#include "SkillControl.h"
#include "SkillPersistence.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

void SkillControl::Publish(unsigned player, SkillTelemetry value) {
    std::lock_guard lock(mutex);
    telemetry[player] = std::move(value);
}

void SkillControl::Select(SkillChoice choice) {
    selectedLevel.store(choice.Level());
    {std::lock_guard lock(mutex);selectedChoice=choice;}
    if (saveSelection) {
        const auto error = saveSelection(choice);
        std::lock_guard lock(mutex);
        saveError = error;
        if (!error.empty()) std::cerr << error << '\n';
    }
}

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <array>
#include <vector>

struct SkillWindow::Impl {
    static constexpr COLORREF Background=RGB(29,34,35), Panel=RGB(36,42,43), Well=RGB(21,24,25);
    static constexpr COLORREF Border=RGB(56,66,68), Text=RGB(216,208,191), Muted=RGB(157,167,165);
    static constexpr COLORREF Teal=RGB(43,116,122), Cyan=RGB(98,198,204), Gold=RGB(231,217,166);
    static constexpr UINT StopMessage=WM_APP+2, TrayMessage=WM_APP+1;
    static constexpr int RowHeight=54;
    struct RowControls { HWND caption{},model{},combo{},minus{},plus{}; unsigned slot=0; };
    std::shared_ptr<SkillControl> control;
    bool standalone=false,expanded=false;
    std::thread thread;
    HWND window{},heading{},subtitle{},apply{},debug{},footer{},details{};
    std::vector<RowControls> rowControls;
    std::array<HFONT,4> fonts{};
    HBRUSH backgroundBrush{},panelBrush{},wellBrush{};
    UINT dpi=96;
    NOTIFYICONDATAW tray{};
    int visibleRows=0;
    std::wstring lastText;
    int Px(int value) const {return MulDiv(value,dpi,96);}
    int Dips(int value) const {return MulDiv(value,96,dpi);}
    static std::wstring Wide(const std::string& text) {
        int size=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);
        std::wstring result(size,L' ');MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),result.data(),size);return result;
    }
    static void SetText(HWND child,const std::wstring& text) {
        std::wstring old(GetWindowTextLengthW(child)+1,L'\0');
        old.resize(GetWindowTextW(child,old.data(),int(old.size())));
        if(old!=text) SetWindowTextW(child,text.c_str());
    }
    static std::wstring ComboText(HWND combo,int index) {
        const auto length=SendMessageW(combo,CB_GETLBTEXTLEN,index,0);
        if(length==CB_ERR) return {};
        std::wstring text(size_t(length)+1,L'\0');
        SendMessageW(combo,CB_GETLBTEXT,index,reinterpret_cast<LPARAM>(text.data()));
        text.resize(size_t(length));return text;
    }
    static void Fill(HDC dc,RECT r,COLORREF color) {auto b=CreateSolidBrush(color);FillRect(dc,&r,b);DeleteObject(b);}
    void Round(HDC dc,RECT r,COLORREF fill,COLORREF edge) {
        auto b=CreateSolidBrush(fill);auto p=CreatePen(PS_SOLID,Px(1),edge);
        auto oldB=SelectObject(dc,b),oldP=SelectObject(dc,p);
        RoundRect(dc,r.left,r.top,r.right,r.bottom,Px(6),Px(6));
        SelectObject(dc,oldB);SelectObject(dc,oldP);DeleteObject(b);DeleteObject(p);
    }
    void RebuildFonts() {
        auto old=fonts;
        const int sizes[]={14,11,23,12};
        for(int i=0;i<4;++i) fonts[i]=CreateFontW(-Px(sizes[i]),0,0,0,i==2?FW_SEMIBOLD:FW_NORMAL,
            FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,i==3?L"Cascadia Mono":L"Segoe UI");
        if(heading) {
            for(auto h:{apply,debug}) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(fonts[0]),TRUE);
            for(auto h:{subtitle,footer}) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(fonts[1]),TRUE);
            SendMessageW(heading,WM_SETFONT,reinterpret_cast<WPARAM>(fonts[2]),TRUE);
            SendMessageW(details,WM_SETFONT,reinterpret_cast<WPARAM>(fonts[3]),TRUE);
            for(auto& r:rowControls) {
                for(auto h:{r.caption,r.model,r.combo,r.minus,r.plus}) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(fonts[0]),TRUE);
                for(auto h:{r.model,r.combo}) {
                    SendMessageW(h,CB_SETITEMHEIGHT,-1,Px(22));SendMessageW(h,CB_SETITEMHEIGHT,0,Px(28));
                }
                SendMessageW(r.combo,CB_SETDROPPEDWIDTH,Px(300),0);
            }
        }
        for(auto f:old) if(f) DeleteObject(f);
    }
    HWND Child(const wchar_t* type,const wchar_t* text,DWORD style,int id,int font=0) {
        auto h=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,0,0,10,10,window,
            reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(fonts[font]),TRUE);return h;
    }
    HWND Label(const wchar_t* text,int id,int font=1) {return Child(L"STATIC",text,SS_LEFT|SS_NOPREFIX,id,font);}
    HWND Button(const wchar_t* text,int id) {return Child(L"BUTTON",text,BS_OWNERDRAW|WS_TABSTOP,id);}
    void EnsureRows(int count) {
        // Keep every car on the same page, including rosters arriving after startup.
        while(int(rowControls.size())<count) {
            const int i=int(rowControls.size());
            rowControls.emplace_back();auto& r=rowControls.back();r.caption=Label(L"",20+i,0);
            r.model=Child(L"COMBOBOX",L"Model",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL|WS_TABSTOP,100+i*4);
            r.combo=Child(L"COMBOBOX",L"Rank",CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL|WS_TABSTOP,101+i*4);
            for(auto h:{r.model,r.combo}) {
                SetWindowTheme(h,L"",L"");SetWindowSubclass(h,ComboProcedure,1,reinterpret_cast<DWORD_PTR>(this));
                SendMessageW(h,CB_SETITEMHEIGHT,-1,Px(22));SendMessageW(h,CB_SETITEMHEIGHT,0,Px(28));
            }
            SendMessageW(r.combo,CB_SETDROPPEDWIDTH,Px(300),0);
            for(int mode=0;mode<4;++mode) {auto label=Wide(SkillChoiceLabel(static_cast<SkillChoiceMode>(mode)));SendMessageW(r.model,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}
            for(int level=0;level<=MaxSelectableRank;++level) {auto label=Wide(std::to_string(level)+" / "+SkillLabel(level));SendMessageW(r.combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));}
            r.minus=Button(L"-",102+i*4);r.plus=Button(L"+",103+i*4);
        }
    }
    void Place(HWND h,int x,int y,int w,int height) {MoveWindow(h,Px(x),Px(y),Px(w),Px(height),TRUE);}
    int Bottom() const {return 70+std::max(visibleRows,1)*RowHeight;}
    void Layout() {
        if(!heading) return;
        RECT client;GetClientRect(window,&client);int w=Dips(client.right),h=Dips(client.bottom),bottom=Bottom();
        Place(heading,24,14,w-48,30);Place(subtitle,24,46,w-48,19);
        for(int i=0;i<int(rowControls.size());++i) {
            auto& r=rowControls[i];int y=70+i*RowHeight;
            Place(r.caption,24,y,w-48,18);
            Place(r.model,24,y+20,160,180);Place(r.combo,196,y+20,w-332,300);
            Place(r.minus,w-124,y+20,44,28);Place(r.plus,w-68,y+20,44,28);
            for(auto child:{r.caption,r.model,r.combo,r.minus,r.plus}) ShowWindow(child,i<visibleRows?SW_SHOWNA:SW_HIDE);
        }
        Place(apply,24,bottom+8,210,32);Place(debug,w-156,bottom+8,132,32);
        Place(footer,24,bottom+48,w-48,40);
        Place(details,24,bottom+94,w-48,std::max(40,h-bottom-114));ShowWindow(details,expanded?SW_SHOWNA:SW_HIDE);
        InvalidateRect(window,nullptr,TRUE);
    }
    void SizeToContent() {
        RECT current;GetWindowRect(window,&current);MONITORINFO monitor={sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
        RECT frame={0,0,Px(740),Px(Bottom()+94+(expanded?220:0))};
        AdjustWindowRectExForDpi(&frame,DWORD(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,0,dpi);
        int width=frame.right-frame.left,height=std::min(int(frame.bottom-frame.top),int(monitor.rcWork.bottom-monitor.rcWork.top));
        int x=std::clamp(int(current.left),int(monitor.rcWork.left),std::max(int(monitor.rcWork.left),int(monitor.rcWork.right)-width));
        int y=std::clamp(int(current.top),int(monitor.rcWork.top),std::max(int(monitor.rcWork.top),int(monitor.rcWork.bottom)-height));
        SetWindowPos(window,nullptr,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE);Layout();
    }
    void Passive(bool value) {
        if(standalone) return;
        const auto style=GetWindowLongPtrW(window,GWL_EXSTYLE);
        const auto next=value?style|WS_EX_NOACTIVATE:style&~LONG_PTR(WS_EX_NOACTIVATE);
        if(next!=style) SetWindowLongPtrW(window,GWL_EXSTYLE,next);
    }
    void OpenFromUser() {
        Passive(false);
        ShowWindow(window,IsIconic(window)?SW_RESTORE:SW_SHOW);
        SetForegroundWindow(window);
    }
    void Edit(int row,int action) {
        const auto slot=rowControls[row].slot;
        {
            std::lock_guard lock(control->mutex);
            const auto found=control->rows.find(slot);
            if(found==control->rows.end() || control->applying) return;
            auto& r=found->second;
            if(action==0) {
                const int selected=int(SendMessageW(rowControls[row].model,CB_GETCURSEL,0,0));
                if(selected>=0 && selected<=3) r.pending.mode=static_cast<SkillChoiceMode>(selected);
            } else if(r.pending.mode==SkillChoiceMode::SelectSkill) {
                const int rank=action==1?int(SendMessageW(rowControls[row].combo,CB_GETCURSEL,0,0)):
                    r.pending.rank+(action==2?-1:1);
                r.pending.rank=std::clamp(rank,0,MaxSelectableRank);
            }
        }
        Refresh();
    }
    void Apply() {
        if(standalone) {
            SkillChoice choice;
            {std::lock_guard lock(control->mutex);choice=control->rows.at(0).pending;}
            control->Select(choice);const int level=choice.Level();
            {std::lock_guard lock(control->mutex);auto& r=control->rows.at(0);r.active=level;r.activeMode=choice.mode;r.name=ChoiceBotName(choice);
                control->status=control->saveError.empty()?"Default saved for the next match":control->saveError;}
        } else if(control->applyPending) control->applyPending();
        Refresh();
    }
    void Refresh() {
        std::map<unsigned,SkillRow> rows;std::map<unsigned,SkillTelemetry> telemetry;
        bool busy,ready;std::string status,error;
        {std::lock_guard lock(control->mutex);rows=control->rows;telemetry=control->telemetry;
            busy=control->applying;ready=control->canRespawn;status=control->status;error=control->saveError;}
        const int count=int(rows.size());bool resize=count!=visibleRows;visibleRows=count;
        EnsureRows(count);
        // Group by team while retaining match slots as the edit keys.
        std::vector<unsigned> displayOrder;
        for(const auto& [slot,row]:rows) displayOrder.push_back(slot);
        std::stable_sort(displayOrder.begin(),displayOrder.end(),[&rows](unsigned a,unsigned b) {
            return rows.at(a).team<rows.at(b).team;
        });
        int dirty=0;for(const auto& [id,r]:rows) dirty+=r.active!=r.pending.Level() || r.activeMode!=r.pending.mode;
        for(int i=0;i<count;++i) {
            const auto slot=displayOrder[i];
            auto& widgets=rowControls[i];const auto& r=rows.at(slot);widgets.slot=slot;
            const std::string caption=standalone?"Next match":(r.team?"Orange":"Blue")+std::string(" / ")+r.name;
            SetText(widgets.caption,Wide(caption));
            const bool custom=r.pending.mode==SkillChoiceMode::SelectSkill;
            const int displayedRank=custom?r.pending.rank:-1;
            // An open combo reports its transient hover highlight as the current
            // selection. Resetting it from pending every timer tick fights the
            // mouse and scroll position; synchronize only after the list closes.
            if(!SendMessageW(widgets.model,CB_GETDROPPEDSTATE,0,0) &&
                SendMessageW(widgets.model,CB_GETCURSEL,0,0)!=int(r.pending.mode)) {
                SendMessageW(widgets.model,CB_SETCURSEL,int(r.pending.mode),0);InvalidateRect(widgets.model,nullptr,TRUE);
            }
            if(!SendMessageW(widgets.combo,CB_GETDROPPEDSTATE,0,0) &&
                SendMessageW(widgets.combo,CB_GETCURSEL,0,0)!=displayedRank) {
                SendMessageW(widgets.combo,CB_SETCURSEL,displayedRank,0);InvalidateRect(widgets.combo,nullptr,TRUE);
            }
            EnableWindow(widgets.model,!busy);EnableWindow(widgets.combo,!busy && custom);
            EnableWindow(widgets.minus,!busy && custom && r.pending.rank>0);
            EnableWindow(widgets.plus,!busy && custom && r.pending.rank<MaxSelectableRank);
        }
        SetText(subtitle,Wide(count?dirty?std::to_string(dirty)+(dirty==1?" pending change":" pending changes"):"Choose a model or skill for each bot":"Waiting for Byte bots..."));
        EnableWindow(apply,!busy && (standalone || ready));
        SetText(apply,busy?L"Respawning...":standalone?L"Save default":L"Apply & respawn");
        std::string note=status;
        if(!error.empty()) note+="\r\n"+error;
        SetText(footer,Wide(note));
        std::ostringstream text;text<<std::fixed<<std::setprecision(2);
        for(const auto& [slot,t]:telemetry) {
            text<<t.name<<" / "<<(t.team?"Orange":"Blue")<<" / "<<t.teammates<<"v"<<t.opponents<<" / "<<t.teamScore<<":"<<t.opponentScore<<"\r\n"
                <<"Effective level "<<t.effectiveLevel<<" / "<<SkillModelName(t.settings.model)<<"\r\n";
            if(t.scriptedKickoff) {
                text<<"Scripted kickoff / Drive + boost to supersonic / No flips\r\n"
                    <<"Steering updates every packet / Policy sampling paused\r\n\r\n";
                continue;
            }
            text<<"Temperature "<<t.settings.sampling.temperature<<" / Top K "<<t.settings.sampling.topK<<" / Cutoff "<<100*t.settings.sampling.minRelativeProbability<<"%\r\n"
                <<"Best share "<<100*t.settings.sampling.bestActionProbability<<"% target / "<<100*t.sampling.finalBestProbability<<"% actual\r\n"
                <<"Confidence "<<100*t.sampling.policyBestProbability<<"% / Entropy "<<t.sampling.normalizedEntropy<<" / Candidates "<<t.sampling.retainedActions<<"\r\n"
                <<"Decision "<<t.settings.decisionTicks<<" ticks / Delay 7 / Kickoff guard "<<(t.kickoff?"ON":"off")<<"\r\n\r\n";
        }
        if(telemetry.empty()) text<<"Waiting for live policy diagnostics.\r\nPending selections do not alter the active bots.";
        if(!error.empty()) text<<"\r\nSave warning: "<<error;
        const auto value=Wide(text.str());
        if(value!=lastText) {auto top=SendMessageW(details,EM_GETFIRSTVISIBLELINE,0,0);SetWindowTextW(details,value.c_str());SendMessageW(details,EM_LINESCROLL,0,top);lastText=value;}
        if(resize) SizeToContent();
    }
    void Paint() {
        PAINTSTRUCT ps;HDC dc=BeginPaint(window,&ps);RECT r;GetClientRect(window,&r);Fill(dc,r,Background);
        Fill(dc,{Px(24),Px(Bottom()),r.right-Px(24),Px(Bottom()+1)},Border);EndPaint(window,&ps);
    }
    void DrawControl(const DRAWITEMSTRUCT& item) {
        const int saved=SaveDC(item.hDC);SetBkMode(item.hDC,TRANSPARENT);SelectObject(item.hDC,fonts[0]);
        SetTextColor(item.hDC,item.itemState&ODS_DISABLED?Muted:Text);RECT r=item.rcItem;
        if(item.CtlType==ODT_COMBOBOX) {
            // Windows also marks the collapsed field as selected during focus
            // changes. Only rows in the open list should get the teal fill.
            const bool listHighlight=(item.itemState&ODS_SELECTED) && !(item.itemState&ODS_COMBOBOXEDIT);
            Fill(item.hDC,r,listHighlight?Teal:Panel);
            if(item.itemID!=UINT(-1)) {
                auto text=ComboText(item.hwndItem,int(item.itemID));
                r.left+=Px(12);r.right-=Px(8);DrawTextW(item.hDC,text.c_str(),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
            }
        } else {
            Fill(item.hDC,r,Background);bool primary=item.hwndItem==apply && !(item.itemState&ODS_DISABLED);
            Round(item.hDC,r,(item.itemState&ODS_SELECTED)||primary?Teal:Panel,(item.itemState&ODS_FOCUS)?Gold:Border);
            wchar_t text[128];GetWindowTextW(item.hwndItem,text,128);DrawTextW(item.hDC,text,-1,&r,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_NOPREFIX);
        }
        RestoreDC(item.hDC,saved);
    }
    static LRESULT CALLBACK ComboProcedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data) {
        auto* self=reinterpret_cast<Impl*>(data);auto result=DefSubclassProc(hwnd,message,wp,lp);
        // Native focus changes redraw the selection but can leave our custom border stale.
        if(message==WM_SETFOCUS || message==WM_KILLFOCUS) InvalidateRect(hwnd,nullptr,FALSE);
        if(message==WM_PAINT) {
            HDC dc=GetDC(hwnd);const int saved=SaveDC(dc);RECT r;GetClientRect(hwnd,&r);Fill(dc,r,Panel);
            auto edge=CreateSolidBrush(GetFocus()==hwnd?Cyan:Border);FrameRect(dc,&r,edge);DeleteObject(edge);
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,IsWindowEnabled(hwnd)?Text:Muted);SelectObject(dc,self->fonts[0]);
            int selected=int(SendMessageW(hwnd,CB_GETCURSEL,0,0));
            {auto text=selected>=0?ComboText(hwnd,selected):L"\u2014";RECT t=r;t.left+=self->Px(12);t.right-=self->Px(36);DrawTextW(dc,text.c_str(),-1,&t,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);}
            int x=r.right-self->Px(18),y=(r.top+r.bottom)/2;POINT points[]={{x-self->Px(4),y-self->Px(2)},{x+self->Px(4),y-self->Px(2)},{x,y+self->Px(3)}};
            auto brush=CreateSolidBrush(IsWindowEnabled(hwnd)?Cyan:Muted);SelectObject(dc,brush);SelectObject(dc,GetStockObject(NULL_PEN));Polygon(dc,points,3);
            RestoreDC(dc,saved);DeleteObject(brush);ReleaseDC(hwnd,dc);
        }
        return result;
    }
    static LRESULT CALLBACK Procedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE) {self=reinterpret_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self) return DefWindowProcW(hwnd,message,wp,lp);
        switch(message) {
        case WM_ACTIVATE:self->Passive(LOWORD(wp)==WA_INACTIVE);break;
        case WM_MOUSEACTIVATE:
            if(!self->standalone) {
                // Active-window tracking may ask to activate on hover even with
                // WS_EX_NOACTIVATE. Only an actual click should opt into focus.
                // https://devblogs.microsoft.com/oldnewthing/20240919-00/?p=110283
                const auto mouse=HIWORD(lp);
                if(mouse==WM_LBUTTONDOWN || mouse==WM_RBUTTONDOWN || mouse==WM_MBUTTONDOWN || mouse==WM_XBUTTONDOWN ||
                    mouse==WM_NCLBUTTONDOWN || mouse==WM_NCRBUTTONDOWN || mouse==WM_NCMBUTTONDOWN || mouse==WM_NCXBUTTONDOWN) {
                    self->Passive(false);return MA_ACTIVATE;
                }
                return MA_NOACTIVATE;
            }
            break;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:self->Paint();return 0;
        case WM_SIZE:self->Layout();return 0;
        case WM_GETMINMAXINFO:{
            RECT minimum={0,0,self->Px(620),self->Px(self->Bottom()+94)};
            AdjustWindowRectExForDpi(&minimum,DWORD(GetWindowLongPtrW(hwnd,GWL_STYLE)),FALSE,0,self->dpi);
            MONITORINFO monitor={sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={minimum.right-minimum.left,
                std::min(minimum.bottom-minimum.top,monitor.rcWork.bottom-monitor.rcWork.top)};return 0;}
        case WM_DPICHANGED:{self->dpi=HIWORD(wp);self->RebuildFonts();const auto& r=*reinterpret_cast<RECT*>(lp);SetWindowPos(hwnd,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);self->Layout();return 0;}
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:{
            auto dc=reinterpret_cast<HDC>(wp);auto child=reinterpret_cast<HWND>(lp);
            COLORREF color=Background;HBRUSH brush=self->backgroundBrush;
            if(child==self->details) {color=Well;brush=self->wellBrush;}
            if(message==WM_CTLCOLORLISTBOX) {color=Panel;brush=self->panelBrush;}
            SetBkColor(dc,color);SetTextColor(dc,child==self->footer || child==self->subtitle?Muted:Text);return reinterpret_cast<LRESULT>(brush);}
        case WM_MEASUREITEM:reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight=self->Px(28);return TRUE;
        case WM_DRAWITEM:self->DrawControl(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));return TRUE;
        case WM_COMMAND:{
            int id=LOWORD(wp);
            if(id>=100 && id<100+int(self->rowControls.size())*4) {int row=(id-100)/4,action=(id-100)%4;
                if((action<2 && HIWORD(wp)==CBN_SELCHANGE) || (action>=2 && HIWORD(wp)==BN_CLICKED)) self->Edit(row,action);
            } else if(id==1) self->Apply();
            else if(id==2) {self->expanded=!self->expanded;self->control->debug=self->expanded;SetWindowTextW(self->debug,self->expanded?L"Hide debug":L"Show debug");self->SizeToContent();}
            return 0;}
        case WM_TIMER:self->Refresh();return 0;
        case TrayMessage:if(lp==WM_LBUTTONDBLCLK || lp==WM_RBUTTONUP) self->OpenFromUser();return 0;
        case WM_CLOSE:if(self->standalone) DestroyWindow(hwnd);else ShowWindow(hwnd,SW_HIDE);return 0;
        case StopMessage:DestroyWindow(hwnd);return 0;
        case WM_DESTROY:Shell_NotifyIconW(NIM_DELETE,&self->tray);PostQuitMessage(0);return 0;
        }
        return DefWindowProcW(hwnd,message,wp,lp);
    }
    Impl(std::shared_ptr<SkillControl> shared,bool preview):control(std::move(shared)),standalone(preview) {
        if(standalone) {std::lock_guard lock(control->mutex);int level=control->selectedLevel.load();control->rows[0]={0,0,ChoiceBotName(control->selectedChoice),level,control->selectedChoice,control->selectedChoice.mode};control->status="Save this default before starting a new match";}
        std::promise<void> ready;auto future=ready.get_future();
        thread=std::thread([this,ready=std::move(ready)]() mutable {
            SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            backgroundBrush=CreateSolidBrush(Background);panelBrush=CreateSolidBrush(Panel);wellBrush=CreateSolidBrush(Well);
            WNDCLASSW cls{};cls.lpfnWndProc=Procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"ByteRankRoster";cls.hCursor=LoadCursor(nullptr,IDC_ARROW);
            RegisterClassW(&cls);
            // Keep automatic startup out of Windows' activation fallback, while
            // retaining taskbar/Alt+Tab access for deliberate user selection.
            const DWORD extended=standalone?0:WS_EX_NOACTIVATE|WS_EX_APPWINDOW;
            auto hwnd=CreateWindowExW(extended,cls.lpszClassName,L"Byte Model Selection",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,620,320,nullptr,nullptr,cls.hInstance,this);
            if(hwnd) {
                dpi=GetDpiForWindow(hwnd);RebuildFonts();BOOL dark=TRUE;DwmSetWindowAttribute(hwnd,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
                COLORREF caption=Well;DwmSetWindowAttribute(hwnd,DWMWA_CAPTION_COLOR,&caption,sizeof(caption));
                heading=Label(L"Byte Model Selection",10,2);subtitle=Label(L"",11);
                apply=Button(L"Apply & respawn",1);expanded=control->debug.load();debug=Button(expanded?L"Hide debug":L"Show debug",2);
                footer=Label(L"",13);details=Child(L"EDIT",L"Debug details",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,14,3);SetWindowTheme(details,L"DarkMode_Explorer",nullptr);
                Refresh();SizeToContent();
                tray.cbSize=sizeof(tray);tray.hWnd=hwnd;tray.uID=1;tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP;
                tray.uCallbackMessage=TrayMessage;tray.hIcon=LoadIcon(nullptr,IDI_APPLICATION);wcscpy_s(tray.szTip,L"Byte Model Selection - double-click to reopen");Shell_NotifyIconW(NIM_ADD,&tray);
                SetTimer(hwnd,1,500,nullptr);
                if(standalone) ShowWindow(hwnd,SW_SHOW);
                // SW_SHOWNOACTIVATE can still raise the window over the game.
                // Show behind existing windows, without ShowWindow's startup override.
                else SetWindowPos(hwnd,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
            }
            ready.set_value();
            if(hwnd) {MSG message;while(GetMessageW(&message,nullptr,0,0)>0) if(!IsDialogMessageW(hwnd,&message)) {TranslateMessage(&message);DispatchMessageW(&message);}}
            for(auto font:fonts) if(font) DeleteObject(font);DeleteObject(backgroundBrush);DeleteObject(panelBrush);DeleteObject(wellBrush);UnregisterClassW(cls.lpszClassName,cls.hInstance);
        });future.wait();
    }
    ~Impl() {if(thread.joinable()) {PostMessageW(window,StopMessage,0,0);thread.join();}}
};
#else
struct SkillWindow::Impl {Impl(std::shared_ptr<SkillControl>,bool) {std::cerr<<"The live panel is available on Windows. Use --no-ui for headless startup.\n";}};
#endif

SkillWindow::SkillWindow(std::shared_ptr<SkillControl> control,bool standalone):impl(std::make_unique<Impl>(std::move(control),standalone)) {}
SkillWindow::~SkillWindow() = default;
void SkillWindow::Wait() {
#ifdef _WIN32
    if(impl->thread.joinable())impl->thread.join();
#endif
}
