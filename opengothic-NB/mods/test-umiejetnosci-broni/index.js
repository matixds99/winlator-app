// Test umiejętności broni i atrybutów - zakładka w menu modów.
// Wersja dla New Balance (2.0.0): NB wybiera animacje broni według PROCENTU trafienia (C_NPC.HITCHANCE),
// a nie według poziomu talentu. Progi skryptów NB (RX_ApplyAnim_Weapons):
//   < 30%  amator   (szpada: Humans_Pirate,     miecz: bez nakładki)
//   >= 30% poziom 1 (szpada: Humans_Rapier_ST1, miecz: humans_1hST1, szabla: humans_1hDexST1)
//   >= 60% poziom 2 (szpada: Humans_Rapier_ST2, miecz: humans_1hST2, szabla: humans_1hDexST3)
//   >= 90% mistrz   (szpada: Humans_Rapier_ST3, miecz: humans_1hST3, szabla: humans_1hDexST2)
// Dlatego procent zmienia się tak jak u nauczyciela: B_RaiseFightTalent (podnosi % "prawdziwy" w AIVAR 84..87
// i % trafienia, ustawia poziom talentu i RX_LastAnim), a potem skrypt NB sam zakłada właściwe animacje.
// Poprzednia wersja zmieniała tylko poziom talentu (Npc_SetTalentSkill) i zakładała zwykłe nakładki
// HUMANS_1HST1/ST2 - mieszały się z animacjami szpady/szabli NB.

const WEAPONS = [
  { id: 1, name: "Broń jednoręczna (1H)" },
  { id: 2, name: "Broń dwuręczna (2H)" },
  { id: 3, name: "Łuk" },
  { id: 4, name: "Kusza" },
];
// docelowe procenty przycisków poziomów
const TIERS = [
  { name: "Amator",  percent: 10 },
  { name: "30%",     percent: 30 },
  { name: "60%",     percent: 60 },
  { name: "Mistrz",  percent: 90 },
];

let lastMessage = "";
let lastError = "";
let percentStep = 10;

function call(name, args) {
  const r = debug.call(name, args);
  if (!r || !r.ok) {
    lastError = `${name}: ${r && r.error ? r.error : "nieznany błąd"}`;
    return null;
  }
  lastError = "";
  return r.result;
}

// "prawdziwy" procent NB (bez premii z broni): AIVAR 84 1H, 85 2H, 86 łuk, 87 kusza
function percentOf(w) {
  const v = player.aiVar(83 + w.id);
  return Number.isFinite(v) ? v : null;
}

function tierName(p) {
  if (p === null) return "?";
  if (p >= 90) return "mistrz";
  if (p >= 60) return "poziom 2";
  if (p >= 30) return "poziom 1";
  return "amator";
}

// NB zakłada animacje broni bohatera sama (RX_ApplyAnim_Weapons). -1: według poziomu zapisanego przez
// B_AddFightSkill (RX_LastAnim), więc od razu, bez zdejmowania broni.
function refreshAnims() {
  call("RX_ApplyAnim_Weapons", [player, -1]);
}

function addPercent(w, delta) {
  if (!delta) return;
  call("B_RaiseFightTalent", [player, w.id, delta]);
  if (lastError) return;
  refreshAnims();
  const p = percentOf(w);
  lastMessage = `${w.name}: ${delta > 0 ? "+" : ""}${delta}% (teraz ${p}%, ${tierName(p)})`;
}

function setPercent(w, target) {
  const cur = percentOf(w);
  if (cur === null) { lastError = `${w.name}: nie da się odczytać procentu`; return; }
  addPercent(w, Math.max(0, Math.min(100, target)) - cur);
}

// --- atrybuty
// Wytrzymałość NB: ATR_STAMINA_MAX trzyma maksimum w jednostkach x1, a gra pokazuje je x10 (pasek, statystyki,
// wymagania broni). Tutaj wszystko jest w jednostkach gry (x10): 100 = ATR_STAMINA_MAX 10.
const ATTRIBUTES = [
  { id: 4, name: "Siła",       read: () => player.strength,  steps: [-10, -1, 1, 10] },
  { id: 5, name: "Zręczność",  read: () => player.dexterity, steps: [-10, -1, 1, 10] },
  { id: "stamina", name: "Wytrzymałość (maks.)", stamina: true,
    read: () => { const m = symValue("ATR_STAMINA_MAX"); return m === null ? null : m * 10; },
    steps: [-100, -10, 10, 100] },
];

function symValue(name) {
  const r = debug.symbol(name);
  if (!r || r.value === undefined || r.value === null) return null;
  const v = Array.isArray(r.value) ? r.value[0] : r.value;
  return Number(v);
}

function setSym(name, value) {
  debug.setSymbol(name, value);
  const now = symValue(name);
  if (now !== Number(value)) lastError = `${name}: nie udało się ustawić (jest ${now})`;
  else lastError = "";
}

function fillStamina() {
  const max = symValue("ATR_STAMINA_MAX");
  if (max === null) { lastError = "Brak ATR_STAMINA_MAX w skryptach"; return; }
  setSym("ATR_STAMINA", max * 10);
}
let attrInput = {};        // tekst w polach "Ustaw"

function attrValue(a) {
  const v = Number(a.read());
  return Number.isFinite(v) ? v : null;
}

function changeAttr(a, delta) {
  if (!delta) return;
  if (a.stamina) {
    const cur = symValue("ATR_STAMINA_MAX");
    if (cur === null) { lastError = `${a.name}: brak zmiennej ATR_STAMINA_MAX`; return; }
    setSym("ATR_STAMINA_MAX", Math.max(0, cur + Math.round(delta / 10)));
    if (!lastError) {
      fillStamina();
      lastMessage = `${a.name}: ${delta > 0 ? "+" : ""}${delta} (teraz ${attrValue(a)})`;
    }
    return;
  }
  call("Npc_ChangeAttribute", [player, a.id, delta]);
  if (!lastError) lastMessage = `${a.name}: ${delta > 0 ? "+" : ""}${delta} (teraz ${attrValue(a)})`;
}

function setAttr(a, target) {
  const cur = attrValue(a);
  if (cur === null || !Number.isFinite(target)) {
    lastError = `${a.name}: podaj liczbę`;
    return;
  }
  changeAttr(a, Math.max(0, Math.round(target)) - cur);
}

ui.tab("Umiejętności broni", () => {
  ui.textWrapped("New Balance: animacje broni zależą od procentu trafienia (progi 30 / 60 / 90%). " +
                 "Zmiana działa jak u nauczyciela, skrypt NB od razu zakłada animacje (szpada, szabla, miecz, tarcza...).");
  ui.separator();

  for (const w of WEAPONS) {
    ui.withId("w" + w.id, () => {
      const p = percentOf(w);
      ui.text(w.name);
      ui.sameLine();
      ui.textColored("#D9D026", `  teraz: ${p === null ? "?" : p + "%"} (${tierName(p)})`);

      for (let i = 0; i < TIERS.length; ++i) {
        if (i > 0) ui.sameLine();
        const t = TIERS[i];
        const active = p !== null && tierName(p) === tierName(t.percent);
        if (ui.button((active ? "> " : "") + t.name, 0, ui.fontSize() * 2.0)) setPercent(w, t.percent);
      }

      ui.sameLine();
      if (ui.button("-" + percentStep + "%", 0, ui.fontSize() * 2.0)) addPercent(w, -percentStep);
      ui.sameLine();
      if (ui.button("+" + percentStep + "%", 0, ui.fontSize() * 2.0)) addPercent(w, percentStep);

      ui.spacing();
    });
  }

  ui.separator();
  ui.text("Atrybuty");
  for (const a of ATTRIBUTES) {
    ui.withId("a" + a.id, () => {
      const cur = attrValue(a);
      ui.text(a.name);
      ui.sameLine();
      ui.textColored("#D9D026", "  teraz: " + (cur === null ? "?" : cur));
      if (a.stamina) {
        const now = symValue("ATR_STAMINA");
        ui.sameLine();
        ui.textDisabled(now === null || !cur ? "" : `  (pasek: ${now}/${cur})`);
      }
      for (let i = 0; i < a.steps.length; ++i) {
        const d = a.steps[i];
        if (i > 0) ui.sameLine();
        if (ui.button((d > 0 ? "+" : "") + d, 0, ui.fontSize() * 2.0)) changeAttr(a, d);
      }
      ui.sameLine();
      ui.setNextWidth(ui.fontSize() * 5);
      attrInput[a.id] = ui.inputText("##val", attrInput[a.id] ?? "", "decimal");
      ui.sameLine();
      if (ui.button("Ustaw", 0, ui.fontSize() * 2.0)) setAttr(a, Number(attrInput[a.id]));
      ui.spacing();
    });
  }
  if (ui.button("Pełne życie, mana i wytrzymałość", 0, ui.fontSize() * 2.0)) {
    player.setHealth(player.maxHealth);
    player.setMana(player.maxMana);
    fillStamina();
    if (!lastError) lastMessage = "Życie, mana i wytrzymałość uzupełnione";
  }
  ui.sameLine();
  if (ui.button("Pełna wytrzymałość", 0, ui.fontSize() * 2.0)) {
    fillStamina();
    if (!lastError) lastMessage = "Wytrzymałość uzupełniona";
  }

  ui.separator();
  const step = ui.sliderInt("Krok % trafienia", percentStep, 1, 50);
  if (typeof step === "number") percentStep = step;

  if (ui.button("Wszystko: Amator", 0, ui.fontSize() * 2.0)) for (const w of WEAPONS) setPercent(w, 10);
  ui.sameLine();
  if (ui.button("Wszystko: Mistrz", 0, ui.fontSize() * 2.0)) for (const w of WEAPONS) setPercent(w, 90);

  if (lastMessage) ui.text(lastMessage);
  if (lastError) ui.textColored("#FF5050", "Błąd: " + lastError);
});

events.on("shutdown", () => { ui.removeTab("Umiejętności broni"); });

// komendy konsoli
const TALENT = { "1h": 1, "2h": 2, "bow": 3, "cbow": 4 };
console.registerCommand("tskill", ({ args }) => {
  // tskill <1h|2h|bow|cbow> <procent>: ustawia procent (dawniej poziom 0/1/2 - teraz 0 = 10%, 1 = 30%, 2 = 90%)
  const w = WEAPONS.find(x => x.id === TALENT[args[0]]);
  let v = Number(args[1] ?? 0);
  if (v === 0) v = 10; else if (v === 1) v = 30; else if (v === 2) v = 90;
  if (w) setPercent(w, v);
  console.log(lastError || lastMessage);
}, "tskill <1h|2h|bow|cbow> <procent>");
console.registerCommand("addskill", ({ args }) => {
  const w = WEAPONS.find(x => x.id === TALENT[args[0]]);
  if (w) addPercent(w, Number(args[1] ?? 10));
  console.log(lastError || lastMessage);
}, "addskill <1h|2h|bow|cbow> <procent>");
