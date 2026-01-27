#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Config {
    fs::path dir = "tests_systematic";
    int n_min = 6;
    int n_max = 24;
    int per_n = 4;
    unsigned int seed = 20260127u;
};

static bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

static Config parse_args(int argc, char** argv) {
    /*
    CLI flags:
    - --dir DIR: output directory for generated cases and manifest.txt.
    - --n-min N: smallest problem size n to generate.
    - --n-max N: largest problem size n to generate.
    - --per-n K: number of cases per n (must be positive).
    - --seed S: RNG seed for reproducibility.
    */
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

static void write_case(const fs::path& out_path,
                       int n,
                       const std::vector<int>& on_vals,
                       const std::vector<int>& broken_vals) {
    std::ofstream out(out_path);
    if (!out) {
        std::cerr << "Failed to open " << out_path << " for writing\n";
        std::exit(1);
    }

    // Line 1: local propositions.
    std::vector<std::string> locals;
    locals.reserve(2 * n);
    for (int i = 1; i <= n; i++) locals.push_back("on" + std::to_string(i));
    for (int i = 1; i <= n; i++) locals.push_back("broken" + std::to_string(i));
    out << join_csv(locals) << "\n";

    // Line 2: global propositions.
    out << "congestion\n";

    // Line 3: initial local truths (on first, then broken).
    std::vector<int> local_vals;
    local_vals.reserve(2 * n);
    local_vals.insert(local_vals.end(), on_vals.begin(), on_vals.end());
    local_vals.insert(local_vals.end(), broken_vals.begin(), broken_vals.end());
    out << join_truths(local_vals) << "\n";

    // Line 4: initial global truths.
    out << "FALSE\n";

    // gamma^- and gamma^+ from the paper's traffic-light example.
    for (int i = 2; i <= n - 1; i++) {
        const std::string cond_or = "NOT (on" + std::to_string(i - 1) + ") OR (NOT (on" + std::to_string(i + 1) + "))";
        const std::string cond_and = "NOT (on" + std::to_string(i - 1) + ") AND (NOT (on" + std::to_string(i + 1) + "))";
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

    // Values: FG(on_i) for all i, and G(not congestion).
    for (int i = 1; i <= n; i++) out << "l : ( FG (on" << i << "))\n";
    out << "g : ( G ( NOT (congestion)))\n";
}

// Deterministic, non-adjacent broken patterns of increasing difficulty.
static std::vector<int> make_pattern(int n, int pattern_id) {
    std::vector<int> broken_indices;
    if (pattern_id == 0) {
        return broken_indices;  // no broken lights
    }
    if (pattern_id == 1) {
        broken_indices.push_back(n / 2);
        return broken_indices;
    }
    if (pattern_id == 2) {
        broken_indices.push_back(std::max(2, n / 3));
        broken_indices.push_back(std::min(n - 1, (2 * n) / 3));
        if (broken_indices[0] == broken_indices[1]) broken_indices.pop_back();
        return broken_indices;
    }
    // pattern_id >= 3: spaced-out multi-break pattern.
    const int k = std::min(1 + pattern_id, std::max(1, n / 6));
    int step = std::max(2, n / (k + 1));
    int pos = step;
    for (int i = 0; i < k; i++) {
        broken_indices.push_back(std::min(n - 1, pos));
        pos += step;
    }
    // Ensure non-adjacent and sorted/unique.
    std::sort(broken_indices.begin(), broken_indices.end());
    broken_indices.erase(std::unique(broken_indices.begin(), broken_indices.end()), broken_indices.end());
    std::vector<int> filtered;
    for (int idx : broken_indices) {
        if (!filtered.empty() && std::abs(filtered.back() - idx) == 1) continue;
        filtered.push_back(std::clamp(idx, 1, n));
    }
    return filtered;
}

int main(int argc, char** argv) {
    const Config cfg = parse_args(argc, argv);
    fs::create_directories(cfg.dir);

    // Reset directory contents for reproducibility.
    for (const auto& entry : fs::directory_iterator(cfg.dir)) {
        fs::remove(entry.path());
    }

    std::mt19937 rng(cfg.seed);

    std::ofstream manifest(cfg.dir / "manifest.txt");
    manifest << "seed=" << cfg.seed << " per_n=" << cfg.per_n
             << " n_min=" << cfg.n_min << " n_max=" << cfg.n_max << "\n";

    int case_id = 0;
    for (int n = cfg.n_min; n <= cfg.n_max; n++) {
        for (int j = 0; j < cfg.per_n; j++) {
            const int pattern_id = j % std::max(1, cfg.per_n);
            std::vector<int> broken_indices = make_pattern(n, pattern_id);

            // Small randomized perturbation: swap one broken index with a nearby
            // non-adjacent alternative when possible, to diversify cases.
            if (!broken_indices.empty() && n >= 8) {
                std::uniform_int_distribution<int> pick(0, static_cast<int>(broken_indices.size()) - 1);
                int idx_i = pick(rng);
                int cur = broken_indices[idx_i];
                std::uniform_int_distribution<int> delta_dist(-2, 2);
                int candidate = std::clamp(cur + delta_dist(rng), 1, n);
                bool ok = true;
                for (int b : broken_indices) {
                    if (b == cur) continue;
                    if (std::abs(b - candidate) <= 1) {
                        ok = false;
                        break;
                    }
                }
                if (ok) broken_indices[idx_i] = candidate;
            }

            std::sort(broken_indices.begin(), broken_indices.end());
            broken_indices.erase(std::unique(broken_indices.begin(), broken_indices.end()), broken_indices.end());

            std::vector<int> on_vals(n, 1);
            std::vector<int> broken_vals(n, 0);
            for (int idx : broken_indices) {
                const int i = idx - 1;
                broken_vals[i] = 1;
                on_vals[i] = 0;
            }

            const std::string filename = "case-" + std::to_string(case_id) + ".input-only.txt";
            const fs::path out_path = cfg.dir / filename;
            write_case(out_path, n, on_vals, broken_vals);

            manifest << filename << " n=" << n << " broken=";
            for (size_t i = 0; i < broken_indices.size(); i++) {
                if (i) manifest << ",";
                manifest << broken_indices[i];
            }
            manifest << " pattern=" << pattern_id << "\n";

            case_id++;
        }
    }

    std::cout << "Wrote " << case_id << " systematic cases to " << cfg.dir << "\n";
    return 0;
}
