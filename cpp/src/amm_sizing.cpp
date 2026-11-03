#include "amm_sizing.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "pyutil.hpp"

namespace amm {

// Python's ** on floats is C pow(); use it (not sqrt) so results match bit for bit.
static inline double ppow(double a, double b) { return std::pow(a, b); }

double cpmm_probability(double pool_yes, double pool_no, double p) {
    return (p * pool_no) / ((1 - p) * pool_yes + p * pool_no);
}

double cpmm_invariant(double pool_yes, double pool_no, double p) {
    return ppow(pool_yes, p) * ppow(pool_no, 1 - p);
}

double cpmm_shares_for_bet(double pool_yes, double pool_no, double p, double bet_amount, bool yes) {
    if (bet_amount <= 0) return 0.0;
    double k = cpmm_invariant(pool_yes, pool_no, p);
    double new_yes = pool_yes + bet_amount;
    double new_no = pool_no + bet_amount;
    if (yes) {
        double remaining = ppow(k / ppow(new_no, 1 - p), 1 / p);
        return new_yes - remaining;
    }
    double remaining = ppow(k / ppow(new_yes, p), 1 / (1 - p));
    return new_no - remaining;
}

double cpmm_prob_after_bet(double pool_yes, double pool_no, double p, double bet_amount, bool yes) {
    double shares = cpmm_shares_for_bet(pool_yes, pool_no, p, bet_amount, yes);
    double ny, nn;
    if (yes) {
        ny = pool_yes + bet_amount - shares;
        nn = pool_no + bet_amount;
    } else {
        ny = pool_yes + bet_amount;
        nn = pool_no + bet_amount - shares;
    }
    return cpmm_probability(ny, nn, p);
}

std::pair<double, double> pool_at_prob(double prob, double k, double p) {
    if (std::fabs(p - 0.5) < 1e-12) {
        double r = ppow(prob / (1 - prob), 0.5);
        return {k / r, k * r};
    }
    double ratio = prob * (1 - p) / (p * (1 - prob));
    double pool_yes = k / ppow(ratio, 1 - p);
    return {pool_yes, pool_yes * ratio};
}

double cpmm_amount_to_move_prob(double pool_yes, double pool_no, double p, double target_prob, bool yes) {
    double current = cpmm_probability(pool_yes, pool_no, p);
    target_prob = std::max(MIN_PROB, std::min(MAX_PROB, target_prob));
    if (yes && target_prob <= current) return 0.0;
    if (!yes && target_prob >= current) return 0.0;
    double k = cpmm_invariant(pool_yes, pool_no, p);
    auto [target_yes, target_no] = pool_at_prob(target_prob, k, p);
    double grows = yes ? (target_no - pool_no) : (target_yes - pool_yes);
    return std::max(0.0, std::floor(grows * 100) / 100);
}

std::pair<double, double> reconstruct_pool(double prob, double total_liquidity, double p) {
    prob = std::max(MIN_PROB, std::min(MAX_PROB, prob));
    double L = std::max(total_liquidity, 1.0);
    if (std::fabs(p - 0.5) < 1e-6) {
        return {L * ppow((1 - prob) / prob, 0.5), L * ppow(prob / (1 - prob), 0.5)};
    }
    double ratio = prob * (1 - p) / (p * (1 - prob));
    double pool_yes = L / ppow(ratio, 1 - p);
    return {pool_yes, pool_yes * ratio};
}

// ---------------------------------------------------------------------------

void MarketLiquidityConfig::set_liquidity(const std::string& slug, const std::string& aid, double liquidity) {
    liquidity_[{slug, aid}] = liquidity;
}
void MarketLiquidityConfig::set_p(const std::string& slug, const std::string& aid, double p) {
    p_overrides_[{slug, aid}] = p;
}
double MarketLiquidityConfig::get_liquidity(const std::string& slug, const std::string& aid) const {
    auto it = liquidity_.find({slug, aid});
    return it == liquidity_.end() ? default_liquidity : it->second;
}
double MarketLiquidityConfig::get_p(const std::string& slug, const std::string& aid) const {
    auto it = p_overrides_.find({slug, aid});
    return it == p_overrides_.end() ? default_p : it->second;
}
bool MarketLiquidityConfig::has_liquidity(const std::string& slug, const std::string& aid) const {
    return liquidity_.count({slug, aid}) > 0;
}

namespace {
std::string str_or_empty(const mj::Value* v) {
    // pair.get(key, '') then `aid or ''`
    if (!v || !v->truthy()) return "";
    if (v->is_string()) return v->s;
    return v->dump();
}
double strict_float(const mj::Value* v, const char* what) {
    if (v) {
        if (v->is_number()) return v->num();
        if (v->is_bool()) return v->b ? 1.0 : 0.0;
        if (v->is_string()) {
            auto f = py::float_from_str(v->s);
            if (f) return *f;
        }
    }
    throw std::runtime_error(std::string("float() failed for ") + what);
}
}  // namespace

void MarketLiquidityConfig::load_overrides_from_pairs(const mj::Value& market_pairs) {
    if (!market_pairs.is_array()) return;
    for (const mj::Value& pair : market_pairs.a) {
        std::string slug = str_or_empty(pair.get("manifold_slug"));
        std::string aid = str_or_empty(pair.get("manifold_answer_id"));
        if (const mj::Value* v = pair.get("p_override")) set_p(slug, aid, strict_float(v, "p_override"));
        if (const mj::Value* v = pair.get("liquidity_override"))
            set_liquidity(slug, aid, strict_float(v, "liquidity_override"));
    }
}

void MarketLiquidityConfig::update_from_api(const std::string& slug, const mj::Value& data) {
    auto record = [&](const std::string& aid, const mj::Value* pool, const mj::Value* p_val,
                      const mj::Value* total_liquidity, bool p_is_half) {
        double p = p_is_half ? 0.5 : ((p_val && !p_val->is_null()) ? strict_float(p_val, "p") : 0.5);
        const mj::Value* yes = (pool && pool->truthy()) ? pool->get("YES") : nullptr;
        const mj::Value* no = (pool && pool->truthy()) ? pool->get("NO") : nullptr;
        bool have_pool = yes && no && yes->truthy() && no->truthy() && yes->is_number() && no->is_number() &&
                         yes->num() > 0 && no->num() > 0;
        if (have_pool) {
            set_liquidity(slug, aid, cpmm_invariant(yes->num(), no->num(), p));
        } else if (total_liquidity && !total_liquidity->is_null()) {
            set_liquidity(slug, aid, strict_float(total_liquidity, "totalLiquidity"));
        } else {
            return;
        }
        if (std::fabs(p - 0.5) > 1e-9) set_p(slug, aid, p);
    };
    const mj::Value* ot = data.get("outcomeType");
    std::string outcome_type = (ot && ot->is_string()) ? ot->s : "";
    if (outcome_type == "MULTIPLE_CHOICE" || outcome_type == "INDEPENDENT_MULTIPLE_CHOICE") {
        const mj::Value* answers = data.get("answers");
        if (answers && answers->is_array()) {
            for (const mj::Value& ans : answers->a) {
                const mj::Value* id = ans.get("id");
                std::string aid = (id && id->is_string()) ? id->s : (id ? str_or_empty(id) : "");
                record(aid, ans.get("pool"), nullptr, ans.get("totalLiquidity"), true);
            }
        }
    } else {
        record("", data.get("pool"), data.get("p"), data.get("totalLiquidity"), false);
    }
}

// ---------------------------------------------------------------------------

std::optional<Sizing> AmmSizer::compute_bet(double current_prob, double target_prob, double total_liquidity,
                                            double p, int64_t position, std::optional<double> max_bet_o,
                                            std::optional<double> max_position_o,
                                            std::optional<double> target_fraction_o,
                                            std::optional<double> min_bet_o) const {
    double mb = max_bet_o ? *max_bet_o : max_bet;
    double mp = max_position_o ? *max_position_o : max_position;
    double tf = target_fraction_o ? *target_fraction_o : target_fraction;
    double mn = min_bet_o ? *min_bet_o : min_bet;

    bool yes = target_prob > current_prob;
    double adjusted_target = current_prob + (target_prob - current_prob) * tf;
    adjusted_target = std::max(MIN_PROB, std::min(MAX_PROB, adjusted_target));

    auto [pool_yes, pool_no] = reconstruct_pool(current_prob, total_liquidity, p);
    double amm_amount = cpmm_amount_to_move_prob(pool_yes, pool_no, p, adjusted_target, yes);
    if (amm_amount < mn) return std::nullopt;

    double amount = std::min(amm_amount, mb);
    bool is_adding = (position >= 0 && yes) || (position <= 0 && !yes);
    if (is_adding) {
        double remaining = std::max(0.0, mp - (double)std::llabs(position));
        amount = std::min(amount, remaining);
    }
    if (amount < mn) return std::nullopt;

    Sizing s;
    s.shares = py::round_n(cpmm_shares_for_bet(pool_yes, pool_no, p, amount, yes), 2);
    s.new_prob = py::round_n(cpmm_prob_after_bet(pool_yes, pool_no, p, amount, yes), 6);
    s.limit_prob = std::max(0.01, std::min(0.99, py::round_n(target_prob, 4)));
    s.amount = py::round_n(amount, 2);
    s.yes = yes;
    s.amm_only_amount = py::round_n(amm_amount, 2);
    s.adjusted_target = py::round_n(adjusted_target, 6);
    s.is_adding = is_adding;
    s.pool_yes = py::round_n(pool_yes, 1);
    s.pool_no = py::round_n(pool_no, 1);
    return s;
}

}  // namespace amm
