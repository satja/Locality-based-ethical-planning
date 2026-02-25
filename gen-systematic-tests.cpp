#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

struct Config {
    fs::path dir = "tests_systematic";
    int n_min = 6;
    int n_max = 24;
    int per_n = 4;
    unsigned int seed = 20260127u;
};

enum class Family {
    Ex1Traffic = 0,
    Ex2Chargers = 1,
    Ex3Narrow2D = 2
};

static bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

static Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto need_value = [&](const char* name) {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(2);
            }
            return std::string(argv[++i]);
        };

        if (arg == "--dir") {
            cfg.dir = need_value("--dir");
        } else if (arg == "--n-min") {
            cfg.n_min = std::stoi(need_value("--n-min"));
        } else if (arg == "--n-max") {
            cfg.n_max = std::stoi(need_value("--n-max"));
        } else if (arg == "--per-n") {
            cfg.per_n = std::stoi(need_value("--per-n"));
        } else if (arg == "--seed") {
            cfg.seed = static_cast<unsigned int>(std::stoul(need_value("--seed")));
        } else if (starts_with(arg, "--")) {
            std::cerr << "Unknown flag: " << arg << "\n";
            std::exit(2);
        } else {
            std::cerr << "Unexpected positional argument: " << arg << "\n";
            std::exit(2);
        }
    }

    if (cfg.n_min < 2 || cfg.n_max < cfg.n_min) {
        std::cerr << "Require 2 <= n-min <= n-max\n";
        std::exit(2);
    }
    if (cfg.per_n <= 0) {
        std::cerr << "--per-n must be positive\n";
        std::exit(2);
    }
    return cfg;
}

static std::string join_csv(const std::vector<std::string>& items) {
    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); i++) {
        if (i) oss << ", ";
        oss << items[i];
    }
    return oss.str();
}

static std::string join_truths(const std::vector<int>& vals) {
    std::ostringstream oss;
    for (size_t i = 0; i < vals.size(); i++) {
        if (i) oss << ", ";
        oss << (vals[i] ? "TRUE" : "FALSE");
    }
    return oss.str();
}

static std::vector<int> make_pattern(int n, int pattern_id) {
    std::vector<int> indices;
    if (pattern_id == 0) {
        return indices;  // optional trivial pattern
    }
    if (pattern_id == 1) {
        indices.push_back(std::max(1, n / 2));
        return indices;
    }
    if (pattern_id == 2) {
        indices.push_back(std::max(2, n / 3));
        indices.push_back(std::min(n - 1, (2 * n) / 3));
        if (indices[0] == indices[1]) indices.pop_back();
        return indices;
    }
    const int k = std::min(1 + pattern_id, std::max(1, n / 6));
    int step = std::max(2, n / (k + 1));
    int pos = step;
    for (int i = 0; i < k; i++) {
        indices.push_back(std::min(n - 1, pos));
        pos += step;
    }
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    std::vector<int> filtered;
    for (int idx : indices) {
        if (!filtered.empty() && std::abs(filtered.back() - idx) == 1) continue;
        filtered.push_back(std::clamp(idx, 1, n));
    }
    return filtered;
}

static void write_case_ex1(const fs::path& out_path,
                           int n,
                           const std::vector<int>& broken_indices) {
    std::ofstream out(out_path);
    if (!out) {
        std::cerr << "Failed to open " << out_path << " for writing\n";
        std::exit(1);
    }

    std::vector<int> on_vals(n, 1), broken_vals(n, 0);
    for (int idx : broken_indices) {
        on_vals[idx - 1] = 0;
        broken_vals[idx - 1] = 1;
    }

    std::vector<std::string> locals;
    locals.reserve(2 * n);
    for (int i = 1; i <= n; i++) locals.push_back("on" + std::to_string(i));
    for (int i = 1; i <= n; i++) locals.push_back("broken" + std::to_string(i));
    out << join_csv(locals) << "\n";
    out << "congestion\n";

    std::vector<int> local_vals;
    local_vals.reserve(2 * n);
    local_vals.insert(local_vals.end(), on_vals.begin(), on_vals.end());
    local_vals.insert(local_vals.end(), broken_vals.begin(), broken_vals.end());
    out << join_truths(local_vals) << "\n";
    out << "FALSE\n";

    for (int i = 2; i <= n - 1; i++) {
        const std::string cond_or =
            "NOT (on" + std::to_string(i - 1) + ") OR (NOT (on" + std::to_string(i + 1) + "))";
        const std::string cond_and =
            "NOT (on" + std::to_string(i - 1) + ") AND (NOT (on" + std::to_string(i + 1) + "))";
        out << "- f" << i << " broken" << i << " : " << cond_or << "\n";
        out << "+ f" << i << " on" << i << " : " << cond_or << "\n";
        out << "+ f" << i << " congestion : " << cond_and << "\n";
    }
    out << "- f1 broken1 : TRUE\n";
    out << "- f" << n << " broken" << n << " : TRUE\n";
    out << "+ f1 on1 : TRUE\n";
    out << "+ f" << n << " on" << n << " : TRUE\n";

    for (int i = 1; i <= n; i++) out << "- g" << i << " on" << i << " : TRUE\n";
    for (int i = 1; i <= n; i++) out << "+ h" << i << " on" << i << " : NOT (broken" << i << ")\n";

    for (int i = 1; i <= n; i++) out << "l : ( FG (on" << i << "))\n";
    out << "g : ( G ( NOT (congestion)))\n";
}

static void write_case_ex2(const fs::path& out_path,
                           int n,
                           const std::vector<int>& broken_indices) {
    std::ofstream out(out_path);
    if (!out) {
        std::cerr << "Failed to open " << out_path << " for writing\n";
        std::exit(1);
    }

    std::vector<int> on_vals(n, 1), broken_vals(n, 0), charged_vals(n, 0);
    for (int idx : broken_indices) {
        on_vals[idx - 1] = 0;
        broken_vals[idx - 1] = 1;
    }

    std::vector<std::string> locals;
    locals.reserve(3 * n);
    for (int i = 1; i <= n; i++) locals.push_back("on" + std::to_string(i));
    for (int i = 1; i <= n; i++) locals.push_back("broken" + std::to_string(i));
    for (int i = 1; i <= n; i++) locals.push_back("charged" + std::to_string(i));
    out << join_csv(locals) << "\n";
    out << "congestion\n";

    std::vector<int> local_vals;
    local_vals.reserve(3 * n);
    local_vals.insert(local_vals.end(), on_vals.begin(), on_vals.end());
    local_vals.insert(local_vals.end(), broken_vals.begin(), broken_vals.end());
    local_vals.insert(local_vals.end(), charged_vals.begin(), charged_vals.end());
    out << join_truths(local_vals) << "\n";
    out << "FALSE\n";

    for (int i = 2; i <= n - 1; i++) {
        const std::string cond_or =
            "NOT (on" + std::to_string(i - 1) + ") OR (NOT (on" + std::to_string(i + 1) + "))";
        const std::string cond_and =
            "NOT (on" + std::to_string(i - 1) + ") AND (NOT (on" + std::to_string(i + 1) + "))";
        out << "- f" << i << " broken" << i << " : " << cond_or << "\n";
        out << "+ f" << i << " on" << i << " : " << cond_or << "\n";
        out << "+ f" << i << " congestion : " << cond_and << "\n";
    }
    out << "- f1 broken1 : TRUE\n";
    out << "- f" << n << " broken" << n << " : TRUE\n";
    out << "+ f1 on1 : TRUE\n";
    out << "+ f" << n << " on" << n << " : TRUE\n";

    for (int i = 1; i <= n; i++) out << "- g" << i << " on" << i << " : TRUE\n";
    for (int i = 1; i <= n; i++) out << "+ h" << i << " on" << i << " : NOT (broken" << i << ")\n";

    for (int i = 1; i <= n; i++) {
        out << "+ c" << i << " charged" << i << " : TRUE\n";
    }

    for (int i = 1; i <= n; i++) out << "l : ( FG (on" << i << "))\n";
    const int heavy = std::clamp(4, 1, n);
    out << "l : ( FG (charged" << heavy << "))\n";
    out << "g : ( G ( NOT (congestion)))\n";
}

static std::vector<int> make_service_indices(int n, int pattern_id) {
    std::vector<int> service = make_pattern(n, pattern_id + 1);
    if (service.empty()) service.push_back(std::max(1, n / 2));
    if (service.back() != n) service.push_back(n);
    std::sort(service.begin(), service.end());
    service.erase(std::unique(service.begin(), service.end()), service.end());
    return service;
}

static std::vector<int> make_blocked_indices(int n,
                                             int pattern_id,
                                             const std::vector<int>& service_indices) {
    std::vector<int> blocked = make_pattern(n, pattern_id + 2);
    std::unordered_set<int> service(service_indices.begin(), service_indices.end());
    std::vector<int> out;
    for (int idx : blocked) {
        if (idx <= 1 || idx >= n) continue;      // keep start and final target reachable
        if (service.count(idx)) continue;        // required service cells must stay repairable
        out.push_back(idx);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.empty()) {
        for (int idx = 2; idx <= n - 1; idx++) {
            if (!service.count(idx)) {
                out.push_back(idx);
                break;
            }
        }
    }
    return out;
}

static void write_case_ex3(const fs::path& out_path,
                           int n,
                           const std::vector<int>& service_indices,
                           const std::vector<int>& blocked_midlane) {
    std::ofstream out(out_path);
    if (!out) {
        std::cerr << "Failed to open " << out_path << " for writing\n";
        std::exit(1);
    }

    std::vector<std::string> locals;
    locals.reserve(7 * n);
    for (int i = 1; i <= n; i++) locals.push_back("ra" + std::to_string(i));
    for (int i = 1; i <= n; i++) locals.push_back("rb" + std::to_string(i));
    for (int i = 1; i <= n; i++) locals.push_back("rc" + std::to_string(i));
    for (int i = 1; i <= n; i++) locals.push_back("done" + std::to_string(i));
    for (int i = 1; i <= n; i++) locals.push_back("ha" + std::to_string(i));
    for (int i = 1; i <= n; i++) locals.push_back("hb" + std::to_string(i));
    for (int i = 1; i <= n; i++) locals.push_back("hc" + std::to_string(i));
    out << join_csv(locals) << "\n";
    out << "collision\n";

    std::vector<int> ra(n, 0), rb(n, 0), rc(n, 0), done(n, 0), ha(n, 0), hb(n, 0), hc(n, 0);
    rb[0] = 1;  // start at B1
    for (int idx : blocked_midlane) {
        if (idx >= 1 && idx <= n) hb[idx - 1] = 1;
    }

    std::vector<int> local_vals;
    local_vals.reserve(7 * n);
    local_vals.insert(local_vals.end(), ra.begin(), ra.end());
    local_vals.insert(local_vals.end(), rb.begin(), rb.end());
    local_vals.insert(local_vals.end(), rc.begin(), rc.end());
    local_vals.insert(local_vals.end(), done.begin(), done.end());
    local_vals.insert(local_vals.end(), ha.begin(), ha.end());
    local_vals.insert(local_vals.end(), hb.begin(), hb.end());
    local_vals.insert(local_vals.end(), hc.begin(), hc.end());
    out << join_truths(local_vals) << "\n";
    out << "FALSE\n";

    for (int i = 1; i <= n - 1; i++) {
        out << "- mA" << i << " ra" << i << " : ra" << i << "\n";
        out << "+ mA" << i << " ra" << (i + 1) << " : ra" << i << "\n";
        out << "+ mA" << i << " collision : ra" << i << " AND ha" << (i + 1) << "\n";

        out << "- mB" << i << " rb" << i << " : rb" << i << "\n";
        out << "+ mB" << i << " rb" << (i + 1) << " : rb" << i << "\n";
        out << "+ mB" << i << " collision : rb" << i << " AND hb" << (i + 1) << "\n";

        out << "- mC" << i << " rc" << i << " : rc" << i << "\n";
        out << "+ mC" << i << " rc" << (i + 1) << " : rc" << i << "\n";
        out << "+ mC" << i << " collision : rc" << i << " AND hc" << (i + 1) << "\n";
    }

    std::unordered_set<int> service_set(service_indices.begin(), service_indices.end());
    for (int i = 1; i <= n; i++) {
        out << "- sAB" << i << " ra" << i << " : ra" << i << "\n";
        out << "+ sAB" << i << " rb" << i << " : ra" << i << "\n";
        out << "+ sAB" << i << " collision : ra" << i << " AND hb" << i << "\n";

        out << "- sBA" << i << " rb" << i << " : rb" << i << "\n";
        out << "+ sBA" << i << " ra" << i << " : rb" << i << "\n";
        out << "+ sBA" << i << " collision : rb" << i << " AND ha" << i << "\n";

        out << "- sBC" << i << " rb" << i << " : rb" << i << "\n";
        out << "+ sBC" << i << " rc" << i << " : rb" << i << "\n";
        out << "+ sBC" << i << " collision : rb" << i << " AND hc" << i << "\n";

        out << "- sCB" << i << " rc" << i << " : rc" << i << "\n";
        out << "+ sCB" << i << " rb" << i << " : rc" << i << "\n";
        out << "+ sCB" << i << " collision : rc" << i << " AND hb" << i << "\n";

        if (service_set.count(i)) {
            out << "+ repB" << i << " done" << i << " : rb" << i << " AND (NOT (hb" << i << "))\n";
            out << "+ repB" << i << " collision : rb" << i << " AND hb" << i << "\n";
        }
    }

    for (int idx : service_indices) out << "l : ( FG (done" << idx << "))\n";
    out << "l : ( FG (rb" << n << "))\n";
    out << "g : ( G ( NOT (collision)))\n";
}

static std::string family_tag(Family f) {
    if (f == Family::Ex1Traffic) return "ex1";
    if (f == Family::Ex2Chargers) return "ex2";
    return "ex3";
}

int main(int argc, char** argv) {
    const Config cfg = parse_args(argc, argv);
    fs::create_directories(cfg.dir);

    for (const auto& entry : fs::directory_iterator(cfg.dir)) {
        fs::remove(entry.path());
    }

    std::mt19937 rng(cfg.seed);
    std::ofstream manifest(cfg.dir / "manifest.txt");
    manifest << "seed=" << cfg.seed << " per_n=" << cfg.per_n
             << " n_min=" << cfg.n_min << " n_max=" << cfg.n_max
             << " families=ex1,ex2,ex3\n";

    int case_id = 0;
    for (int n = cfg.n_min; n <= cfg.n_max; n++) {
        for (int j = 0; j < cfg.per_n; j++) {
            const Family fam = static_cast<Family>(j % 3);
            const int variant = j / 3;
            int pattern_id = variant + 1;  // avoid all-trivial cases by default

            std::vector<int> indices = make_pattern(n, pattern_id);
            if (!indices.empty() && n >= 8 && fam != Family::Ex3Narrow2D) {
                // Small deterministic perturbation for diversity.
                std::uniform_int_distribution<int> pick(0, static_cast<int>(indices.size()) - 1);
                const int idx_i = pick(rng);
                const int cur = indices[idx_i];
                std::uniform_int_distribution<int> delta_dist(-2, 2);
                const int candidate = std::clamp(cur + delta_dist(rng), 1, n);
                bool ok = true;
                for (int b : indices) {
                    if (b == cur) continue;
                    if (std::abs(b - candidate) <= 1) {
                        ok = false;
                        break;
                    }
                }
                if (ok) indices[idx_i] = candidate;
            }
            std::sort(indices.begin(), indices.end());
            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

            const std::string fname = "case-" + std::to_string(case_id) + "-" + family_tag(fam) + ".input-only.txt";
            const fs::path out_path = cfg.dir / fname;

            if (fam == Family::Ex1Traffic) {
                write_case_ex1(out_path, n, indices);
            } else if (fam == Family::Ex2Chargers) {
                write_case_ex2(out_path, n, indices);
            } else {
                std::vector<int> service = make_service_indices(n, pattern_id);
                std::vector<int> blocked = make_blocked_indices(n, pattern_id, service);
                write_case_ex3(out_path, n, service, blocked);
                indices = service;  // record service indices in manifest "broken=" field
            }

            manifest << fname << " n=" << n << " indices=";
            for (size_t i = 0; i < indices.size(); i++) {
                if (i) manifest << ",";
                manifest << indices[i];
            }
            manifest << "\n";

            case_id++;
        }
    }

    std::cout << "Wrote " << case_id << " systematic cases to " << cfg.dir << "\n";
    return 0;
}
