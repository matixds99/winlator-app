#pragma once

// Returning New Balance: the options of the menu of its Union plugin (UNION_ABI.DLL), read out of the plugin.
// zCOption::ReadInt of the engine writes the default value into Gothic.ini when the key is not there, and the
// plugin reads all of them at the start, so the scripts (Menu_ReadInt) always see these defaults.

#include <string>
#include <string_view>
#include <cstdint>

#include "gothic.h"

namespace NbOptions {

struct Default {
  const char* section;
  const char* key;
  int32_t     value;
  };

// the options of the plugin with their default values (only its own sections: [GAME], [ENGINE]...
// are the options of the engine, OpenGothic has its own defaults for them)
inline constexpr Default defaults[] = {
  // NB fix 44: read by the plugin outside of its menu (zCOption::ReadInt with the default 1, 0x10280A31); without it
  // RX_ApplyAni_1H_DexSword gives the weapons of dexterity (sabres) the animations of the usual swords
  {"AST", "IsEnabledDexSwordAnims", 1},
  // NB fix 49: read by the scripts, written by the plugin with these defaults (zCOption::ReadInt) - the difficulty of
  // a new game, chosen in a window of the plugin that OpenGothic does not have. Without them: 0 learning points for
  // a level (bOptionLP), monsters with 25% of the life (bOptionMonsterHealth 0)...
  {"AST", "bOptionLP", 10},
  {"AST", "bOptionHealth", 3},
  {"AST", "bOptionMonsterExp", 2},
  {"AST", "bOptionMonsterHealth", 3},
  {"AST", "bOptionMonsterPower", 2},
  {"AST", "bOptionMonsterProt", 2},
  {"AST", "FULLSNC_MENU_OPTION", 19},
  {"AST", "bToggleQuickLootMessage", 1},
  {"AST", "bToggleDamageAboveHead", 1},
  {"AST", "bToggleDamageShow", 0},
  {"AST", "bToggleShieldBlockShow", 1},
  {"AST", "iMsgPositionX", 0},
  {"AST", "bTogglePetDamageShow", 0},
  {"AST", "bToggleHealAboveHead", 0},
  {"AST", "bShowEnemyHealth", 1},
  {"AST", "bShowKarma", 1},
  {"AST", "bHFTStatus", 1},
  {"AST", "bWatchTime", 1},
  {"AST", "bToggleFPS", 1},
  {"AST", "bShowBuffInfo", 1},
  {"AST", "bShowMonstersSkillEffect", 1},
  {"AST", "bShowNotice", 1},
  {"AST", "bSetColorNames", 1},
  {"AST", "bToggleShowCanSteal", 1},
  {"AST", "bShowDeadScreen", 1},
  {"AST", "bHealthAboveTarget", 1},
  {"AST", "bToggleManaAboveHead", 0},
  {"AST", "bShowDamagePlayerGot", 0},
  {"AST", "bHideStatusBars", 0},
  {"AST", "enableSaveReminder", 1},
  {"AST", "bScaleStr", 0},
  {"AST", "bShowSaveSlotText", 1},
  {"AST", "bShowPocket", 0},
  {"AST", "bShowBag", 0},
  {"AST", "bSkipLogo", 0},
  {"AST", "bFistBattleTip", 0},
  {"AST", "bToggle_SightColorAimBySpellLevel", 1},
  {"AST", "bToggleSpellHint", 0},
  {"AST", "bShowArrowBloodEffect", 0},
  {"AST", "bShowPlatoSandStorm", 0},
  {"AST", "bEnableDPSCounter", 0},
  {"AST", "bHidePotionSmokeEffect", 0},
  {"AST", "bSeparatePoisonDam", 0},
  {"AST", "bShowMesMemoryFull", 1},
  {"AST", "minSlotSL", 10},
  {"AST", "maxSlotSL", 15},
  {"AST", "bUseOldSavegameMenu", 0},
  {"AST", "keyDelayMenuPress", 300},
  {"AST", "bMinimalHealShow", 5},
  {"AST", "bToggleSeparateHealManaNumbers", 0},
  {"AST", "bToggleSeparateManaNumbers", 0},
  {"AST", "bHideWaterMageIceShield", 0},
  {"AST", "bShowSummonHealthBar", 1},
  {"AST", "bShowSummonHealthBarPosX", -1},
  {"AST", "bShowSummonHealthBarPosY", -1},
  {"AST", "bShowBleedMeleeProcBloodEffect", 1},
  {"AST", "bAnimateMessages", 0},
  {"AST", "bDeathCounterShow", 1},
  {"AST", "bHideBarsText", 0},
  {"AST", "bHideBarsTextValues", 0},
  {"AST", "bShowEffectPalUndeadProc", 1},
  {"AST", "bHeroStatusFoodX", 15},
  {"AST", "bHeroStatusFoodY", 1},
  {"AST", "fGallahadEffectLerp", 100},
  {"AST", "bShowMagicIceAddEffects", 1},
  {"AST", "bShowPoisonHero", 1},
  {"AST", "bWindFire", 1},
  {"AST", "bShowDiaryNumbers", 1},
  {"AST", "bShowHelpTextForHotbars", 1},
  {"AST", "pInvWithCategory", 0},
  {"AST", "bShowInvRender", 1},
  {"AST", "bShowNewItems", 1},
  {"AST", "bInvShowQuestItems", 1},
  {"AST", "bToggleInventoryLabels", 0},
  {"AST", "bNewInvView", 0},
  {"AST", "bReleaseMouseWindows", 1},
  {"AST", "bCheckSavegameBadFiles", 1},
  {"AST", "bMakeIniCopies", 1},
  {"AST", "bShowStaminaWhenInvOpen", 0},
  {"AST", "itemDescrSizeValue", 1},
  {"AST", "bToggleHotbar", 1},
  {"AST", "hotBarCellSize", 50},
  {"AST", "bToggleSpellBar", 1},
  {"AST", "bToggleHotbarSets", 0},
  {"AST", "bQuickLootEnabled", 1},
  {"AST", "bCanRenderHotbarInBattle", 1},
  {"AST", "bToggleHardBosses", 1},
  {"AST", "bCanSaveLeg", 1},
  {"AST", "AutoAIM", 0},
  {"AST", "aimLastTextureSize", 0},
  {"AST", "aimLastTextureId", 0},
  {"AST", "bToggleNontargetMagic", 1},
  {"AST", "ntmLastCursorId", 0},
  {"AST", "ntmLastTextureSize", 0},
  {"AST", "bTogglePartySprint", 0},
  {"AST", "bToggleDynamicNpcFocus", 0},
  {"AST", "bHideBadFocus", 0},
  {"AST", "bStrafeNFocusMelee", 1},
  {"AST", "bStrafeNFocusBow", 1},
  {"AST", "bStrafeNFocusMag", 1},
  {"AST", "bRemoveEmptyBodys", 0},
  {"AST", "bSafeHumanBodies", 0},
  {"AST", "bWispLootReaction", 0},
  {"AST", "bNewStrafe", 0},
  {"AST", "bToggleBossMusic", 1},
  {"AST", "bToggleMusicAnimalBoss", 0},
  {"AST", "bToggleHeroVoice", 1},
  {"AST", "bGlobalStaggerEnabled", 1},
  {"AST", "bShowStaggerVisual", 0},
  {"AST", "bShowPoisonBarVisual", 1},
  {"AST", "bFastSmoking", 0},
  {"AST", "bDualDepends", 0},
  {"AST", "bToggleWorldTimeAcceleration", 0},
  {"AST", "bG1ComboEnable", 0},
  {"AST", "bDontForceRotateHero", 0},
  {"AST", "FreezeHeroSpeedNormal", 0},
  {"AST", "bToggleLockPickHelper", 1},
  {"AST", "messagesMaxCountOnScreen", 7},
  {"AST", "bKillZSpyDebug", 0},
  {"AST", "bHidePocketsNpc", 1},
  {"AST", "noGrassGrassVal", 10},
  {"AST", "noGrassBushVal", 10},
  {"RENDERER_D3D", "zFogRadial", 1},
  {"BELIARS_RAGE", "RX_BeliarRage_Opt_MonstersExp", 70},
  {"BELIARS_RAGE", "RX_BeliarRage_Opt_SpawnMonsters", 1},
  {"BELIARS_RAGE", "RX_BeliarRage_Opt_HardCurse", 0},
  {"BELIARS_RAGE", "RX_BeliarRage_Opt_StaminaReduce", 1},
  {"BELIARS_RAGE", "RX_BeliarRage_Opt_AddDamageReduce", 1},
  {"BELIARS_RAGE", "RX_BeliarRage_Opt_RegenEnemies", 1},
  {"BELIARS_RAGE", "bSoundActive", 1},
  {"BELIARS_RAGE", "RX_BeliarRage_ShowVisualPos", 1},
  };

struct Option {
  const char* name;     // RX_MENU_TITLE_STR_<name> in GOTHIC.DAT
  const char* section;
  const char* key;
  int         category; // RX_MENU_CAT_<category>
  };

// the options shown in OpenGothic: the switches that the scripts themselves read (Menu_ReadInt),
// the rest of the menu of the plugin changes things that only the plugin does
// (not here: AutoAIM - the scripts set it back to 0 themselves; bShowStaminaWhenInvOpen - the inventory of the plugin)
inline constexpr Option shown[] = {
  {"SHOW_GODS_KARMA", "AST", "bShowKarma", 0},
  {"SHOW_HUNGER", "AST", "bHFTStatus", 0},
  {"SHOW_PLACES_NAMES", "AST", "bShowNotice", 0},
  {"SHOW_HERO_POCKET", "AST", "bShowPocket", 0},
  {"SHOW_HERO_BAG", "AST", "bShowBag", 0},
  {"SHOW_SKIP_LOGO", "AST", "bSkipLogo", 0},
  {"SHOW_SCALE_NPC", "AST", "bScaleStr", 0},
  {"SHOW_DEATH_SCREEN", "AST", "bShowDeadScreen", 0},
  {"SHOW_HIDE_NPC_POCKETS", "AST", "bHidePocketsNpc", 0},
  {"SHOW_PAL_UNDEAD_PROC", "AST", "bShowEffectPalUndeadProc", 0},
  {"SHOW_POISON_HERO", "AST", "bShowPoisonHero", 0},
  {"GAMEPLAY_HARDBOSSES", "AST", "bToggleHardBosses", 2},
  {"GAMEPLAY_SAVES", "AST", "bCanSaveLeg", 2},
  {"GAMEPLAY_HERO_REACTIONS", "AST", "bToggleHeroVoice", 2},
  {"BR_SPAWN_MONSTERS", "BELIARS_RAGE", "RX_BeliarRage_Opt_SpawnMonsters", 4},
  {"BR_HARD_CURSE", "BELIARS_RAGE", "RX_BeliarRage_Opt_HardCurse", 4},
  {"BR_STAMINA_REDUCE", "BELIARS_RAGE", "RX_BeliarRage_Opt_StaminaReduce", 4},
  {"BR_DAMAGE_REDUCE", "BELIARS_RAGE", "RX_BeliarRage_Opt_AddDamageReduce", 4},
  {"BR_REGEN_ENEMIES", "BELIARS_RAGE", "RX_BeliarRage_Opt_RegenEnemies", 4},
  };

/// writes the default value of every option that is not in Gothic.ini yet (as zCOption::ReadInt does)
inline void ensureDefaults() {
  for(auto& d:defaults) {
    if(Gothic::settingsGetS(d.section,d.key).empty())
      Gothic::settingsSetI(d.section,d.key,d.value);
    }
  }

}
