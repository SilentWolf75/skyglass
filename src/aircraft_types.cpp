#include "aircraft_types.h"
#include <string.h>
#include <ctype.h>

// Prefix -> category. Scanned top to bottom, first match wins, so SPECIFIC ENTRIES
// MUST COME BEFORE THE GENERAL ONES THEY WOULD OTHERWISE BE SWALLOWED BY.
// The classic traps: C208/C212 are turboprops but start like the Cessna singles;
// C25x are Citation jets; MD5x/MD6x are helicopters while MD8x/MD9x are airliners;
// H25B is a Hawker business jet, not a helicopter.
struct TypeRule { const char *prefix; uint8_t cat; };

static const TypeRule TYPE_RULES[] = {
    // ---- specific light singles / twins (MUST PRECED C17 widebody & P3 turboprop) ----
    {"C170", AC_CAT_LIGHT}, {"C172", AC_CAT_LIGHT}, {"C175", AC_CAT_LIGHT}, {"C177", AC_CAT_LIGHT},
    {"C180", AC_CAT_LIGHT}, {"C182", AC_CAT_LIGHT}, {"C185", AC_CAT_LIGHT},
    {"C150", AC_CAT_LIGHT}, {"C152", AC_CAT_LIGHT}, {"C140", AC_CAT_LIGHT}, {"C120", AC_CAT_LIGHT},
    {"C206", AC_CAT_LIGHT}, {"C210", AC_CAT_LIGHT}, {"C310", AC_CAT_LIGHT}, {"C340", AC_CAT_LIGHT},
    {"P28A", AC_CAT_LIGHT}, {"P28B", AC_CAT_LIGHT}, {"P28R", AC_CAT_LIGHT}, {"PA28", AC_CAT_LIGHT},
    {"PA32", AC_CAT_LIGHT}, {"PA34", AC_CAT_LIGHT}, {"PA44", AC_CAT_LIGHT}, {"P32R", AC_CAT_LIGHT}, {"P34A", AC_CAT_LIGHT},

    // ---- specific Cessna Citations (MUST PRECEDE C5 widebody) ----
    {"C25A", AC_CAT_SMALLJET}, {"C25B", AC_CAT_SMALLJET}, {"C25C", AC_CAT_SMALLJET},
    {"C500", AC_CAT_SMALLJET}, {"C510", AC_CAT_SMALLJET}, {"C525", AC_CAT_SMALLJET},
    {"C550", AC_CAT_SMALLJET}, {"C560", AC_CAT_SMALLJET}, {"C56X", AC_CAT_SMALLJET},
    {"C650", AC_CAT_SMALLJET}, {"C680", AC_CAT_SMALLJET}, {"C750", AC_CAT_SMALLJET},

    // ---- helicopters (before anything that shares a leading letter) ----
    {"EC1", AC_CAT_HELI}, {"EC2", AC_CAT_HELI}, {"EC3", AC_CAT_HELI},
    {"EC4", AC_CAT_HELI}, {"EC5", AC_CAT_HELI}, {"EC6", AC_CAT_HELI}, {"EC7", AC_CAT_HELI},
    {"H120", AC_CAT_HELI}, {"H125", AC_CAT_HELI}, {"H130", AC_CAT_HELI},
    {"H135", AC_CAT_HELI}, {"H140", AC_CAT_HELI}, {"H145", AC_CAT_HELI},
    {"H155", AC_CAT_HELI}, {"H160", AC_CAT_HELI}, {"H175", AC_CAT_HELI},
    {"H215", AC_CAT_HELI}, {"H225", AC_CAT_HELI}, {"H500", AC_CAT_HELI},
    {"H60",  AC_CAT_HELI}, {"UH60", AC_CAT_HELI}, {"CH47", AC_CAT_HELI},
    {"CH53", AC_CAT_HELI}, {"NH90", AC_CAT_HELI}, {"EH10", AC_CAT_HELI},
    {"A109", AC_CAT_HELI}, {"A119", AC_CAT_HELI}, {"A129", AC_CAT_HELI},
    {"A139", AC_CAT_HELI}, {"A149", AC_CAT_HELI}, {"A169", AC_CAT_HELI}, {"A189", AC_CAT_HELI},
    {"AS32", AC_CAT_HELI}, {"AS35", AC_CAT_HELI}, {"AS3B", AC_CAT_HELI},
    {"AS50", AC_CAT_HELI}, {"AS55", AC_CAT_HELI}, {"AS65", AC_CAT_HELI},
    {"AW09", AC_CAT_HELI}, {"AW13", AC_CAT_HELI}, {"AW14", AC_CAT_HELI},
    {"AW16", AC_CAT_HELI}, {"AW18", AC_CAT_HELI}, {"AW89", AC_CAT_HELI},
    {"B06",  AC_CAT_HELI}, {"B407", AC_CAT_HELI}, {"B412", AC_CAT_HELI},
    {"B427", AC_CAT_HELI}, {"B429", AC_CAT_HELI}, {"B430", AC_CAT_HELI}, {"B505", AC_CAT_HELI},
    {"B212", AC_CAT_HELI}, {"B222", AC_CAT_HELI}, {"B230", AC_CAT_HELI}, {"B525", AC_CAT_HELI},
    {"BK17", AC_CAT_HELI}, {"BH06", AC_CAT_HELI},
    {"R22",  AC_CAT_HELI}, {"R44",  AC_CAT_HELI}, {"R66",  AC_CAT_HELI},
    {"S61",  AC_CAT_HELI}, {"S64",  AC_CAT_HELI}, {"S76",  AC_CAT_HELI}, {"S92", AC_CAT_HELI},
    {"MI2",  AC_CAT_HELI}, {"MI8",  AC_CAT_HELI}, {"MI17", AC_CAT_HELI}, {"MI24", AC_CAT_HELI},
    {"KA26", AC_CAT_HELI}, {"KA32", AC_CAT_HELI},
    {"MD5",  AC_CAT_HELI}, {"MD6",  AC_CAT_HELI},
    {"V22",  AC_CAT_HELI},
    {"GAZL", AC_CAT_HELI}, {"LYNX", AC_CAT_HELI}, {"EXPL", AC_CAT_HELI},

    // ---- fighters / fast military jets ----
    {"F14", AC_CAT_FIGHTER}, {"F15", AC_CAT_FIGHTER}, {"F16", AC_CAT_FIGHTER},
    {"F18", AC_CAT_FIGHTER}, {"F22", AC_CAT_FIGHTER}, {"F35", AC_CAT_FIGHTER},
    {"F4",  AC_CAT_FIGHTER}, {"F5",  AC_CAT_FIGHTER}, {"A10", AC_CAT_FIGHTER},
    {"AV8B", AC_CAT_FIGHTER}, {"EUFI", AC_CAT_FIGHTER}, {"RFAL", AC_CAT_FIGHTER},
    {"TORN", AC_CAT_FIGHTER}, {"HAWK", AC_CAT_FIGHTER}, {"J39", AC_CAT_FIGHTER},
    {"JAS39", AC_CAT_FIGHTER}, {"MG29", AC_CAT_FIGHTER}, {"MG31", AC_CAT_FIGHTER},
    {"SU25", AC_CAT_FIGHTER}, {"SU27", AC_CAT_FIGHTER}, {"SU30", AC_CAT_FIGHTER},
    {"SU34", AC_CAT_FIGHTER}, {"SU35", AC_CAT_FIGHTER},
    {"L39", AC_CAT_FIGHTER}, {"T38", AC_CAT_FIGHTER}, {"M346", AC_CAT_FIGHTER},

    // ---- gliders ----
    {"GLID", AC_CAT_GLIDER}, {"DG1", AC_CAT_GLIDER}, {"DG4", AC_CAT_GLIDER},
    {"DG8", AC_CAT_GLIDER}, {"ASK", AC_CAT_GLIDER}, {"ASW", AC_CAT_GLIDER},
    {"ASG", AC_CAT_GLIDER}, {"ASH", AC_CAT_GLIDER}, {"LS4", AC_CAT_GLIDER},
    {"LS8", AC_CAT_GLIDER}, {"DISC", AC_CAT_GLIDER}, {"VENT", AC_CAT_GLIDER},
    {"ARCU", AC_CAT_GLIDER}, {"NIMB", AC_CAT_GLIDER}, {"JANU", AC_CAT_GLIDER},
    {"DUOD", AC_CAT_GLIDER}, {"SF25", AC_CAT_GLIDER},

    // ---- turboprops ----
    {"C208", AC_CAT_TURBOPROP}, {"C212", AC_CAT_TURBOPROP}, {"C441", AC_CAT_TURBOPROP},
    {"C295", AC_CAT_TURBOPROP}, {"C130", AC_CAT_TURBOPROP}, {"C27J", AC_CAT_TURBOPROP},
    {"C30J", AC_CAT_TURBOPROP}, {"C130J", AC_CAT_TURBOPROP},
    {"C160", AC_CAT_TURBOPROP}, {"CN35", AC_CAT_TURBOPROP},
    {"AT4",  AC_CAT_TURBOPROP}, {"AT5",  AC_CAT_TURBOPROP}, {"AT7",  AC_CAT_TURBOPROP},
    {"DH8",  AC_CAT_TURBOPROP}, {"DHC6", AC_CAT_TURBOPROP}, {"DH6",  AC_CAT_TURBOPROP},
    {"SF34", AC_CAT_TURBOPROP}, {"SB20", AC_CAT_TURBOPROP},
    {"SW2",  AC_CAT_TURBOPROP}, {"SW3",  AC_CAT_TURBOPROP}, {"SW4",  AC_CAT_TURBOPROP},
    {"B190", AC_CAT_TURBOPROP}, {"B350", AC_CAT_TURBOPROP},
    {"BE20", AC_CAT_TURBOPROP}, {"BE9",  AC_CAT_TURBOPROP}, {"BE10", AC_CAT_TURBOPROP},
    {"BE30", AC_CAT_TURBOPROP}, {"BE99", AC_CAT_TURBOPROP},
    {"D228", AC_CAT_TURBOPROP}, {"D328", AC_CAT_TURBOPROP},
    {"E110", AC_CAT_TURBOPROP}, {"E120", AC_CAT_TURBOPROP},
    {"F27",  AC_CAT_TURBOPROP}, {"F50",  AC_CAT_TURBOPROP},
    {"AN12", AC_CAT_TURBOPROP}, {"AN24", AC_CAT_TURBOPROP}, {"AN26", AC_CAT_TURBOPROP},
    {"AN28", AC_CAT_TURBOPROP}, {"AN30", AC_CAT_TURBOPROP}, {"AN32", AC_CAT_TURBOPROP},
    {"A400", AC_CAT_TURBOPROP}, {"P3",   AC_CAT_TURBOPROP}, {"P180", AC_CAT_TURBOPROP},
    {"L410", AC_CAT_TURBOPROP}, {"PC12", AC_CAT_TURBOPROP}, {"PC6",  AC_CAT_TURBOPROP},
    {"TBM",  AC_CAT_TURBOPROP}, {"MU2",  AC_CAT_TURBOPROP}, {"Y12",  AC_CAT_TURBOPROP},
    {"EPIC", AC_CAT_TURBOPROP},

    // ---- business jets + regional jets (small swept wings) ----
    {"C25", AC_CAT_SMALLJET}, {"C50", AC_CAT_SMALLJET}, {"C51", AC_CAT_SMALLJET},
    {"C52", AC_CAT_SMALLJET}, {"C55", AC_CAT_SMALLJET}, {"C56", AC_CAT_SMALLJET},
    {"C60", AC_CAT_SMALLJET}, {"C65", AC_CAT_SMALLJET}, {"C68", AC_CAT_SMALLJET},
    {"C70", AC_CAT_SMALLJET}, {"C75", AC_CAT_SMALLJET},
    {"CL30", AC_CAT_SMALLJET}, {"CL35", AC_CAT_SMALLJET}, {"CL60", AC_CAT_SMALLJET},
    {"CL64", AC_CAT_SMALLJET}, {"GALX", AC_CAT_SMALLJET}, {"GLEX", AC_CAT_SMALLJET},
    {"GL5T", AC_CAT_SMALLJET}, {"GL7T", AC_CAT_SMALLJET}, {"GLF", AC_CAT_SMALLJET},
    {"G280", AC_CAT_SMALLJET}, {"G150", AC_CAT_SMALLJET},
    {"LJ",   AC_CAT_SMALLJET}, {"F2TH", AC_CAT_SMALLJET}, {"F900", AC_CAT_SMALLJET},
    {"FA7X", AC_CAT_SMALLJET}, {"FA8X", AC_CAT_SMALLJET}, {"FA50", AC_CAT_SMALLJET},
    {"E50P", AC_CAT_SMALLJET}, {"E55P", AC_CAT_SMALLJET}, {"PRM1", AC_CAT_SMALLJET},
    {"H25",  AC_CAT_SMALLJET}, {"BE40", AC_CAT_SMALLJET}, {"ASTR", AC_CAT_SMALLJET},
    {"HA4T", AC_CAT_SMALLJET}, {"PC24", AC_CAT_SMALLJET}, {"SF50", AC_CAT_SMALLJET},
    {"CRJ",  AC_CAT_SMALLJET}, {"E13",  AC_CAT_SMALLJET}, {"E14",  AC_CAT_SMALLJET},
    {"E17",  AC_CAT_SMALLJET}, {"E19",  AC_CAT_SMALLJET}, {"E29",  AC_CAT_SMALLJET},
    {"E45",  AC_CAT_SMALLJET}, {"E75",  AC_CAT_SMALLJET},
    {"RJ1",  AC_CAT_SMALLJET}, {"RJ7",  AC_CAT_SMALLJET}, {"RJ8",  AC_CAT_SMALLJET},
    {"BA46", AC_CAT_SMALLJET}, {"F70",  AC_CAT_SMALLJET}, {"F100", AC_CAT_SMALLJET},
    {"SU95", AC_CAT_SMALLJET}, {"AN148", AC_CAT_SMALLJET}, {"ARJ",  AC_CAT_SMALLJET},

    // ---- widebodies and large military jets ----
    {"A30",  AC_CAT_WIDE}, {"A31",  AC_CAT_WIDE}, {"A33",  AC_CAT_WIDE},
    {"A34",  AC_CAT_WIDE}, {"A35",  AC_CAT_WIDE}, {"A38",  AC_CAT_WIDE},
    {"A124", AC_CAT_WIDE}, {"A225", AC_CAT_WIDE},
    {"B74",  AC_CAT_WIDE}, {"B76",  AC_CAT_WIDE}, {"B77",  AC_CAT_WIDE},
    {"B78",  AC_CAT_WIDE}, {"MD11", AC_CAT_WIDE},
    {"IL86", AC_CAT_WIDE}, {"IL96", AC_CAT_WIDE}, {"IL76", AC_CAT_WIDE},
    {"AN22", AC_CAT_WIDE}, {"C17",  AC_CAT_WIDE}, {"C5",   AC_CAT_WIDE},
    {"KC10", AC_CAT_WIDE}, {"K35",  AC_CAT_WIDE}, {"KC135", AC_CAT_WIDE},
    {"E3",   AC_CAT_WIDE}, {"E6",   AC_CAT_WIDE}, {"B52",  AC_CAT_WIDE},
    {"B1",   AC_CAT_WIDE}, {"B2",   AC_CAT_WIDE},

    // ---- narrowbody airliners ----
    {"A19",  AC_CAT_NARROW}, {"A20",  AC_CAT_NARROW}, {"A21",  AC_CAT_NARROW},
    {"A318", AC_CAT_NARROW}, {"A319", AC_CAT_NARROW}, {"A320", AC_CAT_NARROW},
    {"A321", AC_CAT_NARROW},
    {"B71",  AC_CAT_NARROW}, {"B72",  AC_CAT_NARROW}, {"B73",  AC_CAT_NARROW},
    {"B75",  AC_CAT_NARROW}, {"B37",  AC_CAT_NARROW}, {"B38",  AC_CAT_NARROW},
    {"B39",  AC_CAT_NARROW}, {"B3XM", AC_CAT_NARROW}, {"P8",   AC_CAT_NARROW},
    {"MD8",  AC_CAT_NARROW}, {"MD9",  AC_CAT_NARROW},
    {"BCS",  AC_CAT_NARROW}, {"C919", AC_CAT_NARROW},
    {"T204", AC_CAT_NARROW}, {"T154", AC_CAT_NARROW}, {"YK42", AC_CAT_NARROW},

    // ---- light aircraft (general prefixes last: they are the greediest) ----
    {"C1",  AC_CAT_LIGHT}, {"C2",  AC_CAT_LIGHT}, {"C3",  AC_CAT_LIGHT},
    {"C4",  AC_CAT_LIGHT}, {"C77", AC_CAT_LIGHT},
    {"PA",  AC_CAT_LIGHT}, {"P28", AC_CAT_LIGHT}, {"P32", AC_CAT_LIGHT},
    {"SR2", AC_CAT_LIGHT}, {"S22T", AC_CAT_LIGHT},
    {"DA2", AC_CAT_LIGHT}, {"DA4", AC_CAT_LIGHT}, {"DA6", AC_CAT_LIGHT},
    {"DV20", AC_CAT_LIGHT}, {"DR40", AC_CAT_LIGHT},
    {"BE1", AC_CAT_LIGHT}, {"BE2", AC_CAT_LIGHT}, {"BE3", AC_CAT_LIGHT},
    {"BE5", AC_CAT_LIGHT}, {"BE7", AC_CAT_LIGHT}, {"BE8", AC_CAT_LIGHT},
    {"M20", AC_CAT_LIGHT}, {"MO20", AC_CAT_LIGHT},
    {"RV",  AC_CAT_LIGHT}, {"AA5", AC_CAT_LIGHT}, {"AC11", AC_CAT_LIGHT},
    {"TOBA", AC_CAT_LIGHT}, {"TB",  AC_CAT_LIGHT}, {"GY80", AC_CAT_LIGHT},
    {"HUSK", AC_CAT_LIGHT}, {"KODI", AC_CAT_LIGHT}, {"LNC",  AC_CAT_LIGHT},
    {"PITS", AC_CAT_LIGHT}, {"J3",   AC_CAT_LIGHT}, {"CH7",  AC_CAT_LIGHT},
    {"WT9",  AC_CAT_LIGHT}, {"VELO", AC_CAT_LIGHT}, {"GLAS", AC_CAT_LIGHT},
    {"YK18", AC_CAT_LIGHT}, {"ZLIN", AC_CAT_LIGHT}, {"EV97", AC_CAT_LIGHT},
    {"P208", AC_CAT_LIGHT}, {"F172", AC_CAT_LIGHT}, {"NAVI", AC_CAT_LIGHT},
};

static const int TYPE_RULES_N = (int)(sizeof(TYPE_RULES) / sizeof(TYPE_RULES[0]));

// ADS-B emitter category -> silhouette. Only the ones that map to a distinct shape are
// listed; A0 (no information) and the surface/obstacle C classes deliberately fall
// through to the guess below rather than pretending to know.
static AcCategory cat_from_emitter(const char *e) {
    if (!e || !e[0] || !e[1]) return AC_CAT_COUNT;      // COUNT = "no opinion"
    if (e[0] == 'A') {
        switch (e[1]) {
            case '1': return AC_CAT_LIGHT;        // < 15 500 lb
            case '2': return AC_CAT_SMALLJET;     // 15 500 - 75 000 lb
            case '3': return AC_CAT_NARROW;       // 75 000 - 300 000 lb
            case '4': return AC_CAT_NARROW;       // high-vortex large (B757)
            case '5': return AC_CAT_WIDE;         // > 300 000 lb
            case '6': return AC_CAT_FIGHTER;      // high performance
            case '7': return AC_CAT_HELI;         // rotorcraft
            default:  break;
        }
    } else if (e[0] == 'B') {
        switch (e[1]) {
            case '1': return AC_CAT_GLIDER;       // glider / sailplane
            case '4': return AC_CAT_LIGHT;        // ultralight / paraglider
            default:  break;
        }
    }
    return AC_CAT_COUNT;
}

AcCategory aircraft_category(const char *icaoType, float altFt, float gsKt,
                             const char *emitter) {
    if (icaoType && icaoType[0]) {
        char t[10];
        int j = 0;
        for (const char *p = icaoType; *p && j < (int)sizeof(t) - 1; ++p) {
            if (*p == ' ' || *p == '-') continue;
            t[j++] = (char)toupper((unsigned char)*p);
        }
        t[j] = 0;

        if (j > 0) {
            for (int i = 0; i < TYPE_RULES_N; ++i) {
                const char *pre = TYPE_RULES[i].prefix;
                if (strncmp(t, pre, strlen(pre)) == 0) return (AcCategory)TYPE_RULES[i].cat;
            }
        }
    }

    // No usable type code. Before guessing from altitude and speed, take what the
    // aircraft says about itself: the aggregators leave `t` blank for anything their
    // registration database does not cover -- a third of the traffic overhead on a
    // typical afternoon -- and every one of those was being drawn as an airliner-ish
    // dart. A rotorcraft that broadcasts A7 should look like a rotorcraft.
    {
        const AcCategory fromEmitter = cat_from_emitter(emitter);
        if (fromEmitter != AC_CAT_COUNT) return fromEmitter;
    }

    // Last resort when neither a type code nor an emitter category is available:
    // low altitude / low speed traffic is predominantly light general aviation.
    if ((altFt > 0.0f && altFt < 12000.0f) || (gsKt > 0.0f && gsKt < 210.0f)) {
        return AC_CAT_LIGHT;
    }
    return AC_CAT_NARROW;
}
