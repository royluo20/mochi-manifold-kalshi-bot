// amm_sizing.hpp — transcription of amm_sizing.py: Manifold CPMM math
// (k = YES^p * NO^(1-p)), pool reconstruction, the closed-form cost of moving
// the AMM to a price, per-market liquidity config, and AmmSizer.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>

#include "json.hpp"

namespace amm {

constexpr double MIN_PROB = 0.001;
constexpr double MAX_PROB = 0.999;

double cpmm_probability(double pool_yes, double pool_no, double p);
double cpmm_invariant(double pool_yes, double pool_no, double p);
double cpmm_shares_for_bet(double pool_yes, double pool_no, double p, double bet_amount, bool yes);
double cpmm_prob_after_bet(double pool_yes, double pool_no, double p, double bet_amount, bool yes);
std::pair<double, double> pool_at_prob(double prob, double k, double p);
double cpmm_amount_to_move_prob(double pool_yes, double pool_no, double p, double target_prob, bool yes);
std::pair<double, double> reconstruct_pool(double prob, double total_liquidity, double p = 0.5);

// Per-(slug, answer) liquidity + p. Keys use `aid or ''` like the Python.
class MarketLiquidityConfig {
public:
    MarketLiquidityConfig(double default_liquidity = 200.0, double default_p = 0.5)
        : default_liquidity(default_liquidity), default_p(default_p) {}
    void set_liquidity(const std::string& slug, const std::string& aid, double liquidity);
    void set_p(const std::string& slug, const std::string& aid, double p);
    double get_liquidity(const std::string& slug, const std::string& aid) const;
    double get_p(const std::string& slug, const std::string& aid) const;
    bool has_liquidity(const std::string& slug, const std::string& aid) const;
    void load_overrides_from_pairs(const mj::Value& market_pairs);
    void update_from_api(const std::string& slug, const mj::Value& data);

    double default_liquidity;
    double default_p;

private:
    std::map<std::pair<std::string, std::string>, double> liquidity_;
    std::map<std::pair<std::string, std::string>, double> p_overrides_;
};

struct Sizing {
    double amount = 0;
    bool yes = true;
    double shares = 0;
    double limit_prob = 0;
    double new_prob = 0;
    double amm_only_amount = 0;
    double adjusted_target = 0;
    bool is_adding = false;
    double pool_yes = 0, pool_no = 0;
};

class AmmSizer {
public:
    AmmSizer(double max_bet = 1000, double min_bet = 5, double max_position = 2000, double target_fraction = 0.80)
        : max_bet(max_bet), min_bet(min_bet), max_position(max_position), target_fraction(target_fraction) {}

    // compute_bet(...) with is_extreme=False (the only way the engine calls it).
    std::optional<Sizing> compute_bet(double current_prob, double target_prob, double total_liquidity,
                                      double p, int64_t position, std::optional<double> max_bet_o,
                                      std::optional<double> max_position_o,
                                      std::optional<double> target_fraction_o,
                                      std::optional<double> min_bet_o) const;

    double max_bet, min_bet, max_position, target_fraction;
};

}  // namespace amm
