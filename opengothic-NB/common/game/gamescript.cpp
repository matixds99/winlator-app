#include "gamescript.h"
#include "nboptions.h"
#include "nbiteminfo.h"
#include "nbbars.h"

#include <Tempest/Log>
#include <Tempest/SoundEffect>

#include <algorithm>
#include <array>
#include <cctype>
#include <type_traits>
#include <unordered_map>

#include "game/compatibility/directmemory.h"
#include "game/definitions/spelldefinitions.h"
#include "game/serialize.h"
#include "utils/string_frm.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/interactive.h"
#include "world/triggers/abstracttrigger.h"
#include "graphics/visualfx.h"
#include "utils/fileutil.h"
#include "commandline.h"
#include "gothic.h"
#include "game/dialogtest.h"

using namespace Tempest;

namespace {

// Returning New Balance (NB): the RX_* externals come from its own DLL and are not documented,
// so every binding checks the signature declared in GOTHIC.DAT before it is registered.
// An external whose signature differs is left to the default handler of the VM (it pops the
// parameters and returns 0/""/null), so a wrong guess can never break the stack.
template<class T> struct NbIsSharedPtr : std::false_type {};
template<class I> struct NbIsSharedPtr<std::shared_ptr<I>> : std::true_type {};

template<class T>
constexpr zenkit::DaedalusDataType nbType() {
  using U = std::decay_t<T>;
  if constexpr(std::is_same_v<U,int32_t>)
    return zenkit::DaedalusDataType::INT;
  else if constexpr(std::is_same_v<U,float>)
    return zenkit::DaedalusDataType::FLOAT;
  else if constexpr(std::is_same_v<U,std::string_view> || std::is_same_v<U,std::string>)
    return zenkit::DaedalusDataType::STRING;
  else {
    static_assert(NbIsSharedPtr<U>::value, "unsupported type of an NB external");
    return zenkit::DaedalusDataType::INSTANCE;
    }
  }

bool nbTypeMatches(zenkit::DaedalusDataType want, zenkit::DaedalusDataType got) {
  if(want==got)
    return true;
  // "var func" is passed as an int (the index of the symbol), as in Info_AddChoice
  return want==zenkit::DaedalusDataType::INT && got==zenkit::DaedalusDataType::FUNCTION;
  }

template<class F> struct NbSig : NbSig<decltype(&F::operator())> {};
template<class C, class R, class... A>
struct NbSig<R(C::*)(A...) const> {
  static bool matches(zenkit::DaedalusVm& vm, zenkit::DaedalusSymbol& s) {
    const auto params = vm.find_parameters_for_function(&s);
    const std::array<zenkit::DaedalusDataType, sizeof...(A)> want = {nbType<A>()...};
    if(params.size()!=want.size())
      return false;
    for(size_t i=0; i<want.size(); ++i)
      if(!nbTypeMatches(want[i],params[i].type()))
        return false;
    if constexpr(std::is_void_v<R>)
      return !s.has_return();
    else
      return s.has_return() && nbTypeMatches(nbType<R>(),s.rtype());
    }
  };

const char* nbTypeName(zenkit::DaedalusDataType t) {
  switch(t) {
    case zenkit::DaedalusDataType::VOID:     return "void";
    case zenkit::DaedalusDataType::FLOAT:    return "float";
    case zenkit::DaedalusDataType::INT:      return "int";
    case zenkit::DaedalusDataType::STRING:   return "string";
    case zenkit::DaedalusDataType::FUNCTION: return "func";
    case zenkit::DaedalusDataType::INSTANCE: return "instance";
    default:                                 return "?";
    }
  }

std::string nbSignature(zenkit::DaedalusVm& vm, zenkit::DaedalusSymbol& s) {
  std::string sig = std::string(s.has_return() ? nbTypeName(s.rtype()) : "void") + " " + std::string(s.name()) + "(";
  bool first = true;
  for(auto& p : vm.find_parameters_for_function(&s)) {
    if(!first)
      sig += ", ";
    sig += nbTypeName(p.type());
    first = false;
    }
  return sig + ")";
  }

// the diagnostic of the scripts of NB itself (RX_DEBUG_RETURN_SHOW + CMD_COLOR): "NB trace" in the log.
// false, once the AI of NB works
constexpr bool kNbScriptTrace = true;

// a delayed function of RX_CALLBACK*: at the time, with SELF = the character (if there is one)
struct NbCallback {
  uint64_t                      at         = 0;
  uint32_t                      func       = 0;
  std::shared_ptr<zenkit::INpc> npc;
  bool                          waitDialog = false;
  };

// the state of NB of one GameScript (kept here, so gamescript.h stays as it is)
struct NbState {
  std::vector<NbCallback>  callbacks;
  zenkit::DaedalusSymbol*  cycle           = nullptr;
  uint64_t                 lastCycleSecond = uint64_t(-1);
  // the loops that the Union plugin calls by the name (UNION_ABI.DLL):
  // a timer of 1000 ms: RX_Main_CycleTrigger, the next second b_gettime, and again; PerFrame every frame
  zenkit::DaedalusSymbol*  getTime         = nullptr;
  zenkit::DaedalusSymbol*  perFrame        = nullptr;
  zenkit::DaedalusSymbol*  checkMenu       = nullptr;
  zenkit::DaedalusSymbol*  frameTime       = nullptr;
  int                      unionCycle      = 0;
  uint64_t                 nextMenuCheck   = 0;
  uint64_t                 nextMoveset     = 0;   // the timer of 100 ms of fix 30
  };

std::unordered_map<const GameScript*, NbState> nbStates;

}


// NB: the hit chances and the fight skills of the NPC (B_SetFightSkills -> RX_ApplyProperOverlaysForNpc, fixes 23 and 30)
namespace NbMoveset {

  // B_SetFightSkills -> RX_ApplyProperOverlaysForNpc (UNION_ABI.DLL 0x10198B20), see the binding
  static void applyFightSkills(Npc& npc, int32_t percent) {
    auto& h = npc.handle();
    for(int t=TALENT_1H; t<=TALENT_CROSSBOW; ++t)
      h.hitchance[t] = std::clamp(h.hitchance[t]+percent, 0, 100);

    static const char* all[] = {
      "HUMANS_1HST1.MDS","HUMANS_1HST2.MDS","HUMANS_1HST3.MDS",
      "HUMANS_2HST1.MDS","HUMANS_2HST2.MDS","HUMANS_2HST3.MDS",
      "HUMANS_BOWT1.MDS","HUMANS_BOWT2.MDS","HUMANS_CBOWT1.MDS","HUMANS_CBOWT2.MDS",
      "HUMANS_2X2ST3.MDS",
      };
    for(auto name:all)
      if(auto sk = Resources::loadSkeleton(name))
        npc.delOverlay(sk);

    const auto apply = [&npc](Talent t, int lvl, const char* mds) {
      npc.setTalentSkill(t,lvl);
      if(mds!=nullptr)
        if(auto sk = Resources::loadSkeleton(mds))
          npc.addOverlay(sk,0);
      };
    const auto melee = [&](Talent t, const char* st1, const char* st2, const char* st3) {
      const int hc = h.hitchance[t];
      if(hc>=90)      apply(t,3,st3);
      else if(hc>=60) apply(t,2,st2);
      else if(hc>=30) apply(t,1,st1);
      else            apply(t,0,nullptr);
      };
    const auto range = [&](Talent t, const char* t1, const char* t2) {
      const int hc = h.hitchance[t];
      if(hc>=60)      apply(t,2,t2);
      else if(hc>=30) apply(t,1,t1);
      else            apply(t,0,nullptr);
      };
    melee(TALENT_1H,"HUMANS_1HST1.MDS","HUMANS_1HST2.MDS","HUMANS_1HST3.MDS");
    melee(TALENT_2H,"HUMANS_2HST1.MDS","HUMANS_2HST2.MDS","HUMANS_2HST3.MDS");
    range(TALENT_BOW,     "HUMANS_BOWT1.MDS", "HUMANS_BOWT2.MDS");
    range(TALENT_CROSSBOW,"HUMANS_CBOWT1.MDS","HUMANS_CBOWT2.MDS");
    }

  // NB fix 30: a save made before fix 23 keeps the hit chances 0 of every NPC (B_SetFightSkills runs only when the
  // character is made). The scripts wrote the percents also into AIVAR 84..87 (1H, 2H, bow, crossbow): take them.
  static void restoreFightSkills(Npc& npc) {
    auto& h = npc.handle();
    if(h.hitchance[TALENT_1H]!=0 || h.hitchance[TALENT_2H]!=0 || h.hitchance[TALENT_BOW]!=0 || h.hitchance[TALENT_CROSSBOW]!=0)
      return;
    if(h.aivar[84]<=0 && h.aivar[85]<=0 && h.aivar[86]<=0 && h.aivar[87]<=0)
      return;
    h.hitchance[TALENT_1H]       = std::clamp(h.aivar[84],0,100);
    h.hitchance[TALENT_2H]       = std::clamp(h.aivar[85],0,100);
    h.hitchance[TALENT_BOW]      = std::clamp(h.aivar[86],0,100);
    h.hitchance[TALENT_CROSSBOW] = std::clamp(h.aivar[87],0,100);
    applyFightSkills(npc,0);
    }
  static const char* const allOverlays[] = {
    "humans_1hST1.mds","humans_1hST2.mds","humans_1hST3.mds",
    "Shield_ST1.MDS","Shield_ST2.MDS","Shield_ST3.MDS","Shield_ST4.MDS","Hum_Shield2.MDS","Shield.MDS","PRE_Hum_Shield2.MDS",
    "Humans_Pirate.mds","Humans_Rapier_ST3.mds","Humans_Rapier_ST2.mds","Humans_Rapier_ST1.mds","PRE_START.mds",
    "humans_2hST3.mds","humans_2hST2.mds","humans_2hST1.mds","HUMANS_AXEST1.MDS","HUMANS_AXEST2.MDS",
    "PRE_START_2X2.MDS","HUM_2X2.MDS","Humans_2x2ST3.MDS","Humans_1x2ST3.MDS",
    "PRE_Humans_O2Hl2.mds","Humans_O2Hl2.mds",
    "PRE_Humans_Stafx.mds","PRE_Humans_Stafw.mds","PRE_Humans_Stafd.mds","PRE_Humans_Stafg.mds","PRE_Humans_Stafdemon.mds",
    "Humans_Stafx.mds","Humans_Stafw.mds","Humans_Stafd.mds","Humans_Stafg.mds","Humans_Stafdemon.mds",
    "PRE_Humans_Speer.mds","Humans_Speer.mds","Humans_SPST1.mds","Humans_SPST2.mds","HUMANS_2h_b_st1.mds",
    "HUMANS_BOSS03.MDS","HUMANS_BOSS032.MDS","HUMANS_1HDEXST1.MDS","HUMANS_1HDEXST2.MDS","HUMANS_1HDEXST3.MDS",
    };

  // an equipped shield of NB: ITEM_KAT_NF with ITEM_SHIELD (0x40000), not a weapon of one hand
  static bool hasShield(const Npc& npc) {
    for(auto it = npc.inventory().iterator(Inventory::T_Inventory); it.isValid(); ++it) {
      if(!it.isEquipped())
        continue;
      auto& h = it->handle();
      const uint32_t fl = uint32_t(h.flags);
      if(h.main_flag==2 /*ITEM_KAT_NF*/ && (fl & 0x40000)!=0 && (fl & 0x40003)!=0x40003)
        return true;
      }
    return false;
    }

  // 1024AA90: a style of the weapons of dexterity is replaced by the usual one: always for an axe, else 80 of 101
  static std::string dexFix(const Npc& npc, std::string name, uint32_t rnd) {
    static const char* dex[3] = {"HUMANS_1HDEXST1.MDS","HUMANS_1HDEXST2.MDS","HUMANS_1HDEXST3.MDS"};
    static const char* usu[3] = {"HUMANS_1HST1.MDS","HUMANS_1HST2.MDS","HUMANS_1HST3.MDS"};
    for(int i=0; i<3; ++i) {
      if(name!=dex[i])
        continue;
      auto* w   = const_cast<Npc&>(npc).currentMeleeWeapon();
      bool  axe = w!=nullptr && (uint32_t(w->handle().flags) & 0x8000)!=0;
      if(axe || (rnd%101)<=80)
        return usu[i];
      }
    return name;
    }

  static bool isRapier(Npc& npc) {     // NB: BIT_ITEM_PIERCE_DAMAGE (2) in C_ITEM.HP
    auto* w = npc.currentMeleeWeapon();
    return w!=nullptr && (w->handle().hp & 2)!=0;
    }
  static bool isAxe(Npc& npc) {        // ITEM_AXE
    auto* w = npc.currentMeleeWeapon();
    return w!=nullptr && (uint32_t(w->handle().flags) & 0x8000)!=0;
    }
  // the name of the instance contains one of the names (zSTRING::Search in the plugin)
  static bool nameHas(GameScript& sc, Npc& npc, std::initializer_list<const char*> names) {
    auto* sym = sc.findSymbol(npc.handle().symbol_index());
    if(sym==nullptr)
      return false;
    for(auto n:names)
      if(sym->name().find(n)!=std::string::npos)
        return true;
    return false;
    }
  static const char* pick(GameScript& sc, std::initializer_list<const char*> t) {
    return t.begin()[sc.rand(uint32_t(t.size()))];
    }

  // 1024AD00: bandits, one hand
  static std::string bdt1h(GameScript& sc, Npc& npc) {
    const bool sh = hasShield(npc);
    if(nameHas(sc,npc,{"BDT_1460_NESTOR","BDT_1093_ADDON_FRANCO","BDT_1083_ADDON_ESTEBAN"})) {
      if(isAxe(npc))
        return sh ? pick(sc,{"SHIELD_ST2.MDS","SHIELD_ST3.MDS","SHIELD_ST1.MDS"})
                  : pick(sc,{"HUMANS_1HST1.MDS","HUMANS_1HST2.MDS","HUMANS_1HST3.MDS"});
      if(sh)
        return pick(sc,{"SHIELD_ST2.MDS","SHIELD_ST3.MDS","SHIELD_ST4.MDS"});
      if(isRapier(npc))
        return pick(sc,{"HUMANS_RAPIER_ST1.MDS","HUMANS_RAPIER_ST2.MDS","HUMANS_RAPIER_ST2.MDS","HUMANS_RAPIER_ST3.MDS"});
      return pick(sc,{"HUMANS_1HST1.MDS","HUMANS_1HST3.MDS","HUMANS_1HST2.MDS"});
      }
    if(nameHas(sc,npc,{"BDT_1060_DEXTER","BDT_10620_BANDIT_L","BDT_10621_BANDIT_L","BDT_10622_BANDIT_L",
                       "BDT_10623_BANDIT_L","BDT_HAART"})) {
      if(sh)
        return pick(sc,{"SHIELD_ST1.MDS","SHIELD_ST3.MDS","SHIELD_ST2.MDS"});
      if(isRapier(npc))
        return pick(sc,{"HUMANS_RAPIER_ST1.MDS","HUMANS_RAPIER_ST2.MDS","Humans_Pirate.MDS"});
      return pick(sc,{"HUMANS_1HST3.MDS","HUMANS_1HST2.MDS","HUMANS_1HST1.MDS",
                      "HUMANS_1HDEXST3.MDS","HUMANS_1HDEXST2.MDS","HUMANS_1HDEXST1.MDS"});
      }
    if(npc.handle().hitchance[TALENT_1H]>=60) {
      if(sh)
        return pick(sc,{"SHIELD_ST2.MDS","SHIELD_ST1.MDS","SHIELD_ST2.MDS"});
      if(isRapier(npc))
        return pick(sc,{"HUMANS_RAPIER_ST2.MDS","HUMANS_RAPIER_ST3.MDS","HUMANS_RAPIER_ST2.MDS"});
      return pick(sc,{"HUMANS_1HST3.MDS","HUMANS_1HST2.MDS","HUMANS_1HDEXST2.MDS","HUMANS_1HDEXST3.MDS"});
      }
    if(sh)
      return pick(sc,{"SHIELD_ST2.MDS","SHIELD_ST1.MDS","SHIELD_ST2.MDS","SHIELD_ST3.MDS"});
    if(isRapier(npc))
      return pick(sc,{"HUMANS_RAPIER_ST1.MDS","Humans_Pirate.MDS","HUMANS_RAPIER_ST1.MDS"});
    return pick(sc,{"HUMANS_1HST1.MDS","HUMANS_1HST2.MDS","HUMANS_1HDEXST1.MDS","HUMANS_1HDEXST2.MDS"});
    }

  // 1024B810: bandits, two hands
  static std::string bdt2h(GameScript& sc, Npc& npc) {
    if(nameHas(sc,npc,{"TPL_GOR_BOBA"}))
      return "HUMANS_2h_b_st1.MDS";
    const int hc = npc.handle().hitchance[TALENT_2H];
    if(hc>=85)
      return pick(sc,{"HUMANS_2HST3.MDS","HUMANS_AXEST2.MDS","HUMANS_SPST2.MDS"});
    if(hc>=60)
      return pick(sc,{"HUMANS_2HST2.MDS","HUMANS_AXEST2.MDS","HUMANS_2HST1.MDS"});
    return pick(sc,{"HUMANS_2HST1.MDS","HUMANS_2HST2.MDS"});
    }

  // 1024BA00: mercenaries, one hand (no table by the hit chance in the plugin)
  static std::string sld1h(GameScript& sc, Npc& npc) {
    if(nameHas(sc,npc,{"AL_Boss3"}))
      return sc.rand(2)==1 ? "HUMANS_BOSS03.MDS" : "HUMANS_BOSS032.MDS";
    if(nameHas(sc,npc,{"SLD_814_SENTENZA"})) {
      if(isRapier(npc))
        return pick(sc,{"HUMANS_RAPIER_ST2.MDS","HUMANS_RAPIER_ST3.MDS","HUMANS_RAPIER_ST2.MDS","HUMANS_RAPIER_ST1.MDS"});
      return pick(sc,{"HUMANS_1HST3.MDS","HUMANS_1HST3.MDS","HUMANS_1HST2.MDS","HUMANS_1HST1.MDS",
                      "HUMANS_1HDEXST2.MDS","HUMANS_1HDEXST1.MDS"});
      }
    if(nameHas(sc,npc,{"SLD_802_BUSTER","SLD_816_FESTER","SLD_807_BULLCO","SLD_822_RAOUL"})) {
      if(isRapier(npc))
        return pick(sc,{"Humans_Pirate.MDS","HUMANS_RAPIER_ST1.MDS",""});
      return pick(sc,{"HUMANS_1HST1.MDS","HUMANS_1HST2.MDS","HUMANS_1HDEXST1.MDS"});
      }
    if(hasShield(npc))
      return pick(sc,{"SHIELD_ST2.MDS","SHIELD_ST1.MDS","SHIELD_ST1.MDS","SHIELD_ST2.MDS"});
    if(isRapier(npc))
      return pick(sc,{"Humans_Pirate.MDS","HUMANS_RAPIER_ST1.MDS","HUMANS_RAPIER_ST2.MDS"});
    return pick(sc,{"HUMANS_1HST1.MDS","HUMANS_1HST2.MDS","HUMANS_1HST2.MDS","HUMANS_1HDEXST1.MDS"});
    }
  // 1024C080: mercenaries, two hands
  static std::string sld2h(GameScript& sc, Npc&) {
    return sc.rand(2)==1 ? "HUMANS_AXEST2.MDS" : "HUMANS_2HST2.MDS";
    }

  // 1024C1D0: militia, one hand (two hands: the common table)
  static std::string mil1h(GameScript& sc, Npc& npc) {
    if(nameHas(sc,npc,{"MIL_317_RUGA","MIL_319_PABLO","MIL_324_PECK","MIL_316_WAMBO","MIL_3120_WULFGAR","MIL_312_WULFGAR"}))
      return pick(sc,{"HUMANS_1HST3.MDS","HUMANS_1HST1.MDS","HUMANS_1HST2.MDS",
                      "HUMANS_1HDEXST1.MDS","HUMANS_1HDEXST2.MDS","HUMANS_1HDEXST3.MDS"});
    return pick(sc,{"HUMANS_1HST1.MDS","HUMANS_1HST2.MDS","HUMANS_1HDEXST1.MDS","HUMANS_1HDEXST2.MDS"});
    }

  // 1024C720 / 1024C8D0: paladins
  static std::string pal1h(GameScript& sc, Npc& npc) {
    if(hasShield(npc))
      return pick(sc,{"SHIELD_ST2.MDS","SHIELD_ST3.MDS","SHIELD_ST3.MDS","SHIELD_ST4.MDS"});
    return pick(sc,{"HUMANS_1HST3.MDS","HUMANS_1HST2.MDS","HUMANS_1HDEXST3.MDS","HUMANS_1HDEXST2.MDS"});
    }
  static std::string pal2h(GameScript& sc, Npc&) {
    return pick(sc,{"HUMANS_2HST2.MDS","HUMANS_2HST3.MDS","HUMANS_SPST2.MDS"});
    }

  // 1024CA20 / 1024CBD0: dragon hunters
  static std::string djg1h(GameScript& sc, Npc& npc) {
    if(hasShield(npc))
      return pick(sc,{"SHIELD_ST2.MDS","SHIELD_ST3.MDS","SHIELD_ST4.MDS","SHIELD_ST3.MDS"});
    return pick(sc,{"HUMANS_1HST3.MDS","HUMANS_1HST2.MDS","HUMANS_1HDEXST2.MDS","HUMANS_1HDEXST3.MDS"});
    }
  static std::string djg2h(GameScript& sc, Npc&) {
    return sc.rand(2)==1 ? "HUMANS_2HST3.MDS" : "HUMANS_2HST2.MDS";
    }

  // 1024CD20: guild 16 (GIL_TPL in NB), two hands
  static std::string g16_2h(GameScript& sc, Npc&) {
    return pick(sc,{"HUMANS_SPST2.MDS","HUMANS_2HST3.MDS","HUMANS_2HST2.MDS"});
    }

  // 1024CE70 / 1024D300: pirates
  static std::string pir1h(GameScript& sc, Npc& npc) {
    if(nameHas(sc,npc,{"PIR_1352_Addon_AlligatorJack"}))
      return "HUMANS_1HST2.MDS";
    const int  hc = npc.handle().hitchance[TALENT_1H];
    const bool sh = hasShield(npc);
    if(hc>=90) {
      if(sh)
        return pick(sc,{"SHIELD_ST3.MDS","SHIELD_ST4.MDS","SHIELD_ST2.MDS","SHIELD_ST3.MDS"});
      if(isRapier(npc))
        return pick(sc,{"HUMANS_RAPIER_ST2.MDS","HUMANS_RAPIER_ST1.MDS","HUMANS_RAPIER_ST3.MDS","HUMANS_RAPIER_ST3.MDS"});
      return pick(sc,{"HUMANS_1HST2.MDS","HUMANS_1HST3.MDS","HUMANS_1HST3.MDS","HUMANS_1HST3.MDS",
                      "HUMANS_1HDEXST3.MDS","HUMANS_1HDEXST2.MDS"});
      }
    if(hc>=60) {
      if(sh)
        return pick(sc,{"SHIELD_ST1.MDS","SHIELD_ST2.MDS","SHIELD_ST1.MDS","SHIELD_ST2.MDS"});
      if(isRapier(npc))
        return pick(sc,{"HUMANS_RAPIER_ST2.MDS","HUMANS_RAPIER_ST1.MDS","HUMANS_PIRATE.MDS","HUMANS_RAPIER_ST2.MDS"});
      return pick(sc,{"HUMANS_1HST1.MDS","HUMANS_1HST2.MDS","HUMANS_1HST2.MDS","HUMANS_1HST2.MDS",
                      "HUMANS_1HDEXST1.MDS","HUMANS_1HDEXST2.MDS"});
      }
    if(sh)
      return sc.rand(2)==1 ? "SHIELD_ST2.MDS" : "SHIELD_ST1.MDS";
    if(isRapier(npc))
      return pick(sc,{"HUMANS_PIRATE.MDS","HUMANS_RAPIER_ST1.MDS","HUMANS_RAPIER_ST1.MDS"});
    return pick(sc,{"HUMANS_1HST1.MDS","HUMANS_1HST1.MDS","HUMANS_1HDEXST1.MDS"});
    }
  static std::string pir2h(GameScript& sc, Npc&) {
    return pick(sc,{"HUMANS_2HST1.MDS","HUMANS_2HST2.MDS","HUMANS_SPST2.MDS"});
    }

  // 1024D450 / 1024D6D0: outlaws (GIL_OUT)
  static std::string out1h(GameScript& sc, Npc& npc) {
    const int  hc = npc.handle().hitchance[TALENT_1H];
    const bool sh = hasShield(npc);
    if(hc>=90) {
      if(sh)
        return pick(sc,{"SHIELD_ST2.MDS","SHIELD_ST1.MDS","SHIELD_ST2.MDS","SHIELD_ST3.MDS"});
      return pick(sc,{"","HUMANS_1HST1.MDS","HUMANS_1HST2.MDS","HUMANS_1HST3.MDS",
                      "HUMANS_1HDEXST1.MDS","HUMANS_1HDEXST2.MDS","HUMANS_1HDEXST3.MDS"});
      }
    if(hc>=60) {
      if(sh)
        return pick(sc,{"SHIELD_ST2.MDS","SHIELD_ST3.MDS","SHIELD_ST2.MDS","SHIELD_ST1.MDS"});
      return pick(sc,{"","HUMANS_1HST1.MDS","HUMANS_1HST2.MDS","HUMANS_1HDEXST1.MDS","HUMANS_1HDEXST2.MDS"});
      }
    if(sh)
      return sc.rand(2)==1 ? "SHIELD_ST1.MDS" : "SHIELD_ST2.MDS";
    return pick(sc,{"","HUMANS_1HST1.MDS","HUMANS_1HDEXST1.MDS"});
    }
  static std::string out2h(GameScript& sc, Npc&) {
    return pick(sc,{"HUMANS_AXEST2.MDS","HUMANS_2HST1.MDS","HUMANS_2HST3.MDS"});
    }

  // the style of the guild; "" = no table done yet for this guild (the styles of the hit chance stay)
  static bool styleFor(GameScript& sc, Npc& npc, int guild, bool oneHand, std::string& out) {
    switch(guild) {
      case 9:  out = oneHand ? bdt1h(sc,npc) : bdt2h(sc,npc); return true;
      case 7:  out = oneHand ? sld1h(sc,npc) : sld2h(sc,npc); return true;
      case 2:  if(!oneHand) return false; out = mil1h(sc,npc); return true;
      case 1:  out = oneHand ? pal1h(sc,npc) : pal2h(sc,npc); return true;
      case 6:  out = oneHand ? djg1h(sc,npc) : djg2h(sc,npc); return true;
      case 16: if(oneHand) return false; out = g16_2h(sc,npc); return true;
      case 13: out = oneHand ? pir1h(sc,npc) : pir2h(sc,npc); return true;
      case 10: out = oneHand ? out1h(sc,npc) : out2h(sc,npc); return true;
      // GIL_SKELETON (36): long tables by the names of the skeletons - not done yet
      default: return false;
      }
    }

  // UNION_ABI.DLL 0x1024A460, see the comment at fix 34 in the CHANGELOG
  static void movesetTick(GameScript& sc, Npc& npc, int sepHum) {
    const int g = int(npc.guild());
    if(g>=sepHum && g!=36)
      return;
    const auto ws = npc.weaponState();
    const bool w1 = (ws==WeaponState::W1H), w2 = (ws==WeaponState::W2H);
    if(!w1 && !w2)
      return;
    Npc* enemy = npc.target();
    if(enemy==nullptr || enemy->isDown())
      return;
    auto& h = npc.handle();
    if(h.attribute[ATR_HITPOINTS] <= h.attribute[ATR_HITPOINTSMAX]/15)
      return;

    int changed = npc.newBalanceGetNpcVar(9);
    int count   = npc.newBalanceGetNpcVar(151);
    if(changed!=0 && count==0 && npc.qDistTo(*enemy)>=500.f*500.f) {
      changed = 0;
      npc.newBalanceSetNpcVar(9,0);
      }
    if(count>0)
      npc.newBalanceSetNpcVar(151,count-1);
    if(changed!=0)
      return;

    std::string style;
    if(!styleFor(sc,npc,g,w1,style))
      return;
    for(auto name:allOverlays)
      if(auto sk = Resources::loadSkeleton(name))
        npc.delOverlay(sk);
    if(w1)
      style = dexFix(npc,style,sc.rand(101));
    if(!style.empty())
      if(auto sk = Resources::loadSkeleton(style))
        npc.addOverlay(sk,0);
    npc.newBalanceSetNpcVar(9,1);
    npc.newBalanceSetNpcVar(151,40+int(sc.rand(41)));
    }

  // every 100 ms: the NPC near the hero (old saves: fix 30)
  static void tick(GameScript& sc, World& world, int sepHum) {
    auto* pl = world.player();
    if(pl==nullptr)
      return;
    world.detectNpc(pl->position(), 1500.f, [&sc,pl,sepHum](Npc& npc) {
      if(&npc==pl || npc.isPlayer() || npc.isDown())
        return;
      restoreFightSkills(npc);
      movesetTick(sc,npc,sepHum);
      });
    }

  }

template <typename T>
struct ScopeVar final {
  ScopeVar(zenkit::DaedalusSymbol& sym, const std::shared_ptr<T>& h) : prev(sym.get_instance()), sym(sym) {
    sym.set_instance(h);
    }

  ScopeVar(const ScopeVar&)=delete;
  ~ScopeVar(){
    sym.set_instance(prev);
    }

  std::shared_ptr<zenkit::DaedalusInstance> prev;
  zenkit::DaedalusSymbol&                   sym;
  };

struct ScopeCtx final {
  ScopeCtx(GameScript& script, NpcProcessPolicy pp) : script(script) {
    prev = script.aiProcessPolicy;
    script.aiProcessPolicy = pp;
    }

  ~ScopeCtx() {
    script.aiProcessPolicy = prev;
    }

  GameScript&      script;
  NpcProcessPolicy prev = NpcProcessPolicy::AiNormal;
  };


bool GameScript::GlobalOutput::output(Npc& npc, std::string_view text) {
  return owner.aiOutput(npc,text,false);
  }

bool GameScript::GlobalOutput::outputSvm(Npc &npc, std::string_view text) {
  return owner.aiOutputSvm(npc,text,false);
  }

bool GameScript::GlobalOutput::outputOv(Npc &npc, std::string_view text) {
  return owner.aiOutputSvm(npc,text,true);
  }

bool GameScript::GlobalOutput::printScr(Npc& npc, int time, std::string_view msg, int x, int y, std::string_view font) {
  auto& f = Resources::font(font, Resources::FontType::Normal, 1.0);
  Gothic::inst().onPrintScreen(msg,x,y,time,f);
  return true;
  }

bool GameScript::GlobalOutput::isFinished() {
  return true;
  }

GameScript::PerDist::PerDist() {
  for(auto& i:range)
    i = -1;
  }

int GameScript::PerDist::at(PercType perc, int r) const {
  if(perc>=PERC_Count)
    return r;
  auto rr = range[perc];
  if(rr>0)
    return rr;
  return r;
  }


GameScript::GameScript(GameSession &owner)
    :owner(owner), vm(createVm(Gothic::inst())) {
  if (vm.global_self() == nullptr || vm.global_other() == nullptr || vm.global_item() == nullptr ||
      vm.global_victim() == nullptr || vm.global_hero() == nullptr)
    throw std::runtime_error("Cannot find script symbol SELF, OTHER, ITEM, VICTIM, or HERO! Cannot proceed!");

  vmLang = Gothic::inst().settingsGetI("GAME", "language");
  vm.register_exception_handler([](zenkit::DaedalusVm& v, const zenkit::DaedalusScriptError& exc, const zenkit::DaedalusInstruction& instr) {
    const auto ret = zenkit::lenient_vm_exception_handler(v,exc,instr);
    // where the script was: the first times that every error happens, so a log is not drowned
    static std::unordered_map<std::string,int> seen;
    if(seen.size()<200 && ++seen[exc.what()]<=2)
      v.print_stack_trace();
    return ret;
    });
  Gothic::inst().setupCommonScriptClasses(vm);
  Gothic::inst().setupVmCommonApi(vm);
  aiDefaultPipe.reset(new GlobalOutput(*this));
  initCommon();
  initSettings();
  Gothic::inst().onSettingsChanged.bind(this,&GameScript::initSettings);
  }

std::string GameScript::dmaReport() {
  return dma==nullptr ? std::string() : dma->report();
  }

void GameScript::dmaChangeLevel(bool begin) {
  if(dma!=nullptr) {
    if(begin)
      dma->clearMobs();
    dma->eventChangeLevel(begin);
    }
  }

void GameScript::dmaSave(Serialize& fout) {
  if(dma!=nullptr)
    dma->eventSaveGame(fout);
  }

void GameScript::dmaBeforeSave() {
  if(dma!=nullptr)
    dma->eventBeforeSaveGame();
  }

void GameScript::dmaLoad(Serialize& fin) {
  if(dma!=nullptr)
    dma->eventLoadGame(fin);
  }

void GameScript::dmaItemTaken(Npc& owner, Item& item) {
  if(dma!=nullptr)
    dma->eventItemTaken(owner,item);
  }

int GameScript::dmaChangeAttribute(Npc& npc, int attribute, int change) {
  return dma==nullptr ? change : dma->eventChangeAttribute(npc,attribute,change);
  }

void GameScript::dmaKey(int key) {
  if(dma!=nullptr)
    dma->eventKey(key);
  }

void GameScript::dmaKeyState(int key, bool down) {
  if(dma!=nullptr)
    dma->eventKeyState(key,down);
  }

void GameScript::dmaZoneMusic(std::string_view name) {
  if(dma!=nullptr)
    dma->eventZoneMusic(name);
  }

void GameScript::dmaMobCanOpen(Interactive& mob) {
  if(dma!=nullptr)
    dma->eventMobCanOpen(mob);
  }

void GameScript::dmaInventoryOpen(Npc& npc) {
  if(dma!=nullptr)
    dma->eventInventoryOpen(npc);
  }

void GameScript::dmaItemInfo(const Item& item) {
  // NB: the description of weapons/armors is made by the Union plugin - see nbiteminfo.h
  NbItemInfo::apply(vm,const_cast<Item&>(item).handle());
  if(dma!=nullptr)
    dma->eventItemInfo(item);
  }

void GameScript::dmaDialogChoice(const DlgChoice* choice, bool subChoice, int selection) {
  if(dma!=nullptr) {
    ScopeVar self(*vm.global_self(),vm.global_self()->get_instance());
    ScopeVar other(*vm.global_other(),vm.global_other()->get_instance());
    ScopeVar item(*vm.global_item(),vm.global_item()->get_instance());
    dma->eventDialogChoice(choice,subChoice,selection);
    }
  }

bool GameScript::dmaTakeInventoryClose() {
  return dma!=nullptr && dma->takeInventoryClose();
  }

bool GameScript::dmaInFinalBoards() const {
  return dma!=nullptr && dma->inFinalBoards();
  }

bool GameScript::dmaHidesPartyFocus() const {
  return dma!=nullptr && dma->hidesPartyFocus();
  }

void GameScript::dmaSpellBegin(Npc& caster) {
  if(dma!=nullptr)
    dma->eventSpellBegin(caster);
  }

bool GameScript::dmaSpellKeepsItem(int spellId) {
  return dma!=nullptr && dma->spellKeepsItem(spellId);
  }

const std::vector<std::string>& GameScript::inventoryCategoryNames() {
  if(!invCategoryNamesInit) {
    invCategoryNamesInit = true;
    auto* sym = vm.find_symbol_by_name("TXT_INV_CAT");
    if(sym!=nullptr && sym->type()==zenkit::DaedalusDataType::STRING)
      for(uint32_t i=0; i<sym->count(); ++i)
        invCategoryNames.push_back(sym->get_string(uint16_t(i)));
    }
  return invCategoryNames;
  }

uint32_t GameScript::inventoryCategoryMask(int index) const {
  auto* groups = vm.find_symbol_by_name("INV_CAT_GROUPS");
  if(groups==nullptr || groups->type()!=zenkit::DaedalusDataType::INT || index<0 || uint32_t(index)>=groups->count())
    return 0;
  return uint32_t(groups->get_int(uint16_t(index)));
  }

int GameScript::inventoryScriptCategory() const {
  auto* category = vm.find_symbol_by_name("CATINV_ACTIVECATEGORY");
  return category!=nullptr && category->type()==zenkit::DaedalusDataType::INT ? category->get_int() : -1;
  }

void GameScript::setInventoryScriptCategory(int index) {
  auto* category = vm.find_symbol_by_name("CATINV_ACTIVECATEGORY");
  auto* groups = vm.find_symbol_by_name("INV_CAT_GROUPS");
  if(category==nullptr || groups==nullptr || groups->count()==0 ||
     category->type()!=zenkit::DaedalusDataType::INT || groups->type()!=zenkit::DaedalusDataType::INT)
    return;
  auto* g1 = vm.find_symbol_by_name("CATINV_G1MODE");
  const int first = g1!=nullptr && g1->get_int()!=0 ? std::min(1,int(groups->count())-1) : 0;
  category->set_int(std::clamp(index,first,int(groups->count())-1));
  }

size_t GameScript::dmaPickLockLength(Interactive& mob, size_t length) {
  return dma==nullptr ? length : dma->eventPickLockLength(mob,length);
  }

bool GameScript::dmaFinishingMove(Npc& npc, Npc& target) {
  return dma!=nullptr && dma->eventFinishingMove(npc,target);
  }

bool GameScript::dmaSellItem(Npc& npc, Item& item) {
  return dma!=nullptr && dma->eventSellItem(npc,item);
  }

std::string GameScript::dmaLastCallOther() const {
  return dma!=nullptr ? dma->lastCallOther() : std::string();
  }

bool GameScript::dmaInCamera() const {
  return dma!=nullptr && dma->inCamera();
  }

bool GameScript::dmaCutsceneMenuBlocked() const {
  return dma!=nullptr && dma->cutsceneMenuBlocked();
  }

bool GameScript::dmaCutsceneStatusHidden() const {
  return dma!=nullptr && dma->cutsceneStatusHidden();
  }

void GameScript::dmaDialogMessage(std::string& name, uint32_t& color, bool& bottom, bool playerLine) {
  if(dma!=nullptr)
    dma->eventDialogMessage(name,color,bottom,playerLine);
  }

bool GameScript::dmaSaveDisabled() const {
  return dma!=nullptr && dma->saveDisabled();
  }

bool GameScript::dmaLoadDisabled() const {
  return dma!=nullptr && dma->loadDisabled();
  }

bool GameScript::dmaDialogCameraOff() const {
  return dma!=nullptr && dma->dialogCameraDisabled();
  }

bool GameScript::dmaTrialogFinishPending() const {
  return dma!=nullptr && dma->trialogFinishPending();
  }

bool GameScript::dmaUnderwaterPickup() const {
  return dma!=nullptr && dma->underwaterPickup();
  }

bool GameScript::dmaDropUnconscious(Npc& npc) {
  return dma!=nullptr && dma->eventDropUnconscious(npc);
  }

void GameScript::dmaEquipItem(Npc& npc, Item& item, bool equip) {
  if(dma!=nullptr)
    dma->eventEquipItem(npc,item,equip);
  }

bool GameScript::dmaEquipBestWeapons(Npc& npc) {
  return dma!=nullptr && dma->eventEquipBestWeapons(npc);
  }

bool GameScript::dmaEnemyCandidate(Npc& npc, Npc& candidate) {
  return dma==nullptr || dma->eventEnemyCandidate(npc,candidate);
  }

bool GameScript::dmaDropVob(Npc& npc, Item& item) {
  return dma!=nullptr && dma->eventDropVob(npc,item);
  }

void GameScript::dmaCsCamera(bool trigger) {
  if(dma!=nullptr)
    dma->eventCsCamera(trigger);
  }

void GameScript::dmaStatusScreen(bool show) {
  if(dma!=nullptr)
    dma->eventStatusScreen(show);
  }

auto GameScript::dmaTakeMenuTexts() -> std::vector<std::pair<std::string,std::string>> {
  return dma==nullptr ? std::vector<std::pair<std::string,std::string>>() : dma->takeMenuTexts();
  }

void GameScript::dmaFocus(Npc& player, const Focus& focus) {
  if(dma!=nullptr)
    dma->eventFocus(player,focus);
  }

void GameScript::dmaSpellFocus(Npc& caster, int spellId) {
  if(dma!=nullptr)
    dma->eventSpellFocus(caster,spellId);
  }

bool GameScript::dmaSpellTarget(Interactive& mob, TargetType type) {
  return dma!=nullptr && dma->eventSpellTarget(mob,type);
  }

/// The focus of the magic: the mods of Ikarus change it while the game runs (the lock picking spell of CoM)
const zenkit::IFocus& GameScript::focusMage() const {
  if(dma!=nullptr)
    dma->syncFocusMage(const_cast<zenkit::IFocus&>(cFocusMage));
  return cFocusMage;
  }

void GameScript::dmaOverlay(std::vector<DmaOverlayItem>& out, int width, int height) {
  if(dma!=nullptr)
    dma->collectOverlay(out,width,height);
  }

auto GameScript::dmaDamageBegin(Npc& victim, Npc& attacker, bool isSpell, int spellId, bool ranged, bool dontKill,
                               const DamageCalculator::Damage& damage) -> DmaDamage {
  DmaDamage ret;
  ret.dontKill = dontKill;
  ret.damage = damage;
  if(dma!=nullptr)
    dma->eventDamageBegin(victim,attacker,isSpell,spellId,ranged,ret);
  return ret;
  }

void GameScript::dmaDamageEnd(Npc& victim, DmaDamage& d) {
  if(dma!=nullptr)
    dma->eventDamageEnd(victim,d);
  }

void GameScript::dmaFallDamage(Npc& npc) {
  if(dma!=nullptr)
    dma->eventFallDamage(npc);
  }

GameScript::~GameScript() {
  nbStates.erase(this);
  Gothic::inst().onSettingsChanged.ubind(this,&GameScript::initSettings);
  }

void GameScript::initCommon() {
  // Returning New Balance: the externals of its DLL (UNION_ABI.DLL), with the signatures of its GOTHIC.DAT.
  // The others stay in the default handler of the VM (0, "" or null), which the log lists at the start.
  if(vm.find_symbol_by_name("RX_ISSPACETNET")!=nullptr) {
    auto& nbState = nbStates[this];
    nbState.cycle = vm.find_symbol_by_name("RX_MAIN_CYCLETRIGGER");
    if(nbState.cycle!=nullptr && nbState.cycle->type()!=zenkit::DaedalusDataType::FUNCTION)
      nbState.cycle = nullptr;
    const auto nbFunc = [this](std::string_view name) -> zenkit::DaedalusSymbol* {
      auto* f = vm.find_symbol_by_name(name);
      return f!=nullptr && f->type()==zenkit::DaedalusDataType::FUNCTION && !f->is_external() &&
             vm.find_parameters_for_function(f).empty() ? f : nullptr;
      };
    nbState.getTime   = nbFunc("B_GETTIME");
    nbState.perFrame  = nbFunc("PERFRAME");
    nbState.checkMenu = nbFunc("B_CHECKMENUOPTION");
    if(auto* ft = vm.find_symbol_by_name("RX_FRAMETIME"); ft!=nullptr && ft->type()==zenkit::DaedalusDataType::INT && !ft->is_member())
      nbState.frameTime = ft;
    std::vector<std::string> nbBound;
    // registers only a declared external whose signature in GOTHIC.DAT is the one of the callback
    const auto bindNb = [this,&nbBound](std::string_view name, auto callback) {
      auto* s = vm.find_symbol_by_name(name);
      if(s==nullptr || !s->is_external())
        return;
      if(!NbSig<decltype(callback)>::matches(vm,*s)) {
        Log::e("NB: ",nbSignature(vm,*s)," has another signature than expected, it is left to the default handler");
        return;
        }
      vm.register_external(name, callback);
      nbBound.emplace_back(s->name());
      };
    // the global variables of the scripts, by the name: NB keeps its state in them
    const auto intVar = [this](std::string_view name) -> zenkit::DaedalusSymbol* {
      auto* s = vm.find_symbol_by_name(name);
      return s!=nullptr && s->type()==zenkit::DaedalusDataType::INT && !s->is_member() ? s : nullptr;
      };

    // the variables of NB on the character
    bindNb("RX_GETNPCVAR", [this](std::shared_ptr<zenkit::INpc> ref, int32_t index) -> int32_t {
      auto* npc = findNpcRaw(ref);
      return npc==nullptr ? 0 : npc->newBalanceGetNpcVar(index);
      });
    bindNb("RX_SETNPCVAR", [this](std::shared_ptr<zenkit::INpc> ref, int32_t index, int32_t value) {
      if(auto* npc = findNpcRaw(ref))
        npc->newBalanceSetNpcVar(index,value);
      });
    bindNb("RX_INCNPCVAR", [this](std::shared_ptr<zenkit::INpc> ref, int32_t index, int32_t value) {
      if(auto* npc = findNpcRaw(ref))
        npc->newBalanceSetNpcVar(index,npc->newBalanceGetNpcVar(index)+value);
      });
    // RX_*NPCVARMASK(npc, AIVRX_BITMASK_COMMON_0, AIVRX_BITMASK_...): the value of a mask, not the number of a bit
    bindNb("RX_GETNPCVARMASK", [this](std::shared_ptr<zenkit::INpc> ref, int32_t index, int32_t mask) -> int32_t {
      auto* npc = findNpcRaw(ref);
      return npc!=nullptr && (npc->newBalanceGetNpcVar(index) & mask)!=0 ? 1 : 0;
      });
    bindNb("RX_SETNPCVARMASKTRUE", [this](std::shared_ptr<zenkit::INpc> ref, int32_t index, int32_t mask) {
      if(auto* npc = findNpcRaw(ref))
        npc->newBalanceSetNpcVar(index,npc->newBalanceGetNpcVar(index) | mask);
      });
    bindNb("RX_SETNPCVARMASKFALSE", [this](std::shared_ptr<zenkit::INpc> ref, int32_t index, int32_t mask) {
      if(auto* npc = findNpcRaw(ref))
        npc->newBalanceSetNpcVar(index,npc->newBalanceGetNpcVar(index) & ~mask);
      });
    bindNb("RX_TOGGLENPCVARMASK", [this](std::shared_ptr<zenkit::INpc> ref, int32_t index, int32_t mask) {
      if(auto* npc = findNpcRaw(ref))
        npc->newBalanceSetNpcVar(index,npc->newBalanceGetNpcVar(index) ^ mask);
      });

    // RX_*INTMASKVALUE("RX_AW_NotesMask0", 22): the number of a bit in a global variable
    bindNb("RX_GETINTMASKVALUE", [intVar](std::string_view name, int32_t bit) -> int32_t {
      auto* s = intVar(name);
      return s!=nullptr && bit>=0 && bit<32 && ((uint32_t(s->get_int())>>bit) & 1u) ? 1 : 0;
      });
    bindNb("RX_SETINTMASKVALUE", [intVar](std::string_view name, int32_t bit, int32_t value) {
      auto* s = intVar(name);
      if(s==nullptr || bit<0 || bit>=32)
        return;
      uint32_t v = uint32_t(s->get_int());
      v = value!=0 ? (v | (1u<<bit)) : (v & ~(1u<<bit));
      s->set_int(int32_t(v));
      });
    bindNb("RX_TOGGLETMASKVALUE", [intVar](std::string_view name, int32_t bit) {
      auto* s = intVar(name);
      if(s!=nullptr && bit>=0 && bit<32)
        s->set_int(int32_t(uint32_t(s->get_int()) ^ (1u<<bit)));
      });
    bindNb("RX_GETCOUNTTRUEINMASKVALUE", [intVar](std::string_view name) -> int32_t {
      auto* s = intVar(name);
      int32_t n = 0;
      for(uint32_t v = s!=nullptr ? uint32_t(s->get_int()) : 0u; v!=0; v &= v-1)
        ++n;
      return n;
      });

    // the global variables by the name
    bindNb("RX_GETVARVALUE", [intVar](std::string_view name) -> int32_t {
      auto* s = intVar(name);
      return s!=nullptr ? s->get_int() : 0;
      });
    bindNb("RX_SETVARVALUE", [intVar](std::string_view name, int32_t value) {
      if(auto* s = intVar(name))
        s->set_int(value);
      });
    bindNb("RX_INCREASEVARVALUE", [intVar](std::string_view name, int32_t value) {
      if(auto* s = intVar(name))
        s->set_int(s->get_int()+value);
      });
    // RX_ChangeValueClamp("RX_MonsterProtRatio", delta, min, max)
    bindNb("RX_CHANGEVALUECLAMP", [intVar](std::string_view name, int32_t delta, int32_t lo, int32_t hi) {
      if(auto* s = intVar(name))
        s->set_int(std::clamp(s->get_int()+delta,std::min(lo,hi),std::max(lo,hi)));
      });
    bindNb("RX_GETSTRINGVALUE", [this](std::string_view name) -> std::string {
      auto* s = vm.find_symbol_by_name(name);
      return s!=nullptr && s->type()==zenkit::DaedalusDataType::STRING && !s->is_member() ? s->get_string() : std::string();
      });

    // strings
    bindNb("RX_CONCATSTRINGS", [](std::string_view a, std::string_view b, std::string_view c, std::string_view d) -> std::string {
      std::string ret;
      ret.reserve(a.size()+b.size()+c.size()+d.size());
      ret.append(a).append(b).append(c).append(d);
      return ret;
      });
    // RX_FormatStringPlaceholder("RX_BookManagerText", p0..p5): "{0}".."{5}" in the variable are replaced
    bindNb("RX_FORMATSTRINGPLACEHOLDER", [this](std::string_view name, std::string_view p0, std::string_view p1, std::string_view p2,
                                                std::string_view p3, std::string_view p4, std::string_view p5) {
      auto* s = vm.find_symbol_by_name(name);
      if(s==nullptr || s->type()!=zenkit::DaedalusDataType::STRING || s->is_member())
        return;
      const std::array<std::string_view,6> args = {p0,p1,p2,p3,p4,p5};
      std::string text = s->get_string();
      for(size_t i=0; i<args.size(); ++i) {
        const std::string key = "{" + std::to_string(i) + "}";
        for(size_t at = text.find(key); at!=std::string::npos; at = text.find(key,at+args[i].size()))
          text.replace(at,key.size(),args[i]);
        }
      s->set_string(text);
      });
    bindNb("RX_GETINSTANCENAME", [this](std::shared_ptr<zenkit::DaedalusInstance> ref) -> std::string {
      auto* s = ref!=nullptr ? vm.find_symbol_by_index(ref->symbol_index()) : nullptr;
      return s!=nullptr ? s->name() : std::string();
      });
    bindNb("RX_NPCINSTCONTAINS", [this](std::shared_ptr<zenkit::INpc> ref, std::string_view part) -> int32_t {
      auto* s = ref!=nullptr ? vm.find_symbol_by_index(ref->symbol_index()) : nullptr;
      if(s==nullptr)
        return 0;
      std::string p(part);
      for(auto& c:p)
        c = char(std::toupper(static_cast<unsigned char>(c)));
      return s->name().find(p)!=std::string::npos ? 1 : 0;
      });

    // random
    bindNb("RX_GETRANDVAL", [this](int32_t lo, int32_t hi) -> int32_t {
      if(hi<lo)
        std::swap(lo,hi);
    const uint64_t range = uint64_t(int64_t(hi)-lo)+1u;
    return int32_t(int64_t(lo) + int64_t(uint64_t(randGen()) % range));
      });
    bindNb("RX_GETBIGRANDOMINT", [this]() -> int32_t {
      return int32_t(randGen() & 0x7fffffff);
      });
    // RX_SelfPseudoRandom(chance, "RX_PSEUDORANDOM_VALUE_..."): a chance in percent
    bindNb("RX_SELFPSEUDORANDOM", [this](int32_t chance, std::string_view) -> int32_t {
      return int32_t(randGen()%100) < chance ? 1 : 0;
      });

    // SELF, OTHER, VICTIM and ITEM: saved before a callback of the scripts and restored after it
    auto parserVars = std::make_shared<std::vector<std::array<std::shared_ptr<zenkit::DaedalusInstance>,4>>>();
    bindNb("RX_SAVEPARSERVARS", [this,parserVars]() {
      parserVars->push_back({vm.global_self()->get_instance(),  vm.global_other()->get_instance(),
                             vm.global_victim()->get_instance(),vm.global_item()->get_instance()});
      });
    bindNb("RX_RESTOREPARSERVARS", [this,parserVars]() {
      if(parserVars->empty())
        return;
      auto& v = parserVars->back();
      vm.global_self()  ->set_instance(v[0]);
      vm.global_other() ->set_instance(v[1]);
      vm.global_victim()->set_instance(v[2]);
      vm.global_item()  ->set_instance(v[3]);
      parserVars->pop_back();
      });

    // RX_CallNpcFunc(self, other, "B_AssessDamage"): the function of this character, B_ASSESSDAMAGE_NONE_50003,
    // if the scripts have one. The scripts do "if(!RX_CallNpcFunc(...)) { return; }":
    // TRUE  = there is no own function, the common reaction goes on (B_MM_AssessDamage, B_AssessDamage)
    // FALSE = the own function handled it (it returns FALSE itself)
    bindNb("RX_CALLNPCFUNC", [this](std::shared_ptr<zenkit::INpc> slf, std::shared_ptr<zenkit::INpc> oth, std::string_view base) -> int32_t {
      auto* inst = slf!=nullptr ? vm.find_symbol_by_index(slf->symbol_index()) : nullptr;
      if(inst==nullptr)
        return 1;
      auto* fn = vm.find_symbol_by_name(std::string(base) + "_" + inst->name());
      if(fn==nullptr || fn->type()!=zenkit::DaedalusDataType::FUNCTION || fn->is_external())
        return 1;
      ScopeVar self (*vm.global_self(),  slf);
      ScopeVar other(*vm.global_other(), oth);
      if(fn->has_return())
        return vm.call_function<int>(fn);
      vm.call_function<void>(fn);
      return 0;
      });

    // the delayed functions: run in tick(), RX_CALLBACKDIALOG waits for the end of the dialog
    const auto addCallback = [this](std::shared_ptr<zenkit::INpc> npc, float sec, int32_t func, bool waitDialog) {
      auto* fn = vm.find_symbol_by_index(uint32_t(func));
      if(fn==nullptr || fn->type()!=zenkit::DaedalusDataType::FUNCTION)
        return;
      NbCallback c;
      c.at         = tickCount() + uint64_t(std::max(0.f,sec)*1000.f);
      c.func       = uint32_t(func);
      c.npc        = std::move(npc);
      c.waitDialog = waitDialog;
      nbStates[this].callbacks.push_back(std::move(c));
      };
    bindNb("RX_CALLBACK", [addCallback](float sec, int32_t func) {
      addCallback(nullptr,sec,func,false);
      });
    bindNb("RX_CALLBACKNPC", [addCallback](std::shared_ptr<zenkit::INpc> npc, float sec, int32_t func) {
      if(npc!=nullptr)
        addCallback(npc,sec,func,false);
      });
    bindNb("RX_CALLBACKINDIALOG", [addCallback](std::shared_ptr<zenkit::INpc> npc, float sec, int32_t func) {
      addCallback(npc,sec,func,false);
      });
    bindNb("RX_CALLBACKDIALOG", [addCallback](std::shared_ptr<zenkit::INpc> npc, float sec, int32_t func) {
      addCallback(npc,sec,func,true);
      });

    // dialogs: the text of a choice that the condition function writes, and the choices of NB
    bindNb("RX_SETDIALOGDESCRIPTION", [this](int32_t infoInstance, std::string_view text) {
      if(auto* info = findInfo(uint32_t(infoInstance)))
        info->description = text;
      });
    bindNb("RX_INFO_ADDCHOICE", [this](int32_t info, std::string_view text, int32_t func, int32_t) {
      info_addchoice(info,text,func);
      });

    // fight
    bindNb("RX_ISENEMY", [this](std::shared_ptr<zenkit::INpc> aRef, std::shared_ptr<zenkit::INpc> bRef) -> int32_t {
      auto* a = findNpc(aRef);
      auto* b = findNpc(bRef);
      return a!=nullptr && b!=nullptr && a!=b && a->isEnemy(*b) ? 1 : 0;
      });
    bindNb("RX_CLEARNPCFOCUS", [this](std::shared_ptr<zenkit::INpc> ref) {
      if(auto* npc = findNpc(ref))
        npc->setTarget(nullptr);
      });
    bindNb("RX_ISCASTINGSPELL", [this](std::shared_ptr<zenkit::INpc> ref) -> int32_t {
      auto* npc = findNpc(ref);
      return npc!=nullptr && npc->weaponState()==WeaponState::Mage ? 1 : 0;
      });
    // RX_GetEnemyRadius(self, 500): how many enemies are in the radius
    bindNb("RX_GETENEMYRADIUS", [this](std::shared_ptr<zenkit::INpc> ref, int32_t radius) -> int32_t {
      auto* npc = findNpc(ref);
      if(npc==nullptr || radius<=0)
        return 0;
      int32_t n = 0;
      world().detectNpc(npc->position(), float(radius), [npc,&n](Npc& oth) {
        if(&oth!=npc && !oth.isDead() && oth.isEnemy(*npc))
          ++n;
        });
      return n;
      });
    // RX_AgroAttack(self, 1000, 1): the nearest enemy in the radius is attacked; 1 = there was one
    bindNb("RX_AGROATTACK", [this](std::shared_ptr<zenkit::INpc> ref, int32_t radius, int32_t) -> int32_t {
      auto* npc = findNpc(ref);
      if(npc==nullptr || npc->isDead() || radius<=0)
        return 0;
      Npc*  target = nullptr;
      float dist   = float(radius)*float(radius);
      world().detectNpc(npc->position(), float(radius), [npc,&target,&dist](Npc& oth) {
        if(&oth==npc || oth.isDead() || !oth.isEnemy(*npc))
          return;
        const float d = oth.qDistTo(*npc);
        if(d<dist) {
          dist   = d;
          target = &oth;
          }
        });
      if(target==nullptr)
        return 0;
      npc->setTarget(target);
      vm.global_other()->set_instance(target->handlePtr());
      // below GIL_SEPERATOR_HUM are the humans (ZS_Attack), above the monsters (ZS_MM_Attack); 21 in NB
      auto* sepHum = vm.find_symbol_by_name("GIL_SEPERATOR_HUM");
      const int32_t sep = sepHum!=nullptr ? sepHum->get_int() : 16;
      const ScriptFn st = int32_t(npc->guild())<sep ? ZS_Attack : ZS_MM_Attack;
      if(st.isValid() && !npc->isInState(st)) {
        npc->clearAiQueue();
        npc->aiPush(AiQueue::aiStartState(st,0,target,nullptr,""));
        }
      return 1;
      });
    bindNb("RX_CANSENSEPLAYER", [this](std::shared_ptr<zenkit::INpc> ref) -> int32_t {
      auto* npc = findNpc(ref);
      auto* pl  = world().player();
      return npc!=nullptr && pl!=nullptr && npc->canSenseNpc(*pl,true)!=SensesBit::SENSE_NONE ? 1 : 0;
      });
    bindNb("RX_HASRANGEWEAPONANY", [this](std::shared_ptr<zenkit::INpc> ref) -> int32_t {
      auto* npc = findNpc(ref);
      return npc!=nullptr && npc->currentRangedWeapon()!=nullptr ? 1 : 0;
      });
    bindNb("RX_GETRANGEWEAPONANY", [this](std::shared_ptr<zenkit::INpc> ref) -> std::shared_ptr<zenkit::IItem> {
      auto* npc = findNpc(ref);
      return npc!=nullptr && npc->currentRangedWeapon()!=nullptr ? npc->currentRangedWeapon()->handlePtr() : nullptr;
      });
    bindNb("RX_AI_WAIT", [this](std::shared_ptr<zenkit::INpc> ref, float sec) {
      ai_wait(ref,sec);
      });

    // the characters in the world
    bindNb("RX_UNSTUCK", [this](std::shared_ptr<zenkit::INpc> ref) -> int32_t {
      auto* npc = findNpc(ref);
      if(npc==nullptr)
        return 0;
      fixNpcPosition(*npc,npc->rotation(),0);
      return 1;
      });
    bindNb("RX_REMOVENPCFORCE", [this](std::shared_ptr<zenkit::INpc> ref) {
      if(auto* npc = findNpcRaw(ref))
        world().removeNpc(*npc);
      });
    bindNb("RX_TELEPORTFORCE", [this](std::shared_ptr<zenkit::INpc> ref, std::string_view wp) {
      auto* npc = findNpc(ref);
      auto* pt  = world().findPoint(wp);
      if(npc==nullptr || pt==nullptr)
        return;
      npc->clearAiQueue();
      npc->aiPush(AiQueue::aiTeleport(*pt));
      });
    // RX_EquipArmor(npc, "ITAR_Bau_L", flag): "" takes the armor off
    bindNb("RX_EQUIPARMOR", [this](std::shared_ptr<zenkit::INpc> ref, std::string_view name, int32_t) {
      auto* npc = findNpc(ref);
      if(npc==nullptr)
        return;
      if(name.empty()) {
        if(auto* a = npc->currentArmor())
          npc->unequipItem(a->clsId());
        return;
        }
      auto* sym = vm.find_symbol_by_name(name);
      if(sym==nullptr || sym->type()!=zenkit::DaedalusDataType::INSTANCE)
        return;
      const uint32_t id = sym->index();
      if(npc->itemCount(id)==0)
        npc->addItem(id,1);
      npc->equipItem(id);
      });
    bindNb("RX_GETNPCADDR", [this](std::shared_ptr<zenkit::INpc> ref) -> int32_t {
      auto* npc = findNpcRaw(ref);
      return npc!=nullptr ? int32_t(world().npcId(npc))+1 : 0;
      });

    // The scripts of NB explain why the AI stops (B_MM_AssessEnemy, ZS_MM_Attack, B_AssessDamage...):
    // CMD_COLOR("ZS_MM_Attack ", "self AIVRX_NPC_IS_NEUTRAL"), when RX_DEBUG_RETURN_SHOW is 1.
    // Each message goes to the log a few times only, the perceptions run several times a second.
    if(kNbScriptTrace) {
      if(auto* s = vm.find_symbol_by_name("RX_DEBUG_RETURN_SHOW")) {
        try { s->set_int(1); } catch(...) { Log::e("NB: RX_DEBUG_RETURN_SHOW cannot be set"); }
        }
      }
    auto traceSeen = std::make_shared<std::unordered_map<std::string,int>>();
    bindNb("CMD_COLOR", [traceSeen](std::string_view where, std::string_view why) {
      std::string key = std::string(where) + std::string(why);
      if(traceSeen->size()<2000 && ++(*traceSeen)[key]<=3)
        Log::i("NB trace: ",where,"| ",why);
      });

    // the log, the profiler and the brackets of the code of NB
    bindNb("RX_WRITELOG",            [](std::string_view msg) { Log::d("NB: ",msg); });
    bindNb("RX_PRINTDEBUGINFOFUNC",  [](std::string_view)     {});
    bindNb("RX_PRINTPERF",           [](std::string_view, int32_t) {});
    bindNb("RX_PRINTP",              [](std::string_view msg) { Gothic::inst().onPrint(std::string(msg).c_str()); });
    // the messages of NB on the screen (the plugin draws them; bound only if they are externals in GOTHIC.DAT)
    bindNb("AI_PRINT",               [](std::string_view msg) { Gothic::inst().onPrint(std::string(msg).c_str()); });
    bindNb("AI_PRINTRED",            [](std::string_view msg) { Gothic::inst().onPrint(std::string(msg).c_str()); });
    bindNb("AI_PRINTBONUS",          [](std::string_view msg) { Gothic::inst().onPrint(std::string(msg).c_str()); });
    bindNb("AI_FASTPRINTSKILLCRAFT", [](std::string_view msg) { Gothic::inst().onPrint(std::string(msg).c_str()); });
    bindNb("AI_NOTICEPRINT",         [](int32_t, int32_t, std::string_view msg) { Gothic::inst().onPrint(std::string(msg).c_str()); });
    // NB fix 45: the other messages of the plugin. Without them a refusal was silent, e.g. at a teacher
    // (B_TeachStamina: AI_PrintItemInfo / AI_PrintClr "not enough learning points / gold", "improve by +1"...)
    const auto nbPrint = [](std::string_view msg) {
      if(!msg.empty())
        Gothic::inst().onPrint(std::string(msg).c_str());
      };
    bindNb("AI_PRINTITEMINFO",  [nbPrint](std::string_view /*title*/, std::string_view msg, int32_t, int32_t) { nbPrint(msg); });
    bindNb("AI_PRINTCLR",       [nbPrint](std::string_view msg, int32_t, int32_t, int32_t) { nbPrint(msg); });
    bindNb("AI_PRINTCRAFT",     [nbPrint](std::string_view msg, int32_t, int32_t, int32_t) { nbPrint(msg); });
    bindNb("AI_FASTPRINT_1",    [nbPrint](std::string_view msg, int32_t, int32_t) { nbPrint(msg); });
    bindNb("AI_FASTPRINT_2",    [nbPrint](std::string_view msg, int32_t, int32_t) { nbPrint(msg); });
    bindNb("AI_PRINTEXP",       [nbPrint](std::string_view msg) { nbPrint(msg); });
    bindNb("AI_PRINTMIDDLE",    [nbPrint](std::string_view msg) { nbPrint(msg); });
    bindNb("AI_PRINTBOTTOMRED", [nbPrint](std::string_view msg) { nbPrint(msg); });
    bindNb("AI_PRINTBONUSITEM", [nbPrint](std::string_view msg) { nbPrint(msg); });
    bindNb("AI_PRINTBONUSLONG", [nbPrint](std::string_view msg) { nbPrint(msg); });
    bindNb("PRINTWINC",         [nbPrint](std::string_view msg) { nbPrint(msg); });
    bindNb("AI_PRINTPOS",       [nbPrint](std::string_view msg, int32_t, int32_t, std::string_view, int32_t, int32_t) { nbPrint(msg); });
    bindNb("AI_PRINTPOS_CLEAR", []() {});
    bindNb("AI_PRINTCLRSCREEN", []() {});
    bindNb("PRINTSCREENCOLOR",  [](std::string_view msg, int32_t x, int32_t y, std::string_view font, int32_t time, std::string_view /*color*/) {
      Gothic::inst().onPrintScreen(msg,x,y,time,Resources::font(font,Resources::FontType::Normal,1.0));
      });
    // B_LogEntry: "Nowy wpis w dzienniku!" + the name of the topic. As the Union plugin draws it: on the left,
    // at 29% of the height, the title in RX_QuestTitleFont (new: gold, failed: red, finished: green in the
    // plugin - here: yellow / red / white, the colors of OpenGothic fonts) and the topic in white below.
    const auto printQuest = [this](std::string_view title, std::string_view topic, Resources::FontType color) {
      std::string titleFont = "FONT_OLD_20_WHITE.TGA";
      if(auto* f = vm.find_symbol_by_name("RX_QUESTTITLEFONT"); f!=nullptr && f->type()==zenkit::DaedalusDataType::STRING) {
        if(!f->get_string().empty() && Resources::hasFile(f->get_string()))
          titleFont = f->get_string();
        }
      Gothic::inst().onPrintScreen(title,2,27,5,Resources::font(titleFont,color,1));
      Gothic::inst().onPrintScreen(topic,2,31,5,Resources::font("FONT_OLD_20_WHITE.TGA",Resources::FontType::Normal,1));
      };
    bindNb("AI_PRINTQUEST",          [printQuest](std::string_view t, std::string_view q) { printQuest(t,q,Resources::FontType::Yellow); });
    bindNb("AI_PRINTQUESTCOMPLETE",  [printQuest](std::string_view t, std::string_view q) { printQuest(t,q,Resources::FontType::Normal); });
    bindNb("AI_PRINTQUESTFAIL",      [printQuest](std::string_view t, std::string_view q) { printQuest(t,q,Resources::FontType::Red);    });
    bindNb("AI_FASTPRINTCLEAR",      []() {});
    bindNb("RX_BEGIN",               [](int32_t) {});
    bindNb("RX_END",                 [](int32_t) {});
    bindNb("RX_TOGGLEPLAYERCONTROLS",[](int32_t) {});

    // Menu_ReadInt / Menu_WriteInt: the options of NB in Gothic.ini ([AST] bSkipLogo, bCanSaveGame, bOptionHealth...).
    // They come from Union, without them every option is 0 and nothing is saved.
    // the default values of the options of the plugin, as Union writes them into Gothic.ini at the start
    NbOptions::ensureDefaults();
    bindNb("MENU_READINT", [](std::string_view sec, std::string_view key) -> int32_t {
      return Gothic::settingsGetI(sec,key);
      });
    bindNb("MENU_WRITEINT", [](std::string_view sec, std::string_view key, int32_t value) {
      Gothic::settingsSetI(sec,key,value);
      });

    // the tables of crafting (alchemy, runes, scrolls, anvil, ...): NB opens the window of its DLL here.
    // The scripts of NB still have the old menus of dialogs (PC_Health_Start, PC_Circle, PC_..._End),
    // so this does what AI_ProcessInfos(hero) does in Gothic 2: the table opens the menu of the recipes.
    // RX_SetWeightValue(item, RX_W_*, value) in the instances of the items: the plugin keeps it per the
    // instance; used for the weight in the description (nbiteminfo.h) and by the scripts (Hero_Poisoned)
    bindNb("RX_SETWEIGHTVALUEEXT", [](std::shared_ptr<zenkit::IItem> itm, int32_t index, int32_t value) {
      if(itm!=nullptr)
        NbItemInfo::setWeight(*itm,index,value);
      });
    bindNb("RX_GETWEIGHTVALUE", [](std::shared_ptr<zenkit::IItem> itm, int32_t index) -> int32_t {
      return itm!=nullptr ? NbItemInfo::weight(itm->symbol_index(),index) : 0;
      });
    // B_SetFightSkills(npc, percent) for NPCs: the plugin adds percent to the hit chances (1H, 2H, bow,
    // crossbow; 0..100) and gives the matching fighting skill and overlay - ST3 from 90%, ST2 from 60%,
    // ST1 from 30% (bow/crossbow: T2 from 60%, T1 from 30%). Without it every NPC of NB stayed at 0%:
    // the untrained animations of a beginner and no critical hits (UNION_ABI.DLL 0x10198B20)
    bindNb("RX_APPLYPROPEROVERLAYSFORNPC", [this](std::shared_ptr<zenkit::INpc> npcRef, int32_t percent) {
      if(auto npc = findNpc(npcRef))
        NbMoveset::applyFightSkills(*npc,percent);
      });
    // re-reads the animation ids of the model after the overlays changed; OpenGothic looks the
    // animations up by name each time, so there is nothing to do
    bindNb("RX_INITANIMATIONS", [](std::shared_ptr<zenkit::INpc>) {});
    // B_SelectWeapon(slf, oth) of NB ends with RX_SelectWeaponNpc(slf, oth): the plugin draws the weapon for the fight.
    // Without it no NPC drew a weapon in a fight (fix 25). NB fix 31: the rules of the plugin (UNION_ABI.DLL 0x1017F840):
    //  - with a bow/crossbow and arrows the NPC shoots, unless the enemy is closer than 600 (and AIVRX_NPC_RANGE_ONLY is 0);
    //    bloodflies and harpies are always shot; an enemy immune to arrows (PROT_POINT -1, golems, trolls, guardians)
    //    only by a monster archer (AIVRX_IS_MONSTER_ARCHER);
    //  - an NPC that already shoots does not change to the sword for the animals (wolves, scavengers, rats, gobbos...);
    //  - after a change of the weapon the next one waits 6 s (AIVRX_NPC_CHANGEWEAPON_TIMER), with arrows
    // That is why Lares shoots the wolves with his bow.
    bindNb("RX_SELECTWEAPONNPC", [this](std::shared_ptr<zenkit::INpc> slfRef, std::shared_ptr<zenkit::INpc> othRef) {
      static std::unordered_map<const Npc*,uint64_t> changeTime;
      auto* slf = findNpcRaw(slfRef);
      auto* oth = findNpcRaw(othRef);
      if(slf==nullptr || slf->isDown())
        return;
      // ZS_Attack_Loop asks every frame: the weapon is drawn once, not once per frame
      if(!slf->isAiQueueEmpty())
        return;
      const auto  ws      = slf->weaponState();
      const bool  inRange = (ws==WeaponState::Bow || ws==WeaponState::CBow);
      const bool  melee   = slf->currentMeleeWeapon()!=nullptr;
      // (Npc::hasAmmunition looks only at the drawn weapon: with the sword in the hands it said "no arrows")
      bool ammo = false;
      if(auto* rw = slf->currentRangedWeapon()) {
        const int32_t mun = rw->handle().munition;
        ammo = mun>0 && slf->inventory().itemCount(size_t(mun))>0;
        }
      const float dist    = oth!=nullptr ? std::sqrt(slf->qDistTo(*oth)) : 0.f;
      const int   og      = oth!=nullptr ? int(oth->guild()) : 0;

      bool wantRanged = false, wantMelee = false, decided = false;
      if(ammo) {
        const bool close = slf->newBalanceGetNpcVar(55)==0 && dist<600.f && og!=30 && og!=43;
        if(!close) {
          const bool immune = oth!=nullptr && (oth->handle().protection[zenkit::DamageType::POINT]==-1 || og==44 || og==50 || og==56);
          if(!immune || slf->newBalanceGetNpcVar(136)!=0) {
            wantRanged = !inRange;
            decided    = true;
            }
          }
        }
      if(!decided) {
        if(ws!=WeaponState::Fist && !melee)
          wantMelee = true;                       // fists
        else if(melee && ws!=WeaponState::W1H && ws!=WeaponState::W2H)
          wantMelee = true;
        }
      static const int animals[] = {32,24,27,29,30,31,34,35,40,41,57,43,50,51,53,54,28,22,23};
      if(inRange && ammo && wantMelee && std::find(std::begin(animals),std::end(animals),og)!=std::end(animals))
        wantMelee = false;

      const uint64_t now   = tickCount();
      auto           it    = changeTime.find(slf);
      const bool     timer = (it!=changeTime.end() && now<it->second+6000);
      if(changeTime.size()>4096)
        changeTime.clear();
      if(wantRanged) {
        if(timer)
          return;
        slf->aiPush(AiQueue::aiRemoveWeapon());
        slf->aiPush(AiQueue::aiReadyRangedWeapon());
        changeTime[slf] = now;
        return;
        }
      if(wantMelee) {
        if(timer && ammo)
          return;
        if(ws!=WeaponState::NoWeapon)
          slf->aiPush(AiQueue::aiRemoveWeapon());
        slf->aiPush(AiQueue::aiReadyMeleeWeapon());
        changeTime[slf] = now;
        }
      });

    // B_CheckHealth of NB runs at the start of every state of a human (AIV_TAPOSITION = NOTINPOS) and calls
    // B_Npc_Desynchronize(npc, nCycleChar): AI_WaitMS with a global counter 250, 500 ... 3000 ms, so that the
    // characters of a place do not all move at the same moment. In ZS_Guide_Player / ZS_Follow_Player the
    // position is never reached, so a companion waited up to 3 s after every fight, dialog and stop before it
    // went on. For the members of the party (AIV_PARTYMEMBER) the wait is skipped, the others wait as in NB.
    if(auto* ds = vm.find_symbol_by_name("B_NPC_DESYNCHRONIZE"); ds!=nullptr && !ds->is_external()) {
      vm.override_function("B_NPC_DESYNCHRONIZE", [this](std::shared_ptr<zenkit::INpc> slf, int32_t ncycle) {
        auto* npc = findNpcRaw(slf);
        if(npc==nullptr || ncycle<=0)
          return;
        if(owner.version().game==2 && npc->handle().aivar[15]!=0)
          return;
        npc->aiPush(AiQueue::aiWait(uint64_t(ncycle)));
        });
      }

    // Npc_GetGuildAttitude(npc, other): the attitude of the guilds (the table of Wld_SetGuildAttitude);
    // the scripts of NB ask it in B_CheckHealth and the reactions to the hero. Without it: 0 = ATT_HOSTILE
    bindNb("NPC_GETGUILDATTITUDE", [this](std::shared_ptr<zenkit::INpc> aRef, std::shared_ptr<zenkit::INpc> bRef) -> int32_t {
      auto* a = findNpcRaw(aRef);
      auto* b = findNpcRaw(bRef);
      return a!=nullptr && b!=nullptr ? int32_t(guildAttitude(*a,*b)) : int32_t(ATT_NEUTRAL);
      });
    // NB fix 32: the overlays of the weapons of the hero: RX_ResetAllOverlays (RX_ApplyAnim_Weapons, at every change of
    // the weapon) takes ~60 overlays away with Mdl_RemoveOverlayMds_NoUpdate - an external of the plugin, unknown here.
    // Nothing was taken away: once a rapier was in the hands, Humans_Rapier_ST* stayed for ever and every one-handed
    // weapon (a knife, a club) fought like a rapier. NoUpdate = without the re-init of the model (OpenGothic needs none).
    bindNb("MDL_REMOVEOVERLAYMDS_NOUPDATE", [this](std::shared_ptr<zenkit::INpc> npcRef, std::string_view name) {
      if(auto npc = findNpc(npcRef))
        if(auto sk = Resources::loadSkeleton(name))
          npc->delOverlay(sk);
      });
    bindNb("MDL_APPLYOVERLAYMDS_NOUPDATE", [this](std::shared_ptr<zenkit::INpc> npcRef, std::string_view name) {
      if(auto npc = findNpc(npcRef))
        if(auto sk = Resources::loadSkeleton(name))
          npc->addOverlay(sk,0);
      });
    bindNb("MDL_HASOVERLAYMDS", [this](std::shared_ptr<zenkit::INpc> npcRef, std::string_view name) -> int32_t {
      auto npc = findNpc(npcRef);
      if(npc==nullptr)
        return 0;
      auto sk = Resources::loadSkeleton(name);
      return sk!=nullptr && npc->hasOverlay(sk) ? 1 : 0;
      });
    bindNb("RX_OPENCRAFT", [this](int32_t /*craftType*/) {
      if(auto pl = owner.player()) {
        aiOutOrderId = 0;
        pl->aiPush(AiQueue::aiProcessInfo(*pl));
        }
      });

    // NB fix 41: the stamina. The scripts take it while sprinting only when the run is played (RX_IsRunAniActive), while
    // diving by the water level, and they show it in a bar of the plugin (Bar_*)
    const auto aniActive = [](Npc& npc, std::string_view name) {
      std::string up(name);
      for(auto& c:up)
        c = char(std::toupper(static_cast<unsigned char>(c)));
      for(auto& l:npc.animationLayers())
        if(l.name==up)
          return true;
      return false;
      };
    bindNb("RX_ISANIACTIVE", [this,aniActive](std::shared_ptr<zenkit::INpc> npcRef, std::string_view name) -> int32_t {
      auto npc = findNpc(npcRef);
      return npc!=nullptr && aniActive(*npc,name) ? 1 : 0;
      });
    bindNb("NPC_ISANIMATIONACTIVE", [this,aniActive](std::shared_ptr<zenkit::INpc> npcRef, std::string_view name) -> int32_t {
      auto npc = findNpc(npcRef);
      return npc!=nullptr && aniActive(*npc,name) ? 1 : 0;
      });
    bindNb("RX_ISRUNANIACTIVE", [this](std::shared_ptr<zenkit::INpc> npcRef) -> int32_t {
      // the loop of the run in any weapon mode: S_RUNL, S_1HRUNL, S_FISTRUNL, S_BOWRUNL, S_MAGRUNL...
      auto npc = findNpc(npcRef);
      if(npc==nullptr)
        return 0;
      for(auto& l:npc->animationLayers())
        if(l.name.rfind("S_",0)==0 && l.name.find("RUNL")!=std::string::npos)
          return 1;
      return 0;
      });
    bindNb("NPC_GETWATERLEVEL", [this](std::shared_ptr<zenkit::INpc> npcRef) -> int32_t {
      // WATER_NONE 0, WATER_FEET 1, WATER_DIVE 2
      auto npc = findNpc(npcRef);
      if(npc==nullptr)
        return 0;
      if(npc->isDive())
        return 2;
      return npc->isSwim() || npc->isInWater() ? 1 : 0;
      });
    NbBars::reset();
    bindNb("BAR_SETSHOW", [](int32_t bar, int32_t show) {
      if(auto b = NbBars::at(bar))
        b->show = show!=0;
      });
    bindNb("BAR_GETSHOW", [](int32_t bar) -> int32_t {
      auto b = NbBars::at(bar);
      return b!=nullptr && b->show ? 1 : 0;
      });
    bindNb("BAR_SETBARTEX", [](int32_t bar, std::string_view tex) {
      if(auto b = NbBars::at(bar))
        b->tex = std::string(tex);
      });
    bindNb("BAR_SETVALUE", [](int32_t bar, int32_t value) {
      if(auto b = NbBars::at(bar))
        b->value = value;
      });
    bindNb("BAR_SETMAXRANGE", [](int32_t bar, int32_t range) {
      if(auto b = NbBars::at(bar))
        b->maxRange = range;
      });

    {
    std::string list;
    for(auto& n : nbBound)
      list += n + " ";
    Log::e("NB: bound ",int(nbBound.size())," externals: ",list);
    }
    // what is still missing, with the signature from GOTHIC.DAT
    for(uint32_t i=0; i<vm.symbols().size(); ++i) {
      auto* s = vm.find_symbol_by_index(i);
      if(!s->is_external() || s->name().rfind("RX",0)!=0)
        continue;
      if(std::find(nbBound.begin(),nbBound.end(),s->name())!=nbBound.end())
        continue;
      Log::i("NB: not implemented ",nbSignature(vm,*s));
      }
    }
  bindExternal("hlp_random",                     &GameScript::hlp_random);
  bindExternal("hlp_isvalidnpc",                 &GameScript::hlp_isvalidnpc);
  bindExternal("hlp_isvaliditem",                &GameScript::hlp_isvaliditem);
  bindExternal("hlp_isitem",                     &GameScript::hlp_isitem);
  bindExternal("hlp_getnpc",                     &GameScript::hlp_getnpc);
  bindExternal("hlp_getinstanceid",              &GameScript::hlp_getinstanceid);

  bindExternal("wld_insertnpc",                  &GameScript::wld_insertnpc);
  bindExternal("wld_removenpc",                  &GameScript::wld_removenpc);
  bindExternal("wld_insertitem",                 &GameScript::wld_insertitem);
  bindExternal("wld_insertobject",               &GameScript::wld_insertobject);
  bindExternal("wld_removeitem",                 &GameScript::wld_removeitem);
  bindExternal("wld_settime",                    &GameScript::wld_settime);
  bindExternal("wld_getday",                     &GameScript::wld_getday);
  bindExternal("wld_playeffect",                 &GameScript::wld_playeffect);
  bindExternal("wld_stopeffect",                 &GameScript::wld_stopeffect);
  bindExternal("wld_getplayerportalguild",       &GameScript::wld_getplayerportalguild);
  bindExternal("wld_getformerplayerportalguild", &GameScript::wld_getformerplayerportalguild);
  bindExternal("wld_setguildattitude",           &GameScript::wld_setguildattitude);
  bindExternal("wld_getguildattitude",           &GameScript::wld_getguildattitude);
  bindExternal("wld_exchangeguildattitudes",     &GameScript::wld_exchangeguildattitudes);
  bindExternal("wld_istime",                     &GameScript::wld_istime);
  bindExternal("wld_isfpavailable",              &GameScript::wld_isfpavailable);
  bindExternal("wld_isnextfpavailable",          &GameScript::wld_isnextfpavailable);
  bindExternal("wld_ismobavailable",             &GameScript::wld_ismobavailable);
  bindExternal("wld_setmobroutine",              &GameScript::wld_setmobroutine);
  bindExternal("wld_setobjectroutine",           &GameScript::wld_setobjectroutine);
  bindExternal("wld_getmobstate",                &GameScript::wld_getmobstate);
  bindExternal("wld_assignroomtoguild",          &GameScript::wld_assignroomtoguild);
  bindExternal("wld_detectnpc",                  &GameScript::wld_detectnpc);
  bindExternal("wld_detectnpcex",                &GameScript::wld_detectnpcex);
  bindExternal("wld_detectitem",                 &GameScript::wld_detectitem);
  bindExternal("wld_spawnnpcrange",              &GameScript::wld_spawnnpcrange);
  bindExternal("wld_sendtrigger",                &GameScript::wld_sendtrigger);
  bindExternal("wld_senduntrigger",              &GameScript::wld_senduntrigger);
  bindExternal("wld_israining",                  &GameScript::wld_israining);

  bindExternal("mdl_setvisual",                  &GameScript::mdl_setvisual);
  bindExternal("mdl_setvisualbody",              &GameScript::mdl_setvisualbody);
  bindExternal("mdl_setmodelfatness",            &GameScript::mdl_setmodelfatness);
  bindExternal("mdl_applyoverlaymds",            &GameScript::mdl_applyoverlaymds);
  bindExternal("mdl_applyoverlaymdstimed",       &GameScript::mdl_applyoverlaymdstimed);
  bindExternal("mdl_removeoverlaymds",           &GameScript::mdl_removeoverlaymds);
  bindExternal("mdl_setmodelscale",              &GameScript::mdl_setmodelscale);
  bindExternal("mdl_startfaceani",               &GameScript::mdl_startfaceani);
  bindExternal("mdl_applyrandomani",             &GameScript::mdl_applyrandomani);
  bindExternal("mdl_applyrandomanifreq",         &GameScript::mdl_applyrandomanifreq);
  bindExternal("mdl_applyrandomfaceani",         &GameScript::mdl_applyrandomfaceani);

  bindExternal("npc_settofightmode",             &GameScript::npc_settofightmode);
  bindExternal("npc_settofistmode",              &GameScript::npc_settofistmode);
  bindExternal("npc_isinstate",                  &GameScript::npc_isinstate);
  bindExternal("npc_isinroutine",                &GameScript::npc_isinroutine);
  bindExternal("npc_wasinstate",                 &GameScript::npc_wasinstate);
  bindExternal("npc_getdisttowp",                &GameScript::npc_getdisttowp);
  bindExternal("npc_exchangeroutine",            &GameScript::npc_exchangeroutine);
  bindExternal("npc_isdead",                     &GameScript::npc_isdead);
  bindExternal("npc_knowsinfo",                  &GameScript::npc_knowsinfo);
  bindExternal("npc_settalentskill",             &GameScript::npc_settalentskill);
  bindExternal("npc_gettalentskill",             &GameScript::npc_gettalentskill);
  bindExternal("npc_settalentvalue",             &GameScript::npc_settalentvalue);
  bindExternal("npc_gettalentvalue",             &GameScript::npc_gettalentvalue);
  bindExternal("npc_setrefusetalk",              &GameScript::npc_setrefusetalk);
  bindExternal("npc_refusetalk",                 &GameScript::npc_refusetalk);
  bindExternal("npc_hasitems",                   &GameScript::npc_hasitems);
  bindExternal("npc_hasspell",                   &GameScript::npc_hasspell);
  bindExternal("npc_getinvitem",                 &GameScript::npc_getinvitem);
  bindExternal("npc_getinvitembyslot",           &GameScript::npc_getinvitembyslot);
  bindExternal("npc_removeinvitem",              &GameScript::npc_removeinvitem);
  bindExternal("npc_removeinvitems",             &GameScript::npc_removeinvitems);
  bindExternal("npc_getbodystate",               &GameScript::npc_getbodystate);
  bindExternal("npc_getlookattarget",            &GameScript::npc_getlookattarget);
  bindExternal("npc_getdisttonpc",               &GameScript::npc_getdisttonpc);
  bindExternal("npc_hasequippedarmor",           &GameScript::npc_hasequippedarmor);
  bindExternal("npc_setperctime",                &GameScript::npc_setperctime);
  bindExternal("npc_percenable",                 &GameScript::npc_percenable);
  bindExternal("npc_percdisable",                &GameScript::npc_percdisable);
  bindExternal("npc_getnearestwp",               &GameScript::npc_getnearestwp);
  bindExternal("npc_getnextwp",                  &GameScript::npc_getnextwp);
  bindExternal("npc_clearaiqueue",               &GameScript::npc_clearaiqueue);
  bindExternal("npc_isplayer",                   &GameScript::npc_isplayer);
  bindExternal("npc_getstatetime",               &GameScript::npc_getstatetime);
  bindExternal("npc_setstatetime",               &GameScript::npc_setstatetime);
  bindExternal("npc_changeattribute",            &GameScript::npc_changeattribute);
  bindExternal("npc_isonfp",                     &GameScript::npc_isonfp);
  bindExternal("npc_getheighttonpc",             &GameScript::npc_getheighttonpc);
  bindExternal("npc_getequippedmeleeweapon",     &GameScript::npc_getequippedmeleeweapon);
  bindExternal("npc_getequippedrangedweapon",    &GameScript::npc_getequippedrangedweapon);
  bindExternal("npc_getequippedarmor",           &GameScript::npc_getequippedarmor);
  bindExternal("npc_canseenpc",                  &GameScript::npc_canseenpc);
  bindExternal("npc_canseenpcfreelos",           &GameScript::npc_canseenpcfreelos);
  bindExternal("npc_canseeitem",                 &GameScript::npc_canseeitem);
  bindExternal("npc_hasequippedweapon",          &GameScript::npc_hasequippedweapon);
  bindExternal("npc_hasequippedmeleeweapon",     &GameScript::npc_hasequippedmeleeweapon);
  bindExternal("npc_hasequippedrangedweapon",    &GameScript::npc_hasequippedrangedweapon);
  bindExternal("npc_getactivespell",             &GameScript::npc_getactivespell);
  bindExternal("npc_getactivespellisscroll",     &GameScript::npc_getactivespellisscroll);
  bindExternal("npc_getactivespellcat",          &GameScript::npc_getactivespellcat);
  bindExternal("npc_setactivespellinfo",         &GameScript::npc_setactivespellinfo);
  bindExternal("npc_getactivespelllevel",        &GameScript::npc_getactivespelllevel);
  bindExternal("npc_isinfightmode",              &GameScript::npc_isinfightmode);
  bindExternal("npc_settarget",                  &GameScript::npc_settarget);
  bindExternal("npc_gettarget",                  &GameScript::npc_gettarget);
  bindExternal("npc_getnexttarget",              &GameScript::npc_getnexttarget);
  bindExternal("npc_sendpassiveperc",            &GameScript::npc_sendpassiveperc);
  bindExternal("npc_sendsingleperc",             &GameScript::npc_sendsingleperc);
  bindExternal("npc_checkinfo",                  &GameScript::npc_checkinfo);
  bindExternal("npc_getportalguild",             &GameScript::npc_getportalguild);
  bindExternal("npc_isinplayersroom",            &GameScript::npc_isinplayersroom);
  bindExternal("npc_getreadiedweapon",           &GameScript::npc_getreadiedweapon);
  bindExternal("npc_hasreadiedweapon",           &GameScript::npc_hasreadiedweapon);
  bindExternal("npc_hasreadiedmeleeweapon",      &GameScript::npc_hasreadiedmeleeweapon);
  bindExternal("npc_hasreadiedrangedweapon",     &GameScript::npc_hasreadiedrangedweapon);
  bindExternal("npc_hasrangedweaponwithammo",    &GameScript::npc_hasrangedweaponwithammo);
  bindExternal("npc_isdrawingspell",             &GameScript::npc_isdrawingspell);
  bindExternal("npc_isdrawingweapon",            &GameScript::npc_isdrawingweapon);
  bindExternal("npc_perceiveall",                &GameScript::npc_perceiveall);
  bindExternal("npc_stopani",                    &GameScript::npc_stopani);
  bindExternal("npc_settrueguild",               &GameScript::npc_settrueguild);
  bindExternal("npc_gettrueguild",               &GameScript::npc_gettrueguild);
  bindExternal("npc_clearinventory",             &GameScript::npc_clearinventory);
  bindExternal("npc_getattitude",                &GameScript::npc_getattitude);
  bindExternal("npc_getpermattitude",            &GameScript::npc_getpermattitude);
  bindExternal("npc_setattitude",                &GameScript::npc_setattitude);
  bindExternal("npc_settempattitude",            &GameScript::npc_settempattitude);
  bindExternal("npc_hasbodyflag",                &GameScript::npc_hasbodyflag);
  bindExternal("npc_getlasthitspellid",          &GameScript::npc_getlasthitspellid);
  bindExternal("npc_getlasthitspellcat",         &GameScript::npc_getlasthitspellcat);
  bindExternal("npc_playani",                    &GameScript::npc_playani);
  bindExternal("npc_isdetectedmobownedbynpc",    &GameScript::npc_isdetectedmobownedbynpc);
  bindExternal("npc_getdetectedmob",             &GameScript::npc_getdetectedmob);
  bindExternal("npc_isdetectedmobownedbyguild",  &GameScript::npc_isdetectedmobownedbyguild);
  bindExternal("npc_ownedbynpc",                 &GameScript::npc_ownedbynpc);
  bindExternal("npc_canseesource",               &GameScript::npc_canseesource);
  bindExternal("npc_isincutscene",               &GameScript::npc_isincutscene);
  bindExternal("npc_getdisttoitem",              &GameScript::npc_getdisttoitem);
  bindExternal("npc_getheighttoitem",            &GameScript::npc_getheighttoitem);
  bindExternal("npc_getdisttoplayer",            &GameScript::npc_getdisttoplayer);

  bindExternal("ai_output",                      &GameScript::ai_output);
  bindExternal("ai_stopprocessinfos",            &GameScript::ai_stopprocessinfos);
  bindExternal("ai_processinfos",                &GameScript::ai_processinfos);
  bindExternal("ai_standup",                     &GameScript::ai_standup);
  bindExternal("ai_standupquick",                &GameScript::ai_standupquick);
  bindExternal("ai_continueroutine",             &GameScript::ai_continueroutine);
  bindExternal("ai_stoplookat",                  &GameScript::ai_stoplookat);
  bindExternal("ai_lookat",                      &GameScript::ai_lookat);
  bindExternal("ai_lookatnpc",                   &GameScript::ai_lookatnpc);
  bindExternal("ai_removeweapon",                &GameScript::ai_removeweapon);
  bindExternal("ai_unreadyspell",                &GameScript::ai_unreadyspell);
  bindExternal("ai_turnaway",                    &GameScript::ai_turnaway);
  bindExternal("ai_turntonpc",                   &GameScript::ai_turntonpc);
  bindExternal("ai_whirlaround",                 &GameScript::ai_whirlaround);
  bindExternal("ai_outputsvm",                   &GameScript::ai_outputsvm);
  bindExternal("ai_outputsvm_overlay",           &GameScript::ai_outputsvm_overlay);
  bindExternal("ai_startstate",                  &GameScript::ai_startstate);
  bindExternal("ai_playani",                     &GameScript::ai_playani);
  bindExternal("ai_setwalkmode",                 &GameScript::ai_setwalkmode);
  bindExternal("ai_wait",                        &GameScript::ai_wait);
  bindExternal("ai_waitms",                      &GameScript::ai_waitms);
  bindExternal("ai_waittillend",                 &GameScript::ai_waittillend);
  bindExternal("ai_aimat",                       &GameScript::ai_aimat);
  bindExternal("ai_shootat",                     &GameScript::ai_shootat);
  bindExternal("ai_stopaim",                     &GameScript::ai_stopaim);
  bindExternal("ai_dropitem",                    &GameScript::ai_dropitem);
  bindExternal("ai_snd_play",                    &GameScript::ai_snd_play);
  bindExternal("ai_stopfx",                      &GameScript::ai_stopfx);
  bindExternal("ai_aligntowp",                   &GameScript::ai_aligntowp);
  bindExternal("ai_gotowp",                      &GameScript::ai_gotowp);
  bindExternal("ai_gotofp",                      &GameScript::ai_gotofp);
  bindExternal("ai_playanibs",                   &GameScript::ai_playanibs);
  bindExternal("ai_equiparmor",                  &GameScript::ai_equiparmor);
  bindExternal("ai_equipbestarmor",              &GameScript::ai_equipbestarmor);
  bindExternal("ai_equipbestmeleeweapon",        &GameScript::ai_equipbestmeleeweapon);
  bindExternal("ai_equipbestrangedweapon",       &GameScript::ai_equipbestrangedweapon);
  bindExternal("ai_usemob",                      &GameScript::ai_usemob);
  bindExternal("ai_teleport",                    &GameScript::ai_teleport);
  bindExternal("ai_stoppointat",                 &GameScript::ai_stoppointat);
  bindExternal("ai_drawweapon",                  &GameScript::ai_drawweapon);
  bindExternal("ai_readymeleeweapon",            &GameScript::ai_readymeleeweapon);
  bindExternal("ai_readyrangedweapon",           &GameScript::ai_readyrangedweapon);
  bindExternal("ai_readyspell",                  &GameScript::ai_readyspell);
  bindExternal("ai_attack",                      &GameScript::ai_attack);
  bindExternal("ai_flee",                        &GameScript::ai_flee);
  bindExternal("ai_dodge",                       &GameScript::ai_dodge);
  bindExternal("ai_unequipweapons",              &GameScript::ai_unequipweapons);
  bindExternal("ai_unequiparmor",                &GameScript::ai_unequiparmor);
  bindExternal("ai_gotonpc",                     &GameScript::ai_gotonpc);
  bindExternal("ai_gotonextfp",                  &GameScript::ai_gotonextfp);
  bindExternal("ai_aligntofp",                   &GameScript::ai_aligntofp);
  bindExternal("ai_useitem",                     &GameScript::ai_useitem);
  bindExternal("ai_useitemtostate",              &GameScript::ai_useitemtostate);
  bindExternal("ai_setnpcstostate",              &GameScript::ai_setnpcstostate);
  bindExternal("ai_finishingmove",               &GameScript::ai_finishingmove);
  bindExternal("ai_takeitem",                    &GameScript::ai_takeitem);
  bindExternal("ai_gotoitem",                    &GameScript::ai_gotoitem);
  bindExternal("ai_pointat",                     &GameScript::ai_pointat);
  bindExternal("ai_pointatnpc",                  &GameScript::ai_pointatnpc);

  bindExternal("mob_hasitems",                   &GameScript::mob_hasitems);
  bindExternal("mob_createitems",                &GameScript::mob_createitems);
  bindExternal("ai_printscreen",                 &GameScript::ai_printscreen);

  bindExternal("ta_min",                         &GameScript::ta_min);

  bindExternal("log_createtopic",                &GameScript::log_createtopic);
  bindExternal("log_settopicstatus",             &GameScript::log_settopicstatus);
  bindExternal("log_addentry",                   &GameScript::log_addentry);

  bindExternal("equipitem",                      &GameScript::equipitem);
  bindExternal("createinvitem",                  &GameScript::createinvitem);
  bindExternal("createinvitems",                 &GameScript::createinvitems);

  bindExternal("perc_setrange",                  &GameScript::perc_setrange);

  bindExternal("info_addchoice",                 &GameScript::info_addchoice);
  bindExternal("info_clearchoices",              &GameScript::info_clearchoices);
  bindExternal("infomanager_hasfinished",        &GameScript::infomanager_hasfinished);

  bindExternal("snd_play",                       &GameScript::snd_play);
  bindExternal("snd_play3d",                     &GameScript::snd_play3d);

  bindExternal("game_initgerman",                &GameScript::game_initgerman);
  bindExternal("game_initenglish",               &GameScript::game_initenglish);

  bindExternal("exitsession",                    &GameScript::exitsession);

  // vm.validateExternals();

  spells               = std::make_unique<SpellDefinitions>(vm);
  svm                  = std::make_unique<SvmDefinitions>(vm);

  cFocusNorm           = findFocus("Focus_Normal");
  cFocusMelee          = findFocus("Focus_Melee");
  cFocusRange          = findFocus("Focus_Ranged");
  cFocusMage           = findFocus("Focus_Magic");

  ZS_Dead              = aiState(findSymbolIndex("ZS_Dead")).funcIni;
  ZS_Unconscious       = aiState(findSymbolIndex("ZS_Unconscious")).funcIni;
  ZS_Talk              = aiState(findSymbolIndex("ZS_Talk")).funcIni;
  ZS_Attack            = aiState(findSymbolIndex("ZS_Attack")).funcIni;
  ZS_MM_Attack         = aiState(findSymbolIndex("ZS_MM_Attack")).funcIni;

  spellFxInstanceNames = vm.find_symbol_by_name("spellFxInstanceNames");
  spellFxAniLetters    = vm.find_symbol_by_name("spellFxAniLetters");

  if(spellFxInstanceNames==nullptr || spellFxAniLetters==nullptr) {
    throw std::runtime_error("spellFxInstanceNames and/or spellFxAniLetters not found");
    }

  if(owner.version().game==2) {
    auto* currency = vm.find_symbol_by_name("TRADE_CURRENCY_INSTANCE");
    itMi_Gold      = currency!=nullptr ? vm.find_symbol_by_name(currency->get_string()) : nullptr;
    if(itMi_Gold!=nullptr){ // FIXME
      auto item = vm.init_instance<zenkit::IItem>(itMi_Gold);
      goldTxt = item->name;
      }
    auto* tradeMul = vm.find_symbol_by_name("TRADE_VALUE_MULTIPLIER");
    tradeValMult   = tradeMul != nullptr ? tradeMul->get_float() : 1.0f;

    auto* vtime     = vm.find_symbol_by_name("VIEW_TIME_PER_CHAR");
    viewTimePerChar = vtime != nullptr ? vtime->get_float() : 550.f;
    if(viewTimePerChar<=0.f)
      viewTimePerChar = 550.f;

    ItKE_lockpick     = vm.find_symbol_by_name("ItKE_lockpick");
    B_RefreshAtInsert = vm.find_symbol_by_name("B_RefreshAtInsert");
    } else {
    itMi_Gold      = vm.find_symbol_by_name("ItMiNugget");
    if(itMi_Gold!=nullptr) { // FIXME
      auto item = vm.init_instance<zenkit::IItem>(itMi_Gold);
      goldTxt = item->name;
      }
    //
    tradeValMult    = 1.f;
    viewTimePerChar = 550.f;
    ItKE_lockpick   = vm.find_symbol_by_name("itkelockpick");
    }

  if(auto v = vm.find_symbol_by_name("DAM_CRITICAL_MULTIPLIER")) {
    damCriticalMultiplier = v->get_int();
    }

  auto* gilMax = vm.find_symbol_by_name("GIL_MAX");
  gilCount = gilMax!=nullptr ? size_t(gilMax->get_int()) : 0;

  auto* tblSz = vm.find_symbol_by_name("TAB_ANZAHL");
  gilTblSize = tblSz!=nullptr ? size_t(std::sqrt(tblSz->get_int())) : 0;
  gilAttitudes.resize(gilCount*gilCount,ATT_HOSTILE);
  wld_exchangeguildattitudes("GIL_ATTITUDES");

  auto id = vm.find_symbol_by_name("Gil_Values");
  if(id!=nullptr){
    cGuildVal = vm.init_instance<zenkit::IGuildValues>(id);
    for(size_t i=0;i<Guild::GIL_PUBLIC;++i){
      cGuildVal->water_depth_knee   [i]=cGuildVal->water_depth_knee   [Guild::GIL_HUMAN];
      cGuildVal->water_depth_chest  [i]=cGuildVal->water_depth_chest  [Guild::GIL_HUMAN];
      cGuildVal->jumpup_height      [i]=cGuildVal->jumpup_height      [Guild::GIL_HUMAN];
      cGuildVal->swim_time          [i]=cGuildVal->swim_time          [Guild::GIL_HUMAN];
      cGuildVal->dive_time          [i]=cGuildVal->dive_time          [Guild::GIL_HUMAN];
      cGuildVal->step_height        [i]=cGuildVal->step_height        [Guild::GIL_HUMAN];
      cGuildVal->jumplow_height     [i]=cGuildVal->jumplow_height     [Guild::GIL_HUMAN];
      cGuildVal->jumpmid_height     [i]=cGuildVal->jumpmid_height     [Guild::GIL_HUMAN];
      cGuildVal->slide_angle        [i]=cGuildVal->slide_angle        [Guild::GIL_HUMAN];
      cGuildVal->slide_angle2       [i]=cGuildVal->slide_angle2       [Guild::GIL_HUMAN];
      cGuildVal->disable_autoroll   [i]=cGuildVal->disable_autoroll   [Guild::GIL_HUMAN];
      cGuildVal->surface_align      [i]=cGuildVal->surface_align      [Guild::GIL_HUMAN];
      cGuildVal->climb_heading_angle[i]=cGuildVal->climb_heading_angle[Guild::GIL_HUMAN];
      cGuildVal->climb_horiz_angle  [i]=cGuildVal->climb_horiz_angle  [Guild::GIL_HUMAN];
      cGuildVal->climb_ground_angle [i]=cGuildVal->climb_ground_angle [Guild::GIL_HUMAN];
      cGuildVal->fight_range_base   [i]=cGuildVal->fight_range_base   [Guild::GIL_HUMAN];
      cGuildVal->fight_range_fist   [i]=cGuildVal->fight_range_fist   [Guild::GIL_HUMAN];
      cGuildVal->fight_range_g      [i]=cGuildVal->fight_range_g      [Guild::GIL_HUMAN];
      cGuildVal->fight_range_1hs    [i]=cGuildVal->fight_range_1hs    [Guild::GIL_HUMAN];
      cGuildVal->fight_range_1ha    [i]=cGuildVal->fight_range_1ha    [Guild::GIL_HUMAN];
      cGuildVal->fight_range_2hs    [i]=cGuildVal->fight_range_2hs    [Guild::GIL_HUMAN];
      cGuildVal->fight_range_2ha    [i]=cGuildVal->fight_range_2ha    [Guild::GIL_HUMAN];
      cGuildVal->falldown_height    [i]=cGuildVal->falldown_height    [Guild::GIL_HUMAN];
      cGuildVal->falldown_damage    [i]=cGuildVal->falldown_damage    [Guild::GIL_HUMAN];
      cGuildVal->blood_disabled     [i]=cGuildVal->blood_disabled     [Guild::GIL_HUMAN];
      cGuildVal->blood_max_distance [i]=cGuildVal->blood_max_distance [Guild::GIL_HUMAN];
      cGuildVal->blood_amount       [i]=cGuildVal->blood_amount       [Guild::GIL_HUMAN];
      cGuildVal->blood_flow         [i]=cGuildVal->blood_flow         [Guild::GIL_HUMAN];
      cGuildVal->blood_emitter      [i]=cGuildVal->blood_emitter      [Guild::GIL_HUMAN];
      cGuildVal->blood_texture      [i]=cGuildVal->blood_texture      [Guild::GIL_HUMAN];
      cGuildVal->turn_speed         [i]=cGuildVal->turn_speed         [Guild::GIL_HUMAN];
      }
    }

  if(DirectMemory::isRequired(vm))
    dma.reset(new DirectMemory(*this, vm));
  }

void GameScript::initSettings() {
  auto lang = Gothic::inst().settingsGetI("GAME", "language");
  if(vmLang!=lang) {
    vmLang = lang;
    //vm     = createVm(Gothic::inst());
    initDialogs();
    }
  }

void GameScript::initDialogs() {
  loadDialogOU();

  dialogsInfo.clear();
  vm.enumerate_instances_by_class_name("C_INFO", [this](zenkit::DaedalusSymbol& sym){
    dialogsInfo.push_back(vm.init_instance<zenkit::IInfo>(&sym));
    });
  }

void GameScript::loadDialogOU() {
  // Based on https://github.com/auronen/Gothic-2-localization mod,
  // at least in G2 .BIN should have a priority
  const std::string prefix = std::string(Gothic::inst().defaultOutputUnits());
  const std::array<std::string,2> names = {prefix + ".BIN", prefix + ".DAT"};

  const auto version = Gothic::inst().version().game==1 ? zenkit::GameVersion::GOTHIC_1
                                                        : zenkit::GameVersion::GOTHIC_2;

  for(auto& OU:names) {
    if(Resources::hasFile(OU)) {
      std::unique_ptr<zenkit::Read> read;
      auto zen = Resources::openReader(OU, read);
      dialogs = std::move(*zen->read_object<zenkit::CutsceneLibrary>(version));
      return;
      }

    const size_t segment = OU.find_last_of("\\/");
    if(segment!=std::string::npos && Resources::hasFile(OU.substr(segment+1))) {
      std::unique_ptr<zenkit::Read> read;
      auto zen = Resources::openReader(OU.substr(segment+1), read);
      dialogs = std::move(*zen->read_object<zenkit::CutsceneLibrary>(version));
      return;
      }

    char16_t str16[256] = {};
    for(size_t i=0; OU[i] && i<255; ++i)
      str16[i] = char16_t(OU[i]);

    auto gcutscene = CommandLine::inst().cutscenePath();
    auto full      = FileUtil::caseInsensitiveSegment(gcutscene,str16,Dir::FT_File);
    if(!FileUtil::exists(std::u16string(full)) && vmLang>=0) {
      gcutscene = CommandLine::inst().cutscenePath(ScriptLang(vmLang));
      full      = FileUtil::caseInsensitiveSegment(gcutscene,str16,Dir::FT_File);
      }

    try {
      auto buf = zenkit::Read::from(full);
      if(buf==nullptr)
        continue;
      auto zen = zenkit::ReadArchive::from(buf.get());
      if(zen==nullptr)
        continue;
      // auto zen = Resources::openReader(full);
      dialogs = std::move(*zen->read_object<zenkit::CutsceneLibrary>(version));
      return;
      }
    catch(...){
      // loop to next possible path
      }
    }
  Log::e("none of Zen-files for OU could be loaded");
  }

void GameScript::initializeInstanceNpc(const std::shared_ptr<zenkit::INpc>& npc, size_t instance) {
  auto sym = vm.find_symbol_by_index(uint32_t(instance));

  if(sym == nullptr) {
    Tempest::Log::e("Cannot initialize NPC ", instance, ": Symbol not found.");
    return;
    }

  vm.init_instance(npc, sym);

  if(npc->daily_routine!=0) {
    ScopeVar self(*vm.global_self(), npc);
    auto* daily_routine = vm.find_symbol_by_index(uint32_t(npc->daily_routine));

    if(daily_routine != nullptr) {
      vm.call_function(daily_routine);
      }
    }
  }

void GameScript::initializeInstanceItem(const std::shared_ptr<zenkit::IItem>& item, size_t instance) {
  auto sym = vm.find_symbol_by_index(uint32_t(instance));

  if(sym == nullptr) {
    Tempest::Log::e("Cannot initialize item ", instance, ": Symbol not found.");
    return;
    }

  vm.init_instance(item, sym);
  }

void GameScript::saveQuests(Serialize &fout) {
  quests.save(fout);
  fout.write(uint32_t(dlgKnownInfos.size()));
  for(auto& i:dlgKnownInfos)
    fout.write(uint32_t(i.first),uint32_t(i.second));

  fout.write(gilAttitudes);
  }

void GameScript::loadQuests(Serialize& fin) {
  quests.load(fin);
  uint32_t sz=0;
  fin.read(sz);
  for(size_t i=0;i<sz;++i){
    uint32_t f=0,s=0;
    fin.read(f,s);
    dlgKnownInfos.insert(std::make_pair(f,s));
    }

  fin.read(gilAttitudes);
  }

void GameScript::saveVar(Serialize &fout) {
  auto& dat = vm.symbols();
  fout.write(uint32_t(dat.size()));
  for(unsigned i = 0; i < dat.size(); ++i){
    auto* sym = vm.find_symbol_by_index(i); // never returns nullptr
    saveSym(fout,*sym);
    }
  }

void GameScript::loadVar(Serialize &fin) {
  std::string name;
  uint32_t sz=0;
  fin.read(sz);
  for(size_t i=0;i<sz;++i){
    auto t = uint32_t(zenkit::DaedalusDataType::VOID);
    fin.read(t);
    switch(zenkit::DaedalusDataType(t)) {
      case zenkit::DaedalusDataType::INT:{
        fin.read(name);
        auto* s = findSymbol(name);

        uint32_t size;
        fin.read(size);

        int v = 0;
        for(unsigned j = 0; j < size; ++j) {
          fin.read(v);
          if (s != nullptr && !s->is_member() && !s->is_const()) {
            s->set_int(v, uint16_t(j));
            }
          }

        break;
        }
      case zenkit::DaedalusDataType::FLOAT:{
        fin.read(name);
        auto* s = findSymbol(name);

        uint32_t size;
        fin.read(size);

        float v = 0;
        for (unsigned j = 0; j < size; ++j) {
          fin.read(v);
          if (s != nullptr && !s->is_member() && !s->is_const()) {
            s->set_float(v, uint16_t(j));
            }
          }

        break;
        }
      case zenkit::DaedalusDataType::STRING:{
        fin.read(name);
        auto* s = findSymbol(name);

        uint32_t size;
        fin.read(size);

        std::string v;
        for (unsigned j = 0; j < size; ++j) {
          fin.read(v);
          if (s != nullptr && !s->is_member() && !s->is_const()) {
            s->set_string(v, uint16_t(j));
            }
          }

        break;
        }
      case zenkit::DaedalusDataType::INSTANCE:{
        uint8_t dataClass=0;
        fin.read(dataClass);
        if(dataClass>0){
          uint32_t id=0;
          fin.read(name,id);
          auto* s = findSymbol(name);
          if (s == nullptr)
            break;
          if(dataClass==1) {
            auto npc = world().npcById(id);
            s->set_instance(npc ? npc->handlePtr() : nullptr);
            }
          else if(dataClass==2) {
            auto itm = world().itmById(id);
            s->set_instance(itm != nullptr ? itm->handlePtr() : nullptr);
            }
          else if(dataClass==3) {
            uint32_t itmClass=0;
            fin.read(itmClass);
            if(auto npc = world().npcById(id)) {
              auto itm = npc->getItem(itmClass);
              s->set_instance(itm ? itm->handlePtr() : nullptr);
              }
            }
          }
        break;
        }
      default:
        break;
      }
    }
  }

void GameScript::savePerc(Serialize& fout) {
  fout.write(uint32_t(PERC_Count));
  for(size_t i=0; i<PERC_Count; ++i) {
    fout.write(perceptionRanges.range[i]);
    }
  }

void GameScript::loadPerc(Serialize& fin) {
  uint32_t count = 0;
  fin.read(count);
  if(count!=PERC_Count) {
    if(hasSymbolName("initPerceptions"))
      vm.call_function("initPerceptions");
    return;
    }
  for(size_t i=0; i<PERC_Count; ++i)
    fin.read(perceptionRanges.range[i]);
  }

void GameScript::resetVarPointers() {
  for(uint32_t i=0;i<vm.symbols().size();++i){
    auto* s = vm.find_symbol_by_index(i); // never returns nullptr
    if(s->is_instance_of<zenkit::INpc>() || s->is_instance_of<zenkit::IItem>()){
      s->set_instance(nullptr);
      }
    }
  }

const QuestLog& GameScript::questLog() const {
  return quests;
  }

int GameScript::chapter() const {
  if(auto* sym = vm.find_symbol_by_name("KAPITEL"))
    return sym->get_int();
  return 0;
  }

void GameScript::saveSym(Serialize &fout, zenkit::DaedalusSymbol& i) {
  auto& w = world();
  switch(i.type()) {
    case zenkit::DaedalusDataType::INT:
      if(i.count()>0 && !i.is_member() && !i.is_const()){
        fout.write(i.type(), i.name(), i.count());

        for (unsigned j = 0; j < i.count(); ++j)
          fout.write(i.get_int(uint16_t(j)));
        return;
        }
      break;
    case zenkit::DaedalusDataType::FLOAT:
      if(i.count()>0 && !i.is_member() && !i.is_const()){
        fout.write(i.type(), i.name(), i.count());

        for (unsigned j = 0; j < i.count(); ++j)
          fout.write(i.get_float(uint16_t(j)));
        return;
        }
      break;
    case zenkit::DaedalusDataType::STRING:
      if(i.count()>0 && !i.is_member() && !i.is_const()){
        fout.write(i.type(), i.name(), i.count());

        for (unsigned j = 0; j < i.count(); ++j)
          fout.write(i.get_string(uint16_t(j)));
        return;
        }
      break;
    case zenkit::DaedalusDataType::INSTANCE:
      fout.write(i.type());

      if(i.is_instance_of<zenkit::INpc>()){
        auto hnpc = reinterpret_cast<const zenkit::INpc*>(i.get_instance().get());
        auto npc  = reinterpret_cast<const Npc*>(hnpc==nullptr ? nullptr : hnpc->user_ptr);
        fout.write(uint8_t(1),i.name(),world().npcId(npc));
        }
      else if(i.is_instance_of<zenkit::IItem>()){
        auto     item = reinterpret_cast<const zenkit::IItem*>(i.get_instance().get());
        uint32_t id   = w.itmId(item);
        if(id!=uint32_t(-1) || item==nullptr) {
          fout.write(uint8_t(2),i.name(),id);
          } else {
          uint32_t idNpc = uint32_t(-1);
          for(uint32_t r=0; r<w.npcCount(); ++r) {
            auto& n = *w.npcById(r);
            if(n.itemCount(item->symbol_index())>0) {
              idNpc = r;
              fout.write(uint8_t(3),i.name(),idNpc,uint32_t(item->symbol_index()));
              break;
              }
            }
          if(idNpc==uint32_t(-1))
            fout.write(uint8_t(2),i.name(),uint32_t(-1));
          }
        }
      else if(i.is_instance_of<zenkit::IFocus>() ||
              i.is_instance_of<zenkit::IGuildValues>() ||
              i.is_instance_of<zenkit::IInfo>()) {
        fout.write(uint8_t(0));
        }
      else {
        fout.write(uint8_t(0));
        }
      return;
    default:
      break;
    }
  fout.write(uint32_t(zenkit::DaedalusDataType::VOID));
  }

void GameScript::fixNpcPosition(Npc& npc, float angle0, float distBias) {
  auto& dyn  = *world().physic();
  auto  pos0 = npc.position();

  if(!npc.hasCollision()) {
    auto ray = dyn.ray(pos0+Vec3(0,100,0), pos0+Vec3(0,-200,0));
    if(ray.hasCol && std::abs(pos0.y-ray.v.y) < 10.f) {
      npc.updateTransform();
      return;
      }
    }

  for(int r = 0; r<=800; r+=20) {
    for(int ang = 0; ang<360; ang+=30) {
      float a = float((float(ang)+angle0)*M_PI/180.0);
      float d = float(r)+distBias;
      auto  p = pos0+Vec3(std::cos(a)*d, 0, std::sin(a)*d);

      auto ray = dyn.ray(p+Vec3(0,100,0), p+Vec3(0,-1000,0));
      if(!ray.hasCol)
        continue;
      p.y = ray.v.y;
      npc.setPosition(p);
      if(!npc.hasCollision()) {
        npc.updateTransform();
        return;
        }
      if(d==0) {
        // no need to loop multiple angles, with R of zero
        break;
        }
      }
    }

  // npc.setPosition(pos0);
  }

void GameScript::eventPlayAni(Npc& npc, std::string_view ani) {
  if(dma!=nullptr)
    dma->eventPlayAni(npc,ani);
  }

const World &GameScript::world() const {
  return *owner.world();
  }

World &GameScript::world() {
  return *owner.world();
  }

zenkit::IFocus GameScript::findFocus(std::string_view name) {
  auto id = vm.find_symbol_by_name(name);
  if(id==nullptr)
    return {};
  try {
    return *vm.init_instance<zenkit::IFocus>(id);
    }
  catch(const zenkit::DaedalusScriptError&) {
    return {};
    }
  }

void GameScript::storeItem(Item *itm) {
  auto* s = vm.global_item();
  if(itm!=nullptr) {
    s->set_instance(itm->handlePtr());
    } else {
    s->set_instance(nullptr);
    }
  }

zenkit::DaedalusSymbol* GameScript::findSymbol(std::string_view s) {
  return vm.find_symbol_by_name(s);
  }

zenkit::DaedalusSymbol* GameScript::findSymbol(const size_t s) {
  return vm.find_symbol_by_index(uint32_t(s));
  }

size_t GameScript::findSymbolIndex(std::string_view name) {
  auto sym = vm.find_symbol_by_name(name);
  return sym == nullptr ? size_t(-1) : sym->index();
  }

size_t GameScript::symbolsCount() const {
  return vm.symbols().size();
  }

const AiState& GameScript::aiState(ScriptFn id) {
  auto it = aiStates.find(id.ptr);
  if(it!=aiStates.end())
    return it->second;
  auto ins = aiStates.emplace(id.ptr,AiState(*this,id.ptr));
  return ins.first->second;
  }

const zenkit::ISpell& GameScript::spellDesc(int32_t splId) {
  auto& tag = spellFxInstanceNames->get_string(uint16_t(splId));
  return spells->find(tag);
  }

const VisualFx* GameScript::spellVfx(int32_t splId) {
  auto& tag = spellFxInstanceNames->get_string(uint16_t(splId));
  string_frm name("spellFX_",tag);
  return Gothic::inst().loadVisualFx(name);
  }

std::vector<GameScript::DlgChoice> GameScript::dialogChoices(std::shared_ptr<zenkit::INpc> player,
                                                             std::shared_ptr<zenkit::INpc> hnpc,
                                                             const std::vector<uint32_t>& except,
                                                             bool includeImp) {
  ScopeVar self (*vm.global_self(),  hnpc);
  ScopeVar other(*vm.global_other(), player);
  std::vector<zenkit::IInfo*> hDialog;
  for(auto& info : dialogsInfo) {
    if(info->npc==static_cast<int>(hnpc->symbol_index())) {
      hDialog.push_back(info.get());
      }
    }

  std::vector<DlgChoice> choice;
  for(int important=includeImp ? 1 : 0;important>=0;--important){
    for(auto& i:hDialog) {
      const zenkit::IInfo& info = *i;
      if(info.important!=important)
        continue;
      bool npcKnowsInfo = doesNpcKnowInfo(*player,vm.find_symbol_by_instance(info)->index());
      if(npcKnowsInfo && !info.permanent)
        continue;

      if(info.important && info.permanent){
        bool skip=false;
        for(auto i:except)
          if(static_cast<int>(i)==info.information){
            skip=true;
            break;
            }
        if(skip)
          continue;
        }

      bool valid=true;
      if(info.condition) {
        auto* conditionSymbol = vm.find_symbol_by_index(uint32_t(info.condition));
        if (conditionSymbol != nullptr) {
          valid = vm.call_function<int>(conditionSymbol) != 0;
          }
        }
      if(!valid)
        continue;

      DlgChoice ch;
      ch.title    = info.description;
      ch.scriptFn = uint32_t(info.information);
      ch.handle   = i;
      ch.isTrade  = info.trade!=0;
      ch.sort     = info.nr;
      choice.emplace_back(std::move(ch));
      }
    if(!choice.empty()){
      sort(choice);
      return choice;
      }
    }
  sort(choice);
  return choice;
  }

std::vector<GameScript::DlgChoice> GameScript::updateDialog(const GameScript::DlgChoice &dlg, Npc& player,Npc& npc) {
  if(dlg.handle==nullptr)
    return {};
  const zenkit::IInfo&               info = *dlg.handle;
  std::vector<GameScript::DlgChoice> ret;

  ScopeVar self (*vm.global_self(), npc.handlePtr());
  ScopeVar other(*vm.global_other(), player.handlePtr());

  for(size_t i=0;i<info.choices.size();++i){
    auto& sub = info.choices[i];
    GameScript::DlgChoice ch;
    ch.title    = sub.text;
    ch.scriptFn = uint32_t(sub.function);
    ch.handle   = dlg.handle;
    ch.isTrade  = false;
    ch.isSubChoice = true;
    // only where the scripts have the variable of the answer (the original Gothic 1 and 2 do not)
    if(!ch.title.empty() && ch.title[0]=='?' && dma!=nullptr && vm.find_symbol_by_name("INFOMANAGERANSWER")!=nullptr) {
      // Chronicles of Myrtana: "?Name" is an answer that is typed (INFOMANAGERANSWER of its scripts)
      ch.isInput = true;
      ch.title.erase(0,1);
      }
    ch.sort     = int(i);
    ret.push_back(ch);
    }

  sort(ret);
  return ret;
  }

/// The text that the player typed for a choice of the dialog. CoM reads it from the variable INFOMANAGERANSWER
void GameScript::setDialogAnswer(std::string_view text) {
  if(auto* s = vm.find_symbol_by_name("INFOMANAGERANSWER"))
    s->set_string(std::string(text));
  }

void GameScript::exec(const GameScript::DlgChoice &dlg, Npc& player, Npc& npc) {
  ScopeVar self (*vm.global_self(), npc.handlePtr());
  ScopeVar other(*vm.global_other(), player.handlePtr());

  zenkit::IInfo& info = *dlg.handle;

  if(&player!=&npc)
    player.stopAnim("");
  auto& pl = player.handle();

  if(info.information==int(dlg.scriptFn)) {
    setNpcInfoKnown(pl,info);
    } else {
    for(size_t i=0;i<info.choices.size();){
      if(info.choices[i].function==int(dlg.scriptFn))
        info.choices.erase(info.choices.begin()+int(i)); else
        ++i;
      }
    }

  vm.call_function(vm.find_symbol_by_index(dlg.scriptFn));
  }

void GameScript::printCannotUseError(Npc& npc, int32_t atr, int32_t nValue) {
  auto id = vm.find_symbol_by_name("G_CanNotUse");
  if(id==nullptr)
    return;

  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id, npc.isPlayer(), atr, nValue);
  }

void GameScript::printCannotCastError(Npc &npc, int32_t plM, int32_t itM) {
  auto id = vm.find_symbol_by_name("G_CanNotCast");
  if(id==nullptr)
    return;

  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id, npc.isPlayer(), itM, plM);
  }

void GameScript::printCannotBuyError(Npc &npc) {
  auto id = vm.find_symbol_by_name("player_trade_not_enough_gold");
  if(id==nullptr)
    return;
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingItem(Npc &npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_item");
  if(id==nullptr) {
    if(owner.version().game==1)
      owner.player()->playAnimByName("T_DONTKNOW", BS_NONE);
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingKey(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_key");
  if(id==nullptr) {
    if(owner.version().game==1)
      owner.player()->playAnimByName("T_DONTKNOW", BS_NONE);
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobAnotherIsUsing(Npc &npc) {
  auto id = vm.find_symbol_by_name("player_mob_another_is_using");
  if(id==nullptr) {
    if(owner.version().game==1)
      owner.player()->playAnimByName("T_DONTKNOW", BS_NONE);
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingKeyOrLockpick(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_key_or_lockpick");
  if(id==nullptr) {
    if(owner.version().game==1)
      owner.player()->playAnimByName("T_DONTKNOW", BS_NONE);
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobMissingLockpick(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_missing_lockpick");
  if(id==nullptr) {
    if(owner.version().game==1)
      owner.player()->playAnimByName("T_DONTKNOW", BS_NONE);
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::printMobTooFar(Npc& npc) {
  auto id = vm.find_symbol_by_name("player_mob_too_far_away");
  if(id==nullptr) {
    owner.player()->playAnimByName("T_DONTKNOW", BS_NONE);
    return;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::invokeState(const std::shared_ptr<zenkit::INpc>& hnpc, const std::shared_ptr<zenkit::INpc>& oth, const char *name) {
  auto id = vm.find_symbol_by_name(name);
  if(id==nullptr)
    return;

  ScopeVar self (*vm.global_self(),  hnpc);
  ScopeVar other(*vm.global_other(), oth);
  vm.call_function<void>(id);
  }

int GameScript::invokeState(Npc* npc, Npc* oth, Npc* vic, ScriptFn fn) {
  if(!fn.isValid())
    return 0;
  if(oth==nullptr){
    // oth=npc; //FIXME: PC_Levelinspektor?
    }
  if(vic==nullptr){
    // vic=owner.player();
    }

  if(fn==ZS_Talk){
    if(oth==nullptr || !oth->isPlayer()) {
      Log::e("unxepected perc acton");
      return 0;
      }
    }

  ScopeCtx ctx   (*this, npc!=nullptr ? npc->processPolicy() : NpcProcessPolicy::AiNormal);
  ScopeVar self  (*vm.global_self(),   npc != nullptr ? npc->handlePtr() : nullptr);
  ScopeVar other (*vm.global_other(),  oth != nullptr ? oth->handlePtr() : nullptr);
  ScopeVar victim(*vm.global_victim(), vic != nullptr ? vic->handlePtr() : nullptr);

  auto* sym = vm.find_symbol_by_index(uint32_t(fn.ptr));
  int   ret = 0;
  if(sym!=nullptr && sym->rtype() == zenkit::DaedalusDataType::INT) {
    ret = vm.call_function<int>(sym);
    }
  else if(sym!=nullptr) {
    vm.call_function<void>(sym);
    ret = 0;
    }

  if(vm.global_other()->is_instance_of<zenkit::INpc>()){
    auto oth2 = reinterpret_cast<zenkit::INpc*>(vm.global_other()->get_instance().get());
    if(oth!=nullptr && oth2!=&oth->handle()) {
      Npc* other = findNpc(oth2);
      npc->setOther(other);
      }
    }
  return ret;
  }

/// The function of an item (on_state, on_equip, on_unequip) runs with self = the character and item = the item, as in the
/// original engine: CoM reads ITEM.HP in the functions that read the recipes.
void GameScript::invokeItem(Npc *npc, ScriptFn fn, Item* itm) {
  if(fn==size_t(-1) || fn == 0)
    return;
  auto functionSymbol = vm.find_symbol_by_index(uint32_t(fn.ptr));

  if (functionSymbol == nullptr)
    return;

  ScopeVar self(*vm.global_self(), npc->handlePtr());
  if(itm!=nullptr) {
    ScopeVar item(*vm.global_item(), itm->handlePtr());
    vm.call_function<void>(functionSymbol);
    return;
    }
  vm.call_function<void>(functionSymbol);
  }

int GameScript::invokeMana(Npc &npc, Npc* target, int mana) {
  auto fn = vm.find_symbol_by_name("Spell_ProcessMana");
  if(fn==nullptr)
    return SpellCode::SPL_SENDSTOP;

  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), target != nullptr ? target->handlePtr() : nullptr);

  return vm.call_function<int>(fn,mana);
  }

int GameScript::invokeManaRelease(Npc &npc, Npc* target, int mana) {
  auto fn = vm.find_symbol_by_name("Spell_ProcessMana_Release");
  if(fn==nullptr)
    return SpellCode::SPL_SENDSTOP;

  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), target != nullptr ? target->handlePtr() : nullptr);

  return vm.call_function<int>(fn,mana);
  }

void GameScript::invokeSpell(Npc &npc, Npc* target, Item &it) {
  auto&      tag = spellFxInstanceNames->get_string(uint16_t(it.spellId()));
  string_frm name("Spell_Cast_",tag);
  auto       fn = vm.find_symbol_by_name(name);
  if(fn==nullptr)
    return;

  const int32_t splLevel = npc.activeSpellLevel();
  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), target != nullptr ? target->handlePtr() : nullptr);
  try {
    if(fn->has_return() || fn->count()>1) {
      Log::d("unsupported spell-script signature: ",name);
      return;
      }
    if(fn->count()==1) {
      // this is a leveled spell
      const auto params = vm.find_parameters_for_function(fn);
      if(params.size()!=1 || params[0].type()!=zenkit::DaedalusDataType::INT) {
        Log::d("unsupported spell-level parameter: ",name);
        return;
        }
      vm.push_int(splLevel);
      }
    // Native spell adapters (CoM Telekinesis) must be honored here too.
    vm.invoke_function(fn);
    }
  catch(...) {
    Log::d("unable to call spell-script: \"",name,"\'");
    }
  }

int GameScript::debugInfoCondition(Npc& npc, Npc& player, std::string_view infoName, bool& known, std::string& detail) {
  string_frm cond(infoName,"_CONDITION");
  auto* fn   = vm.find_symbol_by_name(cond);
  auto* info = vm.find_symbol_by_name(infoName);
  if(fn==nullptr || info==nullptr)
    return -1;
  known = doesNpcKnowInfo(npc.handle(),info->index()) || doesNpcKnowInfo(player.handle(),info->index());

  // the variables that the condition reads and the infos that it asks about: what makes it false
  {
  uint32_t end = fn->address();
  for(uint32_t pc=fn->address(), n=0; n<400; ++n) {
    auto ins = vm.instruction_at(pc);
    if(ins.op==zenkit::DaedalusOpcode::B || ins.op==zenkit::DaedalusOpcode::BZ)
      end = std::max(end,uint32_t(ins.address));
    if(ins.op==zenkit::DaedalusOpcode::RSR && pc>=end)
      break;
    if(ins.op==zenkit::DaedalusOpcode::PUSHV || ins.op==zenkit::DaedalusOpcode::PUSHVI) {
      if(auto* s = vm.find_symbol_by_index(ins.symbol)) {
        if(s->type()==zenkit::DaedalusDataType::INT && !s->is_member() && !s->is_const() && s->count()==1)
          detail += std::string(s->name()) + "=" + std::to_string(s->get_int()) + " ";
        }
      }
    if(ins.op==zenkit::DaedalusOpcode::PUSHI && ins.immediate>10000 && ins.immediate<200000) {
      if(auto* s = vm.find_symbol_by_index(uint32_t(ins.immediate)))
        if(s->type()==zenkit::DaedalusDataType::INSTANCE && !s->is_member() && s->name().find("DIA_")==0)
          detail += "[asks " + std::string(s->name()) + (doesNpcKnowInfo(player.handle(),s->index()) ? ": known] " : ": NOT known] ");
      }
    pc += ins.size;
    }
  }
  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), player.handlePtr());
  return vm.call_function<int>(fn);
  }

int GameScript::invokeCond(Npc& npc, std::string_view func) {
  auto fn = vm.find_symbol_by_name(func);
  if(fn==nullptr) {
    Gothic::inst().onPrint("MOBSI::conditionFunc is not invalid");
    return 1;
    }
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  return vm.call_function<int>(fn);
  }

void GameScript::invokePickLock(Npc& npc, int bSuccess, int bBrokenOpen) {
  auto fn   = vm.find_symbol_by_name("G_PickLock");
  if(fn==nullptr)
    return;
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(fn, bSuccess, bBrokenOpen);
  }

void GameScript::invokeRefreshAtInsert(Npc& npc) {
  if(B_RefreshAtInsert==nullptr || owner.version().game!=2)
    return;
  ScopeVar self(*vm.global_self(), npc.handlePtr());
  vm.call_function<void>(B_RefreshAtInsert);
  }

CollideMask GameScript::canNpcCollideWithSpell(Npc& npc, Npc* shooter, int32_t spellId) {
  if(owner.version().game==1) {
    auto& spl = spellDesc(spellId);
    if(npc.isTargetableBySpell(TargetType(spl.target_collect_type)))
      return COLL_DOEVERYTHING; else
      return COLL_DONOTHING;
    }

  auto fn   = vm.find_symbol_by_name("C_CanNpcCollideWithSpell");
  if(fn==nullptr)
    return COLL_DOEVERYTHING;

  ScopeVar self (*vm.global_self(),  npc.handlePtr());
  ScopeVar other(*vm.global_other(), shooter->handlePtr());
  return CollideMask(vm.call_function<int>(fn, spellId));
  }

// Gothic 1 only differentiates between the two worldmap types with and
// without the orc addition and does this inside the code, not the script
int GameScript::playerHotKeyScreenMap_G1(Npc& pl) {
  size_t map = findSymbolIndex("itwrworldmap_orc");
  if(map==size_t(-1) || pl.itemCount(map)<1)
    map = findSymbolIndex("itwrworldmap");

  if(map==size_t(-1) || pl.itemCount(map)<1)
    return -1;

  pl.useItem(map);

  return int(map);
  }

int GameScript::playerHotKeyScreenMap(Npc& pl) {
  auto fn   = vm.find_symbol_by_name("player_hotkey_screen_map");
  if(fn==nullptr) {
    if(owner.version().game==1)
      return playerHotKeyScreenMap_G1(pl);
    return -1;
    }

  ScopeVar self(*vm.global_self(), pl.handlePtr());
  int map = vm.call_function<int>(fn);
  if(map>=0)
    pl.useItem(size_t(map));
  return map;
  }

void GameScript::playerHotLamePotion(Npc& pl) {
  auto opt = Gothic::inst().settingsGetI("GAME", "usePotionKeys");
  if(opt==0)
    return;

  auto fn   = vm.find_symbol_by_name("player_hotkey_lame_potion");
  if(fn==nullptr)
    return;

  ScopeVar self(*vm.global_self(), pl.handlePtr());
  vm.call_function<void>(fn);
  }

void GameScript::playerHotLameHeal(Npc& pl) {
  auto opt = Gothic::inst().settingsGetI("GAME", "usePotionKeys");
  if(opt==0)
    return;

  auto fn   = vm.find_symbol_by_name("player_hotkey_lame_heal");
  if(fn==nullptr)
    return;

  ScopeVar self(*vm.global_self(), pl.handlePtr());
  vm.call_function<void>(fn);
  }

std::string_view GameScript::spellCastAnim(Npc&, Item &it) {
  if(spellFxAniLetters==nullptr)
    return "FIB";
  return spellFxAniLetters->get_string(uint16_t(it.spellId()));
  }

bool GameScript::aiOutput(Npc &npc, std::string_view outputname, bool overlay) {
  string_frm name(outputname,".WAV");
  uint64_t   dt = 0;

  world().addDlgSound(name, npc.mapHeadBone(), WorldSound::talkRange, dt);
  npc.setAiOutputBarrier(messageTime(outputname),overlay);

  return true;
  }

bool GameScript::aiOutputSvm(Npc &npc, std::string_view outputname, bool overlay) {
  if(overlay) {
    if(tickCount()<svmBarrier)
      return true;
    svmBarrier = tickCount()+messageTime(outputname);
    }

  if(!outputname.empty())
    return aiOutput(npc,outputname,overlay);
  return true;
  }

bool GameScript::isDead(const Npc &pl) {
  return pl.isInState(ZS_Dead);
  }

bool GameScript::isUnconscious(const Npc &pl) {
  return pl.isInState(ZS_Unconscious);
  }

bool GameScript::isTalk(const Npc &pl) {
  return pl.isInState(ZS_Talk);
  }

bool GameScript::isAttack(const Npc& pl) const {
  return pl.isInState(ZS_Attack) || pl.isInState(ZS_MM_Attack);
  }

std::string_view GameScript::messageFromSvm(std::string_view id, int voice) const {
  return svm->find(id,voice);
  }

std::string_view GameScript::messageByName(std::string_view id) const {
  auto blk = dialogs.block_by_name(id);
  if(blk == nullptr)
    return "";
  auto msg = blk->get_message();
  if(msg == nullptr)
    return "";
  return msg->text;
  }

uint32_t GameScript::messageTime(std::string_view id) const {
  static std::string tmp;
  tmp.assign(id);

  uint32_t& time = msgTimings[tmp];
  if(time>0)
    return time;

  string_frm name(id,".WAV");
  auto s = Resources::loadSoundBuffer(name);
  if(s.timeLength()>0) {
    time = uint32_t(s.timeLength());
    } else {
    auto txt = messageByName(id);
    time = uint32_t(float(txt.length())*viewTimePerChar);
    time = std::min(time, 16000u);
    }
  return time;
  }

void GameScript::printNothingToGet() {
  auto id = vm.find_symbol_by_name("player_plunder_is_empty");
  if(id==nullptr) {
    if(owner.version().game==1)
      owner.player()->playAnimByName("T_DONTKNOW", BS_NONE);
    return;
    }
  ScopeVar self(*vm.global_self(), owner.player()->handlePtr());
  vm.call_function<void>(id);
  }

void GameScript::useInteractive(const std::shared_ptr<zenkit::INpc>& hnpc, std::string_view func) {
  auto fn = vm.find_symbol_by_name(func);
  if(fn == nullptr)
    return;

  ScopeVar self(*vm.global_self(),hnpc);
  try {
    vm.call_function<void>(fn);
    }
  catch (...) {
    Log::i("unable to use interactive [",func,"]");
    }
  }

Attitude GameScript::guildAttitude(const Npc &p0, const Npc &p1) const {
  auto selfG = std::min<size_t>(gilCount-1,p0.guild());
  auto npcG  = std::min<size_t>(gilCount-1,p1.guild());
  auto ret   = gilAttitudes[selfG*gilCount+npcG];
  return Attitude(ret);
  }

Attitude GameScript::personAttitude(const Npc &p0, const Npc &p1) const {
  if(!p0.isPlayer() && !p1.isPlayer())
    return guildAttitude(p0,p1);

  Attitude att=ATT_NULL;
  const Npc& npc = p0.isPlayer() ? p1 : p0;
  // CoM uses temporary hostility/friendliness for quest fights and restores
  // it afterwards. The saved temporary value must take precedence over the
  // permanent relationship, just as oCNpc::GetAttitude does.
  // New Balance: B_SetAttributesToChapter gives EVERY human the personal attitude ATT_NEUTRAL to the hero
  // (bandits too). In NB the bandits still attack a hero who has no armor of the bandits, so the hostility
  // of the guilds (Wld_SetGuildAttitude: GIL_BDT -> GIL_NONE = ATT_HOSTILE) wins over the neutral personal
  // attitude. FRIENDLY / ANGRY / HOSTILE set by the scripts stay as they are.
  const bool nb = nbStates.find(this)!=nbStates.end();
  att = npc.tempAttitude();
  if(att==ATT_NULL)
    att = npc.attitude();
  if(att!=ATT_NULL) {
    if(nb && att==ATT_NEUTRAL && guildAttitude(p0,p1)==ATT_HOSTILE)
      return ATT_HOSTILE;
    return att;
    }
  att = guildAttitude(p0,p1);
  return att;
  }

bool GameScript::isFriendlyFire(const Npc& src, const Npc& dst) const {
  const int AIV_PARTYMEMBER = (owner.version().game==2) ? 15 : 36;
  if(src.isPlayer())
    return false;
  if(personAttitude(src, dst)==ATT_FRIENDLY)
    return true;
  if(src.handlePtr()->aivar[AIV_PARTYMEMBER]!=0 && dst.isPlayer())
    return true;
  return false;
  }

BodyState GameScript::schemeToBodystate(std::string_view sc) {
  if(searchScheme(sc,"MOB_SIT"))
    return BS_SIT;
  if(searchScheme(sc,"MOB_LIE"))
    return BS_LIE;
  if(searchScheme(sc,"MOB_CLIMB"))
    return BS_CLIMB;
  if(searchScheme(sc,"MOB_NOTINTERRUPTABLE"))
    return BS_MOBINTERACT;
  return BS_MOBINTERACT_INTERRUPT;
  }

void GameScript::onWldItemRemoved(const Item& itm) {
  onWldInstanceRemoved(&itm.handle());
  }

void GameScript::onWldInstanceRemoved(const zenkit::DaedalusInstance* obj) {
  if(obj==nullptr)
    return;
  auto* symbol = vm.find_symbol_by_instance(*obj);
  // Several loose items may share one script instance. Removing an older
  // object must not clear the symbol already rebound to a newer one (CoM's
  // scripted pickups/spawns); transient instances need no symbol update.
  if(symbol!=nullptr && symbol->get_instance().get()==obj)
    symbol->set_instance(nullptr);
  }

void GameScript::makeCurrent(Item* w) {
  if(w==nullptr)
    return;
  auto* s = vm.find_symbol_by_index(uint32_t(w->clsId()));
  if(s != nullptr)
    s->set_instance(w->handlePtr());
  }

bool GameScript::searchScheme(std::string_view sc, std::string_view listName) {
  std::string_view list = findSymbol(listName)->get_string();
  for(size_t i=0; i<=list.size(); ++i) {
    if(i==list.size() || list[i]==',') {
      if(sc==list.substr(0,i))
        return true;
      if(i==list.size())
         break;
      list = list.substr(i+1);
      i    = 0;
      }
    }
  return false;
  }

zenkit::DaedalusVm GameScript::createVm(Gothic& gothic) {
  auto lang   = gothic.settingsGetI("GAME", "language");
  auto script = gothic.loadScript(gothic.defaultGameDatFile(), ScriptLang(lang));
  auto exef   = zenkit::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS;
    if(DirectMemory::isRequired(script) ||
     (script.find_symbol_by_name("RX_ISSPACETNET") != nullptr &&
      script.find_symbol_by_name("_BCANSAVE") != nullptr)) {
    exef |= zenkit::DaedalusVmExecutionFlag::IGNORE_CONST_SPECIFIER;
    }
  return zenkit::DaedalusVm(std::move(script), exef);
  }

bool GameScript::hasSymbolName(std::string_view name) {
  return vm.find_symbol_by_name(name)!=nullptr;
  }

uint64_t GameScript::tickCount() const {
  return owner.tickCount();
  }

void GameScript::tick(uint64_t dt) {
  if(dma!=nullptr)
    dma->tick(dt);

  auto nb = nbStates.find(this);
  if(nb==nbStates.end() || world().player()==nullptr)
    return;

  // the delayed functions of NB (RX_CALLBACK*)
  if(!nb->second.callbacks.empty()) {
    const uint64_t now     = tickCount();
    const bool     inDialog = owner.isInDialog();
    auto&          cbs     = nb->second.callbacks;

    std::vector<NbCallback> due;
    for(size_t i=0; i<cbs.size(); ) {
      if(cbs[i].at<=now && !(cbs[i].waitDialog && inDialog)) {
        due.push_back(std::move(cbs[i]));
        cbs.erase(cbs.begin()+ptrdiff_t(i));
        } else {
        ++i;
        }
      }

    for(auto& c:due) {
      auto* fn = vm.find_symbol_by_index(c.func);
      if(fn==nullptr || fn->type()!=zenkit::DaedalusDataType::FUNCTION)
        continue;
      if(c.npc!=nullptr && findNpcRaw(c.npc)==nullptr)
        continue; // the character is gone
      auto prevSelf  = vm.global_self() ->get_instance();
      auto prevOther = vm.global_other()->get_instance();
      auto prevItem  = vm.global_item() ->get_instance();
      if(c.npc!=nullptr)
        vm.global_self()->set_instance(c.npc);
      try {
        if(fn->has_return())
          (void)vm.call_function<int>(fn); else
          vm.call_function<void>(fn);
        }
      catch(const std::exception& e) {
        Log::e("NB: callback ",fn->name()," failed: ",e.what());
        }
      vm.global_self() ->set_instance(prevSelf);
      vm.global_other()->set_instance(prevOther);
      vm.global_item() ->set_instance(prevItem);
      }
    }

  auto& st = nb->second;
  const auto callNb = [this](zenkit::DaedalusSymbol* fn) {
    if(fn==nullptr)
      return;
    try {
      if(fn->has_return())
        (void)vm.call_function<int>(fn); else
        vm.call_function<void>(fn);
      }
    catch(const std::exception& e) {
      Log::e("NB: ",fn->name()," failed: ",e.what());
      }
    };

  // PerFrame: every frame, RX_FrameTime is the time of the frame in ms (the plugin writes it)
  if(st.perFrame!=nullptr) {
    if(st.frameTime!=nullptr)
      st.frameTime->set_int(int32_t(dt));
    callNb(st.perFrame);
    }

  // B_CheckMenuOption: the plugin calls it after the menu; here once in 5 s (it only reads the options)
  if(st.checkMenu!=nullptr && tickCount()>=st.nextMenuCheck) {
    st.nextMenuCheck = tickCount()+5000;
    callNb(st.checkMenu);
    }

  if(tickCount()>=st.nextMoveset) {
    st.nextMoveset = tickCount()+100;
    auto* sepSym = vm.find_symbol_by_name("GIL_SEPERATOR_HUM");
    NbMoveset::tick(*this,world(),(sepSym!=nullptr ? sepSym->get_int() : 21));

    // NB fix 43: the level of the animations of the weapons of the hero. RX_LastAnim_1h/2h is the level that the
    // player chose in the quick panel of the plugin (RX_QuickPanel_ChangeAnim: 1 fighter, 2 ... 3 master), -1 is
    // "by the talent". There is no quick panel here: a 0 (amateur) stayed from the start of the game and every
    // weapon took the animations of an amateur, also of a master. With the talent learned it goes back to -1 and
    // B_CheckMenuOption (DoCheckFightSkill1h/2h) puts on the animations of the level of the talent.
    if(auto* pl = world().player()) {
      bool changed = false;
      const auto autoLevel = [&](std::string_view lastAnim, std::string_view check, int talent) {
        auto* v = vm.find_symbol_by_name(lastAnim);
        auto* c = vm.find_symbol_by_name(check);
        if(v==nullptr || c==nullptr || v->is_member() || c->is_member() ||
           v->type()!=zenkit::DaedalusDataType::INT || c->type()!=zenkit::DaedalusDataType::INT)
          return;
        if(v->get_int()==0 && pl->handle().hitchance[talent]>=30) {
          v->set_int(-1);
          c->set_int(1);
          changed = true;
          }
        };
      autoLevel("RX_LASTANIM_1H","DOCHECKFIGHTSKILL1H",TALENT_1H);
      autoLevel("RX_LASTANIM_2H","DOCHECKFIGHTSKILL2H",TALENT_2H);
      if(changed)
        st.nextMenuCheck = 0;   // B_CheckMenuOption at the next frame
      }
    }

  // the timer of 1000 ms of the plugin: RX_Main_CycleTrigger, the next second b_gettime
  // (b_gettime: the world events, e.g. the quest of the rune stone near the tower of Xardas)
  const uint64_t currentSecond = tickCount()/1000;
  if(currentSecond==st.lastCycleSecond)
    return;
  st.lastCycleSecond = currentSecond;
  ++st.unionCycle;
  if(st.unionCycle==1) {
    callNb(st.cycle);
    }
  if(st.unionCycle>=2) {
    st.unionCycle = 0;
    callNb(st.getTime);
    }
  }

uint32_t GameScript::rand(uint32_t max) {
  return uint32_t(randGen())%max;
  }

Npc* GameScript::findNpc(zenkit::DaedalusSymbol* s) {
  if(s!=nullptr && s->is_instance_of<zenkit::INpc>()) {
    auto cNpc = reinterpret_cast<zenkit::INpc*>(s->get_instance().get());
    return findNpc(cNpc);
    }
  return nullptr;
  }

Npc* GameScript::findNpc(zenkit::INpc *handle) {
  if(handle==nullptr)
    return nullptr;
  return reinterpret_cast<Npc*>(handle->user_ptr);
  }

Npc* GameScript::findNpc(const std::shared_ptr<zenkit::INpc>& handle) {
  if(handle==nullptr)
    return nullptr;
  auto* npc = reinterpret_cast<Npc*>(handle->user_ptr);
  if(dma!=nullptr)
    npc = dma->triaRedirect(npc);   // a trialog: the self of the scripts is who speaks
  return npc;
  }

Npc* GameScript::findNpcRaw(const std::shared_ptr<zenkit::INpc>& handle) {
  if(handle==nullptr)
    return nullptr;
  return reinterpret_cast<Npc*>(handle->user_ptr);
  }

Npc* GameScript::findNpcById(const std::shared_ptr<zenkit::DaedalusInstance>& handle) {
  if(handle==nullptr)
    return nullptr;
  if(auto npc = dynamic_cast<const zenkit::INpc*>(handle.get())) {
    return reinterpret_cast<Npc*>(npc->user_ptr);
    }
  return findNpcById(handle->symbol_index());
  }

Item *GameScript::findItem(zenkit::IItem* handle) {
  if(handle==nullptr)
    return nullptr;
  auto& itData = *handle;
  assert(itData.user_ptr); // engine bug, if null
  return reinterpret_cast<Item*>(itData.user_ptr);
  }

Item *GameScript::findItemById(size_t id) {
  auto* handle = vm.find_symbol_by_index(uint32_t(id));
  if(handle==nullptr||!handle->is_instance_of<zenkit::IItem>())
    return nullptr;
  auto hitm = reinterpret_cast<zenkit::IItem*>(handle->get_instance().get());
  return findItem(hitm);
  }

Item* GameScript::findItemById(const std::shared_ptr<zenkit::DaedalusInstance>& handle) {
  if(handle==nullptr)
    return nullptr;
  if(auto itm = dynamic_cast<const zenkit::IItem*>(handle.get())) {
    assert(itm->user_ptr); // engine bug, if null
    return reinterpret_cast<Item*>(itm->user_ptr);
    }
  return findItemById(handle->symbol_index());
  }

Npc* GameScript::findNpcById(size_t id) {
  auto* handle = vm.find_symbol_by_index(uint32_t(id));
  if(handle==nullptr || !handle->is_instance_of<zenkit::INpc>())
    return nullptr;

  auto hnpc = reinterpret_cast<zenkit::INpc*>(handle->get_instance().get());
  if(hnpc==nullptr) {
    auto obj = world().findNpcByInstance(id);
    handle->set_instance(obj ? obj->handlePtr() : nullptr);
    hnpc = reinterpret_cast<zenkit::INpc*>(handle->get_instance().get());
    }
  return findNpc(hnpc);
  }

zenkit::IInfo* GameScript::findInfo(size_t id) {
  auto* sym = vm.find_symbol_by_index(uint32_t(id));
  if(sym==nullptr||!sym->is_instance_of<zenkit::IInfo>())
    return nullptr;
  auto* h = sym->get_instance().get();
  if(h==nullptr)
    Log::e("invalid c_info object: \"",sym->name(),"\"");
  return reinterpret_cast<zenkit::IInfo*>(h);
  }

void GameScript::removeItem(Item &it) {
  world().removeItem(it);
  }

void GameScript::setInstanceNPC(std::string_view name, Npc &npc) {
  auto sym = vm.find_symbol_by_name(name);
  if(sym == nullptr) {
    Tempest::Log::e("Cannot set NPC instance ", name, ": Symbol not found.");
    return;
    }
  sym->set_instance(npc.handlePtr());
  }

void GameScript::setInstanceItem(Npc &holder, size_t itemId) {
  storeItem(holder.getItem(itemId));
  }

AiOuputPipe *GameScript::openAiOuput() {
  return aiDefaultPipe.get();
  }

AiOuputPipe *GameScript::openDlgOuput(Npc &player, Npc &npc) {
  if(player.isPlayer())
    return owner.openDlgOuput(player,npc);
  return owner.openDlgOuput(npc,player);
  }

ScriptFn GameScript::playerPercAssessMagic() {
  auto id = vm.find_symbol_by_name("PLAYER_PERC_ASSESSMAGIC");
  if(id==nullptr)
    return ScriptFn();

  if(id->count()>0)
    return ScriptFn(uint32_t(id->get_int()));
  return ScriptFn();
  }

int GameScript::npcDamDiveTime() {
  auto id = vm.find_symbol_by_name("NPC_DAM_DIVE_TIME");
  if(id==nullptr)
    return 0;
  return id->get_int();
  }

int32_t GameScript::criticalDamageMultiplyer() const {
  return damCriticalMultiplier;
  }

uint32_t GameScript::lockPickId() const {
  return ItKE_lockpick!=nullptr ? ItKE_lockpick->index() : 0;
  }

std::string_view GameScript::menuMain() const {
  if(dma!=nullptr)
    return dma->menuMain();
  return ::menuMain;
  }

bool GameScript::game_initgerman() {
  return true;
  }

bool GameScript::game_initenglish() {
  return true;
  }

void GameScript::wld_settime(int hour, int minute) {
  world().setDayTime(hour,minute);
  }

int GameScript::wld_getday() {
  return int(owner.time().day());
  }

void GameScript::wld_playeffect(std::string_view visual, std::shared_ptr<zenkit::DaedalusInstance> sourceId, std::shared_ptr<zenkit::DaedalusInstance> targetId,
                                int effectLevel, int damage, int damageType, int isProjectile) {
  if(aiProcessPolicy>=NpcProcessPolicy::AiFar2)
    return;

  if(isProjectile!=0 || damageType!=0 || damage!=0 || effectLevel!=0) {
    // TODO
    Log::i("effect not implemented [",visual.data(),"]");
    return;
    }
  const VisualFx* vfx = Gothic::inst().loadVisualFx(visual);
  if(vfx==nullptr) {
    Log::i("invalid effect [",visual.data(),"]");
    return;
    }

  auto dstNpc = findNpcById(targetId);
  auto srcNpc = findNpcById(sourceId);

  auto dstItm = findItemById(targetId);
  auto srcItm = findItemById(sourceId);

  if(srcNpc!=nullptr && dstNpc!=nullptr) {
    srcNpc->startEffect(*dstNpc,*vfx);
    } else
  if(srcItm!=nullptr && dstItm!=nullptr){
    Effect e(*vfx,world(),srcItm->position());
    e.setActive(true);
    world().runEffect(std::move(e));
    }
  }

void GameScript::wld_stopeffect(std::string_view visual) {
  const VisualFx*          vfx    = Gothic::inst().loadVisualFx(visual);
  if(vfx==nullptr) {
    Log::i("invalid effect [",visual.data(),"]");
    return;
    }
  if(auto w = owner.world())
    w->stopEffect(*vfx);
  }

int GameScript::wld_getplayerportalguild() {
  int32_t g = GIL_NONE;
  if(auto p = world().player())
    g = world().guildOfRoom(p->portalName());
  return g;
  }

int GameScript::wld_getformerplayerportalguild() {
  int32_t g = GIL_NONE;
  if(auto p = world().player())
    g = world().guildOfRoom(p->formerPortalName());
  return g;
  }

void GameScript::wld_setguildattitude(int gil1, int att, int gil2) {
  if(gil1<0 || gil2<0 || gil1>=int(gilCount) || gil2>=int(gilCount))
    return;
  gilAttitudes[size_t(gil1)*gilCount+size_t(gil2)] = att;
  }

int GameScript::wld_getguildattitude(int gil1, int gil2) {
  if(gil1<0 || gil2<0 || gil1>=int(gilCount) || gil2>=int(gilCount))
    return ATT_HOSTILE; // error
  return gilAttitudes[size_t(gil1)*gilCount+size_t(gil2)];
  }

void GameScript::wld_exchangeguildattitudes(std::string_view name) {
  auto guilds = vm.find_symbol_by_name(name);
  if(guilds==nullptr)
    return;
  for(size_t i=0;i<gilTblSize;++i) {
    for(size_t r=0;r<gilTblSize;++r)
      gilAttitudes[i*gilCount+r] = guilds->get_int(uint16_t(i * gilTblSize + r));
    }
  }

bool GameScript::wld_istime(int hour0, int min0, int hour1, int min1) {
  gtime begin{hour0,min0}, end{hour1,min1};
  gtime now = owner.time();
  now = gtime(0,now.hour(),now.minute());

  if(begin<=end && begin<=now && now<end)
    return true;
  else if(end<begin && (now<end || begin<=now))
    return true;
  else
    return 0;
  }

bool GameScript::wld_isfpavailable(std::shared_ptr<zenkit::INpc> self, std::string_view name) {
  if(self==nullptr){
    return false;
    }

  auto wp = world().findFreePoint(*findNpc(self.get()),name);
  return wp!=nullptr;
  }

bool GameScript::wld_isnextfpavailable(std::shared_ptr<zenkit::INpc> self, std::string_view name) {
  if(self==nullptr){
    return false;
    }
  auto fp = world().findNextFreePoint(*findNpc(self.get()),name);
  return fp != nullptr;
  }

bool GameScript::wld_ismobavailable(std::shared_ptr<zenkit::INpc> self, std::string_view name) {
  auto npc = findNpc(self);
  if(npc==nullptr) {
    return false;
    }

  auto wp = world().availableMob(*npc, name);
  return wp != nullptr;
  }

void GameScript::wld_setmobroutine(int h, int m, std::string_view name, int st) {
  world().setMobRoutine(gtime(h,m), name, st);
  }

/// Wld_SetObjectRoutine of Gothic 2 (Addon): the same as for the mobs, the object is found by the name
void GameScript::wld_setobjectroutine(int h, int m, std::string_view name, int st) {
  world().setMobRoutine(gtime(h,m), name, st);
  }

int GameScript::wld_getmobstate(std::shared_ptr<zenkit::INpc> npcRef, std::string_view scheme) {
  auto npc = findNpc(npcRef);

  if(npc==nullptr) {
    return -1;
    }

  auto mob = world().availableMob(*npc,scheme);
  if(mob==nullptr) {
    return -1;
    }

  return std::max(0,mob->stateId());
  }

void GameScript::wld_assignroomtoguild(std::string_view name, int g) {
  world().assignRoomToGuild(name,g);
  }

bool GameScript::wld_detectnpc(std::shared_ptr<zenkit::INpc> npcRef, int inst, int state, int guild) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    return false;
    }

  Npc*  ret =nullptr;
  float dist=std::numeric_limits<float>::max();

  world().detectNpc(npc->position(), float(npc->handle().senses_range), [inst,state,guild,&ret,&dist,npc](Npc& n){
    if((inst ==-1 || int32_t(n.instanceSymbol())==inst) &&
       (state==-1 || n.isInState(uint32_t(state))) &&
       (guild==-1 || int32_t(n.guild())==guild) &&
       (&n!=npc) && !n.isDead()) {
      float d = n.qDistTo(*npc);
      if(d<dist){
        ret = &n;
        dist = d;
        }
      }
    });
  if(ret)
    vm.global_other()->set_instance(ret->handlePtr());
  return ret != nullptr;
  }

bool GameScript::wld_detectnpcex(std::shared_ptr<zenkit::INpc> npcRef, int inst, int state, int guild, int player) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    return false;
    }
  Npc*  ret =nullptr;
  float dist=std::numeric_limits<float>::max();

  world().detectNpc(npc->position(), float(npc->handle().senses_range), [inst,state,guild,&ret,&dist,npc,player](Npc& n){
    if((inst ==-1 || int32_t(n.instanceSymbol())==inst) &&
       (state==-1 || n.isInState(uint32_t(state))) &&
       (guild==-1 || int32_t(n.guild())==guild) &&
       (&n!=npc) && !n.isDead() &&
       (player!=0 || !n.isPlayer())) {
      float d = n.qDistTo(*npc);
      if(d<dist){
        ret = &n;
        dist = d;
        }
      }
    });
  if(ret)
    vm.global_other()->set_instance(ret->handlePtr());
  return ret != nullptr;
  }

bool GameScript::wld_detectitem(std::shared_ptr<zenkit::INpc> npcRef, int flags) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    return false;
    }

  Item* ret =nullptr;
  float dist=std::numeric_limits<float>::max();
  world().detectItem(npc->position(), float(npc->handle().senses_range), [npc,&ret,&dist,flags](Item& it) {
    if((it.handle().main_flag&flags)==0)
      return;
    float d = (npc->position()-it.position()).quadLength();
    if(d<dist) {
      ret = &it;
      dist= d;
      }
    });

  if(ret)
    vm.global_item()->set_instance(ret->handlePtr());
  return ret != nullptr;
  }

void GameScript::wld_spawnnpcrange(std::shared_ptr<zenkit::INpc> npcRef, int clsId, int count, float lifeTime) {
  auto at = findNpc(npcRef);
  if(at==nullptr || clsId<=0)
    return;

  (void)lifeTime;
  for(int32_t i=0;i<count;++i) {
    auto* npc = world().addNpc(size_t(clsId),at->position());
    fixNpcPosition(*npc,at->rotation() + 360.f*float(i)/float(count),100);
    }
  }

void GameScript::wld_sendtrigger(std::string_view triggerTarget) {
  if(triggerTarget.empty())
    return;

  auto& world = *owner.world();
  const TriggerEvent evt(std::string{triggerTarget},"",world.tickCount(),TriggerEvent::T_Trigger);
  world.triggerEvent(evt);
  }

void GameScript::wld_senduntrigger(std::string_view triggerTarget) {
  if(triggerTarget.empty())
    return;
  auto& world = *owner.world();
  const TriggerEvent evt(std::string{triggerTarget},"",world.tickCount(),TriggerEvent::T_Untrigger);
  world.triggerEvent(evt);
  }

bool GameScript::wld_israining() {
  static bool first=true;
  if(first){
    Log::e("not implemented call [wld_israining]");
    first=false;
  }
  return false;
  }

void GameScript::mdl_setvisual(std::shared_ptr<zenkit::INpc> npcRef, std::string_view visual) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return;
  npc->setVisual(visual);
  }

void GameScript::mdl_setvisualbody(std::shared_ptr<zenkit::INpc> npcRef, std::string_view body, int bodyTexNr, int bodyTexColor, std::string_view head, int headTexNr, int teethTexNr, int armor) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return;

  npc->setVisualBody(headTexNr,teethTexNr,bodyTexNr,bodyTexColor,body,head);
  if(armor>0) {
    if(npc->itemCount(uint32_t(armor))==0)
      npc->addItem(uint32_t(armor),1);
    npc->equipItem(uint32_t(armor));
    }
  else if(armor<0) {
    // NO_ARMOR is -1. CoM's NPC_ChangeFace passes 0 to retain the armor.
    if(auto* equipped = npc->currentArmor())
      npc->unequipItem(equipped->clsId());
    }
  }

void GameScript::mdl_setmodelfatness(std::shared_ptr<zenkit::INpc> npcRef, float fat) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setFatness(fat);
  }

void GameScript::mdl_applyoverlaymds(std::shared_ptr<zenkit::INpc> npcRef, std::string_view overlayname) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr) {
    auto skelet = Resources::loadSkeleton(overlayname);
    npc->addOverlay(skelet,0);
    Log::d("Mdl_ApplyOverlayMds: ", overlayname, skelet==nullptr ? " (not found)" : "");
    // NB diagnostics (fix 47): an overlay of the hero that does not load leaves him the animations of the base
    if(npc->isPlayer())
      Log::i("[anim] hero overlay +",overlayname,skelet==nullptr ? " NOT LOADED" : "");
    }
  }

void GameScript::mdl_applyoverlaymdstimed(std::shared_ptr<zenkit::INpc> npcRef, std::string_view overlayname, int ticks) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && ticks>0) {
    auto skelet = Resources::loadSkeleton(overlayname);
    npc->addOverlay(skelet,uint64_t(ticks));
    }
  }

void GameScript::mdl_removeoverlaymds(std::shared_ptr<zenkit::INpc> npcRef, std::string_view overlayname) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr) {
    auto skelet = Resources::loadSkeleton(overlayname);
    npc->delOverlay(skelet);
    Log::d("Mdl_RemoveOverlayMds: ", overlayname, skelet==nullptr ? " (not found)" : "");
  }
}

void GameScript::mdl_setmodelscale(std::shared_ptr<zenkit::INpc> npcRef, float x, float y, float z) {
  auto npc = findNpc(npcRef);
  if(npcRef!=nullptr)
    npc->setScale(x,y,z);
  }

void GameScript::mdl_startfaceani(std::shared_ptr<zenkit::INpc> npcRef, std::string_view ani, float intensity, float time) {
  if(npcRef!=nullptr)
    findNpc(npcRef.get())->startFaceAnim(ani,intensity,uint64_t(time*1000.f));
  }

void GameScript::mdl_applyrandomani(std::shared_ptr<zenkit::INpc> npcRef, std::string_view s1, std::string_view s0) {
  (void)npcRef;
  (void)s1;
  (void)s0;

  static bool first=true;
  if(first){
    Log::e("not implemented call [mdl_applyrandomani]");
    first=false;
    }
  }

void GameScript::mdl_applyrandomanifreq(std::shared_ptr<zenkit::INpc> npcRef, std::string_view s1, float f0) {
  (void)f0;
  (void)s1;
  (void)npcRef;

  static bool first=true;
  if(first){
    Log::e("not implemented call [mdl_applyrandomanifreq]");
    first=false;
    }
  }

void GameScript::mdl_applyrandomfaceani(std::shared_ptr<zenkit::INpc> npcRef, std::string_view name, float timeMin, float timeMinVar, float timeMax, float timeMaxVar, float probMin) {
  (void)probMin;
  (void)timeMaxVar;
  (void)timeMax;
  (void)timeMinVar;
  (void)timeMin;
  (void)name;
  (void)npcRef;

  static bool first=true;
  if(first){
    Log::e("not implemented call [mdl_applyrandomfaceani]");
    first=false;
    }
  }

void GameScript::wld_insertnpc(int npcInstance, std::string_view spawnpoint) {
  if(npcInstance<=0)
    return;

  auto npc = world().addNpc(size_t(npcInstance),spawnpoint);
  if(npc!=nullptr)
    fixNpcPosition(*npc,0,150);
  }

void GameScript::wld_removenpc(int npcInstance) {
  if(auto npc = findNpcById(size_t(npcInstance)))
    world().removeNpc(*npc);
  }

void GameScript::wld_insertitem(int itemInstance, std::string_view spawnpoint) {
  if(spawnpoint.empty() || itemInstance<=0)
    return;

  world().addItem(size_t(itemInstance),spawnpoint);
  }

void GameScript::wld_insertobject(std::string_view visual, std::string_view spawnpoint) {
  world().addObject(visual,spawnpoint);
  }

void GameScript::npc_settofightmode(std::shared_ptr<zenkit::INpc> npcRef, int weaponSymbol) {
  if(npcRef!=nullptr && weaponSymbol>=0)
    findNpc(npcRef.get())->setToFightMode(size_t(weaponSymbol));
  }

void GameScript::npc_settofistmode(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setToFistMode();
  }

bool GameScript::npc_isinstate(std::shared_ptr<zenkit::INpc> npcRef, int stateFn) {
  auto npc = findNpc(npcRef);
  // the developer tool of the dialogs holds a dialog without the state of the talk
  if(npc!=nullptr && DialogTest::active && npc==DialogTest::talkNpc && size_t(stateFn)==ZS_Talk)
    return true;
  if(npc!=nullptr)
    return npc->isInState(uint32_t(stateFn));
  return false;
  }

bool GameScript::npc_isinroutine(std::shared_ptr<zenkit::INpc> npcRef, int stateFn) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    return npc->isInRoutine(uint32_t(stateFn));
  return false;
  }

bool GameScript::npc_wasinstate(std::shared_ptr<zenkit::INpc> npcRef, int stateFn) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    return npc->wasInState(uint32_t(stateFn));
  return false;
  }

int GameScript::npc_getdisttowp(std::shared_ptr<zenkit::INpc> npcRef, std::string_view wpname) {
  auto  npc = findNpc(npcRef);
  //NOTE: in CoM, some way-point and free-points share same name - need to be precise
  auto* wp  = world().findWayPoint(wpname);

  if(npc!=nullptr && wp!=nullptr){
    float ret = std::sqrt(npc->qDistTo(wp));
    if(ret<float(std::numeric_limits<int32_t>::max()))
      return int32_t(ret); else
      return std::numeric_limits<int32_t>::max();
    } else {
    if(wp==nullptr)
      Log::d("Npc_GetDistToWP: no way point '",wpname,"'");
    return std::numeric_limits<int32_t>::max();
    }
  }

void GameScript::npc_exchangeroutine(std::shared_ptr<zenkit::INpc> npcRef, std::string_view rname) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr) {
    auto& v = npc->handle();
    string_frm name("Rtn_",rname,'_',v.id);

    auto* sym = vm.find_symbol_by_name(name);
    size_t d = sym != nullptr ? sym->index() : 0;
    if(hasDma())
      Log::d("Npc_ExchangeRoutine: ",npc->displayName()," -> ",rname,d==0 ? " (no such routine)" : "");
    if(d>0)
      npc->excRoutine(d);
    }
  }

bool GameScript::npc_isdead(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc==nullptr || isDead(*npc);
  }

bool GameScript::npc_knowsinfo(std::shared_ptr<zenkit::INpc> npcRef, int infoinstance) {
  auto npc = findNpc(npcRef);
  if(!npc)
    return false;

  zenkit::INpc& vnpc = npc->handle();
  return doesNpcKnowInfo(vnpc, uint32_t(infoinstance));
  }

void GameScript::npc_settalentskill(std::shared_ptr<zenkit::INpc> npcRef, int t, int lvl) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setTalentSkill(Talent(t),lvl);
  }

int GameScript::npc_gettalentskill(std::shared_ptr<zenkit::INpc> npcRef, int skillId) {
  auto npc = findNpc(npcRef);
  return npc==nullptr ? 0 : npc->talentSkill(Talent(skillId));
  }

void GameScript::npc_settalentvalue(std::shared_ptr<zenkit::INpc> npcRef, int t, int lvl) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setTalentValue(Talent(t),lvl);
  }

int GameScript::npc_gettalentvalue(std::shared_ptr<zenkit::INpc> npcRef, int skillId) {
  auto npc = findNpc(npcRef);
  return npc==nullptr ? 0 : npc->talentValue(Talent(skillId));
  }

void GameScript::npc_setrefusetalk(std::shared_ptr<zenkit::INpc> npcRef, int timeSec) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->setRefuseTalk(uint64_t(std::max(timeSec*1000,0)));
  }

bool GameScript::npc_refusetalk(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc && npc->isRefuseTalk();
  }

int GameScript::npc_hasitems(std::shared_ptr<zenkit::INpc> npcRef, int itemId) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr ? int(npc->itemCount(uint32_t(itemId))) : 0;
  }

bool GameScript::npc_hasspell(std::shared_ptr<zenkit::INpc> npcRef, int splId) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr && npc->inventory().hasSpell(splId);
  }

int GameScript::npc_getinvitem(std::shared_ptr<zenkit::INpc> npcRef, int itemId) {
  auto npc = findNpc(npcRef);
  auto itm = npc==nullptr ? nullptr : npc->getItem(uint32_t(itemId));
  storeItem(itm);
  if(itm!=nullptr) {
    return int(itm->handle().symbol_index());
    }
  return -1;
  }

// This seems specific to Gothic 1, where the inventory was grouped into categories.
// In the shared inventory, these categories are just following one after the other.
// So we should transform this to iterate over the different categories in the
// shared inventory
int GameScript::npc_getinvitembyslot(std::shared_ptr<zenkit::INpc> npcRef, int cat, int slotnr) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    storeItem(nullptr);
    return 0;
    }

  // The category flag names were global, but for the scripts, only npc_getinvitembyslot
  // ever used them, so as long as nobody implements the Gothic 1 inventory, they can
  // be limited to the comments below.
  ItmFlags f = ITM_CAT_NONE;
  switch(cat) {
    case 1: // INV_WEAPON
      f = ItmFlags(ITM_CAT_NF|ITM_CAT_FF|ITM_CAT_MUN);
      break;
    case 2: // INV_ARMOR
      f = ITM_CAT_ARMOR;
      break;
    case 3: // INV_RUNE
      f = ITM_CAT_RUNE;
      break;
    case 4: // INV_MAGIC
      f = ITM_CAT_MAGIC;
      break;
    case 5: // INV_FOOD
      f = ITM_CAT_FOOD;
      break;
    case 6: // INV_POTION
      f = ITM_CAT_POTION;
      break;
    case 7: // INV_DOC
      f = ITM_CAT_DOCS;
      break;
    case 8: // INV_MISC
      f = ItmFlags(ITM_CAT_LIGHT|ITM_CAT_NONE);
      break;
    default:
      // 0 is "no category" in the scripts of the mods: there is no item, it is not an error
      if(cat!=0)
        Log::e("Unknown item category ", cat);
      storeItem(nullptr);
      return 0;
    }

  auto itm = npc==nullptr ? nullptr : npc->inventory().findByFlags(f, uint32_t(slotnr));
  // Store the found item in the global item var
  storeItem(itm);

  return itm!=nullptr ? int(itm->count()) : 0;
  }

int GameScript::npc_removeinvitem(std::shared_ptr<zenkit::INpc> npcRef, int itemId) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->delItem(uint32_t(itemId),1);
  return 0;
  }

int GameScript::npc_removeinvitems(std::shared_ptr<zenkit::INpc> npcRef, int itemId, int amount) {
  auto npc = findNpc(npcRef);

  if(npc!=nullptr && amount>0)
    npc->delItem(uint32_t(itemId),uint32_t(amount));

  return 0;
  }

int GameScript::npc_getbodystate(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);

  if(npc!=nullptr)
    return int32_t(npc->bodyState());
  return int32_t(0);
  }

std::shared_ptr<zenkit::INpc> GameScript::npc_getlookattarget(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc && npc->lookAtTarget() ? npc->lookAtTarget()->handlePtr() : nullptr;
  }

int GameScript::npc_getdisttonpc(std::shared_ptr<zenkit::INpc> aRef, std::shared_ptr<zenkit::INpc> bRef) {
  auto a = findNpc(aRef);
  auto b = findNpc(bRef);

  if(a==nullptr || b==nullptr)
    return std::numeric_limits<int32_t>::max();

  float ret = std::sqrt(a->qDistTo(*b));
  if(ret>float(std::numeric_limits<int32_t>::max()))
    return std::numeric_limits<int32_t>::max();
  return int(ret);
  }

bool GameScript::npc_hasequippedarmor(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr && npc->currentArmor()!=nullptr;
  }

void GameScript::npc_setperctime(std::shared_ptr<zenkit::INpc> npcRef, float sec) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->setPerceptionTime(uint64_t(sec*1000));
  }

void GameScript::npc_percenable(std::shared_ptr<zenkit::INpc> npcRef, int pr, int fn) {
  auto npc = findNpc(npcRef);
  if(npc && fn>=0)
    npc->setPerceptionEnable(PercType(pr),size_t(fn));
  }

void GameScript::npc_percdisable(std::shared_ptr<zenkit::INpc> npcRef, int pr) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->setPerceptionDisable(PercType(pr));
  }

std::string GameScript::npc_getnearestwp(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto wp  = npc ? world().findWayPoint(npc->position()) : nullptr;
  if(wp)
    return wp->name;
  return "";
  }

std::string GameScript::npc_getnextwp(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto wp  = npc ? world().findNextWayPoint(*npc) : nullptr;
  if(wp)
    return wp->name;
  return "";
  }

void GameScript::npc_clearaiqueue(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->clearAiQueue();
  }

bool GameScript::npc_isplayer(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc && npc->isPlayer();
  }

int GameScript::npc_getstatetime(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc)
    return int32_t(npc->stateTime()/1000);
  return 0;
  }

void GameScript::npc_setstatetime(std::shared_ptr<zenkit::INpc> npcRef, int val) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->setStateTime(val*1000);
  }

void GameScript::npc_changeattribute(std::shared_ptr<zenkit::INpc> npcRef, int atr, int val) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && atr>=0)
    npc->changeAttribute(Attribute(atr),val,false);
  }

bool GameScript::npc_isonfp(std::shared_ptr<zenkit::INpc> npcRef, std::string_view val) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return false;

  auto w = npc->currentWayPoint();
  if(w==nullptr || !MoveAlgo::isClose(*npc,*w,MAX_AI_USE_DISTANCE) || !w->checkName(val))
    return false;
  return w->isFreePoint();
  }

int GameScript::npc_getheighttonpc(std::shared_ptr<zenkit::INpc> aRef, std::shared_ptr<zenkit::INpc> bRef) {
  auto a = findNpc(aRef);
  auto b = findNpc(bRef);
  float ret = 0;
  if(a!=nullptr && b!=nullptr)
    ret = std::abs(a->position().y - b->position().y);
  return int32_t(ret);
  }

std::shared_ptr<zenkit::IItem> GameScript::npc_getequippedmeleeweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && npc->currentMeleeWeapon() != nullptr) {
    return npc->currentMeleeWeapon()->handlePtr();
    }
  return nullptr;
  }

std::shared_ptr<zenkit::IItem> GameScript::npc_getequippedrangedweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && npc->currentRangedWeapon() != nullptr) {
    return npc->currentRangedWeapon()->handlePtr();
    }
  return nullptr;
  }

std::shared_ptr<zenkit::IItem> GameScript::npc_getequippedarmor(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && npc->currentArmor()!=nullptr) {
    return npc->currentArmor()->handlePtr();
    }
  return nullptr;
  }

bool GameScript::npc_canseenpc(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::INpc> otherRef) {
  // 'see' functions are intended as ray-cast, ignoring hnpc->senses mask
  // https://discord.com/channels/989316194148433950/989333514543587339/1226664463697182760
  auto other = findNpc(otherRef);
  auto npc   = findNpc(npcRef);

  if(npc!=nullptr && other!=nullptr){
    return npc->canSeeNpc(*other,false);
    }
  return false;
  }

bool GameScript::npc_canseenpcfreelos(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::INpc> otherRef) {
  auto npc = findNpc(npcRef);
  auto oth = findNpc(otherRef);

  if(npc!=nullptr && oth!=nullptr){
    return npc->canSeeNpc(*oth,true);
    }
  return false;
  }

bool GameScript::npc_canseeitem(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::IItem> itemRef) {
  auto npc = findNpc(npcRef);
  auto itm = findItem(itemRef.get());

  if(npc!=nullptr && itm!=nullptr){
    return npc->canSeeItem(*itm,false);
    }
  return false;
  }

bool GameScript::npc_hasequippedweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return (npc!=nullptr &&
     (npc->currentMeleeWeapon()!=nullptr ||
      npc->currentRangedWeapon()!=nullptr));
  }

bool GameScript::npc_hasequippedmeleeweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr && npc->currentMeleeWeapon()!=nullptr;
  }

bool GameScript::npc_hasequippedrangedweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr && npc->currentRangedWeapon()!=nullptr;
  }

int GameScript::npc_getactivespell(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return -1;

  Item* w = npc->activeWeapon();
  if(w==nullptr || !w->isSpellOrRune())
    return -1;

  makeCurrent(w);
  return w->spellId();
  }

bool GameScript::npc_getactivespellisscroll(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return false;

  Item* w = npc->activeWeapon();
  if(w==nullptr || !w->isSpell())
    return false;

  return true;
  }

bool GameScript::npc_isinfightmode(std::shared_ptr<zenkit::INpc> npcRef, int modeI) {
  auto npc  = findNpc(npcRef);
  auto mode = FightMode(modeI);

  if(npc==nullptr){
    return false;
    }

  auto st  = npc->weaponState();
  bool ret = false;
  if(mode==FightMode::FMODE_NONE){
    ret = (st==WeaponState::NoWeapon);
    }
  else if(mode==FightMode::FMODE_FIST){
    ret = (st==WeaponState::Fist);
    }
  else if(mode==FightMode::FMODE_MELEE){
    ret = (st==WeaponState::W1H || st==WeaponState::W2H);
    }
  else if(mode==FightMode::FMODE_FAR){
    ret = (st==WeaponState::Bow || st==WeaponState::CBow);
    }
  else if(mode==FightMode::FMODE_MAGIC){
    ret = (st==WeaponState::Mage);
    }
  return ret;
  }

void GameScript::npc_settarget(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::INpc> otherRef) {
  auto oth = findNpc(otherRef);
  auto npc = findNpc(npcRef);
  if(npc)
    npc->setTarget(oth);
  }

/**
 * @brief WorldScript::npc_gettarget
 * Fill 'other' with the current npc target.
 * set by Npc_SetTarget () or Npc_GetNextTarget ().
 * - return: current target saved -> TRUE
 * no target saved -> FALSE
 */
bool GameScript::npc_gettarget(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto s   = vm.global_other();

  if(npc!=nullptr && npc->target()) {
    s->set_instance(npc->target()->handlePtr());
    return true;
    }

  s->set_instance(nullptr);
  return false;
  }

bool GameScript::npc_getnexttarget(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  Npc* ret = nullptr;

  if(npc!=nullptr){
    float dist = float(npc->handle().senses_range);
    dist*=dist;

    world().detectNpc(npc->position(),float(npc->handle().senses_range),[&,npc](Npc& oth){
      if(&oth!=npc && !oth.isDown() && oth.isEnemy(*npc) && npc->canSenseNpc(oth,true)!=SensesBit::SENSE_NONE){
        float qd = oth.qDistTo(*npc);
        if(qd<dist){
          dist=qd;
          ret = &oth;
          }
        }
      return false;
      });
    if(ret!=nullptr)
      npc->setTarget(ret);
    }

  auto s = vm.global_other();
  if(ret!=nullptr) {
    s->set_instance(ret->handlePtr());
    return true;
    } else {
    s->set_instance(nullptr);
    return false;
    }
  }

void GameScript::npc_sendpassiveperc(std::shared_ptr<zenkit::INpc> npcRef, int id, std::shared_ptr<zenkit::INpc> victimRef, std::shared_ptr<zenkit::INpc> otherRef) {
  auto npc    = findNpc(npcRef);
  auto other  = findNpc(otherRef);
  auto victim = findNpc(victimRef);

  if(npc && other && victim)
    world().sendPassivePerc(*npc,*other,*victim,id);
  else if(npc && other)
    world().sendPassivePerc(*npc,*other,id);
  }

void GameScript::npc_sendsingleperc(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::INpc> otherRef, int id) {
  auto other  = findNpc(otherRef);
  auto npc    = findNpc(npcRef);

  if(npc && other)
    other->perceptionProcess(*npc,nullptr,0,PercType(id));
  }

bool GameScript::npc_checkinfo(std::shared_ptr<zenkit::INpc> npcRef, int imp) {
  auto n    = findNpc(npcRef);
  auto hero = findNpc(vm.global_other());
  if(n==nullptr || hero==nullptr)
    return false;

  auto& pl  = hero->handle();
  auto& npc = n->handle();
  for(auto& info:dialogsInfo) {
    if(info->npc!=int32_t(npc.symbol_index()) || info->important!=imp)
      continue;
    bool npcKnowsInfo = doesNpcKnowInfo(pl,info->symbol_index());
    if(npcKnowsInfo && !info->permanent)
      continue;
    bool valid=false;
    if(info->condition) {
      auto* conditionSymbol = vm.find_symbol_by_index(uint32_t(info->condition));
      if (conditionSymbol != nullptr)
        valid = vm.call_function<int>(conditionSymbol)!=0;
      }
    if(valid) {
      return true;
      }
    }
  return false;
  }

int GameScript::npc_getportalguild(std::shared_ptr<zenkit::INpc> npcRef) {
  int32_t g   = GIL_NONE;
  auto    npc = findNpc(npcRef);
  if(npc!=nullptr)
    g = world().guildOfRoom(npc->portalName());
  return g;
  }

bool GameScript::npc_isinplayersroom(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto pl  = world().player();

  if(npc!=nullptr && pl!=nullptr) {
    auto g1 = pl ->portalName();
    auto g2 = npc->portalName();
    if(g1==g2)
      return true;
    }
  return false;
  }

std::shared_ptr<zenkit::IItem> GameScript::npc_getreadiedweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    return 0;
    }

  auto ret = npc->activeWeapon();
  if(ret!=nullptr) {
    makeCurrent(ret);
    return ret->handlePtr();
    } else {
    return nullptr;
    }
  }

bool GameScript::npc_hasreadiedweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return false;
  auto ws = npc->weaponState();
  return (ws==WeaponState::W1H || ws==WeaponState::W2H ||
          ws==WeaponState::Bow || ws==WeaponState::CBow);
  }

bool GameScript::npc_hasreadiedmeleeweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr) {
    return false;
    }
  auto ws = npc->weaponState();
  return ws==WeaponState::W1H || ws==WeaponState::W2H;
  }

bool GameScript::npc_hasreadiedrangedweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return false;
  auto ws = npc->weaponState();
  return ws==WeaponState::Bow || ws==WeaponState::CBow;
  }

bool GameScript::npc_hasrangedweaponwithammo(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  return npc!=nullptr && npc->inventory().hasRangedWeaponWithAmmo();
  }

int GameScript::npc_isdrawingspell(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return 0;

  auto ret = npc->activeWeapon();
  if(ret==nullptr || !ret->isSpell())
    return 0;

  makeCurrent(ret);
  return int32_t(ret->clsId());
  }

int GameScript::npc_isdrawingweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return 0;

  auto ret = npc->activeWeapon();
  if(ret==nullptr || !ret->isSpell())
    return 0;

  makeCurrent(ret);
  return int32_t(ret->clsId());
  }

void GameScript::npc_perceiveall(std::shared_ptr<zenkit::INpc> npcRef) {
  (void)npcRef; // nop
  }

void GameScript::npc_stopani(std::shared_ptr<zenkit::INpc> npcRef, std::string_view name) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->stopAnim(name);
  }

int GameScript::npc_settrueguild(std::shared_ptr<zenkit::INpc> npcRef, int gil) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setTrueGuild(gil);
  return 0;
  }

int GameScript::npc_gettrueguild(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    return npc->trueGuild();
  return int32_t(GIL_NONE);
  }

void GameScript::npc_clearinventory(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->clearInventory();
  }

int GameScript::npc_getattitude(std::shared_ptr<zenkit::INpc> aRef, std::shared_ptr<zenkit::INpc> bRef) {
  auto a = findNpc(aRef);
  auto b = findNpc(bRef);

  if(a!=nullptr && b!=nullptr){
    return personAttitude(*a,*b);
    }
  return ATT_NEUTRAL;
  }

int GameScript::npc_getpermattitude(std::shared_ptr<zenkit::INpc> aRef, std::shared_ptr<zenkit::INpc> bRef) {
  auto a = findNpc(aRef);
  auto b = findNpc(bRef);

  if(a!=nullptr && b!=nullptr) {
    if(a->isPlayer() || b->isPlayer()) {
      const Npc& npc = a->isPlayer() ? *b : *a;
      if(npc.attitude()!=ATT_NULL)
        return npc.attitude();
      }
    return guildAttitude(*a,*b);
    }
  return ATT_NEUTRAL;
  }

void GameScript::npc_setattitude(std::shared_ptr<zenkit::INpc> npcRef, int att) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setAttitude(Attitude(att));
  }

void GameScript::npc_settempattitude(std::shared_ptr<zenkit::INpc> npcRef, int att) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setTempAttitude(Attitude(att));
  }

bool GameScript::npc_hasbodyflag(std::shared_ptr<zenkit::INpc> npcRef, int bodyflag) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return false;
  return npc->hasStateFlag(BodyState(bodyflag));
  }

int GameScript::npc_getlasthitspellid(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr){
    return 0;
    }
  return npc->lastHitSpellId();
  }

int GameScript::npc_getlasthitspellcat(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return SPELL_GOOD;

  const int id    = npc->lastHitSpellId();
  auto&     spell = spellDesc(id);
  return spell.spell_type;
  }

void GameScript::npc_playani(std::shared_ptr<zenkit::INpc> npcRef, std::string_view name) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->playAnimByName(name,BS_NONE);
  }

bool GameScript::npc_isdetectedmobownedbynpc(std::shared_ptr<zenkit::INpc> usrRef, std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto usr = findNpc(usrRef);

  if(npc!=nullptr && usr!=nullptr && usr->interactive()!=nullptr){
    auto* inst = vm.find_symbol_by_index(npc->instanceSymbol());
    auto  ow   = usr->interactive()->ownerName();
    return inst->name() == ow;
    }
  return false;
  }

bool GameScript::npc_isdetectedmobownedbyguild(std::shared_ptr<zenkit::INpc> npcRef, int guild) {
  static bool first=true;
  if(first){
    Log::e("not implemented call [npc_isdetectedmobownedbyguild]");
    first=false;
    }

  auto npc = findNpc(npcRef);
  (void)guild;

  if(npc!=nullptr && npc->detectedMob()!=nullptr) {
    auto  ow   = npc->detectedMob()->ownerName();
    (void)ow;
    //vm.setReturn(inst.name==ow ? 1 : 0);
    return false;
    }
  return false;
  }

std::string GameScript::npc_getdetectedmob(std::shared_ptr<zenkit::INpc> npcRef) {
  auto usr = findNpc(npcRef);
  if(usr!=nullptr && usr->detectedMob()!=nullptr){
    auto i = usr->detectedMob();
    return std::string(i->schemeName());
    }
  return "";
  }

bool GameScript::npc_ownedbynpc(std::shared_ptr<zenkit::IItem> itmRef, std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto itm = findItem(itmRef.get());
  if(itm==nullptr || npc==nullptr) {
    return false;
    }

  auto* sym = vm.find_symbol_by_index(uint32_t(itm->handle().owner));
  return sym != nullptr && npc->handlePtr()==sym->get_instance();
  }

bool GameScript::npc_canseesource(std::shared_ptr<zenkit::INpc> npcRef) {
  auto self = findNpc(npcRef);
  if(!self)
    return false;
  return self->canSeeSource();
  }

// Used (only?) in Gothic 1 in B_AssessEnemy, to prevent attacks during cutscenes.
bool GameScript::npc_isincutscene(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  auto w = Gothic::inst().world();
  if(w==nullptr)
    return false;

  if(npc!=nullptr && owner.isNpcInDialog(*npc))
    return true;

  return false;
  }

int GameScript::npc_getdisttoitem(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::IItem> itmRef) {
  auto itm = findItem(itmRef.get());
  auto npc = findNpc(npcRef);
  if(itm==nullptr || npc==nullptr) {
    return std::numeric_limits<int32_t>::max();
    }
  auto dp = itm->position()-npc->position();
  return int32_t(dp.length());
  }

int GameScript::npc_getheighttoitem(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::IItem> itmRef) {
  auto itm = findItem(itmRef.get());
  auto npc = findNpc(npcRef);
  if(itm==nullptr || npc==nullptr) {
    return std::numeric_limits<int32_t>::max();
    }
  auto dp = int32_t(itm->position().y-npc->position().y);
  return std::abs(dp);
  }

int GameScript::npc_getdisttoplayer(std::shared_ptr<zenkit::INpc> npcRef) {
  auto pl  = world().player();
  auto npc = findNpc(npcRef);
  if(pl==nullptr || npc==nullptr) {
    return std::numeric_limits<int32_t>::max();
    }
  auto dp = pl->position()-npc->position();
  auto l  = dp.length();
  if(l>float(std::numeric_limits<int32_t>::max())) {
    return std::numeric_limits<int32_t>::max();
    }
  return int32_t(l);
  }

int GameScript::npc_getactivespellcat(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return SPELL_GOOD;

  const Item* w = npc->activeWeapon();
  if(w==nullptr || !w->isSpellOrRune())
    return SPELL_GOOD;

  const int id    = w->spellId();
  auto&     spell = spellDesc(id);
  return spell.spell_type;
  }

int GameScript::npc_setactivespellinfo(std::shared_ptr<zenkit::INpc> npcRef, int v) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->setActiveSpellInfo(v);
  return 0;
  }

int GameScript::npc_getactivespelllevel(std::shared_ptr<zenkit::INpc> npcRef) {
  int  v   = 0;
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    v = npc->activeSpellLevel();
  return v;
  }

void GameScript::ai_processinfos(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpcRaw(npcRef);
  auto pl  = owner.player();
  if(pl!=nullptr && npc!=nullptr) {
    aiOutOrderId=0;
    npc->aiPush(AiQueue::aiProcessInfo(*pl));
    }
  }

void GameScript::ai_output(std::shared_ptr<zenkit::INpc> selfRef, std::shared_ptr<zenkit::INpc> targetRef, std::string_view outputname) {
  auto target = findNpc(targetRef);
  auto self   = findNpc(selfRef);

  if(target==nullptr && owner.version().game==1)
    target = findNpc(vm.global_other());

  if(self!=nullptr && target!=nullptr) {
    self->aiPush(AiQueue::aiOutput(*target,outputname,aiOutOrderId));
    ++aiOutOrderId;
    }
  }

void GameScript::ai_stopprocessinfos(std::shared_ptr<zenkit::INpc> selfRef) {
  auto self = findNpcRaw(selfRef);
  if(self) {
    self->aiPush(AiQueue::aiStopProcessInfo(aiOutOrderId));
    ++aiOutOrderId;
    }
  }

void GameScript::ai_standup(std::shared_ptr<zenkit::INpc> selfRef) {
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiStandup());
  }

void GameScript::ai_standupquick(std::shared_ptr<zenkit::INpc> selfRef) {
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiStandupQuick());
  }

void GameScript::ai_continueroutine(std::shared_ptr<zenkit::INpc> selfRef) {
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiContinueRoutine());
  }

void GameScript::ai_stoplookat(std::shared_ptr<zenkit::INpc> selfRef) {
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiStopLookAt());
  }

void GameScript::ai_lookat(std::shared_ptr<zenkit::INpc> selfRef, std::string_view waypoint) {
  auto self = findNpc(selfRef);
  auto to  = world().findPoint(waypoint);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiLookAt(to));
  }

void GameScript::ai_lookatnpc(std::shared_ptr<zenkit::INpc> selfRef, std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc  = findNpc(npcRef);
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiLookAtNpc(npc));
  }

void GameScript::ai_removeweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiRemoveWeapon());
  }

void GameScript::ai_unreadyspell(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiRemoveWeapon());
  }

void GameScript::ai_turnaway(std::shared_ptr<zenkit::INpc> selfRef, std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc  = findNpc(npcRef);
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiTurnAway(npc));
  }

void GameScript::ai_turntonpc(std::shared_ptr<zenkit::INpc> selfRef, std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc  = findNpc(npcRef);
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiTurnToNpc(npc));
  }

void GameScript::ai_whirlaround(std::shared_ptr<zenkit::INpc> selfRef, std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc  = findNpc(npcRef);
  auto self = findNpc(selfRef);
  if(self!=nullptr)
    self->aiPush(AiQueue::aiWhirlToNpc(npc));
  }

void GameScript::ai_outputsvm(std::shared_ptr<zenkit::INpc> selfRef, std::shared_ptr<zenkit::INpc> targetRef, std::string_view name) {
  auto target = findNpc(targetRef);
  auto self   = findNpc(selfRef);

  if(target==nullptr && owner.version().game==1)
    target = findNpc(vm.global_other());

  if(self!=nullptr && target!=nullptr) {
    self->aiPush(AiQueue::aiOutputSvm(*target,name,aiOutOrderId));
    ++aiOutOrderId;
    }
  }

void GameScript::ai_outputsvm_overlay(std::shared_ptr<zenkit::INpc> selfRef, std::shared_ptr<zenkit::INpc> targetRef, std::string_view name) {
  auto target = findNpc(targetRef);
  auto self   = findNpc(selfRef);

  if(target==nullptr && owner.version().game==1)
    target = findNpc(vm.global_other());

  if(self!=nullptr && target!=nullptr) {
    self->aiPush(AiQueue::aiOutputSvmOverlay(*target,name,aiOutOrderId));
    ++aiOutOrderId;
    }
  }

void GameScript::ai_startstate(std::shared_ptr<zenkit::INpc> selfRef, int func, int state, std::string_view wp) {
  auto self = findNpc(selfRef);
  if(self!=nullptr && func>0) {
    Npc*  oth = findNpc(vm.global_other());
    Npc*  vic = findNpc(vm.global_victim());
    auto& st  = aiState(size_t(func));
    self->aiPush(AiQueue::aiStartState(st.funcIni,state,oth,vic,wp));
    }
  }

void GameScript::ai_playani(std::shared_ptr<zenkit::INpc> npcRef, std::string_view name) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && dma!=nullptr)
    dma->noteAiPlayAni(*npc,name);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiPlayAnim(name));
  }

void GameScript::ai_setwalkmode(std::shared_ptr<zenkit::INpc> npcRef, int modeBits) {
  int32_t weaponBit = 0x80;
  auto npc = findNpc(npcRef);

  int32_t mode = modeBits & (~weaponBit);
  if(npc!=nullptr && mode>=0 && mode<=3){ //TODO: weapon flags
    npc->aiPush(AiQueue::aiSetWalkMode(WalkBit(mode)));
    }
  }

void GameScript::ai_wait(std::shared_ptr<zenkit::INpc> npcRef, float ms) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && ms>0)
    npc->aiPush(AiQueue::aiWait(uint64_t(ms*1000)));
  }

void GameScript::ai_waitms(std::shared_ptr<zenkit::INpc> npcRef, int ms) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && ms>0)
    npc->aiPush(AiQueue::aiWait(uint64_t(ms)));
  }

void GameScript::ai_aligntowp(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->aiPush(AiQueue::aiAlignToWp());
  }

void GameScript::ai_waittillend(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::INpc> otherRef) {
  auto* npc = findNpc(npcRef);
  auto* other = findNpc(otherRef);
  if(npc!=nullptr && other!=nullptr)
    npc->aiWaitTillEnd(*other);
  }

void GameScript::ai_aimat(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::INpc> otherRef) {
  auto* npc = findNpc(npcRef);
  auto* other = findNpc(otherRef);
  if(npc!=nullptr && other!=nullptr)
    npc->aiPush(AiQueue::aiAimAt(*other));
  }

void GameScript::ai_shootat(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::INpc> otherRef) {
  auto* npc = findNpc(npcRef);
  auto* other = findNpc(otherRef);
  if(npc!=nullptr && other!=nullptr)
    npc->aiPush(AiQueue::aiShootAt(*other));
  }

void GameScript::ai_stopaim(std::shared_ptr<zenkit::INpc> npcRef) {
  if(auto* npc = findNpc(npcRef))
    npc->aiPush(AiQueue::aiStopAim());
  }

void GameScript::ai_dropitem(std::shared_ptr<zenkit::INpc> npcRef, int itemInstance) {
  if(itemInstance>0)
    if(auto* npc = findNpc(npcRef))
      npc->aiPush(AiQueue::aiDropItem(itemInstance));
  }

void GameScript::ai_snd_play(std::shared_ptr<zenkit::INpc> npcRef, std::string_view sound) {
  std::string name(sound);
  for(auto& c:name)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  if(auto* npc = findNpc(npcRef))
    npc->aiPush(AiQueue::aiSndPlay(name));
  }

void GameScript::ai_stopfx(std::shared_ptr<zenkit::INpc> npcRef, std::string_view effect) {
  if(auto* npc = findNpc(npcRef))
    npc->aiPush(AiQueue::aiStopFx(effect));
  }

int GameScript::wld_removeitem(std::shared_ptr<zenkit::IItem> itemRef) {
  auto* item = findItem(itemRef.get());
  if(item==nullptr)
    return 0;
  // Only remove loose world items. Inventory objects belong to their inventory.
  for(uint32_t i=0; auto* loose = world().itmById(i); ++i) {
    if(loose==item) {
      world().removeItem(*loose);
      return 1;
      }
    }
  return 0;
  }

void GameScript::ai_gotowp(std::shared_ptr<zenkit::INpc> npcRef, std::string_view waypoint) {
  auto npc = findNpc(npcRef);
  if(npc==nullptr)
    return;

  auto to = world().findWayPoint(npc->position(), waypoint);
  if(to!=nullptr) {
    npc->aiPush(AiQueue::aiGoToPoint(*to));
    return;
    }

  // in vanilla 'ai_gotowp' sometimes is used incorrectly, so we need to check all other points
  to = world().findPoint(waypoint, false);
  if(to!=nullptr)
    npc->aiPush(AiQueue::aiGoToPoint(*to));
  }

void GameScript::ai_gotofp(std::shared_ptr<zenkit::INpc> npcRef, std::string_view waypoint) {
  auto npc = findNpc(npcRef);

  if(npc) {
    auto to = world().findFreePoint(*npc,waypoint);
    if(to!=nullptr)
      npc->aiPush(AiQueue::aiGoToPoint(*to));
    }
  }

void GameScript::ai_playanibs(std::shared_ptr<zenkit::INpc> npcRef, std::string_view ani, int bs) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiPlayAnimBs(ani,BodyState(bs)));
  }

void GameScript::ai_equiparmor(std::shared_ptr<zenkit::INpc> npcRef, int id) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiEquipArmor(id));
  }

void GameScript::ai_equipbestarmor(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiEquipBestArmor());
  }

int GameScript::ai_equipbestmeleeweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiEquipBestMeleeWeapon());
  return 0;
  }

int GameScript::ai_equipbestrangedweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiEquipBestRangedWeapon());
  return 0;
  }

bool GameScript::ai_usemob(std::shared_ptr<zenkit::INpc> npcRef, std::string_view tg, int state) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiUseMob(tg,state));
  return 0;
  }

void GameScript::ai_teleport(std::shared_ptr<zenkit::INpc> npcRef, std::string_view tg) {
  auto npc = findNpc(npcRef);
  auto pt  = world().findPoint(tg);
  if(npc!=nullptr && pt!=nullptr)
    npc->aiPush(AiQueue::aiTeleport(*pt));
  }

void GameScript::ai_stoppointat(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiStopPointAt());
  }

void GameScript::ai_drawweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiDrawWeapon());
  }

void GameScript::ai_readymeleeweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiReadyMeleeWeapon());
  }

void GameScript::ai_readyrangedweapon(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiReadyRangedWeapon());
  }

void GameScript::ai_readyspell(std::shared_ptr<zenkit::INpc> npcRef, int spell, int mana) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && mana>0)
    npc->aiPush(AiQueue::aiReadySpell(spell,mana));
  }

void GameScript::ai_attack(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiAttack());
  }

void GameScript::ai_flee(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiFlee());
  }

void GameScript::ai_dodge(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiDodge());
  }

void GameScript::ai_unequipweapons(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiUnEquipWeapons());
  }

void GameScript::ai_unequiparmor(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiUnEquipArmor());
  }

void GameScript::ai_gotonpc(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::INpc> toRef) {
  auto to  = findNpc(toRef);
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiGoToNpc(to));
  }

void GameScript::ai_gotonextfp(std::shared_ptr<zenkit::INpc> npcRef, std::string_view to) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiGoToNextFp(to));
  }

void GameScript::ai_aligntofp(std::shared_ptr<zenkit::INpc> npcRef) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->aiPush(AiQueue::aiAlignToFp());
  }

void GameScript::ai_useitem(std::shared_ptr<zenkit::INpc> npcRef, int item) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->aiPush(AiQueue::aiUseItem(item));
  }

void GameScript::ai_useitemtostate(std::shared_ptr<zenkit::INpc> npcRef, int item, int state) {
  auto npc = findNpc(npcRef);
  if(npc)
    npc->aiPush(AiQueue::aiUseItemToState(item,state));
  }

void GameScript::ai_setnpcstostate(std::shared_ptr<zenkit::INpc> npcRef, int state, int radius) {
  auto npc = findNpc(npcRef);
  if(npc && state>0)
    npc->aiPush(AiQueue::aiSetNpcsToState(size_t(state),radius));
  }

void GameScript::ai_finishingmove(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::INpc> othRef) {
  auto oth = findNpc(othRef);
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && oth!=nullptr)
    npc->aiPush(AiQueue::aiFinishingMove(*oth));
  }

void GameScript::ai_takeitem(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::IItem> itmRef) {
  auto itm = findItem(itmRef.get());
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && itm!=nullptr)
    npc->aiPush(AiQueue::aiTakeItem(*itm));
  }

void GameScript::ai_gotoitem(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::IItem> itmRef) {
  auto itm = findItem(itmRef.get());
  auto npc = findNpc(npcRef);
  if(npc!=nullptr && itm!=nullptr)
    npc->aiPush(AiQueue::aiGotoItem(*itm));
  }

void GameScript::ai_pointat(std::shared_ptr<zenkit::INpc> npcRef, std::string_view waypoint) {
  auto npc = findNpc(npcRef);
  auto to  = world().findPoint(waypoint);
  if(npc!=nullptr && to!=nullptr)
    npc->aiPush(AiQueue::aiPointAt(*to));
  }

void GameScript::ai_pointatnpc(std::shared_ptr<zenkit::INpc> npcRef, std::shared_ptr<zenkit::INpc> otherRef) {
  auto other = findNpc(otherRef);
  auto npc   = findNpc(npcRef);
  if(npc!=nullptr && other!=nullptr)
    npc->aiPush(AiQueue::aiPointAtNpc(*other));
  }

int GameScript::ai_printscreen(std::string_view msg, int posx, int posy, std::string_view font, int timesec) {
  auto npc = findNpc(vm.global_self());
  if(npc==nullptr)
    npc = owner.player();
  if(npc==nullptr) {
    Gothic::inst().onPrintScreen(msg,posx,posy,timesec,Resources::font(font,Resources::FontType::Normal,1.0));
    return 0;
    }
  npc->aiPush(AiQueue::aiPrintScreen(timesec,font,posx,posy,msg));
  return 0;
  }

int GameScript::mob_hasitems(std::string_view tag, int item) {
  return int(world().hasItems(tag,uint32_t(item)));
  }

void GameScript::mob_createitems(std::string_view tag, int itemInstance, int amount) {
  if(itemInstance<=0 || amount<=0)
    return;
  auto* cls = vm.find_symbol_by_index(uint32_t(itemInstance));
  if(cls==nullptr || cls->type()!=zenkit::DaedalusDataType::INSTANCE)
    return;
  for(uint32_t i=0; auto* mob = world().mobsiById(i); ++i) {
    const auto name = mob->tag();
    const bool matches = name.size()==tag.size() && std::equal(name.begin(),name.end(),tag.begin(),[](char a, char b) {
      return std::toupper(static_cast<unsigned char>(a))==std::toupper(static_cast<unsigned char>(b));
      });
    if(mob->isContainer() && matches) {
      mob->inventory().addItem(size_t(itemInstance),size_t(amount),world());
      return;
      }
    }
  Log::d("Mob_CreateItems: container not found: ",tag);
  }

void GameScript::ta_min(std::shared_ptr<zenkit::INpc> npcRef, int start_h, int start_m, int stop_h, int stop_m, int action, std::string_view waypoint) {
  auto npc = findNpc(npcRef);
  if(npc!=nullptr)
    npc->addRoutine(gtime(start_h,start_m),gtime(stop_h,stop_m),uint32_t(action),waypoint);
  }

void GameScript::log_createtopic(std::string_view topicName, int section) {
  if(section==QuestLog::Mission || section==QuestLog::Note)
    quests.add(topicName,QuestLog::Section(section));
  }

void GameScript::log_settopicstatus(std::string_view topicName, int status) {
  if(status==int32_t(QuestLog::Status::Running) ||
     status==int32_t(QuestLog::Status::Success) ||
     status==int32_t(QuestLog::Status::Failed ) ||
     status==int32_t(QuestLog::Status::Obsolete))
    quests.setStatus(topicName,QuestLog::Status(status));
  }

void GameScript::logMoveToTop(std::string_view topic) {
  quests.moveToTop(topic);
  }

void GameScript::log_addentry(std::string_view topicName, std::string_view entry) {
  quests.addEntry(topicName,entry);
  }

void GameScript::equipitem(std::shared_ptr<zenkit::INpc> npcRef, int cls) {
  auto self = findNpc(npcRef);
  if(self!=nullptr) {
    if(self->itemCount(uint32_t(cls))==0)
      self->addItem(uint32_t(cls),1);
    self->useItem(uint32_t(cls),Item::NSLOT,true);
    }
  }

void GameScript::createinvitem(std::shared_ptr<zenkit::INpc> npcRef, int itemInstance) {
  if(itemInstance<=0)
    return; // an empty slot of a script, the original engine does nothing
  auto self = findNpc(npcRef);
  if(self!=nullptr) {
    Item* itm = self->addItem(uint32_t(itemInstance),1);
    storeItem(itm);
    }
  }

void GameScript::createinvitems(std::shared_ptr<zenkit::INpc> npcRef, int itemInstance, int amount) {
  if(itemInstance<=0)
    return;
  auto self = findNpc(npcRef);
  if(self!=nullptr && amount>0) {
    Item* itm = self->addItem(uint32_t(itemInstance),size_t(amount));
    storeItem(itm);
    }
  }

void GameScript::perc_setrange(int perc, int dist) {
  if(perc<0 || perc>=PERC_Count)
    return;
  perceptionRanges.range[perc] = dist;
  }

int GameScript::hlp_getinstanceid(std::shared_ptr<zenkit::DaedalusInstance> instance) {
  // Log::d("hlp_getinstanceid: name \"",handle.name,"\" not found");
  return instance == nullptr ? -1 : int(instance->symbol_index());
  }

int GameScript::hlp_random(int bound) {
  uint32_t mod = uint32_t(std::max(1,bound));
  return int32_t(randGen() % mod);
  }

bool GameScript::hlp_isvalidnpc(std::shared_ptr<zenkit::INpc> npcRef) {
  auto self = findNpc(npcRef);
  return self != nullptr;
  }

bool GameScript::hlp_isitem(std::shared_ptr<zenkit::IItem> itemRef, int instanceSymbol) {
  auto item = findItem(itemRef.get());
  if(item!=nullptr){
    auto& v = item->handle();
    return int(v.symbol_index()) == instanceSymbol;
    } else {
      return false;
    }
  }

bool GameScript::hlp_isvaliditem(std::shared_ptr<zenkit::IItem> itemRef) {
  auto item = findItem(itemRef.get());
  return item!=nullptr;
  }

std::shared_ptr<zenkit::INpc> GameScript::hlp_getnpc(int instanceSymbol) {
  auto npc = findNpcById(uint32_t(instanceSymbol));
  if(npc != nullptr)
    return npc->handlePtr();
  else
    return nullptr;
  }

void GameScript::info_addchoice(int infoInstance, std::string_view text, int func) {
  auto info = findInfo(uint32_t(infoInstance));
  if(info==nullptr)
    return;
  zenkit::IInfoChoice choice {};
  choice.text     = text;
  choice.function = func;
  info->add_choice(choice);
  }

void GameScript::info_clearchoices(int infoInstance) {
  auto info = findInfo(uint32_t(infoInstance));
  if(info==nullptr)
    return;
  info->choices.clear();
  }

bool GameScript::infomanager_hasfinished() {
  return !owner.isInDialog();
  }

void GameScript::snd_play(std::string_view fileS) {
  if(aiProcessPolicy>=NpcProcessPolicy::AiFar2)
    return;

  std::string file {fileS};
  for(auto& c:file)
    c = char(std::toupper(c));
  Gothic::inst().emitGlobalSound(file);
  }

void GameScript::snd_play3d(std::shared_ptr<zenkit::INpc> npcRef, std::string_view fileS) {
  if(aiProcessPolicy>=NpcProcessPolicy::AiFar2)
    return;

  std::string file {fileS};
  Npc*        npc  = findNpc(npcRef);
  if(npc==nullptr)
    return;
  for(auto& c:file)
    c = char(std::toupper(c));
  auto sfx = ::Sound(*owner.world(),::Sound::T_3D,file,npc->centerPosition(),0.f,false);
  sfx.play();
  }

void GameScript::exitsession() {
  if(DialogTest::active || DialogTest::guardExit) {
    Log::i("ExitSession of the scripts is ignored by the dialog test");
    return;
    }
  if(auto* pl = world().player())
    Log::i("ExitSession called by the script, the player is at ",pl->position().x," ",pl->position().y," ",pl->position().z);
  owner.exitSession();
  }

void GameScript::sort(std::vector<GameScript::DlgChoice> &dlg) {
  std::sort(dlg.begin(),dlg.end(),[](const GameScript::DlgChoice& l,const GameScript::DlgChoice& r){
    return std::tie(l.sort,l.scriptFn)<std::tie(r.sort,r.scriptFn); // small hack with scriptfn to reproduce behavior of original game
    });
  }

void GameScript::setNpcInfoKnown(const zenkit::INpc& npc, const zenkit::IInfo& info) {
  auto id = std::make_pair(vm.find_symbol_by_instance(npc)->index(),vm.find_symbol_by_instance(info)->index());
  dlgKnownInfos.insert(id);
  }

bool GameScript::setInfoKnown(Npc& npc, std::string_view infoName, bool known) {
  auto* sym = vm.find_symbol_by_name(infoName);
  if(sym==nullptr || !sym->is_instance_of<zenkit::IInfo>())
    return false;
  const auto key = std::make_pair(size_t(npc.instanceSymbol()),size_t(sym->index()));
  if(known)
    dlgKnownInfos.insert(key); else
    dlgKnownInfos.erase(key);
  return true;
  }

bool GameScript::isInfoTold(size_t infoInstance) {
  auto* sym = vm.find_symbol_by_index(uint32_t(infoInstance));
  if(sym==nullptr || !sym->is_instance_of<zenkit::IInfo>())
    return false;
  auto info = std::dynamic_pointer_cast<zenkit::IInfo>(sym->get_instance());
  return info!=nullptr && dlgKnownInfos.find(std::make_pair(size_t(info->npc),infoInstance))!=dlgKnownInfos.end();
  }

void GameScript::setInfoTold(size_t infoInstance, bool told) {
  auto* sym = vm.find_symbol_by_index(uint32_t(infoInstance));
  if(sym==nullptr || !sym->is_instance_of<zenkit::IInfo>())
    return;
  auto info = std::dynamic_pointer_cast<zenkit::IInfo>(sym->get_instance());
  if(info==nullptr)
    return;
  const auto key = std::make_pair(size_t(info->npc),infoInstance);
  if(told)
    dlgKnownInfos.insert(key); else
    dlgKnownInfos.erase(key);
  }

bool GameScript::npcKnowsInfo(Npc& npc, size_t infoInstance) const {
  return doesNpcKnowInfo(npc.handle(),infoInstance);
  }

DialogTest& GameScript::dialogTest() {
  if(dlgTest==nullptr)
    dlgTest.reset(new DialogTest(*this));
  return *dlgTest;
  }

bool GameScript::doesNpcKnowInfo(const zenkit::INpc& npc, size_t infoInstance) const {
  auto id = std::make_pair(vm.find_symbol_by_instance(npc)->index(),infoInstance);
  return dlgKnownInfos.find(id)!=dlgKnownInfos.end();
  }
