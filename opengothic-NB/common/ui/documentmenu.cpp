#include "documentmenu.h"

#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "utils/gthfont.h"
#include "utils/keycodec.h"
#include "gothic.h"

#include <algorithm>

using namespace Tempest;

DocumentMenu::DocumentMenu(const KeyCodec& key)
  :keycodec(key) {
  setFocusPolicy(NoFocus);
  cursor = Resources::loadTexture("U.TGA");
  }

void DocumentMenu::show(const DocumentMenu::Show &doc) {
  document = doc;
  active   = true;
  nbSpread = 0;
  nbLines  = 0;
  nbTotal  = 0;
  update();
  }

void DocumentMenu::close() {
  if(!active)
    return;
  active=false;

  if(auto pl = Gothic::inst().player())
    pl->setInteraction(nullptr);
  }

void DocumentMenu::keyDownEvent(KeyEvent &e) {
  if(!active){
    e.ignore();
    return;
    }

  if(!document.nb.img.empty()) {
    // NB fix 51: the spreads of a book. Escape (the button SKIP of the touch screen) turns the page, and closes
    // the book on the last one; the arrows go both ways
    if(e.key==Event::K_Left || e.key==Event::K_Up) {
      nbSpread = std::max(0,nbSpread-1);
      update();
      return;
      }
    if(e.key==Event::K_Right || e.key==Event::K_Down) {
      if(nbHasNext())
        ++nbSpread;
      update();
      return;
      }
    if(e.key==Event::K_ESCAPE && nbHasNext()) {
      ++nbSpread;
      update();
      return;
      }
    }

  if(e.key!=Event::K_ESCAPE && keycodec.tr(e)!=KeyCodec::Inventory){
    e.ignore();
    return;
    }
  close();
  }

bool DocumentMenu::nbHasNext() const {
  const int cols = int(document.nb.columns.size());
  return cols>0 && nbLines>0 && (nbSpread+1)*cols*nbLines<nbTotal;
  }

void DocumentMenu::paintNb(Tempest::Painter& p) {
  auto& nb   = document.nb;
  auto  back = Resources::loadTexture(nb.img);
  if(back==nullptr || nb.w<=0 || nb.h<=0)
    return;

  // the plugin gives the size in % of the screen; it is made for 16:9, so the shape stays as on a monitor
  float h = float(this->h())*nb.h;
  float w = h*(nb.w*16.f)/(nb.h*9.f);
  if(w>float(this->w())*0.98f) {
    h *= float(this->w())*0.98f/w;
    w  = float(this->w())*0.98f;
    }
  const int x0 = (this->w()-int(w))/2;
  const int y0 = (this->h()-int(h))/2;

  p.setBrush(*back);
  p.drawRect(x0,y0,int(w),int(h), 0,0,back->w(),back->h());

  const float scale = Gothic::interfaceScale(this);
  auto& fnt = Resources::font("FONT_10_BOOK.TGA",Resources::FontType::Normal,scale);
  const int ph = std::max(1,fnt.pixelSize());

  nbLines = 0;
  nbTotal = 0;
  const int cols = int(nb.columns.size());
  for(int i=0; i<cols; ++i) {
    auto& c  = nb.columns[size_t(i)];
    const int cx = x0+int(c.x0*w);
    const int cy = y0+int(c.y0*h);
    const int cw = std::max(1,int((c.x1-c.x0)*w));
    const int ch = std::max(1,int((c.y1-c.y0)*h));
    if(i==0) {
      nbLines = std::max(1,ch/ph);
      nbTotal = fnt.lineCount(cw,nb.text);
      }
    const int first = (nbSpread*cols+i)*nbLines;
    if(first>=nbTotal)
      break;
    fnt.drawText(p,cx,cy+ph,cw,nbLines*ph,nb.text,Tempest::AlignLeft,first);
    }

  if(nbSpread>0 || nbHasNext()) {
    // the number of the spread, under the texture
    const int  all = std::max(1,(nbTotal+cols*nbLines-1)/std::max(1,cols*nbLines));
    const auto num = std::to_string(nbSpread+1)+" / "+std::to_string(all);
    auto& f2 = Resources::font(scale);
    f2.drawText(p,(this->w()-f2.textSize(num).w)/2,std::min(this->h()-4,y0+int(h)+f2.pixelSize()+4),num);
    }
  }

void DocumentMenu::keyUpEvent(KeyEvent&) {
  }

void DocumentMenu::paintEvent(PaintEvent &e) {
  if(!active)
    return;

  if(!document.nb.img.empty()) {
    Painter p(e);
    paintNb(p);
    return;
    }

  const float scale = Gothic::interfaceScale(this);
  float mw = 0, mh = 0;
  for(auto& i:document.pages){
    auto back = Resources::loadTexture((i.flg&F_Backgr) ? i.img : document.img);
    if(!back)
      continue;
    mw += float(back->w())*scale;
    mh = std::max(mh,float(back->h())*scale);
    }

  float k  = std::min(1.f,float(800.f+float(document.margins.xMargin()))*scale/std::max(mw,1.f));

  int   x  = (w()-int(k*mw))/2, y = (h()-int(mh))/2;
  auto  pl = Gothic::inst().player();

  Painter p(e);
  for(auto& i:document.pages) {
    const GthFont* fnt = nullptr;
    if(i.flg&F_Font)
      fnt = &Resources::font(i.font, Resources::FontType::Normal, scale); else
      fnt = &Resources::font(document.font, Resources::FontType::Normal, scale);
    if(fnt==nullptr)
      fnt = &Resources::font(scale);
    auto back = Resources::loadTexture((i.flg&F_Backgr) ? i.img : document.img);
    if(!back)
      continue;

    auto mgr = (i.flg&F_Margin) ? i.margins : document.margins;
    mgr.left   = int(float(mgr.left)  *scale);
    mgr.right  = int(float(mgr.right) *scale);
    mgr.top    = int(float(mgr.top)   *scale);
    mgr.bottom = int(float(mgr.bottom)*scale);

    const int w = int(k*scale*float(back->w()));
    const int h = int(  scale*float(back->h()));

    p.setBrush(*back);
    p.drawRect(x,y,w,h,
               0,0,back->w(),back->h());

    p.setBrush(Color(0.04f,0.04f,0.04f,1));

    fnt->drawText(p,x+mgr.left,
                  y+mgr.top+fnt->pixelSize(),
                  w - mgr.xMargin(),
                  h - mgr.yMargin(),
                  i.text, Tempest::AlignLeft);

    if(document.showPlayer && cursor!=nullptr && pl!=nullptr) {
      auto  pos = pl->position();
      float wx  = (pos.x-float(document.wbounds.x))/float(document.wbounds.w);
      float wy  = (pos.z-float(document.wbounds.y))/float(document.wbounds.h);

      p.setBrush(*cursor);
      int cx = x+int(wx*float(w));
      int cy = y+int(wy*float(h));

      p.pushState();
      p.translate(cx,cy);
      p.rotate(-pl->rotation()-90);
      p.drawRect(-cursor->w()/2,-cursor->h()/2, cursor->w(),cursor->h());
      p.popState();
      }
    x+=w;
    }
  }
