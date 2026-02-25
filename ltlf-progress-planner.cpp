#include <algorithm>
#include <cassert>
#include <cctype>
#include <iostream>
#include <memory>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
Automata-style baseline planner using LTLf progression.

This baseline follows the standard idea from LTLf/automata planning:
- Maintain a residual ("progressed") formula per value.
- Progress it with the current state valuation.
- Search over the product of planning state and residual formulas.

Important scope notes:
- The value operator FG is treated as a final-state constraint when it appears
  at the root of a value formula, which matches the validator's FG handling in
  this repository.
- Nested FG occurrences are replaced with TRUE and reported once on stderr.
*/

// Proposition and operator indexing mirrors validate.cpp.
static std::unordered_map<std::string, int> lv_key;  // local propositions -> key
static std::unordered_map<std::string, int> gv_key;  // global propositions -> key
static std::unordered_map<int, int> lKey_index;      // local key -> index in lv array
static std::unordered_map<int, int> gKey_index;      // global key -> index in gv array
static std::unordered_map<std::string, int> operator_key = {
    {"G", 0}, {"FG", 1}, {"F", 2}, {"X", 3},
    {"U", 4}, {"NOT", 5}, {"AND", 6}, {"OR", 7}};
static std::unordered_map<int, std::string> key_operator = {
    {0, "G"}, {1, "FG"}, {2, "F"}, {3, "X"},
    {4, "U"}, {5, "NOT"}, {6, "AND"}, {7, "OR"}};

// The evaluator reads from these pointers, which we re-bind per state.
static int* lv_values = nullptr;
static int* gv_values = nullptr;
static int lvSize = 0;
static int gvSize = 0;

// We keep the initial assignment as vectors for easy copying.
static std::vector<int> initial_lv_values;
static std::vector<int> initial_gv_values;

// We preserve all non-action lines verbatim for output.
static std::vector<std::string> raw_lines_prefix;

// Action theory gamma^-, gamma^+.
static std::unordered_map<std::string, std::unordered_map<std::string, class formula*>> gammaMinus;
static std::unordered_map<std::string, std::unordered_map<std::string, class formula*>> gammaPlus;

// If the input provides a plan, we can validate it or ignore it.
static std::vector<std::string> input_actions;

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

/*
Propositional/temporal formula evaluator for gamma conditions.
This mirrors the existing bruteforce planner so gamma expressions remain
compatible with the current input format.
*/
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

    formula(std::string f) {
        fixed = value = 0;
        std::string op = "";
        int ind = 0;
        if (f[0] == '(') {
            const bool maybeUnaryPrefix = f.size() >= 5 && f[1] != '(';
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

                if (stripped != f) {
                    parts.push_back(new formula(stripped));
                    return;
                }

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

    // Mark that the next evaluation should treat FG as "final step".
    void setLastAction() { lastAction = 1; }

    int evaluate() {
        if (parts.empty()) {
            if (TrueConstant) {
                return 1;
            }
            if (FalseConstant) {
                return 0;
            }

            if (variables.empty()) {
                return 0;
            }
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

static void bindState(const std::vector<int>& lv, const std::vector<int>& gv) {
    assert(!lv.empty());
    lv_values = const_cast<int*>(lv.data());
    gv_values = gv.empty() ? nullptr : const_cast<int*>(gv.data());
    lvSize = static_cast<int>(lv.size());
    gvSize = static_cast<int>(gv.size());
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

// ---------------- LTLf progression AST ----------------

enum class NodeType {
    True,
    False,
    Atom,
    Not,
    And,
    Or,
    Next,
    Eventually,
    Always,
    Until,
    FinalAlways,  // FG
};

struct Node;
using NodePtr = std::shared_ptr<const Node>;

struct Node {
    NodeType type;
    std::string atom;  // only for Atom
    NodePtr left;
    NodePtr right;
};

static NodePtr make_node(NodeType type, NodePtr left = nullptr, NodePtr right = nullptr) {
    auto n = std::make_shared<Node>();
    n->type = type;
    n->left = std::move(left);
    n->right = std::move(right);
    return n;
}

static NodePtr make_atom(std::string atom) {
    auto n = std::make_shared<Node>();
    n->type = NodeType::Atom;
    n->atom = std::move(atom);
    return n;
}

static NodePtr kTrue() {
    static NodePtr v = make_node(NodeType::True);
    return v;
}

static NodePtr kFalse() {
    static NodePtr v = make_node(NodeType::False);
    return v;
}

static bool is_true(const NodePtr& n) { return n->type == NodeType::True; }
static bool is_false(const NodePtr& n) { return n->type == NodeType::False; }

static NodePtr simplify(const NodePtr& n);

static NodePtr make_not(const NodePtr& a) {
    if (is_true(a)) return kFalse();
    if (is_false(a)) return kTrue();
    if (a->type == NodeType::Not) return a->left ? a->left : kTrue();
    return make_node(NodeType::Not, a);
}

static NodePtr make_and(const NodePtr& a, const NodePtr& b) {
    if (is_false(a) || is_false(b)) return kFalse();
    if (is_true(a)) return b;
    if (is_true(b)) return a;
    if (a == b) return a;
    return make_node(NodeType::And, a, b);
}

static NodePtr make_or(const NodePtr& a, const NodePtr& b) {
    if (is_true(a) || is_true(b)) return kTrue();
    if (is_false(a)) return b;
    if (is_false(b)) return a;
    if (a == b) return a;
    return make_node(NodeType::Or, a, b);
}

static NodePtr simplify(const NodePtr& n) {
    switch (n->type) {
        case NodeType::Not: {
            const NodePtr a = simplify(n->left ? n->left : kTrue());
            return make_not(a);
        }
        case NodeType::And: {
            const NodePtr a = simplify(n->left ? n->left : kTrue());
            const NodePtr b = simplify(n->right ? n->right : kTrue());
            return make_and(a, b);
        }
        case NodeType::Or: {
            const NodePtr a = simplify(n->left ? n->left : kFalse());
            const NodePtr b = simplify(n->right ? n->right : kFalse());
            return make_or(a, b);
        }
        case NodeType::Eventually: {
            const NodePtr a = simplify(n->left ? n->left : kFalse());
            if (is_true(a)) return kTrue();
            if (is_false(a)) return kFalse();
            return make_node(NodeType::Eventually, a);
        }
        case NodeType::Always: {
            const NodePtr a = simplify(n->left ? n->left : kTrue());
            if (is_true(a)) return kTrue();
            if (is_false(a)) return kFalse();
            return make_node(NodeType::Always, a);
        }
        case NodeType::Next: {
            const NodePtr a = simplify(n->left ? n->left : kFalse());
            if (is_false(a)) return kFalse();
            return make_node(NodeType::Next, a);
        }
        case NodeType::Until: {
            const NodePtr a = simplify(n->left ? n->left : kFalse());
            const NodePtr b = simplify(n->right ? n->right : kFalse());
            if (is_true(b)) return kTrue();
            if (is_false(a)) return b;
            return make_node(NodeType::Until, a, b);
        }
        case NodeType::FinalAlways: {
            const NodePtr a = simplify(n->left ? n->left : kTrue());
            return make_node(NodeType::FinalAlways, a);
        }
        default:
            return n;
    }
}

// Tokenizer for the value formulas.
struct Token {
    std::string text;
};

static std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    out.reserve(s.size() / 2);
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            out.push_back({cur});
            cur.clear();
        }
    };

    for (char ch : s) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            flush();
            continue;
        }
        if (ch == '(' || ch == ')') {
            flush();
            out.push_back({std::string(1, ch)});
            continue;
        }
        cur.push_back(ch);
    }
    flush();
    return out;
}

static bool is_prefix_op(const std::string& t) {
    return t == "NOT" || t == "X" || t == "F" || t == "G" || t == "FG";
}

static bool is_infix_op(const std::string& t) {
    return t == "AND" || t == "OR" || t == "U";
}

static int precedence(const std::string& op) {
    if (op == "U") return 1;
    if (op == "OR") return 2;
    if (op == "AND") return 3;
    return -1;
}

struct Parser {
    std::vector<Token> toks;
    size_t pos = 0;

    explicit Parser(std::vector<Token> toks) : toks(std::move(toks)) {}

    bool at_end() const { return pos >= toks.size(); }

    const std::string& peek() const {
        static const std::string empty;
        return at_end() ? empty : toks[pos].text;
    }

    const std::string& consume() {
        assert(!at_end());
        return toks[pos++].text;
    }

    bool maybe_consume(const std::string& t) {
        if (peek() == t) {
            pos++;
            return true;
        }
        return false;
    }

    NodePtr parse_expression(int min_prec = 1) {
        NodePtr left = parse_prefix();
        while (!at_end() && is_infix_op(peek()) && precedence(peek()) >= min_prec) {
            const std::string op = consume();
            const int prec = precedence(op);
            NodePtr right = parse_expression(prec + 1);
            if (op == "AND") {
                left = make_and(left, right);
            } else if (op == "OR") {
                left = make_or(left, right);
            } else if (op == "U") {
                left = simplify(make_node(NodeType::Until, left, right));
            }
        }
        return left;
    }

    NodePtr parse_prefix() {
        if (at_end()) return kFalse();

        const std::string t = consume();
        if (t == "(") {
            NodePtr inner = parse_expression();
            (void)maybe_consume(")");
            return inner;
        }
        if (t == "TRUE") return kTrue();
        if (t == "FALSE") return kFalse();
        if (is_prefix_op(t)) {
            NodePtr sub = parse_prefix();
            if (t == "NOT") return make_not(sub);
            if (t == "X") return simplify(make_node(NodeType::Next, sub));
            if (t == "F") return simplify(make_node(NodeType::Eventually, sub));
            if (t == "G") return simplify(make_node(NodeType::Always, sub));
            if (t == "FG") return simplify(make_node(NodeType::FinalAlways, sub));
        }
        return make_atom(t);
    }
};

static NodePtr parse_formula_ast(const std::string& text) {
    Parser p(tokenize(text));
    NodePtr ast = p.parse_expression();
    return simplify(ast);
}

// Evaluate an atom under the current valuation.
static bool atom_value(const std::string& atom, const std::vector<int>& lv, const std::vector<int>& gv) {
    const auto lit = lv_key.find(atom);
    if (lit != lv_key.end()) {
        const int key = lit->second;
        const auto idxIt = lKey_index.find(key);
        assert(idxIt != lKey_index.end());
        return lv[idxIt->second] != 0;
    }
    const auto git = gv_key.find(atom);
    if (git != gv_key.end()) {
        const int key = git->second;
        const auto idxIt = gKey_index.find(key);
        assert(idxIt != gKey_index.end());
        return gv[idxIt->second] != 0;
    }
    return false;
}

static NodePtr progress(const NodePtr& f,
                        const std::vector<int>& lv,
                        const std::vector<int>& gv);

static NodePtr progress(const NodePtr& f,
                        const std::vector<int>& lv,
                        const std::vector<int>& gv) {
    switch (f->type) {
        case NodeType::True:
            return kTrue();
        case NodeType::False:
            return kFalse();
        case NodeType::Atom:
            return atom_value(f->atom, lv, gv) ? kTrue() : kFalse();
        case NodeType::Not:
            return make_not(progress(f->left ? f->left : kTrue(), lv, gv));
        case NodeType::And:
            return make_and(progress(f->left ? f->left : kTrue(), lv, gv),
                            progress(f->right ? f->right : kTrue(), lv, gv));
        case NodeType::Or:
            return make_or(progress(f->left ? f->left : kFalse(), lv, gv),
                           progress(f->right ? f->right : kFalse(), lv, gv));
        case NodeType::Next:
            return f->left ? f->left : kFalse();
        case NodeType::Eventually: {
            const NodePtr sub = f->left ? f->left : kFalse();
            return make_or(progress(sub, lv, gv), simplify(make_node(NodeType::Eventually, sub)));
        }
        case NodeType::Always: {
            const NodePtr sub = f->left ? f->left : kTrue();
            return make_and(progress(sub, lv, gv), simplify(make_node(NodeType::Always, sub)));
        }
        case NodeType::Until: {
            const NodePtr phi = f->left ? f->left : kFalse();
            const NodePtr psi = f->right ? f->right : kFalse();
            const NodePtr psiProg = progress(psi, lv, gv);
            const NodePtr phiProg = progress(phi, lv, gv);
            return make_or(psiProg, make_and(phiProg, f));
        }
        case NodeType::FinalAlways:
            // FG is handled as a final-state check at the value root.
            return kTrue();
    }
    return kFalse();
}

// LTLf acceptance on the empty trace.
static bool accepts_empty(const NodePtr& f) {
    switch (f->type) {
        case NodeType::True:
            return true;
        case NodeType::False:
        case NodeType::Atom:
            return false;
        case NodeType::Not:
            return !accepts_empty(f->left ? f->left : kTrue());
        case NodeType::And:
            return accepts_empty(f->left ? f->left : kTrue()) &&
                   accepts_empty(f->right ? f->right : kTrue());
        case NodeType::Or:
            return accepts_empty(f->left ? f->left : kFalse()) ||
                   accepts_empty(f->right ? f->right : kFalse());
        case NodeType::Next:
            return false;
        case NodeType::Eventually:
            return false;
        case NodeType::Always:
            return true;
        case NodeType::Until:
            return false;
        case NodeType::FinalAlways:
            return true;
    }
    return false;
}

static void serialize_ast(const NodePtr& f, std::string& out) {
    auto append = [&](const char* s) { out.append(s); out.push_back(' '); };
    switch (f->type) {
        case NodeType::True:
            append("T");
            return;
        case NodeType::False:
            append("F");
            return;
        case NodeType::Atom:
            out.append("P:");
            out.append(f->atom);
            out.push_back(' ');
            return;
        case NodeType::Not:
            append("NOT");
            serialize_ast(f->left ? f->left : kTrue(), out);
            return;
        case NodeType::And:
            append("AND");
            serialize_ast(f->left ? f->left : kTrue(), out);
            serialize_ast(f->right ? f->right : kTrue(), out);
            return;
        case NodeType::Or:
            append("OR");
            serialize_ast(f->left ? f->left : kFalse(), out);
            serialize_ast(f->right ? f->right : kFalse(), out);
            return;
        case NodeType::Next:
            append("X");
            serialize_ast(f->left ? f->left : kFalse(), out);
            return;
        case NodeType::Eventually:
            append("F");
            serialize_ast(f->left ? f->left : kFalse(), out);
            return;
        case NodeType::Always:
            append("G");
            serialize_ast(f->left ? f->left : kTrue(), out);
            return;
        case NodeType::Until:
            append("U");
            serialize_ast(f->left ? f->left : kFalse(), out);
            serialize_ast(f->right ? f->right : kFalse(), out);
            return;
        case NodeType::FinalAlways:
            append("FG");
            serialize_ast(f->left ? f->left : kTrue(), out);
            return;
    }
}

static std::string ast_signature(const NodePtr& f) {
    std::string out;
    out.reserve(128);
    serialize_ast(f, out);
    return out;
}

static void collect_fg_roots(const NodePtr& f,
                             std::vector<NodePtr>& final_constraints,
                             bool is_root,
                             bool& warned_nested) {
    if (f->type == NodeType::FinalAlways) {
        if (is_root) {
            final_constraints.push_back(f->left ? f->left : kTrue());
        } else if (!warned_nested) {
            std::cerr << "Warning: nested FG encountered; treating it as TRUE in automata baseline.\n";
            warned_nested = true;
        }
        return;
    }
    if (f->left) {
        collect_fg_roots(f->left, final_constraints, false, warned_nested);
    }
    if (f->right) {
        collect_fg_roots(f->right, final_constraints, false, warned_nested);
    }
}

// Replace all FG nodes with TRUE, except root FG which is already extracted.
static NodePtr strip_fg(const NodePtr& f) {
    switch (f->type) {
        case NodeType::FinalAlways:
            return kTrue();
        case NodeType::Not:
            return make_not(strip_fg(f->left ? f->left : kTrue()));
        case NodeType::And:
            return make_and(strip_fg(f->left ? f->left : kTrue()),
                            strip_fg(f->right ? f->right : kTrue()));
        case NodeType::Or:
            return make_or(strip_fg(f->left ? f->left : kFalse()),
                           strip_fg(f->right ? f->right : kFalse()));
        case NodeType::Next:
            return simplify(make_node(NodeType::Next, strip_fg(f->left ? f->left : kFalse())));
        case NodeType::Eventually:
            return simplify(make_node(NodeType::Eventually, strip_fg(f->left ? f->left : kFalse())));
        case NodeType::Always:
            return simplify(make_node(NodeType::Always, strip_fg(f->left ? f->left : kTrue())));
        case NodeType::Until:
            return simplify(make_node(NodeType::Until,
                                      strip_fg(f->left ? f->left : kFalse()),
                                      strip_fg(f->right ? f->right : kFalse())));
        default:
            return f;
    }
}

// ---------------- Automata baseline search ----------------

struct AutomataState {
    std::vector<int> lv;
    std::vector<int> gv;
    std::vector<NodePtr> residuals;  // per-value progressed formulas for the current state
};

static std::vector<NodePtr> value_formulas;
static std::vector<NodePtr> final_constraints;

static bool final_constraints_hold(const std::vector<int>& lv, const std::vector<int>& gv) {
    for (const NodePtr& f : final_constraints) {
        // Evaluate final-state constraints propositionally on the current valuation.
        // We do this by progressing once and requiring it to be TRUE.
        NodePtr progressed = progress(f, lv, gv);
        if (!is_true(progressed)) {
            return false;
        }
    }
    return true;
}

static std::string state_signature(const AutomataState& s) {
    std::string out;
    out.reserve(256);
    auto append_int = [&](int v) {
        out.append(std::to_string(v));
        out.push_back(',');
    };

    append_int(static_cast<int>(s.lv.size()));
    for (int v : s.lv) append_int(v);
    append_int(static_cast<int>(s.gv.size()));
    for (int v : s.gv) append_int(v);

    append_int(static_cast<int>(s.residuals.size()));
    for (const NodePtr& f : s.residuals) {
        out.append(ast_signature(f));
        out.push_back('|');
    }

    return out;
}

static void apply_action_in_place(AutomataState& state, const std::string& action) {
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

struct QueueNode {
    AutomataState state;
    int depth = 0;
    int parent = -1;
    std::string action;
};

static std::vector<std::string> reconstruct_plan(const std::vector<QueueNode>& nodes, int idx) {
    std::vector<std::string> plan;
    for (int cur = idx; cur >= 0; cur = nodes[cur].parent) {
        if (!nodes[cur].action.empty()) {
            plan.push_back(nodes[cur].action);
        }
    }
    std::reverse(plan.begin(), plan.end());
    return plan;
}

static bool find_plan_progression(int max_depth, std::vector<std::string>& plan) {
    const std::vector<std::string> actions = collectActions();

    AutomataState initial;
    initial.lv = initial_lv_values;
    initial.gv = initial_gv_values;
    initial.residuals = value_formulas;

    // Progress over the initial state to get residuals for the next state.
    std::vector<NodePtr> progressed_initial;
    progressed_initial.reserve(initial.residuals.size());
    for (const NodePtr& f : initial.residuals) {
        NodePtr pf = simplify(progress(f, initial.lv, initial.gv));
        if (is_false(pf)) {
            plan.clear();
            return false;
        }
        progressed_initial.push_back(pf);
    }
    initial.residuals = std::move(progressed_initial);

    // Acceptance at the initial state (empty plan).
    bool initial_accepts_empty = true;
    for (const NodePtr& pf : initial.residuals) {
        if (!accepts_empty(pf)) {
            initial_accepts_empty = false;
            break;
        }
    }
    if (initial_accepts_empty && final_constraints_hold(initial.lv, initial.gv)) {
        plan.clear();
        return true;
    }

    std::vector<QueueNode> nodes;
    nodes.reserve(4096);
    nodes.push_back({initial, 0, -1, ""});

    std::queue<int> q;
    q.push(0);

    std::unordered_map<std::string, int> best_depth;
    best_depth.reserve(4096);
    best_depth[state_signature(initial)] = 0;

    while (!q.empty()) {
        const int idx = q.front();
        q.pop();
        const QueueNode cur_node = nodes[idx];

        if (cur_node.depth >= max_depth) {
            continue;
        }

        // Each residual here is already progressed through the current state.
        // We only need to progress once we reach the next state.
        for (const std::string& action : actions) {
            AutomataState next_state = cur_node.state;
            apply_action_in_place(next_state, action);

            // Progress all residuals using the new current state.
            std::vector<NodePtr> progressed;
            progressed.reserve(next_state.residuals.size());
            bool dead = false;
            for (const NodePtr& f : next_state.residuals) {
                NodePtr pf = simplify(progress(f, next_state.lv, next_state.gv));
                if (is_false(pf)) {
                    dead = true;
                    break;
                }
                progressed.push_back(pf);
            }
            if (dead) {
                continue;
            }
            next_state.residuals = std::move(progressed);

            // Acceptance check at this state.
            bool empty_ok = true;
            for (const NodePtr& pf : next_state.residuals) {
                if (!accepts_empty(pf)) {
                    empty_ok = false;
                    break;
                }
            }
            if (empty_ok && final_constraints_hold(next_state.lv, next_state.gv)) {
                QueueNode accept_node{next_state, cur_node.depth + 1, idx, action};
                nodes.push_back(std::move(accept_node));
                plan = reconstruct_plan(nodes, static_cast<int>(nodes.size() - 1));
                return true;
            }

            const int next_depth = cur_node.depth + 1;
            const std::string sig = state_signature(next_state);
            const auto it = best_depth.find(sig);
            if (it != best_depth.end() && it->second <= next_depth) {
                continue;
            }
            best_depth[sig] = next_depth;

            nodes.push_back({std::move(next_state), next_depth, idx, action});
            q.push(static_cast<int>(nodes.size() - 1));
        }
    }

    plan.clear();
    return false;
}

// ---------------- Parsing and I/O ----------------

static std::string trim_parens(const std::string& s) {
    size_t start = 0;
    size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) start++;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
    if (end - start >= 2 && s[start] == '(' && s[end - 1] == ')') {
        start++;
        end--;
    }
    return s.substr(start, end - start);
}

static void register_atom(const std::string& atom, bool local, int key, int index) {
    if (local) {
        lv_key[atom] = key;
        lKey_index[key] = index;
    } else {
        gv_key[atom] = key;
        gKey_index[key] = index;
    }
}

static void readProblem(std::istream& input) {
    int key = 0;
    int lind = 0;
    int gind = 0;
    std::string s;
    int line = 1;

    bool warned_nested_fg = false;

    while (std::getline(input, s)) {
        if (!s.empty() && s.back() == '\r') {
            s.pop_back();
        }

        if (s.empty()) {
            line++;
            continue;
        }

        if (line == 1) {
            raw_lines_prefix.push_back(s);
            // Local propositions.
            std::string sub;
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i] == ',' || i == s.size() - 1) {
                    if (i == s.size() - 1 && s[i] != ',') {
                        sub += s[i];
                    }
                    // Trim spaces.
                    size_t st = 0;
                    while (st < sub.size() && sub[st] == ' ') st++;
                    size_t en = sub.size();
                    while (en > st && sub[en - 1] == ' ') en--;
                    const std::string atom = sub.substr(st, en - st);
                    if (!atom.empty()) {
                        register_atom(atom, true, key, lind);
                        key++;
                        lind++;
                    }
                    sub.clear();
                } else if (s[i] != ' ') {
                    sub += s[i];
                }
            }
            initial_lv_values.assign(lv_key.size(), 0);
        } else if (line == 2) {
            raw_lines_prefix.push_back(s);
            // Global propositions.
            if (s != "None") {
                std::string sub;
                for (size_t i = 0; i < s.size(); i++) {
                    if (s[i] == ',' || i == s.size() - 1) {
                        if (i == s.size() - 1 && s[i] != ',') {
                            sub += s[i];
                        }
                        size_t st = 0;
                        while (st < sub.size() && sub[st] == ' ') st++;
                        size_t en = sub.size();
                        while (en > st && sub[en - 1] == ' ') en--;
                        const std::string atom = sub.substr(st, en - st);
                        if (!atom.empty()) {
                            register_atom(atom, false, key, gind);
                            key++;
                            gind++;
                        }
                        sub.clear();
                    } else if (s[i] != ' ') {
                        sub += s[i];
                    }
                }
            }
            initial_gv_values.assign(gv_key.size(), 0);
        } else if (line == 3) {
            raw_lines_prefix.push_back(s);
            // Local initial valuation.
            int idx = 0;
            std::string sub;
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i] == ',' || i == s.size() - 1) {
                    if (i == s.size() - 1 && s[i] != ',') {
                        sub += s[i];
                    }
                    size_t st = 0;
                    while (st < sub.size() && sub[st] == ' ') st++;
                    const std::string lit = sub.substr(st);
                    if (idx < static_cast<int>(initial_lv_values.size())) {
                        initial_lv_values[idx] = (lit == "TRUE") ? 1 : 0;
                    }
                    idx++;
                    sub.clear();
                } else if (s[i] != ' ') {
                    sub += s[i];
                }
            }
        } else if (line == 4) {
            raw_lines_prefix.push_back(s);
            // Global initial valuation.
            int idx = 0;
            std::string sub;
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i] == ',' || i == s.size() - 1) {
                    if (i == s.size() - 1 && s[i] != ',') {
                        sub += s[i];
                    }
                    size_t st = 0;
                    while (st < sub.size() && sub[st] == ' ') st++;
                    const std::string lit = sub.substr(st);
                    if (idx < static_cast<int>(initial_gv_values.size())) {
                        initial_gv_values[idx] = (lit == "TRUE") ? 1 : 0;
                    }
                    idx++;
                    sub.clear();
                } else if (s[i] != ' ') {
                    sub += s[i];
                }
            }
        } else {
            if (s.rfind("- ", 0) == 0 || s.rfind("+ ", 0) == 0) {
                // Gamma lines.
                raw_lines_prefix.push_back(s);

                const bool isMinus = s[0] == '-';
                size_t aStart = 2;
                size_t aEnd = s.find(' ', aStart);
                const std::string action = s.substr(aStart, aEnd - aStart);

                size_t pStart = aEnd + 1;
                size_t pEnd = s.find(' ', pStart);
                const std::string prop = s.substr(pStart, pEnd - pStart);

                size_t colon = s.find(':', pEnd);
                std::string expr = (colon == std::string::npos) ? "TRUE" : s.substr(colon + 1);
                // Trim leading space.
                while (!expr.empty() && expr[0] == ' ') expr.erase(expr.begin());

                formula* f = new formula(expr);
                if (isMinus) {
                    gammaMinus[action][prop] = f;
                } else {
                    gammaPlus[action][prop] = f;
                }
            } else if (s.rfind("l :", 0) == 0 || s.rfind("g :", 0) == 0) {
                raw_lines_prefix.push_back(s);
                std::string expr = s.substr(3);
                expr = trim_parens(expr);

                NodePtr ast = parse_formula_ast(expr);

                // Extract root-level FG and treat it as a final-state constraint.
                if (ast->type == NodeType::FinalAlways) {
                    final_constraints.push_back(ast->left ? ast->left : kTrue());
                    value_formulas.push_back(kTrue());
                } else {
                    collect_fg_roots(ast, final_constraints, true, warned_nested_fg);
                    value_formulas.push_back(strip_fg(ast));
                }
            } else {
                // Ignore trailing action-sequence lines from full-format inputs.
                // The planner always synthesizes a plan from the model.
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
    - --max-depth D: depth bound for the automata/progression baseline.
      Default is 80 to match the benchmark script defaults.
    */
    int maxDepth = 80;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--max-depth" && i + 1 < argc) {
            maxDepth = std::stoi(argv[++i]);
        }
    }

    readProblem(std::cin);

    std::vector<std::string> plan;
    const bool found = find_plan_progression(maxDepth, plan);

    if (!found) {
        std::cerr << "No satisfying plan found up to depth " << maxDepth
                  << " (automata baseline).\n";
        return 1;
    }

    writeOutput(plan);
    return 0;
}
