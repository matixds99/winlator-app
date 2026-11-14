# Log poprawek – OpenGothic + Gothic II: New Balance

Bazą są pliki z repo `borsuczyna/opengothic-android` (wersja z 31.10.2026, commit `31b8115`).
Każda poprawka ma swój numer. Nowe dopisuję na końcu listy.

Status:
- ✅ sprawdzone w grze
- 🧪 czeka na test

Pliki: przy każdej poprawce jest lista plików. **🆕 NOWY PLIK** oznacza plik, którego nie ma w projekcie borsuczyny i który trzeba dodać. Pozostałe pliki się podmienia. Pełna lista jest w [README.md](README.md).

---

### ✅ Sprawdzone też w grze (12.11.2026): pulpity z księgami dają inteligencję (5 → 6), stół demonologiczny otwiera menu.

### 0. Obsługa skryptów New Balance (Copilot, przed tym logiem) – ✅
- **Pliki:** `common/game/gamescript.cpp`, `common/world/objects/npc.cpp`
- **Co:** zastępniki funkcji `RX_*` z pluginu Union (zmienne NB postaci, maski, teksty, losowanie, wywołania zwrotne, cykl `RX_Main_CycleTrigger` co sekundę). Dzięki nim działają teksty i ticki.
- **Zmienne NB postaci (`npc.cpp`):** `RX_GETNPCVAR`/`RX_SETNPCVAR`, zapisywane też w save'ach.

### 0a. AI potworów – potwory nie atakowały – ✅
- **Plik:** `common/world/objects/npc.cpp`
- **Objaw:** wilki, gobliny i inne potwory nie reagowały na bohatera i nie atakowały.
- **Przyczyna:** skrypt NB `ZS_MM_AllScheduler` robi `AI_RemoveWeapon` + `AI_StartState`. Potwory nie mają animacji chowania broni (wilk w trybie pięści, goblin z pałką), więc `AI_RemoveWeapon` wisiał w kolejce AI w nieskończoność i blokował wszystko po nim. Potwór nie dostawał stanu ani percepcji (`percEnemy=0`), więc `PERC_ASSESSENEMY` → `B_MM_AssessEnemy` nigdy się nie uruchamiało.
- **Poprawka:** u potworów `AI_RemoveWeapon` chowa broń od razu, bez animacji (tak jak `drawWeaponFist` wyciąga ją bez animacji). Kolejka się odblokowuje, a atak dalej idzie przez skrypty NB. Dzięki temu działają wyjątki z NB: przywołańcy i drużyna nie atakują bohatera, potwory z questów czekają na swój etap, wilki najpierw warczą, owce uciekają.
- **Diagnostyka:** w logu są linie `NB ai: ...` (stan, kolejka, percepcje potwora przy bohaterze). Można je wyłączyć przez `NB_AI_TRACE = false`.

### 1. Czarny ekran w scenie z Xardasem – ✅ (11.11.2026)
- **Pliki:** `common/world/triggers/cscamera.cpp`, `common/world/triggers/cscamera.h`
- **Objaw:** po pierwszym dialogu z Xardasem ekran robił się czarny i zostawały same napisy („Otrzymano parę zwojów…”).
- **Przyczyna:** kamera filmowa `EVT_START_CAM` miała błędne czasy klatek kluczowych (oryginalny silnik przelicza je sam, OpenGothic nie). Wychodziło dzielenie przez zero, kamera dostawała pozycję NaN i nie widziała nic.
- **Poprawka:** błędne czasy są rozkładane po trasie kamery, dodane zabezpieczenia przed dzieleniem przez zero. Jeśli kamera mimo to dostanie złą pozycję, scena jest przerywana i wraca zwykła kamera.

### 2. Stoły rzemieślnicze nic nie robiły – ✅ alchemia, runy, demonologia, tabliczki (12.11.2026)
- **Plik:** `common/game/gamescript.cpp` (funkcja `RX_OPENCRAFT` w bloku NB)
- **Objaw:** stół alchemiczny, runiczny itd. nie otwierał żadnego menu.
- **Przyczyna:** NB otwiera stoły przez `RX_OpenCraft(...)`, czyli graficzne okno z pluginu Union, którego OpenGothic nie ma.
- **Poprawka:** `RX_OpenCraft` otwiera stare menu dialogowe, które nadal jest w skryptach NB (jak `AI_ProcessInfos(hero)` w Gothicu 2). Przepisy i składniki pochodzą ze skryptów moda.
- **Sprawdzone:** stół alchemiczny, runiczny (kamienie runiczne), demonologiczny i stół z tabliczkami. **Do sprawdzenia:** zwoje, kowadło, piec. Kuchnia, łuki, biżuteria i runy demoniczne mogą nie mieć starego menu.

### 3. Biały ekran wczytywania – ✅ (12.11.2026, poprawione 2x)
- **Pliki:** `common/gothic.cpp`, `common/mainwindow.cpp`
- **Objaw:** przy wczytywaniu białe tło, na nim tylko pasek postępu.
- **Przyczyna:** `LOADING.TGA` w NB to tylko malutka biała zaślepka. Plugin Union losuje prawdziwe obrazki z `/_work/data/textures/_compiled/` według wzoru `LOADING_<ŚWIAT>_<numer>` (sprawdzone w kodzie pluginu). Pierwsza wersja poprawki szukała złego wzoru (`LOADING_<numer>`), dlatego nadal było białe tło.
- **Poprawka:** losowany jest obrazek `LOADING_<ŚWIAT>_<numer>`. Przy zmianie świata brany jest obrazek tego świata, przy wczytywaniu save'a dowolny. Zaślepki mniejsze niż 64×64 są pomijane, a gdy nie ma żadnego obrazka, tło jest czarne zamiast białego.

### 3a. Ekran wczytywania pokazywał obrazek innego świata – ✅ (12.11.2026)
- **Pliki:** `common/gothic.cpp`, `common/mainwindow.cpp`
- **Objaw:** przy wczytywaniu save'a albo teleporcie do Khorinis pokazywał się Jarkendar, Pustynia Adanosa itd.
- **Przyczyna:** przy wczytywaniu save'a OpenGothic zawsze prosi o `LOADING.TGA`, nie podając świata, więc losowany był obrazek z dowolnego świata. Gdy dla danego świata nie było obrazka, poprawka 3 też brała losowy z innego świata.
- **Poprawka:** przy wczytywaniu save'a świat jest odczytywany z nagłówka save'a i prosi się o `LOADING_<ŚWIAT>.TGA`, tak jak przy zmianie świata. Kolejność wyboru: losowy obrazek tego świata → obrazek tego świata bez numeru → losowy z dowolnego świata. W logu jest linia `loading screen: ...` z nazwą świata i wybranym obrazkiem.

### 4. Opcje moda z Gothic.ini nie działały – ✅ (12.11.2026, sprawdzone na bSkipLogo=1)
- **Plik:** `common/game/gamescript.cpp` (`MENU_READINT`, `MENU_WRITEINT` w bloku NB)
- **Objaw:** skrypty NB czytają swoje opcje z `Gothic.ini`, sekcja `[AST]` (np. `bSkipLogo`, `bCanSaveGame`, `bOptionHealth`, `bOptionMonsterPower`, `bShowKarma`, ok. 70 opcji). Każda zwracała 0, a zapisy przepadały.
- **Przyczyna:** `Menu_ReadInt` / `Menu_WriteInt` dostarcza plugin Union, a OpenGothic ich nie miał.
- **Poprawka:** obie funkcje czytają i zapisują ustawienia OpenGothic (`Gothic.ini`). Opcje, które sprawdzają same skrypty NB, działają po wpisaniu ich do `[AST]`, np. `bSkipLogo=1` pomija intro.

### 5. Puste menu „Ustawienia gry” – ✅ (12.11.2026)
- **Pliki:** `common/ui/gamemenu.cpp`, `common/game/gamescript.cpp`, `common/game/nboptions.h` 🆕 **NOWY PLIK**
- ⚠️ **`nboptions.h` to NOWY PLIK.** Trzeba go dodać do `common/game/`, obok `gamescript.cpp`. Bez niego build się nie uda.
- **Objaw:** pierwsza kategoria w ustawieniach („Ustawienia gry”) była pusta.
- **Przyczyna:** w `MENU.DAT` moda ta strona to pusta rama (`MENU_OPT_CONTROLS_NEWOPTIONS`), wypełnia ją plugin Union. Z kodu pluginu odczytałem pełną listę: ok. 140 opcji, ich klucze w `Gothic.ini`, typy i wartości domyślne.
- **Poprawka:**
  - strona pokazuje 19 przełączników, które czytają same skrypty NB. Wśród nich: pomijanie prologu, zapis w dowolnym miejscu, trudniejsi bossowie, okrzyki bohatera, karma bogów, sakiewka i torba na bohaterze, ekran śmierci oraz opcje Gniewu Beliara. Nazwy i „tak/nie” są brane ze skryptów gry, więc są po polsku. Reszty opcji pluginu nie pokazuję, bo sterują funkcjami, które ma tylko Union.
  - wartości domyślne opcji NB (sekcje `[AST]` i `[BELIARS_RAGE]`) są dopisywane do `Gothic.ini`, jeśli ich tam nie ma, tak jak robi to silnik z Unionem. Przykład: `bCanSaveLeg=1` (zapis w dowolnym miejscu) i `bToggleHardBosses=1` są domyślnie włączone, a wcześniej skrypty widziały 0.
- **Co robi każdy przełącznik** (sprawdzone w skryptach NB):
  | Opcja | Co robi skrypt | Działa w OpenGothic? |
  |---|---|---|
  | Pomiń ekran powitalny (`bSkipLogo`) | pomija prolog | ✅ sprawdzone |
  | Status karmy u bogów (`bShowKarma`) | wypisuje karmę Innosa/Beliara | ✅ powinno (zwykłe teksty) |
  | Głód/pragnienie/zmęczenie (`bHFTStatus`) | wypisuje potrzeby bohatera | ✅ powinno (zwykłe teksty) |
  | Nazwy lokacji (`bShowNotice`) | tablice z nazwami miejsc | ✅ powinno |
  | Skalowanie postaci od siły (`bScaleStr`) | `Mdl_SetModelScale` bohatera | ✅ powinno |
  | Ekran śmierci (`bShowDeadScreen`) | dźwięk i ekran po śmierci | ✅ powinno |
  | Efekt zatrucia (`bShowPoisonHero`) | efekt `SPELLFX_UNDEAD_SCREENBLEND` | ✅ powinno |
  | Efekt ataków Paladyna (`bShowEffectPalUndeadProc`) | efekt przy obrażeniach nieumarłym | ✅ powinno |
  | Trudniejsi bossowie (`bToggleHardBosses`) | umiejętności bossów w AI | ✅ powinno |
  | Okrzyki bohatera (`bToggleHeroVoice`) | dźwięki bohatera w walce | ✅ powinno |
  | Gniew Beliara (5 opcji) | zmienne trybu Gniewu Beliara | ✅ powinno (tylko w tym trybie) |
  | Zapis w dowolnym miejscu (`bCanSaveLeg`) | blokada zapisu w NB | ❓ OpenGothic chyba w ogóle nie blokuje zapisu |
  | Ukryj sakiewki NPC (`bHidePocketsNpc`) | sakiewki na postaciach | ❓ zależy od slotów, patrz niżej |
  | Mieszek / torba bohatera (`bShowPocket`, `bShowBag`) | `EXT_PutInSlot` / `ActivateZSSlot` | ❌ tych funkcji Uniona jeszcze nie ma |
  - Usunięte ze strony: `AutoAIM` (skrypty same ustawiają go z powrotem na 0) i „Wytrzymałość przy otwartym ekwipunku” (to funkcja ekwipunku Uniona).

### 6. Brak wpisów do dziennika i zdarzeń w świecie (np. kamień runiczny przy wieży Xardasa) – ✅ (wpis w dzienniku jest) (12.11.2026)
- **Plik:** `common/game/gamescript.cpp` (pętla NB w `GameScript::tick`)
- **Objaw:** przy kamieniu runicznym koło wieży Xardasa nie pojawia się wpis „Kamienie runiczne”.
- **Przyczyna:** ten wpis (i wiele innych zdarzeń w świecie) daje skrypt `B_GetTime`. W NB woła go plugin Union po nazwie, a nie skrypty, więc w OpenGothic nie wołał go nikt. Z kodu pluginu: timer co 1000 ms woła na zmianę `RX_Main_CycleTrigger` i w następnej sekundzie `b_gettime`, a `PerFrame` jest wołany co klatkę.
- **Poprawka:** pętla działa tak jak w pluginie:
  - co klatkę `PerFrame` (z `RX_FrameTime` = czas klatki w ms),
  - co sekundę na zmianę `RX_Main_CycleTrigger` / `B_GetTime`. Uwaga: wcześniej (wersja Copilota) `RX_Main_CycleTrigger` szedł co sekundę, teraz co 2 sekundy, tak jak w pluginie,
  - co 5 sekund `B_CheckMenuOption` (odczyt opcji moda).

### 7. Łóżko: po przespaniu bohater nagle stoi, a potem robi animację wychodzenia z łóżka – ✅ (12.11.2026)
- **Plik:** `common/world/objects/npc.cpp` (`Npc::resetPositionToTA`)
- **Przyczyna:** sen przewija czas przez `Wld_SetTime`. OpenGothic ustawia wtedy wszystkie postacie według ich planu dnia i przy okazji zatrzymywał animację bohatera, czyli stawiał go na nogi, mimo że ten dalej leżał w łóżku (w menu snu). Gdy zamykałeś menu, łóżko odtwarzało jeszcze animację wstawania.
- **Poprawka:** jeśli bohater używa obiektu (łóżka), jego animacja i kolejka dialogu zostają bez zmian. Wstaje dopiero normalną animacją łóżka.

### 8. Statystyki postaci (B): ciemny ekran bez niczego – ✅ (12.11.2026)
- **Pliki:** `common/ui/gamemenu.cpp`, `common/ui/gamemenu.h` (oba zmienione, nowych plików brak)
- **Przyczyna:** w NB statystyki to okno pluginu Union z 5 zakładkami. `MENU_STATUS` w `MENU.DAT` moda ma tylko pusty nagłówek, więc OpenGothic rysował tylko przyciemnienie.
- **Poprawka:** własny ekran statystyk w stylu menu ustawień, w dwóch kolumnach:
  - **Postać:** gildia, poziom, doświadczenie, następny poziom, punkty nauki,
  - **Atrybuty:** siła, zręczność, **inteligencja** (atrybut NB), mana, punkty życia, **wytrzymałość** (NB),
  - **Walka:** broń jednoręczna, dwuręczna, łuk, kusza (nazwy ze skryptów gry),
  - **Ochrona:** broń sieczna, obuchowa, pociski, ogień, magia,
  - **Czas:** dzień i godzina.
- **Poprawione po teście:** wytrzymałość pokazywała „99/10” (NB trzyma ją ×10, teraz jest „99/100”), sekcja „Czas” przeszła do lewej kolumny, bo przycisk wstecz zasłaniał dzień, a tytuł nie nachodzi już na przycisk pauzy.
- **Poprawione po teście 2:** ekran czasem był po angielsku („Character”), bo ustawienie języka gry nie zawsze wynosi 2. Teraz język jest rozpoznawany też po polskich literach w tekstach gry (nazwy talentów i gildii).
- **Jeszcze nie ma:** pozostałych zakładek Uniona (rzemiosło, trudność itd.), można je dodać później.

### 9. Brak komunikatu na ekranie przy nowym wpisie do dziennika – ✅ (12.11.2026)
- **Plik:** `common/game/gamescript.cpp` (blok NB)
- **Objaw:** wpis trafia do dziennika, ale na ekranie nie pojawia się „Zadanie uzupełnione!”.
- **Przyczyna:** `B_LogEntry` w NB pokazuje komunikat przez `AI_PrintQuest(tytuł, temat)`, czyli funkcję Uniona.
- **Poprawka:** `AI_PrintQuest`, `AI_PrintQuestComplete` i `AI_PrintQuestFail` wypisują na ekranie tytuł i nazwę tematu. Dodane są też inne komunikaty Uniona (`AI_Print`, `AI_PrintRed`, `AI_PrintBonus`, `AI_NoticePrint`, `AI_FastPrintSkillCraft`), ale tylko wtedy, gdy w skryptach są to funkcje zewnętrzne. Te, które już działają przez skrypty, zostają bez zmian.

### 10. Pełne okno statystyk NB – 5 zakładek – ✅ (12.11.2026)
- **Pliki:** `common/ui/gamemenu.cpp`, `common/ui/gamemenu.h` (oba zmienione, nowych plików brak)
- **Co:** ekran statystyk (B) ma zakładki jak okno pluginu Union. Przy każdym otwarciu woła skrypt NB `RefreshStatusMenuData`, który wylicza wszystkie wartości (po polsku, ze skryptów: „mistrz”, „ekspert”, „uczony”, nazwy bossów…):
  1. **Postać:** gildia, rozdział, poziom, doświadczenie, punkty nauki, dzień, ranga, karma Innosa i Beliara, gildie złodziei, zabójców i kupców, zabici przeciwnicy, cały poziom trudności (PN i życie na poziom, życie, siła i ochrona wrogów, mikstury, pułapki, sen, głód),
  2. **Umiejętności:** rzemiosło, górnictwo i wytapianie, strzały, myślistwo (skóry, kły, pazury, rogi, łuski smoków…), gotowanie, złodziejstwo, magia, języki, retoryka, regeneracja, dary bogów,
  3. **Atrybuty:** siła, zręczność i mana (z wartością bazową), inteligencja, życie, wytrzymałość, władanie bronią w % ze stopniem (zielony/adept/ekspert/mistrz), tarcza, dwie bronie, styl orków,
  4. **Ochrona:** sieczna, obuchowa, pociski, ogień, magia, przewrócenie, odporność na trucizny, zimno i żar,
  5. **Osiągnięcia:** pokonani bossowie (39 możliwych).
- Listę przewija się palcem, a zakładkę zmienia się dotknięciem.
- **Uwaga:** opisów pozycji nie ma w plikach moda (rysuje je plugin), więc są napisane od nowa i mogą się trochę różnić od oryginału. Wartości są oryginalne, ze skryptów.

### 11. Wędrujący ognik od Vatrasa (L-Shift+F5) – ✅ (12.11.2026) – mod JS
- **Pliki:** 🆕 **NOWY MOD** `mods/nb-wisp/` (`manifest.json`, `index.js`), gotowy do instalacji jako `mods/nb-wisp.zip`. Nie trzeba przebudowywać APK.
- **Przyczyna:** w NB ognika przywołuje plugin Union pod L-Shift+F5 (`RX_QuickPanel_Wisp` tylko ustawia flagę dla pluginu). W skryptach jest tylko sama postać `WISP_DETECTOR` i jej AI: podąża za bohaterem (`ZS_MM_Rtn_Summoned`) i szuka przedmiotów (`B_MM_WispDetect`).
- **Mod:**
  - przywołuje albo odsyła ognika (`WISP_DETECTOR`), jeśli bohater ma amulet wędrującego ognika (`ITAM_ADDON_WISPDETECTORNEW` albo `ITAM_ADDON_WISPDETECTOR`),
  - włącza się przez **Shift+F5** na klawiaturze, przyciskiem w zakładce **„Ognik”** menu modów (na telefonie: Ustawienia → MODS) albo komendą `ognik` w konsoli,
  - po zmianie świata albo wczytaniu gry ognik wraca, jeśli był przywołany, a gdy zgubi się daleko (np. po teleporcie), pojawia się znowu przy bohaterze.

### 12. Opis przedmiotów w ekwipunku: był tylko „Wartość” – ✅ (12.11.2026)
- **Pliki:** 🆕 **NOWY PLIK** `common/game/nbiteminfo.h` (do folderu `common/game/`, obok `gamescript.cpp`), `common/game/gamescript.cpp`, `common/ui/inventorymenu.cpp`
- **Przyczyna:** w NB większość broni i pancerzy ma w skrypcie tylko `TEXT[5]` (wartość). Resztę opisu dopisuje plugin Union w chwili pokazania przedmiotu, z pól przedmiotu i tekstów ze stałych skryptu `RX_INV_DESC*`. Bez pluginu zostawała sama „Wartość”.
- **Poprawka:** kod pluginu przepisany z UNION_ABI.DLL. Wywołuje się przy wybraniu przedmiotu w ekwipunku (`dmaItemInfo`):
  - **broń biała:** obrażenia (np. „Obrażenia cięte: 15”), „Długość broni: 40 | Jednoręczna”, wymagania („Wymagana siła: 10” albo „Brak wymagań”), wartość,
  - **łuki i kusze:** obrażenia, typ („Łuk”/„Kusza”), wymagania (też umiejętność łuku/kuszy), wartość,
  - **pancerze:** ochrona przed cięciem, obuchem, kłuciem, ogniem | magią, wymagania,
  - **pierścienie i amulety:** „Ulepszenia: siła + N” (tylko gdy przedmiot daje siłę, zręczność albo manę).
- **Sprawdzone w grze:** broń („Obrażenia cięte: 10”, „Długość broni: 70 | Jednoręczna”), pancerz (ochrony), runy i księgi pokazują swoje opisy.
- **Okienko opisu (`inventorymenu.cpp`):** przy 5–6 wierszach pierwszy wiersz nachodził na nazwę przedmiotu, a długie wiersze (np. opis Świętej Strzały) wychodziły poza okienko na ekwipunek. Teraz wiersze zmniejszają się, gdy nie mieszczą się pod nazwą, a za długi wiersz dostaje mniejszą czcionkę, żeby zmieścił się na szerokość.
- **Czego jeszcze nie ma:** dopisków NB zależnych od stanu broni: „Naostrzona”, „Magiczna”, „Przeklęta”, broń orków i jaszczuroludzi, premie z pieczęci, waga.

### 13. Pełne opisy NB: dopiski broni i waga – ✅ waga sprawdzona (12.11.2026)
- **Pliki:** `common/game/nbiteminfo.h` (🆕 nowy plik z poprawki 12, podmień na nową wersję), `common/game/gamescript.cpp`
- **Przyczyna:** reszta opisu, którą rysuje plugin: dopiski typu broni zapisane w bitach `C_ITEM.HP` (`SetItemVarTrue(…, BIT_ITEM_…)`) i waga z `RX_SetWeightValue`. Funkcja `RX_SetWeightValueExt` (i `RX_GetWeightValue`) to zewnętrzne funkcje pluginu, których nikt nie obsługiwał, więc wagi się w ogóle nie zapisywały.
- **Poprawka:**
  - typ broni dostaje dopiski jak w pluginie: „| Broń orków” (`BIT_ITEM_ORC_WEAPON`; dla Mieczy Draconów „Broń jaszczuroczłeków”), „| Przeklęta” (`BIT_ITEM_UNDEAD_WEAPON`), „| Magiczna” (`BIT_ITEM_MAG_SWORD`), „| Naostrzona” (`BIT_ITEM_SHARPED`),
  - nowy wiersz **„Waga: N”**: broń biała = `RX_W_CRUSHINDEX` + `RX_W_MELEEWEAPONINDEX`, łuki i kusze = `RX_W_RANGEWEAPONINDEX`, pancerze, hełmy i tarcze = `WEIGHT` przedmiotu,
  - obsługa `RX_SetWeightValueExt` / `RX_GetWeightValue` (wagę czytają też skrypty, np. `Hero_Poisoned`),
  - tarcze zostawiają opis ze skryptu (tak robi plugin), dostają tylko wagę,
  - opis biżuterii tylko dla pierścieni i amuletów (jak w pluginie).
- **Czego nie ma i nie będzie:** premii z pieczęci (zaklinanie kamieniami `ITMI_ENCHANTSTONE_*`) – to osobne okno i dane pluginu, których OpenGothic nie ma. Naostrzenie plugin też trzyma u siebie, więc „Naostrzona” pojawi się tylko przy broni, którą skrypt oznaczy bitem.

### 14. Gruby pasek wczytywania, wypełnienie wychodzi poza ramkę – ✅ (12.11.2026)
- **Pliki:** `common/mainwindow.cpp`
- **Przyczyna:** OpenGothic rysuje pasek (`PROGRESS.TGA`) w rozmiarze tekstury, a marginesy wypełnienia (`PROGRESS_BAR.TGA`) ma na sztywno w pikselach oryginalnej tekstury Gothica II (512 px szerokości: 75 px z boku, 10 px z góry). NB ma tekstury HD 2× większe (ze zrzutu wychodzi ok. 1024×128), więc pasek był dwa razy większy, a wypełnienie zaczynało się przed ramką i nachodziło na lewy ornament.
- **Poprawka:** gdy tekstura jest szersza niż 512 px, pasek jest rysowany w rozmiarze oryginału (tekstura / skala), a marginesy wypełnienia skalują się razem z teksturą. Dla zwykłego Gothica II nic się nie zmienia. Ta sama poprawka marginesów działa przy pasku na ekranie zapisu.

### 15. Dziennik: puste „Zakończone zadania”, bestiariusz pod „Handel i rzemiosło” – ✅ (12.11.2026)
- **Pliki:** `common/ui/gamemenu.cpp`, `common/ui/gamemenu.h`
- **Przyczyna:**
  - NB zmienia stałe dziennika: `LOG_SUCCESS` = 4 (dla silnika to „nieaktualne”), a wpisy bestiariusza zapisuje jako `LOG_FAILED` (3). OpenGothic pokazywał w zakończonych tylko status 2, więc ta zakładka była pusta (np. runy teleportacyjne, bandyci Cavalorna).
  - Na telefonie zakładki miały na sztywno kolejność z Gothica II: obecne, zakończone, nieudane, notatki. W NB trzecim przyciskiem jest „Handel i rzemiosło” (notatki), a czwartym „Bestiariusz” (status 3), więc bestiariusz trafiał pod handel, a notatki pod bestiariusz.
- **Poprawka:**
  - każda zakładka bierze teraz to, co pokazuje, ze swojego przycisku w MENU.DAT (`EFFECTS MENU_ITEM_LIST_…` → `USERSTRING` listy), a nie z pozycji na ekranie,
  - w NB lista „FAILEDMISSIONS” (Zakończone zadania) pokazuje zadania o statusie 2 i 4, a lista „OLDMISSIONS” (Bestiariusz) status 3, tak jak w menu moda. Dla zwykłego Gothica II nic się nie zmienia.

### 16. Niewidoczne zbroje paladynów (Lothar, Lord Andre – sama głowa) – 🔍 diagnostyka (12.11.2026)
- **Pliki:** `common/world/objects/npc.cpp`
- **Co wiadomo:** Lord Andre nosi `ITAR_PAL_H_NPC` (model `Armor_Pal_H.asc`), Lothar zbroję paladyna `Armor_Pal_M.asc`. Pancerz straży (`WACHE_ARMOR_V2.asc`) działa. W logu nie ma `unable to load mesh` dla tych zbroi, więc model chyba się wczytuje, tylko się nie rysuje.
- **Na razie:** do `log.txt` dopisuje się raz na każdy model zbroi linia `armor "…": …` (czy plik jest w archiwach, ile ma części skórowanych, tekstur i doczepianych elementów, rozmiar). Na jej podstawie zrobię właściwą poprawkę.

### 19. Komunikat „Nowy wpis w dzienniku!” wyglądał inaczej niż w oryginale – ✅ (12.11.2026)
- **Pliki:** `common/game/gamescript.cpp`
- **Przyczyna:** ten napis rysuje plugin Union (`AI_PrintQuest`, `AI_PrintQuestComplete`, `AI_PrintQuestFail` z UNION_ABI.DLL), nie silnik gry. W poprawce 9 był wypisywany jak zwykły komunikat (biały, w liście u góry). W pluginie: po lewej stronie, na ok. 29% wysokości ekranu, tytuł czcionką ze skryptu `RX_QuestTitleFont` (`FONT_OLD_20_ZHITE.TGA`) w kolorze: nowy wpis – złoty, zadanie nieudane – czerwony, zakończone – zielony, a pod nim nazwa tematu białą `FONT_OLD_20_WHITE`.
- **Poprawka:** tak samo: tytuł po lewej czcionką `RX_QuestTitleFont`, pod nim temat, przez 5 s. Kolory: nowy wpis żółty, nieudane czerwone, zakończone białe (OpenGothic ma tylko takie kolory czcionek – zielonego brak).

### 23. NPC walczą jak nowicjusze: brak talentów i animacji walki (cały świat NB) – 🧪 (13.11.2026)
- **Pliki:** `common/game/gamescript.cpp`
- **Przyczyna:** każda postać NB w swojej definicji wywołuje `B_SetFightSkills(npc, procent)`. Skrypt zapisuje tylko zmienne pomocnicze i całą resztę zostawia pluginowi: `RX_ApplyProperOverlaysForNpc`. OpenGothic nie zna tej funkcji, więc u **wszystkich NPC** (Brago, bandyci, strażnicy, najemnicy …) talent 1H/2H/łuk/kusza zostawał na 0%. Skutki: animacje nieszkolonego nowicjusza (bez `humans_1hST1–3` / `2hST1–3`), żadnych trafień krytycznych (obrażenia zawsze ÷10, minimum 5), słabsze strzelanie.
- **Poprawka:** przepisany kod z UNION_ABI.DLL (0x10198B20):
  - procent dodawany do szansy trafienia 1H, 2H, łuku i kuszy (zakres 0–100);
  - zdejmowane stare nakładki, potem zakładana właściwa: od 90% ST3, od 60% ST2, od 30% ST1, poniżej – bez nakładki (łuk i kusza: od 60% T2, od 30% T1);
  - ustawiany poziom umiejętności walki (0–3).
  - `RX_InitAnimations` – w OpenGothic nic nie musi robić (animacje i tak są szukane po nazwie).
- **Bohater:** jego animacje (rapier, szabla, włócznia, halabarda, kostury, topory, dwie bronie, tarcze) zakładają skrypty NB przy założeniu broni (`RX_ApplyAnim_Weapons`, `EQUIP_*`). To działa także w OpenGothic, dlatego bohater ma np. rapier.

### 24. Bandyci nie atakują bezimiennego w zwykłej zbroi – ✅ (13.11.2026)
- **Pliki:** `common/game/gamescript.cpp`
- **Przyczyna (z logu):** `NB trace: B_AssessEnemy | ATT_HOSTILE false` – reakcja na wroga kończy się na `Npc_GetAttitude(self, hero) != ATT_HOSTILE`. NB w `B_SetAttributesToChapter` daje **każdemu** człowiekowi (bandytom też) osobisty stosunek do bohatera ATT_NEUTRAL. OpenGothic stawia stosunek osobisty ponad gildią, więc bandyci byli dla bohatera neutralni. W NB wrogość gildii (bandyci → bohater: wrogo) wygrywa i bandyci atakują, chyba że bohater ma zbroję bandyty (`C_PlayerIsFakeBandit` – to sprawdzają same skrypty).
- **Poprawka:**
  - w NB: gdy osobisty stosunek jest tylko „neutralny”, a gildia jest wroga – NPC jest wrogi. Stosunek „przyjazny”, „zły” i „wrogi” ustawiony przez skrypty zostaje bez zmian;
  - dodane `Npc_GetGuildAttitude` (stosunek gildii) – brakowało go, a skrypty NB pytają o to po walce (`B_CheckHealth`) i zwracało „wrogo” dla wszystkich.
- **Uwaga:** błąd `Accessing member "C_NPC.AIVAR" without an instance set (in C_PLAYERISFAKEBANDIT)` jest nieszkodliwy – funkcja zagląda do zmiennych Ramona z obozu bandytów (Jarkendar), którego nie ma w Khorinis; w oryginale tak samo. Diagnostyka z wcześniejszej wersji tej poprawki usunięta.

### 25. NPC nie dobywają broni w walce (Lares stoi i obrywa od zwierząt) – ✅ (lepiej: Lares wybiera cel i bije) (13.11.2026)
- **Pliki:** `common/game/gamescript.cpp`
- **Przyczyna:** `B_SelectWeapon` w NB (wybór broni na początku i w trakcie walki) kończy się wywołaniem pluginu `RX_SelectWeaponNpc(slf, oth)` – to on dobywa miecza lub łuku. OpenGothic tego nie znał, więc NPC w walce (towarzysze, strażnicy, bandyci) nie dobywali broni i stali w miejscu.
- **Poprawka:** wybór broni jak w zwykłym Gothic 2:
  - dalej niż `FIGHT_DIST_MELEE` (600) i jest łuk/kusza ze strzałami – broń dystansowa, bliżej – broń biała (albo pięści, gdy nie ma broni);
  - z pięści na miecz, gdy NPC ma miecz; z łuku na miecz, gdy wróg podejdzie albo skończą się strzały; z magii na broń, gdy `B_SelectSpell` nie wybrało czaru;
  - polecenie idzie tylko przy pustej kolejce AI (pętla ataku pyta co klatkę).

### 26. Zwierzęta gryzą za szybko, towarzysz (Lares) nie nadąża – ✅ (13.11.2026)
- **Pliki:** `common/world/objects/npc.cpp`
- **Przyczyna:** po ataku NPC czekał na następną decyzję tylko do znacznika `DEF_HIT_END` animacji. To okno kombinacji ciosów broni białej. Potwory (i walka pięściami) kombinacji nie mają, więc następny atak zaczynał się zaraz po trafieniu i ucinał resztę animacji – zwierzęta gryzły ok. 1,5 raza szybciej niż w Gothic. Każde ugryzienie przewraca trafionego (animacja „potknięcia”), więc Lares przy kilku zwierzętach ciągle dostawał i nie zdążał oddać.
- **Poprawka:** w walce pięściami/kłami (wszystkie potwory) NPC czeka do końca animacji ataku. Ludzie z bronią dalej mogą łączyć ciosy w kombinacje.

### 27. Towarzysz (Lares) stoi 0,25–3 s po każdej walce, dialogu i postoju – ✅ (13.11.2026)
- **Pliki:** `common/game/gamescript.cpp` (+ `common/world/objects/npc.cpp`: wycofana pierwsza próba i diagnostyka)
- **Przyczyna (z logu diagnostyki):** po zmianie stanu Lares dostawał w kolejce `AI_Wait` ~2,5 s przed `SetWalkMode GoToPoint`. Źródło to skrypt NB: `B_CheckHealth` na początku każdego stanu człowieka (dopóki nie stanie na swoim miejscu – `AIV_TAPOSITION`) woła `B_Npc_Desynchronize(npc, nCycleChar)` = `AI_WaitMS` z globalnym licznikiem 250, 500 … 3000 ms, żeby postacie w jednym miejscu nie ruszały wszystkie naraz. W stanach towarzysza (`ZS_Guide_Player`, `ZS_Follow_Player`) miejsca nigdy nie osiąga, więc czekał przy każdej zmianie stanu. Do tego po walce mówi kwestię końca walki (~3 s) – to normalne.
- **Poprawka:** dla członków drużyny (`AIV_PARTYMEMBER`) to czekanie jest pomijane; pozostałe postacie czekają jak w NB.
- **Wycofane:** pierwsza próba (pętla stanu co 100 ms) – nie miała wpływu; diagnostyka `NB comp` usunięta.

### 29. Style walki NPC z pluginu NB (część 1: gildie bez własnej tabeli – m.in. Lares) – ⏸ wycofane z kodu na życzenie (gra lepiej działała bez tej wersji); opis zostaje do późniejszego powrotu (13.11.2026)
- **Pliki:** (było) `common/game/gamescript.cpp`
- **Co robi plugin (UNION_ABI.DLL 0x1024A460, co 100 ms):** dla każdego człowieka w promieniu 1500 od bohatera, który walczy (ma wroga) z dobytą bronią jedno- lub dwuręczną i ma więcej niż 1/15 życia, zdejmuje wszystkie nakładki broni (ok. 45: ST1–3, tarcze, rapier, pirat, włócznie, kostury, halabarda, topory, dwie bronie, „zręcznościowe”…) i zakłada jeden styl walki dobrany wg **gildii**, szansy trafienia i losowania. Zmienna `AIVRX_CHANGE_MOVESET` oznacza „styl wybrany”, `AIVRX_NPC_CHANGEMOVESET3` odlicza 40–80 kroków (4–8 s); potem, gdy wróg jest 500+ dalej, styl jest losowany na nowo.
- **Zrobione teraz – gildie bez własnej tabeli** (Lares ma gildię GIL_NONE, więc to jego przypadek; też VLK, KDF, NOV, BAU, KDW…):
  - jedna ręka, szansa ≥ 90%: z tarczą Shield_ST1/ST2/ST2/ST3; bez tarczy losowo: brak nakładki, 1hST1, 1hST2, 1hST3, 1hDexST1–3;
  - ≥ 60%: z tarczą Shield_ST1/ST2; bez: brak, 1hST1, 1hST2, 1hDexST1, 1hDexST2; poniżej 60%: 1hST1 albo brak;
  - styl „zręcznościowy” zamieniany na zwykły – zawsze przy toporze, inaczej w 80% (jak w pluginie);
  - dwie ręce, ≥ 90%: 2hST1/2hST2/2hST3/AxeST2; ≥ 60%: 2hST1/2hST2; poniżej: 2hST1.
- **Do zrobienia (części 2…):** własne tabele gildii BDT, SLD (m.in. Sentenza, Buster, Fester, Bullco, Raoul – po imieniu; rapier dla broni z bitem rapiera), MIL, PAL, DJG, PIR, OUT i szkieletów. Do tego czasu te gildie zostają przy stylach z poprawki 23.

### 28. Lares (i inni ludzie z bronią) odskakuje zamiast bić – ✅ (13.11.2026)
- **Pliki:** `common/world/objects/npc.cpp`
- **Przyczyna:** sztuczka OpenGothic „czekanie przy wrogu = odskok” (dla wilków, które podchodzą za blisko) działała też na ludzi z bronią.
- **Poprawka:** zamiana „czekanie → odskok” tylko dla walczących pięściami/kłami (potwory), najwyżej raz na 2,5 s. (Druga część – reakcja na zamach raz na zamach w `fightalgo.cpp` – wycofana: bez niej gra działa lepiej.)

### 30. NPC bez trafień krytycznych na starym zapisie (Lares bije po 41 zamiast zabić pająka w 2 ciosy) – ✅ (13.11.2026)
- **Pliki:** `common/game/gamescript.cpp`
- **Przyczyna:** obrażenia w NB: trafienie krytyczne (szansa = umiejętność walki w %) daje siła + broń − ochrona, zwykłe tylko 1/10 tego. Szanse NPC ustawia `B_SetFightSkills` → `RX_ApplyProperOverlaysForNpc` (poprawka 23) – ale tylko w chwili tworzenia postaci, czyli przy nowej grze. W zapisie zrobionym przed poprawką 23 wszyscy NPC (Lares – mistrz, 100%) mają szansę 0% i nigdy nie trafiają krytycznie.
- **Poprawka:** skrypty NB zapisały te procenty także w `AIVAR[84..87]` (1H, 2H, łuk, kusza). NPC w pobliżu bohatera (sprawdzane co 100 ms), który ma wszystkie szanse 0, a te zmienne większe od 0, dostaje szanse z nich (i poziom umiejętności/nakładkę jak w poprawce 23). Nowa gra tego nie potrzebuje.
- **Wynik:** ✅ wilki, gobliny, topielce i ścierwojady Lares kładzie jednym ciosem.

### 31. Lares (i inni z łukiem) nie strzela – w NB łuk ma pierwszeństwo przed mieczem – ✅ (Lares wyciąga łuk, zwierzęta na jedno trafienie) (13.11.2026)
- **Pliki:** `common/game/gamescript.cpp`
- **Przyczyna:** poprawka 25 wybierała broń jak zwykły Gothic 2 (łuk tylko dalej niż 600, z bliska od razu miecz). Plugin NB (`RX_SelectWeaponNpc`, UNION_ABI.DLL 0x1017F840) ma własne zasady – w oryginale Lares prawie całą drogę strzela z łuku.
- **Poprawka – zasady z pluginu:**
  - z łukiem/kuszą i strzałami NPC strzela, chyba że wróg jest bliżej niż 600 (a `AIVRX_NPC_RANGE_ONLY` = 0); do krwiopijców i harpii zawsze strzela; do wrogów odpornych na strzały (ochrona przed strzałami −1, golemy, trolle, strażnicy) tylko „łucznik-potwór” (`AIVRX_IS_MONSTER_ARCHER`);
  - NPC, który już strzela, **nie zmienia łuku na miecz przy zwierzętach** (wilki, ścierwojady, szczury, gobliny, krwiopijce, warany, pełzacze, kretoszczury…), nawet z bliska;
  - po zmianie broni następna dopiero po 6 s (`AIVRX_NPC_CHANGEWEAPON_TIMER`), gdy ma strzały;
  - bez broni białej – pięści.
- **Poprawka do poprawki (13.11.2026, 23:30):** dalej walczył mieczem – `Npc::hasAmmunition` sprawdza strzały tylko do broni trzymanej w ręku, więc z mieczem w dłoni „nie miał strzał”. Strzały liczone teraz do założonego łuku/kuszy.

### 32. Bohater walczy nożem i pałką jak rapierem – ✅ (13.11.2026)
- **Pliki:** `common/game/gamescript.cpp`
- **Przyczyna:** przy każdej zmianie broni skrypty NB (`RX_ApplyAnim_Weapons` → `RX_ResetAllOverlays`) zdejmują ok. 60 nakładek animacji funkcją pluginu `Mdl_RemoveOverlayMds_NoUpdate`, której OpenGothic nie znał – nic nie było zdejmowane. Raz założony rapier zostawiał `Humans_Rapier_ST*` na zawsze i każda broń jednoręczna (nóż, pałka) walczyła jak rapier.
- **Poprawka:** dodane `Mdl_RemoveOverlayMds_NoUpdate`, `Mdl_ApplyOverlayMds_NoUpdate` (jak zwykłe zdjęcie/założenie nakładki; „NoUpdate” = bez przeładowania modelu, OpenGothic tego nie potrzebuje) i `Mdl_HasOverlayMds` (log pokazywał je jako niezaimplementowane).
- **Uwaga:** Lares (NPC) ma już prawidłowe animacje miecza – NPC dostają je z poprawki 23.

### 33. Ścierwojad odskakuje od pierwszego zamachu – rytm walki jak w Gothic – ✅ (14.11.2026)
- **Pliki:** `common/world/objects/npc.cpp`
- **Jak jest w NB/Gothic (opis gracza):** ścierwojad ostrzega 3 razy, rzuca się na bohatera; bohater odskakuje, ścierwojad wpada na niego i dostaje 2 ciosy, dopiero wtedy odskakuje – i cykl się powtarza (da się go zabić bez utraty życia).
- **Przyczyna:** reakcja na zamach (`FA_ENEMY_PREHIT`: ścierwojad – zawsze odskok) jest w oryginale sprawdzana tylko, gdy potwór skończy swoją akcję i przestanie się chwiać po trafieniu. OpenGothic sprawdzał ją co ~0,2 s, więc ścierwojad odskakiwał już od pierwszego zamachu.
- **Poprawka (na razie tylko ścierwojad, `FAI_SCAVENGER`):** odskok przed zamachem dopiero po 2 trafieniach od poprzedniego odskoku; wcześniej ścierwojad czeka. Jeśli rytm się zgadza, rozszerzę na inne zwierzęta z odskokiem w taktyce (wilk, krwiopijca, waran, szczur, zębacz, cieniostwór, harpia…).
- **Usunięte:** tymczasowe komunikaty `[odskok] …` na ekranie.

### 34. Rytm walki z poprawki 33 dla wszystkich zwierząt – 🧪 (14.11.2026)
- **Pliki:** `common/world/objects/npc.cpp`
- Każdy potwór walczący pazurami/kłami odskakuje przed zamachem bohatera dopiero po 2 trafieniach od poprzedniego odskoku (dotąd tylko ścierwojad). Dotyczy zwierząt, które mają odskok w swojej taktyce (`FIGHT.DAT`, `FA_ENEMY_PREHIT`): wilk (odskok/unik), krwiopijca, waran, szczur, zębacz, cieniostwór, harpia, jaszczurka, aligator, niedźwiedź, gargulec, smok… Kretoszczur, goblin, golem nie odskakują – u nich nic się nie zmienia.

### 35. Style walki NPC z pluginu NB wg gildii (bandyci, najemnicy, straż, paladyni, łowcy smoków, piraci, banici) – 🧪 (14.11.2026)
- **Pliki:** `common/game/gamescript.cpp`
- Wraca mechanizm z poprawki 29 (co 100 ms, wróg w pobliżu, broń w ręku, nowe losowanie po 4–8 s), ale **tylko dla gildii z własną tabelą w pluginie** – postacie bez gildii (Lares, VLK, BAU…) zostają przy stylach z poprawki 23, bo te działają dobrze.
- **Bandyci, jedna ręka (UNION_ABI.DLL 0x1024AD00):**
  - Nestor, Franco, Esteban: topór → 1hST1–3 (z tarczą Shield_ST1–3); tarcza → Shield_ST2–4; rapier → Rapier_ST1–3; inaczej 1hST1–3;
  - Dexter, bandyci BDT_10620–10623, Haart: tarcza → Shield_ST1–3; rapier → Rapier_ST1/ST2/Pirate; inaczej 1hST1–3 albo 1hDexST1–3;
  - pozostali, szansa ≥ 60%: tarcza ST1/ST2; rapier ST2/ST3; inaczej 1hST2/ST3, 1hDexST2/ST3; poniżej 60%: tarcza ST1–3; rapier ST1/Pirate; inaczej 1hST1/ST2, 1hDexST1/ST2.
- **Bandyci, dwie ręce (0x1024B810):** Gor Boba – 2h_b_st1; ≥ 85%: 2hST3/AxeST2/SpST2; ≥ 60%: 2hST1/2hST2/AxeST2; poniżej: 2hST1/2hST2.
- „Rapier” = broń z bitem `BIT_ITEM_PIERCE_DAMAGE` (2) w `C_ITEM.HP`; styl „zręcznościowy” w 80% zamieniany na zwykły (jak w pluginie).
- **Dodane (14.11.2026, część 2) – reszta gildii z tabel pluginu:**
  - najemnicy (SLD, 0x1024BA00/C080): AL_Boss3 – Boss03/Boss032; Sentenza – rapier ST1–3 albo 1hST1–3/1hDexST1–2; Buster, Fester, Bullco, Raoul – Pirate/Rapier_ST1/brak albo 1hST1–2/1hDexST1; pozostali – tarcza ST1–2, rapier ST1–2/Pirate, inaczej 1hST1–2/1hDexST1; dwie ręce – 2hST2/AxeST2;
  - straż (MIL, 0x1024C1D0): Ruga, Pablo, Peck, Wambo, Wulfgar – 1hST1–3/1hDexST1–3; pozostali – 1hST1–2/1hDexST1–2 (dwie ręce bez własnej tabeli);
  - paladyni (PAL, 0x1024C720/C8D0): tarcza ST2–4, inaczej 1hST2–3/1hDexST2–3; dwie ręce – 2hST2–3/SpST2;
  - łowcy smoków (DJG, 0x1024CA20/CBD0): tarcza ST2–4, inaczej 1hST2–3/1hDexST2–3; dwie ręce – 2hST2–3;
  - gildia 16 (Templariusze w NB), dwie ręce (0x1024CD20): 2hST2–3/SpST2;
  - piraci (PIR, 0x1024CE70/D300): Alligator Jack – 1hST2; wg szansy (≥ 90 / ≥ 60 / mniej) tarcze, rapier/Pirate, 1hST/1hDexST; dwie ręce – 2hST1–2/SpST2;
  - banici (OUT, 0x1024D450/D6D0): wg szansy tarcze ST1–3, 1hST/1hDexST albo brak nakładki; dwie ręce – 2hST1/2hST3/AxeST2.
- **Jeszcze nie:** szkielety (GIL_SKELETON) – bardzo długa tabela po nazwach szkieletów (rycerze z tarczą, kapitanowie, marynarze, mroczni wojownicy sfer…).

### 36. Zacinanie się dźwięku na ekranie wczytywania – ✅ razem z 38 (14.11.2026)
- **Pliki:** `common/gamemusic.cpp` (zmieniony plik z projektu – podmienić)
- **Z nagrania:** przez cały czas ekran wczytywania „KHORINIS”; w dźwięku między 5,5 a 10,5 s kilkanaście przerw po 0,1–0,6 s.
- **Przyczyna:** muzykę (GothicKit/DirectMusic) generuje funkcja wywoływana przez OpenAL w jego wątku miksowania. Przy zmianie motywu ta funkcja sama ładowała segment muzyki razem z instrumentami (DLS) – pod blokadą zasobów, którą w tym czasie trzyma wczytywanie świata. Wątek OpenAL czekał, a z nim cały dźwięk (muzyka i efekty).
- **Poprawka:** segment nowego motywu jest ładowany w wątku gry w chwili wyboru motywu; wątek dźwięku tylko go odtwarza. Gdy coś się nie zgadza (inny plik), działa jak dotąd.
- **Jeśli dalej się zacina:** druga możliwa przyczyna to zajęty procesor w czasie wczytywania (wątek dźwięku nie dostaje czasu) – wtedy pomoże większy bufor OpenAL.
- **Test:** zacinki zostały → poprawka 37.

### 37. Zacinanie się dźwięku – priorytet wątku dźwięku i dłuższy bufor – ✅ razem z 38 (14.11.2026)
- **Pliki:** `common/gamemusic.cpp` (zmieniony plik z projektu – podmienić), `game/main.cpp` (zmieniony plik z projektu – podmienić; folder `game/` w głównym katalogu projektu, nie w `common/`)
- **Przyczyna:** na Androidzie OpenAL miksuje dźwięk (OpenSL ES) we własnym wątku, a ten wątek chodzi ze zwykłym priorytetem – OpenAL próbuje ustawić priorytet czasu rzeczywistego (SCHED_RR), ale Android nie pozwala na to aplikacji. Bufor to 3 porcje po 20 ms (60 ms). Przy wczytywaniu świata wątki robocze zajmują wszystkie rdzenie i wątek dźwięku nie zdąża przygotować kolejnej porcji – przerwa.
- **Poprawka:**
  - `gamemusic.cpp`: przy pierwszym wywołaniu w wątku miksowania (funkcja generująca muzykę jest wywoływana właśnie w nim) podnosi jego priorytet do priorytetu audio Androida (−16, jak `THREAD_PRIORITY_AUDIO`; gdy się nie da, −10);
  - `main.cpp`: przed pierwszym użyciem OpenAL zapisuje `alsoft.conf` (w folderze gry, obok `log.txt` – wersja `main.cpp` z wyborem folderu gry) z `period_size = 960`, `periods = 6` i wskazuje go zmienną `ALSOFT_CONF` – bufor 120 ms zamiast 60 ms.
- **Do sprawdzenia:** czy dźwięki w grze (ciosy, kroki) nie wydają się spóźnione – wtedy zmniejszyć `periods` do 4–5.
- **Test:** zacinki zostały → poprawka 38.

### 38. Zacinanie się dźwięku – muzyka ładowana w osobnym wątku, też dla domyślnej muzyki OpenGothic – ✅ (14.11.2026)
- **Pliki:** `common/gamemusic.cpp` (zmieniony plik z projektu – podmienić)
- **Przyczyna:** OpenGothic ma dwa silniki muzyki: „OpenGothic” (domyślny, `soundProviderIndex=0`) i „GothicKit”. Poprawka 36 działała tylko dla GothicKit, a domyślny dalej ładował muzykę motywu (`Resources::loadDxMusic`) w wątku dźwięku – pod blokadą zasobów, którą trzyma wczytywanie świata (im większy mod, tym dłużej). Do tego ekran wczytywania co klatkę zleca motyw `SYS_Loading`, a poprawka 36 ładowała go wtedy od nowa co klatkę.
- **Poprawka:** oba silniki ładują muzykę nowego motywu w osobnym wątku. Wątek dźwięku zaczyna motyw dopiero, gdy jest załadowany (do tego czasu gra poprzednia muzyka albo cisza – efekty grają dalej). Motyw, który już gra, nie jest ładowany ponownie. Ani wątek gry, ani wątek dźwięku nie czekają na blokadę zasobów.

## Do zrobienia

- [ ] Czarny goblin nie atakuje, stoi w miejscu (14.11.2026). Z logu: wrogi (attitude=0), czuje bohatera (sense=5 przy ~1100), ale zostaje w `ZS_MM_RTN_ROAM` bez celu – reakcja na wroga (`B_MM_AssessEnemy`) nie przełącza go na atak; raz pojawia się `ZS_MM_ThreatenEnemy`. `RX_GobboClan` (niezaimplementowane) jest tylko w `AI_OnDead_Gobbo_Black_Clan` – przy śmierci, nie przy ataku. Do zbadania z włączonymi śladami `B_MM_AssessEnemy` (więcej niż 3 na komunikat).
- [ ] **Animacje broni NB – sprawdzić, co się za nimi kryje (w toku).** Ustalone do tej pory:
  - bohater: animacje zakładają skrypty (`RX_ApplyAnim_Weapons` → `RX_ApplyAni_1H_Rapier` / `_DexSword` / `_1H_Sword` / `_2H_Speer` / `_2H_Staff` / `_2H_Hallberd` / `_2H_Common`, tarcze `RX_ApplyShieldAni`, dwie bronie `Humans_1x2ST3` / `2x2ST3`, topory orków `HUMANS_AXEST2`) – w OpenGothic działa;
  - NPC: talenty i nakładki ST1–3 z pluginu – poprawka 23;
  - do sprawdzenia w grze: czy zestawy rapiera/szabli/włóczni mają w plikach .MDS ciosy, które OpenGothic odtwarza inaczej niż oryginał (kilka pchnięć w jednej animacji, kombinacje, zasięg) – potrzebne nagranie walki rapierem i zwykłym mieczem;
  - kod wyboru dla NPC z innymi broniami (ok. 0x1024AE00 w DLL) – do przejrzenia.
- [ ] Potwory w walce za często odskakują/krążą (ścierwojad cofa się zamiast atakować). To problem silnika OpenGothic, nie tylko NB – to samo widać w modzie Renovation (goblin). Reguła `FA_ENEMY_PREHIT` (odskok, gdy przeciwnik bierze zamach) jest w OpenGothic sprawdzana przy każdej decyzji potwora – do porównania z oryginałem.
- [ ] Walka kilku postaci z jednym celem: cel „głupieje” (krąży, nie atakuje) – do zbadania (nagranie z obozu bandytów, Brago)
- [ ] `EXT_PutInSlot` / `ActivateZSSlot`: mieszek i torba na bohaterze (opcje z poprawki 5)
