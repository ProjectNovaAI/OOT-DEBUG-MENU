// Warps, the age switch and guest calls on the game thread for the debug menu (debug_tools.h). Game build 1.1.1 only.
//
// An area change in this game goes through a few flags. A loading zone's event (Dm_SY_BancChangeGo) reserves the change:
// it writes the next area (Sequence_ReservedNextBanc, the name of a stage file such as KakarikoVillage_Adult), the actor
// to start at (Sequence_ReservedNextStartPosName), the arrival event (Sequence_ReservedBootEventName) and the actor that
// starts it (Sequence_ReservedEventStarterName), then switches the event sequence to ChangeBancOnEvent, which loads the
// area named there. A warp keeps these four strings set to its destination while the game reserves a change: the next
// area change of any kind lands there instead. Child and adult are separate stage files of the same area (the _Child and
// _Adult suffix), and the area's parameters set the age, so a warp to the other age's file changes the age.
//
// Instant warps need a change to reserve: the player is put into a loading zone of the area (dbg_warp_now) with the
// game's own actor warp, called from the debug frame hook (debug_game.c, on the game thread once per frame).
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "debug_tools.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

// ---- the destinations: area file, start actor, arrival event. Every start actor exists in its area file.

static const struct Warp {
    const char *banc, *start, *boot;
} warps[] = {
    {"BottomOfTheWell", "Well_KakarikoSide", "Dm_SY_BancChangeArrive"},
    {"DeathMountainCrater_Adult", "Crater_GoronCitySide", "DeathMountainCrater_Arrive"},
    {"DeathMountainCrater_Adult", "Crater_MountainTrailSide", "DeathMountainCrater_Arrive"},
    {"DeathMountainCrater_Adult", "OutsideOfFireTemple", "Dm_SY_BancChangeArrive_DeathMountainCrater_02"},
    {"DeathMountainCrater_Child", "Crater_GoronCitySide", "DeathMountainCrater_Arrive"},
    {"DeathMountainCrater_Child", "Crater_MountainTrailSide", "DeathMountainCrater_Arrive"},
    {"DeathMountainCrater_Child", "OutsideOfFireTemple", "Dm_SY_BancChangeArrive_DeathMountainCrater_02"},
    {"DeathMountainTrail_Adult", "MountainTrail_CraterSide", "MountainTrail_ArriveFromDeathMountainCrater"},
    {"DeathMountainTrail_Adult", "MountainTrail_DodongoCavernSide", "Dm_SY_BancChangeArrive_DeathMountainTrail_03"},
    {"DeathMountainTrail_Adult", "MountainTrail_GoronCitySide", "Dm_SY_BancChangeArrive_DeathMountainTrail_02"},
    {"DeathMountainTrail_Adult", "MountainTrail_KakarikoSide", "Dm_SY_BancChangeArrive_DeathMountainTrail_01"},
    {"DeathMountainTrail_Child", "MountainTrail_CraterSide", "MountainTrail_ArriveFromDeathMountainCrater"},
    {"DeathMountainTrail_Child", "MountainTrail_DodongoCavernSide", "Dm_SY_BancChangeArrive_DeathMountainTrail_03"},
    {"DeathMountainTrail_Child", "MountainTrail_GoronCitySide", "Dm_SY_BancChangeArrive_DeathMountainTrail_02"},
    {"DeathMountainTrail_Child", "MountainTrail_KakarikoSide", "Dm_SY_BancChangeArrive_DeathMountainTrail_01"},
    {"DesertColossus_Adult", "DesertColossus_FromHountedWasteland", "Dm_SY_BancChangeArrive_DesertColossus_01"},
    {"DesertColossus_Adult", "DesertColossus_FromSpiritTempleEntrance", "FromSpiritTemple_ArriveColossus"},
    {"DesertColossus_Adult", "DesertColossus_FromSpiritTempleLeftHand", "Dm_SY_BancChangeArrive_SpiritTemple_02"},
    {"DesertColossus_Adult", "DesertColossus_FromSpiritTempleRightHand", "Dm_SY_BancChangeArrive_SpiritTemple_01"},
    {"DesertColossus_Child", "DesertColossus_FromHountedWasteland", "Dm_SY_BancChangeArrive_DesertColossus_01"},
    {"DesertColossus_Child", "DesertColossus_FromSpiritTempleEntrance", "FromSpiritTemple_ArriveColossus"},
    {"DesertColossus_Child", "DesertColossus_FromSpiritTempleLeftHand", "Dm_SY_BancChangeArrive_SpiritTemple_02"},
    {"DesertColossus_Child", "DesertColossus_FromSpiritTempleRightHand", "Dm_SY_BancChangeArrive_SpiritTemple_01"},
    {"DodongoCavern", "DodongoCavern_MountainTrailSide", "Dm_SY_BancChangeArrive"},
    {"FinalBattleField", "FinalBattleField_Entrance", "InsideGanonCaslte_Dungeon_Escape"},
    {"FireTemple", "FireTemple", "Dm_SY_BancChangeArrive"},
    {"ForestTemple", "ForestTemple", "Dm_SY_BancChangeArrive"},
    {"GanonCastle", "GanonCastleEntrance", "Dm_SY_BancChangeArrive_GanonCastle_01"},
    {"GanonCastle", "GanonCastleEntrance_FromInside", "Dm_SY_BancChangeArrive_GanonCastle_02"},
    {"GerudoFortress_Adult", "GerudoFortress_FromGerudoTrainingGround", "Dm_SY_BancChangeArrive_GerudoFortress_01"},
    {"GerudoFortress_Adult", "GerudoFortress_FromGerudoValley", "Dm_SY_BancChangeArrive_GerudoFortress_03"},
    {"GerudoFortress_Adult", "GerudoFortress_FromHountedWasteland", "Dm_SY_BancChangeArrive_GerudoFortress_02"},
    {"GerudoTrainingGround", "GerudoTrainingGround_FromGerudoFortress", "FromGerudoFortress_ArriveTrainingGround"},
    {"GerudoValley_Adult", "GerudoValley_Entrance", "FromHyruleField_ArriveGerudoValley"},
    {"GerudoValley_Adult", "GerudoValley_FromGerudoFortress", "Dm_SY_BancChangeArrive_GerudoValley_01"},
    {"GerudoValley_Adult", "GerudoValley_FromLakeHylia", "Dm_SY_BancChangeArrive"},
    {"GerudoValley_Child", "GerudoValley_Entrance", "FromHyruleField_ArriveGerudoValley"},
    {"GerudoValley_Child", "GerudoValley_FromLakeHylia", "Dm_SY_BancChangeArrive"},
    {"GoronCity_Adult", "GoronCity_CraterSide", "Dm_SY_BancChangeArrive"},
    {"GoronCity_Adult", "GoronCity_LostWoodSide_after", "Dm_SY_BancChangeArrive_GoronCity_02"},
    {"GoronCity_Adult", "GoronCity_LostWoodSide_before", "Dm_SY_BancChangeArrive_GoronCity_02_before"},
    {"GoronCity_Adult", "GoronCity_MountainTrailSide", "Dm_SY_BancChangeArrive_GoronCity_01"},
    {"GoronCity_Child", "GoronCity_CraterSide", "Dm_SY_BancChangeArrive"},
    {"GoronCity_Child", "GoronCity_LostWoodSide_after", "Dm_SY_BancChangeArrive_GoronCity_02"},
    {"GoronCity_Child", "GoronCity_LostWoodSide_before", "Dm_SY_BancChangeArrive_GoronCity_02_before"},
    {"GoronCity_Child", "GoronCity_MountainTrailSide", "Dm_SY_BancChangeArrive_GoronCity_01"},
    {"Graveyard_Adult", "GraveYard_KakarikoSide", "Dm_SY_BancChangeArrive_Graveyard_01"},
    {"Graveyard_Adult", "GraveYard_KakarikoWindmillSide", "Dm_SY_BancChangeArrive_UndetTheGrave_01"},
    {"Graveyard_Adult", "GraveYard_ShadowTempleSide", "Dm_SY_BancChangeArrive_ShadowTemple_01"},
    {"Graveyard_Child", "GraveYard_KakarikoSide", "Dm_SY_BancChangeArrive_Graveyard_01"},
    {"Graveyard_Child", "GraveYard_ShadowTempleSide", "Dm_SY_BancChangeArrive_ShadowTemple_01"},
    {"HountedWasteland_Adult", "HountedWasteland_FromDesertColossus", "FromColossus_ArriveWasteland"},
    {"HountedWasteland_Adult", "HountedWasteland_FromGerudoFortress", "FromGerudoFortress_ArriveWateland"},
    {"HountedWasteland_Child", "HountedWasteland_FromDesertColossus", "FromColossus_ArriveWasteland"},
    {"HyruleCastle", "HyruleCastle_Entrance", "Dm_SY_BancChangeArrive_HyruleCastle_01"},
    {"HyruleField_Adult", "HyruleField_FrontOfGerudoValley", "Dm_SY_BancChangeArrive_HyruleField_03"},
    {"HyruleField_Adult", "HyruleField_FrontOfKakarikoVillage", "Dm_SY_BancChangeArrive_HyruleField_01"},
    {"HyruleField_Adult", "HyruleField_FrontOfLostWoodBridge", "Dm_SY_BancChangeArrive"},
    {"HyruleField_Adult", "HyruleField_FrontOfMarket", "Dm_SY_BancChangeArrive_HyruleField_02"},
    {"HyruleField_Adult", "HyruleField_FrontOfZoraRiver", "Dm_SY_BancChangeArrive_HyruleField_03_Land"},
    {"HyruleField_Adult", "HyruleField_FrontOfZoraRiver_UnderWater", "Dm_SY_BancChangeArrive_ZoraRiver_01_Under"},
    {"HyruleField_Adult", "LakeHylia_FromGerudoValley", "Dm_SY_BancChangeArrive_LakeHylia_02"},
    {"HyruleField_Adult", "OutsideOfWaterTemple", "Dm_SY_BancChangeArrive"},
    {"HyruleField_Adult", "ZoraLakeHylia_FromZoraDomain", "Dm_SY_BancChangeArrive_LakeHylia_01"},
    {"HyruleField_Child", "HyruleField_FrontOfGerudoValley", "Dm_SY_BancChangeArrive_HyruleField_03"},
    {"HyruleField_Child", "HyruleField_FrontOfKakarikoVillage", "Dm_SY_BancChangeArrive_HyruleField_01"},
    {"HyruleField_Child", "HyruleField_FrontOfLostWoodBridge", "Dm_SY_BancChangeArrive"},
    {"HyruleField_Child", "HyruleField_FrontOfMarket", "Dm_SY_BancChangeArrive_HyruleField_02"},
    {"HyruleField_Child", "HyruleField_FrontOfZoraRiver", "Dm_SY_BancChangeArrive_HyruleField_03_Land"},
    {"HyruleField_Child", "HyruleField_FrontOfZoraRiver_UnderWater", "Dm_SY_BancChangeArrive_ZoraRiver_01_Under"},
    {"HyruleField_Child", "LakeHylia_FromGerudoValley", "Dm_SY_BancChangeArrive_LakeHylia_02"},
    {"HyruleField_Child", "OutsideOfWaterTemple", "Dm_SY_BancChangeArrive"},
    {"HyruleField_Child", "ZoraLakeHylia_FromZoraDomain", "Dm_SY_BancChangeArrive_LakeHylia_01"},
    {"IceCavern", "IceCavern_FromZoraFountain", "SageOfWater_Arrival_IceCavern"},
    {"IceCavern", "IceCavern_FromZoraFountain_Backdoor", "SageOfWater_Arrival_IceCavern"},
    {"InsideGanonCastle", "Entrance", "Dm_SY_BancChangeArrive_InsideGanonCastle_01"},
    {"InsideJabuJabuBelly", "InsideJabuJabuBelly_FromZoraFountain", "Dm_SY_BancChangeArrive"},
    {"InsideTheDekuTree", "Entrance", "Dm_SY_BancChangeArrive_InsideTheDekuTree_01"},
    {"KakarikoVillage_Adult", "KakarikoVillage_Entrance", "Dm_SY_BancChangeArrive_KakarikoVillage_01"},
    {"KakarikoVillage_Adult", "KakarikoWindmill_GraveYardSide", "Dm_SY_BancChangeArrive_UnderTheGrave_02"},
    {"KakarikoVillage_Adult", "Kakariko_GraveYardSide", "Dm_SY_BancChangeArrive"},
    {"KakarikoVillage_Adult", "Kakariko_MountainTrailSide", "Dm_SY_BancChangeArrive_KakarikoVillage_02"},
    {"KakarikoVillage_Adult", "Kakariko_WellSide", "Dm_SY_BancChangeArrive"},
    {"KakarikoVillage_Child", "KakarikoVillage_Entrance", "Dm_SY_BancChangeArrive_KakarikoVillage_01"},
    {"KakarikoVillage_Child", "Kakariko_GraveYardSide", "Dm_SY_BancChangeArrive"},
    {"KakarikoVillage_Child", "Kakariko_MountainTrailSide", "Dm_SY_BancChangeArrive_KakarikoVillage_02"},
    {"KakarikoVillage_Child", "Kakariko_WellSide", "Dm_SY_BancChangeArrive"},
    {"KokiriForest_Adult", "FrontDekuTree", "Dm_SY_BancChangeArrive_KokiriForest_01"},
    {"KokiriForest_Adult", "KokiriForest_FrontOfLostWood", "Dm_SY_BancChangeArrive_Kokiri"},
    {"KokiriForest_Adult", "LostWoodBridge_FrontOfHyruleField", "Dm_SY_BancChangeArrive"},
    {"KokiriForest_Child", "FrontDekuTree", "Dm_SY_BancChangeArrive_KokiriForest_01"},
    {"KokiriForest_Child", "KokiriForest_FrontOfLostWood", "Dm_SY_BancChangeArrive_Kokiri"},
    {"KokiriForest_Child", "LostWoodBridge_FrontOfHyruleField", "Dm_SY_BancChangeArrive"},
    {"LostWood_Adult", "LostWood_Entrance", "Get_StoneOfFire_ArriveFromKokiri"},
    {"LostWood_Adult", "LostWood_FrontOfSacredForestMeadow", "Dm_SY_BancChangeArrive_LostWood"},
    {"LostWood_Adult", "LostWood_GoronCitySide_after", "Get_StoneOfFire_ArriveFromGoronCity"},
    {"LostWood_Adult", "LostWood_GoronCitySide_before", "Get_StoneOfFire_ArriveFromGoronCity"},
    {"LostWood_Adult", "LostWood_ZoraRiverSide", "Dm_SY_BancChangeArrive_LostWood_01"},
    {"LostWood_Child", "LostWood_Entrance", "Get_StoneOfFire_ArriveFromKokiri"},
    {"LostWood_Child", "LostWood_FrontOfSacredForestMeadow", "Dm_SY_BancChangeArrive_LostWood"},
    {"LostWood_Child", "LostWood_GoronCitySide_after", "Get_StoneOfFire_ArriveFromGoronCity"},
    {"LostWood_Child", "LostWood_GoronCitySide_before", "Get_StoneOfFire_ArriveFromGoronCity"},
    {"LostWood_Child", "LostWood_ZoraRiverSide", "Dm_SY_BancChangeArrive_LostWood_01"},
    {"Market_Adult", "Market_Entrance", "Dm_SY_BancChangeArrive_Market_02"},
    {"Market_Adult", "Market_Entrance_FromCastle", "Dm_SY_BancChangeArrive_Market_03"},
    {"Market_Child", "Market_Entrance", "Dm_SY_BancChangeArrive_Market_02"},
    {"Market_Child", "Market_FrontOfHyruleCastle", "Dm_SY_BancChangeArrive_Market_01"},
    {"SacredForestMeadow_Adult", "OutsideOfForestTemple", "Dm_SY_BancChangeArrive_SacredForestMeadow_01"},
    {"SacredForestMeadow_Adult", "SacredForestMeadow_FrontOfLostWood", "SacredForestMeadow_ArriveFromLostWood"},
    {"SacredForestMeadow_Child", "OutsideOfForestTemple", "Dm_SY_BancChangeArrive_SacredForestMeadow_01"},
    {"SacredForestMeadow_Child", "SacredForestMeadow_FrontOfLostWood", "SacredForestMeadow_ArriveFromLostWood"},
    {"ShadowTemple", "Entrance", "ShadowTemple_Arrive"},
    {"SpiritTemple", "SpiritTemple_Entrance", "FromColossus_ArriveSpiritTemple"},
    {"SpiritTemple", "SpiritTemple_FromLeftHand", "Dm_SY_BancChangeArrive"},
    {"SpiritTemple", "SpiritTemple_FromRightHand", "Dm_SY_BancChangeArrive"},
    {"WaterTemple", "WaterTemple_FromLakeHylia", "SageOfWater_Arrival_WaterTemple"},
    {"ZoraDomain_Adult", "ZoraDomain_FromLakeHylia", "Dm_SY_BancChangeArrive_ZoraDomain_01"},
    {"ZoraDomain_Adult", "ZoraDomain_FromZoraFountain", "Dm_SY_BancChangeArrive"},
    {"ZoraDomain_Adult", "ZoraDomain_FromZoraRiver", "Dm_SY_BancChangeArrive"},
    {"ZoraDomain_Child", "ZoraDomain_FromLakeHylia", "Dm_SY_BancChangeArrive_ZoraDomain_01"},
    {"ZoraDomain_Child", "ZoraDomain_FromZoraFountain", "Dm_SY_BancChangeArrive"},
    {"ZoraDomain_Child", "ZoraDomain_FromZoraRiver", "Dm_SY_BancChangeArrive"},
    {"ZoraFountain_Adult", "ZoraFountain_FromIceCavern", "Dm_SY_BancChangeArrive_IceCaven_02"},
    {"ZoraFountain_Adult", "ZoraFountain_FromIceCavern_Backdoor", "Dm_SY_BancChangeArrive_IceCaven_01"},
    {"ZoraFountain_Adult", "ZoraFountain_FromZoraDomain", "Dm_SY_BancChangeArrive"},
    {"ZoraFountain_Child", "ZoraFountain_FromInsideJabuJabuBelly", "Dm_SY_BancChangeArrive_ZoraFountain_01"},
    {"ZoraFountain_Child", "ZoraFountain_FromZoraDomain", "Dm_SY_BancChangeArrive"},
    {"ZoraRiver_Adult", "ZoraRiver_Entrance", "Dm_SY_BancChangeArrive"},
    {"ZoraRiver_Adult", "ZoraRiver_Entrance_UnderWater", "Dm_SY_BancChangeArrive_ZoraRiver_01_Under"},
    {"ZoraRiver_Adult", "ZoraRiver_FromZoraDomain", "ZoraRiver_ArriveFromZoraDomain_Child"},
    {"ZoraRiver_Adult", "ZoraRiver_LostWoodSide", "ZoraRiver_ArriveFromLostWood_Child"},
    {"ZoraRiver_Child", "ZoraRiver_Entrance", "Dm_SY_BancChangeArrive"},
    {"ZoraRiver_Child", "ZoraRiver_Entrance_UnderWater", "Dm_SY_BancChangeArrive_ZoraRiver_01_Under"},
    {"ZoraRiver_Child", "ZoraRiver_FromZoraDomain", "ZoraRiver_ArriveFromZoraDomain_Child"},
    {"ZoraRiver_Child", "ZoraRiver_LostWoodSide", "ZoraRiver_ArriveFromLostWood_Child"},
};
#define NWARPS ((int)(sizeof warps / sizeof warps[0]))

int dbg_warp_count(void) { return NWARPS; }

const char *dbg_warp_banc(int i) { return i >= 0 && i < NWARPS ? warps[i].banc : NULL; }
const char *dbg_warp_start(int i) { return i >= 0 && i < NWARPS ? warps[i].start : NULL; }

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), k = strlen(tail);
    return n >= k && !strcmp(s + n - k, tail);
}

int dbg_warp_age(int i) {
    if (i < 0 || i >= NWARPS) return DBG_AGE_ANY;
    return ends_with(warps[i].banc, "_Child") ? DBG_AGE_CHILD : ends_with(warps[i].banc, "_Adult") ? DBG_AGE_ADULT : DBG_AGE_ANY;
}

// The same entrance in the other age's file of the area, -1 when that file has no such entrance.
int dbg_warp_other_age(int i) {
    int age = dbg_warp_age(i);
    if (age == DBG_AGE_ANY) return -1;
    char want[64];
    size_t n = strlen(warps[i].banc) - 6;
    snprintf(want, sizeof want, "%.*s%s", (int)n, warps[i].banc, age == DBG_AGE_CHILD ? "_Adult" : "_Child");
    for (int k = 0; k < NWARPS; k++)
        if (!strcmp(warps[k].banc, want) && !strcmp(warps[k].start, warps[i].start)) return k;
    return -1;
}

// ---- keys of the flags used here, hashed once

static uint32_t K_NEXT_BANC, K_START, K_BOOT, K_STARTER, K_CURRENT_BANC, K_LOADING, K_CHILD, K_CONTROL, K_FADE, K_PAUSE;

static void keys_init(void) {
    if (K_NEXT_BANC) return;
    K_START = dbg_flag_key("Sequence_ReservedNextStartPosName");
    K_BOOT = dbg_flag_key("Sequence_ReservedBootEventName");
    K_STARTER = dbg_flag_key("Sequence_ReservedEventStarterName");
    K_CURRENT_BANC = dbg_flag_key("Sequence_CurrentBanc");
    K_LOADING = dbg_flag_key("Sequence_IsLoadingBanc");
    K_CHILD = dbg_flag_key("Sequence_IsChildhood");
    K_CONTROL = dbg_flag_key("Sequence_ControlInEvent");
    K_FADE = dbg_flag_key("Sequence_BancChangeFadePattern");
    K_PAUSE = dbg_flag_key("Sequence_IsEventBancChangePause");
    K_NEXT_BANC = dbg_flag_key("Sequence_ReservedNextBanc");
}

int dbg_current_area(char out[64]) {
    keys_init();
    return dbg_flag_get_string(K_CURRENT_BANC, out);
}

int dbg_is_child(void) {
    keys_init();
    return dbg_flag_get_bool(K_CHILD);
}

// ---- the redirect

enum { W_IDLE, W_ARMED, W_ENFORCING };
#define ENFORCE_NS 20000000000ull   // a redirect stops this long after it first applied, whatever happened

static _Atomic int warp_state, warp_target = -1, warp_thread_on;
static _Atomic uint64_t warp_since;
static _Atomic int warp_saw_loading;
static char warp_note[96];

// Age on arrival. The area's parameters set the age when it loads; with an age chosen here the polling thread writes
// Sequence_IsChildhood over the area's choice from the moment the change is reserved until a few seconds after arrival,
// which covers the creation of the player (the child or the adult pouch and model follow that flag). Areas made for one
// age then hold the other one, with whatever the area's own scripts make of it.
#define AGE_HOLD_NS 4000000000ull
static _Atomic int warp_age = DBG_AGE_ANY;
static _Atomic uint64_t age_hold_until;

void dbg_warp_set_age(int age) { atomic_store(&warp_age, age == DBG_AGE_CHILD || age == DBG_AGE_ADULT ? age : DBG_AGE_ANY); }
int dbg_warp_get_age(void) { return atomic_load(&warp_age); }

static void age_hold(void) {
    int age = atomic_load(&warp_age);
    if (age != DBG_AGE_ANY && dbg_flag_get_bool(K_CHILD) != (age == DBG_AGE_CHILD)) dbg_flag_set_bool(K_CHILD, age == DBG_AGE_CHILD);
}

const char *dbg_warp_note(void) { return warp_note; }
int dbg_warp_armed(void) { return atomic_load(&warp_state) != W_IDLE ? atomic_load(&warp_target) : -1; }

// Writes the destination over a reserved change. Returns 1 when something was reserved (and is now ours).
static int redirect_apply(const struct Warp *w) {
    char cur[64];
    if (!dbg_flag_get_string(K_NEXT_BANC, cur) || !cur[0]) return 0;
    char start[64];
    if (!strcmp(cur, w->banc) && dbg_flag_get_string(K_START, start) && !strcmp(start, w->start)) return 1;
    dbg_flag_set_string(K_NEXT_BANC, w->banc);
    dbg_flag_set_string(K_START, w->start);
    dbg_flag_set_string(K_BOOT, w->boot);
    dbg_flag_set_string(K_STARTER, w->start);
    return 1;
}

// One step of the redirect; from the polling thread and from the frame hook.
static void redirect_step(void) {
    int st = atomic_load(&warp_state), i = atomic_load(&warp_target);
    if (st == W_IDLE || i < 0 || i >= NWARPS) return;
    const struct Warp *w = &warps[i];
    if (redirect_apply(w) && st == W_ARMED) {
        atomic_store(&warp_since, plat_now_ns());
        atomic_store(&warp_saw_loading, 0);
        atomic_store(&warp_state, W_ENFORCING);
        snprintf(warp_note, sizeof warp_note, "Warping to %s", w->banc);
    }
    if (atomic_load(&warp_state) != W_ENFORCING) return;
    age_hold();
    int loading = dbg_flag_get_bool(K_LOADING);
    if (loading == 1) atomic_store(&warp_saw_loading, 1);
    char cur[64];
    int arrived = atomic_load(&warp_saw_loading) && loading == 0 && dbg_flag_get_string(K_CURRENT_BANC, cur) && !strcmp(cur, w->banc);
    if (arrived || plat_now_ns() - atomic_load(&warp_since) > ENFORCE_NS) {
        if (arrived) atomic_store(&age_hold_until, plat_now_ns() + AGE_HOLD_NS);
        atomic_store(&warp_state, W_IDLE);
        snprintf(warp_note, sizeof warp_note, arrived ? "Arrived: %s" : "Warp to %s timed out", w->banc);
    }
}

// The reserve and the read of the destination may fall in one frame, so a thread also polls while a warp is armed.
static void warp_thread(void *arg) {
    (void)arg;
    for (;;) {
        if (atomic_load(&warp_state) != W_IDLE) redirect_step();
        else if (plat_now_ns() < atomic_load(&age_hold_until)) age_hold();
        else break;
        plat_sleep_ns(200000);
    }
    atomic_store(&warp_thread_on, 0);
}

int dbg_warp_arm(int i) {
    if (!dbg_available() || i < 0 || i >= NWARPS) return 0;
    keys_init();
    if (!dbg_flag_addr(DBG_FLAG_STRING, K_NEXT_BANC)) {
        snprintf(warp_note, sizeof warp_note, "%s", dbg_flags_locating() ? "Still looking for the game's flags" : "Flags not found yet");
        return 0;
    }
    atomic_store(&age_hold_until, 0);
    atomic_store(&warp_target, i);
    atomic_store(&warp_state, W_ARMED);
    snprintf(warp_note, sizeof warp_note, "Next area change goes to %s, %s", warps[i].banc, warps[i].start);
    int zero = 0;
    if (atomic_compare_exchange_strong(&warp_thread_on, &zero, 1) && !plat_thread_spawn(warp_thread, NULL, "debug-warp", 0))
        atomic_store(&warp_thread_on, 0);
    return 1;
}

void dbg_warp_cancel(void) {
    atomic_store(&warp_state, W_IDLE);
    snprintf(warp_note, sizeof warp_note, "Warp cancelled");
}

// ---- guest calls from the frame hook

static struct {
    _Atomic int state;   // 0 idle, 1 queued, 2 done
    uint64_t fn, a[8], result;
} gcall;
static char gtext[4][128];
static uint64_t gslot[4];
static _Alignas(64) uint8_t gscratch[0x10000];   // room for request structures the game fills

uint64_t dbg_guest_scratch(void) { return (uint64_t)(uintptr_t)gscratch; }

uint64_t dbg_guest_text(int slot, const char *text, int indirect) {
    if (slot < 0 || slot >= 4) return 0;
    snprintf(gtext[slot], sizeof gtext[slot], "%s", text);
    gslot[slot] = (uint64_t)(uintptr_t)gtext[slot];
    return indirect ? (uint64_t)(uintptr_t)&gslot[slot] : (uint64_t)(uintptr_t)gtext[slot];
}

int dbg_guest_call(uint64_t fn, const uint64_t a[8]) {
    int idle = 0;
    if (!fn || !atomic_compare_exchange_strong(&gcall.state, &idle, 3)) return 0;
    gcall.fn = fn;
    memcpy(gcall.a, a, sizeof gcall.a);
    atomic_store(&gcall.state, 1);
    return 1;
}

int dbg_guest_result(uint64_t *r) {
    if (atomic_load(&gcall.state) != 2) return 0;
    *r = gcall.result;
    atomic_store(&gcall.state, 0);
    return 1;
}

// The player's event performer: the component the game asks to play an event for an actor.
uint64_t dbg_player_performer(void) {
    uint64_t actor = dbg_player_actor(), comp = 0, perf = 0;
    if (!actor || !plat_mem_read(actor + 0x270, &comp, 8) || !comp || !plat_mem_read(comp + 0x90, &perf, 8)) return 0;
    return perf;
}

// Runs a guest function with the hook's registers saved: the hook sits in the middle of the framework's update.
static uint64_t call_saved(Cpu *c, uint64_t fn, const uint64_t a[8]) {
    static __thread Cpu save;
    memcpy(save.x, c->x, sizeof save.x);
    memcpy(save.v, c->v, sizeof save.v);
    uint64_t nzcv = c->nzcv;
    uint64_t r = rt_call(c, fn, 8, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    memcpy(c->x, save.x, sizeof save.x);
    memcpy(c->v, save.v, sizeof save.v);
    c->nzcv = nzcv;
    return r;
}

// Teleport: the game's own warp of an actor to a pose (the routine its "warp to target position" event nodes call), so
// the physics body moves with the actor and the game does not pull it back. The pose is a 3x4 row-major matrix, the
// translation in the last column; the rotation is left upright.
#define GUEST_WARP_ACTOR 0x7000c6fb30ull   // actor, &matrix, 0
#define FRAME_HOOK_ALIVE_NS 1000000000ull
static _Atomic int tp_pending;
static float tp_pos[3];
static _Alignas(16) float tp_matrix[12];
static _Atomic uint64_t frame_seen_ns;

static int frame_hook_alive(void) { return plat_now_ns() - atomic_load(&frame_seen_ns) < FRAME_HOOK_ALIVE_NS; }

int dbg_player_teleport(const float pos[3]) {
    if (!dbg_available() || !frame_hook_alive() || !dbg_player_actor()) return 0;
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) return 0;
    memcpy(tp_pos, pos, sizeof tp_pos);
    atomic_store(&tp_pending, 1);
    return 1;
}

// The actor's own pose is a column-major 3x4 matrix: the three axis columns at +0x2f8 (x, y, z: the facing), then the
// position at +0x31c. A teleport keeps the actor's rotation unless a facing is given (a horizontal direction).
#define ACTOR_AXES 0x2f8
void dbg_frame_teleport(Cpu *c, const float pos[3], const float *face) {
    uint64_t actor = dbg_player_actor();
    float ax[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (!actor) return;
    float len = face ? sqrtf(face[0] * face[0] + face[2] * face[2]) : 0;
    if (len > 1e-4f) {
        float fx = face[0] / len, fz = face[2] / len;
        const float f[9] = {fz, 0, -fx, 0, 1, 0, fx, 0, fz};
        memcpy(ax, f, sizeof ax);
    } else if (!plat_mem_read(actor + ACTOR_AXES, ax, sizeof ax) || !isfinite(ax[0]) || fabsf(ax[4]) < 0.5f) {
        const float f[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // unreadable or tipped over: upright
        memcpy(ax, f, sizeof ax);
    }
    const float m[12] = {ax[0], ax[3], ax[6], pos[0], ax[1], ax[4], ax[7], pos[1], ax[2], ax[5], ax[8], pos[2]};
    memcpy(tp_matrix, m, sizeof m);
    uint64_t a[8] = {actor, (uint64_t)(uintptr_t)tp_matrix, 0};
    call_saved(c, GUEST_WARP_ACTOR, a);
}

static void teleport_run(Cpu *c) { dbg_frame_teleport(c, tp_pos, NULL); }

// Instant warp: with a warp armed, the player is put into the nearest loading zone of the area. The zone reserves its
// area change and the redirect writes the destination over it. An entrance of an area is an actor named
// FieldAndDungeonEntranceEventRequestActor (actor class at main+0x4b25e18, name pointer at +0x20, position at +0x31c)
// standing at the arrival point; its trigger is an actor named Area a few metres away, at ground level, that leads to it
// through [[area+0x3d8]+0x160]. A helper thread finds the triggers in the heap and tries them nearest first, each for up
// to two seconds, as some are off (locked doors, story conditions) or left over from an earlier area. The player goes
// back if none starts a change.
#define ACTOR_CLASS 0x4b25e18ull
#define ENTRANCE_NAME "FieldAndDungeonEntranceEventRequestActor"
#define TRIGGER_NAME "Area"
#define TRIGGER_LINK 0x3d8
#define TRIGGER_LINK_ENTRANCE 0x160
#define TRIGGER_RAISE_M 1.0f   // the trigger's position is its floor
#define ZONE_TRY_NS 2000000000ull
#define HEAP_FROM 0x100000000ull
#define HEAP_TO 0x700000000ull
enum { ZONES_MAX = 64 };
static _Atomic int now_on;

static int actor_named(uint64_t act, const char *want) {
    uint64_t cls = 0, name = 0;
    char text[64];
    size_t n = strlen(want) + 1;
    return act && plat_mem_read(act, &cls, 8) && cls == MOD_MAIN_BASE + ACTOR_CLASS && plat_mem_read(act + 0x20, &name, 8) &&
           name && plat_mem_read(name, text, n) && !memcmp(text, want, n);
}

int dbg_actor_named(uint64_t actor, const char *name) { return actor_named(actor, name); }

// Every actor of the generic actor class, in heap order, with its name (a helper thread's job: about a second). fn
// returns 0 to stop. Returns how many were passed to fn.
int dbg_each_actor(int (*fn)(uint64_t actor, const char *name, void *ctx), void *ctx) {
    enum { CHUNK = 1 << 20 };
    static uint64_t buf[CHUNK / 8];
    static PlatMutex lock;   // zero is unlocked; buf is shared by the callers' threads
    const uint64_t cls = MOD_MAIN_BASE + ACTOR_CLASS;
    int n = 0, go = 1;
    plat_mutex_lock(&lock);
    for (uint64_t addr = HEAP_FROM; addr < HEAP_TO && go;) {
        uint64_t end = 0;
        int st = plat_mem_state(addr, HEAP_TO, &end);
        if (end <= addr) break;
        for (uint64_t at = addr; st == PLAT_PROT_RW && at < end && go; at += CHUNK) {
            uint64_t len = end - at < CHUNK ? end - at : CHUNK;
            if (!plat_mem_read(at, buf, len)) continue;
            for (uint64_t i = 0; i < len / 8 && go; i++) {
                uint64_t name = 0;
                char text[80];
                if (buf[i] != cls || !plat_mem_read(at + i * 8 + 0x20, &name, 8) || !name ||
                    !plat_mem_read(name, text, sizeof text))
                    continue;
                text[sizeof text - 1] = 0;
                n++;
                go = fn(at + i * 8, text, ctx);
            }
        }
        addr = end;
    }
    plat_mutex_unlock(&lock);
    return n;
}

struct find_ctx {
    const char *name;
    uint64_t *out;
    int max, n;
};

static int find_one(uint64_t actor, const char *name, void *ctx) {
    struct find_ctx *f = ctx;
    if (!strcmp(name, f->name)) f->out[f->n++] = actor;
    return f->n < f->max;
}

// Every actor of the generic actor class with this name, in heap order (a helper thread's job: about a second).
int dbg_find_actors(const char *name, uint64_t *out, int max) {
    struct find_ctx f = {name, out, max, 0};
    if (max > 0) dbg_each_actor(find_one, &f);
    return f.n;
}

// The actor an Area trigger acts on: the target of the link record at +0x3d8 (an entrance for a loading zone, an
// event, another Area, a chest...); 0 when it has none.
uint64_t dbg_area_target(uint64_t area) {
    uint64_t link = 0, target = 0;
    if (!plat_mem_read(area + TRIGGER_LINK, &link, 8) || !link || !plat_mem_read(link + TRIGGER_LINK_ENTRANCE, &target, 8))
        return 0;
    return target;
}

// 1 when the Area trigger leads to an entrance (a loading zone): its link's target is the entrance actor.
int dbg_area_is_loading_zone(uint64_t area) { return actor_named(dbg_area_target(area), ENTRANCE_NAME); }

static int find_zones(float zone[ZONES_MAX][3]) {
    enum { AREAS_MAX = 512 };
    static uint64_t area[AREAS_MAX];
    int na = dbg_find_actors(TRIGGER_NAME, area, AREAS_MAX), n = 0;
    for (int k = 0; k < na && n < ZONES_MAX; k++) {
        uint64_t link = 0, entrance = 0;
        float p[3];
        if (!plat_mem_read(area[k] + TRIGGER_LINK, &link, 8) || !link) continue;
        if (!plat_mem_read(link + TRIGGER_LINK_ENTRANCE, &entrance, 8) || !actor_named(entrance, ENTRANCE_NAME)) continue;
        if (!plat_mem_read(area[k] + 0x31c, p, sizeof p) || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) continue;
        if (fabsf(p[0]) > 1e5f || fabsf(p[1]) > 1e5f || fabsf(p[2]) > 1e5f) continue;
        p[1] += TRIGGER_RAISE_M;
        memcpy(zone[n++], p, sizeof p);
    }
    return n;
}

static float dist2(const float a[3], const float b[3]) {
    float d = 0;
    for (int j = 0; j < 3; j++) d += (a[j] - b[j]) * (a[j] - b[j]);
    return d;
}

static void warp_now_thread(void *arg) {
    (void)arg;
    static float zone[ZONES_MAX][3];
    float home[3];
    int n = dbg_player_pos(home) ? find_zones(zone) : 0;
    for (int i = 0; i < n; i++)   // nearest first
        for (int k = i + 1; k < n; k++)
            if (dist2(zone[k], home) < dist2(zone[i], home)) {
                float t[3];
                memcpy(t, zone[i], sizeof t);
                memcpy(zone[i], zone[k], sizeof t);
                memcpy(zone[k], t, sizeof t);
            }
    int started = 0, tried = 0;
    for (int i = 0; i < n && !started && atomic_load(&warp_state) == W_ARMED; i++) {
        if (!dbg_player_teleport(zone[i])) break;
        tried++;
        snprintf(warp_note, sizeof warp_note, "Trying loading zone %d of %d", i + 1, n);
        for (uint64_t t0 = plat_now_ns(); plat_now_ns() - t0 < ZONE_TRY_NS && !started; plat_sleep_ns(20000000ull))
            started = atomic_load(&warp_state) != W_ARMED;
    }
    if (!started) {
        if (tried) dbg_player_teleport(home);
        if (atomic_load(&warp_state) == W_ARMED)
            snprintf(warp_note, sizeof warp_note, "%s; the warp stays armed",
                     n ? "No loading zone here started an area change" : "No loading zone found here");
    }
    atomic_store(&now_on, 0);
}

int dbg_warp_now(void) {
    if (atomic_load(&warp_state) != W_ARMED || !dbg_player_actor()) return 0;
    if (!frame_hook_alive()) {
        snprintf(warp_note, sizeof warp_note, "Instant warps need the debug frame hook (patch set dbgframe)");
        return 0;
    }
    int zero = 0;
    if (!atomic_compare_exchange_strong(&now_on, &zero, 1)) return 1;
    snprintf(warp_note, sizeof warp_note, "Looking for loading zones");
    if (!plat_thread_spawn(warp_now_thread, NULL, "debug-warp-now", 0)) {
        atomic_store(&now_on, 0);
        return 0;
    }
    return 1;
}

// The entrance the player last came in by: the reserved start position is kept for a moment after each area change, so
// the frame hook copies it while it is set. The age switch goes to the same entrance of the other age's file.
static char last_banc[64], last_start[64];

static void remember_start(void) {
    char banc[64], start[64];
    if (!dbg_flag_get_string(K_NEXT_BANC, banc) || !banc[0] || !dbg_flag_get_string(K_START, start) || !start[0]) return;
    memcpy(last_banc, banc, sizeof last_banc);
    memcpy(last_start, start, sizeof last_start);
}

// The same area in the other age's file: the entrance the player came in by when that file has it, else its first one.
static int other_age_warp(int *age_out) {
    char cur[64];
    if (!dbg_current_area(cur)) return -1;
    size_t n = strlen(cur);
    int child = n > 6 && !strcmp(cur + n - 6, "_Child"), adult = n > 6 && !strcmp(cur + n - 6, "_Adult");
    if (!child && !adult) return -1;
    char want[64];
    snprintf(want, sizeof want, "%.*s%s", (int)(n - 6), cur, child ? "_Adult" : "_Child");
    *age_out = child ? DBG_AGE_ADULT : DBG_AGE_CHILD;
    int first = -1;
    for (int k = 0; k < NWARPS; k++) {
        if (strcmp(warps[k].banc, want)) continue;
        if (first < 0) first = k;
        if (!strcmp(last_banc, cur) && !strcmp(warps[k].start, last_start)) return k;
    }
    return first;
}

int dbg_age_switch_target(void) {
    int age;
    keys_init();
    return other_age_warp(&age);
}

int dbg_age_switch(void) {
    int age, i;
    keys_init();
    if ((i = other_age_warp(&age)) < 0) {
        snprintf(warp_note, sizeof warp_note, "This area has no %s version", dbg_is_child() == 1 ? "adult" : "child");
        return 0;
    }
    return dbg_warp_arm(i) && dbg_warp_now();
}

// This area again from the entrance the player last came in by, else its first entrance: a retry for practice.
int dbg_reload_target(void) {
    char cur[64];
    keys_init();
    if (!dbg_current_area(cur)) return -1;
    int first = -1;
    for (int k = 0; k < NWARPS; k++) {
        if (strcmp(warps[k].banc, cur)) continue;
        if (first < 0) first = k;
        if (!strcmp(last_banc, cur) && !strcmp(warps[k].start, last_start)) return k;
    }
    return first;
}

int dbg_last_entrance(char banc[64], char start[64]) {
    if (!last_start[0]) return 0;
    memcpy(banc, last_banc, 64);
    memcpy(start, last_start, 64);
    return 1;
}

int dbg_reload_area(void) {
    int i = dbg_reload_target();
    if (i < 0) {
        snprintf(warp_note, sizeof warp_note, "This area has no entrance in the warp list");
        return 0;
    }
    return dbg_warp_arm(i) && dbg_warp_now();
}

void dbg_game_frame(Cpu *c) {
    if (!dbg_available()) return;
    atomic_store(&frame_seen_ns, plat_now_ns());
    if (atomic_load(&gcall.state) == 1) {
        gcall.result = call_saved(c, gcall.fn, gcall.a);
        atomic_store(&gcall.state, 2);
    }
    if (atomic_exchange(&tp_pending, 0)) teleport_run(c);
    if (atomic_load(&warp_state) != W_IDLE) redirect_step();
    keys_init();
    if (dbg_flag_addr(DBG_FLAG_STRING, K_START)) remember_start();
    dbg_move_frame(c);
}
