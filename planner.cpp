#include <algorithm>
#include <cassert>
#include <cctype>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
Planner implementing the paper's "graph of substates" algorithm.

- Input (stdin): the validator format without the final action sequence line.
- Output (stdout): the full validator input format, including a synthesized plan.

Important notes:
- The code is aligned to the paper's definitions:
  Definition 3 (substates), Definition 4/5 (validity), and Algorithm 1.
- We do not call into the validator or re-check plans via validator logic.
- Locations loc(p) and action intervals [l(a), r(a)] are inferred from names
  and references in gamma when they are not provided explicitly.
*/

// Global proposition and operator tables (kept aligned with validate.cpp).
std::unordered_map<std::string, int> lv_key;  // local proposition -> key
std::unordered_map<std::string, int> gv_key;  // global proposition -> key
std::unordered_map<int, int> lKey_index;      // local key -> lv index
std::unordered_map<int, int> gKey_index;      // global key -> gv index
std::unordered_map<std::string, int> operator_key = {
    {"G", 0}, {"FG", 1}, {"F", 2}, {"X", 3},
    {"U", 4}, {"NOT", 5}, {"AND", 6}, {"OR", 7}};
std::unordered_map<int, std::string> key_operator = {
    {0, "G"}, {1, "FG"}, {2, "F"}, {3, "X"},
    {4, "U"}, {5, "NOT"}, {6, "AND"}, {7, "OR"}};

// Evaluator pointers that we re-bind to each substate.
int* lv_values = nullptr;
int* gv_values = nullptr;
int lvSize = 0;
int gvSize = 0;

// Initial valuation (kept as vectors for easy copying/resetting).
std::vector<int> initial_lv_values;
std::vector<int> initial_gv_values;

// Locality mapping loc(p): key -> location index, and inverse loc -> keys.
std::unordered_map<int, int> key_loc;                  // key -> loc index (0 for globals)
std::unordered_map<int, std::vector<int>> loc_keys;    // loc index -> local keys at that loc
int nLoc = 0;  // maximum local location index

// Raw lines that should be emitted verbatim before the action sequence.
std::vector<std::string> raw_lines_prefix;

struct NamedFormula {
    std::string text;      // original textual formula (for output/debug)
    class formula* node;   // parsed tree
};

std::vector<NamedFormula> local_formulas;
std::vector<NamedFormula> global_formulas;

// Action theory gamma^-, gamma^+.
std::unordered_map<std::string, std::unordered_map<std::string, formula*>> gammaMinus;
std::unordered_map<std::string, std::unordered_map<std::string, formula*>> gammaPlus;

// If the input includes an action sequence, we can validate or reuse it.
std::vector<std::string> input_actions;

// ----- Formula parsing and evaluation (mirrors validate.cpp) -----

static int getValueForKey(int key) {
    const auto lit = lKey_index.find(key);
    if (lit != lKey_index.end()) {
        assert(lv_values != nullptr);
        assert(lit->second >= 0 && lit->second < lvSize);
        return lv_values[lit->second];
    }

    const auto git = gKey_index.find(key);
    assert(git != gKey_index.end());
    assert(gv_values != nullptr);
    assert(git->second >= 0 && git->second < gvSize);
    return gv_values[git->second];
}

class formula {
    std::vector<formula*> parts;
    std::vector<int> operators;
    std::vector<int> variables;
    std::vector<int> xPrepared;
    std::vector<int> xTrue;
    int xInd = 0;
    std::vector<int> fTrue;
    int fInd = 0;
    std::vector<int> gFalse;
    int gInd = 0;
    int lastAction = 0;
    int fixed = 0;
    int value = 0;
    int TrueConstant = 0;
    int FalseConstant = 0;

   public:
    formula() = default;

    void setLastAction() { lastAction = 1; }

    /*
    Collect all referenced proposition keys in this formula tree.
    This is used to compute action intervals and value locality.
    */
    void collectVariables(std::vector<int>& out) const {
        out.insert(out.end(), variables.begin(), variables.end());
        for (const formula* part : parts) {
            assert(part != nullptr);
            part->collectVariables(out);
        }
    }

    formula(std::string f) {
        fixed = value = 0;
        std::string op = "";
        int ind = 0;
        if (f[0] == '(') {
            if (f[2] == 'F') {
                op += "F";
                if (f[3] == 'G') {
                    if (f[4] == ' ') {
                        op += "G";
                        ind = 4;
                    }
                } else if (f[3] == ' ') {
                    ind = 3;
                }
            } else if (f[2] == 'G') {
                op += "G";
                if (f[3] == 'F') {
                    if (f[4] == ' ') {
                        op += "F";
                        ind = 4;
                    }
                } else if (f[3] == ' ') {
                    ind = 3;
                }
            } else if (f[2] == 'X') {
                op += "X";
                ind = 3;
            } else if (f[2] == 'N' && f[3] == 'O' && f[4] == 'T') {
                op += "NOT";
                ind = 5;
            }

            if (ind == 3 || ind == 4 || ind == 5) {
                if (op != "") {
                    operators.push_back(operator_key[op]);
                }
                std::string subf = "";
                for (int i = ind + 2; i < static_cast<int>(f.size()) - 2; i++) {
                    subf += f[i];
                }
                formula* fs = new formula(subf);
                parts.push_back(fs);
            } else {
                if (f[0] == '(' && f[1] == '(') {
                    std::string tmp = "";
                    for (int i = 1; i < static_cast<int>(f.size()) - 1; i++) {
                        tmp += f[i];
                    }
                    f = tmp;
                }

                std::string subf = "";
                for (int i = 1; i < static_cast<int>(f.size()) - 1; i++) {
                    if (f[i] != ')' && f[i] != '(') {
                        subf += f[i];
                    } else if (f[i] == '(') {
                        continue;
                    } else {
                        formula* fs = new formula(subf);
                        parts.push_back(fs);
                        subf = "";
                        if (i == static_cast<int>(f.size()) - 1) {
                            break;
                        }
                        int j = i + 2;
                        while (f[j] != ' ') {
                            subf += f[j];
                            j++;
                        }
                        if (subf != "") {
                            operators.push_back(operator_key[subf]);
                        }
                        subf = "";
                    }
                }
            }
        } else {
            if (f == "TRUE") {
                TrueConstant = 1;
                return;
            }
            if (f == "FALSE") {
                FalseConstant = 1;
                return;
            }

            if (lv_key.find(f) != lv_key.end()) {
                variables.push_back(lv_key[f]);
                return;
            }
            if (gv_key.find(f) != gv_key.end()) {
                variables.push_back(gv_key[f]);
                return;
            }

            std::string subf = "";
            int i = 0;
            if (f[0] == ' ') {
                i = 1;
            }
            for (; i < static_cast<int>(f.size()); i++) {
                while (i < static_cast<int>(f.size()) && f[i] != ' ') {
                    subf += f[i];
                    i++;
                }
                if (subf == "NOT" || subf == "AND" || subf == "OR") {
                    if (subf != "") {
                        operators.push_back(operator_key[subf]);
                    }
                    subf = "";
                    i++;
                    int c = 0;
                    if (f[i] == '(') {
                        i++;
                        c++;
                    }
                    while (i < static_cast<int>(f.size()) && f[i] != ')') {
                        if (f[i] == '(') {
                            c++;
                        }
                        subf += f[i];
                        i++;
                    }
                    if (c == 2) {
                        subf += ")";
                    }
                    formula* fs = new formula(subf);
                    parts.push_back(fs);
                    subf = "";
                } else {
                    if (lv_key.find(subf) != lv_key.end() || gv_key.find(subf) != gv_key.end()) {
                        formula* fs = new formula(subf);
                        parts.push_back(fs);
                        subf = "";
                    }
                }

                subf = "";
                if (i == static_cast<int>(f.size()) - 1) {
                    break;
                }

                while (i < static_cast<int>(f.size()) && f[i] != ' ' && f[i] != ')') {
                    subf += f[i];
                    i++;
                }
                if (subf != "") {
                    operators.push_back(operator_key[subf]);
                }
            }
        }
    }

    int evaluate() {
        if (parts.empty()) {
            if (TrueConstant) {
                return 1;
            }
            if (FalseConstant) {
                return 0;
            }

            assert(!variables.empty());

            if (variables.size() == 1) {
                const int varA = variables[0];
                return getValueForKey(varA);
            }

            int truth = 0;
            assert(variables.size() >= 2);
            assert(!operators.empty());
            const int varA = variables[0];
            const int varB = variables[1];

            const int val1 = getValueForKey(varA);
            const int val2 = getValueForKey(varB);

            const std::string op = key_operator[operators[0]];
            if (op == "AND") {
                truth = val1 && val2;
                return truth;
            }
            if (op == "OR") {
                truth = val1 || val2;
                return truth;
            }
            return truth;
        }

        int truth = 0;
        int partsInd = 0;
        for (int i = 0; i < static_cast<int>(operators.size()); i++) {
            const std::string op = key_operator[operators[i]];

            if (op == "NOT") {
                assert(partsInd < static_cast<int>(parts.size()));
                truth = parts[partsInd]->evaluate();
                truth = 1 - truth;
                partsInd++;
            } else if (op == "X") {
                if (static_cast<int>(xPrepared.size()) <= xInd) {
                    xPrepared.push_back(1);
                    xInd++;
                    truth = 1;
                } else {
                    if (static_cast<int>(xTrue.size()) <= xInd) {
                        assert(partsInd < static_cast<int>(parts.size()));
                        truth = parts[partsInd]->evaluate();
                        xTrue.push_back(truth);
                        xInd++;
                        partsInd++;
                    } else {
                        truth = xTrue[xInd];
                    }
                }
            } else if (op == "F") {
                if (static_cast<int>(fTrue.size()) <= fInd) {
                    assert(partsInd < static_cast<int>(parts.size()));
                    truth = parts[partsInd]->evaluate();
                    partsInd++;
                    if (truth == 1) {
                        fTrue.push_back(1);
                        fInd++;
                    }
                } else {
                    truth = 1;
                }
            } else if (op == "G") {
                if (static_cast<int>(gFalse.size()) <= gInd) {
                    assert(partsInd < static_cast<int>(parts.size()));
                    truth = parts[partsInd]->evaluate();
                    partsInd++;
                    if (truth == 0) {
                        gFalse.push_back(1);
                        gInd++;
                    }
                } else {
                    truth = 0;
                }
            } else if (op == "FG") {
                if (lastAction == 1) {
                    assert(partsInd < static_cast<int>(parts.size()));
                    truth = parts[partsInd]->evaluate();
                    partsInd++;
                } else {
                    truth = 1;
                }
            } else if (op == "AND") {
                if (i + 1 < static_cast<int>(operators.size()) &&
                    key_operator[operators[i + 1]] == "NOT") {
                    assert(partsInd < static_cast<int>(parts.size()));
                    truth = truth && (1 - parts[partsInd]->evaluate());
                    partsInd++;
                    i++;
                } else {
                    assert(partsInd < static_cast<int>(parts.size()));
                    truth = truth && parts[partsInd]->evaluate();
                    partsInd++;
                }
            } else if (op == "OR") {
                if (i + 1 < static_cast<int>(operators.size()) &&
                    key_operator[operators[i + 1]] == "NOT") {
                    assert(partsInd < static_cast<int>(parts.size()));
                    truth = truth || (1 - parts[partsInd]->evaluate());
                    partsInd++;
                    i++;
                } else {
                    assert(partsInd < static_cast<int>(parts.size()));
                    truth = truth || parts[partsInd]->evaluate();
                    partsInd++;
                }
            }
        }
        xInd = fInd = gInd = 0;
        return truth;
    }
};

// ----- Locality helpers -----

static void bindState(const std::vector<int>& lv, const std::vector<int>& gv) {
    assert(!lv.empty());
    lv_values = const_cast<int*>(lv.data());
    gv_values = gv.empty() ? nullptr : const_cast<int*>(gv.data());
    lvSize = static_cast<int>(lv.size());
    gvSize = static_cast<int>(gv.size());
}

static std::string trim(const std::string& s) {
    int a = 0;
    int b = static_cast<int>(s.size()) - 1;
    while (a <= b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b >= a && std::isspace(static_cast<unsigned char>(s[b]))) b--;
    return (a <= b) ? s.substr(a, b - a + 1) : std::string();
}

// Remove a single layer of outer parentheses if they wrap the whole string.
static std::string stripOuterParens(const std::string& s) {
    std::string cur = trim(s);
    while (!cur.empty() && cur.front() == '(' && cur.back() == ')') {
        int depth = 0;
        bool wrapsAll = true;
        for (int i = 0; i < static_cast<int>(cur.size()); i++) {
            if (cur[i] == '(') depth++;
            if (cur[i] == ')') depth--;
            if (depth == 0 && i != static_cast<int>(cur.size()) - 1) {
                wrapsAll = false;
                break;
            }
        }
        if (!wrapsAll) break;
        cur = trim(cur.substr(1, cur.size() - 2));
    }
    return cur;
}

// Infer location index loc(p) from a local proposition name like "on7".
static int locFromName(const std::string& name, int fallbackLoc) {
    int i = static_cast<int>(name.size()) - 1;
    while (i >= 0 && std::isdigit(static_cast<unsigned char>(name[i]))) i--;
    if (i < static_cast<int>(name.size()) - 1) {
        const std::string digits = name.substr(i + 1);
        return std::max(1, std::stoi(digits));
    }
    // If there is no numeric suffix, fall back to the insertion order.
    return std::max(1, fallbackLoc);
}

static bool locIsActive(int loc, int i, int L) {
    if (loc <= 0) return true;
    const int recentStart = std::max(1, i - L + 1);
    const int activeEnd = std::min(nLoc, i + 2 * L);
    return loc >= recentStart && loc <= activeEnd;
}

// ----- Value parsing and indexing -----

enum class ValueType { FG, F, U, G, OTHER };

struct Value {
    int id = -1;
    std::string raw;          // raw value text
    ValueType type = ValueType::OTHER;
    std::string psi1_text;    // propositional component(s)
    std::string psi2_text;
    formula* psi1 = nullptr;  // parsed propositional formula(s)
    formula* psi2 = nullptr;
    std::vector<int> local_locs;  // referenced local locations (sorted unique)
    bool has_global = false;
};

std::vector<Value> values;

// Indexes to quickly find values relevant to a location or globals.
std::unordered_map<int, std::vector<int>> values_by_loc;  // loc -> value ids
std::vector<int> global_value_ids;                        // value ids with globals
std::vector<int> always_value_ids;                        // value ids of type G

// Split on a top-level " U " token (depth 0), if present.
static std::optional<std::pair<std::string, std::string>> splitTopLevelU(const std::string& s) {
    int depth = 0;
    for (int i = 0; i + 2 < static_cast<int>(s.size()); i++) {
        const char c = s[i];
        if (c == '(') depth++;
        if (c == ')') depth--;
        if (depth == 0 && s[i] == ' ' && s[i + 1] == 'U' && s[i + 2] == ' ') {
            const std::string left = trim(s.substr(0, i));
            const std::string right = trim(s.substr(i + 3));
            return std::make_pair(left, right);
        }
    }
    return std::nullopt;
}

static Value parseValue(const std::string& text, int id) {
    Value v;
    v.id = id;
    v.raw = text;

    // Normalize the outer structure to make operator detection easier.
    std::string core = stripOuterParens(text);

    if (const auto uSplit = splitTopLevelU(core)) {
        v.type = ValueType::U;
        v.psi1_text = stripOuterParens(uSplit->first);
        v.psi2_text = stripOuterParens(uSplit->second);
        v.psi1 = new formula(v.psi1_text);
        v.psi2 = new formula(v.psi2_text);
    } else {
        core = trim(core);
        // Detect FG/F/G prefix operators.
        auto startsWithOp = [&](const std::string& op) {
            return core.rfind(op, 0) == 0 &&
                   (core.size() == op.size() || std::isspace(static_cast<unsigned char>(core[op.size()])) || core[op.size()] == '(');
        };

        if (startsWithOp("FG")) {
            v.type = ValueType::FG;
            v.psi1_text = stripOuterParens(trim(core.substr(2)));
        } else if (startsWithOp("F")) {
            v.type = ValueType::F;
            v.psi1_text = stripOuterParens(trim(core.substr(1)));
        } else if (startsWithOp("G")) {
            v.type = ValueType::G;
            v.psi1_text = stripOuterParens(trim(core.substr(1)));
        } else {
            v.type = ValueType::OTHER;
            v.psi1_text = core;
        }
        v.psi1 = new formula(v.psi1_text);
    }

    // Collect referenced locations by walking the propositional parts.
    std::vector<int> vars;
    if (v.psi1) v.psi1->collectVariables(vars);
    if (v.psi2) v.psi2->collectVariables(vars);

    std::set<int> locs;
    for (int key : vars) {
        const auto itLoc = key_loc.find(key);
        if (itLoc == key_loc.end()) continue;
        const int loc = itLoc->second;
        if (loc == 0) {
            v.has_global = true;
        } else {
            locs.insert(loc);
        }
    }
    v.local_locs.assign(locs.begin(), locs.end());

    return v;
}

static void buildValuesIndex() {
    values.clear();
    values_by_loc.clear();
    global_value_ids.clear();
    always_value_ids.clear();

    int id = 0;
    auto addValue = [&](const NamedFormula& nf) {
        Value v = parseValue(nf.text, id);
        values.push_back(v);
        for (int loc : v.local_locs) {
            values_by_loc[loc].push_back(id);
        }
        if (v.has_global) {
            global_value_ids.push_back(id);
        }
        if (v.type == ValueType::G) {
            always_value_ids.push_back(id);
        }
        id++;
    };

    for (const NamedFormula& nf : local_formulas) addValue(nf);
    for (const NamedFormula& nf : global_formulas) addValue(nf);
}

// ----- Actions and intervals -----

struct ActionInfo {
    std::string name;
    int minLoc = std::numeric_limits<int>::max();
    int maxLoc = std::numeric_limits<int>::min();
    std::vector<int> locs;  // sorted unique local locations mentioned/affected
};

std::vector<ActionInfo> actionsList;
std::unordered_map<std::string, ActionInfo> actionInfoByName;

static void addLocToAction(ActionInfo& info, int loc) {
    if (loc <= 0) return;
    info.minLoc = std::min(info.minLoc, loc);
    info.maxLoc = std::max(info.maxLoc, loc);
    info.locs.push_back(loc);
}

static void collectActionLocsFromFormula(ActionInfo& info, formula* f) {
    if (!f) return;
    std::vector<int> vars;
    f->collectVariables(vars);
    for (int key : vars) {
        const auto itLoc = key_loc.find(key);
        if (itLoc == key_loc.end()) continue;
        addLocToAction(info, itLoc->second);
    }
}

static void buildActionInfo() {
    actionInfoByName.clear();

    auto ensureAction = [&](const std::string& name) -> ActionInfo& {
        auto it = actionInfoByName.find(name);
        if (it != actionInfoByName.end()) return it->second;
        ActionInfo info;
        info.name = name;
        auto [pos, _] = actionInfoByName.emplace(name, std::move(info));
        return pos->second;
    };

    // Walk gamma^- and gamma^+ to gather referenced and affected locations.
    // This defines the inferred action interval [l(a), r(a)] from the paper.
    for (const auto& entry : gammaMinus) {
        ActionInfo& info = ensureAction(entry.first);
        for (const auto& propEntry : entry.second) {
            const std::string& prop = propEntry.first;
            formula* f = propEntry.second;
            const auto itKey = lv_key.find(prop);
            if (itKey != lv_key.end()) {
                addLocToAction(info, key_loc[itKey->second]);
            }
            collectActionLocsFromFormula(info, f);
        }
    }

    for (const auto& entry : gammaPlus) {
        ActionInfo& info = ensureAction(entry.first);
        for (const auto& propEntry : entry.second) {
            const std::string& prop = propEntry.first;
            formula* f = propEntry.second;
            const auto itKey = lv_key.find(prop);
            if (itKey != lv_key.end()) {
                addLocToAction(info, key_loc[itKey->second]);
            }
            collectActionLocsFromFormula(info, f);
        }
    }

    // Finalize the action list with deduped/sorted location sets.
    actionsList.clear();
    actionsList.reserve(actionInfoByName.size());
    for (auto& kv : actionInfoByName) {
        ActionInfo& info = kv.second;
        std::sort(info.locs.begin(), info.locs.end());
        info.locs.erase(std::unique(info.locs.begin(), info.locs.end()), info.locs.end());
        if (info.locs.empty()) {
            info.minLoc = std::numeric_limits<int>::max();
            info.maxLoc = std::numeric_limits<int>::min();
        }
        actionsList.push_back(info);
    }

    // Make action iteration deterministic.
    std::sort(actionsList.begin(), actionsList.end(), [](const ActionInfo& a, const ActionInfo& b) {
        return a.name < b.name;
    });
}

static bool actionFitsInterval(const ActionInfo& info, int i, int L) {
    if (info.locs.empty()) return true;  // global-only actions can apply anywhere
    const int left = i + 1;
    const int right = i + 2 * L;
    return info.minLoc >= left && info.maxLoc <= right;
}

// ----- Substate search -----

/*
Substate S = (i, T, Theta) from Definition 3 in the paper:
- i is the leading index of the mutable interval [i+1, i+2L].
- T is represented implicitly by the current valuation (lv/gv), restricted
  to active propositions when needed.
- Theta stores satisfied values of the form F psi and psi1 U psi2.
*/
struct Substate {
    int i = 0;                          // mutable interval starts at i+1
    std::vector<int> lv;                // full local valuation (active subset matters)
    std::vector<int> gv;                // full global valuation
    std::unordered_set<int> theta;      // satisfied F/U value ids
    std::unordered_set<int> u_broken;   // U values that can no longer be satisfied
};

static bool stateModels(formula* psi, const Substate& s) {
    assert(psi != nullptr);
    bindState(s.lv, s.gv);
    return psi->evaluate() == 1;
}

static bool valueOnlyActive(const Value& v, int i, int L) {
    for (int loc : v.local_locs) {
        if (!locIsActive(loc, i, L)) return false;
    }
    return true;
}

/*
Theta update from Definition 4 / Definition 5:
- Add any active F psi that is satisfied now.
- Add any active psi1 U psi2 that becomes satisfied now.

We also maintain u_broken to prevent incorrectly adding a U value after it
has already been violated (psi1 false while psi2 not yet true). This is not
used as pruning; it only preserves intended U semantics.
*/
static void updateTheta(Substate& s, int L) {
    for (const Value& v : values) {
        if (!valueOnlyActive(v, s.i, L)) continue;
        if (v.type == ValueType::F) {
            if (stateModels(v.psi1, s)) {
                s.theta.insert(v.id);
            }
        } else if (v.type == ValueType::U) {
            if (s.theta.find(v.id) != s.theta.end()) {
                continue;
            }
            if (s.u_broken.find(v.id) != s.u_broken.end()) {
                continue;
            }
            const bool psi2True = (v.psi2 != nullptr) && stateModels(v.psi2, s);
            if (psi2True) {
                s.theta.insert(v.id);
                continue;
            }
            const bool psi1True = (v.psi1 != nullptr) && stateModels(v.psi1, s);
            if (!psi1True) {
                s.u_broken.insert(v.id);
            }
        }
    }
}

static bool checkAlwaysConstraints(const Substate& s, int L) {
    // Enforce G-values on every step when their locals are active.
    for (int id : always_value_ids) {
        const Value& v = values[id];
        if (!valueOnlyActive(v, s.i, L)) continue;
        if (!stateModels(v.psi1, s)) return false;
    }
    return true;
}

static bool valueContainsLoc(const Value& v, int loc) {
    return std::binary_search(v.local_locs.begin(), v.local_locs.end(), loc);
}

/*
Global validity from Definition 4:
- FG psi must hold now.
- F psi and psi1 U psi2 must already be in Theta.
*/
static bool globalValid(const Substate& s, int L) {
    for (int id : global_value_ids) {
        const Value& v = values[id];
        if (!valueOnlyActive(v, s.i, L)) continue;
        if (v.type == ValueType::FG) {
            if (!stateModels(v.psi1, s)) return false;
        } else if (v.type == ValueType::F || v.type == ValueType::U) {
            if (s.theta.find(id) == s.theta.end()) return false;
        } else if (v.type == ValueType::G) {
            if (!stateModels(v.psi1, s)) return false;
        }
    }
    return true;
}

/*
Local validity (Def. 5 in the paper) for the location that is about to leave
mutable status. We check only values where all other locals are recent.
*/
static bool localLocValid(const Substate& s, int targetLoc, int L) {
    if (targetLoc <= 0) return true;
    const int recentStart = std::max(1, s.i - L + 1);
    const int recentEnd = s.i;

    auto otherLocsRecent = [&](const Value& v) {
        if (!valueContainsLoc(v, targetLoc)) return false;
        for (int loc : v.local_locs) {
            if (loc == targetLoc) continue;
            if (loc < recentStart || loc > recentEnd) return false;
        }
        return true;
    };

    const auto itVals = values_by_loc.find(targetLoc);
    if (itVals == values_by_loc.end()) return true;

    for (int id : itVals->second) {
        const Value& v = values[id];
        if (!otherLocsRecent(v)) continue;
        if (v.type == ValueType::FG) {
            if (!stateModels(v.psi1, s)) return false;
        } else if (v.type == ValueType::F || v.type == ValueType::U) {
            if (s.theta.find(id) == s.theta.end()) return false;
        } else if (v.type == ValueType::G) {
            if (!stateModels(v.psi1, s)) return false;
        }
    }
    return true;
}

/*
Final vertex check from the paper:
- i must equal n-2L (handled by the caller).
- All global propositions must be valid (Definition 4).
- All local propositions with loc(p) > i must be valid (Definition 5).
*/
static bool finalValid(Substate& s, int L) {
    updateTheta(s, L);
    if (!checkAlwaysConstraints(s, L)) return false;
    if (!globalValid(s, L)) return false;

    for (int loc = s.i + 1; loc <= nLoc; loc++) {
        if (!localLocValid(s, loc, L)) return false;
    }
    return true;
}

// Remove from Theta any value that references a dropped location.
static void dropLocFromTheta(Substate& s, int dropLoc) {
    if (dropLoc <= 0) return;
    const auto itVals = values_by_loc.find(dropLoc);
    if (itVals == values_by_loc.end()) return;
    for (int id : itVals->second) {
        s.theta.erase(id);
        s.u_broken.erase(id);
    }
}

// Reset newly activated locations to their initial values.
static void addLocFromInitial(Substate& s, int addLoc) {
    if (addLoc <= 0 || addLoc > nLoc) return;
    const auto itKeys = loc_keys.find(addLoc);
    if (itKeys == loc_keys.end()) return;
    for (int key : itKeys->second) {
        const int idx = lKey_index[key];
        assert(idx >= 0 && idx < static_cast<int>(initial_lv_values.size()));
        s.lv[idx] = initial_lv_values[idx];
    }
}

static void applyAction(Substate& next, const Substate& cur, const ActionInfo& action, int L) {
    next = cur;  // start from the current valuation and Theta
    bindState(cur.lv, cur.gv);

    const int mutableLeft = cur.i + 1;
    const int mutableRight = cur.i + 2 * L;

    auto canAffect = [&](const std::string& prop) {
        const auto itLv = lv_key.find(prop);
        if (itLv != lv_key.end()) {
            const int loc = key_loc[itLv->second];
            return loc >= mutableLeft && loc <= mutableRight;
        }
        return true;  // globals can always be affected
    };

    // Type 1 edge from Definition 6: apply gamma^- then gamma^+.
    const auto minusIt = gammaMinus.find(action.name);
    if (minusIt != gammaMinus.end()) {
        for (const auto& entry : minusIt->second) {
            const std::string& prop = entry.first;
            if (!canAffect(prop)) continue;
            formula* f = entry.second;
            assert(f != nullptr);
            if (f->evaluate() == 1) {
                const auto itLv = lv_key.find(prop);
                if (itLv != lv_key.end()) {
                    const int key = itLv->second;
                    next.lv[lKey_index[key]] = 0;
                } else {
                    const int key = gv_key[prop];
                    next.gv[gKey_index[key]] = 0;
                }
            }
        }
    }

    const auto plusIt = gammaPlus.find(action.name);
    if (plusIt != gammaPlus.end()) {
        for (const auto& entry : plusIt->second) {
            const std::string& prop = entry.first;
            if (!canAffect(prop)) continue;
            formula* f = entry.second;
            assert(f != nullptr);
            if (f->evaluate() == 1) {
                const auto itLv = lv_key.find(prop);
                if (itLv != lv_key.end()) {
                    const int key = itLv->second;
                    next.lv[lKey_index[key]] = 1;
                } else {
                    const int key = gv_key[prop];
                    next.gv[gKey_index[key]] = 1;
                }
            }
        }
    }

    // Theta' is Theta plus any newly satisfied active F/U values.
    updateTheta(next, L);
}

// Signature for visited(S) from the paper's algorithm.
static std::string signature(const Substate& s, int L) {
    std::string out;
    out.reserve(256);
    auto appendInt = [&](int v) {
        out.append(std::to_string(v));
        out.push_back(',');
    };

    appendInt(s.i);

    const int recentStart = std::max(1, s.i - L + 1);
    const int activeEnd = std::min(nLoc, s.i + 2 * L);
    for (int loc = recentStart; loc <= activeEnd; loc++) {
        const auto itKeys = loc_keys.find(loc);
        if (itKeys == loc_keys.end()) continue;
        appendInt(loc);
        for (int key : itKeys->second) {
            appendInt(s.lv[lKey_index[key]]);
        }
    }

    appendInt(static_cast<int>(s.gv.size()));
    for (int v : s.gv) appendInt(v);

    std::vector<int> thetaSorted(s.theta.begin(), s.theta.end());
    std::sort(thetaSorted.begin(), thetaSorted.end());
    appendInt(static_cast<int>(thetaSorted.size()));
    for (int id : thetaSorted) appendInt(id);

    std::vector<int> brokenSorted(s.u_broken.begin(), s.u_broken.end());
    std::sort(brokenSorted.begin(), brokenSorted.end());
    appendInt(static_cast<int>(brokenSorted.size()));
    for (int id : brokenSorted) appendInt(id);

    return out;
}

/*
DFS over substates. Action edges append to the plan; move edges do not.
This matches the paper's "take action edges along the path" reconstruction.
*/
static bool dfs(Substate& cur,
                int L,
                int maxIndex,
                std::vector<std::string>& plan,
                std::unordered_set<std::string>& visited) {
    // visited(S) from Algorithm 1: skip substates already explored.
    const std::string sig = signature(cur, L);
    if (visited.find(sig) != visited.end()) {
        return false;
    }
    visited.insert(sig);

    if (cur.i == maxIndex) {
        Substate tmp = cur;
        if (finalValid(tmp, L)) return true;
    }

    // Type 1 edges: apply any action that fits the current mutable window.
    for (const ActionInfo& action : actionsList) {
        if (!actionFitsInterval(action, cur.i, L)) continue;
        Substate next;
        applyAction(next, cur, action, L);
        if (!checkAlwaysConstraints(next, L)) continue;
        if (!globalValid(next, L)) continue;

        plan.push_back(action.name);
        if (dfs(next, L, maxIndex, plan, visited)) {
            cur = std::move(next);
            return true;
        }
        plan.pop_back();
    }

    // Type 2 edge from Definition 6: move right if the leaving loc is valid.
    if (cur.i < maxIndex) {
        const int targetLoc = cur.i + 1;
        if (localLocValid(cur, targetLoc, L)) {
            Substate next = cur;
            next.i = cur.i + 1;

            const int dropLoc = next.i - L;
            const int addLoc = next.i + 2 * L;

            dropLocFromTheta(next, dropLoc);
            addLocFromInitial(next, addLoc);
            // Theta' removes values tied to the dropped loc and adds new ones.
            updateTheta(next, L);

            if (checkAlwaysConstraints(next, L) && globalValid(next, L)) {
                if (dfs(next, L, maxIndex, plan, visited)) {
                    cur = std::move(next);
                    return true;
                }
            }
        }
    }

    return false;
}

static std::vector<std::string> synthesizePlan(int L) {
    const int maxIndex = std::max(0, nLoc - 2 * L);

    // Initial substate S0 from Definition 3 (i=0, T from s0, Theta from s0).
    Substate initial;
    initial.i = 0;
    initial.lv = initial_lv_values;
    initial.gv = initial_gv_values;
    updateTheta(initial, L);

    if (!checkAlwaysConstraints(initial, L) || !globalValid(initial, L)) {
        return {};
    }

    std::vector<std::string> plan;
    std::unordered_set<std::string> visited;
    Substate cur = initial;
    const bool found = dfs(cur, L, maxIndex, plan, visited);
    if (!found) plan.clear();
    return plan;
}

// ----- Input parsing and output emission -----

/*
Parse the validator input format.
We store all non-action lines verbatim in raw_lines_prefix so we can re-emit
exactly what the validator expects, with a synthesized final line.
*/
static void readProblem(std::istream& input) {
    int key = 0;
    int lind = 0;
    int gind = 0;
    std::string s;
    int line = 1;

    while (std::getline(input, s)) {
        if (!s.empty() && s.back() == '\r') s.pop_back();
        if (s.empty()) continue;

        if (line == 1) {
            raw_lines_prefix.push_back(s);
            std::string tmp = "";
            for (int i = 0; i < static_cast<int>(s.size()); i++) {
                if (s[i] != ',') {
                    tmp += s[i];
                    if (i == static_cast<int>(s.size()) - 1) {
                        lv_key[tmp] = key;
                        lKey_index[key] = lind;
                        const int loc = locFromName(tmp, lind + 1);
                        key_loc[key] = loc;
                        loc_keys[loc].push_back(key);
                        nLoc = std::max(nLoc, loc);
                        key++;
                        lind++;
                        tmp = "";
                    }
                } else {
                    lv_key[tmp] = key;
                    lKey_index[key] = lind;
                    const int loc = locFromName(tmp, lind + 1);
                    key_loc[key] = loc;
                    loc_keys[loc].push_back(key);
                    nLoc = std::max(nLoc, loc);
                    key++;
                    lind++;
                    i++;
                    tmp = "";
                }
            }
        } else if (line == 2) {
            raw_lines_prefix.push_back(s);
            std::string tmp = "";
            for (int i = 0; i < static_cast<int>(s.size()); i++) {
                if (s[i] != ',') {
                    tmp += s[i];
                    if (i == static_cast<int>(s.size()) - 1) {
                        gv_key[tmp] = key;
                        gKey_index[key] = gind;
                        key_loc[key] = 0;
                        key++;
                        gind++;
                        tmp = "";
                    }
                } else {
                    gv_key[tmp] = key;
                    gKey_index[key] = gind;
                    key_loc[key] = 0;
                    key++;
                    gind++;
                    i++;
                    tmp = "";
                }
            }
        } else if (line == 3) {
            raw_lines_prefix.push_back(s);
            initial_lv_values.assign(lind, 0);
            initial_gv_values.assign(gind, 0);

            std::string tmp = "";
            int ind = 0;
            for (int i = 0; i < static_cast<int>(s.size()); i++) {
                if (s[i] != ',') {
                    tmp += s[i];
                    if (i == static_cast<int>(s.size()) - 1) {
                        initial_lv_values[ind] = (tmp == "TRUE") ? 1 : 0;
                        ind++;
                        tmp = "";
                    }
                } else {
                    initial_lv_values[ind] = (tmp == "TRUE") ? 1 : 0;
                    ind++;
                    i++;
                    tmp = "";
                }
            }
        } else if (line == 4) {
            raw_lines_prefix.push_back(s);
            std::string tmp = "";
            int ind = 0;
            for (int i = 0; i < static_cast<int>(s.size()); i++) {
                if (s[i] != ',') {
                    tmp += s[i];
                    if (i == static_cast<int>(s.size()) - 1) {
                        initial_gv_values[ind] = (tmp == "TRUE") ? 1 : 0;
                        ind++;
                        tmp = "";
                    }
                } else {
                    initial_gv_values[ind] = (tmp == "TRUE") ? 1 : 0;
                    ind++;
                    i++;
                    tmp = "";
                }
            }
        } else {
            if (s[0] == '-') {
                raw_lines_prefix.push_back(s);
                std::string act, prop, form;
                int part = 0;
                for (int i = 2; i < static_cast<int>(s.size()); i++) {
                    if (part == 0 && s[i] != ' ') act += s[i];
                    else if (part == 0 && s[i] == ' ') part++;
                    else if (part == 1 && s[i] != ' ') prop += s[i];
                    else if (part == 1 && s[i] == ' ') {
                        part++;
                        i += 2;
                    } else if (part == 2) form += s[i];
                }
                gammaMinus[act][prop] = new formula(form);
            } else if (s[0] == '+') {
                raw_lines_prefix.push_back(s);
                std::string act, prop, form;
                int part = 0;
                for (int i = 2; i < static_cast<int>(s.size()); i++) {
                    if (part == 0 && s[i] != ' ') act += s[i];
                    else if (part == 0 && s[i] == ' ') part++;
                    else if (part == 1 && s[i] != ' ') prop += s[i];
                    else if (part == 1 && s[i] == ' ') {
                        part++;
                        i += 2;
                    } else if (part == 2) form += s[i];
                }
                gammaPlus[act][prop] = new formula(form);
            } else if (s[0] == 'l' && s.size() > 2 && s[2] == ':') {
                raw_lines_prefix.push_back(s);
                std::string st = "";
                for (int i = 4; i < static_cast<int>(s.size()); i++) st += s[i];
                local_formulas.push_back({st, new formula(st)});
            } else if (s[0] == 'g' && s.size() > 2 && s[2] == ':') {
                raw_lines_prefix.push_back(s);
                std::string st = "";
                for (int i = 4; i < static_cast<int>(s.size()); i++) st += s[i];
                global_formulas.push_back({st, new formula(st)});
            } else {
                // Treat any other trailing line as a provided action sequence.
                std::string act;
                for (int i = 0; i < static_cast<int>(s.size()); i++) {
                    if (s[i] != ' ') act += s[i];
                    else {
                        input_actions.push_back(act);
                        act.clear();
                    }
                }
                if (!act.empty()) input_actions.push_back(act);
            }
        }

        line++;
    }

    assert(!initial_lv_values.empty());
}

static void writeOutput(const std::vector<std::string>& plan) {
    for (const std::string& line : raw_lines_prefix) {
        std::cout << line << '\n';
    }
    for (int i = 0; i < static_cast<int>(plan.size()); i++) {
        if (i) std::cout << ' ';
        std::cout << plan[i];
    }
    std::cout << '\n';
}

int main(int argc, char** argv) {
    /*
    CLI flags:
    - --L L: locality lookahead/limit parameter from the paper. Larger values
      allow more flexibility but reduce pruning. Default is 3.
    */
    int L = 3;  // default from the paper's traffic-light example
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--L" && i + 1 < argc) {
            L = std::max(1, std::stoi(argv[++i]));
        }
    }

    readProblem(std::cin);
    buildValuesIndex();
    buildActionInfo();

    std::vector<std::string> plan = input_actions;
    if (plan.empty()) {
        plan = synthesizePlan(L);
    }

    if (plan.empty()) {
        std::cerr << "No satisfying plan found (L=" << L << ").\n";
        return 1;
    }

    writeOutput(plan);
    return 0;
}
