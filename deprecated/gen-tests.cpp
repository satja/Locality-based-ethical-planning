#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Config {
    int count = 20;
    int n_min = 6;
    int n_max = 14;
    unsigned int seed = 12345;
    int max_broken = 3;
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

        if (arg == "--count") {
            cfg.count = std::stoi(need_value("--count"));
        } else if (arg == "--n-min") {
            cfg.n_min = std::stoi(need_value("--n-min"));
        } else if (arg == "--n-max") {
            cfg.n_max = std::stoi(need_value("--n-max"));
        } else if (arg == "--seed") {
            cfg.seed = static_cast<unsigned int>(std::stoul(need_value("--seed")));
        } else if (arg == "--max-broken") {
            cfg.max_broken = std::stoi(need_value("--max-broken"));
        } else if (starts_with(arg, "--")) {
            std::cerr << "Unknown flag: " << arg << "\n";
            std::exit(2);
        } else {
            std::cerr << "Unexpected positional argument: " << arg << "\n";
            std::exit(2);
        }
    }

    if (cfg.count <= 0) {
        std::cerr << "--count must be positive\n";
        std::exit(2);
    }
    if (cfg.n_min < 2 || cfg.n_max < cfg.n_min) {
        std::cerr << "Require 2 <= n-min <= n-max\n";
        std::exit(2);
    }
    if (cfg.max_broken < 0) {
        std::cerr << "--max-broken must be >= 0\n";
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

    for (int i = 1; i <= n; i++) {
        out << "- g" << i << " on" << i << " : TRUE\n";
    }
    for (int i = 1; i <= n; i++) {
        out << "+ h" << i << " on" << i << " : NOT (broken" << i << ")\n";
    }

    // Values: FG(on_i) for all i, and G(not congestion).
    for (int i = 1; i <= n; i++) {
        out << "l : ( FG (on" << i << "))\n";
    }
    out << "g : ( G ( NOT (congestion)))\n";

    // No action sequence: this is input-only by design.
}

int main(int argc, char** argv) {
    const Config cfg = parse_args(argc, argv);

    fs::path out_dir = "tests";
    fs::create_directories(out_dir);

    std::mt19937 rng(cfg.seed);
    std::uniform_int_distribution<int> n_dist(cfg.n_min, cfg.n_max);

    // Manifest describing the generated cases.
    std::ofstream manifest(out_dir / "manifest.txt");
    manifest << "seed=" << cfg.seed << " count=" << cfg.count
             << " n_min=" << cfg.n_min << " n_max=" << cfg.n_max
             << " max_broken=" << cfg.max_broken << "\n";

    for (int case_id = 0; case_id < cfg.count; case_id++) {
        const int n = n_dist(rng);

        std::vector<int> broken_vals(n, 0);
        std::vector<int> on_vals(n, 1);
        std::vector<int> broken_indices;

        const int maxBrokenEff = std::min(cfg.max_broken, std::max(0, n / 3));
        std::uniform_int_distribution<int> broken_target_dist(0, maxBrokenEff);
        int broken_target = broken_target_dist(rng);

        // Choose a non-adjacent broken set to keep plans simple and feasible.
        std::vector<int> candidates(n);
        for (int i = 0; i < n; i++) candidates[i] = i + 1;
        std::shuffle(candidates.begin(), candidates.end(), rng);

        auto is_adjacent = [](int a, int b) { return std::abs(a - b) == 1; };
        for (int idx : candidates) {
            bool ok = true;
            for (int chosen : broken_indices) {
                if (is_adjacent(idx, chosen)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            broken_indices.push_back(idx);
            if (static_cast<int>(broken_indices.size()) >= broken_target) break;
        }

        std::sort(broken_indices.begin(), broken_indices.end());
        for (int idx : broken_indices) {
            const int i = idx - 1;
            broken_vals[i] = 1;
            on_vals[i] = 0;
        }

        const std::string filename = "case-" + std::to_string(case_id) + ".input-only.txt";
        const fs::path out_path = out_dir / filename;
        write_case(out_path, n, on_vals, broken_vals);

        manifest << filename << " n=" << n << " broken=";
        for (size_t i = 0; i < broken_indices.size(); i++) {
            if (i) manifest << ",";
            manifest << broken_indices[i];
        }
        manifest << "\n";
    }

    std::cout << "Wrote " << cfg.count << " cases to " << out_dir << "\n";
    return 0;
}
