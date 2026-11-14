#pragma once

#include <Tempest/Widget>
#include <Tempest/Texture2d>
#include <Tempest/Timer>

class KeyCodec;

class DocumentMenu : public Tempest::Widget {
  public:
    DocumentMenu(const KeyCodec& key);

    enum Flags : uint8_t {
      F_None,
      F_Margin=1,
      F_Backgr=2,
      F_Font  =4
      };

    struct Page {
      std::string     img;
      std::string     text;
      std::string     font;
      Tempest::Margin margins;
      Flags           flg=F_None;
      };

    // TODO: set default values for these
    struct Show {
      std::vector<Page> pages;
      std::string       font;
      Tempest::Margin   margins;
      std::string       img;
      // map
      bool              showPlayer=false;
      Tempest::Rect     wbounds;
      // NB fix 51: a note or a book of New Balance (RX_OpenNote, RX_ToggleBookManagerWithSize of the Union plugin):
      // one texture over a part of the screen, the text in columns that go on to the next spread
      struct NbColumn {
        float x0=0, y0=0, x1=1, y1=1;   // in parts of the texture (0..1)
        };
      struct NbSheet {
        std::string           img;
        std::string           text;
        float                 w = 0;     // the size in parts of the screen (16:9), as the plugin gives it in %
        float                 h = 0;
        std::vector<NbColumn> columns;
        };
      NbSheet           nb;
      };

    void show(const Show& doc);
    bool isActive() const { return active; }
    void close();
    void tick(uint64_t dt);

    void keyDownEvent(Tempest::KeyEvent &e);
    void keyUpEvent  (Tempest::KeyEvent &e);

  protected:
    void paintEvent(Tempest::PaintEvent& e);

  private:
    void paintNb(Tempest::Painter& p);
    bool nbHasNext() const;

    DocumentMenu::Show        document;
    const KeyCodec&           keycodec;
    const Tempest::Texture2d* cursor = nullptr;
    bool                      active = false;
    int                       nbSpread  = 0;  // NB fix 51: the spread of the book on the screen
    int                       nbLines   = 0;  // the lines of the text in one column and in the whole text,
    int                       nbTotal   = 0;  // counted at the paint (they depend on the font and the screen)
  };

