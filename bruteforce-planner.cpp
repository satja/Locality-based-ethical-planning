#include <cassert>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cctype>

/*
Brute-force planner for the locality-based LTL validator input format.

Input (stdin): the same format as validate.cpp, but it may omit the final
line with the action sequence.

Output (stdout): the full validator input format, including a synthesized
action sequence on the last line.

This program is intentionally NOT the paper's algorithm. Instead:
- It searches over the full valuation plus temporal memory.
- It enforces all values at every step during search.
- It uses only a visited-state set for deduplication (no heuristics).

Definition-to-code mapping where it still makes sense:
- The action update rule H_{t+1} is implemented in applyActionInPlace().
- Value satisfaction H, t |= phi is implemented by formula::evaluate().

Paper vs brute-force (at a glance):
- Paper planner (`planner.cpp`) searches over substates S = (i, T, Theta).
- This brute-force planner searches over full valuations plus temporal memory.
- Paper planner uses validity checks to move the interval and prune.
- This planner enforces all values at every step and relies on visited(state).
*/

// Proposition and operator indexing mirrors validate.cpp.
std::unordered_map<std::string, int> lv_key;  // local propositions -> key
std::unordered_map<std::string, int> gv_key;  // global propositions -> key
std::unordered_map<int, int> lKey_index;      // local key -> index in lv array
std::unordered_map<int, int> gKey_index;      // global key -> index in gv array
std::unordered_map<std::string, int> operator_key = {
    {"G", 0}, {"FG", 1}, {"F", 2}, {"X", 3},
    {"U", 4}, {"NOT", 5}, {"AND", 6}, {"OR", 7}};
std::unordered_map<int, std::string> key_operator = {
    {0, "G"}, {1, "FG"}, {2, "F"}, {3, "X"},
    {4, "U"}, {5, "NOT"}, {6, "AND"}, {7, "OR"}};

// The evaluator reads from these pointers, which we re-bind per state.
int* lv_values = nullptr;
int* gv_values = nullptr;
int lvSize = 0;
int gvSize = 0;

// We keep the initial assignment as vectors for easy copying.
std::vector<int> initial_lv_values;
std::vector<int> initial_gv_values;

// We preserve all non-action lines verbatim for output.
std::vector<std::string> raw_lines_prefix;

struct NamedFormula {
    std::string text;   // original textual formula
    class formula* node;  // parsed formula tree
};

// All value formulas in Omega.
std::vector<NamedFormula> local_formulas;
std::vector<NamedFormula> global_formulas;

// Action theory gamma^-, gamma^+.
std::unordered_map<std::string, std::unordered_map<std::string, formula*>> gammaMinus;
std::unordered_map<std::string, std::unordered_map<std::string, formula*>> gammaPlus;

// If the input provides a plan, we can validate it or ignore it.
std::vector<std::string> input_actions;

// Look up a proposition value by its key in the currently bound state.
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

static std::string trimString(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        start++;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        end--;
    }
    return s.substr(start, end - start);
}

static std::string stripOuterParens(const std::string& s) {
    std::string cur = trimString(s);
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
        cur = trimString(cur.substr(1, cur.size() - 2));
    }
    return cur;
}

static bool splitTopLevelOps(const std::string& expr,
                             std::vector<std::string>& parts_out,
                             std::vector<int>& ops_out) {
    std::string current;
    int depth = 0;
    auto match_word = [&](int i, const std::string& word) {
        if (i + static_cast<int>(word.size()) > static_cast<int>(expr.size())) return false;
        if (expr.compare(i, word.size(), word) != 0) return false;
        const bool left_ok = (i == 0) || std::isspace(static_cast<unsigned char>(expr[i - 1]));
        const bool right_ok =
            (i + static_cast<int>(word.size()) == static_cast<int>(expr.size())) ||
            std::isspace(static_cast<unsigned char>(expr[i + word.size()]));
        return left_ok && right_ok;
    };

    for (int i = 0; i < static_cast<int>(expr.size()); i++) {
        const char ch = expr[i];
        if (ch == '(') depth++;
        if (ch == ')') depth--;

        if (depth == 0) {
            if (match_word(i, "AND")) {
                parts_out.push_back(trimString(current));
                current.clear();
                ops_out.push_back(operator_key["AND"]);
                i += 2;
                continue;
            }
            if (match_word(i, "OR")) {
                parts_out.push_back(trimString(current));
                current.clear();
                ops_out.push_back(operator_key["OR"]);
                i += 1;
                continue;
            }
            if (match_word(i, "U")) {
                parts_out.push_back(trimString(current));
                current.clear();
                ops_out.push_back(operator_key["U"]);
                continue;
            }
        }
        current.push_back(ch);
    }

    if (!current.empty()) {
        parts_out.push_back(trimString(current));
    }

    return !ops_out.empty();
}

class formula {
    std::vector<formula*> parts;
    std::vector<int> operators;
    std::vector<int> variables;
    std::vector<int> xPrepared;
    std::vector<int> xTrue;
    int xInd = 0;
    std::vector<int> uRes;
    std::vector<int> fTrue;
    int fInd = 0, uInd = 0;
    std::vector<int> gFalse;
    int gInd = 0;
    int lastAction = 0;
    int fixed = 0;
    int value = 0;
    int TrueConstant = 0;
    int FalseConstant = 0;

   public:
    formula() = default;

    // Mark that the next evaluation should treat FG as "final step".
    void setLastAction() { lastAction = 1; }

    /*
    Deep copy the formula, including its temporal memory.
    This is required because evaluate() mutates per-node memory.
    */
    formula* clone() const {
        formula* cp = new formula();
        cp->operators = operators;
        cp->variables = variables;
        cp->xPrepared = xPrepared;
        cp->xTrue = xTrue;
        cp->xInd = xInd;
        cp->uRes = uRes;
        cp->fTrue = fTrue;
        cp->fInd = fInd;
        cp->uInd = uInd;
        cp->gFalse = gFalse;
        cp->gInd = gInd;
        cp->lastAction = lastAction;
        cp->fixed = fixed;
        cp->value = value;
        cp->TrueConstant = TrueConstant;
        cp->FalseConstant = FalseConstant;
        cp->parts.reserve(parts.size());
        for (const formula* part : parts) {
            assert(part != nullptr);
            cp->parts.push_back(part->clone());
        }
        return cp;
    }

    /*
    Serialize formula memory into a compact string so it can be part of a
    visited-state signature for DFS pruning.
    */
    void serialize(std::string& out) const {
        auto appendInt = [&out](int v) {
            out.append(std::to_string(v));
            out.push_back(',');
        };
        auto appendVec = [&appendInt](const std::vector<int>& vec) {
            appendInt(static_cast<int>(vec.size()));
            for (int v : vec) {
                appendInt(v);
            }
        };

        out.push_back('{');
        appendInt(TrueConstant);
        appendInt(FalseConstant);
        appendInt(lastAction);
        appendInt(xInd);
        appendInt(fInd);
        appendInt(uInd);
        appendInt(gInd);
        appendVec(operators);
        appendVec(variables);
        appendVec(xPrepared);
        appendVec(xTrue);
        appendVec(uRes);
        appendVec(fTrue);
        appendVec(gFalse);
        appendInt(static_cast<int>(parts.size()));
        for (const formula* part : parts) {
            assert(part != nullptr);
            part->serialize(out);
        }
        out.push_back('}');
    }

    formula(std::string f) {
        fixed = value = 0;
        std::string op = "";
        int ind = 0;
        if (f[0] == '(') {
            const bool maybeUnaryPrefix = f.size() >= 5 && f[1] != '(';
            // Unary temporal/logical wrapper like ( G ( ... ) ).
            if (maybeUnaryPrefix && f[2] == 'F') {
                op += "F";
                if (f[3] == 'G') {
                    if (f[4] == ' ') {
                        op += "G";
                        ind = 4;
                    }
                } else if (f[3] == ' ') {
                    ind = 3;
                }
            } else if (maybeUnaryPrefix && f[2] == 'G') {
                op += "G";
                if (f[3] == 'F') {
                    if (f[4] == ' ') {
                        op += "F";
                        ind = 4;
                    }
                } else if (f[3] == ' ') {
                    ind = 3;
                }
            } else if (maybeUnaryPrefix && f[2] == 'X') {
                op += "X";
                ind = 3;
            } else if (maybeUnaryPrefix && f[2] == 'N' && f[3] == 'O' && f[4] == 'T') {
                op += "NOT";
                ind = 5;
            }

            if (ind == 3 || ind == 4 || ind == 5) {
                // Parse the wrapped subformula recursively.
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
                // Prefer a top-level split for nested parentheses.
                std::string stripped = stripOuterParens(f);
                std::vector<std::string> top_parts;
                std::vector<int> top_ops;
                if (splitTopLevelOps(stripped, top_parts, top_ops)) {
                    for (const std::string& part : top_parts) {
                        if (!part.empty()) {
                            parts.push_back(new formula(part));
                        }
                    }
                    operators = top_ops;
                    return;
                }

                // If this is just extra wrapping parentheses around one
                // subformula (e.g., "(NOT (p))"), recurse on the stripped body.
                if (stripped != f) {
                    parts.push_back(new formula(stripped));
                    return;
                }

                // Parse a parenthesized binary chain.
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
                        // Close a subformula, then read the operator.
                        formula* fs = new formula(subf);
                        parts.push_back(fs);
                        subf = "";
                        if (i == static_cast<int>(f.size()) - 1) {
                            break;
                        }
                        int j = i + 2;
                        while (j < static_cast<int>(f.size()) && f[j] != ' ') {
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
            // Non-parenthesized cases: constants, literals, or NOT/AND/OR chains.
            if (f == "TRUE") {
                TrueConstant = 1;
                return;
            } else if (f == "FALSE") {
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

            // Handle top-level binary chains (including U) even without
            // outer parentheses, e.g., "NOT (p) U q".
            std::vector<std::string> top_parts;
            std::vector<int> top_ops;
            if (splitTopLevelOps(f, top_parts, top_ops)) {
                for (const std::string& part : top_parts) {
                    if (!part.empty()) {
                        parts.push_back(new formula(part));
                    }
                }
                operators = top_ops;
                return;
            }

            std::string subf = "";
            int i = 0;
            if (f[0] == ' ') {
                i = 1;
            }
            for (; i < static_cast<int>(f.size()); i++) {
                // Read a token up to the next space.
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

                // Read the operator between subformulas.
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

    /*
    Evaluate the formula under the currently bound state.
    WARNING: this mutates temporal memory (xTrue/fTrue/gFalse/etc.).
    */
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
                const int val1 = getValueForKey(varA);
                return val1;
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
            if (op == "U") {
                if (uRes.size() > uInd) {
                    if (uRes[uInd] == 1) return 1;
                    if (uRes[uInd] == -1) return 0;
                }
                if (!val1 && !val2) {
                    uRes.push_back(-1);
                    uInd++;
                    return 0;
                }
                if (val1 && !val2) return 1;
                if (val2) {
                    uRes.push_back(1);
                    uInd++;
                    return 1;
                }
            }
            return truth;
        }

        // Composite case: walk operators left-to-right with per-operator behavior.
        if (operators.empty()) {
            // Degenerate wrapped form, e.g., "(NOT (p))" after stripping.
            assert(parts.size() == 1);
            return parts[0]->evaluate();
        }

        int truth = 0;
        int partsInd = 0;
        // When parsing as top-level binary parts (e.g., a AND b),
        // seed `truth` with the left operand before folding operators.
        if (!operators.empty()) {
            const std::string firstOp = key_operator[operators[0]];
            if (firstOp == "AND" || firstOp == "OR" || firstOp == "U") {
                assert(partsInd < static_cast<int>(parts.size()));
                truth = parts[partsInd]->evaluate();
                partsInd++;
            }
        }
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
            } else if (op == "U") {
                // The left side of U is in `truth`; now evaluate right side.
                int t1 = 0;
                if (i + 1 < static_cast<int>(operators.size()) &&
                    key_operator[operators[i + 1]] == "NOT") {
                    assert(partsInd < static_cast<int>(parts.size()));
                    t1 = (1 - parts[partsInd]->evaluate());
                    partsInd++;
                    i++;
                } else {
                    assert(partsInd < static_cast<int>(parts.size()));
                    t1 = parts[partsInd]->evaluate();
                    partsInd++;
                }

                if (uRes.size() > uInd) {
                    if (uRes[uInd] == 1) {
                        truth = 1;
                    } else if (uRes[uInd] == -1) {
                        truth = 0;
                    }
                } else {
                    if (!truth && !t1) {
                        uRes.push_back(-1);
                        uInd++;
                        truth = 0;
                    }
                    if (t1) {
                        uRes.push_back(1);
                        uInd++;
                        truth = 1;
                    }
                }
            }
        }
        xInd = fInd = gInd = uInd = 0;
        return truth;
    }
};

/*
Bind the global evaluator pointers to the provided state vectors.
This is the main mechanism that lets the same formula code work in both
validator and planner contexts.
*/
static void bindState(const std::vector<int>& lv, const std::vector<int>& gv) {
    assert(!lv.empty());
    lv_values = const_cast<int*>(lv.data());
    gv_values = gv.empty() ? nullptr : const_cast<int*>(gv.data());
    lvSize = static_cast<int>(lv.size());
    gvSize = static_cast<int>(gv.size());
}

// Deep-clone all formulas so each state has its own temporal memory.
static std::vector<NamedFormula> cloneFormulas(const std::vector<NamedFormula>& src) {
    std::vector<NamedFormula> out;
    out.reserve(src.size());
    for (const NamedFormula& nf : src) {
        assert(nf.node != nullptr);
        out.push_back({nf.text, nf.node->clone()});
    }
    return out;
}

// Mark all formulas as being evaluated on the last action.
static void setLastAction(std::vector<NamedFormula>& formulas) {
    for (NamedFormula& nf : formulas) {
        assert(nf.node != nullptr);
        nf.node->setLastAction();
    }
}

// A full planner search state: valuation + per-formula temporal memory.
struct PlannerState {
    std::vector<int> lv;
    std::vector<int> gv;
    std::vector<NamedFormula> local;
    std::vector<NamedFormula> global;
};

// Clone a state for DFS branching.
static PlannerState cloneState(const PlannerState& src) {
    PlannerState out;
    out.lv = src.lv;
    out.gv = src.gv;
    out.local = cloneFormulas(src.local);
    out.global = cloneFormulas(src.global);
    return out;
}

/*
Evaluate all formulas once for a given step. This mutates temporal memory and
should be called exactly once per step.
*/
static bool evaluateFormulas(std::vector<NamedFormula>& formulas) {
    bool ok = true;
    for (NamedFormula& nf : formulas) {
        assert(nf.node != nullptr);
        if (nf.node->evaluate() == 0) {
            ok = false;
        }
    }
    return ok;
}

// Check all constraints at a given step under the provided state.
static bool checkStepConstraints(PlannerState& state) {
    bindState(state.lv, state.gv);
    const bool localsOk = evaluateFormulas(state.local);
    const bool globalsOk = evaluateFormulas(state.global);
    return localsOk && globalsOk;
}

// Check whether all formulas are satisfied at the final step (FG semantics).
static bool checkFinalSatisfied(const PlannerState& state) {
    PlannerState tmp = cloneState(state);
    setLastAction(tmp.local);
    setLastAction(tmp.global);
    bindState(tmp.lv, tmp.gv);
    const bool localsOk = evaluateFormulas(tmp.local);
    const bool globalsOk = evaluateFormulas(tmp.global);
    return localsOk && globalsOk;
}

/*
Apply one action using pre-state semantics:
all gamma^- / gamma^+ conditions are evaluated on the same snapshot.
*/
static void applyActionInPlace(PlannerState& state, const std::string& action) {
    const std::vector<int> pre_lv = state.lv;
    const std::vector<int> pre_gv = state.gv;
    bindState(pre_lv, pre_gv);

    std::vector<std::string> minus_true;
    std::vector<std::string> plus_true;

    const auto minusIt = gammaMinus.find(action);
    if (minusIt != gammaMinus.end()) {
        const auto& tmpMinus = minusIt->second;
        for (const auto& entry : tmpMinus) {
            const std::string& prop = entry.first;
            formula* f = entry.second;
            assert(f != nullptr);
            const int val = f->evaluate();
            if (val == 1) {
                minus_true.push_back(prop);
            }
        }
    }

    const auto plusIt = gammaPlus.find(action);
    if (plusIt != gammaPlus.end()) {
        const auto& tmpPlus = plusIt->second;
        for (const auto& entry : tmpPlus) {
            const std::string& prop = entry.first;
            formula* f = entry.second;
            assert(f != nullptr);
            const int val = f->evaluate();
            if (val == 1) {
                plus_true.push_back(prop);
            }
        }
    }

    auto apply_props = [&](const std::vector<std::string>& props, int value) {
        for (const std::string& prop : props) {
            const auto lvIt = lv_key.find(prop);
            if (lvIt != lv_key.end()) {
                const int key = lvIt->second;
                const auto idxIt = lKey_index.find(key);
                assert(idxIt != lKey_index.end());
                state.lv[idxIt->second] = value;
            } else {
                const auto gvIt = gv_key.find(prop);
                assert(gvIt != gv_key.end());
                const int key = gvIt->second;
                const auto idxIt = gKey_index.find(key);
                assert(idxIt != gKey_index.end());
                state.gv[idxIt->second] = value;
            }
        }
    };

    apply_props(minus_true, 0);
    apply_props(plus_true, 1);
}

// Collect all distinct actions mentioned in gamma^- and gamma^+.
static std::vector<std::string> collectActions() {
    std::vector<std::string> actionsList;
    actionsList.reserve(gammaMinus.size() + gammaPlus.size());
    for (const auto& entry : gammaMinus) {
        actionsList.push_back(entry.first);
    }
    for (const auto& entry : gammaPlus) {
        if (gammaMinus.find(entry.first) == gammaMinus.end()) {
            actionsList.push_back(entry.first);
        }
    }
    return actionsList;
}

/*
Build a signature over valuation + temporal memory so we can implement
a plain visited-state set without additional heuristics.
*/
static std::string stateSignature(const PlannerState& state) {
    std::string out;
    out.reserve(256);
    auto appendInt = [&out](int v) {
        out.append(std::to_string(v));
        out.push_back(',');
    };

    appendInt(static_cast<int>(state.lv.size()));
    for (int v : state.lv) {
        appendInt(v);
    }
    appendInt(static_cast<int>(state.gv.size()));
    for (int v : state.gv) {
        appendInt(v);
    }

    appendInt(static_cast<int>(state.local.size()));
    for (const NamedFormula& nf : state.local) {
        assert(nf.node != nullptr);
        nf.node->serialize(out);
    }
    appendInt(static_cast<int>(state.global.size()));
    for (const NamedFormula& nf : state.global) {
        assert(nf.node != nullptr);
        nf.node->serialize(out);
    }
    return out;
}

/*
Depth-first search over the full state. We deduplicate via visited(state).
*/
static bool dfsPlan(const PlannerState& state,
                    int depth,
                    int maxDepth,
                    const std::vector<std::string>& actionsList,
                    std::vector<std::string>& plan,
                    std::unordered_set<std::string>& visited) {
    if (checkFinalSatisfied(state)) {
        return true;
    }
    if (depth >= maxDepth) {
        return false;
    }

    const std::string sig = stateSignature(state);
    if (visited.find(sig) != visited.end()) {
        return false;
    }
    visited.insert(sig);

    // Try each action as the next step.
    for (const std::string& action : actionsList) {
        PlannerState next = cloneState(state);
        applyActionInPlace(next, action);

        // Enforce all values at this step; prune immediately on violation.
        if (!checkStepConstraints(next)) {
            continue;
        }

        plan.push_back(action);
        if (dfsPlan(next, depth + 1, maxDepth, actionsList, plan, visited)) {
            return true;
        }
        plan.pop_back();
    }
    return false;
}

// Find a satisfying plan up to a maximum depth bound.
static bool findPlan(int maxDepth, std::vector<std::string>& plan) {
    PlannerState initial;
    initial.lv = initial_lv_values;
    initial.gv = initial_gv_values;
    initial.local = cloneFormulas(local_formulas);
    initial.global = cloneFormulas(global_formulas);

    // Initialize temporal memory at the initial state.
    const bool initialOk = checkStepConstraints(initial);
    assert(initialOk);

    const std::vector<std::string> actionsList = collectActions();
    std::unordered_set<std::string> visited;
    const bool found = dfsPlan(initial, 0, maxDepth, actionsList, plan, visited);
    if (!found) {
        plan.clear();
    }
    return found;
}

/*
Parse the validator input format. We store all non-action lines in
raw_lines_prefix so the planner can emit a complete validator input.
*/
static void readProblem(std::istream& input) {
    int key = 0;
    int lind = 0;
    int gind = 0;
    std::string s;
    int line = 1;

    while (std::getline(input, s)) {
        // Normalize CRLF to LF for robust parsing.
        if (!s.empty() && s.back() == '\r') {
            s.pop_back();
        }
        if (s.empty()) {
            continue;
        }

        if (line == 1) {
            raw_lines_prefix.push_back(s);
            std::string tmp = "";
            // Parse comma-separated local propositions.
            for (int i = 0; i < static_cast<int>(s.size()); i++) {
                if (s[i] != ',') {
                    tmp += s[i];
                    if (i == static_cast<int>(s.size()) - 1) {
                        lv_key[tmp] = key;
                        lKey_index[key] = lind;
                        key++;
                        lind++;
                        tmp = "";
                    }
                } else {
                    lv_key[tmp] = key;
                    lKey_index[key] = lind;
                    key++;
                    lind++;
                    i++;
                    tmp = "";
                }
            }
        } else if (line == 2) {
            raw_lines_prefix.push_back(s);
            std::string tmp = "";
            // Parse comma-separated global propositions.
            for (int i = 0; i < static_cast<int>(s.size()); i++) {
                if (s[i] != ',') {
                    tmp += s[i];
                    if (i == static_cast<int>(s.size()) - 1) {
                        gv_key[tmp] = key;
                        gKey_index[key] = gind;
                        key++;
                        gind++;
                        tmp = "";
                    }
                } else {
                    gv_key[tmp] = key;
                    gKey_index[key] = gind;
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
            // Parse initial local truth values.
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
            // Parse initial global truth values.
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
            // Remaining lines are gamma, formulas, or a provided action sequence.
            if (s[0] == '-') {
                raw_lines_prefix.push_back(s);
                std::string act = "";
                std::string prop = "";
                std::string form = "";
                int part = 0;
                // Parse "- action proposition : formula".
                for (int i = 2; i < static_cast<int>(s.size()); i++) {
                    if (part == 0 && s[i] != ' ') {
                        act += s[i];
                    } else if (part == 0 && s[i] == ' ') {
                        part++;
                    } else if (part == 1 && s[i] != ' ') {
                        prop += s[i];
                    } else if (part == 1 && s[i] == ' ') {
                        part++;
                        i += 2;
                    } else if (part == 2) {
                        form += s[i];
                    }
                }

                formula* f = new formula(form);
                gammaMinus[act][prop] = f;
            } else if (s[0] == '+') {
                raw_lines_prefix.push_back(s);
                std::string act = "";
                std::string prop = "";
                std::string form = "";
                int part = 0;
                // Parse "+ action proposition : formula".
                for (int i = 2; i < static_cast<int>(s.size()); i++) {
                    if (part == 0 && s[i] != ' ') {
                        act += s[i];
                    } else if (part == 0 && s[i] == ' ') {
                        part++;
                    } else if (part == 1 && s[i] != ' ') {
                        prop += s[i];
                    } else if (part == 1 && s[i] == ' ') {
                        part++;
                        i += 2;
                    } else if (part == 2) {
                        form += s[i];
                    }
                }

                formula* f = new formula(form);
                gammaPlus[act][prop] = f;
            } else if (s[0] == 'l' && s.size() > 2 && s[2] == ':') {
                raw_lines_prefix.push_back(s);
                std::string st = "";
                // Parse "l : formula".
                for (int i = 4; i < static_cast<int>(s.size()); i++) {
                    st += s[i];
                }
                formula* f = new formula(st);
                local_formulas.push_back({st, f});
            } else if (s[0] == 'g' && s.size() > 2 && s[2] == ':') {
                raw_lines_prefix.push_back(s);
                std::string st = "";
                // Parse "g : formula".
                for (int i = 4; i < static_cast<int>(s.size()); i++) {
                    st += s[i];
                }
                formula* f = new formula(st);
                global_formulas.push_back({st, f});
            } else {
                // Ignore trailing action-sequence lines from full-format inputs.
                // The planner always synthesizes a plan from the model.
            }
        }

        line++;
    }

    assert(!initial_lv_values.empty());
}

// Emit the full validator input format using the synthesized plan.
static void writeOutput(const std::vector<std::string>& plan) {
    for (const std::string& line : raw_lines_prefix) {
        std::cout << line << '\n';
    }
    for (int i = 0; i < static_cast<int>(plan.size()); i++) {
        if (i) {
            std::cout << ' ';
        }
        std::cout << plan[i];
    }
    std::cout << '\n';
}

int main(int argc, char** argv) {
    /*
    CLI flags:
    - --max-depth D: depth bound for the DFS search. Default is 40.
    */
    // Allow a configurable depth bound; default is modest for safety.
    int maxDepth = 40;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--max-depth" && i + 1 < argc) {
            maxDepth = std::stoi(argv[++i]);
        }
    }

    readProblem(std::cin);

    std::vector<std::string> plan;
    const bool found = findPlan(maxDepth, plan);

    if (!found) {
        std::cerr << "No satisfying plan found up to depth " << maxDepth << '\n';
        return 1;
    }

    writeOutput(plan);
    return 0;
}
