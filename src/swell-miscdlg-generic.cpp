/* Cockos SWELL (Simple/Small Win32 Emulation Layer for Linux/OSX)
   Copyright (C) 2006 and later, Cockos, Inc.

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software
       in a product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.
  

    This file provides basic APIs for browsing for files, directories, and messageboxes.

    These APIs don't all match the Windows equivelents, but are close enough to make it not too much trouble.

  */


#ifndef SWELL_PROVIDED_BY_APP

#include "swell.h"
#include "swell-internal.h"
#include "swell-dlggen.h"

#include "../wdlcstring.h"
#include "../assocarray.h"
#include "../ptrlist.h"
#include <dirent.h>
#include <time.h>
#include <cctype>
#include <sys/wait.h>

#include "../lineparse.h"
#define WDL_HASSTRINGS_EXPORT static
#include "../has_strings.h"


#ifndef SWELL_BROWSE_RECENT_SIZE
#define SWELL_BROWSE_RECENT_SIZE 12
#endif
static WDL_PtrList<char> s_browse_rcu, s_browse_rcu_tmp;
static int recent_size() { return s_browse_rcu.GetSize() + s_browse_rcu_tmp.GetSize(); }

static void recent_addtocb(HWND hwnd)
{
  int x;
  for (x=0;x<s_browse_rcu_tmp.GetSize();x++) 
    SendMessage(hwnd,CB_ADDSTRING,0,(LPARAM)s_browse_rcu_tmp.Get(x));
  for (x=0;x<s_browse_rcu.GetSize();x++) 
    SendMessage(hwnd,CB_ADDSTRING,0,(LPARAM)s_browse_rcu.Get(x));
}
static void recent_write(const char *path)
{
  if (!path || !path[0]) return;
  int x;
  for (x=0;x<s_browse_rcu.GetSize() && strcmp(s_browse_rcu.Get(x),path); x++);
  if (x<s_browse_rcu.GetSize())
  {
    if (!x) return; // already at top of flist

    char *ps = s_browse_rcu.Get(x);
    s_browse_rcu.Delete(x,false);
    s_browse_rcu.Insert(0,ps);
  }
  else
  {
    if (s_browse_rcu.GetSize()>=SWELL_BROWSE_RECENT_SIZE)
      s_browse_rcu.Delete(SWELL_BROWSE_RECENT_SIZE,true,free);
    s_browse_rcu.Insert(0,strdup(path));
  }

  for (x=0;x<=s_browse_rcu.GetSize();x++)
  {
    char tmp[64];
    snprintf(tmp,sizeof(tmp),"path%d",x);
    WritePrivateProfileString(".swell_recent_path",tmp, s_browse_rcu.Get(x),"");
  }
}
static void recent_read()
{
  s_browse_rcu_tmp.Empty(true,free);
  if (s_browse_rcu.GetSize()) return;
  int x;
  for (x=0;x<SWELL_BROWSE_RECENT_SIZE;x++)
  {
    char tmp[64], path[2048];
    snprintf(tmp,sizeof(tmp),"path%d",x);
    GetPrivateProfileString(".swell_recent_path",tmp, "", path,sizeof(path),"");
    if (!path[0]) break;
    s_browse_rcu.Add(strdup(path));
  }
}
static void recent_add_tmp(const char *path)
{
  if (!path || !*path) return;

  int x;
  for (x=0;x<s_browse_rcu_tmp.GetSize();x++) 
    if (!strcmp(s_browse_rcu_tmp.Get(x),path)) return;
  for (x=0;x<s_browse_rcu.GetSize();x++) 
    if (!strcmp(s_browse_rcu.Get(x),path)) return;

  s_browse_rcu_tmp.Add(strdup(path));
}

static const char *BFSF_Templ_dlgid;
static DLGPROC BFSF_Templ_dlgproc;
static struct SWELL_DialogResourceIndex *BFSF_Templ_reshead;
void BrowseFile_SetTemplate(const char *dlgid, DLGPROC dlgProc, struct SWELL_DialogResourceIndex *reshead)
{
  BFSF_Templ_reshead=reshead;
  BFSF_Templ_dlgid=dlgid;
  BFSF_Templ_dlgproc=dlgProc;
}

extern struct stat stat_chk;
typedef char assert_failed_stat_not_64[sizeof(stat_chk.st_size)!=8 ? -1 : 1];

class BrowseFile_State
{
public:
  static char s_sortrev;
  enum modeEnum { SAVE=0,OPEN, OPENMULTI, OPENDIR };

  BrowseFile_State(const char *_cap, const char *_idir, const char *_ifile, const char *_el, modeEnum _mode, char *_fnout, int _fnout_sz) :
    caption(_cap), initialdir(_idir), initialfile(_ifile), extlist(_el), mode(_mode),
    sortcol(0), sortrev(0),
    fnout(_fnout), fnout_sz(_fnout_sz), viewlist_store(16384), viewlist(4096), show_hidden(false),
    places_visible(true), places_width(SWELL_UI_SCALE(140))
  {
  }
  ~BrowseFile_State()
  {
    viewlist_clear();
  }

  const char *caption;
  const char *initialdir;
  const char *initialfile;
  const char *extlist;

  modeEnum mode;
  char sortcol, sortrev;

  char *fnout; // if NULL this will be malloced by the window
  int fnout_sz;

  struct rec {
    WDL_INT64 size;
    time_t date;
    char *name;
    int type; // 1 = directory, 2 = file

    void format_date(char *buf, int bufsz)
    {
      *buf=0;
      const WDL_INT64 date64 = (WDL_INT64) date;
      if (date64 > 0 && date64 < WDL_INT64_CONST(0x793406fff))
      {
        struct tm *a=localtime(&date);
        if (a) strftime(buf,bufsz,"%c",a);
      }
    }

    void format_size(char *buf, int bufsz)
    {
      if (type == 1)
      {
        lstrcpyn_safe(buf,"<DIR>",bufsz);
      }
      else
      {
        static const char *tab[]={ "bytes","KB","MB","GB" };
        int lf=0;
        WDL_INT64 s=size;
        if (s<1024)
        {
          snprintf(buf,bufsz,"%d %s",(int)s,tab[0]);
        }
        else
        {
          int w = 1;
          do {  w++; lf = (int)(s&1023); s/=1024; } while (s >= 1024 && w<4);
          snprintf(buf,bufsz,"%d.%d %s",(int)s,(int)((lf*10.0)/1024.0+0.5),tab[w-1]);
        }
      }
    }

    char *format_all(char *buf, int bufsz)
    {
      char dstr[128],sstr[128];
      format_date(dstr,sizeof(dstr));
      format_size(sstr,sizeof(sstr));
      snprintf(buf,bufsz,"%s\t%s\t%s",WDL_get_filepart(name),dstr,sstr);
      return buf;
    }

  };

  void viewlist_clear()
  {
    rec *l = viewlist_store.Get();
    const int n = viewlist_store.GetSize();
    for (int x = 0; x < n; x ++) free(l[x].name);
    viewlist_store.Resize(0);
    viewlist.Empty();
  }
  WDL_TypedBuf<rec> viewlist_store;
  WDL_PtrList<rec> viewlist;

  bool show_hidden;
  bool places_visible;
  int  places_width;

  void viewlist_sort(const char *filter)
  {
    if (filter)
    {
      viewlist.Empty();
      LineParser lp;
      const bool no_filter = !*filter || !WDL_makeSearchFilter(filter,&lp);
      for (int x=0;x<viewlist_store.GetSize();x++) 
      {
        rec *r = viewlist_store.Get()+x;
        char tmp[512];
        if (no_filter || WDL_hasStrings(r->format_all(tmp,sizeof(tmp)),&lp))
          viewlist.Add(r);
      }
    }
    s_sortrev = sortrev;
    if (viewlist.GetSize()>1)
      qsort(viewlist.GetList(), viewlist.GetSize(),sizeof(rec*), 
        sortcol == 1 ? sortFunc_sz :
        sortcol == 2 ? sortFunc_date : 
        sortFunc_fn);
  }
  static int sortFunc_fn(const void *_a, const void *_b)
  {
    const rec *a = *(const rec * const*)_a, *b = *(const rec * const*)_b;
    int d = a->type - b->type;
    if (d) return d;
    d = stricmp(a->name,b->name);
    return s_sortrev ? -d : d;
  }
  static int sortFunc_date(const void *_a, const void *_b)
  {
    const rec *a = *(const rec * const*)_a, *b = *(const rec * const*)_b;
    if (a->date != b->date) return s_sortrev ? (a->date>b->date?-1:1) : (a->date>b->date?1:-1);
    return stricmp(a->name,b->name);
  }
  static int sortFunc_sz(const void *_a, const void *_b)
  {
    const rec *a = *(const rec * const *)_a, *b = *(const rec * const *)_b;
    int d = a->type - b->type;
    if (d) return s_sortrev ? -d : d;
    if (a->size != b->size) return s_sortrev ? (a->size>b->size?-1:1) : (a->size>b->size?1:-1);
    return stricmp(a->name,b->name);
  }


  void scan_path(const char *path, const char *filterlist, bool dir_only)
  {
    viewlist_clear();
    DIR *dir = opendir(path);
    if (!dir) return;
    char tmp[2048];
    struct dirent *ent;
    while (NULL != (ent = readdir(dir)))
    {
      if (ent->d_name[0] == '.') 
      {
        if (ent->d_name[1] == 0 || ent->d_name[1] == '.' || !show_hidden) continue;
      }
      bool is_dir = (ent->d_type == DT_DIR);
      if (ent->d_type == DT_UNKNOWN)
      {
        snprintf(tmp,sizeof(tmp),"%s/%s",path,ent->d_name);
        DIR *d = opendir(tmp);
        if (d) { is_dir = true; closedir(d); }
      }
      else if (ent->d_type == DT_LNK)
      {
        snprintf(tmp,sizeof(tmp),"%s/%s",path,ent->d_name);
        char *rp = realpath(tmp,NULL);
        if (rp)
        {
          DIR *d = opendir(rp);
          if (d) { is_dir = true; closedir(d); }
          free(rp);
        }
      }
      if (!dir_only || is_dir)
      {
        if (filterlist && *filterlist && !is_dir)
        {
          const char *f = filterlist;
          while (*f)
          {
            const char *nf = f;
            while (*nf && *nf != ';') nf++;
            if (*f != '*')
            {
              const char *nw = f;
              while (nw < nf && *nw != '*') nw++;

              if ((nw!=nf || f+strlen(ent->d_name) == nw) && !strncasecmp(ent->d_name,f,nw-f)) 
              {
                // matched leading text
                if (nw == nf) break;
                f = nw;
              }
            }

            if (*f == '*')
            {
              f++;
              if (!*f || *f == ';' || (*f == '.' && f[1] == '*')) break;
              size_t l = strlen(ent->d_name);
              if (f+l > nf && !strncasecmp(ent->d_name + l - (nf-f), f,nf-f)) break;
            }
            f = nf;
            while (*f == ';') f++;
          }
          if (!*f) continue; // did not match
        }
        snprintf(tmp,sizeof(tmp),"%s/%s",path,ent->d_name);

        struct stat st={0,};
        stat(tmp,&st);

        rec r = { st.st_size, st.st_mtime, strdup(ent->d_name), is_dir?1:2 } ;
        viewlist_store.Add(&r,1);
      }
    }
    // sort viewlist

    closedir(dir);
  }
};

char BrowseFile_State::s_sortrev;

static void preprocess_user_path(char *buf, int bufsz)
{
  if (buf[0] == '~')
  {
    char *tmp = strdup(buf+1);
    if (buf[1] == '/' || !buf[1])
    {
      char *p = getenv("HOME");
      if (p && *p) snprintf(buf,bufsz,"%s%s",p,tmp);
    }
    else
    {
      snprintf(buf,bufsz,"/home/%s",tmp); // if someone wants to write code to lookup homedirs, please, go right ahead!
    }
    free(tmp);
  }
}

// ── Sidebar and file-op control IDs ──────────────────────────────────────────
enum {
  FC_IDC_DIR           = 0x103,
  FC_IDC_LIST          = 0x104,
  FC_IDC_PLACES        = 0x109,
  FC_IDC_PLACES_TOGGLE = 0x10A,
  FC_IDC_SB_NEW_GROUP  = 0x10B,
  FC_IDC_SB_MANAGE     = 0x10C,
  FC_WM_UPD            = WM_USER + 100,
  ID_FC_NEWFOLDER      = 0x200,
  ID_FC_RENAME,
  ID_FC_TRASH,
  ID_FC_COPY,
  ID_FC_CUT,
  ID_FC_PASTE,
  ID_FC_COPYPATH,
  ID_FC_OPENFILEMANAGER,
  ID_FC_SIDEBAR_ADD,
  ID_FC_SIDEBAR_REMOVE,
  ID_FC_GROUP_RENAME      = 0x210,
  ID_FC_GROUP_DELETE      = 0x211,
  ID_FC_GROUP_ADD_BASE    = 0x280, // 0x280..0x2FF = "add to section N" (up to 128 sections)
  ID_FC_GROUP_NEW_SECTION = 0x300,
  ID_FC_MANAGE_RENAME_BASE = 0x380, // 0x380..0x3BF = rename group N (up to 64 groups)
  ID_FC_MANAGE_DELETE_BASE = 0x3C0, // 0x3C0..0x3FF = delete group N (up to 64 groups)
};

// ── lParam sentinel helpers ───────────────────────────────────────────────────
// lParam == 1  → fixed section header (Places, Devices) — non-navigatable
// lParam > 1, [0] == '\xFF' → custom group section header (name at lParam+1)
// lParam > 1, [0] != '\xFF' → navigatable filesystem path
static inline bool lp_is_group_hdr(LPARAM lp) { return lp > 1 && ((char*)lp)[0] == '\xFF'; }
static inline bool lp_is_path(LPARAM lp)       { return lp > 1 && ((char*)lp)[0] != '\xFF'; }

// ── Splitter subclass proc ────────────────────────────────────────────────────
static int s_spl_drag_x0, s_spl_drag_w0;

static LRESULT WINAPI splitter_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  WNDPROC orig = (WNDPROC)GetWindowLongPtr(hwnd, GWLP_USERDATA);
  HWND par = GetParent(hwnd);

  switch (msg)
  {
    case WM_SETCURSOR:
      SetCursor(LoadCursor(NULL, IDC_SIZEWE));
      return TRUE;
    case WM_LBUTTONDOWN:
      {
        SetCapture(hwnd);
        POINT pt = { GET_X_LPARAM(lParam), 0 };
        ClientToScreen(hwnd, &pt);
        s_spl_drag_x0 = pt.x;
        BrowseFile_State *parms = par ? (BrowseFile_State *)GetWindowLongPtr(par, GWLP_USERDATA) : NULL;
        s_spl_drag_w0 = parms ? parms->places_width : SWELL_UI_SCALE(140);
      }
      return 0;
    case WM_MOUSEMOVE:
      if (GetCapture() == hwnd)
      {
        POINT pt = { GET_X_LPARAM(lParam), 0 };
        ClientToScreen(hwnd, &pt);
        BrowseFile_State *parms = par ? (BrowseFile_State *)GetWindowLongPtr(par, GWLP_USERDATA) : NULL;
        if (parms)
        {
          int new_w = s_spl_drag_w0 + (pt.x - s_spl_drag_x0);
          if (new_w < SWELL_UI_SCALE(60))  new_w = SWELL_UI_SCALE(60);
          if (new_w > SWELL_UI_SCALE(400)) new_w = SWELL_UI_SCALE(400);
          if (new_w != parms->places_width)
          {
            parms->places_width = new_w;
            parms->places_visible = true;
            SetWindowText(hwnd, "\xe2\x97\x80"); // ◀
            SendMessage(par, WM_SIZE, 0, 0);
          }
        }
      }
      return 0;
    case WM_LBUTTONUP:
      {
        int moved = 0;
        if (GetCapture() == hwnd)
        {
          POINT pt = { GET_X_LPARAM(lParam), 0 };
          ClientToScreen(hwnd, &pt);
          moved = abs(pt.x - s_spl_drag_x0);
          ReleaseCapture();
        }
        if (moved < 4) // click without drag: toggle collapse
          PostMessage(par, WM_COMMAND, MAKEWPARAM(FC_IDC_PLACES_TOGGLE, BN_CLICKED), (LPARAM)hwnd);
      }
      return 0;
  }
  return orig ? orig(hwnd, msg, wParam, lParam) : DefWindowProc(hwnd, msg, wParam, lParam);
}

// ── Internal file clipboard ────────────────────────────────────────────────
struct fc_clip_t {
  WDL_PtrList<char> paths;
  bool is_cut;
  fc_clip_t() : is_cut(false) {}
  void clear() { paths.Empty(true, free); is_cut = false; }
};
static fc_clip_t s_fc_clip;

// ── Input dialog (new folder / rename) ────────────────────────────────────
struct swellInputState { const char *title, *prompt, *def_text; char result[2048]; };

static LRESULT WINAPI swellInputProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  enum { IDC_PROMPT=0x100, IDC_EDIT };
  switch (uMsg) {
    case WM_CREATE:
      if (lParam) {
        SetWindowLong(hwnd,GWL_WNDPROC,(LPARAM)SwellDialogDefaultWindowProc);
        SetWindowLong(hwnd,DWL_DLGPROC,(LPARAM)swellInputProc);
        SetWindowLongPtr(hwnd,GWLP_USERDATA,lParam);
        swellInputState *st=(swellInputState*)lParam;
        if (st->title) SetWindowText(hwnd,st->title);
        SWELL_MakeSetCurParms(1,1,0,0,hwnd,false,false);
        SWELL_MakeLabel(-1,st->prompt?st->prompt:"",IDC_PROMPT,0,0,10,10,0);
        SWELL_MakeEditField(IDC_EDIT,0,0,0,0,0);
        SWELL_MakeButton(1,"OK",IDOK,0,0,0,0,0);
        SWELL_MakeButton(0,"Cancel",IDCANCEL,0,0,0,0,0);
        SWELL_MakeSetCurParms(1,1,0,0,NULL,false,false);
        SetWindowPos(hwnd,NULL,0,0,SWELL_UI_SCALE(300),SWELL_UI_SCALE(110),
                     SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOMOVE);
        if (st->def_text) SetDlgItemText(hwnd,IDC_EDIT,st->def_text);
        SendDlgItemMessage(hwnd,IDC_EDIT,EM_SETSEL,0,-1);
        SetFocus(GetDlgItem(hwnd,IDC_EDIT));
      }
    break;
    case WM_SIZE: {
      RECT r; GetClientRect(hwnd,&r);
      const int sc8=SWELL_UI_SCALE(8), rh=SWELL_UI_SCALE(22), bw=SWELL_UI_SCALE(68);
      SetWindowPos(GetDlgItem(hwnd,IDC_PROMPT),NULL,sc8,sc8,r.right-sc8*2,rh,SWP_NOACTIVATE|SWP_NOZORDER);
      SetWindowPos(GetDlgItem(hwnd,IDC_EDIT),NULL,sc8,sc8*2+rh,r.right-sc8*2,rh,SWP_NOACTIVATE|SWP_NOZORDER);
      int by=r.bottom-sc8-rh;
      SetWindowPos(GetDlgItem(hwnd,IDOK),NULL,r.right-sc8*2-bw*2,by,bw,rh,SWP_NOACTIVATE|SWP_NOZORDER);
      SetWindowPos(GetDlgItem(hwnd,IDCANCEL),NULL,r.right-sc8-bw,by,bw,rh,SWP_NOACTIVATE|SWP_NOZORDER);
    }
    break;
    case WM_COMMAND:
      if (LOWORD(wParam)==IDOK) {
        swellInputState *st=(swellInputState*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
        if (st) GetDlgItemText(hwnd,IDC_EDIT,st->result,sizeof(st->result));
        EndDialog(hwnd,1);
      } else if (LOWORD(wParam)==IDCANCEL) EndDialog(hwnd,0);
    return 0;
    case WM_KEYDOWN:
      if (wParam==VK_RETURN) { SendMessage(hwnd,WM_COMMAND,IDOK,0); return 1; }
      if (wParam==VK_ESCAPE) { EndDialog(hwnd,0); return 1; }
    return 0;
  }
  return 0;
}

// ── File operation helpers ─────────────────────────────────────────────────
static void fc_makedirs(const char *dir)
{
  struct stat st;
  if (!dir||(stat(dir,&st)==0&&S_ISDIR(st.st_mode))) return;
  char tmp[PATH_MAX]; lstrcpyn_safe(tmp,dir,sizeof(tmp));
  for (char *p=tmp+1; *p; p++) { if (*p=='/') { *p=0; mkdir(tmp,0755); *p='/'; } }
  mkdir(tmp,0755);
}

static bool fc_trash_file(const char *path)
{
  const char *home=getenv("HOME");
  if (!home||!path||!*path) return false;
  char tf[PATH_MAX], ti[PATH_MAX];
  snprintf(tf,sizeof(tf),"%s/.local/share/Trash/files",home);
  snprintf(ti,sizeof(ti),"%s/.local/share/Trash/info",home);
  fc_makedirs(tf); fc_makedirs(ti);
  const char *base=WDL_get_filepart(path);
  char dest[PATH_MAX]; snprintf(dest,sizeof(dest),"%s/%s",tf,base);
  if (access(dest,F_OK)==0) {
    const char *ext=WDL_get_fileext(base);
    int bl=ext>base?(int)(ext-base):(int)strlen(base);
    for (int n=1;n<10000;n++) {
      if (ext&&*ext) snprintf(dest,sizeof(dest),"%s/%.*s.%d%s",tf,bl,base,n,ext);
      else           snprintf(dest,sizeof(dest),"%s/%s.%d",tf,base,n);
      if (access(dest,F_OK)!=0) break;
    }
  }
  if (rename(path,dest)!=0) return false;
  char info[PATH_MAX];
  snprintf(info,sizeof(info),"%s/%s.trashinfo",ti,WDL_get_filepart(dest));
  time_t now=time(NULL); struct tm *tm_info=localtime(&now);
  char ts[32]=""; if (tm_info) strftime(ts,sizeof(ts),"%Y-%m-%dT%H:%M:%S",tm_info);
  FILE *fi=fopen(info,"w");
  if (fi) { fprintf(fi,"[Trash Info]\nPath=%s\nDeletionDate=%s\n",path,ts); fclose(fi); }
  return true;
}

static bool fc_copy_file(const char *src, const char *dst)
{
  FILE *fin=fopen(src,"rb"); if (!fin) return false;
  FILE *fout=fopen(dst,"wb"); if (!fout) { fclose(fin); return false; }
  char buf[65536]; size_t n; bool ok=true;
  while ((n=fread(buf,1,sizeof(buf),fin))>0) if (!fwrite(buf,1,n,fout)){ok=false;break;}
  fclose(fin); fclose(fout);
  if (!ok) unlink(dst);
  return ok;
}

static void fc_set_system_clip(HWND hwnd, WDL_PtrList<char> &paths, bool is_cut)
{
  size_t total=sizeof(DROPFILES)+1;
  for (int i=0;i<paths.GetSize();i++) total+=strlen(paths.Get(i))+1;
  HANDLE h=GlobalAlloc(GMEM_MOVEABLE,total);
  if (!h) return;
  DROPFILES *df=(DROPFILES*)GlobalLock(h);
  if (!df) { GlobalFree(h); return; }
  df->pFiles=sizeof(DROPFILES); df->pt.x=df->pt.y=0; df->fNC=FALSE; df->fWide=FALSE;
  char *p=(char*)(df+1);
  for (int i=0;i<paths.GetSize();i++) { const char *s=paths.Get(i); size_t l=strlen(s)+1; memcpy(p,s,l); p+=l; }
  *p=0; GlobalUnlock(h);
  OpenClipboard(hwnd); EmptyClipboard(); SetClipboardData(CF_HDROP,h);
  // x-special/gnome-copied-files for Dolphin cut/copy awareness
  char gnome[65536];
  snprintf(gnome,sizeof(gnome),"%s",is_cut?"cut":"copy");
  for (int i=0;i<paths.GetSize();i++) snprintf_append(gnome,sizeof(gnome),"\nfile://%s",paths.Get(i));
  snprintf_append(gnome,sizeof(gnome),"\n");
  UINT gfmt=RegisterClipboardFormat("x-special/gnome-copied-files");
  if (gfmt) {
    size_t gsl=strlen(gnome)+1; HANDLE gh=GlobalAlloc(GMEM_MOVEABLE,gsl);
    if (gh) { char *gp=(char*)GlobalLock(gh); if(gp){memcpy(gp,gnome,gsl);GlobalUnlock(gh);} SetClipboardData(gfmt,gh); }
  }
  CloseClipboard();
}

// forward declarations for sidebar helpers below
static void browse_tree_free(HWND tv, HTREEITEM item);
static void browse_populate_places(HWND tv);
static HTREEITEM browse_tree_insert(HWND tv, HTREEITEM parent, const char *label, const char *path);

static void fc_refresh_sidebar(HWND hwnd)
{
  HWND places=GetDlgItem(hwnd,FC_IDC_PLACES);
  if (!places) return;
  browse_tree_free(places,TreeView_GetRoot(places));
  TreeView_DeleteAllItems(places);
  browse_populate_places(places);
}

static void fc_bookmarks_path(char *out, size_t sz)
{
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) { snprintf(out, sz, "%s/gtk-3.0/bookmarks", xdg); return; }
  const char *home = getenv("HOME");
  if (home && *home) snprintf(out, sz, "%s/.config/gtk-3.0/bookmarks", home);
  else out[0] = 0;
}

static void fc_url_encode_path(const char *path, char *out, size_t outsz)
{
  char *e = out, *end = out + outsz - 1;
  for (const char *p = path; *p && e < end; p++)
  {
    unsigned char c = (unsigned char)*p;
    if (c == '/' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
      *e++ = c;
    else if (e + 3 <= end) { sprintf(e, "%%%02X", c); e += 3; }
  }
  *e = 0;
}

static void fc_sidebar_add_bm(HWND hwnd, const char *path, const char *label=NULL)
{
  if (!path||!*path) return;
  char bmpath[PATH_MAX]; fc_bookmarks_path(bmpath, sizeof(bmpath));
  if (!bmpath[0]) return;
  char enc[PATH_MAX*3]; fc_url_encode_path(path, enc, sizeof(enc));
  char uri[PATH_MAX*3+8]; snprintf(uri,sizeof(uri),"file://%s",enc);
  FILE *f=fopen(bmpath,"r"); bool exists=false;
  if (f) {
    char line[PATH_MAX*3+64];
    while (fgets(line,sizeof(line),f))
      if (strncmp(line,uri,strlen(uri))==0) { exists=true; break; }
    fclose(f);
  }
  if (exists) return;
  f=fopen(bmpath,"a");
  if (f) { fprintf(f,"%s %s\n",uri,label&&*label?label:WDL_get_filepart(path)); fclose(f); }
  fc_refresh_sidebar(hwnd);
}

static void fc_sidebar_remove_bm(HWND hwnd, const char *path)
{
  if (!path||!*path) return;
  char bmpath[PATH_MAX]; fc_bookmarks_path(bmpath, sizeof(bmpath));
  if (!bmpath[0]) return;
  char enc[PATH_MAX*3]; fc_url_encode_path(path, enc, sizeof(enc));
  char uri[PATH_MAX*3+8]; snprintf(uri,sizeof(uri),"file://%s",enc);
  size_t ulen=strlen(uri);
  WDL_PtrList<char> lines; FILE *f=fopen(bmpath,"r"); bool found=false;
  if (f) {
    char line[PATH_MAX];
    while (fgets(line,sizeof(line),f)) {
      size_t l=strlen(line); while(l>0&&(line[l-1]=='\n'||line[l-1]=='\r')) line[--l]=0;
      if (strncmp(line,uri,ulen)==0&&(line[ulen]==0||line[ulen]==' ')) found=true;
      else lines.Add(strdup(line));
    }
    fclose(f);
  }
  if (found) {
    f=fopen(bmpath,"w");
    if (f) { for (int i=0;i<lines.GetSize();i++) fprintf(f,"%s\n",lines.Get(i)); fclose(f); }
    fc_refresh_sidebar(hwnd);
  }
  lines.Empty(true,free);
}

// ── Named bookmark groups ─────────────────────────────────────────────────────

static void fc_groups_path(char *out, size_t sz)
{
  const char *h = getenv("HOME");
  if (h) snprintf(out, sz, "%s/.config/reaper-swell-groups", h);
  else out[0] = 0;
}

static void fc_groups_get_sections(WDL_PtrList<char> &out)
{
  char gp[PATH_MAX]; fc_groups_path(gp, sizeof(gp));
  FILE *f = fopen(gp, "r"); if (!f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] != '[') continue;
    char *e = strchr(line+1, ']'); if (!e) continue; *e = 0;
    out.Add(strdup(line+1));
  }
  fclose(f);
}

static void browse_populate_groups(HWND tv)
{
  char gp[PATH_MAX]; fc_groups_path(gp, sizeof(gp));
  FILE *f = fopen(gp, "r"); if (!f) return;
  HTREEITEM cur_hdr = NULL;
  char line[PATH_MAX*3 + 256];
  while (fgets(line, sizeof(line), f)) {
    size_t l = strlen(line);
    while (l > 0 && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l] = 0;
    if (!line[0] || line[0] == '#') continue;
    if (line[0] == '[') {
      char *e = strchr(line+1, ']'); if (!e) continue; *e = 0;
      const char *name = line+1;
      TV_INSERTSTRUCT tis; memset(&tis, 0, sizeof(tis));
      tis.hParent = TVI_ROOT; tis.hInsertAfter = TVI_LAST;
      tis.item.mask = TVIF_TEXT|TVIF_PARAM|TVIF_CHILDREN;
      tis.item.pszText = (char*)name;
      char *lp = (char*)malloc(strlen(name)+2);
      lp[0] = '\xFF'; strcpy(lp+1, name);
      tis.item.lParam = (LPARAM)lp;
      tis.item.cChildren = 1;
      cur_hdr = TreeView_InsertItem(tv, &tis);
      if (cur_hdr) TreeView_Expand(tv, cur_hdr, TVE_EXPAND);
    } else if (strncmp(line, "file://", 7) == 0 && cur_hdr) {
      char *p = line+7, *end = p;
      while (*end && *end != ' ' && *end != '\t') end++;
      char *lbl = *end ? end+1 : NULL;
      *end = 0;
      if (lbl) { char *le=lbl; while(*le&&*le!='\n'&&*le!='\r') le++; *le=0; }
      char dec[PATH_MAX]; char *r=p, *w=dec;
      while (*r && w < dec+sizeof(dec)-1) {
        if (*r=='%'&&isxdigit((unsigned char)r[1])&&isxdigit((unsigned char)r[2])) {
          char hex[3]={r[1],r[2],0}; *w++=(char)strtol(hex,NULL,16); r+=3;
        } else *w++=*r++;
      }
      *w = 0;
      const char *label = (lbl&&*lbl) ? lbl : WDL_get_filepart(dec);
      browse_tree_insert(tv, cur_hdr, label, dec);
    }
  }
  fclose(f);
}

static void fc_group_add(HWND hwnd, const char *section, const char *path, const char *label)
{
  if (!section||!*section||!path||!*path) return;
  char gp[PATH_MAX]; fc_groups_path(gp, sizeof(gp));
  char enc[PATH_MAX*3]; fc_url_encode_path(path, enc, sizeof(enc));
  char uri[PATH_MAX*3+8]; snprintf(uri, sizeof(uri), "file://%s", enc);
  const char *lbl = (label&&*label) ? label : WDL_get_filepart(path);
  WDL_PtrList<char> lines;
  FILE *f = fopen(gp, "r");
  if (f) {
    char line[PATH_MAX*3+256];
    while (fgets(line, sizeof(line), f)) {
      size_t l=strlen(line); while(l>0&&(line[l-1]=='\n'||line[l-1]=='\r')) line[--l]=0;
      lines.Add(strdup(line));
    }
    fclose(f);
  }
  f = fopen(gp, "w"); if (!f) { lines.Empty(true,free); return; }
  bool section_found=false, added=false, in_sec=false;
  for (int i=0; i<lines.GetSize(); i++) {
    const char *l = lines.Get(i);
    if (l[0]=='[') {
      char *dup=strdup(l+1); char *e=strchr(dup,']'); if(e)*e=0;
      bool this_sec = !strcmp(dup, section);
      free(dup);
      if (in_sec && !added) { fprintf(f,"%s %s\n",uri,lbl); added=true; }
      if (this_sec) { section_found=true; in_sec=true; } else in_sec=false;
    }
    fprintf(f,"%s\n",l);
  }
  if (!added) {
    if (!section_found) fprintf(f,"\n[%s]\n",section);
    fprintf(f,"%s %s\n",uri,lbl);
  }
  fclose(f); lines.Empty(true,free);
  fc_refresh_sidebar(hwnd);
}

static void fc_group_remove_item(HWND hwnd, const char *path)
{
  if (!path||!*path) return;
  char gp[PATH_MAX]; fc_groups_path(gp, sizeof(gp));
  char enc[PATH_MAX*3]; fc_url_encode_path(path, enc, sizeof(enc));
  char uri[PATH_MAX*3+8]; snprintf(uri, sizeof(uri), "file://%s", enc);
  size_t ulen=strlen(uri);
  WDL_PtrList<char> lines; bool found=false;
  FILE *f=fopen(gp,"r");
  if (f) {
    char line[PATH_MAX*3+256];
    while(fgets(line,sizeof(line),f)) {
      size_t l=strlen(line); while(l>0&&(line[l-1]=='\n'||line[l-1]=='\r')) line[--l]=0;
      if (strncmp(line,uri,ulen)==0&&(line[ulen]==0||line[ulen]==' '||line[ulen]=='\t')) found=true;
      else lines.Add(strdup(line));
    }
    fclose(f);
  }
  if (found) {
    f=fopen(gp,"w");
    if(f){for(int i=0;i<lines.GetSize();i++)fprintf(f,"%s\n",lines.Get(i));fclose(f);}
    fc_refresh_sidebar(hwnd);
  }
  lines.Empty(true,free);
}

static void fc_group_rename_section(HWND hwnd, const char *old_name, const char *new_name)
{
  if (!old_name||!new_name||!*old_name||!*new_name) return;
  char gp[PATH_MAX]; fc_groups_path(gp, sizeof(gp));
  WDL_PtrList<char> lines; bool found=false;
  FILE *f=fopen(gp,"r");
  if(f){char line[512];while(fgets(line,sizeof(line),f)){size_t l=strlen(line);while(l>0&&(line[l-1]=='\n'||line[l-1]=='\r'))line[--l]=0;lines.Add(strdup(line));}fclose(f);}
  f=fopen(gp,"w"); if(!f){lines.Empty(true,free);return;}
  for(int i=0;i<lines.GetSize();i++){
    const char *l=lines.Get(i);
    if(l[0]=='['){char *dup=strdup(l+1);char *e=strchr(dup,']');if(e)*e=0;if(!strcmp(dup,old_name)){fprintf(f,"[%s]\n",new_name);found=true;free(dup);continue;}free(dup);}
    fprintf(f,"%s\n",l);
  }
  fclose(f); lines.Empty(true,free);
  if(found) fc_refresh_sidebar(hwnd);
}

static void fc_group_delete_section(HWND hwnd, const char *section)
{
  if (!section||!*section) return;
  char gp[PATH_MAX]; fc_groups_path(gp, sizeof(gp));
  WDL_PtrList<char> lines;
  FILE *f=fopen(gp,"r");
  if(f){char line[PATH_MAX*3+256];while(fgets(line,sizeof(line),f)){size_t l=strlen(line);while(l>0&&(line[l-1]=='\n'||line[l-1]=='\r'))line[--l]=0;lines.Add(strdup(line));}fclose(f);}
  f=fopen(gp,"w"); if(!f){lines.Empty(true,free);return;}
  bool in_sec=false,found=false;
  for(int i=0;i<lines.GetSize();i++){
    const char *l=lines.Get(i);
    if(l[0]=='['){char *dup=strdup(l+1);char *e=strchr(dup,']');if(e)*e=0;bool hit=!strcmp(dup,section);free(dup);if(hit){in_sec=true;found=true;continue;}else in_sec=false;}
    if(!in_sec) fprintf(f,"%s\n",l);
  }
  fclose(f); lines.Empty(true,free);
  if(found) fc_refresh_sidebar(hwnd);
}

// ── Places sidebar (tree) ────────────────────────────────────────────────────

static int cmp_strcase(const void *a, const void *b)
{
  return strcasecmp(*(const char **)a, *(const char **)b);
}

static bool browse_has_subdirs(const char *path)
{
  DIR *d = opendir(path);
  if (!d) return false;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL)
  {
    if (ent->d_name[0] == '.') continue;
    bool isdir = false;
#ifdef DT_DIR
    if (ent->d_type == DT_DIR) isdir = true;
    else if (ent->d_type == DT_UNKNOWN || ent->d_type == DT_LNK)
#endif
    {
      char sub[PATH_MAX];
      snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
      struct stat st;
      if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) isdir = true;
    }
    if (isdir) { closedir(d); return true; }
  }
  closedir(d);
  return false;
}

static HTREEITEM browse_tree_insert_header(HWND tv, const char *label)
{
  TV_INSERTSTRUCT tis;
  memset(&tis, 0, sizeof(tis));
  tis.hParent = TVI_ROOT;
  tis.hInsertAfter = TVI_LAST;
  tis.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
  tis.item.pszText = (char*)label;
  tis.item.lParam = 1; // sentinel: non-navigatable section header
  tis.item.cChildren = 1; // has children (populated inline, show expand arrow)
  return TreeView_InsertItem(tv, &tis);
}

static HTREEITEM browse_tree_insert(HWND tv, HTREEITEM parent, const char *label, const char *path)
{
  struct stat st;
  if (!path || !*path || stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return NULL;
  TV_INSERTSTRUCT tis;
  memset(&tis, 0, sizeof(tis));
  tis.hParent = parent ? parent : TVI_ROOT;
  tis.hInsertAfter = TVI_LAST;
  tis.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
  tis.item.pszText = (char*)label;
  tis.item.lParam = (LPARAM)strdup(path);
  tis.item.cChildren = browse_has_subdirs(path) ? 1 : 0;
  return TreeView_InsertItem(tv, &tis);
}

static void browse_tree_expand(HWND tv, HTREEITEM hitem, const char *path)
{
  DIR *d = opendir(path);
  if (!d) return;
  WDL_PtrList<char> names;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL)
  {
    if (ent->d_name[0] == '.') continue;
    bool isdir = false;
#ifdef DT_DIR
    if (ent->d_type == DT_DIR) isdir = true;
    else if (ent->d_type == DT_UNKNOWN || ent->d_type == DT_LNK)
#endif
    {
      char sub[PATH_MAX];
      snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
      struct stat st;
      if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) isdir = true;
    }
    if (isdir) names.Add(strdup(ent->d_name));
  }
  closedir(d);
  qsort(names.GetList(), names.GetSize(), sizeof(char*), cmp_strcase);
  for (int i = 0; i < names.GetSize(); i++)
  {
    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/%s", path, names.Get(i));
    browse_tree_insert(tv, hitem, names.Get(i), sub);
    free(names.Get(i));
  }
}

static void browse_tree_free(HWND tv, HTREEITEM item)
{
  while (item)
  {
    HTREEITEM child = TreeView_GetChild(tv, item);
    if (child) browse_tree_free(tv, child);
    TVITEM ti;
    memset(&ti, 0, sizeof(ti));
    ti.mask = TVIF_PARAM | TVIF_HANDLE;
    ti.hItem = item;
    if (TreeView_GetItem(tv, &ti) && ti.lParam > 1) free((char*)ti.lParam);
    item = TreeView_GetNextSibling(tv, item);
  }
}

static void browse_populate_devices(HWND tv)
{
  FILE *f = fopen("/proc/mounts", "r");
  if (!f) return;

  static const char *skip_prefix[] = {
    "/", "/boot", "/sys", "/proc", "/dev", "/run", "/snap", "/tmp", NULL
  };

  WDL_PtrList<char> seen_mnts;
  WDL_PtrList<char> labels;
  WDL_PtrList<char> paths;

  char line[PATH_MAX + 256];
  while (fgets(line, sizeof(line), f))
  {
    char dev[256], mnt[PATH_MAX], fstype[64];
    if (sscanf(line, "%255s %4095s %63s", dev, mnt, fstype) < 3) continue;
    if (strncmp(dev, "/dev/", 5) != 0) continue;

    bool skip = false;
    for (int i = 0; skip_prefix[i] && !skip; i++)
    {
      size_t n = strlen(skip_prefix[i]);
      if (!strcmp(mnt, skip_prefix[i]) || (!strncmp(mnt, skip_prefix[i], n) && mnt[n] == '/'))
        skip = true;
    }
    if (skip) continue;

    bool dup = false;
    for (int i = 0; i < seen_mnts.GetSize() && !dup; i++)
      if (!strcmp(seen_mnts.Get(i), mnt)) dup = true;
    if (dup) continue;

    struct stat st;
    if (stat(mnt, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

    seen_mnts.Add(strdup(mnt));
    const char *lbl = WDL_get_filepart(mnt);
    labels.Add(strdup(lbl && *lbl ? lbl : mnt));
    paths.Add(strdup(mnt));
  }
  fclose(f);

  if (labels.GetSize() > 0)
  {
    HTREEITEM hdr = browse_tree_insert_header(tv, "Devices");
    for (int i = 0; i < labels.GetSize(); i++)
      browse_tree_insert(tv, hdr, labels.Get(i), paths.Get(i));
    TreeView_Expand(tv, hdr, TVE_EXPAND);
  }
  seen_mnts.Empty(true, free);
  labels.Empty(true, free);
  paths.Empty(true, free);
}

static void browse_populate_bookmarks(HWND tv)
{
  char bmpath[PATH_MAX];
  fc_bookmarks_path(bmpath, sizeof(bmpath));
  if (!bmpath[0]) return;
  FILE *f = fopen(bmpath, "r");
  if (!f) return;

  // collect entries first so we only create the header if there are any
  WDL_PtrList<char> labels, paths;
  char line[PATH_MAX + 256];
  while (fgets(line, sizeof(line), f))
  {
    if (strncmp(line, "file://", 7) != 0) continue;
    char *p = line + 7;
    char *end = p;
    while (*end && *end != ' ' && *end != '\n' && *end != '\r') end++;
    char *name = (*end == ' ') ? end + 1 : NULL;
    *end = 0;
    if (name) { char *ne = name; while (*ne && *ne != '\n' && *ne != '\r') ne++; *ne = 0; }
    char *r = p, *w = p;
    while (*r)
    {
      if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2]))
      {
        char hex[3] = { r[1], r[2], 0 };
        *w++ = (char)strtol(hex, NULL, 16);
        r += 3;
      }
      else *w++ = *r++;
    }
    *w = 0;
    if (!*p) continue;
    const char *label = (name && *name) ? name : WDL_get_filepart(p);
    labels.Add(strdup(label));
    paths.Add(strdup(p));
  }
  fclose(f);

  if (labels.GetSize() > 0)
  {
    HTREEITEM hdr = browse_tree_insert_header(tv, "Bookmarks");
    for (int i = 0; i < labels.GetSize(); i++)
      browse_tree_insert(tv, hdr, labels.Get(i), paths.Get(i));
    TreeView_Expand(tv, hdr, TVE_EXPAND);
  }
  labels.Empty(true, free);
  paths.Empty(true, free);
}

static void browse_populate_places(HWND tv)
{
  const char *home = getenv("HOME");
  if (!home || !*home) return;

  WDL_PtrList<char> seen; // deduplicate paths
  HTREEITEM places_hdr = browse_tree_insert_header(tv, "Places");

  auto try_add = [&](const char *label, const char *path) {
    if (!path || !*path) return;
    for (int i = 0; i < seen.GetSize(); i++)
      if (!strcmp(seen.Get(i), path)) return;
    if (browse_tree_insert(tv, places_hdr, label, path))
      seen.Add(strdup(path));
  };
  try_add("Home", home);

  static const struct { const char *key; const char *label; } xdg_map[] = {
    { "XDG_DESKTOP_DIR",   "Desktop"   },
    { "XDG_DOWNLOAD_DIR",  "Downloads" },
    { "XDG_DOCUMENTS_DIR", "Documents" },
    { "XDG_MUSIC_DIR",     "Music"     },
    { "XDG_PICTURES_DIR",  "Pictures"  },
    { "XDG_VIDEOS_DIR",    "Videos"    },
    { NULL, NULL }
  };

  char fpath[PATH_MAX];
  snprintf(fpath, sizeof(fpath), "%s/.config/user-dirs.dirs", home);
  FILE *f = fopen(fpath, "r");
  bool got_xdg = false;
  if (f)
  {
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof(line), f))
    {
      if (line[0] == '#' || line[0] == '\n') continue;
      char *eq = strchr(line, '=');
      if (!eq) continue;
      *eq = 0;
      char *val = eq + 1;
      if (*val == '"') val++;
      char *vend = val;
      while (*vend && *vend != '"' && *vend != '\n' && *vend != '\r') vend++;
      *vend = 0;
      char expanded[PATH_MAX];
      if (strncmp(val, "$HOME", 5) == 0)
        snprintf(expanded, sizeof(expanded), "%s%s", home, val + 5);
      else
        lstrcpyn_safe(expanded, val, sizeof(expanded));
      for (int i = 0; xdg_map[i].key; i++)
        if (!strcmp(line, xdg_map[i].key))
        {
          try_add(xdg_map[i].label, expanded);
          got_xdg = true;
          break;
        }
    }
    fclose(f);
  }
  if (!got_xdg)
  {
    static const char *std[] = { "Desktop","Downloads","Documents","Music","Pictures","Videos",NULL };
    for (int i = 0; std[i]; i++)
    {
      snprintf(fpath, sizeof(fpath), "%s/%s", home, std[i]);
      try_add(std[i], fpath);
    }
  }

  // Trash — at bottom of Places
  char trash[PATH_MAX];
  snprintf(trash, sizeof(trash), "%s/.local/share/Trash/files", home);
  try_add("Trash", trash);

  seen.Empty(true, free);
  TreeView_Expand(tv, places_hdr, TVE_EXPAND);

  // User-defined named groups
  browse_populate_groups(tv);

  // Devices section (reads /proc/mounts)
  browse_populate_devices(tv);
}

// ── end Places sidebar ───────────────────────────────────────────────────────

static LRESULT WINAPI swellFileSelectProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  enum { IDC_EDIT=0x100, IDC_LABEL, IDC_CHILD, IDC_DIR, IDC_LIST, IDC_EXT, IDC_PARENTBUTTON, IDC_FILTER, ID_SHOW_HIDDEN,
         IDC_PLACES=FC_IDC_PLACES, IDC_PLACES_TOGGLE=FC_IDC_PLACES_TOGGLE };
  enum { WM_UPD=WM_USER+100 };
  const int maxPathLen = 2048;
  const char *multiple_files = "(multiple files)";
  switch (uMsg)
  {
    case WM_CREATE:
      if (lParam)  // swell-specific
      {
        SetWindowLong(hwnd,GWL_WNDPROC,(LPARAM)SwellDialogDefaultWindowProc);
        SetWindowLong(hwnd,DWL_DLGPROC,(LPARAM)swellFileSelectProc);

        SetWindowLong(hwnd,GWL_STYLE, GetWindowLong(hwnd,GWL_STYLE)|WS_THICKFRAME);

        SetWindowLongPtr(hwnd,GWLP_USERDATA,lParam);
        BrowseFile_State *parms = (BrowseFile_State *)lParam;

        char tmp[1024];
        recent_read();

        recent_add_tmp(parms->initialdir);

        if (parms->initialfile && *parms->initialfile != '.')
        {
          lstrcpyn_safe(tmp,parms->initialfile,sizeof(tmp));
          WDL_remove_filepart(tmp);
          recent_add_tmp(tmp);
        }

        if (parms->caption) SetWindowText(hwnd,parms->caption);

        SWELL_MakeSetCurParms(1,1,0,0,hwnd,false,false);

        HWND edit = SWELL_MakeEditField(IDC_EDIT, 0,0,0,0,  0);
        SWELL_MakeButton(0,
              parms->mode == BrowseFile_State::OPENDIR ? "Choose directory" :
              parms->mode == BrowseFile_State::SAVE ? "Save" : "Open",
              IDOK,0,0,0,0, 0);

        SWELL_MakeButton(0, "Cancel", IDCANCEL,0,0,0,0, 0);
        HWND dir = SWELL_MakeCombo(IDC_DIR, 0,0,0,0, 0);
        SWELL_MakeButton(0, "..", IDC_PARENTBUTTON, 0,0,0,0, 0);
        SWELL_MakeEditField(IDC_FILTER, 0,0,0,0,  0);

        const char *ent = parms->mode == BrowseFile_State::OPENDIR ? "dir_browser" : "file_browser";
        GetPrivateProfileString(".swell",ent,"", tmp,sizeof(tmp),"");
        int x=0,y=0,w=0,h=0, c1=0,c2=0,c3=0,extraflag=0,sbw_ini=0;
        int flag = SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER;
        if (tmp[0] &&
            sscanf(tmp,"%d %d %d %d %d %d %d %d %d",&x,&y,&w,&h,&c1,&c2,&c3,&extraflag,&sbw_ini) >= 4)
          flag &= ~SWP_NOMOVE;
        if (w < 100) w=SWELL_UI_SCALE(600);
        if (h < 100) h=SWELL_UI_SCALE(400);
        if (extraflag&1) parms->show_hidden=true;
        if (extraflag&2) parms->places_visible=false;
        if (sbw_ini >= SWELL_UI_SCALE(60) && sbw_ini <= SWELL_UI_SCALE(400)) parms->places_width=sbw_ini;

        if (c1 + c2 + c3 < w/2)
        {
          c1=SWELL_UI_SCALE(280);
          c2=SWELL_UI_SCALE(120);
          c3=SWELL_UI_SCALE(140);
        }

        HWND list = SWELL_MakeControl("",IDC_LIST,"SysListView32",LVS_REPORT|LVS_SHOWSELALWAYS|
              (parms->mode == BrowseFile_State::OPENMULTI ? 0 : LVS_SINGLESEL)|
              LVS_OWNERDATA|WS_BORDER|WS_TABSTOP,0,0,0,0,0);
        if (list)
        {
          LVCOLUMN c={LVCF_TEXT|LVCF_WIDTH, 0, c1, (char*)"Filename" };
          ListView_InsertColumn(list,0,&c);
          c.cx = c2;
          c.pszText = (char*) "Size";
          ListView_InsertColumn(list,1,&c);
          c.cx = c3;
          c.pszText = (char*) "Date";
          ListView_InsertColumn(list,2,&c);
          HWND hdr = ListView_GetHeader(list);
          HDITEM hi;
          memset(&hi,0,sizeof(hi));
          hi.mask = HDI_FORMAT;
          hi.fmt = parms->sortrev ? HDF_SORTDOWN : HDF_SORTUP;
          Header_SetItem(hdr,parms->sortcol,&hi);
        }
        {
          HWND places = SWELL_MakeControl("", IDC_PLACES, "SysTreeView32", WS_BORDER, 0,0,0,0,0);
          if (places)
          {
            static HFONT s_places_font;
            if (!s_places_font)
              s_places_font = CreateFont(-(g_swell_ctheme.default_font_size + 2),
                0,0,0,FW_NORMAL,0,0,0,0,0,0,0,0,g_swell_deffont_face);
            if (s_places_font) SendMessage(places, WM_SETFONT, (WPARAM)s_places_font, 0);
            browse_populate_places(places);
          }
          SWELL_MakeButton(0, "+ Group", FC_IDC_SB_NEW_GROUP, 0,0,0,0, 0);
          SWELL_MakeButton(0, "\xe2\x9a\x99", FC_IDC_SB_MANAGE, 0,0,0,0, 0);
          SWELL_MakeButton(0, parms->places_visible ? "\xe2\x97\x80" : "\xe2\x96\xb6",
                           IDC_PLACES_TOGGLE, 0,0,0,0, 0);
          {
            HWND tog = GetDlgItem(hwnd, IDC_PLACES_TOGGLE);
            if (tog)
            {
              WNDPROC orig = (WNDPROC)SetWindowLongPtr(tog, GWL_WNDPROC, (LONG_PTR)splitter_wndproc);
              SetWindowLongPtr(tog, GWLP_USERDATA, (LONG_PTR)orig);
            }
          }
        }

        HWND extlist = (parms->extlist && *parms->extlist) ? SWELL_MakeCombo(IDC_EXT, 0,0,0,0, CBS_DROPDOWNLIST) : NULL;
        if (extlist)
        {
          const char *p = parms->extlist;
          while (*p)
          {
            const char *rd=p;
            p += strlen(p)+1;
            if (!*p) break;
            int a = SendMessage(extlist,CB_ADDSTRING,0,(LPARAM)rd);
            SendMessage(extlist,CB_SETITEMDATA,a,(LPARAM)p);
            p += strlen(p)+1;
          }

          int sel = 0;
          if (parms->initialfile && *parms->initialfile)
          {
            sel = ext_valid_for_extlist(WDL_get_fileext(parms->initialfile), parms->extlist);
            if (sel<0) sel=0;
          }
          SendMessage(extlist,CB_SETCURSEL,sel,0);
        }

        SWELL_MakeLabel(-1,parms->mode == BrowseFile_State::OPENDIR ? "Directory: " : "File:",IDC_LABEL, 0,0,0,0, 0); 
        
        if (BFSF_Templ_dlgid && BFSF_Templ_dlgproc)
        {
          HWND dlg = SWELL_CreateDialog(BFSF_Templ_reshead, BFSF_Templ_dlgid, hwnd, BFSF_Templ_dlgproc, 0);
          if (dlg) SetWindowLong(dlg,GWL_ID,IDC_CHILD);
          BFSF_Templ_dlgproc=0;
          BFSF_Templ_dlgid=0;
        }

        SWELL_MakeSetCurParms(1,1,0,0,NULL,false,false);

        if (edit && dir)
        {
          char buf[maxPathLen];
          const char *filepart = "";
          if (parms->initialfile && *parms->initialfile && *parms->initialfile != '.')
          { 
            lstrcpyn_safe(buf,parms->initialfile,sizeof(buf));
            char *p = (char *)WDL_get_filepart(buf);
            if (p > buf) 
            { 
              p[-1]=0; 
              filepart = p; 
            }
            else
            {
              filepart = parms->initialfile;
              goto get_dir;
            }
          }
          else 
          {
get_dir:
            if (parms->initialdir && *parms->initialdir && strcmp(parms->initialdir,".")) 
            {
              lstrcpyn_safe(buf,parms->initialdir,sizeof(buf));
            }
            else if (!getcwd(buf,sizeof(buf)))
              buf[0]=0;
          }

          SetWindowText(edit,filepart);
          SendMessage(hwnd, WM_UPD, IDC_DIR, (LPARAM)buf);
        }

        if (list) SetWindowPos(list,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        SetWindowPos(hwnd,NULL,x,y, w,h, flag);
        SendMessage(hwnd,WM_UPD,1,0);
        SendMessage(edit,EM_SETSEL,0,(LPARAM)-1);
        SetFocus(edit);
      }
    break;
    case WM_DESTROY:
      {
        HWND places = GetDlgItem(hwnd, IDC_PLACES);
        if (places) browse_tree_free(places, TreeView_GetRoot(places));
        BrowseFile_State *parms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
        if (parms)
        {
          RECT r;
          GetWindowRect(hwnd,&r);
          HWND list = GetDlgItem(hwnd,IDC_LIST);
          const int c1 = ListView_GetColumnWidth(list,0);
          const int c2 = ListView_GetColumnWidth(list,1);
          const int c3 = ListView_GetColumnWidth(list,2);
          char tmp[128];
          int extraflag=0;
          if (parms->show_hidden)    extraflag|=1;
          if (!parms->places_visible) extraflag|=2;
          snprintf(tmp,sizeof(tmp),"%d %d %d %d %d %d %d %d %d",
                   r.left,r.top,r.right-r.left,r.bottom-r.top,c1,c2,c3,extraflag,parms->places_width);
          const char *ent = parms->mode == BrowseFile_State::OPENDIR ? "dir_browser" : "file_browser";
          WritePrivateProfileString(".swell",ent, tmp, "");
        }
      }
    break;
    case WM_UPD:
      switch (wParam)
      {
        case IDC_DIR: // update directory combo box -- destroys buffer pointed to by lParam
          if (lParam)
          {
            char *path = (char*)lParam;
            HWND combo=GetDlgItem(hwnd,IDC_DIR);
            SendMessage(combo,CB_RESETCONTENT,0,0);
            WDL_remove_trailing_dirchars(path);
            while (path[0]) 
            {
              SendMessage(combo,CB_ADDSTRING,0,(LPARAM)path);
              WDL_remove_filepart(path);
              WDL_remove_trailing_dirchars(path);
            }
            SendMessage(combo,CB_ADDSTRING,0,(LPARAM)"/");
            recent_addtocb(combo);
            SendMessage(combo,CB_SETCURSEL,0,0);
          } 
        break;
        case IDC_EXT:
        case 1:
        {
          BrowseFile_State *parms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
          if (parms)
          {
            SetDlgItemText(hwnd,IDC_FILTER,"");
            KillTimer(hwnd,1);

            char buf[maxPathLen];
            const char *filt = NULL;
            HWND ext = GetDlgItem(hwnd,IDC_EXT);
            if (ext)
            {
              LRESULT aidx = SendMessage(ext,CB_GETCURSEL,0,0);
              if (aidx != CB_ERR)
              {
                filt = (const char *)SendMessage(ext,CB_GETITEMDATA,aidx,0);
                if (wParam == IDC_EXT && parms->extlist && *parms->extlist)
                {
                  GetDlgItemText(hwnd,IDC_EDIT,buf,sizeof(buf));

                  if (buf[0])
                  {
                    const char *erd = filt;
                    if (*erd == '*' && erd[1] == '.' && erd[2] && erd[2] != '*')
                    {
                      const char *a = (erd+=1);
                      while (*erd && *erd != ';') erd++;
                      if (erd > a+1)
                      {
                        if (ext_valid_for_extlist(WDL_get_fileext(buf),parms->extlist)>=0)
                        {
                          WDL_remove_fileext(buf);
                        }
                        else
                        {
                          char *p = buf;
                          while (*p) p++;
                          while (p > buf && (p[-1] == ' ' || p[-1] == '.' || p[-1] == '/' || p[-1] == '\\')) p--;
                          *p=0;
                        }
                        if (buf[0])
                        {
                          snprintf_append(buf,sizeof(buf),"%.*s",(int)(erd-a),a);
                          SetDlgItemText(hwnd,IDC_EDIT,buf);
                        }
                      }
                    }
                  }
                }
              }
            }

            GetDlgItemText(hwnd,IDC_DIR,buf,sizeof(buf));
            preprocess_user_path(buf,sizeof(buf));

            if (buf[0]) parms->scan_path(buf, filt, parms->mode == BrowseFile_State::OPENDIR);
            else parms->viewlist_clear();
            HWND list = GetDlgItem(hwnd,IDC_LIST);
            ListView_SetItemCount(list, 0); // clear selection

            parms->viewlist_sort("");
            ListView_SetItemCount(list, parms->viewlist.GetSize());
            ListView_RedrawItems(list,0, parms->viewlist.GetSize());
          }
        }
        break;
      }
    break;
    case WM_GETMINMAXINFO:
      {
        LPMINMAXINFO p=(LPMINMAXINFO)lParam;
        p->ptMinTrackSize.x = 300;
        p->ptMinTrackSize.y = 300;
      }
    break;
    case WM_SIZE:
      {
        BrowseFile_State *parms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
        // reposition controls
        RECT r;
        GetClientRect(hwnd,&r);
        const int buth = SWELL_UI_SCALE(24), cancelbutw = SWELL_UI_SCALE(50), okbutw = SWELL_UI_SCALE(parms->mode == BrowseFile_State::OPENDIR ? 120 : 50);
        const int xborder = SWELL_UI_SCALE(4), yborder=SWELL_UI_SCALE(8);
        const int fnh = SWELL_UI_SCALE(20), fnlblw = SWELL_UI_SCALE(parms->mode == BrowseFile_State::OPENDIR ? 70 : 50);
        const int ypad = SWELL_UI_SCALE(4);

        int ypos = r.bottom - ypad - buth;
        int xpos = r.right;
        SetWindowPos(GetDlgItem(hwnd,IDCANCEL), NULL, xpos -= cancelbutw + xborder, ypos, cancelbutw,buth, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd,IDOK), NULL, xpos -= okbutw + xborder, ypos, okbutw,buth, SWP_NOZORDER|SWP_NOACTIVATE);

        HWND emb = GetDlgItem(hwnd,IDC_CHILD);
        if (emb)
        {
          RECT sr;
          GetClientRect(emb,&sr);
          if (ypos > r.bottom-ypad-sr.bottom) ypos = r.bottom-ypad-sr.bottom;
          SetWindowPos(emb,NULL, xborder,ypos, xpos - xborder*2, sr.bottom, SWP_NOZORDER|SWP_NOACTIVATE);
          ShowWindow(emb,SW_SHOWNA);
        }

        HWND filt = GetDlgItem(hwnd,IDC_EXT);
        if (filt)
        {
          SetWindowPos(filt, NULL, xborder*2 + fnlblw, ypos -= fnh + yborder, r.right-fnlblw-xborder*3, fnh, SWP_NOZORDER|SWP_NOACTIVATE);
        }

        SetWindowPos(GetDlgItem(hwnd,IDC_EDIT), NULL, xborder*2 + fnlblw, ypos -= fnh + yborder, r.right-fnlblw-xborder*3, fnh, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd,IDC_LABEL), NULL, xborder, ypos, fnlblw, fnh, SWP_NOZORDER|SWP_NOACTIVATE);
        {
          BrowseFile_State *szparms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
          const bool pv    = !szparms || szparms->places_visible;
          const int sbw    = pv ? (szparms ? szparms->places_width : SWELL_UI_SCALE(140)) : 0;
          const int tgw    = SWELL_UI_SCALE(14);
          const int list_x = xborder + sbw + tgw;
          const int comboh = g_swell_ctheme.combo_height;
          const int filterw = wdl_max(r.right/8, SWELL_UI_SCALE(50));
          const int bar_y  = yborder/2;
          const int top_y  = comboh + yborder;   // file list starts below address bar
          const int mid_h  = ypos - top_y - yborder;
          const int full_h = ypos - bar_y - yborder; // sidebar spans full height

          // Address bar — to the right of the sidebar/toggle
          SetWindowPos(GetDlgItem(hwnd,IDC_DIR), NULL, list_x, bar_y,
              r.right - xborder*3 - comboh - filterw - list_x, comboh, SWP_NOZORDER|SWP_NOACTIVATE);
          SetWindowPos(GetDlgItem(hwnd,IDC_PARENTBUTTON), NULL,
              r.right - xborder*2 - comboh - filterw, bar_y,
              comboh, comboh, SWP_NOZORDER|SWP_NOACTIVATE);
          SetWindowPos(GetDlgItem(hwnd,IDC_FILTER), NULL,
              r.right - xborder - filterw, bar_y + (comboh - fnh)/2,
              filterw, fnh, SWP_NOZORDER|SWP_NOACTIVATE);

          // Sidebar mini toolbar (new group + manage) — same row as address bar
          {
            HWND sbnew = GetDlgItem(hwnd, FC_IDC_SB_NEW_GROUP);
            HWND sbmgr = GetDlgItem(hwnd, FC_IDC_SB_MANAGE);
            if (sbnew && sbmgr)
            {
              ShowWindow(sbnew, pv ? SW_SHOW : SW_HIDE);
              ShowWindow(sbmgr, pv ? SW_SHOW : SW_HIDE);
              if (pv && sbw > 0)
              {
                const int mgr_w = comboh;
                SetWindowPos(sbnew, NULL, xborder, bar_y, sbw - mgr_w, comboh, SWP_NOZORDER|SWP_NOACTIVATE);
                SetWindowPos(sbmgr, NULL, xborder + sbw - mgr_w, bar_y, mgr_w, comboh, SWP_NOZORDER|SWP_NOACTIVATE);
              }
            }
          }
          // Sidebar tree — full height minus toolbar row
          HWND places = GetDlgItem(hwnd, IDC_PLACES);
          if (places)
          {
            ShowWindow(places, pv ? SW_SHOW : SW_HIDE);
            if (pv) SetWindowPos(places, NULL, xborder, bar_y + comboh, sbw, full_h - comboh, SWP_NOZORDER|SWP_NOACTIVATE);
          }
          SetWindowPos(GetDlgItem(hwnd,IDC_PLACES_TOGGLE), NULL,
              xborder + sbw, bar_y, tgw, full_h, SWP_NOZORDER|SWP_NOACTIVATE);

          // File list — right pane, below address bar
          SetWindowPos(GetDlgItem(hwnd,IDC_LIST), NULL,
              list_x, top_y, r.right - xborder - list_x, mid_h, SWP_NOZORDER|SWP_NOACTIVATE);
        }
      }
    break;
    case WM_TIMER:
      if (wParam == 1)
      {
        KillTimer(hwnd,1);
        BrowseFile_State *parms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
        if (parms)
        {
          char buf[128];
          GetDlgItemText(hwnd,IDC_FILTER,buf,sizeof(buf));
          parms->viewlist_sort(buf);
          HWND list = GetDlgItem(hwnd,IDC_LIST);
          ListView_SetItemCount(list, parms->viewlist.GetSize());
          ListView_RedrawItems(list,0, parms->viewlist.GetSize());
        }
      }
    break;
    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDC_PLACES_TOGGLE:
          {
            BrowseFile_State *parms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
            if (parms)
            {
              parms->places_visible = !parms->places_visible;
              SetWindowText(GetDlgItem(hwnd, IDC_PLACES_TOGGLE),
                  parms->places_visible ? "\xe2\x97\x80" : "\xe2\x96\xb6"); // ◀ / ▶
              RECT cr; GetClientRect(hwnd, &cr);
              SendMessage(hwnd, WM_SIZE, 0, 0);
            }
          }
        return 0;
        case FC_IDC_SB_NEW_GROUP:
          {
            swellInputState gs={"New group","Group name:","",""};
            if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&gs)&&gs.result[0]) {
              char gp[PATH_MAX]; fc_groups_path(gp,sizeof(gp));
              WDL_PtrList<char> secs; fc_groups_get_sections(secs);
              bool dup=false;
              for(int si=0;si<secs.GetSize();si++) if(!strcmp(secs.Get(si),gs.result)){dup=true;break;}
              secs.Empty(true,free);
              if (!dup) {
                FILE *f=fopen(gp,"a");
                if(f){fprintf(f,"\n[%s]\n",gs.result);fclose(f);}
                fc_refresh_sidebar(hwnd);
              }
            }
          }
        return 0;
        case FC_IDC_SB_MANAGE:
          {
            WDL_PtrList<char> secs; fc_groups_get_sections(secs);
            HMENU m=CreatePopupMenu(); int pos=0;
            for(int si=0;si<secs.GetSize()&&si<64;si++) {
              HMENU gsub=CreatePopupMenu();
              SWELL_InsertMenu(gsub,0,MF_BYPOSITION,ID_FC_MANAGE_RENAME_BASE+si,"Rename...");
              SWELL_InsertMenu(gsub,1,MF_BYPOSITION,ID_FC_MANAGE_DELETE_BASE+si,"Delete");
              SWELL_InsertMenu(m,pos++,MF_BYPOSITION|MF_POPUP,(UINT_PTR)gsub,secs.Get(si));
            }
            if(secs.GetSize()>0)
              SWELL_InsertMenu(m,pos++,MF_BYPOSITION|MF_SEPARATOR,0,"");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_GROUP_NEW_SECTION,"New group...");
            POINT pt; GetCursorPos(&pt);
            int cmd=TrackPopupMenu(m,TPM_RETURNCMD,pt.x,pt.y,0,hwnd,NULL);
            DestroyMenu(m);
            if(cmd>=ID_FC_MANAGE_RENAME_BASE&&cmd<ID_FC_MANAGE_RENAME_BASE+64) {
              int idx=cmd-ID_FC_MANAGE_RENAME_BASE;
              if(idx<secs.GetSize()) {
                swellInputState is={"Rename group","New name:",secs.Get(idx),""};
                if(DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0]&&strcmp(is.result,secs.Get(idx)))
                  fc_group_rename_section(hwnd,secs.Get(idx),is.result);
              }
            } else if(cmd>=ID_FC_MANAGE_DELETE_BASE&&cmd<ID_FC_MANAGE_DELETE_BASE+64) {
              int idx=cmd-ID_FC_MANAGE_DELETE_BASE;
              if(idx<secs.GetSize()) fc_group_delete_section(hwnd,secs.Get(idx));
            } else if(cmd==ID_FC_GROUP_NEW_SECTION) {
              swellInputState gs={"New group","Group name:","",""};
              if(DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&gs)&&gs.result[0]) {
                char gp[PATH_MAX]; fc_groups_path(gp,sizeof(gp));
                WDL_PtrList<char> secs2; fc_groups_get_sections(secs2);
                bool dup=false;
                for(int si=0;si<secs2.GetSize();si++) if(!strcmp(secs2.Get(si),gs.result)){dup=true;break;}
                secs2.Empty(true,free);
                if(!dup){FILE *f=fopen(gp,"a");if(f){fprintf(f,"\n[%s]\n",gs.result);fclose(f);}fc_refresh_sidebar(hwnd);}
              }
            }
            secs.Empty(true,free);
          }
        return 0;
        case IDC_FILTER:
          if (HIWORD(wParam) == EN_CHANGE)
          {
            KillTimer(hwnd,1);
            SetTimer(hwnd,1,250,NULL);
          }
        return 0;
        case IDC_EXT:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            SendMessage(hwnd,WM_UPD,IDC_EXT,0);
          }
        return 0;
        case IDC_PARENTBUTTON:
          {
            int a = (int) SendDlgItemMessage(hwnd,IDC_DIR,CB_GETCURSEL,0,0);
            int cbcnt = (int) SendDlgItemMessage(hwnd,IDC_DIR,CB_GETCOUNT,0,0);
            if (a>=0 && a < cbcnt - recent_size())
            {
              SendDlgItemMessage(hwnd,IDC_DIR,CB_SETCURSEL,a+1,0);
            }
            else
            {
              char buf[maxPathLen];
              GetDlgItemText(hwnd,IDC_DIR,buf,sizeof(buf));
              preprocess_user_path(buf,sizeof(buf));
              WDL_remove_filepart(buf);
              if (a>=0)
                SendMessage(hwnd,WM_UPD,IDC_DIR,(LPARAM)buf);
              else
                SetDlgItemText(hwnd,IDC_DIR,buf);
            }
            SendMessage(hwnd,WM_UPD,1,0);
          }
        return 0;
        case IDC_DIR:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            int a = (int) SendDlgItemMessage(hwnd,IDC_DIR,CB_GETCURSEL,0,0);
            int cbcnt = (int) SendDlgItemMessage(hwnd,IDC_DIR,CB_GETCOUNT,0,0);
            if (a>=cbcnt - recent_size())
            {
              char buf[maxPathLen];
              GetDlgItemText(hwnd,IDC_DIR,buf,sizeof(buf));
              preprocess_user_path(buf,sizeof(buf));
              SendMessage(hwnd,WM_UPD,IDC_DIR,(LPARAM)buf);
            }
            SendMessage(hwnd,WM_UPD,1,0);
          }
        return 0;
        case IDCANCEL: EndDialog(hwnd,0); return 0;
        case IDOK: 
          {
            char buf[maxPathLen], msg[2048];
            GetDlgItemText(hwnd,IDC_DIR,buf,sizeof(buf));
            preprocess_user_path(buf,sizeof(buf));

            if (GetFocus() == GetDlgItem(hwnd,IDC_DIR))
            {
              DIR *dir = opendir(buf);
              if (!dir)
              {
                //snprintf(msg,sizeof(msg),"Path does not exist:\r\n\r\n%s",buf);
                //MessageBox(hwnd,msg,"Path not found",MB_OK);
                return 0;
              }
              closedir(dir);

              SendMessage(hwnd,WM_UPD,1,0);
              SendMessage(hwnd, WM_UPD, IDC_DIR, (LPARAM)buf);
              HWND e = GetDlgItem(hwnd,IDC_EDIT);
              SendMessage(e,EM_SETSEL,0,(LPARAM)-1);
              SetFocus(e);
              return 0;
            }

            size_t buflen = strlen(buf);
            if (!buflen) strcpy(buf,"/");
            else
            {
              if (buflen > sizeof(buf)-2) buflen = sizeof(buf)-2;
              if (buf[buflen-1]!='/') { buf[buflen++] = '/'; buf[buflen]=0; }
            }
            GetDlgItemText(hwnd,IDC_EDIT,msg,sizeof(msg));
            preprocess_user_path(msg,sizeof(msg));

            BrowseFile_State *parms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
            int cnt;
            if (parms->mode == BrowseFile_State::OPENMULTI && (cnt=ListView_GetSelectedCount(GetDlgItem(hwnd,IDC_LIST)))>1 && (!*msg || !strcmp(msg,multiple_files)))
            {
              recent_write(buf);
              HWND list = GetDlgItem(hwnd,IDC_LIST);
              WDL_TypedBuf<char> fs;
              fs.Set(buf,strlen(buf)+1);
              int a = ListView_GetNextItem(list,-1,LVNI_SELECTED);
              while (a != -1 && fs.GetSize() < 4096*4096 && cnt--)
              {
                if (a < 0 || a >= parms->viewlist.GetSize()) break;
                const struct BrowseFile_State::rec *rec = parms->viewlist.Get(a);
                if (!rec) break;

                fs.Add(rec->name,strlen(rec->name)+1);
                a = ListView_GetNextItem(list,a,LVNI_SELECTED);
              }
              fs.Add("",1);

              parms->fnout = (char*)malloc(fs.GetSize());
              if (parms->fnout) memcpy(parms->fnout,fs.Get(),fs.GetSize());

              EndDialog(hwnd,1);
              return 0;
            }
            else 
            {
              if (msg[0] == '.' && (msg[1] == '.' || msg[1] == 0))
              {
                if (msg[1] == '.') 
                {
                  int a = (int) SendDlgItemMessage(hwnd,IDC_DIR,CB_GETCURSEL,0,0);
                  if (a>=0) SendDlgItemMessage(hwnd,IDC_DIR,CB_SETCURSEL,a+1,0);
                }
                SetDlgItemText(hwnd,IDC_EDIT,"");
                SendMessage(hwnd,WM_UPD,1,0);
                return 0;
              }
              else if (msg[0] == '/') lstrcpyn_safe(buf,msg,sizeof(buf));
              else lstrcatn(buf,msg,sizeof(buf));
            }

            switch (parms->mode)
            {
              case BrowseFile_State::OPENDIR:
                 if (!buf[0]) return 0;
                 else if (msg[0])
                 {
                   // navigate to directory if filepart set
treatAsDir:
                   DIR *dir = opendir(buf);
                   if (!dir) 
                   {
                     snprintf(msg,sizeof(msg),"Error opening directory:\r\n\r\n%.1000s\r\n\r\nCreate?",buf);
                     if (MessageBox(hwnd,msg,"Create directory?",MB_OKCANCEL)==IDCANCEL) return 0;
                     CreateDirectory(buf,NULL);
                     dir=opendir(buf);
                   }
                   if (!dir) { MessageBox(hwnd,"Error creating directory","Error",MB_OK); return 0; }
                   closedir(dir);
                   SendMessage(hwnd, WM_UPD, IDC_DIR, (LPARAM)buf);
                   SetDlgItemText(hwnd,IDC_EDIT,"");
                   SendMessage(hwnd,WM_UPD,1,0);

                   return 0;
                 }
                 else
                 {
                   DIR *dir = opendir(buf);
                   if (!dir) return 0;
                   closedir(dir);
                 }
              break;
              case BrowseFile_State::SAVE:
                 if (!buf[0]) return 0;
                 else  
                 {
                   struct stat st={0,};
                   DIR *dir = opendir(buf);
                   if (dir)
                   {
                     closedir(dir);
                     SendMessage(hwnd, WM_UPD, IDC_DIR, (LPARAM)buf);
                     SetDlgItemText(hwnd,IDC_EDIT,"");
                     SendMessage(hwnd,WM_UPD,1,0);
                     return 0;
                   }
                   if (buf[strlen(buf)-1] == '/') goto treatAsDir;

                   const char *extlist = parms->extlist;
                   if (extlist && *extlist && ext_valid_for_extlist(WDL_get_fileext(buf),extlist) < 0)
                   {
                     const char *erd = extlist+strlen(extlist)+1;
                     LRESULT combo_sel;
                     HWND combo = GetDlgItem(hwnd, IDC_EXT);
                     if (combo && (combo_sel=SendMessage(combo,CB_GETCURSEL,0,0)) != CB_ERR)
                     {
                       const char *filt = (const char *)SendMessage(combo,CB_GETITEMDATA,combo_sel,0);
                       if (filt && *filt == '*' && filt[1] == '.' && filt[2] && filt[2] != '*') erd = filt;
                     }

                     if (*erd == '*' && erd[1] == '.' && erd[2] && erd[2] != '*') // add default extension
                     {
                       const char *a = (erd+=1);
                       while (*erd && *erd != ';') erd++;
                       if (erd > a+1) snprintf_append(buf,sizeof(buf),"%.*s",(int)(erd-a),a);
                     }
                   }
                   if (!stat(buf,&st))
                   {
                     snprintf(msg,sizeof(msg),"File exists:\r\n\r\n%.1000s\r\n\r\nOverwrite?",buf);
                     if (MessageBox(hwnd,msg,"Overwrite file?",MB_OKCANCEL)==IDCANCEL) return 0;
                   }
                 }
              break;
              default:
                 if (!buf[0]) return 0;
                 else  
                 {
                   struct stat st={0,};
                   DIR *dir = opendir(buf);
                   if (dir)
                   {
                     closedir(dir);
                     SendMessage(hwnd, WM_UPD, IDC_DIR, (LPARAM)buf);
                     SetDlgItemText(hwnd,IDC_EDIT,"");
                     SendMessage(hwnd,WM_UPD,1,0);
                     return 0;
                   }
                   if (stat(buf,&st))
                   {
                     //snprintf(msg,sizeof(msg),"File does not exist:\r\n\r\n%s",buf);
                     //MessageBox(hwnd,msg,"File not found",MB_OK);
                     return 0;
                   }
                 }
              break;
            }
            if (parms->fnout) 
            {
              lstrcpyn_safe(parms->fnout,buf,parms->fnout_sz);
            }
            else
            {
              size_t l = strlen(buf);
              parms->fnout = (char*)calloc(l+2,1);
              memcpy(parms->fnout,buf,l);
            }
            if (parms->mode != BrowseFile_State::OPENDIR)
              WDL_remove_filepart(buf);
            recent_write(buf);
          }
          EndDialog(hwnd,1);
        return 0;
        case ID_SHOW_HIDDEN:
          {
            BrowseFile_State *parms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
            parms->show_hidden = !parms->show_hidden;
            SendMessage(hwnd,WM_UPD,1,0);
          }
        return 0;
        case ID_FC_NEWFOLDER:
          {
            char dir[2048]; GetDlgItemText(hwnd,IDC_DIR,dir,sizeof(dir));
            swellInputState is={"New folder","Folder name:","New folder",""};
            if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0]) {
              char nd[PATH_MAX]; snprintf(nd,sizeof(nd),"%s/%s",dir,is.result);
              if (mkdir(nd,0755)==0) SendMessage(hwnd,WM_UPD,1,0);
              else MessageBox(hwnd,"Could not create folder","Error",MB_OK);
            }
          }
        return 0;
        case ID_FC_RENAME:
          {
            BrowseFile_State *parms=(BrowseFile_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
            HWND list=GetDlgItem(hwnd,IDC_LIST);
            int selidx=ListView_GetNextItem(list,-1,LVNI_SELECTED);
            if (selidx>=0&&parms) {
              struct BrowseFile_State::rec *rec=parms->viewlist.Get(selidx);
              if (rec&&rec->name) {
                char dir[2048]; GetDlgItemText(hwnd,IDC_DIR,dir,sizeof(dir));
                char oldpath[PATH_MAX]; snprintf(oldpath,sizeof(oldpath),"%s/%s",dir,rec->name);
                swellInputState is={"Rename","New name:",rec->name,""};
                if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0]&&strcmp(is.result,rec->name)) {
                  char np[PATH_MAX]; snprintf(np,sizeof(np),"%s/%s",dir,is.result);
                  if (rename(oldpath,np)==0) SendMessage(hwnd,WM_UPD,1,0);
                  else MessageBox(hwnd,"Could not rename","Error",MB_OK);
                }
              }
            }
          }
        return 0;
        case ID_FC_TRASH:
          {
            BrowseFile_State *parms=(BrowseFile_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
            HWND list=GetDlgItem(hwnd,IDC_LIST);
            if (!parms) return 0;
            char dir[2048]; GetDlgItemText(hwnd,IDC_DIR,dir,sizeof(dir));
            int a=ListView_GetNextItem(list,-1,LVNI_SELECTED); bool any=false;
            while (a!=-1) {
              struct BrowseFile_State::rec *rec=parms->viewlist.Get(a);
              if (rec&&rec->name) {
                char fp[PATH_MAX]; snprintf(fp,sizeof(fp),"%s/%s",dir,rec->name);
                fc_trash_file(fp); any=true;
              }
              a=ListView_GetNextItem(list,a,LVNI_SELECTED);
            }
            if (any) SendMessage(hwnd,WM_UPD,1,0);
          }
        return 0;
        case ID_FC_COPY:
        case ID_FC_CUT:
          {
            BrowseFile_State *parms=(BrowseFile_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
            HWND list=GetDlgItem(hwnd,IDC_LIST);
            if (!parms) return 0;
            char dir[2048]; GetDlgItemText(hwnd,IDC_DIR,dir,sizeof(dir));
            s_fc_clip.clear();
            s_fc_clip.is_cut=(LOWORD(wParam)==ID_FC_CUT);
            int a=ListView_GetNextItem(list,-1,LVNI_SELECTED);
            while (a!=-1) {
              struct BrowseFile_State::rec *rec=parms->viewlist.Get(a);
              if (rec&&rec->name) {
                char fp[PATH_MAX]; snprintf(fp,sizeof(fp),"%s/%s",dir,rec->name);
                s_fc_clip.paths.Add(strdup(fp));
              }
              a=ListView_GetNextItem(list,a,LVNI_SELECTED);
            }
            if (s_fc_clip.paths.GetSize()>0) fc_set_system_clip(hwnd,s_fc_clip.paths,s_fc_clip.is_cut);
          }
        return 0;
        case ID_FC_PASTE:
          {
            BrowseFile_State *parms=(BrowseFile_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
            if (!parms) return 0;
            char dir[2048]; GetDlgItemText(hwnd,IDC_DIR,dir,sizeof(dir));
            size_t dl=strlen(dir); if (dl>0&&dir[dl-1]=='/') dir[--dl]=0;
            WDL_PtrList<char> *srcs=NULL; bool is_cut=false;
            WDL_PtrList<char> sys_paths;
            if (s_fc_clip.paths.GetSize()>0) {
              srcs=&s_fc_clip.paths; is_cut=s_fc_clip.is_cut;
            } else {
              OpenClipboard(hwnd);
              HANDLE hc=GetClipboardData(CF_HDROP);
              if (hc) {
                DROPFILES *df=(DROPFILES*)GlobalLock(hc);
                if (df) { const char *pp=(const char*)df+df->pFiles; while(*pp){sys_paths.Add(strdup(pp));pp+=strlen(pp)+1;} GlobalUnlock(hc); }
              }
              CloseClipboard();
              if (sys_paths.GetSize()>0) srcs=&sys_paths;
            }
            if (!srcs||srcs->GetSize()==0) return 0;
            bool any_err=false;
            for (int i=0;i<srcs->GetSize();i++) {
              const char *src=srcs->Get(i);
              char dst[PATH_MAX]; snprintf(dst,sizeof(dst),"%s/%s",dir,WDL_get_filepart(src));
              if (strcmp(src,dst)==0) continue;
              if (is_cut) { if (rename(src,dst)!=0) any_err=true; }
              else {
                struct stat st;
                if (stat(src,&st)==0) {
                  if (S_ISREG(st.st_mode)) { if (!fc_copy_file(src,dst)) any_err=true; }
                  else if (S_ISDIR(st.st_mode)) {
                    pid_t pid=fork();
                    if (pid==0){execlp("cp","cp","-rp",src,dst,(char*)NULL);_exit(1);}
                    else if (pid>0){int ws;waitpid(pid,&ws,0);if(!WIFEXITED(ws)||WEXITSTATUS(ws)!=0)any_err=true;}
                    else any_err=true;
                  }
                } else any_err=true;
              }
            }
            sys_paths.Empty(true,free);
            if (is_cut&&!any_err) s_fc_clip.clear();
            if (any_err) MessageBox(hwnd,"Some files could not be pasted","Warning",MB_OK);
            SendMessage(hwnd,WM_UPD,1,0);
          }
        return 0;
        case ID_FC_COPYPATH:
          {
            BrowseFile_State *parms=(BrowseFile_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
            HWND list=GetDlgItem(hwnd,IDC_LIST);
            int selidx=ListView_GetNextItem(list,-1,LVNI_SELECTED);
            char dir[2048]; GetDlgItemText(hwnd,IDC_DIR,dir,sizeof(dir));
            char fp[PATH_MAX];
            if (selidx>=0&&parms) {
              struct BrowseFile_State::rec *rec=parms->viewlist.Get(selidx);
              if (rec&&rec->name) snprintf(fp,sizeof(fp),"%s/%s",dir,rec->name);
              else lstrcpyn_safe(fp,dir,sizeof(fp));
            } else lstrcpyn_safe(fp,dir,sizeof(fp));
            OpenClipboard(hwnd); EmptyClipboard();
            size_t l=strlen(fp)+1; HANDLE hh=GlobalAlloc(GMEM_MOVEABLE,l);
            if (hh){char *q=(char*)GlobalLock(hh);if(q){memcpy(q,fp,l);GlobalUnlock(hh);}SetClipboardData(CF_TEXT,hh);}
            CloseClipboard();
          }
        return 0;
        case ID_FC_OPENFILEMANAGER:
          {
            char dir[2048]; GetDlgItemText(hwnd,IDC_DIR,dir,sizeof(dir));
            pid_t pid=fork();
            if (pid==0){setsid();execlp("xdg-open","xdg-open",dir,(char*)NULL);_exit(1);}
            else if (pid>0) waitpid(pid,NULL,WNOHANG);
          }
        return 0;
        case ID_FC_SIDEBAR_ADD:
          {
            char dir[2048]; GetDlgItemText(hwnd,IDC_DIR,dir,sizeof(dir));
            WDL_PtrList<char> secs; fc_groups_get_sections(secs);
            if (secs.GetSize() == 0) {
              // No groups yet — create new group and add
              swellInputState gs={"New group","Group name:","",""};
              if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&gs)&&gs.result[0]) {
                swellInputState is={"Add to group","Name:",WDL_get_filepart(dir),""};
                if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0])
                  fc_group_add(hwnd, gs.result, dir, is.result);
              }
            } else {
              // Pick existing group or create new
              HMENU sub=CreatePopupMenu(); int sp=0;
              for (int si=0; si<secs.GetSize() && si<128; si++)
                SWELL_InsertMenu(sub, sp++, MF_BYPOSITION, ID_FC_GROUP_ADD_BASE+si, secs.Get(si));
              SWELL_InsertMenu(sub, sp++, MF_BYPOSITION|MF_SEPARATOR, 0, "");
              SWELL_InsertMenu(sub, sp++, MF_BYPOSITION, ID_FC_GROUP_NEW_SECTION, "New group...");
              POINT pt; GetCursorPos(&pt);
              int cmd=TrackPopupMenu(sub, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, NULL);
              DestroyMenu(sub);
              if (cmd >= ID_FC_GROUP_ADD_BASE && cmd < ID_FC_GROUP_NEW_SECTION) {
                WDL_PtrList<char> secs2; fc_groups_get_sections(secs2);
                int idx = cmd - ID_FC_GROUP_ADD_BASE;
                if (idx < secs2.GetSize()) {
                  const char *sname = secs2.Get(idx);
                  swellInputState is={"Add to group","Name:",WDL_get_filepart(dir),""};
                  if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0])
                    fc_group_add(hwnd, sname, dir, is.result);
                }
                secs2.Empty(true,free);
              } else if (cmd == ID_FC_GROUP_NEW_SECTION) {
                swellInputState gs={"New group","Group name:","",""};
                if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&gs)&&gs.result[0]) {
                  swellInputState is={"Add to group","Name:",WDL_get_filepart(dir),""};
                  if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0])
                    fc_group_add(hwnd, gs.result, dir, is.result);
                }
              }
            }
            secs.Empty(true,free);
          }
        return 0;
        case ID_FC_SIDEBAR_REMOVE:
          // handled inline in WM_CONTEXTMENU
        return 0;
      }
    break;
    case WM_NOTIFY:
      {
        LPNMHDR l=(LPNMHDR)lParam;
        if (l->idFrom == IDC_PLACES)
        {
          NMTREEVIEW *nmtv = (NMTREEVIEW*)lParam;
          if (l->code == TVN_SELCHANGED)
          {
            char *path = (char*)nmtv->itemNew.lParam;
            if (lp_is_path((LPARAM)path))
            {
              char buf[2048];
              lstrcpyn_safe(buf, path, sizeof(buf));
              SendMessage(hwnd, WM_UPD, IDC_DIR, (LPARAM)buf);
              SendMessage(hwnd, WM_UPD, 1, 0);
            }
          }
          else if (l->code == TVN_ITEMEXPANDING && nmtv->action == TVE_EXPAND)
          {
            HTREEITEM first = TreeView_GetChild((HWND)l->hwndFrom, nmtv->itemNew.hItem);
            if (!first)
            {
              char *path = (char*)nmtv->itemNew.lParam;
              if (lp_is_path((LPARAM)path)) browse_tree_expand((HWND)l->hwndFrom, nmtv->itemNew.hItem, path);
            }
          }
          else if (l->code == NM_RCLICK)
          {
            // WM_RBUTTONDOWN already selected the item (swell-wnd-generic patch),
            // so GetSelection and IDC_DIR both reflect what was right-clicked
            HTREEITEM hitem = TreeView_GetSelection((HWND)l->hwndFrom);
            const char *item_path = NULL;
            const char *group_hdr = NULL;
            if (hitem) {
              TVITEM ti; memset(&ti,0,sizeof(ti));
              ti.mask=TVIF_PARAM|TVIF_HANDLE; ti.hItem=hitem;
              if (TreeView_GetItem((HWND)l->hwndFrom,&ti)) {
                if (lp_is_path(ti.lParam)) item_path=(const char*)ti.lParam;
                else if (lp_is_group_hdr(ti.lParam)) group_hdr=(const char*)ti.lParam+1;
              }
            }
            char cur_dir[2048]; GetDlgItemText(hwnd, IDC_DIR, cur_dir, sizeof(cur_dir));
            POINT p; GetCursorPos(&p);
            // build "Add to group ▶" submenu
            WDL_PtrList<char> secs;
            fc_groups_get_sections(secs);
            HMENU sub = CreatePopupMenu(); int sp=0;
            for (int si=0; si<secs.GetSize() && si<128; si++)
              SWELL_InsertMenu(sub, sp++, MF_BYPOSITION, ID_FC_GROUP_ADD_BASE+si, secs.Get(si));
            if (secs.GetSize()) SWELL_InsertMenu(sub, sp++, MF_BYPOSITION|MF_SEPARATOR, 0, "");
            SWELL_InsertMenu(sub, sp++, MF_BYPOSITION, ID_FC_GROUP_NEW_SECTION, "New group...");
            HMENU m = CreatePopupMenu(); int pos=0;
            SWELL_InsertMenu(m, pos++, MF_BYPOSITION|MF_POPUP, (UINT_PTR)sub, "Add to group");
            if (item_path) {
              SWELL_InsertMenu(m, pos++, MF_BYPOSITION, ID_FC_SIDEBAR_REMOVE, "Remove from sidebar");
              SWELL_InsertMenu(m, pos++, MF_BYPOSITION|MF_SEPARATOR, 0, "");
              SWELL_InsertMenu(m, pos++, MF_BYPOSITION, ID_FC_COPYPATH, "Copy path");
              SWELL_InsertMenu(m, pos++, MF_BYPOSITION, ID_FC_OPENFILEMANAGER, "Open in file manager");
            }
            if (group_hdr) {
              SWELL_InsertMenu(m, pos++, MF_BYPOSITION|MF_SEPARATOR, 0, "");
              SWELL_InsertMenu(m, pos++, MF_BYPOSITION, ID_FC_GROUP_RENAME, "Rename group");
              SWELL_InsertMenu(m, pos++, MF_BYPOSITION, ID_FC_GROUP_DELETE, "Delete group");
            }
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD, p.x, p.y, 0, hwnd, NULL);
            DestroyMenu(m); // also destroys sub
            char item_path_copy[2048]; item_path_copy[0]=0;
            if (item_path) lstrcpyn_safe(item_path_copy, item_path, sizeof(item_path_copy));
            char group_hdr_copy[256]; group_hdr_copy[0]=0;
            if (group_hdr) lstrcpyn_safe(group_hdr_copy, group_hdr, sizeof(group_hdr_copy));
            secs.Empty(true, free);
            if (cmd >= ID_FC_GROUP_ADD_BASE && cmd < ID_FC_GROUP_NEW_SECTION) {
              // re-read sections to get name (secs was freed)
              WDL_PtrList<char> secs2; fc_groups_get_sections(secs2);
              int idx = cmd - ID_FC_GROUP_ADD_BASE;
              if (idx < secs2.GetSize()) {
                const char *sname = secs2.Get(idx);
                swellInputState is={"Add to group","Name:",WDL_get_filepart(cur_dir),""};
                if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0])
                  fc_group_add(hwnd, sname, cur_dir, is.result);
              }
              secs2.Empty(true,free);
            }
            else if (cmd == ID_FC_GROUP_NEW_SECTION) {
              swellInputState gs={"New group","Group name:","",""};
              if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&gs)&&gs.result[0]) {
                swellInputState is={"Add to group","Name:",WDL_get_filepart(cur_dir),""};
                if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0])
                  fc_group_add(hwnd, gs.result, cur_dir, is.result);
              }
            }
            else if (cmd==ID_FC_SIDEBAR_REMOVE && item_path_copy[0]) fc_group_remove_item(hwnd, item_path_copy);
            else if (cmd==ID_FC_COPYPATH && item_path_copy[0]) {
              OpenClipboard(hwnd); EmptyClipboard();
              size_t hl=strlen(item_path_copy)+1; HANDLE hh=GlobalAlloc(GMEM_MOVEABLE,hl);
              if(hh){char *p2=(char*)GlobalLock(hh);if(p2){memcpy(p2,item_path_copy,hl);GlobalUnlock(hh);}SetClipboardData(CF_TEXT,hh);}
              CloseClipboard();
            }
            else if (cmd==ID_FC_OPENFILEMANAGER && item_path_copy[0]) {
              pid_t pid=fork();
              if(pid==0){execlp("xdg-open","xdg-open",item_path_copy,(char*)NULL);_exit(1);}
              else if(pid>0){int st;waitpid(pid,&st,WNOHANG);}
            }
            else if (cmd==ID_FC_GROUP_RENAME && group_hdr_copy[0]) {
              swellInputState is={"Rename group","New name:",group_hdr_copy,""};
              if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0]&&strcmp(is.result,group_hdr_copy))
                fc_group_rename_section(hwnd, group_hdr_copy, is.result);
            }
            else if (cmd==ID_FC_GROUP_DELETE && group_hdr_copy[0]) {
              fc_group_delete_section(hwnd, group_hdr_copy);
            }
            return 1;
          }
          return 0;
        }
        if (l->code == LVN_GETDISPINFO)
        {
          BrowseFile_State *parms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
          NMLVDISPINFO *lpdi = (NMLVDISPINFO*) lParam;
          const int idx=lpdi->item.iItem;
          if (l->idFrom == IDC_LIST && parms)
          {
            struct BrowseFile_State::rec *rec = parms->viewlist.Get(idx);
            if (rec && rec->name)
            {
              if (lpdi->item.mask&LVIF_TEXT) 
              {
                switch (lpdi->item.iSubItem)
                {
                  case 0:
                    lpdi->item.pszText = rec->name;
                  break;
                  case 1:
                    rec->format_size(lpdi->item.pszText,lpdi->item.cchTextMax);
                  break;
                  case 2:
                    rec->format_date(lpdi->item.pszText,lpdi->item.cchTextMax);
                  break;
                }
              }
            }
          }
        }
        else if (l->code == LVN_ODFINDITEM)
        {
        }
        else if (l->code == LVN_ITEMCHANGED)
        {
          const int selidx = ListView_GetNextItem(l->hwndFrom, -1, LVNI_SELECTED);
          BrowseFile_State *parms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
          if (selidx>=0 && parms)
          {
            if (parms->mode == BrowseFile_State::OPENMULTI && ListView_GetSelectedCount(l->hwndFrom)>1)
            {
              SetDlgItemText(hwnd,IDC_EDIT,multiple_files);
            }
            else
            {
              struct BrowseFile_State::rec *rec = parms->viewlist.Get(selidx);
              if (rec)
              {
                SetDlgItemText(hwnd,IDC_EDIT,rec->name);
              }
            }
          }
        }
        else if (l->code == NM_DBLCLK)
        {
          SendMessage(hwnd,WM_COMMAND,IDOK,0);
        }
        else if (l->code == LVN_COLUMNCLICK)
        {
          NMLISTVIEW* lv = (NMLISTVIEW*) l;
          BrowseFile_State *parms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
          parms->sortrev=!parms->sortrev;
          char lcol = parms->sortcol;
          if ((int)parms->sortcol != lv->iSubItem)
          { 
            parms->sortcol = (char)lv->iSubItem; 
            parms->sortrev=0; 
          }

          HWND hdr = ListView_GetHeader(l->hwndFrom);
          HDITEM hi;
          memset(&hi,0,sizeof(hi));
          hi.mask = HDI_FORMAT;
          Header_SetItem(hdr,lcol,&hi);
          hi.fmt = parms->sortrev ? HDF_SORTDOWN : HDF_SORTUP;
          Header_SetItem(hdr,parms->sortcol,&hi);

          parms->viewlist_sort(NULL);
          ListView_RedrawItems(l->hwndFrom,0, parms->viewlist.GetSize());
        }
      }
    break;
    case WM_KEYDOWN:
      if (lParam == FVIRTKEY && wParam == VK_F5)
      {
        SendMessage(hwnd,WM_UPD,1,0);
        return 1;
      }
      else if (lParam == (FVIRTKEY|FCONTROL) && wParam == 'H')
      {
        SendMessage(hwnd,WM_COMMAND,ID_SHOW_HIDDEN,0);
        return 1;
      }
      else if (lParam == FVIRTKEY && wParam == VK_BACK && 
               GetFocus() == GetDlgItem(hwnd,IDC_LIST))
      {
        SendMessage(hwnd,WM_COMMAND,IDC_PARENTBUTTON,0);
        return 1;
      }
      else if ((lParam&0xff) == FVIRTKEY && wParam == VK_RETURN &&
               GetFocus() == GetDlgItem(hwnd,IDC_LIST))
      {
        SendMessage(hwnd,WM_COMMAND,IDOK,0);
        return 1;
      }
      else if (lParam == (FVIRTKEY|FCONTROL) && wParam == 'C' && GetFocus()==GetDlgItem(hwnd,IDC_LIST))
      { SendMessage(hwnd,WM_COMMAND,ID_FC_COPY,0); return 1; }
      else if (lParam == (FVIRTKEY|FCONTROL) && wParam == 'X' && GetFocus()==GetDlgItem(hwnd,IDC_LIST))
      { SendMessage(hwnd,WM_COMMAND,ID_FC_CUT,0); return 1; }
      else if (lParam == (FVIRTKEY|FCONTROL) && wParam == 'V')
      { SendMessage(hwnd,WM_COMMAND,ID_FC_PASTE,0); return 1; }
      else if (lParam == FVIRTKEY && wParam == VK_DELETE && GetFocus()==GetDlgItem(hwnd,IDC_LIST))
      { SendMessage(hwnd,WM_COMMAND,ID_FC_TRASH,0); return 1; }
      else if (lParam == FVIRTKEY && wParam == VK_F2 && GetFocus()==GetDlgItem(hwnd,IDC_LIST))
      { SendMessage(hwnd,WM_COMMAND,ID_FC_RENAME,0); return 1; }
      else if (lParam == (FVIRTKEY|FCONTROL|FSHIFT) && wParam == 'N')
      { SendMessage(hwnd,WM_COMMAND,ID_FC_NEWFOLDER,0); return 1; }
    return 0;
    case WM_CONTEXTMENU:
      {
        BrowseFile_State *parms = (BrowseFile_State *)GetWindowLongPtr(hwnd,GWLP_USERDATA);
        if (!parms) return 1;
        HWND places = GetDlgItem(hwnd,IDC_PLACES);
        HWND list   = GetDlgItem(hwnd,IDC_LIST);
        HWND hit    = (HWND)wParam;
        POINT p; GetCursorPos(&p);

        if (hit == places)
        {
          // ── Sidebar context menu ───────────────────────────────────────
          POINT lp=p; ScreenToClient(places,&lp);
          TVHITTESTINFO thi; memset(&thi,0,sizeof(thi)); thi.pt=lp;
          HTREEITEM hitem=TreeView_HitTest(places,&thi);
          const char *item_path=NULL;
          const char *group_hdr=NULL;
          if (hitem) {
            TVITEM ti; memset(&ti,0,sizeof(ti)); ti.mask=TVIF_PARAM|TVIF_HANDLE; ti.hItem=hitem;
            if (TreeView_GetItem(places,&ti)) {
              if (lp_is_path(ti.lParam)) item_path=(const char*)ti.lParam;
              else if (lp_is_group_hdr(ti.lParam)) group_hdr=(const char*)ti.lParam+1;
            }
          }
          char cur_dir[2048]; GetDlgItemText(hwnd,IDC_DIR,cur_dir,sizeof(cur_dir));
          // build "Add to group ▶" submenu
          WDL_PtrList<char> secs;
          fc_groups_get_sections(secs);
          HMENU sub=CreatePopupMenu(); int sp=0;
          for (int si=0; si<secs.GetSize() && si<128; si++)
            SWELL_InsertMenu(sub, sp++, MF_BYPOSITION, ID_FC_GROUP_ADD_BASE+si, secs.Get(si));
          if (secs.GetSize()) SWELL_InsertMenu(sub, sp++, MF_BYPOSITION|MF_SEPARATOR, 0, "");
          SWELL_InsertMenu(sub, sp++, MF_BYPOSITION, ID_FC_GROUP_NEW_SECTION, "New group...");
          HMENU m=CreatePopupMenu(); int pos=0;
          SWELL_InsertMenu(m, pos++, MF_BYPOSITION|MF_POPUP, (UINT_PTR)sub, "Add to group");
          if (item_path) {
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_SIDEBAR_REMOVE,"Remove from sidebar");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION|MF_SEPARATOR,0,"");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_COPYPATH,"Copy path");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_OPENFILEMANAGER,"Open in file manager");
          }
          if (group_hdr) {
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION|MF_SEPARATOR,0,"");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_GROUP_RENAME,"Rename group");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_GROUP_DELETE,"Delete group");
          }
          int cmd=TrackPopupMenu(m,TPM_RETURNCMD,p.x,p.y,0,hwnd,NULL);
          DestroyMenu(m); // also destroys sub
          char item_path_copy[2048]; item_path_copy[0]=0;
          if (item_path) lstrcpyn_safe(item_path_copy,item_path,sizeof(item_path_copy));
          char group_hdr_copy[256]; group_hdr_copy[0]=0;
          if (group_hdr) lstrcpyn_safe(group_hdr_copy,group_hdr,sizeof(group_hdr_copy));
          secs.Empty(true,free);
          if (cmd >= ID_FC_GROUP_ADD_BASE && cmd < ID_FC_GROUP_NEW_SECTION) {
            WDL_PtrList<char> secs2; fc_groups_get_sections(secs2);
            int idx = cmd - ID_FC_GROUP_ADD_BASE;
            if (idx < secs2.GetSize()) {
              const char *sname = secs2.Get(idx);
              swellInputState is={"Add to group","Name:",WDL_get_filepart(cur_dir),""};
              if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0])
                fc_group_add(hwnd, sname, cur_dir, is.result);
            }
            secs2.Empty(true,free);
          }
          else if (cmd==ID_FC_GROUP_NEW_SECTION) {
            swellInputState gs={"New group","Group name:","",""};
            if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&gs)&&gs.result[0]) {
              swellInputState is={"Add to group","Name:",WDL_get_filepart(cur_dir),""};
              if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0])
                fc_group_add(hwnd, gs.result, cur_dir, is.result);
            }
          }
          else if (cmd==ID_FC_SIDEBAR_REMOVE && item_path_copy[0]) fc_group_remove_item(hwnd,item_path_copy);
          else if (cmd==ID_FC_COPYPATH && item_path_copy[0]) {
            OpenClipboard(hwnd); EmptyClipboard();
            size_t l=strlen(item_path_copy)+1; HANDLE hh=GlobalAlloc(GMEM_MOVEABLE,l);
            if (hh){char *q=(char*)GlobalLock(hh);if(q){memcpy(q,item_path_copy,l);GlobalUnlock(hh);}SetClipboardData(CF_TEXT,hh);}
            CloseClipboard();
          }
          else if (cmd==ID_FC_OPENFILEMANAGER && item_path_copy[0]) {
            pid_t pid=fork();
            if (pid==0){setsid();execlp("xdg-open","xdg-open",item_path_copy,(char*)NULL);_exit(1);}
            else if (pid>0) waitpid(pid,NULL,WNOHANG);
          }
          else if (cmd==ID_FC_GROUP_RENAME && group_hdr_copy[0]) {
            swellInputState is={"Rename group","New name:",group_hdr_copy,""};
            if (DialogBoxParam(NULL,NULL,hwnd,swellInputProc,(LPARAM)&is)&&is.result[0]&&strcmp(is.result,group_hdr_copy))
              fc_group_rename_section(hwnd, group_hdr_copy, is.result);
          }
          else if (cmd==ID_FC_GROUP_DELETE && group_hdr_copy[0]) {
            fc_group_delete_section(hwnd, group_hdr_copy);
          }
        }
        else
        {
          // ── File list / background context menu ───────────────────────
          int selidx=ListView_GetNextItem(list,-1,LVNI_SELECTED);
          int selcnt=ListView_GetSelectedCount(list);
          HMENU m=CreatePopupMenu(); int pos=0;
          SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_NEWFOLDER,"New folder\tCtrl+Shift+N");
          if (selidx>=0) {
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION|MF_SEPARATOR,0,"");
            if (selcnt==1) SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_RENAME,"Rename\tF2");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_COPY,"Copy\tCtrl+C");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_CUT,"Cut\tCtrl+X");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_TRASH,"Move to Trash\tDel");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION|MF_SEPARATOR,0,"");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_COPYPATH,"Copy path\tCtrl+Shift+C");
          }
          if (s_fc_clip.paths.GetSize()>0) {
            if (selidx<0) SWELL_InsertMenu(m,pos++,MF_BYPOSITION|MF_SEPARATOR,0,"");
            SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_PASTE,"Paste\tCtrl+V");
          }
          SWELL_InsertMenu(m,pos++,MF_BYPOSITION|MF_SEPARATOR,0,"");
          SWELL_InsertMenu(m,pos++,MF_BYPOSITION,ID_FC_OPENFILEMANAGER,"Open in file manager");
          SWELL_InsertMenu(m,pos++,MF_BYPOSITION|MF_SEPARATOR,0,"");
          SWELL_InsertMenu(m,pos++,MF_BYPOSITION|(parms->show_hidden?MF_CHECKED:MF_UNCHECKED),ID_SHOW_HIDDEN,"Show hidden files\tCtrl+H");
          int cmd=TrackPopupMenu(m,TPM_RETURNCMD,p.x,p.y,0,hwnd,NULL);
          DestroyMenu(m);
          if (cmd) SendMessage(hwnd,WM_COMMAND,cmd,0);
        }
      }
    return 1;
  }
  return 0;
}

// return true
bool BrowseForSaveFile(const char *text, const char *initialdir, const char *initialfile, const char *extlist,
                       char *fn, int fnsize)
{
  BrowseFile_State state( text, initialdir, initialfile, extlist, BrowseFile_State::SAVE, fn, fnsize );
  if (!DialogBoxParam(NULL,NULL,GetForegroundWindow(),swellFileSelectProc,(LPARAM)&state)) return false;

  return true;
}

bool BrowseForDirectory(const char *text, const char *initialdir, char *fn, int fnsize)
{
  BrowseFile_State state( text, initialdir, initialdir, NULL, BrowseFile_State::OPENDIR, fn, fnsize );
  return !!DialogBoxParam(NULL,NULL,GetForegroundWindow(),swellFileSelectProc,(LPARAM)&state);
}


char *BrowseForFiles(const char *text, const char *initialdir, 
                     const char *initialfile, bool allowmul, const char *extlist)
{
  BrowseFile_State state( text, initialdir, initialfile, extlist, 
           allowmul ? BrowseFile_State::OPENMULTI : BrowseFile_State::OPEN, NULL, 0 );
  return DialogBoxParam(NULL,NULL,GetForegroundWindow(),swellFileSelectProc,(LPARAM)&state) ? state.fnout : NULL;
}

static const char *mbidtostr(int idx)
{
  switch (idx)
  {
    case IDOK: return "OK";
    case IDCANCEL: return "Cancel";
    case IDYES: return "Yes";
    case IDNO: return "No";
    case IDRETRY: return "Retry";
    case IDABORT: return "Abort";
    case IDIGNORE: return "Ignore";
    default:
      WDL_ASSERT(idx == 0);
    return "";
  }
}

static LRESULT WINAPI swellMessageBoxProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  enum { IDC_LABEL=0x100 };
  const int button_spacing = 8;
  switch (uMsg)
  {
    case WM_CREATE:
      if (lParam)  // swell-specific
      {
        SetWindowLong(hwnd,GWL_WNDPROC,(LPARAM)SwellDialogDefaultWindowProc);
        SetWindowLong(hwnd,DWL_DLGPROC,(LPARAM)swellMessageBoxProc);
        void **parms = (void **)lParam;
        if (parms[1]) SetWindowText(hwnd,(const char*)parms[1]);


        int b1=IDOK, b2=0,b3=0;

        const int fmode = (int)(INT_PTR)parms[2];
        switch (fmode & 0xf)
        {
          case MB_ABORTRETRYIGNORE:
            b1 = IDABORT;
            b2 = IDRETRY;
            b3 = IDIGNORE;
          break;
          case MB_RETRYCANCEL:
            b1 = IDRETRY;
            // fallthrough
          case MB_OKCANCEL:
            b2 = IDCANCEL;
          break;
          case MB_YESNOCANCEL:
            b3 = IDCANCEL;
            // fallthrough
          case MB_YESNO:
            b1 = IDYES;
            b2 = IDNO;
          break;
        }

        SWELL_MakeSetCurParms(1,1,0,0,hwnd,false,false);
        RECT labsize = {0,0,300,20};
        HWND lab = SWELL_MakeLabel(-1,parms[0] ? (const char *)parms[0] : "", IDC_LABEL, 0,0,10,10,SS_CENTER|SS_NOPREFIX); //we'll resize this manually
        HDC dc=GetDC(lab); 
        if (lab && parms[0])
        {
          DrawText(dc,(const char *)parms[0],-1,&labsize,DT_CALCRECT|DT_NOPREFIX);// if dc isnt valid yet, try anyway
        }

        const int sc10 = SWELL_UI_SCALE(10);
        const int sc8 = SWELL_UI_SCALE(8);
        labsize.top += sc10;
        labsize.bottom += sc10 + sc8;

        RECT vp;
        SWELL_GetViewPort(&vp,NULL,true);
        vp.bottom -= vp.top;
        if (labsize.bottom > vp.bottom*7/8)
          labsize.bottom = vp.bottom*7/8;


        int x;
        int button_height=0, button_total_w=0;;
        const int bspace = SWELL_UI_SCALE(button_spacing);
        int button_sizes[3];
        const int nbuttons = b3 ? 3 : b2 ? 2 : 1;
        for (x = 0; x < nbuttons; x ++)
        {
          const int idx = x==0?b1:x==1?b2:b3;
          RECT r={0,0,35,12};
          DrawText(dc,mbidtostr(idx),-1,&r,DT_CALCRECT|DT_NOPREFIX|DT_SINGLELINE);
          button_sizes[x] = r.right-r.left + sc10;
          button_total_w += button_sizes[x] + (x ? bspace : 0);
          if (r.bottom-r.top+sc10 > button_height) button_height = r.bottom-r.top+sc10;
        }

        if (labsize.right < button_total_w+sc8*2) labsize.right = button_total_w+sc8*2;

        int xpos = labsize.right/2 - button_total_w/2;
        const int def_id = (fmode & MB_DEFBUTTON3)&&b3 ? b3 : (fmode & MB_DEFBUTTON2)&&b2 ? b2 : b1;

        for (x = 0; x < nbuttons; x ++)
        {
          const int idx = x==0?b1:x==1?b2:b3;
          SWELL_MakeButton(idx==def_id,mbidtostr(idx),idx,xpos,labsize.bottom,button_sizes[x],button_height,0);
          xpos += button_sizes[x] + bspace;
        }

        if (dc) ReleaseDC(lab,dc);
        SWELL_MakeSetCurParms(1,1,0,0,NULL,false,false);
        SetWindowPos(hwnd,NULL,0,0,
              labsize.right + sc8*2,labsize.bottom + button_height + sc8,SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOMOVE);
        if (lab) SetWindowPos(lab,NULL,sc8,0,labsize.right,labsize.bottom,SWP_NOACTIVATE|SWP_NOZORDER);
        SetFocus(GetDlgItem(hwnd,def_id));
      }
    break;
    case WM_SIZE:
      {
        RECT r;
        GetClientRect(hwnd,&r);
        HWND h = GetWindow(hwnd,GW_CHILD);
        int n = 10, w[8];
        HWND tab[8],lbl=NULL;
        int tabsz=0, bxwid=0, button_height=0;
        while (h && n-- && tabsz<8) 
        {
          int idx = GetWindowLong(h,GWL_ID);
          if (idx == IDCANCEL || idx == IDOK || idx == IDNO || idx == IDYES || idx == IDRETRY) 
          { 
            RECT tr;
            GetClientRect(h,&tr);
            tab[tabsz] = h;
            w[tabsz++] = tr.right - tr.left;
            button_height = tr.bottom-tr.top;
            bxwid += tr.right-tr.left;
          } else if (idx==IDC_LABEL) lbl=h;
          h = GetWindow(h,GW_HWNDNEXT);
        }
        const int bspace = SWELL_UI_SCALE(button_spacing), sc8 = SWELL_UI_SCALE(8);
        if (lbl) SetWindowPos(lbl,NULL,sc8,0,r.right,r.bottom - sc8 - button_height,  SWP_NOZORDER|SWP_NOACTIVATE);
        int xo = r.right/2 - (bxwid + (tabsz-1)*bspace)/2;
        for (int x=0; x<tabsz; x++)
        {
          SetWindowPos(tab[x],NULL,xo,r.bottom - button_height - sc8, 0,0, SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
          xo += w[x] + bspace;
        }
      }
    break;
    case WM_COMMAND:
      if (LOWORD(wParam) && HIWORD(wParam) == BN_CLICKED) 
      {
        EndDialog(hwnd,LOWORD(wParam));
      }
    break;
    case WM_CLOSE:
      if (GetDlgItem(hwnd,IDCANCEL)) EndDialog(hwnd,IDCANCEL);
      else if (GetDlgItem(hwnd,IDNO)) EndDialog(hwnd,IDNO);
      else EndDialog(hwnd,IDOK);
    break;
  }
  return 0;
}

int MessageBox(HWND hwndParent, const char *text, const char *caption, int type)
{
#ifndef SWELL_LICE_GDI
  printf("MessageBox: %s %s\n",text,caption);
#endif
  const void *parms[4]= {text,caption,(void*)(INT_PTR)type} ;
  return DialogBoxParam(NULL,NULL,hwndParent,swellMessageBoxProc,(LPARAM)parms);

#if 0
  int ret=0;
  
  if (type == MB_OK)
  {
    // todo
    ret=IDOK;
  }	
  else if (type == MB_OKCANCEL)
  {
    ret = 1; // todo
    if (ret) ret=IDOK;
    else ret=IDCANCEL;
  }
  else if (type == MB_YESNO)
  {
    ret = 1 ; // todo
    if (ret) ret=IDYES;
    else ret=IDNO;
  }
  else if (type == MB_RETRYCANCEL)
  {
    ret = 1; // todo

    if (ret) ret=IDRETRY;
    else ret=IDCANCEL;
  }
  else if (type == MB_YESNOCANCEL)
  {
    ret = 1; // todo

    if (ret == 1) ret=IDYES;
    else if (ret==-1) ret=IDNO;
    else ret=IDCANCEL;
  }
  
  return ret; 
#endif
}

#ifdef SWELL_LICE_GDI
struct ChooseColor_State {
  int ncustom;
  COLORREF *custom;

  double h,s,v;

  LICE_IBitmap *bm;
};

static double h6s2i(double h)
{
  h -= ((int)(h*1.0/6.0))*6.0;
  if (h < 3)
  {
    if (h < 1.0) return 1.0 - h;
    return 0.0;
  }
  if (h < 4.0) return h - 3.0;
  return 1.0;
};

static void _HSV2RGB(double h, double s, double v, double *r, double *g, double *b)
{
  h *= 1.0 / 60.0; 
  s *= v * 1.0 / 255.0;
  *r = v-h6s2i(h+2)*s;
  *g = v-h6s2i(h)*s;
  *b = v-h6s2i(h+4)*s;
}
static int _HSV2RGBV(double h, double s, double v)
{
  double r,g,b;
  _HSV2RGB(h,s,v,&r,&g,&b);
  int ir = (int) (r+0.5);
  int ig = (int) (g+0.5);
  int ib = (int) (b+0.5);
  if (ir<0) ir=0; else if (ir>255) ir=255;
  if (ig<0) ig=0; else if (ig>255) ig=255;
  if (ib<0) ib=0; else if (ib>255) ib=255;
  return RGB(ir,ig,ib);
}


static void _RGB2HSV(double r, double g, double b, double *h, double *s, double *v)
{
  const double maxrgb=wdl_max(wdl_max(r,g),b);
  const double df = maxrgb - wdl_min(wdl_min(r,g),b);
  double d=r-g, degoffs = 240.0;

  if (g > r)
  {
    if (g > b)
    {
      degoffs=120;
      d=b-r;
    }
  }
  else if (r > b)
  {
    degoffs=0.0;
    d=g-b;
  }
  
  *v = maxrgb;
  if (df)
  {
    degoffs += (d*60)/df;
    if (degoffs<0.0) degoffs+=360.0;
    else if (degoffs >= 360.0) degoffs-=360.0;
    *h = degoffs;
    *s = (df*256)/(maxrgb+1);
  }
  else
    *h = *s = 0;
}

static LRESULT WINAPI swellColorSelectProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  static int s_reent,s_vmode;
  static int wndw, custsz, buth, border, butw, edh, edlw, edew, vsize, psize, yt;
  if (!wndw)
  {
    wndw = SWELL_UI_SCALE(400);
    custsz = SWELL_UI_SCALE(20);
    butw = SWELL_UI_SCALE(50);
    buth = SWELL_UI_SCALE(24);
    border = SWELL_UI_SCALE(4);
    edh = SWELL_UI_SCALE(20);
    edlw = SWELL_UI_SCALE(16);
    edew = SWELL_UI_SCALE(40);
    vsize = SWELL_UI_SCALE(40);
    psize = border+edlw + edew;
    yt = border + psize + border + (edh + border)*6;
  }

  const int customperrow = (wndw-border)/(custsz+border);
  switch (uMsg)
  {
    case WM_CREATE:
      if (lParam)  // swell-specific
      {
        SetWindowLong(hwnd,GWL_WNDPROC,(LPARAM)SwellDialogDefaultWindowProc);
        SetWindowLong(hwnd,DWL_DLGPROC,(LPARAM)swellColorSelectProc);
        SetWindowLongPtr(hwnd,GWLP_USERDATA,lParam);

        SetWindowText(hwnd,"Choose Color");

        SWELL_MakeSetCurParms(1,1,0,0,hwnd,false,false);

        SWELL_MakeButton(0, "OK", IDOK,0,0,0,0, 0);
        SWELL_MakeButton(0, "Cancel", IDCANCEL,0,0,0,0, 0);
        SWELL_MakeLabel(0, "(right click a custom color to save)", 0x500, 0,0,0,0, 0); 

        static const char * const lbl[] = { "R","G","B","H","S","V"};
        for (int x=0;x<6;x++)
        {
          SWELL_MakeLabel(0,lbl[x], 0x100+x, 0,0,0,0, 0); 
          SWELL_MakeEditField(0x200+x, 0,0,0,0,  0);
        }

        ChooseColor_State *cs = (ChooseColor_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
        SWELL_MakeSetCurParms(1,1,0,0,NULL,false,false);
        int nrows = ((cs?cs->ncustom : 0 ) + customperrow-1)/wdl_max(customperrow,1);
        SetWindowPos(hwnd,NULL,0,0, wndw, 
            yt + buth + border + nrows * (custsz+border), 
            SWP_NOZORDER|SWP_NOMOVE);
        SendMessage(hwnd,WM_USER+100,0,3);
      }
    break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
      {
        ChooseColor_State *cs = (ChooseColor_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
        if (!cs) break;
        RECT r;
        GetClientRect(hwnd,&r);
        const int xt = r.right - edew - edlw - border*3;

        const int y = GET_Y_LPARAM(lParam);
        const int x = GET_X_LPARAM(lParam);
        if (x < xt && y < yt)
        {
          s_vmode = x >= xt-vsize;
          SetCapture(hwnd);
          // fall through
        }
        else 
        {
          if (cs->custom && cs->ncustom && y >= yt && y < r.bottom - buth - border)
          {
            int row = (y-yt) / (custsz+border), rowoffs = (y-yt) % (custsz+border);
            if (rowoffs < custsz)
            {
              int col = (x-border) / (custsz+border), coloffs = (x-border) % (custsz+border);
              if (coloffs < custsz)
              {
                col += customperrow*row;
                if (col >= 0 && col < cs->ncustom)
                {
                  if (uMsg == WM_LBUTTONDOWN)
                  {
                    _RGB2HSV(GetRValue(cs->custom[col]),GetGValue(cs->custom[col]),GetBValue(cs->custom[col]),&cs->h,&cs->s,&cs->v);
                    SendMessage(hwnd,WM_USER+100,0,3);
                  }
                  else
                  {
                    cs->custom[col] = _HSV2RGBV(cs->h,cs->s,cs->v);
                    InvalidateRect(hwnd,NULL,FALSE);
                  }
                }
              }
            }
          }
          break;
        }
        // fall through
      }
    case WM_MOUSEMOVE:
      if (GetCapture()==hwnd)
      {
        RECT r;
        GetClientRect(hwnd,&r);
        const int xt = r.right - edew - edlw - border*3;
        ChooseColor_State *cs = (ChooseColor_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
        if (!cs) break;
        int var = 255 - (GET_Y_LPARAM(lParam) - border)*256 / (yt-border*2);
        if (var<0)var=0;
        else if (var>255)var=255;
        if (s_vmode)
        {
          if (var != cs->v)
          {
            cs->v=var;
            SendMessage(hwnd,WM_USER+100,0,3);
          }
        }
        else
        {
          int hue = (GET_X_LPARAM(lParam) - border)*360 / (xt-border - vsize);
          if (hue<0) hue=0;
          else if (hue>359) hue=359;
          if (cs->h != hue || cs->s != var)
          {
            cs->h=hue;
            cs->s=var;
            SendMessage(hwnd,WM_USER+100,0,3);
          }
        }
      }
    break;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
      ReleaseCapture();
    break;
    case WM_PAINT:
      {
        PAINTSTRUCT ps;
        ChooseColor_State *cs = (ChooseColor_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
        if (cs && BeginPaint(hwnd,&ps))
        {
          RECT r;
          GetClientRect(hwnd,&r);
          const int xt = r.right - edew - edlw - border*3;
          if (cs->custom && cs->ncustom)
          {
            int ypos = yt;
            int xpos = border;
            for (int x = 0; x < cs->ncustom; x ++)
            {
              HBRUSH br = CreateSolidBrush(cs->custom[x]);
              RECT tr={xpos,ypos,xpos+custsz, ypos+custsz };
              FillRect(ps.hdc,&tr,br);
              DeleteObject(br);

              xpos += border+custsz;
              if (xpos+custsz >= r.right)
              {
                xpos=border;
                ypos += border + custsz;
              }
            }
          }

          {
            HBRUSH br = CreateSolidBrush(_HSV2RGBV(cs->h,cs->s,cs->v));
            RECT tr={r.right - border - psize, border, r.right-border, border+psize};
            FillRect(ps.hdc,&tr,br);
            DeleteObject(br);
          }

          if (!cs->bm) cs->bm = new LICE_SysBitmap(xt-border,yt-border);
          else cs->bm->resize(xt-border,yt-border);

          int x1 = xt - border - vsize;
          int var = cs->v;

          const int ysz = yt-border*2;
          const int vary = ysz - 1 - (ysz * cs->v)/256;

          for (int y = 0; y < ysz; y ++)
          {
            LICE_pixel *wr = cs->bm->getBits() + cs->bm->getRowSpan() * y;
            const int sat = 255 - y*256/ysz;
            double xx=0.0, dx=384.0/x1;
            int x;
            for (x = 0; x < x1; x++)
            {
              *wr++ = LICE_HSV2Pix((int)(xx+0.5),sat,var,255);
              xx+=dx;
            }
            LICE_pixel p = LICE_HSV2Pix(cs->h * 384.0/360.0,cs->s,sat ^ (y==vary ? 128 : 0),255);
            for (;x < xt-border;x++) *wr++ = p;
          }
          LICE_pixel p = LICE_HSV2Pix((int)(cs->h+0.5),(int)(cs->s+0.5),((int)(128.5+cs->v))&255,255);
          const int saty = ysz - 1 - (int) (ysz * cs->s + 0.5)/256;
          const int huex = (x1*cs->h)/360;
          LICE_Line(cs->bm,huex,saty-4,huex,saty+4,p,.75f,LICE_BLIT_MODE_COPY,false);
          LICE_Line(cs->bm,huex-4,saty,huex+4,saty,p,.75f,LICE_BLIT_MODE_COPY,false);

          BitBlt(ps.hdc,border,border,xt-border,ysz,cs->bm->getDC(),0,0,SRCCOPY);

          EndPaint(hwnd,&ps);
        }
      }

    break;
    case WM_GETMINMAXINFO:
      {
        LPMINMAXINFO p=(LPMINMAXINFO)lParam;
        p->ptMinTrackSize.x = 300;
        p->ptMinTrackSize.y = 300;
      }
    break;
    case WM_USER+100:
      {
        ChooseColor_State *cs = (ChooseColor_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
        if (cs)
        {
          double t[6];
          t[3] = cs->h;
          t[4] = cs->s;
          t[5] = cs->v;
          _HSV2RGB(t[3],t[4],t[5],t,t+1,t+2);
          s_reent++;
          for (int x=0;x<6;x++) if (lParam & ((x<3)?1:2)) SetDlgItemInt(hwnd,0x200+x,(int) (t[x]+0.5),FALSE);
          s_reent--;
          InvalidateRect(hwnd,NULL,FALSE);
        }
      }
    break;
    case WM_SIZE:
      {
        RECT r;
        GetClientRect(hwnd,&r);
        int tx = r.right - edew-edlw-border*2, ty = border*2 + psize;
        for (int x=0;x<6;x++)
        {
          SetWindowPos(GetDlgItem(hwnd,0x100+x),NULL,tx, ty, edlw, edh, SWP_NOZORDER|SWP_NOACTIVATE);
          SetWindowPos(GetDlgItem(hwnd,0x200+x),NULL,tx+edlw+border, ty, edew, edh, SWP_NOZORDER|SWP_NOACTIVATE);
          ty += border+edh;
        }

        r.right -= border + butw;
        r.bottom -= border + buth;
        SetWindowPos(GetDlgItem(hwnd,IDCANCEL), NULL, r.right, r.bottom, butw, buth, SWP_NOZORDER|SWP_NOACTIVATE);
        r.right -= border*2 + butw;
        SetWindowPos(GetDlgItem(hwnd,IDOK), NULL, r.right, r.bottom, butw, buth, SWP_NOZORDER|SWP_NOACTIVATE);

        SetWindowPos(GetDlgItem(hwnd,0x500), NULL, border, r.bottom, r.right-border*2, buth, SWP_NOZORDER|SWP_NOACTIVATE);
      }
    break;
    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDCANCEL:
          EndDialog(hwnd,0);
        break;
        case IDOK:
          EndDialog(hwnd,1);
        break;
        case 0x200:
        case 0x201:
        case 0x202:
        case 0x203:
        case 0x204:
        case 0x205:
          if (!s_reent)
          {
            const bool ishsv = LOWORD(wParam) >= 0x203;
            int offs = ishsv ? 0x203 : 0x200;
            BOOL t = FALSE;
            double h = GetDlgItemInt(hwnd,offs++,&t,FALSE);
            if (!t) break;
            double s = GetDlgItemInt(hwnd,offs++,&t,FALSE);
            if (!t) break;
            double v = GetDlgItemInt(hwnd,offs++,&t,FALSE);
            if (!t) break;
            if (s<0) s=0; else if (s>255) s=255;
            if (v<0) v=0; else if (v>255) v=255;
            if (h<0) h=0;

            if (!ishsv) 
            {
              if (h>255) h=255;
              _RGB2HSV(h,s,v,&h,&s,&v);
            }
            else
            {
              if (h>360) h=360;
            }

            ChooseColor_State *cs = (ChooseColor_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
            if (cs)
            {
              cs->h = h;
              cs->s = s;
              cs->v = v;
            }
            SendMessage(hwnd,WM_USER+100,0,ishsv?1:2);
          }
        break;
      }
    break;

  }
  return 0;
}
#endif //SWELL_LICE_GDI

bool SWELL_ChooseColor(HWND h, COLORREF *val, int ncustom, COLORREF *custom)
{
#ifdef SWELL_LICE_GDI
  ChooseColor_State state = { ncustom, custom };
  COLORREF c = val ? *val : 0;
  _RGB2HSV(GetRValue(c),GetGValue(c),GetBValue(c),&state.h,&state.s,&state.v);
  bool rv = DialogBoxParam(NULL,NULL,h,swellColorSelectProc,(LPARAM)&state)!=0;
  delete state.bm;
  if (rv && val) 
  {
    *val = _HSV2RGBV(state.h,state.s,state.v);
  }
  return rv;
#else
  return false;
#endif
}

#if defined(SWELL_FREETYPE) && defined(SWELL_LICE_GDI)

struct FontChooser_State
{
  FontChooser_State()
  {
    hFont = 0;
  }
  ~FontChooser_State()
  {
    DeleteObject(hFont);
  }

  LOGFONT font;
  HFONT hFont;
  WDL_FastString lastfn;
};

extern const char *swell_last_font_filename;

const char *swell_enumFontFiles(int x);
int swell_getLineLength(const char *buf, int *post_skip, int wrap_maxwid, HDC hdc);

static LRESULT WINAPI swellFontChooserProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  enum { IDC_LIST=0x100, IDC_FACE, IDC_SIZE, IDC_WEIGHT, IDC_ITALIC };
  enum { preview_h = 90, _border = 4, _buth = 24 };

  switch (uMsg)
  {
    case WM_CREATE:
      if (lParam)  // swell-specific
      {
        SetWindowLong(hwnd,GWL_WNDPROC,(LPARAM)SwellDialogDefaultWindowProc);
        SetWindowLong(hwnd,DWL_DLGPROC,(LPARAM)swellFontChooserProc);
        SetWindowLongPtr(hwnd,GWLP_USERDATA,lParam);

        SetWindowText(hwnd,"Choose Font");

        SWELL_MakeSetCurParms(1,1,0,0,hwnd,false,false);

        SWELL_MakeButton(0, "OK", IDOK,0,0,0,0, 0);
        SWELL_MakeButton(0, "Cancel", IDCANCEL,0,0,0,0, 0);
        SWELL_MakeListBox(IDC_LIST,0,0,0,0, LBS_OWNERDRAWFIXED);
        SWELL_MakeEditField(IDC_FACE, 0,0,0,0,  0);
        SWELL_MakeEditField(IDC_SIZE, 0,0,0,0,  0);
        SWELL_MakeCombo(IDC_WEIGHT, 0,0,0,0, CBS_DROPDOWNLIST);
        SWELL_MakeCheckBox("Italic",IDC_ITALIC,0,0,0,0, 0);

        SendDlgItemMessage(hwnd,IDC_WEIGHT,CB_ADDSTRING,0,(LPARAM)"Normal");
        SendDlgItemMessage(hwnd,IDC_WEIGHT,CB_ADDSTRING,0,(LPARAM)"Bold");
        SendDlgItemMessage(hwnd,IDC_WEIGHT,CB_ADDSTRING,0,(LPARAM)"Light");

        SWELL_MakeSetCurParms(1,1,0,0,NULL,false,false);

        SetWindowPos(hwnd,NULL,0,0, 550,380, SWP_NOZORDER|SWP_NOMOVE);

        WDL_StringKeyedArray<char> list;
        const char *fontfile;
        int x;
        for (x=0; (fontfile=swell_enumFontFiles(x)); x ++)
        {
          char buf[512];
          lstrcpyn_safe(buf,WDL_get_filepart(fontfile),sizeof(buf));
          char *tmp = buf;
          while (*tmp && *tmp != '-' && *tmp != '.') tmp++;
          *tmp=0;
          if (*buf) list.AddUnsorted(buf,true);
        }
        swell_enumFontFiles(-1); // clear cache
        list.Resort();
        FontChooser_State *cs = (FontChooser_State*)lParam;
        bool italics = cs->font.lfItalic!=0;
        int wt = cs->font.lfWeight;
        const char *lp=NULL;
        int cnt=0;
        for (x=0;x<list.GetSize();x++)
        {
          const char *p=NULL;
          if (list.Enumerate(x,&p) && p)
          {
            if (!stricmp(p,cs->font.lfFaceName))
              SendDlgItemMessage(hwnd,IDC_LIST,LB_SETCURSEL,cnt,0);

            size_t ll;
            if (lp && !strncmp(p,lp,ll=strlen(lp)))
            {
              // if this is an extension of the last one, skip
              const char *trail = p+ll;
              if (strlen(trail)<=2)
              {
                for (int y=0;y<2;y++)
                {
                  char c = *trail;
                  if (c>0) c=toupper_safe(c);
                  if (c == 'B' || c == 'I' || c == 'L') trail++;
                }
              }
              else while (*trail)
              {
                if (!strnicmp(trail,"Bold",4)) trail+=4;
                else if (!strnicmp(trail,"Light",5)) trail+=5;
                else if (!strnicmp(trail,"Italic",6)) trail+=6;
                else if (!strnicmp(trail,"Oblique",7)) trail+=7;
                else break;
              }
              if (!*trail) continue;
            }
            cnt++;
            SendDlgItemMessage(hwnd,IDC_LIST,LB_ADDSTRING,0,(LPARAM)p);
            lp=p;
          }
        }
        SetDlgItemText(hwnd,IDC_FACE,cs->font.lfFaceName);
        if (cs->font.lfHeight > 0)
        {
          HFONT font = CreateFontIndirect(&cs->font);
          if (font)
          {
            HDC hdc = GetDC(hwnd);
            if (hdc)
            {
              HGDIOBJ oldfont = SelectObject(hdc,font);
              TEXTMETRIC tm = { 0, };
              GetTextMetrics(hdc,&tm);
              if (tm.tmHeight > 0) cs->font.lfHeight = -tm.tmHeight;
              SelectObject(hdc,oldfont);
              ReleaseDC(hwnd,hdc);
            }
            DeleteObject(font);
          }
        }
        SetDlgItemInt(hwnd,IDC_SIZE,cs->font.lfHeight < 0 ? -cs->font.lfHeight : cs->font.lfHeight,TRUE);
        SendDlgItemMessage(hwnd,IDC_WEIGHT,CB_SETCURSEL,wt<=FW_LIGHT ? 2 : wt < FW_BOLD ? 0 : 1,0);
        if (italics)
          CheckDlgButton(hwnd,IDC_ITALIC,BST_CHECKED);
      }
    break;
    case WM_DRAWITEM:
    {
      DRAWITEMSTRUCT *di=(DRAWITEMSTRUCT *)lParam;
      FontChooser_State *cs = (FontChooser_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
      if (cs && di->CtlID == IDC_LIST)
      {
        char buf[512];
        buf[0]=0;
        SendDlgItemMessage(hwnd,IDC_LIST,LB_GETTEXT,di->itemID,(WPARAM)buf);
        if (buf[0])
        {
          HFONT font = CreateFont(-wdl_abs(g_swell_ctheme.default_font_size), 0, 0, 0,
              cs->font.lfWeight, cs->font.lfItalic,
              FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, buf);

          HGDIOBJ oldFont = SelectObject(di->hDC,font);
          DrawText(di->hDC,buf,-1,&di->rcItem,DT_VCENTER|DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
          wchar_t tmp[] = {'a','A','z','Z'};
          unsigned short ind[4];
          GetGlyphIndicesW(di->hDC,tmp,4,ind,0);
          SelectObject(di->hDC,oldFont);

          int x;
          for (x=0;x<4 && ind[x]==0xffff;x++);
          if (x==4)
          {
            RECT r = di->rcItem;
            r.right-=4;
            DrawText(di->hDC,buf,-1,&r,DT_VCENTER|DT_RIGHT|DT_SINGLELINE|DT_NOPREFIX);
          }
          DeleteObject(font);

        }
      }
    }
    return 0;
    case WM_PAINT:
      {
        PAINTSTRUCT ps;
        FontChooser_State *cs = (FontChooser_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
        if (cs && BeginPaint(hwnd,&ps))
        {
          RECT r;
          GetClientRect(hwnd,&r);

          const int border = SWELL_UI_SCALE(_border);
          const int buth = SWELL_UI_SCALE(_buth);
          const int ph = SWELL_UI_SCALE(preview_h);
          r.left += border;
          r.right -= border;
          r.bottom -= border*2 + buth;
          r.top = r.bottom - ph;

          HFONT f = CreateFontIndirect(&cs->font);

          HBRUSH br = CreateSolidBrush(RGB(255,255,255));
          FillRect(ps.hdc,&r,br);
          DeleteObject(br);
          SetTextColor(ps.hdc,RGB(0,0,0));
          SetBkMode(ps.hdc,TRANSPARENT);
          r.right-=4;
          r.left+=4;
          if (swell_last_font_filename)
          {
            r.bottom -= DrawText(ps.hdc,swell_last_font_filename,-1,&r,DT_BOTTOM|DT_NOPREFIX|DT_SINGLELINE|DT_RIGHT);
          }

          HGDIOBJ oldFont = SelectObject(ps.hdc,f);

          extern const char *g_swell_fontpangram;
          const char *str = g_swell_fontpangram;
          //
          // thanks, http://dailypangram.tumblr.com/ :)
          if (!str) str = "Strangely, aerobic exercise doesn’t quite work with improvised free jazz.";

          while (*str)
          {
            int sk=0, lb=swell_getLineLength(str, &sk, r.right-r.left, ps.hdc);
            if (!lb&&!sk) break;
            if (lb>0) r.top += DrawText(ps.hdc,str,lb,&r,DT_TOP|DT_LEFT|DT_NOPREFIX|DT_SINGLELINE);
            str+=lb+sk;
          }


          SelectObject(ps.hdc,oldFont);
          DeleteObject(f);


          EndPaint(hwnd,&ps);
        }
      }

    break;
    case WM_GETMINMAXINFO:
      {
        LPMINMAXINFO p=(LPMINMAXINFO)lParam;
        p->ptMinTrackSize.x = 400;
        p->ptMinTrackSize.y = 300;
      }
    break;
    case WM_SIZE:
      {
        RECT r;
        GetClientRect(hwnd,&r);
        const int border = SWELL_UI_SCALE(_border);
        const int buth = SWELL_UI_SCALE(_buth);
        const int butw = SWELL_UI_SCALE(50);
        const int edh = SWELL_UI_SCALE(20);
        const int size_w = SWELL_UI_SCALE(50);
        const int wt_w = SWELL_UI_SCALE(80);
        const int italic_w = SWELL_UI_SCALE(60);

        r.left += border;
        r.right -= border;

        r.bottom -= border + buth;
        SetWindowPos(GetDlgItem(hwnd,IDCANCEL), NULL, r.right - butw, r.bottom, butw, buth, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd,IDOK), NULL, r.right - border - butw*2, r.bottom, butw, buth, SWP_NOZORDER|SWP_NOACTIVATE);
        r.bottom -= SWELL_UI_SCALE(preview_h) + border;
        int psz=wdl_max(g_swell_ctheme.combo_height,edh);
        r.bottom -= psz + border;
        SetWindowPos(GetDlgItem(hwnd,IDC_FACE),NULL,r.left,r.bottom + (psz-edh)/2, 
            r.right-r.left - size_w-wt_w-italic_w - border*3, edh, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd,IDC_SIZE),NULL,r.right-size_w-wt_w-italic_w-border*2,r.bottom + (psz-edh)/2, 
            size_w, edh, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd,IDC_WEIGHT),NULL,r.right-wt_w-italic_w-border,r.bottom + (psz-g_swell_ctheme.combo_height)/2, 
            wt_w, g_swell_ctheme.combo_height, SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(GetDlgItem(hwnd,IDC_ITALIC),NULL,r.right-italic_w,r.bottom + (psz-edh)/2, 
            italic_w, edh, SWP_NOZORDER|SWP_NOACTIVATE);

        SetWindowPos(GetDlgItem(hwnd,IDC_LIST), NULL, border, border, r.right, r.bottom - border*2, SWP_NOZORDER|SWP_NOACTIVATE);


      }
    break;
    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDC_LIST:
          if (HIWORD(wParam) == LBN_SELCHANGE)
          {
            int idx = (int) SendDlgItemMessage(hwnd,IDC_LIST,LB_GETCURSEL,0,0);
            if (idx>=0)
            {
              char buf[512];
              buf[0]=0;
              SendDlgItemMessage(hwnd,IDC_LIST,LB_GETTEXT,idx,(WPARAM)buf);
              if (buf[0]) SetDlgItemText(hwnd,IDC_FACE,buf);
            }
          }
        break;
        case IDC_SIZE:
        case IDC_FACE:
        case IDC_ITALIC:
        case IDC_WEIGHT:
        {
          FontChooser_State *cs = (FontChooser_State*)GetWindowLongPtr(hwnd,GWLP_USERDATA);
          if (cs) 
          {
            if (LOWORD(wParam) == IDC_FACE)
              GetDlgItemText(hwnd,IDC_FACE,cs->font.lfFaceName,sizeof(cs->font.lfFaceName));
            else if (LOWORD(wParam) == IDC_SIZE)
            {
              BOOL t;
              int a = GetDlgItemInt(hwnd,IDC_SIZE,&t,FALSE);
              if (t && a > 0)
                cs->font.lfHeight = -a;
            }
            else if (LOWORD(wParam) == IDC_ITALIC) cs->font.lfItalic = IsDlgButtonChecked(hwnd,IDC_ITALIC) ? 1:0;
            else if (LOWORD(wParam) == IDC_WEIGHT && HIWORD(wParam) == CBN_SELCHANGE)
            {
              int idx = (int) SendDlgItemMessage(hwnd,IDC_WEIGHT,CB_GETCURSEL,0,0);
              if (idx==0) cs->font.lfWeight = FW_NORMAL;
              else if (idx==1) cs->font.lfWeight = FW_BOLD;
              else if (idx==2) cs->font.lfWeight = FW_LIGHT;
            }
            InvalidateRect(hwnd,NULL,FALSE);
          }
        }
        break;
        case IDCANCEL:
          EndDialog(hwnd,0);
        break;
        case IDOK:
          EndDialog(hwnd,1);
        break;
      }
    break;

  }
  return 0;
}

void *swell_MatchFont(const char *lfFaceName, int weight, int italic, const char **fnOut);

#endif

bool SWELL_ChooseFont(HWND h, LOGFONT *lf)
{
#if defined(SWELL_FREETYPE) && defined(SWELL_LICE_GDI)
  FontChooser_State state;
  state.font = *lf;

  bool rv = DialogBoxParam(NULL,NULL,h,swellFontChooserProc,(LPARAM)&state)!=0;
  if (rv)
  {
    *lf = state.font;
  }
  return rv;
#else
  return false;
#endif
}

#endif
