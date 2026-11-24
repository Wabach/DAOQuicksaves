// SPDX-License-Identifier: GPL-3.0-only
//
// DAO Quick Saves: rotating quicksaves and first-visit zone saves for Dragon Age: Origins.
// Copyright (C) 2026 <GREGORY DRABBLE>
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
// Public License as published by the Free Software Foundation, version 3 of the License.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
// implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// for more details. You should have received a copy of the GNU General Public License along with this program
// (see the LICENSE file); if not, see <https://www.gnu.org/licenses/>.
//
// It is built on DAOLib (https://github.com/Whocares66-Dev/DAOLib, GPLv3), which it compiles in.
//
// DAOQuickSaves : sauvegardes rapides en rotation (F5/F9) et sauvegarde automatique à la première visite
// de chaque zone, dans des emplacements à part.
//
// Principe, avec la seule API documentée de dao/Saves.h :
//  - F5 : le plugin prend le message de sauvegarde rapide du jeu et fait à la place une sauvegarde dans un anneau
//    d'emplacements manuels réservés (« Quick 17 », « Quick 18 », ...). Le numéro croît, donc on voit laquelle
//    est la plus récente. F9 recharge la plus récente sauvegarde, donc celle-là.
//  - À la première entrée dans une zone, il sauvegarde dans un autre anneau (« Zone 23 <zone> »). Ni l'autosave
//    du jeu ni la sauvegarde rapide ne peuvent l'écraser.
//  - Les zones déjà visitées et les deux compteurs sont écrits dans chaque sauvegarde (co-save), donc
//    recharger une ancienne partie remet la liste telle qu'elle était à ce moment-là.
//
// Réglages : daoquicksave.ini, à côté de la DLL (créé avec les valeurs par défaut s'il n'existe pas). Les
// valeurs sont lues au démarrage ; elles servent aussi à chaque sauvegarde, donc un changement en jeu (menu
// des réglages des plugins) s'applique tout de suite. Les plages d'emplacements sont fixes : changer un
// nombre ne déplace aucune sauvegarde existante.
//
// Les réglages sont aussi dans le menu des options (onglet Online) : le mod Nexus 7114 les reprend dans sa
// fenêtre « Plugin Settings ». Un changement fait là est écrit dans le .ini.

#include "dao/ClientExoApp.h"
#include "dao/Frame.h"
#include "dao/GameModes.h"
#include "dao/Log.h"
#include "dao/Messages.h"
#include "dao/Options.h"
#include "dao/Party.h"
#include "dao/Plugin.h"
#include "dao/Saves.h"
#include "dao/String.h"
#include "dao/TypeName.h"
#include "dao/World.h"
#include "dao/generated/ScriptApi.h"

#include <spdlog/spdlog.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{

using dao::saves::Kind;

constexpr const char *kKey = "daoquicksaves"; // nom de notre fichier dans chaque sauvegarde

constexpr int kQuickMax = 20;       // plus grand nombre de sauvegardes rapides que l'on peut demander
constexpr int kZoneMax = 100;        // plus grand nombre de sauvegardes de zone que l'on peut demander
constexpr int kSettleFrames = 120;  // laisse la zone finir de se charger (environ 2 s)
constexpr int kRetryFrames = 30;    // espacement des nouvelles tentatives quand le jeu refuse de sauvegarder
constexpr int kBusyTimeout = 600;   // abandonne l'attente d'une sauvegarde qui n'est jamais signalée
constexpr int kQuickTries = 10;     // essais de copie d'une sauvegarde rapide (le jeu peut être encore occupé)

// Emplacements manuels réservés tout en haut de la plage, une fois pour toutes (les « prochain emplacement
// libre » du joueur partent du bas) : sauvegardes rapides [kQuickFirst, +kQuickMax), zones [kZoneFirst, +kZoneMax).
constexpr std::int32_t kZoneFirst = dao::saves::MostSlot(Kind::Manual) - kZoneMax + 1;
constexpr std::int32_t kQuickFirst = kZoneFirst - kQuickMax;

// --- Réglages (daoquicksave.ini) ----------------------------------------------------------------------------

constexpr int kDefaultQuickCount = 5;
constexpr int kDefaultZoneCount = 100;

struct Settings
{
    std::atomic<bool> quickEnabled{true}; // copier chaque sauvegarde rapide dans l'anneau
    std::atomic<int> quickCount{kDefaultQuickCount}; // 1 à kQuickMax
    std::atomic<bool> zoneEnabled{true};  // sauvegarder à la première visite d'une zone
    std::atomic<int> zoneCount{kDefaultZoneCount};  // 1 à kZoneMax
    bool logMessages = false;             // journaliser les messages de sauvegarde et de chargement du moteur (diagnostic)
};

Settings g_cfg;

int Clamp(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Trim(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return std::string(text);
}

// Le fichier est dans le dossier de la DLL, quel que soit l'endroit où le jeu est installé.
std::filesystem::path IniPath()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&IniPath), &module))
        return {};
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return std::filesystem::path(buffer).parent_path() / L"daoquicksave.ini";
}

void SaveIni()
{
    const std::filesystem::path path = IniPath();
    if (path.empty())
        return;
    std::ofstream out(path, std::ios::trunc);
    if (!out)
    {
        spdlog::warn("could not write {}", dao::Utf8(path.wstring()));
        return;
    }
    out << "; DAO Quick Saves settings.\n"
        << "; Put comments on their own line. Changes made in the Plugin Settings screen are written here.\n"
        << "\n"
        << "[QuickSaves]\n"
        << "; 1 = keep a rotating copy of every quicksave (F5) in extra save slots, 0 = off\n"
        << "Enabled=" << (g_cfg.quickEnabled.load() ? 1 : 0) << "\n"
        << "; how many copies to keep, 1 to " << kQuickMax << "\n"
        << "Count=" << g_cfg.quickCount.load() << "\n"
        << "\n"
        << "[ZoneSaves]\n"
        << "; 1 = save automatically the first time a zone is entered, 0 = off\n"
        << "Enabled=" << (g_cfg.zoneEnabled.load() ? 1 : 0) << "\n"
        << "; how many of these saves to keep, 1 to " << kZoneMax << "\n"
        << "Count=" << g_cfg.zoneCount.load() << "\n"
        << "\n"
        << "[Debug]\n"
        << "; 1 = log the engine's save and load messages (to see what F5 and F9 send), 0 = off\n"
        << "LogMessages=" << (g_cfg.logMessages ? 1 : 0) << "\n";
}

// Un changement fait dans le jeu : on le garde dans le fichier.
void SettingsChanged()
{
    SaveIni();
}

void LoadIni()
{
    const std::filesystem::path path = IniPath();
    if (path.empty())
    {
        spdlog::warn("cannot locate the plugin's folder: settings stay at their defaults");
        return;
    }
    std::ifstream in(path);
    if (!in)
    {
        SaveIni(); // première fois : on écrit le fichier avec les valeurs par défaut
        spdlog::info("wrote the default settings to {}", dao::Utf8(path.wstring()));
        return;
    }

    std::string section;
    for (std::string raw; std::getline(in, raw);)
    {
        const std::string line = Trim(raw);
        if (line.empty() || line.front() == ';' || line.front() == '#')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = Lower(Trim(std::string_view(line).substr(1, line.size() - 2)));
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = Lower(Trim(std::string_view(line).substr(0, equals)));
        const std::string value = Trim(std::string_view(line).substr(equals + 1));

        int number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{})
            continue; // pas un nombre : la valeur par défaut reste

        if (section == "quicksaves")
        {
            if (key == "enabled")
                g_cfg.quickEnabled = number != 0;
            else if (key == "count")
                g_cfg.quickCount = Clamp(number, 1, kQuickMax);
        }
        else if (section == "zonesaves")
        {
            if (key == "enabled")
                g_cfg.zoneEnabled = number != 0;
            else if (key == "count")
                g_cfg.zoneCount = Clamp(number, 1, kZoneMax);
        }
        else if (section == "debug")
        {
            if (key == "logmessages")
                g_cfg.logMessages = number != 0;
        }
    }
}

// Une case à cocher du menu, liée à un réglage : un clic le change et l'écrit dans le fichier.
dao::options::Toggle Switch(const wchar_t *name, const wchar_t *label, const wchar_t *tooltip,
                            std::atomic<bool> &value, bool defaultValue)
{
    dao::options::Toggle toggle;
    toggle.name = name;
    toggle.label = label;
    toggle.tooltip = tooltip;
    toggle.defaultValue = defaultValue; // ce que « Restore Defaults » du jeu lui redonne
    toggle.get = [&value] { return value.load(); };
    toggle.set = [&value, name](bool wanted) {
        spdlog::info("option {} set to {}", dao::Utf8(name), wanted); // aussi quand « Restore Defaults » l'appelle
        value = wanted;
        SettingsChanged();
        return wanted;
    };
    return toggle;
}

// Une liste de nombres à proposer : la liste de base, plus la valeur actuelle si le .ini en a mis une autre.
std::vector<int> CountChoices(std::vector<int> base, int current)
{
    base.push_back(current);
    std::sort(base.begin(), base.end());
    base.erase(std::unique(base.begin(), base.end()), base.end());
    return base;
}

std::int32_t NearestIndex(const std::vector<int> &options, int current)
{
    std::size_t best = 0;
    for (std::size_t i = 1; i < options.size(); ++i)
    {
        const int here = options[i] > current ? options[i] - current : current - options[i];
        const int there = options[best] > current ? options[best] - current : current - options[best];
        if (here < there)
            best = i;
    }
    return static_cast<std::int32_t>(best);
}

// Un curseur à crans nommés (les nombres de la liste), lié à un réglage.
dao::options::Choice CountChoice(const wchar_t *name, const wchar_t *label, const wchar_t *tooltip,
                                 std::vector<int> options, std::atomic<int> &value, int defaultNumber)
{
    dao::options::Choice choice;
    choice.name = name;
    choice.label = label;
    choice.tooltip = tooltip;
    for (std::size_t i = 0; i < options.size(); ++i)
    {
        choice.values.push_back(std::to_wstring(options[i]));
        if (options[i] == defaultNumber)
            choice.defaultValue = static_cast<std::int32_t>(i); // ce que « Restore Defaults » du jeu lui redonne
    }
    choice.get = [options, &value]() -> std::int32_t { return NearestIndex(options, value.load()); };
    choice.set = [options, &value, name](std::int32_t wanted) -> std::int32_t {
        if (wanted >= 0 && static_cast<std::size_t>(wanted) < options.size())
        {
            spdlog::info("option {} set to {}", dao::Utf8(name), options[static_cast<std::size_t>(wanted)]);
            value = options[static_cast<std::size_t>(wanted)];
            SettingsChanged();
        }
        return NearestIndex(options, value.load());
    };
    return choice;
}

// Les nombres proposés par les deux curseurs (remplis par RegisterSettings, relus par ResetSettings).
std::vector<int> g_quickOptions;
std::vector<int> g_zoneOptions;

// Le bouton « Reset DAO Quick Saves » : remet nos quatre réglages à leurs valeurs par défaut.
void ResetSettings()
{
    g_cfg.quickEnabled = true;
    g_cfg.quickCount = kDefaultQuickCount;
    g_cfg.zoneEnabled = true;
    g_cfg.zoneCount = kDefaultZoneCount;
    SettingsChanged();
    spdlog::info("settings reset to their defaults");

    // Pour que les contrôles affichés suivent : sans effet (le jeu refuse) si leur écran n'est pas ouvert.
    // Chaque Set rappelle le « set » du contrôle, qui réécrit la même valeur : sans conséquence.
    dao::options::Set(L"DAOQuickSavesQuickEnabled", 1);
    dao::options::Set(L"DAOQuickSavesZoneEnabled", 1);
    dao::options::Set(L"DAOQuickSavesQuickKept", NearestIndex(g_quickOptions, kDefaultQuickCount));
    dao::options::Set(L"DAOQuickSavesZoneKept", NearestIndex(g_zoneOptions, kDefaultZoneCount));
}

// Les réglages dans le menu des options. La fenêtre « Plugin Settings » du mod Nexus 7114 reprend toute
// seule ce que les plugins ajoutent ici. Appelé depuis DAOLib_Load, avant que le jeu n'ouvre l'écran des
// options. Onglet Online : le plus de place sans ce mod (neuf lignes à gauche, contre six sur Game, et la
// section en demande un peu plus de six) ; avec le mod, l'onglet n'a plus d'importance.
void RegisterSettings()
{
    const std::vector<int> quickBase = {1, 2, 3, 4, 5, 6, 8, 10, 15, 20};
    const std::vector<int> zoneBase = {5, 10, 15, 20, 30, 40, 50, 75, 100};
    g_quickOptions = CountChoices(quickBase, g_cfg.quickCount);
    g_zoneOptions = CountChoices(zoneBase, g_cfg.zoneCount);

    dao::options::Button reset;
    reset.name = L"DAOQuickSavesReset";
    reset.label = L"Reset DAO Quick Saves";
    reset.tooltip = L"Put all the DAO Quick Saves settings back to their defaults";
    reset.pressed = [] { ResetSettings(); };

    const bool added = dao::options::AddSection(
        L"DAO Quick Saves",
        {Switch(L"DAOQuickSavesQuickEnabled", L"Rotating quicksaves",
                L"Keep a copy of each quicksave (F5) in extra save slots, so you can go back further than the last one",
                g_cfg.quickEnabled, true),
         Switch(L"DAOQuickSavesZoneEnabled", L"Save on first visit to a zone",
                L"Save automatically the first time you enter a zone, in slots the game never overwrites",
                g_cfg.zoneEnabled, true)},
        {CountChoice(L"DAOQuickSavesQuickKept", L"Quicksaves kept", L"How many copies of your quicksaves are kept",
                     g_quickOptions, g_cfg.quickCount, kDefaultQuickCount),
         CountChoice(L"DAOQuickSavesZoneKept", L"Zone saves kept", L"How many first-visit saves are kept",
                     g_zoneOptions, g_cfg.zoneCount, kDefaultZoneCount)},
        dao::options::Column::Either, dao::options::Tab::Online, {reset});
    if (!added)
        spdlog::warn("the settings section was refused");
}

// --- État du plugin : tout tourne sur le fil du jeu, une chose à la fois, sans verrou ----------------------

int g_quickCount = 0;               // nombre de sauvegardes rapides copiées dans cette partie
int g_zoneCount = 0;                // nombre de sauvegardes de première visite dans cette partie
std::set<std::string> g_visited;    // clés des zones déjà vues par ce personnage

bool g_quickPending = false;        // une sauvegarde rapide attend sa copie
bool g_zonePending = false;         // la zone courante est inédite et attend sa sauvegarde
bool g_resumed = false;             // la partie vient d'une sauvegarde : sa première zone n'est pas « inédite »

const void *g_area = nullptr;       // la zone vue à la dernière image
std::string g_key;                  // sa clé, une fois lue
std::string g_name;                 // son nom lisible, pour le nom de la sauvegarde
bool g_decided = false;             // on a décidé si cette zone est inédite
bool g_keyWarned = false;
int g_settle = 0;
int g_throttle = 0;
int g_quickTries = 0;

bool g_busy = false;                // une de nos sauvegardes est en cours d'écriture
std::int32_t g_busySlot = 0;
int g_busyFrames = 0;

// --- Co-save : compteurs et zones visitées, en texte ------------------------------------------------------

std::string Serialize()
{
    std::string out = "q=" + std::to_string(g_quickCount) + "\nz=" + std::to_string(g_zoneCount) + "\n";
    for (const std::string &key : g_visited)
        out += "a=" + key + "\n";
    return out;
}

int ParseCount(std::string_view text)
{
    int value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value); // pas de std::stoi : un callback ne doit pas lever
    return value < 0 ? 0 : value;
}

void Deserialize(std::string_view data)
{
    g_quickCount = 0;
    g_zoneCount = 0;
    g_visited.clear();
    while (!data.empty())
    {
        const std::size_t end = data.find('\n');
        const std::string_view line = data.substr(0, end);
        data = end == std::string_view::npos ? std::string_view{} : data.substr(end + 1);
        if (line.size() < 2 || line[1] != '=')
            continue;
        const std::string_view value = line.substr(2);
        if (line[0] == 'q')
            g_quickCount = ParseCount(value);
        else if (line[0] == 'z')
            g_zoneCount = ParseCount(value);
        else if (line[0] == 'a' && !value.empty())
            g_visited.emplace(value);
    }
}

// --- Le jeu ------------------------------------------------------------------------------------------------

// Le tag de la zone où se trouve le personnage contrôlé : stable d'une session à l'autre, contrairement au
// pointeur de CurrentArea(). C'est GetTag(GetArea(GetMainControlled())) des scripts ; Controlled() est
// GetMainControlled. Appelé depuis dao::OnFrame, donc sur le fil des scripts, comme il le faut.
// Rend une chaîne vide tant que la clé n'est pas lisible : le plugin réessaie à l'image suivante.
std::string AreaKey()
{
    const auto hero = dao::party::Controlled();
    if (hero == dao::kInvalidObject)
        return {};
    const auto area = dao::script::api::GetArea(hero);
    const auto tag = dao::script::api::GetTag(area);
    return dao::Utf8(tag);
}

// Le nom lisible de la zone, dans la langue du jeu, tel que le donne GetName(GetArea(...)) des scripts.
// Vide si le jeu ne rend rien : l'appelant retombe alors sur la clé technique.
std::string AreaName()
{
    const auto hero = dao::party::Controlled();
    if (hero == dao::kInvalidObject)
        return {};
    const auto area = dao::script::api::GetArea(hero);
    // TODO(compilation) : comme pour GetTag, le type de retour de GetName n'a pas été lu ; si dao::Utf8 ne
    // l'accepte pas tel quel, convertir comme dans AreaKey().
    return dao::Utf8(dao::script::api::GetName(area));
}

bool Exploring()
{
    // explore() n'est non nul qu'en exploration ou en combat ; en combat, Save() refuse et on réessaie plus tard.
    auto *app = dao::CClientExoApp::Get();
    if (app == nullptr)
        return false;
    auto *modes = app->GameModes();
    return modes != nullptr && modes->Explore() != nullptr;
}

std::wstring FromUtf8(const std::string &utf8)
{
    if (utf8.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

// Le nom d'une sauvegarde sert aussi de nom de fichier : on retire ce que Windows y refuse.
std::wstring SaveText(const std::string &utf8)
{
    std::wstring text = FromUtf8(utf8);
    const std::wstring_view forbidden = L"<>:\"/\\|?*";
    text.erase(std::remove_if(text.begin(), text.end(),
                              [&](wchar_t c) { return c < 32 || forbidden.find(c) != std::wstring_view::npos; }),
               text.end());
    return text;
}

// Écrit une sauvegarde manuelle dans l'anneau [first, first + ring), à la place du plus ancien.
// Le compteur est avancé avant l'appel : la co-save écrite par Saved() doit déjà le porter.
bool StartRingSave(std::int32_t first, int ring, int &counter, const std::wstring &label, const std::wstring &suffix)
{
    const int next = counter + 1;
    const std::int32_t slot = first + next % ring;
    counter = next;
    const std::wstring name = label + std::to_wstring(next) + suffix;
    if (!dao::saves::Save(Kind::Manual, slot, name))
    {
        counter = next - 1;
        return false;
    }
    g_busy = true;
    g_busySlot = slot;
    g_busyFrames = 0;
    return true;
}

// --- Callbacks ---------------------------------------------------------------------------------------------

void Loaded(const std::optional<dao::saves::Slot> &save)
{
    Deserialize({});
    g_area = nullptr;
    g_key.clear();
    g_decided = false;
    g_zonePending = false;
    g_quickPending = false;
    g_busy = false;
    g_resumed = save.has_value(); // reprendre une partie n'est pas une première visite
    if (!save)
    {
        spdlog::info("new game: no zone seen yet");
        return;
    }
    const auto text = save->Read(kKey);
    if (text)
        Deserialize(std::string_view(text->data(), text->size()));
    spdlog::info("loaded {}'s {} save, slot {}: {} zones seen, counters {}/{}{}", dao::Utf8(save->character),
                 dao::saves::Name(save->kind), save->number, g_visited.size(), g_quickCount, g_zoneCount,
                 text ? "" : " (no file of ours)");
}

void Saved(const dao::saves::Slot &save)
{
    // Chaque sauvegarde, la nôtre comme celle du jeu, porte la liste des zones et les compteurs.
    if (!save.Write(kKey, Serialize()))
        spdlog::warn("could not keep our data with {} save {}", dao::saves::Name(save.kind), save.number);

    if (g_busy && save.kind == Kind::Manual && save.number == g_busySlot)
        g_busy = false;                // c'est la nôtre : l'anneau est à jour
    else if (save.kind == Kind::Quick && g_cfg.quickEnabled)
    {
        g_quickPending = true;         // le joueur vient d'appuyer sur F5 : on en garde une copie
        g_throttle = 0;                // mais on laisse d'abord le jeu finir d'écrire la sienne
        g_quickTries = 0;
    }
}

void TrackArea()
{
    const void *area = dao::world::CurrentArea();
    if (area != g_area) // une zone se charge, ou on quitte le monde
    {
        g_area = area;
        g_key.clear();
        g_decided = false;
        g_zonePending = false;
        g_settle = area != nullptr ? kSettleFrames : 0;
        return;
    }
    if (area == nullptr || g_decided)
        return;
    if (g_settle > 0)
    {
        --g_settle;
        return;
    }
    g_key = AreaKey();
    if (g_key.empty())
    {
        if (!g_keyWarned)
            spdlog::warn("the area's tag is not readable yet (no controlled character?)");
        g_keyWarned = true;
        return;
    }
    g_name = AreaName();
    if (g_name.empty())
        g_name = g_key; // le jeu ne donne pas de nom lisible : on garde le tag
    g_decided = true;
    if (g_resumed)
    {
        g_resumed = false;
        g_visited.insert(g_key); // la zone où on reprend est « déjà vue », sans nouvelle sauvegarde
        spdlog::info("resumed in '{}': marked as seen, no save", g_key);
        return;
    }
    if (!g_cfg.zoneEnabled)
    {
        g_visited.insert(g_key); // fonction coupée : la zone est notée, mais on ne sauvegarde pas
        return;
    }
    g_zonePending = g_visited.find(g_key) == g_visited.end();
    if (g_zonePending)
        spdlog::info("first visit of '{}' ({})", g_key, g_name);
}

void Tick()
{
    if (g_busy && ++g_busyFrames > kBusyTimeout)
    {
        spdlog::warn("a save was never reported: no longer waiting for it");
        g_busy = false;
    }

    TrackArea();
    if (!g_cfg.quickEnabled)
        g_quickPending = false;
    if (!g_cfg.zoneEnabled)
        g_zonePending = false;
    if (g_busy)
        return;

    if (g_quickPending)
    {
        if (++g_throttle < kRetryFrames)
            return;
        g_throttle = 0;
        ++g_quickTries;
        if (StartRingSave(kQuickFirst, g_cfg.quickCount, g_quickCount, L"Quick ", {}))
        {
            spdlog::info("quicksave copied to ring slot {} (try {})", g_busySlot, g_quickTries);
            g_quickPending = false;
        }
        else if (g_quickTries >= kQuickTries)
        {
            spdlog::warn("could not copy the quicksave after {} tries (count {}, exploring: {}, busy: {})",
                         g_quickTries, g_quickCount, Exploring(), g_busy);
            g_quickPending = false;
        }
        else
            spdlog::debug("quicksave copy refused, try {}", g_quickTries);
        return;
    }

    if (!g_zonePending || !Exploring() || ++g_throttle < kRetryFrames)
        return;
    g_throttle = 0;

    // La zone est notée avant la sauvegarde, car la co-save écrite avec elle doit déjà la contenir.
    g_visited.insert(g_key);
    if (StartRingSave(kZoneFirst, g_cfg.zoneCount, g_zoneCount, L"Zone ", L" " + SaveText(g_name)))
    {
        spdlog::info("zone save written to ring slot {}", g_busySlot);
        g_zonePending = false;
    }
    else
        g_visited.erase(g_key); // le jeu refuse pour l'instant (combat, scène) : on réessaiera
}

// F5 envoie au jeu un SaveGameMessage de type « sauvegarde rapide » (kind 2, slot -1). On le prend pour que le jeu
// ne fasse pas la sienne : il ne reste que la sauvegarde de l'anneau, donc un seul message à l'écran. Les
// sauvegardes manuelles, les autosauvegardes et les nôtres (kind 0 ou 1) passent sans être touchées.
// Rend true pour prendre le message (le jeu ne le traite alors pas).
bool TakeQuicksave(const std::int32_t kind)
{
    if (kind != static_cast<std::int32_t>(Kind::Quick) || !g_cfg.quickEnabled.load())
        return false;
    g_quickPending = true;
    g_throttle = kRetryFrames; // pas d'attente : il n'y a plus de sauvegarde du jeu à laisser finir
    g_quickTries = 0;
    spdlog::info("quicksave key taken: saving to the ring instead");
    return true;
}

} // namespace

extern "C" __declspec(dllexport) bool __cdecl DAOLib_Load(const dao::LoaderInfo *info)
{
    dao::OpenLog("daoquicksaves");
    if (info == nullptr || info->apiVersion != dao::kPluginApiVersion || !info->knownExecutable)
    {
        spdlog::error("not the loader or the game this was built for");
        return false;
    }

    LoadIni();
    RegisterSettings();

    spdlog::info("slot limits: manual {}, auto {}, quick {}; reserved slots: quick {}-{}, zone {}-{}",
                 dao::saves::MostSlot(Kind::Manual), dao::saves::MostSlot(Kind::Auto),
                 dao::saves::MostSlot(Kind::Quick), kQuickFirst, kQuickFirst + kQuickMax - 1, kZoneFirst,
                 kZoneFirst + kZoneMax - 1);
    spdlog::info("settings: quicksaves {} x{}, zone saves {} x{}", g_cfg.quickEnabled.load() ? "on" : "off",
                 g_cfg.quickCount.load(), g_cfg.zoneEnabled.load() ? "on" : "off", g_cfg.zoneCount.load());

    if (g_cfg.logMessages)
    {
        // Diagnostic : quels messages du moteur parlent de sauvegarde ou de chargement quand on appuie sur F5 / F9.
        // TODO(compilation) : le type de retour de dao::TypeName n'a pas été lu (une chaîne attendue).
        dao::messages::OnAnyMessage([](void *receiver, const auto &message) {
            const std::string_view name = dao::TypeName(&message);
            if (name.find("Save") == std::string_view::npos && name.find("Load") == std::string_view::npos)
                return;
            spdlog::info("message {} to {}", name, receiver);
        });
        spdlog::info("logging the engine's save and load messages");
    }

    // TODO(compilation) : la forme exacte du rappel d'InterceptMessage<M> n'a pas été lue (void *, const M &) -> bool.
    dao::messages::InterceptMessage<dao::messages::SaveGameMessage>(
        [](void *, const auto &message) -> bool { return TakeQuicksave(message.kind); });

    dao::saves::OnGameLoaded(&Loaded);
    dao::saves::OnSaved(&Saved);
    dao::OnFrame([] { Tick(); });
    return true;
}
